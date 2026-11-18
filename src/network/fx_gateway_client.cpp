#include "network/fx_gateway_client.h"
#include "core/crypto_utils.h"
#include "core/logger.h"
#include "core/routing_matrix.h"
#include "core/session_registry.h"
#include "network/enet.h"
#include "network/protocol.h"

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace eevs {

FxGatewayClient::FxGatewayClient(SessionRegistry& sessions, RoutingMatrix& routing)
    : m_sessions(sessions)
    , m_routing(routing)
    , m_decompressBuffer(protocol::kDecompressBufferSize, 0) {
}

FxGatewayClient::~FxGatewayClient() {
    Disconnect();
}

bool FxGatewayClient::Connect(std::string_view gatewayHost, uint16_t gatewayPort, std::string_view licenseKey) {
    CleanupHostAndPeer();

    m_gatewayHostName = std::string(gatewayHost);
    m_gatewayPort = gatewayPort;
    m_licenseJoaat = crypto::ComputeJoaat(licenseKey);
    m_licenseSha256 = crypto::ComputeSha256(licenseKey);
    m_hasTargetConfig = true;
    m_reconnectAttempts = 0;

    return InitiateConnectAttempt();
}

bool FxGatewayClient::InitiateConnectAttempt() {
    CleanupHostAndPeer();

    if (!m_hasTargetConfig || m_gatewayHostName.empty() || m_gatewayPort == 0) {
        return false;
    }

    m_gatewayHost = enet_host_create(nullptr, 1, protocol::kChannelCount, 0, 0);
    if (!m_gatewayHost) {
        EEVS_LOG_ERROR("[GatewayClient] Failed to create ENet client host.");
        ScheduleReconnect();
        return false;
    }

    m_gatewayHost->mtu = protocol::kDefaultMtu;

    ENetAddress address{};
    if (enet_address_set_host(&address, m_gatewayHostName.c_str()) != 0) {
        EEVS_LOG_ERROR("[GatewayClient] Could not parse gateway server address: {}:{}",
                       m_gatewayHostName, m_gatewayPort);
        enet_host_destroy(m_gatewayHost);
        m_gatewayHost = nullptr;
        ScheduleReconnect();
        return false;
    }
    address.port = m_gatewayPort;

    EEVS_LOG_INFO("[GatewayClient] Connecting to voice gateway server at {}:{} (joaat=0x{:08X})...",
                  m_gatewayHostName, m_gatewayPort, m_licenseJoaat);

    m_gatewayPeer = enet_host_connect(m_gatewayHost, &address, protocol::kChannelCount, m_licenseJoaat);
    if (!m_gatewayPeer) {
        EEVS_LOG_ERROR("[GatewayClient] enet_host_connect failed.");
        enet_host_destroy(m_gatewayHost);
        m_gatewayHost = nullptr;
        ScheduleReconnect();
        return false;
    }

    enet_peer_timeout(m_gatewayPeer, protocol::kTimeoutLimit, protocol::kTimeoutMinimumMs, protocol::kTimeoutMaximumMs);
    enet_peer_ping_interval(m_gatewayPeer, protocol::kPingIntervalMs);

    m_connecting = true;
    m_connectStartTime = std::chrono::steady_clock::now();
    return true;
}

void FxGatewayClient::CleanupHostAndPeer() {
    if (m_gatewayPeer && m_gatewayHost) {
        enet_peer_disconnect_now(m_gatewayPeer, 0);
        m_gatewayPeer = nullptr;
    } else if (m_gatewayPeer) {
        enet_peer_reset(m_gatewayPeer);
        m_gatewayPeer = nullptr;
    }
    if (m_gatewayHost) {
        enet_host_destroy(m_gatewayHost);
        m_gatewayHost = nullptr;
    }
    m_connected = false;
    m_authenticated = false;
    m_connecting = false;
}

void FxGatewayClient::Disconnect() {
    m_hasTargetConfig = false;
    m_reconnectAttempts = 0;
    CleanupHostAndPeer();
}

void FxGatewayClient::ScheduleReconnect() {
    if (!m_autoReconnect || !m_hasTargetConfig) {
        return;
    }

    const uint32_t shift = std::min<uint32_t>(m_reconnectAttempts, 4u);
    auto delay = kInitialReconnectDelay * (1u << shift);
    if (delay > kMaxReconnectDelay) {
        delay = kMaxReconnectDelay;
    }
    ++m_reconnectAttempts;
    m_nextReconnectTime = std::chrono::steady_clock::now() + delay;

    EEVS_LOG_WARN("[GatewayClient] Scheduling reconnect attempt #{} in {} ms...",
                  m_reconnectAttempts,
                  static_cast<long long>(delay.count()));
}

bool FxGatewayClient::SendGatewayAuthPacket() {
    if (!m_gatewayPeer) {
        return false;
    }

    protocol::VoiceAuthPacket pkt{};
    pkt.marker = protocol::kControlPacketMarker;
    pkt.packetId = protocol::kVoiceAuthPacketId;
    pkt.clientIdOrZero = 0;
    std::memcpy(pkt.secret, m_licenseSha256.data(), sizeof(pkt.secret));

    ENetPacket* packet = enet_packet_create(&pkt, sizeof(pkt), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) {
        return false;
    }

    return enet_peer_send(m_gatewayPeer, 0, packet) == 0;
}

void FxGatewayClient::HandleGatewayPacket(const uint8_t* data, size_t length) {
    if (length == 0) {
        return;
    }

    uint8_t packetId = data[0];
    if (packetId == protocol::kControlPacketMarker) {
        if (length >= sizeof(protocol::VoiceHandshakeDonePacket)) {
            uint16_t controlId = 0;
            std::memcpy(&controlId, data + 1, sizeof(uint16_t));
            if (controlId == protocol::kVoiceHandshakeDonePacketId) {
                m_authenticated = true;
                EEVS_LOG_INFO("[GatewayClient] Voice handshake with gateway server completed.");
                return;
            }
            EEVS_LOG_DEBUG("[GatewayClient] Received control packet id={} ({} bytes)", controlId, length);
        }
        return;
    }

    if (packetId == protocol::kVoiceStreamingSyncPacketId) {
        if (length > 1) {
            HandleVoiceStreamingSync(data + 1, length - 1);
        }
        return;
    }

    EEVS_LOG_WARN("[GatewayClient] Received unknown gateway packet id=0x{:02X} ({} bytes)", packetId, length);
}

void FxGatewayClient::HandleVoiceStreamingSync(const uint8_t* payload, size_t length) {
    if (length == 0 || length > protocol::kMaxSyncCompressedSize) {
        return;
    }

    int decompressedSize = LZ4_decompress_safe(
        reinterpret_cast<const char*>(payload),
        reinterpret_cast<char*>(m_decompressBuffer.data()),
        static_cast<int>(length),
        static_cast<int>(m_decompressBuffer.size())
    );

    if (decompressedSize <= 0) {
        EEVS_LOG_ERROR("[GatewayClient] Failed to decompress VoiceStreamingSync data ({} bytes).", length);
        return;
    }

    const uint8_t* buf = m_decompressBuffer.data();
    size_t total = static_cast<size_t>(decompressedSize);
    size_t offset = 0;

    while (offset < total) {
        if (offset + 2 > total) {
            EEVS_LOG_ERROR("[GatewayClient] Failed to read ClientID from VoiceStreamingSync data.");
            return;
        }

        uint16_t rawClientId = 0;
        std::memcpy(&rawClientId, buf + offset, sizeof(uint16_t));
        offset += 2;

        bool hasTokenUpdate = (rawClientId & 0x8000u) != 0;
        uint16_t clientId = rawClientId & 0x7FFFu;

        if (hasTokenUpdate) {
            if (offset + 32 > total) {
                EEVS_LOG_ERROR("[GatewayClient] Failed to read token from VoiceStreamingSync data.");
                return;
            }

            std::array<uint8_t, 32> token{};
            std::memcpy(token.data(), buf + offset, 32);
            offset += 32;

            if (offset + 2 > total) {
                EEVS_LOG_ERROR("[GatewayClient] Failed to read netID from VoiceStreamingSync data.");
                return;
            }

            uint16_t netId = 0;
            std::memcpy(&netId, buf + offset, sizeof(uint16_t));
            offset += 2;

            if (crypto::IsAllZeros32(token.data())) {
                m_routing.ClearClient(clientId);
                m_sessions.UnregisterPlayer(clientId);
            } else {
                const VoicePlayerSession* existing = m_sessions.FindByClientId(clientId);
                if (!existing || existing->token != token) {
                    m_routing.ClearClient(clientId);
                }
                m_sessions.RegisterPlayerToken(clientId, netId, token);
            }
        }

        if (offset + 2 > total) {
            EEVS_LOG_ERROR("[GatewayClient] Failed to read addedClientsCount from VoiceStreamingSync data.");
            return;
        }

        uint16_t addedCount = 0;
        std::memcpy(&addedCount, buf + offset, sizeof(uint16_t));
        offset += 2;

        if (offset + static_cast<size_t>(addedCount) * sizeof(uint16_t) > total) {
            EEVS_LOG_ERROR("[GatewayClient] Failed to read addedClientID array from VoiceStreamingSync data.");
            return;
        }

        std::span<const uint16_t> addedEntries(
            reinterpret_cast<const uint16_t*>(buf + offset),
            addedCount
        );
        offset += static_cast<size_t>(addedCount) * sizeof(uint16_t);

        if (offset + 2 > total) {
            EEVS_LOG_ERROR("[GatewayClient] Failed to read removedClientsCount from VoiceStreamingSync data.");
            return;
        }

        uint16_t removedCount = 0;
        std::memcpy(&removedCount, buf + offset, sizeof(uint16_t));
        offset += 2;

        if (offset + static_cast<size_t>(removedCount) * sizeof(uint16_t) > total) {
            EEVS_LOG_ERROR("[GatewayClient] Failed to read removedClientID array from VoiceStreamingSync data.");
            return;
        }

        std::span<const uint16_t> removedEntries(
            reinterpret_cast<const uint16_t*>(buf + offset),
            removedCount
        );
        offset += static_cast<size_t>(removedCount) * sizeof(uint16_t);

        if (m_sessions.FindByClientId(clientId) != nullptr) {
            m_routing.ApplySyncDeltaBatch(clientId, addedEntries, removedEntries);
        }
    }
}

void FxGatewayClient::Poll(uint32_t timeoutMs) {
    const auto now = std::chrono::steady_clock::now();

    // Check if an in-flight connection attempt timed out without receiving ENET_EVENT_TYPE_CONNECT
    if (m_connecting && !m_connected && (now - m_connectStartTime) >= kConnectAttemptTimeout) {
        EEVS_LOG_WARN("[GatewayClient] Connection attempt to {}:{} timed out.",
                      m_gatewayHostName, m_gatewayPort);
        CleanupHostAndPeer();
        ScheduleReconnect();
        return;
    }

    // If disconnected and auto-reconnect is due, initiate a new connection attempt
    if (!m_gatewayHost) {
        if (m_autoReconnect && m_hasTargetConfig && now >= m_nextReconnectTime) {
            InitiateConnectAttempt();
        }
        return;
    }

    ENetEvent event{};
    while (enet_host_service(m_gatewayHost, &event, timeoutMs) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                m_connected = true;
                m_connecting = false;
                m_reconnectAttempts = 0;
                EEVS_LOG_INFO("[GatewayClient] Connected to voice gateway server (assigned peerID={}).",
                              event.peer->outgoingPeerID);
                enet_peer_throttle_configure(
                    event.peer,
                    protocol::kThrottleIntervalMs,
                    protocol::kThrottleAcceleration,
                    protocol::kThrottleDeceleration
                );
                enet_host_flush(m_gatewayHost);
                if (SendGatewayAuthPacket()) {
                    enet_host_flush(m_gatewayHost);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                if (event.packet) {
                    HandleGatewayPacket(event.packet->data, event.packet->dataLength);
                    enet_packet_destroy(event.packet);
                }
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                bool wasTimeout = (event.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT);
                EEVS_LOG_WARN("[GatewayClient] Disconnected from voice gateway server. Timeout: {}",
                              wasTimeout ? "true" : "false");

                m_gatewayPeer = nullptr; // Peer is already disconnected; avoid sending disconnect packet in CleanupHostAndPeer
                m_routing.ClearAllSyncTargets();
                m_sessions.UnregisterAllPlayers();
                CleanupHostAndPeer();
                ScheduleReconnect();
                return;
            }
            default:
                break;
        }
        timeoutMs = 0;
    }
}

} // namespace eevs
