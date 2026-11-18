#include "audio/audio_pipeline.h"
#include "audio/packet_synthesizer.h"
#include "core/profiler.h"
#include "core/routing_matrix.h"
#include "core/session_registry.h"
#include "network/protocol.h"
#include "network/voice_udp_server.h"

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace eevs {

namespace {
    // Fast AVX2 reverse scan to strip trailing 0x00 padding from FiveM's 1274-byte Opus buffer
    [[nodiscard]] size_t StripTrailingZeroPaddingSimd(const uint8_t* buf, size_t len) noexcept {
#if defined(__AVX2__)
        const __m256i zeroVec = _mm256_setzero_si256();
        while (len > 32) {
            const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buf + len - 32));
            const uint32_t zeroMask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, zeroVec)));
            if (zeroMask == 0xFFFFFFFFu) {
                len -= 32;
                continue;
            }
            const uint32_t nonZeroMask = ~zeroMask;
            const int trailingZeros = std::countl_zero(nonZeroMask);
            return len - static_cast<size_t>(trailingZeros);
        }
#endif
        while (len > 1 && buf[len - 1] == 0x00) {
            --len;
        }
        return len;
    }
} // namespace

AudioPipeline::AudioPipeline(
    const SessionRegistry& sessions,
    const RoutingMatrix& routing,
    VoiceUdpServer& voiceServer,
    PacketSynthesizer& synthesizer,
    OpusCodecPool& codecs
)
    : m_sessions(sessions)
    , m_routing(routing)
    , m_voiceServer(voiceServer)
    , m_synthesizer(synthesizer)
    , m_codecs(codecs)
    , m_activeSpeakers(kMaxClients)
    , m_decodedPcmBuffer(OpusCodecPool::kMaxFrameSamples, 0.0f)
    , m_branchPcmBuffer(OpusCodecPool::kMaxFrameSamples, 0.0f) {
    m_talkingClientIds.reserve(128);
    m_fastPathPacket.assign(protocol::kDefaultMtu, 0);
    m_encodedOpusScratch.resize(OpusCodecPool::kMaxOpusPacketBytes);
    m_speakerEncodedOpusScratch.resize(OpusCodecPool::kMaxOpusPacketBytes);
}

void AudioPipeline::RegisterPacketFilter(IVoicePacketFilter* filter) {
    if (!filter) {
        return;
    }
    if (std::ranges::find(m_packetFilters, filter) == m_packetFilters.end()) {
        m_packetFilters.push_back(filter);
    }
}

void AudioPipeline::UnregisterPacketFilter(IVoicePacketFilter* filter) {
    std::erase(m_packetFilters, filter);
}

void AudioPipeline::RegisterAudioTap(IAudioTap* tap) {
    if (!tap) {
        return;
    }
    if (std::ranges::find(m_audioTaps, tap) == m_audioTaps.end()) {
        m_audioTaps.push_back(tap);
    }
}

void AudioPipeline::UnregisterAudioTap(IAudioTap* tap) {
    std::erase(m_audioTaps, tap);
}

void AudioPipeline::RegisterDspProcessor(IDspProcessor* processor, int priority) {
    if (!processor) {
        return;
    }

    auto& targetVec = (processor->GetStage() == DspStage::PerSpeakerIngress)
        ? m_ingressDsp
        : m_branchDsp;

    for (const auto& entry : targetVec) {
        if (entry.processor == processor) {
            return;
        }
    }

    targetVec.push_back(DspEntry{processor, priority});
    std::ranges::stable_sort(targetVec, [](const DspEntry& a, const DspEntry& b) {
        return a.priority < b.priority;
    });
}

void AudioPipeline::UnregisterDspProcessor(IDspProcessor* processor) {
    auto pred = [processor](const DspEntry& e) { return e.processor == processor; };
    std::erase_if(m_ingressDsp, pred);
    std::erase_if(m_branchDsp, pred);
}

void AudioPipeline::ProcessIngressPacket(
    ClientId senderClientId,
    const uint8_t* data,
    size_t length,
    bool fromFastPath
) {
    EEVS_PROFILE_SCOPE("AudioPipeline::ProcessIngressPacket");

    // Fast-path packet has 18-byte header [peerId(2B)][tokenPrefix(16B)], followed by:
    //   [18]     = packetId (VoicePacketId::ClientVoice -> rewritten to VoicePacketId::ServerVoice)
    //   [19..20] = senderNetId (uint16_t)
    //   [21..24] = sequenceNumber (uint32_t)
    //   [25]     = routingMode (uint8_t)
    //   [26..]   = 1274-byte zero-padded Opus frame payload (starts at byte 26 with Opus TOC byte, e.g. 0x70)
    if (!fromFastPath || length <= 26) {
        return;
    }

    const auto pipelineStart = std::chrono::steady_clock::now();
    ++m_telemetryIngressPackets;

    const VoicePlayerSession* sender = m_sessions.FindByClientId(senderClientId);
    if (!sender || !sender->authenticated) {
        return;
    }

    NetId packetNetId = 0;
    std::memcpy(&packetNetId, data + 19, sizeof(uint16_t));
    if (packetNetId != sender->netId) {
        return;
    }

    uint32_t sequenceNumber = 0;
    std::memcpy(&sequenceNumber, data + 21, sizeof(uint32_t));
    m_synthesizer.ObserveSpeakerSequence(packetNetId, sequenceNumber);

    // Strip trailing 0x00 padding from the 1274-byte FiveM voice buffer so libopus gets the exact Opus frame
    const uint8_t* rawOpusStart = data + 26;
    const size_t rawOpusLen = StripTrailingZeroPaddingSimd(rawOpusStart, length - 26);
    const size_t nonZeroPacketBytes = sizeof(protocol::ServerVoicePacketHeader) + rawOpusLen;

    const bool hasOpusPayload = (rawOpusLen > 0 && rawOpusStart[0] != 0x00);
    OpusFrameView opusView{};
    if (hasOpusPayload) {
        opusView = OpusFrameView{
            .data = rawOpusStart,
            .size = rawOpusLen
        };
    }

    // 0. Run Pre-Decode Ingress Packet Filters (DropSilent > TapOnlyNoForward > Allow)
    PacketFilterVerdict filterVerdict = PacketFilterVerdict::Allow;
    if (!m_packetFilters.empty()) {
        const VoiceIngressPacketView ingressView{
            .senderClientId = senderClientId,
            .senderNetId = packetNetId,
            .sequenceNumber = sequenceNumber,
            .opus = opusView
        };

        for (IVoicePacketFilter* filter : m_packetFilters) {
            const PacketFilterVerdict v = filter->InspectIngressPacket(ingressView);
            if (static_cast<uint8_t>(v) > static_cast<uint8_t>(filterVerdict)) {
                filterVerdict = v;
                if (filterVerdict == PacketFilterVerdict::DropSilent) {
                    return; // Drop packet before VAD, Taps, DSP, or forwarding
                }
            }
        }
    }

    // 1. Update edge-triggered VAD state
    if (senderClientId >= m_activeSpeakers.size()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    ActiveSpeakerState& vadState = m_activeSpeakers[senderClientId];
    vadState.netId = packetNetId;
    vadState.lastPacketTime = now;
    if (!vadState.isTalking) {
        vadState.isTalking = true;
        m_talkingClientIds.push_back(senderClientId);
        m_codecs.ResetSpeakerState(senderClientId);
        for (IAudioTap* tap : m_audioTaps) {
            tap->OnTalkStateChanged(senderClientId, packetNetId, true);
        }
    }

    const SpeakerContext speakerCtx{
        .clientId = senderClientId,
        .netId = packetNetId,
        .sequenceNumber = sequenceNumber,
        .isSynthetic = false
    };

    if (hasOpusPayload) {
        // 2. Deliver raw Opus frame to registered taps
        for (IAudioTap* tap : m_audioTaps) {
            tap->OnOpusFrame(speakerCtx, opusView);
        }
    }

    // TapOnlyNoForward suppresses outgoing listener routes while still delivering frames to taps
    std::span<const ResolvedRoute> routes = (filterVerdict == PacketFilterVerdict::TapOnlyNoForward)
        ? std::span<const ResolvedRoute>{}
        : m_routing.GetResolvedRoutes(senderClientId);

    // 3. Check if Opus decoding is required for any tap or DSP processor
    bool anyTapWantsPcm = false;
    if (hasOpusPayload) {
        for (const IAudioTap* tap : m_audioTaps) {
            if (tap->WantsDecodedPcm(senderClientId)) {
                anyTapWantsPcm = true;
                break;
            }
        }
    }

    bool anyIngressDspActive = false;
    if (hasOpusPayload && !routes.empty()) {
        for (const DspEntry& entry : m_ingressDsp) {
            if (entry.processor->ShouldProcess(senderClientId, packetNetId, RoutingMode::None)) {
                anyIngressDspActive = true;
                break;
            }
        }
    }

    uint8_t routeModesPresentMask = 0;
    for (const ResolvedRoute& r : routes) {
        const auto modeIdx = static_cast<uint8_t>(r.mode);
        if (modeIdx >= 1 && modeIdx <= 3) {
            routeModesPresentMask |= static_cast<uint8_t>(1u << modeIdx);
        }
    }

    uint8_t branchDspActiveMask = 0;
    if (hasOpusPayload && routeModesPresentMask != 0 && !m_branchDsp.empty()) {
        for (uint8_t modeIdx = 1; modeIdx <= 3; ++modeIdx) {
            if ((routeModesPresentMask & (1u << modeIdx)) == 0) {
                continue;
            }
            const auto mode = static_cast<RoutingMode>(modeIdx);
            for (const DspEntry& entry : m_branchDsp) {
                if (entry.processor->ShouldProcess(senderClientId, packetNetId, mode)) {
                    branchDspActiveMask |= static_cast<uint8_t>(1u << modeIdx);
                    break;
                }
            }
        }
    }

    auto recordPipelineEnd = [this, pipelineStart]() noexcept {
        const auto elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - pipelineStart
            ).count()
        );
        m_telemetryPipelineTotalNs += elapsedNs;
        m_telemetryPipelineMaxNs = std::max(m_telemetryPipelineMaxNs, elapsedNs);
    };

    // 4. Fast-Path: No tap needs PCM and no DSP processor is active for this speaker
    if (!anyTapWantsPcm && !anyIngressDspActive && branchDspActiveMask == 0) {
        if (!routes.empty()) {
            ForwardRawPacketToRoutes(data + 18, length - 18, nonZeroPacketBytes, routes, RoutingMode::None);
        }
        recordPipelineEnd();
        return;
    }

    // 5. Lazy Decode Path: Decode Opus -> 48kHz float PCM once
    int decodedSamples = 0;
    {
        EEVS_PROFILE_SCOPE("OpusDecode");
        const auto t0 = std::chrono::steady_clock::now();
        decodedSamples = m_codecs.DecodeFloat(
            senderClientId,
            std::span<const uint8_t>(opusView.data, opusView.size),
            m_decodedPcmBuffer
        );
        const auto dtNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
        );
        ++m_telemetryDecodedFrames;
        m_telemetryDecodeTotalNs += dtNs;
        m_telemetryDecodeMaxNs = std::max(m_telemetryDecodeMaxNs, dtNs);
    }

    if (decodedSamples <= 0) {
        // Fallback to raw packet forwarding if Opus frame failed to decode
        if (!routes.empty()) {
            ForwardRawPacketToRoutes(data + 18, length - 18, nonZeroPacketBytes, routes, RoutingMode::None);
        }
        recordPipelineEnd();
        return;
    }

    const auto sampleCount = static_cast<size_t>(decodedSamples);

    // 6. Deliver PreDsp PCM to taps
    if (anyTapWantsPcm) {
        EEVS_PROFILE_SCOPE("AudioTap::PreDsp");
        const PcmFrameView preDspView{
            .samples = m_decodedPcmBuffer.data(),
            .sampleCount = sampleCount,
            .sampleRate = OpusCodecPool::kSampleRate
        };
        for (IAudioTap* tap : m_audioTaps) {
            if (tap->WantsDecodedPcm(senderClientId) && tap->GetPcmTapStage() == TapStage::PreDsp) {
                tap->OnPcmFrame(speakerCtx, preDspView);
            }
        }
    }

    // 7. Run PerSpeakerIngress DSP processors
    if (anyIngressDspActive) {
        EEVS_PROFILE_SCOPE("DSP::PerSpeakerIngress");
        const auto t0 = std::chrono::steady_clock::now();
        PcmFrameBuffer speakerPcm{
            .samples = m_decodedPcmBuffer.data(),
            .sampleCount = sampleCount,
            .sampleRate = OpusCodecPool::kSampleRate
        };
        for (const DspEntry& entry : m_ingressDsp) {
            if (entry.processor->ShouldProcess(senderClientId, packetNetId, RoutingMode::None)) {
                entry.processor->ProcessPcm(speakerCtx, RoutingMode::None, speakerPcm);
            }
        }
        const auto dtNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
        );
        ++m_telemetryDspFrames;
        m_telemetryDspTotalNs += dtNs;
        m_telemetryDspMaxNs = std::max(m_telemetryDspMaxNs, dtNs);
    }

    // 8. Deliver PostIngressDsp PCM to taps
    if (anyTapWantsPcm) {
        EEVS_PROFILE_SCOPE("AudioTap::PostIngressDsp");
        const PcmFrameView postDspView{
            .samples = m_decodedPcmBuffer.data(),
            .sampleCount = sampleCount,
            .sampleRate = OpusCodecPool::kSampleRate
        };
        for (IAudioTap* tap : m_audioTaps) {
            if (tap->WantsDecodedPcm(senderClientId) && tap->GetPcmTapStage() == TapStage::PostIngressDsp) {
                tap->OnPcmFrame(speakerCtx, postDspView);
            }
        }
    }

    if (routes.empty()) {
        recordPipelineEnd();
        return;
    }

    // 9. Route per branch (Spatial3D = 1, NonSpatial = 2, Both = 3)
    int speakerEncodedBytes = -1;

    for (uint8_t modeIdx = 1; modeIdx <= 3; ++modeIdx) {
        if ((routeModesPresentMask & (1u << modeIdx)) == 0) {
            continue;
        }

        const auto branchMode = static_cast<RoutingMode>(modeIdx);
        const bool hasBranchDsp = (branchDspActiveMask & (1u << modeIdx)) != 0;

        if (!anyIngressDspActive && !hasBranchDsp) {
            // Unmodified branch: forward original raw Opus bytes directly
            ForwardRawPacketToRoutes(data + 18, length - 18, nonZeroPacketBytes, routes, branchMode);
            continue;
        }

        if (!hasBranchDsp) {
            // Only speaker-level ingress DSP ran: encode once and reuse across clean branches
            if (speakerEncodedBytes < 0) {
                EEVS_PROFILE_SCOPE("OpusEncode::IngressBranch");
                const auto t0 = std::chrono::steady_clock::now();
                const uint32_t encKey = OpusCodecPool::MakeEncoderKey(senderClientId, RoutingMode::None);
                speakerEncodedBytes = m_codecs.EncodeFloat(
                    encKey,
                    std::span<const float>(m_decodedPcmBuffer.data(), sampleCount),
                    m_speakerEncodedOpusScratch
                );
                const auto dtNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
                );
                ++m_telemetryEncodedFrames;
                m_telemetryEncodeTotalNs += dtNs;
                m_telemetryEncodeMaxNs = std::max(m_telemetryEncodeMaxNs, dtNs);
            }

            if (speakerEncodedBytes > 0) {
                BuildAndSendServerVoicePacket(
                    packetNetId,
                    sequenceNumber,
                    std::span<const uint8_t>(m_speakerEncodedOpusScratch.data(), static_cast<size_t>(speakerEncodedBytes)),
                    routes,
                    branchMode
                );
            } else {
                ForwardRawPacketToRoutes(data + 18, length - 18, nonZeroPacketBytes, routes, branchMode);
            }
            continue;
        }

        // Branch DSP active: clone PCM, process, and encode
        std::copy_n(m_decodedPcmBuffer.data(), sampleCount, m_branchPcmBuffer.data());
        PcmFrameBuffer branchPcm{
            .samples = m_branchPcmBuffer.data(),
            .sampleCount = sampleCount,
            .sampleRate = OpusCodecPool::kSampleRate
        };

        {
            EEVS_PROFILE_SCOPE("DSP::PerRouteBranch");
            const auto t0 = std::chrono::steady_clock::now();
            for (const DspEntry& entry : m_branchDsp) {
                if (entry.processor->ShouldProcess(senderClientId, packetNetId, branchMode)) {
                    entry.processor->ProcessPcm(speakerCtx, branchMode, branchPcm);
                }
            }
            const auto dtNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
            );
            ++m_telemetryDspFrames;
            m_telemetryDspTotalNs += dtNs;
            m_telemetryDspMaxNs = std::max(m_telemetryDspMaxNs, dtNs);
        }

        int branchEncodedBytes = 0;
        {
            EEVS_PROFILE_SCOPE("OpusEncode::RouteBranch");
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t branchEncKey = OpusCodecPool::MakeEncoderKey(senderClientId, branchMode);
            branchEncodedBytes = m_codecs.EncodeFloat(
                branchEncKey,
                std::span<const float>(m_branchPcmBuffer.data(), sampleCount),
                m_encodedOpusScratch
            );
            const auto dtNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
            );
            ++m_telemetryEncodedFrames;
            m_telemetryEncodeTotalNs += dtNs;
            m_telemetryEncodeMaxNs = std::max(m_telemetryEncodeMaxNs, dtNs);
        }

        if (branchEncodedBytes > 0) {
            BuildAndSendServerVoicePacket(
                packetNetId,
                sequenceNumber,
                std::span<const uint8_t>(m_encodedOpusScratch.data(), static_cast<size_t>(branchEncodedBytes)),
                routes,
                branchMode
            );
        } else {
            ForwardRawPacketToRoutes(data + 18, length - 18, nonZeroPacketBytes, routes, branchMode);
        }
    }

    recordPipelineEnd();
}

void AudioPipeline::ForwardRawPacketToRoutes(
    const uint8_t* clientVoicePacketAtByte18,
    size_t payloadLengthFromByte18,
    size_t nonZeroPrefixBytes,
    std::span<const ResolvedRoute> routes,
    RoutingMode filterMode
) {
    if (m_fastPathPacket.size() < payloadLengthFromByte18) {
        m_fastPathPacket.resize(payloadLengthFromByte18, 0);
    }
    const size_t copyLen = std::min(nonZeroPrefixBytes, payloadLengthFromByte18);
    std::memcpy(m_fastPathPacket.data(), clientVoicePacketAtByte18, copyLen);
    if (m_lastFastPathNonZeroLen > copyLen) {
        std::memset(m_fastPathPacket.data() + copyLen, 0, m_lastFastPathNonZeroLen - copyLen);
    }
    m_lastFastPathNonZeroLen = copyLen;
    m_fastPathPacket[0] = protocol::kVoiceServerAudioPacketId;

    for (const ResolvedRoute& route : routes) {
        if (route.mode == RoutingMode::None) {
            continue;
        }
        if (filterMode != RoutingMode::None && route.mode != filterMode) {
            continue;
        }

        const VoicePlayerSession* target = m_sessions.FindByClientId(route.targetClientId);
        if (!target || !target->authenticated || !target->peer) {
            continue;
        }

        m_fastPathPacket[7] = static_cast<uint8_t>(route.mode);
        if (m_voiceServer.SendRawUdpToPeer(target->peer, m_fastPathPacket.data(), payloadLengthFromByte18, copyLen)) {
            ++m_telemetryEgressPackets;
        }
    }
}

void AudioPipeline::BuildAndSendServerVoicePacket(
    NetId senderNetId,
    uint32_t sequenceNumber,
    std::span<const uint8_t> opusPayload,
    std::span<const ResolvedRoute> routes,
    RoutingMode filterMode
) {
    constexpr size_t kFiveMVoicePayloadBytes = 1274;

    protocol::ServerVoicePacketHeader hdr{};
    hdr.packetId = protocol::kVoiceServerAudioPacketId;
    hdr.senderNetId = senderNetId;
    hdr.sequenceNumber = sequenceNumber;
    hdr.routingMode = static_cast<uint8_t>(filterMode);

    const size_t totalSize = sizeof(hdr) + kFiveMVoicePayloadBytes;
    if (m_fastPathPacket.size() < totalSize) {
        m_fastPathPacket.resize(totalSize, 0);
    }

    std::memcpy(m_fastPathPacket.data(), &hdr, sizeof(hdr));
    const size_t copyBytes = std::min(opusPayload.size(), kFiveMVoicePayloadBytes);
    std::memcpy(m_fastPathPacket.data() + sizeof(hdr), opusPayload.data(), copyBytes);

    const size_t nonZeroLen = sizeof(hdr) + copyBytes;
    if (m_lastFastPathNonZeroLen > nonZeroLen) {
        std::memset(m_fastPathPacket.data() + nonZeroLen, 0, m_lastFastPathNonZeroLen - nonZeroLen);
    }
    m_lastFastPathNonZeroLen = nonZeroLen;

    for (const ResolvedRoute& route : routes) {
        if (route.mode == RoutingMode::None) {
            continue;
        }
        if (filterMode != RoutingMode::None && route.mode != filterMode) {
            continue;
        }

        const VoicePlayerSession* target = m_sessions.FindByClientId(route.targetClientId);
        if (!target || !target->authenticated || !target->peer) {
            continue;
        }

        m_fastPathPacket[7] = static_cast<uint8_t>(route.mode);
        if (m_voiceServer.SendRawUdpToPeer(target->peer, m_fastPathPacket.data(), totalSize, nonZeroLen)) {
            ++m_telemetryEgressPackets;
        }
    }
}

void AudioPipeline::OnClientUnregistered(ClientId clientId) noexcept {
    if (clientId < m_activeSpeakers.size()) {
        ActiveSpeakerState& state = m_activeSpeakers[clientId];
        if (state.isTalking) {
            state.isTalking = false;
            std::erase(m_talkingClientIds, clientId);
            const NetId netId = state.netId;
            for (IAudioTap* tap : m_audioTaps) {
                tap->OnTalkStateChanged(clientId, netId, false);
            }
        }
        state = ActiveSpeakerState{};
    }

    m_codecs.ReleaseSpeaker(clientId);
}

void AudioPipeline::TickVad() {
    if (m_talkingClientIds.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (size_t i = m_talkingClientIds.size(); i > 0; --i) {
        const ClientId clientId = m_talkingClientIds[i - 1];
        ActiveSpeakerState& state = m_activeSpeakers[clientId];
        if (!state.isTalking || (now - state.lastPacketTime) >= m_vadHangoverDuration) {
            const bool wasTalking = state.isTalking;
            state.isTalking = false;
            m_talkingClientIds[i - 1] = m_talkingClientIds.back();
            m_talkingClientIds.pop_back();
            if (wasTalking) {
                m_codecs.ResetSpeakerState(clientId);
                for (IAudioTap* tap : m_audioTaps) {
                    tap->OnTalkStateChanged(clientId, state.netId, false);
                }
            }
        }
    }
}

bool AudioPipeline::IsSpeakerTalking(ClientId clientId) const noexcept {
    return clientId < m_activeSpeakers.size() && m_activeSpeakers[clientId].isTalking;
}

AudioPipeline::TelemetrySnapshot AudioPipeline::GetTelemetrySnapshot() const noexcept {
    auto avgUs = [](uint64_t totalNs, uint64_t count) noexcept -> double {
        return count > 0 ? (static_cast<double>(totalNs) / static_cast<double>(count)) / 1000.0 : 0.0;
    };

    return TelemetrySnapshot{
        .ingressPackets = m_telemetryIngressPackets,
        .egressPackets = m_telemetryEgressPackets,
        .activeSpeakers = m_talkingClientIds.size(),
        .decodedFrames = m_telemetryDecodedFrames,
        .opusDecodeAvgUs = avgUs(m_telemetryDecodeTotalNs, m_telemetryDecodedFrames),
        .opusDecodeMaxUs = static_cast<double>(m_telemetryDecodeMaxNs) / 1000.0,
        .dspFrames = m_telemetryDspFrames,
        .dspProcessAvgUs = avgUs(m_telemetryDspTotalNs, m_telemetryDspFrames),
        .dspProcessMaxUs = static_cast<double>(m_telemetryDspMaxNs) / 1000.0,
        .encodedFrames = m_telemetryEncodedFrames,
        .opusEncodeAvgUs = avgUs(m_telemetryEncodeTotalNs, m_telemetryEncodedFrames),
        .opusEncodeMaxUs = static_cast<double>(m_telemetryEncodeMaxNs) / 1000.0,
        .pipelineAvgUs = avgUs(m_telemetryPipelineTotalNs, m_telemetryIngressPackets),
        .pipelineMaxUs = static_cast<double>(m_telemetryPipelineMaxNs) / 1000.0
    };
}

void AudioPipeline::ResetTelemetry() noexcept {
    m_telemetryIngressPackets = 0;
    m_telemetryEgressPackets = 0;
    m_telemetryDecodedFrames = 0;
    m_telemetryDecodeTotalNs = 0;
    m_telemetryDecodeMaxNs = 0;
    m_telemetryDspFrames = 0;
    m_telemetryDspTotalNs = 0;
    m_telemetryDspMaxNs = 0;
    m_telemetryEncodedFrames = 0;
    m_telemetryEncodeTotalNs = 0;
    m_telemetryEncodeMaxNs = 0;
    m_telemetryPipelineTotalNs = 0;
    m_telemetryPipelineMaxNs = 0;
}

} // namespace eevs
