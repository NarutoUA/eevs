#pragma once

#include "audio/opus_codec_pool.h"
#include "eevs/interfaces/i_audio_tap.h"
#include "eevs/interfaces/i_dsp_processor.h"
#include "eevs/interfaces/i_voice_packet_filter.h"
#include "eevs/types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace eevs {

class PacketSynthesizer;
class RoutingMatrix;
class SessionRegistry;
class VoiceUdpServer;

class AudioPipeline {
public:
    static constexpr auto kVadHangoverDuration = std::chrono::milliseconds(150);

    struct TelemetrySnapshot {
        uint64_t ingressPackets = 0;
        uint64_t egressPackets = 0;
        size_t activeSpeakers = 0;
        uint64_t decodedFrames = 0;
        double opusDecodeAvgUs = 0.0;
        double opusDecodeMaxUs = 0.0;
        uint64_t dspFrames = 0;
        double dspProcessAvgUs = 0.0;
        double dspProcessMaxUs = 0.0;
        uint64_t encodedFrames = 0;
        double opusEncodeAvgUs = 0.0;
        double opusEncodeMaxUs = 0.0;
        double pipelineAvgUs = 0.0;
        double pipelineMaxUs = 0.0;
    };

    AudioPipeline(
        const SessionRegistry& sessions,
        const RoutingMatrix& routing,
        VoiceUdpServer& voiceServer,
        PacketSynthesizer& synthesizer,
        OpusCodecPool& codecs
    );
    ~AudioPipeline() = default;

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // Plugin / Capability registration
    void RegisterPacketFilter(IVoicePacketFilter* filter);
    void UnregisterPacketFilter(IVoicePacketFilter* filter);

    void RegisterAudioTap(IAudioTap* tap);
    void UnregisterAudioTap(IAudioTap* tap);

    void RegisterDspProcessor(IDspProcessor* processor, int priority = 0);
    void UnregisterDspProcessor(IDspProcessor* processor);

    // Processes an incoming ClientVoicePacket from a game client
    void ProcessIngressPacket(
        ClientId senderClientId,
        const uint8_t* data,
        size_t length,
        bool fromFastPath
    );

    // Called when a client unregisters to clean up VAD state and codec instances
    void OnClientUnregistered(ClientId clientId) noexcept;

    // Called every 10ms tick to detect end-of-speech (VAD hangover timeout)
    void TickVad();

    void SetVadHangoverMs(uint32_t hangoverMs) noexcept {
        m_vadHangoverDuration = std::chrono::milliseconds(hangoverMs);
    }

    [[nodiscard]] bool IsSpeakerTalking(ClientId clientId) const noexcept;
    [[nodiscard]] TelemetrySnapshot GetTelemetrySnapshot() const noexcept;
    void ResetTelemetry() noexcept;

private:
    struct DspEntry {
        IDspProcessor* processor = nullptr;
        int priority = 0;
    };

    struct ActiveSpeakerState {
        NetId netId = 0;
        std::chrono::steady_clock::time_point lastPacketTime{};
        bool isTalking = false;
    };

    void ForwardRawPacketToRoutes(
        const uint8_t* clientVoicePacketAtByte18,
        size_t payloadLengthFromByte18,
        size_t nonZeroPrefixBytes,
        std::span<const ResolvedRoute> routes,
        RoutingMode filterMode = RoutingMode::None
    );

    void BuildAndSendServerVoicePacket(
        NetId senderNetId,
        uint32_t sequenceNumber,
        std::span<const uint8_t> opusPayload,
        std::span<const ResolvedRoute> routes,
        RoutingMode filterMode
    );

    const SessionRegistry& m_sessions;
    const RoutingMatrix& m_routing;
    VoiceUdpServer& m_voiceServer;
    PacketSynthesizer& m_synthesizer;
    OpusCodecPool& m_codecs;

    std::vector<IVoicePacketFilter*> m_packetFilters;
    std::vector<IAudioTap*> m_audioTaps;
    std::vector<DspEntry> m_ingressDsp;
    std::vector<DspEntry> m_branchDsp;

    std::chrono::milliseconds m_vadHangoverDuration = kVadHangoverDuration;
    std::vector<ActiveSpeakerState> m_activeSpeakers;
    std::vector<ClientId> m_talkingClientIds;

    std::vector<uint8_t> m_fastPathPacket;
    size_t m_lastFastPathNonZeroLen = 0;
    std::vector<uint8_t> m_encodedOpusScratch;
    std::vector<uint8_t> m_speakerEncodedOpusScratch;
    std::vector<float> m_decodedPcmBuffer;
    std::vector<float> m_branchPcmBuffer;

    uint64_t m_telemetryIngressPackets = 0;
    uint64_t m_telemetryEgressPackets = 0;
    uint64_t m_telemetryDecodedFrames = 0;
    uint64_t m_telemetryDecodeTotalNs = 0;
    uint64_t m_telemetryDecodeMaxNs = 0;
    uint64_t m_telemetryDspFrames = 0;
    uint64_t m_telemetryDspTotalNs = 0;
    uint64_t m_telemetryDspMaxNs = 0;
    uint64_t m_telemetryEncodedFrames = 0;
    uint64_t m_telemetryEncodeTotalNs = 0;
    uint64_t m_telemetryEncodeMaxNs = 0;
    uint64_t m_telemetryPipelineTotalNs = 0;
    uint64_t m_telemetryPipelineMaxNs = 0;
};

} // namespace eevs
