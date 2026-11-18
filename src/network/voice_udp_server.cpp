#include "network/voice_udp_server.h"
#include "audio/audio_pipeline.h"
#include "core/logger.h"
#include "core/session_registry.h"
#include "network/enet.h"
#include "network/protocol.h"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(__linux__)
#include <cstdio>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#endif

namespace eevs {

VoiceUdpServer::VoiceUdpServer(SessionRegistry& sessions)
    : m_sessions(sessions) {
    m_sessions.SetPeerDisconnectCallback([this](_ENetPeer* peer) {
        DisconnectPeerNow(peer);
    });
}

VoiceUdpServer::~VoiceUdpServer() {
    m_sessions.SetPeerDisconnectCallback(nullptr);
    Stop();
}

bool VoiceUdpServer::Start(std::string_view bindHost, uint16_t bindPort, size_t maxPeers) {
    Stop();

    ENetAddress address{};
    std::string hostStr(bindHost);
    if (hostStr.empty() || hostStr == "0.0.0.0" || hostStr == "::") {
        address.host = ENET_HOST_ANY;
    } else if (enet_address_set_host(&address, hostStr.c_str()) != 0) {
        EEVS_LOG_ERROR("[VoiceServer] Could not parse bind address: {}:{}", hostStr, bindPort);
        return false;
    }
    address.port = bindPort;

    m_voiceHost = enet_host_create(&address, maxPeers, protocol::kChannelCount, 0, 0);
    if (!m_voiceHost) {
        EEVS_LOG_ERROR("[VoiceServer] Failed to start external voice UDP server on {}:{}",
                       hostStr, bindPort);
        return false;
    }

    m_voiceHost->mtu = protocol::kDefaultMtu;
    m_voiceHost->userData = reinterpret_cast<void*>(this);
    m_voiceHost->intercept = reinterpret_cast<ENetInterceptCallback>(&VoiceUdpServer::VoiceRawReceiveIntercept);

    // Increase kernel UDP receive/send buffers to 8 MB
    enet_socket_set_option(m_voiceHost->socket, ENET_SOCKOPT_RCVBUF, protocol::kVoiceSocketBufferBytes);
    enet_socket_set_option(m_voiceHost->socket, ENET_SOCKOPT_SNDBUF, protocol::kVoiceSocketBufferBytes);

#if defined(__linux__)
    {
        const int busyPollUs = 50;
        ::setsockopt(m_voiceHost->socket, SOL_SOCKET, SO_BUSY_POLL, &busyPollUs, sizeof(busyPollUs));

        const int pmtuDont = IP_PMTUDISC_DONT;
        ::setsockopt(m_voiceHost->socket, IPPROTO_IP, IP_MTU_DISCOVER, &pmtuDont, sizeof(pmtuDont));

#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DONT)
        const int pmtu6Dont = IPV6_PMTUDISC_DONT;
        ::setsockopt(m_voiceHost->socket, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &pmtu6Dont, sizeof(pmtu6Dont));
#endif
    }
#endif

    StartEgressWorkers();

    EEVS_LOG_INFO("[VoiceServer] Started external voice UDP server on {}:{} (maxPeers={}, egressWorkers={})",
                  hostStr, bindPort, maxPeers, m_egressThreads.size());
    return true;
}

void VoiceUdpServer::Stop() {
    StopEgressWorkers();
    if (m_voiceHost) {
        enet_host_destroy(m_voiceHost);
        m_voiceHost = nullptr;
    }
    m_sessions.ClearAllPeers();
}

void VoiceUdpServer::StartEgressWorkers() {
    StopEgressWorkers();

    const size_t numWorkers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2u, 4u, 8u);
    m_egressQueues.reserve(numWorkers);
    m_egressThreads.reserve(numWorkers);

    for (size_t i = 0; i < numWorkers; ++i) {
        m_egressQueues.push_back(std::make_unique<EgressWorkerQueue>());
    }
    for (size_t i = 0; i < numWorkers; ++i) {
        m_egressThreads.emplace_back([this, i](std::stop_token st) {
            EgressWorkerLoop(st, i);
        });
    }
}

void VoiceUdpServer::StopEgressWorkers() {
    for (size_t i = 0; i < m_egressThreads.size(); ++i) {
        if (m_egressThreads[i].joinable()) {
            m_egressThreads[i].request_stop();
        }
        if (i < m_egressQueues.size() && m_egressQueues[i]) {
            m_egressQueues[i]->wakeSem.release();
        }
    }
    for (auto& t : m_egressThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_egressThreads.clear();
    m_egressQueues.clear();
}

void VoiceUdpServer::EgressWorkerLoop(std::stop_token st, size_t workerIndex) {
    EgressWorkerQueue& q = *m_egressQueues[workerIndex];

#if defined(__linux__)
    {
        char threadName[16]{};
        std::snprintf(threadName, sizeof(threadName), "eevs-egress-%zu", workerIndex);
        ::pthread_setname_np(::pthread_self(), threadName);

        const unsigned int hwCores = std::thread::hardware_concurrency();
        if (hwCores > 1u) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(static_cast<int>((workerIndex + 1u) % hwCores), &cpuset);
            ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
        }
    }
#endif

    while (!st.stop_requested()) {
        size_t head = q.head.load(std::memory_order_relaxed);
        size_t tail = q.tail.load(std::memory_order_acquire);

        if (head == tail) {
            bool woke = false;
            for (int spin = 0; spin < 64; ++spin) {
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
                tail = q.tail.load(std::memory_order_acquire);
                if (head != tail || st.stop_requested()) {
                    woke = true;
                    break;
                }
            }
            if (!woke && !st.stop_requested()) {
                q.waiting.store(true, std::memory_order_seq_cst);
                if (q.tail.load(std::memory_order_seq_cst) == head && !st.stop_requested()) {
                    (void)q.wakeSem.try_acquire_for(std::chrono::milliseconds(1));
                }
                q.waiting.store(false, std::memory_order_relaxed);
            }
            continue;
        }

#if defined(__linux__)
        // Linux batch transmission via sendmmsg() — flushes up to 64 UDP datagrams per syscall
        constexpr size_t kMaxBatch = 64;
        struct mmsghdr msgs[kMaxBatch]{};
        struct iovec iovs[kMaxBatch]{};
#ifdef ENET_IPV4_ONLY
        struct sockaddr_in addrs[kMaxBatch]{};
#else
        struct sockaddr_in6 addrs[kMaxBatch]{};
#endif

        while (head != tail && !st.stop_requested()) {
            const size_t avail = tail - head;
            const size_t count = std::min(avail, kMaxBatch);
            unsigned int validCount = 0;

            for (size_t i = 0; i < count; ++i) {
                EgressDatagram& dg = q.ring[(head + i) & EgressWorkerQueue::kMask];
                if (!m_voiceHost || dg.length == 0) {
                    continue;
                }

#ifdef ENET_IPV4_ONLY
                addrs[validCount].sin_family = AF_INET;
                addrs[validCount].sin_port = ENET_HOST_TO_NET_16(dg.destAddress.port);
                addrs[validCount].sin_addr = dg.destAddress.host;
#else
                addrs[validCount].sin6_family = AF_INET6;
                addrs[validCount].sin6_port = ENET_HOST_TO_NET_16(dg.destAddress.port);
                addrs[validCount].sin6_addr = dg.destAddress.host;
                addrs[validCount].sin6_scope_id = dg.destAddress.sin6_scope_id;
#endif
                iovs[validCount].iov_base = dg.payload.data();
                iovs[validCount].iov_len = dg.length;

                msgs[validCount].msg_hdr.msg_name = &addrs[validCount];
                msgs[validCount].msg_hdr.msg_namelen = sizeof(addrs[validCount]);
                msgs[validCount].msg_hdr.msg_iov = &iovs[validCount];
                msgs[validCount].msg_hdr.msg_iovlen = 1;
                msgs[validCount].msg_hdr.msg_control = nullptr;
                msgs[validCount].msg_hdr.msg_controllen = 0;
                msgs[validCount].msg_hdr.msg_flags = 0;
                ++validCount;
            }

            if (validCount > 0 && m_voiceHost) {
                ::sendmmsg(m_voiceHost->socket, msgs, validCount, MSG_NOSIGNAL | MSG_DONTWAIT);
            }
            head += count;
        }
#else
        while (head != tail && !st.stop_requested()) {
            EgressDatagram& dg = q.ring[head & EgressWorkerQueue::kMask];
            if (m_voiceHost && dg.length > 0) {
                ENetBuffer buf{};
                buf.data = dg.payload.data();
                buf.dataLength = dg.length;
                enet_socket_send(m_voiceHost->socket, &dg.destAddress, &buf, 1);
            }
            ++head;
        }
#endif

        q.head.store(head, std::memory_order_release);
    }
}

void VoiceUdpServer::DisconnectPeerNow(_ENetPeer* peer) {
    if (peer && m_voiceHost) {
        enet_peer_disconnect_now(peer, 0);
    }
}

int EEVS_ENET_CALLBACK VoiceUdpServer::VoiceRawReceiveIntercept(_ENetHost* host, void* /*event*/) {
    auto* self = reinterpret_cast<VoiceUdpServer*>(host->userData);
    if (!self) {
        return 0;
    }

    // Fast-path raw UDP voice packet header: [uint16_t peerID][uint8_t tokenPrefix[16]][uint8_t packetId]
    if (host->receivedDataLength < sizeof(protocol::FastPathVoicePrefix)) {
        return 0;
    }

    const uint8_t* raw = host->receivedData;
    uint16_t enetPeerId = 0;
    std::memcpy(&enetPeerId, raw, sizeof(uint16_t));

    if (enetPeerId >= host->peerCount) {
        return 0;
    }

    const VoicePlayerSession* session = self->m_sessions.FindByPeerId(enetPeerId);
    if (!session || !session->authenticated || !session->peer) {
        return 0;
    }

    // Verify source IP and port match the authenticated ENet peer
    if (!enet_host_equal(host->receivedAddress.host, session->peer->address.host) ||
        host->receivedAddress.port != session->peer->address.port) {
        return 0;
    }

    // Verify first 16 bytes of the player's 32-byte voice token using a single 128-bit SIMD compare
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i pktTok = _mm_loadu_si128(reinterpret_cast<const __m128i*>(raw + 2));
    const __m128i sesTok = _mm_loadu_si128(reinterpret_cast<const __m128i*>(session->token.data()));
    const __m128i diff = _mm_xor_si128(pktTok, sesTok);
    if (!_mm_testz_si128(diff, diff)) {
        return 0;
    }
#else
    if (std::memcmp(raw + 2, session->token.data(), 16) != 0) {
        return 0;
    }
#endif

    uint8_t packetId = raw[18];
    if (packetId == protocol::kVoiceClientAudioPacketId) {
        self->HandleVoiceAudioPacket(session->clientId, raw, host->receivedDataLength, true);
        return 1; // Handled by fast-path, skip ENet protocol parsing
    }

    return 0;
}

void VoiceUdpServer::HandleGameClientConnect(_ENetPeer* peer, uint32_t tokenJoaat) {
    // Reject connection attempts whose JOAAT token hash has not been registered by the gateway
    if (!m_sessions.IsTokenJoaatAllowed(tokenJoaat)) {
        EEVS_LOG_WARN("[VoiceServer] Rejecting uninvited peer (incomingPeerID={}, tokenJoaat=0x{:08X})",
                      peer->incomingPeerID, tokenJoaat);
        enet_peer_disconnect_now(peer, 0);
        return;
    }

    peer->mtu = protocol::kDefaultMtu;
    // Apply a short timeout while awaiting VoiceAuthPacket so half-open connections expire quickly
    enet_peer_timeout(
        peer,
        protocol::kUnauthenticatedTimeoutLimit,
        protocol::kUnauthenticatedTimeoutMinimumMs,
        protocol::kUnauthenticatedTimeoutMaximumMs
    );
    enet_peer_ping_interval(peer, protocol::kPingIntervalMs);
    EEVS_LOG_INFO("[VoiceServer] Game client connected (incomingPeerID={}, tokenJoaat=0x{:08X}), awaiting VoiceAuthPacket...",
                  peer->incomingPeerID, tokenJoaat);
}

void VoiceUdpServer::HandleGameClientPacket(_ENetPeer* peer, uint8_t /*channelId*/, const uint8_t* data, size_t length) {
    if (length == 0) {
        return;
    }

    // Control packet (0xFF)
    if (data[0] == protocol::kControlPacketMarker) {
        if (length >= 3) {
            uint16_t controlId = 0;
            std::memcpy(&controlId, data + 1, sizeof(uint16_t));
            if (controlId == protocol::kVoiceAuthPacketId) {
                HandleGameClientAuth(peer, data, length);
                return;
            }
            EEVS_LOG_DEBUG("[VoiceServer] Peer {} sent control packet id={} ({} bytes)",
                           peer->incomingPeerID, controlId, length);
        }
        return;
    }

    // ENet-framed voice packet (2-byte packetId prefix)
    if (length >= 2) {
        uint16_t packetId = 0;
        std::memcpy(&packetId, data, sizeof(uint16_t));
        if (packetId == protocol::kVoiceClientAudioPacketId) {
            if (auto clientId = m_sessions.ResolveClientIdByPeerId(peer->incomingPeerID)) {
                HandleVoiceAudioPacket(*clientId, data + 2, length - 2, false);
            }
            return;
        }
    }
}

void VoiceUdpServer::HandleGameClientAuth(_ENetPeer* peer, const uint8_t* data, size_t length) {
    // VoiceAuthPacket layout: [0xFF][uint16_t packetId = 7][uint16_t clientId][uint8_t token[32]]
    if (length < sizeof(protocol::VoiceAuthPacket)) {
        enet_peer_disconnect_now(peer, 0);
        return;
    }

    protocol::VoiceAuthPacket auth{};
    std::memcpy(&auth, data, sizeof(protocol::VoiceAuthPacket));

    uint16_t clientId = auth.clientIdOrZero;
    VoicePlayerSession* session = m_sessions.FindByClientId(clientId);
    if (!session) {
        EEVS_LOG_WARN("[VoiceServer] Peer {} failed auth: unknown clientId={}",
                      peer->incomingPeerID, clientId);
        enet_peer_disconnect_now(peer, 0);
        return;
    }

    if (std::memcmp(auth.secret, session->token.data(), 32) != 0) {
        EEVS_LOG_WARN("[VoiceServer] Peer {} failed auth: token mismatch for clientId={}",
                      peer->incomingPeerID, clientId);
        enet_peer_disconnect_now(peer, 0);
        return;
    }

    m_sessions.BindPeer(clientId, peer, peer->incomingPeerID);

    enet_peer_throttle_configure(
        peer,
        protocol::kThrottleIntervalMs,
        protocol::kThrottleAcceleration,
        protocol::kThrottleDeceleration
    );
    enet_peer_timeout(peer, protocol::kTimeoutLimit, protocol::kTimeoutMaximumMs, protocol::kTimeoutMaximumMs);
    enet_peer_ping_interval(peer, protocol::kPingIntervalMs);

    if (SendClientHandshakeDone(peer)) {
        enet_host_flush(m_voiceHost);
    }

    EEVS_LOG_INFO("[VoiceServer] Game client authenticated: clientId={}, netID={}, enetPeerID={}",
                  session->clientId, session->netId, peer->incomingPeerID);
}

bool VoiceUdpServer::SendClientHandshakeDone(_ENetPeer* peer) {
    if (!peer) {
        return false;
    }

    protocol::VoiceHandshakeDonePacket pkt{};
    pkt.marker = protocol::kControlPacketMarker;
    pkt.packetId = protocol::kVoiceHandshakeDonePacketId;

    ENetPacket* packet = enet_packet_create(&pkt, sizeof(pkt), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        return false;
    }

    return enet_peer_send(peer, 0, packet) == 0;
}

void VoiceUdpServer::HandleGameClientDisconnect(_ENetPeer* peer, bool wasTimeout) {
    if (auto clientId = m_sessions.UnbindPeer(peer->incomingPeerID, wasTimeout)) {
        EEVS_LOG_INFO("[VoiceServer] Game client disconnected (clientId={}, enetPeerID={}, timeout={})",
                      *clientId, peer->incomingPeerID, wasTimeout ? "true" : "false");
    }
}

void VoiceUdpServer::HandleVoiceAudioPacket(uint16_t senderClientId, const uint8_t* data, size_t length, bool fromFastPath) {
    if (m_pipeline) {
        m_pipeline->ProcessIngressPacket(senderClientId, data, length, fromFastPath);
    }
}

bool VoiceUdpServer::SendRawUdpToPeer(
    const _ENetPeer* peer,
    const uint8_t* data,
    size_t length,
    size_t nonZeroPrefixBytes
) {
    if (!m_voiceHost || !peer || !data || length == 0 || length > protocol::kDefaultMtu) {
        return false;
    }

    const size_t copyPrefix = (nonZeroPrefixBytes > 0 && nonZeroPrefixBytes < length)
        ? nonZeroPrefixBytes
        : length;

    if (!m_egressQueues.empty()) {
        const size_t qIdx = (m_nextWorkerIdx++) % m_egressQueues.size();
        EgressWorkerQueue& q = *m_egressQueues[qIdx];

        const size_t tail = q.tail.load(std::memory_order_relaxed);
        const size_t head = q.head.load(std::memory_order_acquire);
        if ((tail - head) < EgressWorkerQueue::kCapacity) {
            EgressDatagram& slot = q.ring[tail & EgressWorkerQueue::kMask];
            slot.destAddress = peer->address;
            slot.length = static_cast<uint16_t>(length);
            std::memcpy(slot.payload.data(), data, copyPrefix);
            if (slot.dirtyPrefixLen > copyPrefix) {
                std::memset(slot.payload.data() + copyPrefix, 0, slot.dirtyPrefixLen - copyPrefix);
            }
            slot.dirtyPrefixLen = static_cast<uint16_t>(copyPrefix);
            q.tail.store(tail + 1, std::memory_order_seq_cst);
            if (q.waiting.load(std::memory_order_seq_cst)) {
                q.wakeSem.release();
            }
            return true;
        }
    }

    // Fallback to direct send if worker queue is full
    ENetBuffer buf{};
    buf.data = const_cast<uint8_t*>(data);
    buf.dataLength = length;

    return enet_socket_send(m_voiceHost->socket, &peer->address, &buf, 1) == static_cast<int>(length);
}

void VoiceUdpServer::Poll(uint32_t timeoutMs) {
    if (!m_voiceHost) {
        return;
    }

    // 1. Drain fast-path ClientVoicePacket datagrams directly from the socket.
    // Use MSG_PEEK on the 19-byte fast-path prefix so ENet control packets remain queued for enet_host_service.
    // Note: On Windows Winsock, MSG_PEEK with a 19-byte buffer on a larger UDP datagram fills peekHdr
    // and returns SOCKET_ERROR (-1) with WSAGetLastError() == WSAEMSGSIZE.
    ENetEvent event{};
    bool controlPacketWaiting = false;
    size_t drainedFastPath = 0;

    auto drainFastPathQueue = [&]() {
        for (int i = 0; i < 4096; ++i) {
            uint8_t peekHdr[sizeof(protocol::FastPathVoicePrefix)]{};
            const int peekLen = ::recv(
                m_voiceHost->socket,
                reinterpret_cast<char*>(peekHdr),
                static_cast<int>(sizeof(peekHdr)),
                MSG_PEEK
            );
#if defined(_WIN32)
            const int wsaErr = (peekLen == SOCKET_ERROR) ? ::WSAGetLastError() : 0;
            const bool hasDatagram = (peekLen > 0) || (peekLen == SOCKET_ERROR && wsaErr == WSAEMSGSIZE);
            const bool hasFullPrefix = (peekLen >= static_cast<int>(sizeof(protocol::FastPathVoicePrefix))) ||
                                       (peekLen == SOCKET_ERROR && wsaErr == WSAEMSGSIZE);
#else
            const bool hasDatagram = (peekLen > 0);
            const bool hasFullPrefix = (peekLen >= static_cast<int>(sizeof(protocol::FastPathVoicePrefix)));
#endif
            if (!hasDatagram) {
                break; // Socket queue is empty (WSAEWOULDBLOCK / EWOULDBLOCK)
            }
            if (!hasFullPrefix || peekHdr[18] != protocol::kVoiceClientAudioPacketId) {
                controlPacketWaiting = true; // Next datagram is an ENet control packet
                break;
            }

            ENetBuffer buffer{};
            buffer.data = m_voiceHost->packetData[0];
            buffer.dataLength = m_voiceHost->mtu;

            const int receivedLength = enet_socket_receive(m_voiceHost->socket, &m_voiceHost->receivedAddress, &buffer, 1);
            if (receivedLength <= 0) {
                break;
            }

            m_voiceHost->receivedData = m_voiceHost->packetData[0];
            m_voiceHost->receivedDataLength = static_cast<size_t>(receivedLength);
            m_voiceHost->totalReceivedData += static_cast<uint32_t>(receivedLength);
            m_voiceHost->totalReceivedPackets++;

            VoiceRawReceiveIntercept(m_voiceHost, &event);
            ++drainedFastPath;
        }
    };

    drainFastPathQueue();

    // If the UDP socket was empty and a non-zero timeout was specified, wait on the socket
    // via select/poll and drain any newly arrived fast-path packets immediately on wakeup.
    if (drainedFastPath == 0 && !controlPacketWaiting && timeoutMs > 0) {
        enet_uint32 waitCondition = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(m_voiceHost->socket, &waitCondition, timeoutMs) == 0 &&
            (waitCondition & ENET_SOCKET_WAIT_RECEIVE) != 0) {
            drainFastPathQueue();
        }
    }

    // 2. Service ENet control state machine (handshakes, pings, timeouts, disconnects)
    // Only run the 4095-peer outgoing timer sweep when a control packet is waiting or at least 5ms has elapsed.
    const uint32_t nowMs = enet_time_get();
    if (!controlPacketWaiting && ENET_TIME_DIFFERENCE(nowMs, m_lastEnetServiceMs) < 5) {
        return;
    }
    m_lastEnetServiceMs = nowMs;

    while (enet_host_service(m_voiceHost, &event, 0) > 0) {
        do {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    HandleGameClientConnect(event.peer, event.data);
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    if (event.packet) {
                        HandleGameClientPacket(event.peer, event.channelID, event.packet->data, event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                    }
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                    bool wasTimeout = (event.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT);
                    HandleGameClientDisconnect(event.peer, wasTimeout);
                    break;
                }
                default:
                    break;
            }
        } while (enet_host_check_events(m_voiceHost, &event) > 0);
        timeoutMs = 0;
    }
}

} // namespace eevs
