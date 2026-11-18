#include "audio/packet_synthesizer.h"
#include "audio/clip_manager.h"
#include "audio/opus_codec_pool.h"
#include "core/profiler.h"
#include "core/routing_matrix.h"
#include "core/session_registry.h"
#include "network/voice_udp_server.h"

#include <opus/opus.h>

#include <algorithm>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace eevs {

namespace {
    constexpr auto kSynthesisTickInterval = std::chrono::milliseconds(10);
    constexpr size_t kMaxCatchUpTicks = 5;
    constexpr uint8_t kSyntheticEncoderTag = 0xFEu;
    constexpr size_t kFiveMVoicePayloadBytes = 1274; // Fixed 1274-byte Opus buffer in ServerVoicePacket (total 1282 bytes)

    void AccumulatePcmSimd(float* dst, const float* src, size_t count) noexcept {
        size_t i = 0;
#if defined(__AVX2__)
        for (; i + 8 <= count; i += 8) {
            const __m256 a = _mm256_loadu_ps(dst + i);
            const __m256 b = _mm256_loadu_ps(src + i);
            _mm256_storeu_ps(dst + i, _mm256_add_ps(a, b));
        }
#endif
        for (; i < count; ++i) {
            dst[i] += src[i];
        }
    }

    void ClampPcmUnitSimd(float* buf, size_t count) noexcept {
        size_t i = 0;
#if defined(__AVX2__)
        const __m256 minVec = _mm256_set1_ps(-1.0f);
        const __m256 maxVec = _mm256_set1_ps(1.0f);
        for (; i + 8 <= count; i += 8) {
            const __m256 v = _mm256_loadu_ps(buf + i);
            _mm256_storeu_ps(buf + i, _mm256_max_ps(minVec, _mm256_min_ps(maxVec, v)));
        }
#endif
        for (; i < count; ++i) {
            buf[i] = std::clamp(buf[i], -1.0f, 1.0f);
        }
    }
}

PacketSynthesizer::PacketSynthesizer(
    const SessionRegistry& sessions,
    const RoutingMatrix& routing,
    VoiceUdpServer& voiceServer,
    OpusCodecPool& codecs
)
    : m_sessions(sessions)
    , m_routing(routing)
    , m_voiceServer(voiceServer)
    , m_codecs(codecs)
    , m_scratchPacket(sizeof(protocol::ServerVoicePacketHeader) + kFiveMVoicePayloadBytes, 0)
    , m_encodedOpusScratch(OpusCodecPool::kMaxOpusPacketBytes)
    , m_mixAccumPcm(OpusCodecPool::kFrameSamples10ms, 0.0f)
    , m_mixDecodePcm(OpusCodecPool::kMaxFrameSamples, 0.0f) {
}

PacketSynthesizer::~PacketSynthesizer() {
    std::scoped_lock lock(m_mutex);
    for (QueuedOpusBurst& b : m_activeBursts) {
        DestroyBurstDecoder(b);
    }
}

void PacketSynthesizer::DestroyBurstDecoder(QueuedOpusBurst& burst) noexcept {
    if (burst.mixDecoder) {
        opus_decoder_destroy(burst.mixDecoder);
        burst.mixDecoder = nullptr;
    }
}

void PacketSynthesizer::ObserveSpeakerSequence(NetId senderNetId, uint32_t sequenceNumber) noexcept {
    std::scoped_lock lock(m_mutex);
    uint32_t& current = m_sequenceByNetId[senderNetId];
    if (sequenceNumber >= current) {
        current = sequenceNumber + 1;
    }
}

uint32_t PacketSynthesizer::NextSequenceNumber(NetId senderNetId) noexcept {
    std::scoped_lock lock(m_mutex);
    return m_sequenceByNetId[senderNetId]++;
}

void PacketSynthesizer::ClearSequence(NetId senderNetId) noexcept {
    std::scoped_lock lock(m_mutex);
    m_sequenceByNetId.erase(senderNetId);
}

size_t PacketSynthesizer::SendOpusFrameToRoutes(
    NetId senderNetId,
    std::span<const ResolvedRoute> routes,
    OpusFrameView frame
) {
    if (routes.empty() || !frame.data || frame.size == 0 || !m_voiceServer.IsRunning()) {
        return 0;
    }

    // If `senderNetId` is a player's server ID (`clientId`) whose ped `netId` differs,
    // emit the player's actual OneSync ped `netId` in the ServerVoicePacket header so FiveM renders 3D/2D audio properly.
    NetId packetSenderNetId = senderNetId;
    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(senderNetId)) {
        if (const VoicePlayerSession* spkSession = m_sessions.FindByClientId(*speakerClientId)) {
            if (spkSession->netId != 0) {
                packetSenderNetId = spkSession->netId;
            }
        }
    }

    const uint32_t seq = NextSequenceNumber(packetSenderNetId);

    protocol::ServerVoicePacketHeader hdr{};
    hdr.packetId = protocol::kVoiceServerAudioPacketId;
    hdr.senderNetId = packetSenderNetId;
    hdr.sequenceNumber = seq;
    hdr.routingMode = static_cast<uint8_t>(RoutingMode::Spatial3D);

    // FiveM expects a fixed 1282-byte ServerVoicePacket: 8-byte header + 1274-byte zero-padded Opus buffer
    const size_t totalSize = sizeof(protocol::ServerVoicePacketHeader) + kFiveMVoicePayloadBytes;
    if (m_scratchPacket.size() < totalSize) {
        m_scratchPacket.resize(totalSize, 0);
    }

    std::memcpy(m_scratchPacket.data(), &hdr, sizeof(hdr));
    const size_t copyBytes = std::min(frame.size, kFiveMVoicePayloadBytes);
    std::memcpy(m_scratchPacket.data() + sizeof(hdr), frame.data, copyBytes);

    const size_t nonZeroLen = sizeof(hdr) + copyBytes;
    if (m_lastScratchNonZeroLen > nonZeroLen) {
        std::memset(m_scratchPacket.data() + nonZeroLen, 0, m_lastScratchNonZeroLen - nonZeroLen);
    }
    m_lastScratchNonZeroLen = nonZeroLen;

    size_t sentCount = 0;
    for (const ResolvedRoute& route : routes) {
        if (route.mode == RoutingMode::None) {
            continue;
        }

        const VoicePlayerSession* target = m_sessions.FindByClientId(route.targetClientId);
        if (!target || !target->authenticated || !target->peer) {
            continue;
        }

        m_scratchPacket[7] = static_cast<uint8_t>(route.mode);
        if (m_voiceServer.SendRawUdpToPeer(target->peer, m_scratchPacket.data(), totalSize, nonZeroLen)) {
            ++sentCount;
        }
    }

    return sentCount;
}

size_t PacketSynthesizer::InjectOpusFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    std::span<const uint8_t> opusPayload,
    std::span<const ClientId> explicitListeners
) {
    if (opusPayload.empty() || !m_voiceServer.IsRunning()) {
        return 0;
    }

    OpusFrameView frame{
        .data = opusPayload.data(),
        .size = opusPayload.size()
    };

    // 1. Explicit listener list provided by caller
    if (!explicitListeners.empty()) {
        const RoutingMode effectiveMode = (fallbackMode != RoutingMode::None)
            ? fallbackMode
            : RoutingMode::Spatial3D;

        m_scratchRoutes.clear();
        m_scratchRoutes.reserve(explicitListeners.size());
        for (ClientId listenerId : explicitListeners) {
            m_scratchRoutes.push_back(ResolvedRoute{listenerId, effectiveMode});
        }
        return SendOpusFrameToRoutes(senderNetId, m_scratchRoutes, frame);
    }

    // 2. Registered player speaker routes in RoutingMatrix
    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(senderNetId)) {
        std::span<const ResolvedRoute> playerRoutes = m_routing.GetResolvedRoutes(*speakerClientId);
        if (!playerRoutes.empty()) {
            if (fallbackMode == RoutingMode::None) {
                return SendOpusFrameToRoutes(senderNetId, playerRoutes, frame);
            }

            m_scratchRoutes.clear();
            m_scratchRoutes.reserve(playerRoutes.size());
            for (const ResolvedRoute& r : playerRoutes) {
                m_scratchRoutes.push_back(ResolvedRoute{r.targetClientId, fallbackMode});
            }
            return SendOpusFrameToRoutes(senderNetId, m_scratchRoutes, frame);
        }
    }

    // 3. Synthetic / NPC NetID routes configured in RoutingMatrix
    std::span<const ResolvedRoute> synthRoutes = m_routing.GetSyntheticNetRoutes(senderNetId);
    if (!synthRoutes.empty()) {
        return SendOpusFrameToRoutes(senderNetId, synthRoutes, frame);
    }

    // 4. Broadcast fallback when no explicit or per-speaker routes exist
    if (fallbackMode != RoutingMode::None) {
        m_scratchRoutes.clear();
        for (ClientId clientId : m_sessions.GetActiveClientIds()) {
            const VoicePlayerSession* session = m_sessions.FindByClientId(clientId);
            if (session && session->authenticated && session->peer) {
                m_scratchRoutes.push_back(ResolvedRoute{clientId, fallbackMode});
            }
        }
        return SendOpusFrameToRoutes(senderNetId, m_scratchRoutes, frame);
    }

    return 0;
}

size_t PacketSynthesizer::InjectPcmFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    std::span<const float> pcmSamples,
    std::span<const ClientId> explicitListeners
) {
    if (pcmSamples.empty() || !m_voiceServer.IsRunning()) {
        return 0;
    }

    const uint32_t encoderKey = (static_cast<uint32_t>(senderNetId) << 8u) | kSyntheticEncoderTag;
    const int encodedBytes = m_codecs.EncodeFloat(encoderKey, pcmSamples, m_encodedOpusScratch);
    if (encodedBytes <= 0) {
        return 0;
    }

    return InjectOpusFrame(
        senderNetId,
        fallbackMode,
        std::span<const uint8_t>(m_encodedOpusScratch.data(), static_cast<size_t>(encodedBytes)),
        explicitListeners
    );
}

uint64_t PacketSynthesizer::QueueOpusBurst(
    NetId senderNetId,
    RoutingMode mode,
    std::vector<std::vector<uint8_t>> opusFrames,
    std::vector<ClientId> explicitListeners
) {
    if (opusFrames.empty()) {
        return 0;
    }

    std::scoped_lock lock(m_mutex);
    const uint64_t playbackId = m_nextPlaybackId++;
    m_activeBursts.push_back(QueuedOpusBurst{
        .playbackId = playbackId,
        .senderNetId = senderNetId,
        .mode = mode,
        .explicitListeners = std::move(explicitListeners),
        .frames = std::move(opusFrames),
        .nextFrameIndex = 0,
        .mixDecoder = nullptr
    });
    return playbackId;
}

uint64_t PacketSynthesizer::PlayClip(
    const AudioClip& clip,
    NetId senderNetId,
    RoutingMode mode,
    std::vector<ClientId> explicitListeners
) {
    if (clip.opusFrames.empty()) {
        return 0;
    }

    return QueueOpusBurst(
        senderNetId,
        mode,
        clip.opusFrames,
        std::move(explicitListeners)
    );
}

bool PacketSynthesizer::StopPlayback(uint64_t playbackId) {
    std::scoped_lock lock(m_mutex);
    return std::erase_if(m_activeBursts, [playbackId](QueuedOpusBurst& b) {
        if (b.playbackId == playbackId) {
            DestroyBurstDecoder(b);
            return true;
        }
        return false;
    }) > 0;
}

size_t PacketSynthesizer::StopAllPlaybacksForSender(NetId senderNetId) {
    std::scoped_lock lock(m_mutex);
    return std::erase_if(m_activeBursts, [senderNetId](QueuedOpusBurst& b) {
        if (b.senderNetId == senderNetId) {
            DestroyBurstDecoder(b);
            return true;
        }
        return false;
    });
}

bool PacketSynthesizer::IsPlaybackActive(uint64_t playbackId) const noexcept {
    std::scoped_lock lock(m_mutex);
    return std::ranges::any_of(m_activeBursts, [playbackId](const QueuedOpusBurst& b) {
        return b.playbackId == playbackId;
    });
}

void PacketSynthesizer::RegisterTickCallback(TickCallback callback) {
    if (callback) {
        m_tickCallbacks.push_back(std::move(callback));
    }
}

void PacketSynthesizer::Tick() {
    const auto now = std::chrono::steady_clock::now();
    if (!m_timerInitialized) {
        m_nextTickTime = now + kSynthesisTickInterval;
        m_timerInitialized = true;
        return;
    }

    size_t ticksProcessed = 0;
    while (now >= m_nextTickTime && ticksProcessed < kMaxCatchUpTicks) {
        Step10ms();
        m_nextTickTime += kSynthesisTickInterval;
        ++ticksProcessed;
    }

    if (now >= m_nextTickTime) {
        m_nextTickTime = now + kSynthesisTickInterval;
    }
}

void PacketSynthesizer::Step10ms() {
    EEVS_PROFILE_SCOPE("PacketSynthesizer::Step10ms");
    EEVS_PROFILE_FRAME("Synthesis10ms");
    // 1. Advance queued Opus bursts (1 frame per 10ms).
    // When only 1 burst is active for a given senderNetId, emit its pre-encoded Opus frame directly (zero-transcode).
    // When 2+ bursts are simultaneously active for the same senderNetId, decode and sum their 10ms PCM frames into a
    // single mixed Opus frame so FiveM's single-decoder-per-speaker voice channel plays both clips cleanly together.
    {
        std::unique_lock lock(m_mutex);
        const size_t n = m_activeBursts.size();
        for (size_t i = 0; i < n; ++i) {
            QueuedOpusBurst& first = m_activeBursts[i];
            if (first.nextFrameIndex >= first.frames.size()) {
                continue;
            }

            const NetId sender = first.senderNetId;
            bool alreadyProcessedForSender = false;
            for (size_t prev = 0; prev < i; ++prev) {
                if (m_activeBursts[prev].senderNetId == sender) {
                    alreadyProcessedForSender = true;
                    break;
                }
            }
            if (alreadyProcessedForSender) {
                continue;
            }

            size_t concurrentForSender = 0;
            for (size_t j = i; j < n; ++j) {
                if (m_activeBursts[j].senderNetId == sender &&
                    m_activeBursts[j].nextFrameIndex < m_activeBursts[j].frames.size()) {
                    ++concurrentForSender;
                }
            }

            if (concurrentForSender == 1) {
                const auto& frameBytes = first.frames[first.nextFrameIndex++];
                if (!frameBytes.empty()) {
                    const RoutingMode mode = first.mode;
                    const std::vector<ClientId> listeners = first.explicitListeners;
                    const std::vector<uint8_t> frameCopy = frameBytes;
                    lock.unlock();
                    InjectOpusFrame(sender, mode, frameCopy, listeners);
                    lock.lock();
                }
            } else if (concurrentForSender > 1) {
                std::ranges::fill(m_mixAccumPcm, 0.0f);
                RoutingMode combinedMode = RoutingMode::None;
                std::vector<ClientId> combinedListeners;
                size_t mixedStreams = 0;

                for (size_t j = i; j < n; ++j) {
                    QueuedOpusBurst& b = m_activeBursts[j];
                    if (b.senderNetId != sender || b.nextFrameIndex >= b.frames.size()) {
                        continue;
                    }

                    const auto& frameBytes = b.frames[b.nextFrameIndex++];
                    if (frameBytes.empty()) {
                        continue;
                    }

                    combinedMode = combinedMode | b.mode;
                    for (ClientId cid : b.explicitListeners) {
                        if (std::ranges::find(combinedListeners, cid) == combinedListeners.end()) {
                            combinedListeners.push_back(cid);
                        }
                    }

                    if (!b.mixDecoder) {
                        int err = OPUS_OK;
                        b.mixDecoder = opus_decoder_create(
                            static_cast<opus_int32>(OpusCodecPool::kSampleRate),
                            OpusCodecPool::kChannels,
                            &err
                        );
                    }
                    if (!b.mixDecoder) {
                        continue;
                    }

                    const int decoded = opus_decode_float(
                        b.mixDecoder,
                        frameBytes.data(),
                        static_cast<opus_int32>(frameBytes.size()),
                        m_mixDecodePcm.data(),
                        static_cast<int>(m_mixDecodePcm.size()),
                        0
                    );
                    if (decoded > 0) {
                        const size_t count = std::min(static_cast<size_t>(decoded), m_mixAccumPcm.size());
                        AccumulatePcmSimd(m_mixAccumPcm.data(), m_mixDecodePcm.data(), count);
                        ++mixedStreams;
                    }
                }

                if (mixedStreams > 0) {
                    ClampPcmUnitSimd(m_mixAccumPcm.data(), m_mixAccumPcm.size());
                    const RoutingMode effectiveMode = (combinedMode != RoutingMode::None)
                        ? combinedMode
                        : first.mode;
                    const std::vector<float> pcmCopy = m_mixAccumPcm;
                    lock.unlock();
                    InjectPcmFrame(sender, effectiveMode, pcmCopy, combinedListeners);
                    lock.lock();
                }
            }
        }

        std::erase_if(m_activeBursts, [](QueuedOpusBurst& b) {
            if (b.nextFrameIndex >= b.frames.size()) {
                DestroyBurstDecoder(b);
                return true;
            }
            return false;
        });
    }

    // 2. Invoke registered 10ms audio sources
    for (const auto& cb : m_tickCallbacks) {
        cb(*this);
    }
}

} // namespace eevs
