#pragma once

#include "core/containers.h"
#include "eevs/types.h"
#include "network/protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

struct OpusDecoder;

namespace eevs {

struct AudioClip;
class OpusCodecPool;
class RoutingMatrix;
class SessionRegistry;
class VoiceUdpServer;

class PacketSynthesizer {
public:
    using TickCallback = std::function<void(PacketSynthesizer&)>;

    PacketSynthesizer(
        const SessionRegistry& sessions,
        const RoutingMatrix& routing,
        VoiceUdpServer& voiceServer,
        OpusCodecPool& codecs
    );
    ~PacketSynthesizer();

    PacketSynthesizer(const PacketSynthesizer&) = delete;
    PacketSynthesizer& operator=(const PacketSynthesizer&) = delete;

    // Keeps per-NetID sequence counter monotonic with live ClientVoicePackets
    void ObserveSpeakerSequence(NetId senderNetId, uint32_t sequenceNumber) noexcept;
    [[nodiscard]] uint32_t NextSequenceNumber(NetId senderNetId) noexcept;
    void ClearSequence(NetId senderNetId) noexcept;

    // Sends a single 10ms Opus frame (`ServerVoicePacket`) to an explicit list of resolved routes
    size_t SendOpusFrameToRoutes(
        NetId senderNetId,
        std::span<const ResolvedRoute> routes,
        OpusFrameView frame
    );

    // Injects a single 10ms Opus frame (`ServerVoicePacket`) using explicit listeners, player routes,
    // synthetic NetID routes, or broadcast fallback when fallbackMode != RoutingMode::None
    size_t InjectOpusFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const uint8_t> opusPayload,
        std::span<const ClientId> explicitListeners = {}
    );

    // Injects a single 10ms 48kHz float PCM frame (480 samples), encoding to Opus via OpusCodecPool
    size_t InjectPcmFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const float> pcmSamples,
        std::span<const ClientId> explicitListeners = {}
    );

    // Queues a sequence of 10ms Opus frames to be emitted automatically on each 10ms tick.
    // Returns a non-zero playbackId (> 0).
    uint64_t QueueOpusBurst(
        NetId senderNetId,
        RoutingMode mode,
        std::vector<std::vector<uint8_t>> opusFrames,
        std::vector<ClientId> explicitListeners = {}
    );

    // Queues playback of an existing AudioClip and returns a non-zero playbackId (> 0).
    uint64_t PlayClip(
        const AudioClip& clip,
        NetId senderNetId,
        RoutingMode mode,
        std::vector<ClientId> explicitListeners = {}
    );

    bool StopPlayback(uint64_t playbackId);
    size_t StopAllPlaybacksForSender(NetId senderNetId);
    [[nodiscard]] bool IsPlaybackActive(uint64_t playbackId) const noexcept;

    void RegisterTickCallback(TickCallback callback);

    // Driven by VoiceEngine::Poll(); executes 10ms synthesis steps when due
    void Tick();

private:
    struct QueuedOpusBurst {
        uint64_t playbackId = 0;
        NetId senderNetId = 0;
        RoutingMode mode = RoutingMode::Spatial3D;
        std::vector<ClientId> explicitListeners;
        std::vector<std::vector<uint8_t>> frames;
        size_t nextFrameIndex = 0;
        OpusDecoder* mixDecoder = nullptr;
    };

    void Step10ms();
    static void DestroyBurstDecoder(QueuedOpusBurst& burst) noexcept;

    const SessionRegistry& m_sessions;
    const RoutingMatrix& m_routing;
    VoiceUdpServer& m_voiceServer;
    OpusCodecPool& m_codecs;

    mutable std::mutex m_mutex;
    FlatMap<NetId, uint32_t> m_sequenceByNetId;
    std::vector<uint8_t> m_scratchPacket;
    size_t m_lastScratchNonZeroLen = 0;
    std::vector<uint8_t> m_encodedOpusScratch;
    std::vector<ResolvedRoute> m_scratchRoutes;
    std::vector<float> m_mixAccumPcm;
    std::vector<float> m_mixDecodePcm;

    uint64_t m_nextPlaybackId = 1;
    std::vector<QueuedOpusBurst> m_activeBursts;
    std::vector<TickCallback> m_tickCallbacks;
    std::chrono::steady_clock::time_point m_nextTickTime{};
    bool m_timerInitialized = false;
};

} // namespace eevs
