#include "audio_io.h"
#include "containers.h"
#include "json_response.h"
#include "spsc_ring_buffer.h"

#include "eevs/interfaces/i_audio_tap.h"
#include "eevs/interfaces/i_session_listener.h"
#include "eevs/plugin.h"
#include "eevs/plugin_scope.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <glaze/glaze.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eevs::plugins::recorder {

using eevs::plugins::common::CopyStringToResponseBuffer;
using eevs::plugins::common::ErrorRespDto;
using eevs::plugins::common::kRelaxedJsonOpts;
using eevs::plugins::common::WriteJsonToResponseBuffer;
using eevs::plugins::common::SpscRingBuffer;

struct RecorderStartReqDto {
    std::optional<uint16_t> netId;
    uint32_t maxSeconds = 30;
    std::string effect = "clean";
    std::string storage;
    std::string clipId;
    bool isolate = false;
    glz::raw_json options;
};

struct RecorderStartRespDto {
    bool ok = true;
    uint16_t netId = 0;
    std::string storage;
    bool recording = true;
};

struct RecorderStopReqDto {
    std::optional<uint16_t> netId;
    std::string storage;
    bool wait = true;
    glz::raw_json options;
};

struct RecorderSaveReqDto {
    std::string clipId;
    std::string file;
    std::string format;
};

struct RecorderClipReadyDto {
    bool ok = true;
    std::string clipId;
    std::string uri;
    std::string storage;
    std::string effect;
    uint32_t durationMs = 0;
};

} // namespace eevs::plugins::recorder

namespace {

using namespace eevs::plugins::recorder;

struct ActiveRecordingSession {
    eevs::ClientId clientId = 0;
    eevs::NetId netId = 0;
    std::string requestedClipId;
    std::string effectPreset = "clean";
    std::string storageBackend;
    std::string optionsJson;
    bool isolate = false;
    size_t maxSamples = 48000 * 30;
    std::vector<std::vector<uint8_t>> rawOpusFrames;
    std::vector<float> pcmSamples;
};

struct EncodeRecordingJob {
    uint64_t jobId = 0;
    eevs::NetId speakerNetId = 0;
    eevs::IHostContext::AsyncControlRequestHandle requestHandle = 0;
    std::string requestedClipId;
    std::string effectPreset;
    std::vector<std::vector<uint8_t>> rawOpusFrames;
    std::vector<float> pcmSamples;
    uint32_t sampleRate = 48000;
};

class VoiceRecorderPlugin final
    : public eevs::IPlugin
    , public eevs::IAudioTap
    , public eevs::IVoicePacketFilter
    , public eevs::ISessionListener {
public:
    [[nodiscard]] const char* GetName() const noexcept override { return "voice_recorder"; }
    [[nodiscard]] const char* GetVersion() const noexcept override { return "1.2.0"; }

    bool OnLoad(eevs::IHostContext* host) noexcept override {
        m_host = host;
        m_scope.Bind(host);
        m_workerThread = std::jthread([this](std::stop_token st) { WorkerLoop(st); });

        m_scope.AddPacketFilter(this);
        m_scope.AddAudioTap(this);
        m_scope.AddSessionListener(this);

        m_scope.AddEndpoint<&VoiceRecorderPlugin::HandleStartHttp>("/recorder/start", this);
        m_scope.AddAsyncEndpoint<&VoiceRecorderPlugin::HandleStopAsyncHttp>("/recorder/stop", this);
        m_scope.AddEndpoint<&VoiceRecorderPlugin::HandleSaveHttp>("/recorder/save_wav", this);
        m_scope.AddEndpoint<&VoiceRecorderPlugin::HandleSaveHttp>("/clip/save_wav", this);
        m_scope.AddEndpoint<&VoiceRecorderPlugin::HandleSaveHttp>("/clip/save", this);
        m_scope.AddEndpoint<&VoiceRecorderPlugin::HandleLoadHttp>("/clip/load", this);
        return true;
    }

    void OnUnload() noexcept override {
        m_workerThread = std::jthread{}; // Requests stop and joins automatically
        m_scope.Reset();
        m_activeRecordings.clear();
        std::scoped_lock lock(m_savedClipsMutex);
        m_savedClips.clear();
        m_host = nullptr;
    }

    [[nodiscard]] eevs::PacketFilterVerdict InspectIngressPacket(
        const eevs::VoiceIngressPacketView& packet
    ) noexcept override {
        auto it = m_activeRecordings.find(packet.senderClientId);
        if (it != m_activeRecordings.end() && it->second.isolate) {
            return eevs::PacketFilterVerdict::TapOnlyNoForward;
        }
        return eevs::PacketFilterVerdict::Allow;
    }

    void OnOpusFrame(
        const eevs::SpeakerContext& speaker,
        eevs::OpusFrameView frame
    ) noexcept override {
        auto it = m_activeRecordings.find(speaker.clientId);
        if (it == m_activeRecordings.end() || frame.Empty()) {
            return;
        }

        ActiveRecordingSession& rec = it->second;
        const size_t maxFrames = rec.maxSamples / 480u;
        if (rec.rawOpusFrames.size() < maxFrames) {
            const auto span = frame.AsSpan();
            rec.rawOpusFrames.emplace_back(span.begin(), span.end());
        }
    }

    [[nodiscard]] bool WantsDecodedPcm(eevs::ClientId speakerClientId) const noexcept override {
        auto it = m_activeRecordings.find(speakerClientId);
        if (it == m_activeRecordings.end()) {
            return false;
        }
        const auto& preset = it->second.effectPreset;
        return !preset.empty() && preset != "clean" && preset != "none";
    }

    [[nodiscard]] eevs::TapStage GetPcmTapStage() const noexcept override {
        return eevs::TapStage::PostIngressDsp;
    }

    void OnPcmFrame(
        const eevs::SpeakerContext& speaker,
        eevs::PcmFrameView pcm
    ) noexcept override {
        auto it = m_activeRecordings.find(speaker.clientId);
        if (it == m_activeRecordings.end() || pcm.Empty()) {
            return;
        }

        ActiveRecordingSession& rec = it->second;
        if (rec.pcmSamples.size() < rec.maxSamples) {
            const auto span = pcm.AsSpan();
            const size_t allowed = std::min(span.size(), rec.maxSamples - rec.pcmSamples.size());
            rec.pcmSamples.insert(rec.pcmSamples.end(), span.begin(), span.begin() + allowed);
        }
    }

    void OnClientUnregistered(eevs::ClientId clientId, eevs::NetId /*netId*/) noexcept override {
        m_activeRecordings.erase(clientId);
    }

private:
    void WorkerLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            EncodeRecordingJob job{};
            if (!m_jobQueue.TryPop(job)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (!m_host) {
                continue;
            }

            if (job.pcmSamples.empty() && job.rawOpusFrames.empty()) {
                const char* errMsg = "No voice frames captured";
                std::string logMsg = std::format(
                    "Recorder stop failed (netId={}, jobId={}): {}",
                    job.speakerNetId,
                    job.jobId,
                    errMsg
                );
                m_host->Log(eevs::LogLevel::Warn, GetName(), logMsg.c_str());

                if (job.requestHandle != 0) {
                    std::string errJson;
                    (void)glz::write_json(ErrorRespDto{false, errMsg}, errJson);
                    m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 500, errJson.c_str(), errJson.size());
                }
                continue;
            }

            char clipIdBuf[128]{};
            uint32_t durationMs = 0;
            bool stored = false;
            const bool isClean = (job.effectPreset.empty() || job.effectPreset == "clean" || job.effectPreset == "none");

            if (isClean && !job.rawOpusFrames.empty()) {
                std::vector<const uint8_t*> ptrs(job.rawOpusFrames.size());
                std::vector<uint16_t> sizes(job.rawOpusFrames.size());
                for (size_t i = 0; i < job.rawOpusFrames.size(); ++i) {
                    ptrs[i] = job.rawOpusFrames[i].data();
                    sizes[i] = static_cast<uint16_t>(job.rawOpusFrames[i].size());
                }
                stored = m_host->GetClips().StoreOpusClip(
                    job.requestedClipId.c_str(),
                    ptrs.data(),
                    sizes.data(),
                    ptrs.size(),
                    clipIdBuf,
                    sizeof(clipIdBuf),
                    &durationMs
                );
            } else {
                stored = m_host->GetClips().StorePcmClip(
                    job.requestedClipId.c_str(),
                    job.pcmSamples.data(),
                    job.pcmSamples.size(),
                    job.sampleRate,
                    clipIdBuf,
                    sizeof(clipIdBuf),
                    &durationMs
                );
            }

            if (!stored) {
                const char* errMsg = "Failed to encode recording into ClipManager";
                m_host->Log(eevs::LogLevel::Warn, GetName(), errMsg);
                if (job.requestHandle != 0) {
                    std::string errJson;
                    (void)glz::write_json(ErrorRespDto{false, errMsg}, errJson);
                    m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 500, errJson.c_str(), errJson.size());
                }
                continue;
            }

            std::string finalId = clipIdBuf[0] != '\0' ? std::string(clipIdBuf) : std::format("rec_{}", job.jobId);
            {
                std::scoped_lock lock(m_savedClipsMutex);
                m_savedClips[finalId] = SavedRecordingData{
                    .rawOpusFrames = std::move(job.rawOpusFrames),
                    .pcmSamples = std::move(job.pcmSamples),
                    .sampleRate = job.sampleRate
                };
            }

            RecorderClipReadyDto respDto{
                .ok = true,
                .clipId = finalId,
                .uri = std::format("memory://clips/{}", finalId),
                .storage = "memory",
                .effect = std::move(job.effectPreset),
                .durationMs = durationMs
            };

            std::string jsonOut;
            (void)glz::write_json(respDto, jsonOut);

            m_host->GetEvents().Publish("recorder.clip_ready", job.speakerNetId, jsonOut.c_str());

            if (job.requestHandle != 0) {
                m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 200, jsonOut);
            }
        }
    }

    bool HandleStartHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        RecorderStartReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || !req.netId) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Missing or invalid netId"}, responseBuf, responseCap);
            return true;
        }

        const auto netId = *req.netId;
        auto clientIdOpt = m_host->GetSessions().ResolveClientId(netId);
        if (!clientIdOpt) {
            outStatusCode = 404;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Unknown player netId"}, responseBuf, responseCap);
            return true;
        }
        const eevs::ClientId clientId = *clientIdOpt;

        const uint32_t maxSeconds = std::clamp(req.maxSeconds, 1u, 120u);
        std::string selectedStorage = req.storage.empty() ? "default" : req.storage;

        ActiveRecordingSession session{
            .clientId = clientId,
            .netId = netId,
            .requestedClipId = std::move(req.clipId),
            .effectPreset = std::move(req.effect),
            .storageBackend = std::move(req.storage),
            .optionsJson = std::move(req.options.str),
            .isolate = req.isolate,
            .maxSamples = static_cast<size_t>(maxSeconds) * 48000u,
            .pcmSamples = {}
        };
        m_activeRecordings[clientId] = std::move(session);

        outStatusCode = 200;
        WriteJsonToResponseBuffer(
            RecorderStartRespDto{true, netId, selectedStorage, true},
            responseBuf,
            responseCap
        );
        return true;
    }

    bool HandleStopAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        auto completeImmediateError = [this, requestHandle](uint16_t status, std::string_view msg) {
            std::string json;
            (void)glz::write_json(ErrorRespDto{false, msg}, json);
            m_host->GetControlApi().CompleteAsyncRequest(requestHandle, status, json);
            return true;
        };

        RecorderStopReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || !req.netId) {
            return completeImmediateError(400, "Missing or invalid netId");
        }

        const auto netId = *req.netId;
        auto clientIdOpt = m_host->GetSessions().ResolveClientId(netId);
        if (!clientIdOpt) {
            return completeImmediateError(404, "Unknown player netId");
        }

        auto it = m_activeRecordings.find(*clientIdOpt);
        if (it == m_activeRecordings.end()) {
            return completeImmediateError(404, "No active recording for netId");
        }

        ActiveRecordingSession rec = std::move(it->second);
        m_activeRecordings.erase(it);

        const uint64_t jobId = m_nextJobId++;
        EncodeRecordingJob job{
            .jobId = jobId,
            .speakerNetId = netId,
            .requestHandle = req.wait ? requestHandle : 0,
            .requestedClipId = rec.requestedClipId,
            .effectPreset = rec.effectPreset,
            .rawOpusFrames = std::move(rec.rawOpusFrames),
            .pcmSamples = std::move(rec.pcmSamples),
            .sampleRate = 48000
        };

        if (!m_jobQueue.TryPush(std::move(job))) {
            return completeImmediateError(500, "Recorder worker queue full");
        }

        if (!req.wait) {
            RecorderClipReadyDto queuedDto{
                .ok = true,
                .clipId = rec.requestedClipId,
                .uri = "queued",
                .storage = "memory",
                .effect = rec.effectPreset,
                .durationMs = 0
            };
            std::string json;
            (void)glz::write_json(queuedDto, json);
            m_host->GetControlApi().CompleteAsyncRequest(requestHandle, 200, json);
        }
        return true;
    }

    bool HandleSaveHttp(
        std::string_view /*method*/,
        std::string_view reqPath,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        RecorderSaveReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || req.clipId.empty()) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Missing clipId"}, responseBuf, responseCap);
            return true;
        }

        SavedRecordingData data{};
        {
            std::scoped_lock lock(m_savedClipsMutex);
            auto it = m_savedClips.find(req.clipId);
            if (it == m_savedClips.end()) {
                outStatusCode = 404;
                WriteJsonToResponseBuffer(ErrorRespDto{false, "Recording not found in voice_recorder"}, responseBuf, responseCap);
                return true;
            }
            data = it->second;
        }

        const bool wantWav = (reqPath.ends_with("save_wav") || req.format == "wav" || req.format == "WAV" || req.format == ".wav");
        std::string targetFile = req.file;
        if (targetFile.empty()) {
            std::string safeId;
            safeId.reserve(req.clipId.size());
            for (char c : req.clipId) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                    safeId.push_back(c);
                }
            }
            if (safeId.empty()) {
                safeId = "clip";
            }
            const char* recDir = m_host->GetConfigProperty("eevs_recordings_dir", "recordings");
            const char* clipsDir = m_host->GetConfigProperty("eevs_clips_dir", "clips");
            targetFile = wantWav
                ? std::format("{}/{}.wav", recDir, safeId)
                : std::format("{}/{}.eevs", clipsDir, safeId);
        }

        std::string errMsg;
        bool saved = false;
        if (wantWav) {
            std::vector<float> pcm = !data.pcmSamples.empty()
                ? data.pcmSamples
                : eevs::plugins::common::DecodeOpusFramesToPcm(data.rawOpusFrames);
            saved = eevs::plugins::common::WriteMonoFloatToWavFile(targetFile, pcm, data.sampleRate, &errMsg);
        } else {
            std::vector<std::vector<uint8_t>> frames = !data.rawOpusFrames.empty()
                ? data.rawOpusFrames
                : eevs::plugins::common::EncodePcmToOpusFrames(data.pcmSamples, data.sampleRate);
            saved = eevs::plugins::common::WriteEevsClipFile(targetFile, frames, &errMsg);
        }

        if (!saved) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, errMsg.empty() ? "Failed to save recording" : errMsg}, responseBuf, responseCap);
            return true;
        }

        outStatusCode = 200;
        std::string json = std::format(R"({{"ok":true,"clipId":"{}","file":"{}"}})", req.clipId, targetFile);
        CopyStringToResponseBuffer(json, responseBuf, responseCap);
        return true;
    }

    bool HandleLoadHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        RecorderSaveReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || req.file.empty()) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Missing file path"}, responseBuf, responseCap);
            return true;
        }

        uint32_t durationMs = 0;
        std::string errMsg;
        std::string loadedId = eevs::plugins::common::LoadClipFileToHost(
            m_host,
            req.file,
            req.clipId,
            &durationMs,
            &errMsg
        );
        if (loadedId.empty()) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, errMsg.empty() ? "Failed to load clip file" : errMsg}, responseBuf, responseCap);
            return true;
        }

        outStatusCode = 200;
        std::string json = std::format(R"({{"ok":true,"clipId":"{}","durationMs":{}}})", loadedId, durationMs);
        CopyStringToResponseBuffer(json, responseBuf, responseCap);
        return true;
    }

    struct SavedRecordingData {
        std::vector<std::vector<uint8_t>> rawOpusFrames;
        std::vector<float> pcmSamples;
        uint32_t sampleRate = 48000;
    };

    eevs::IHostContext* m_host = nullptr;
    eevs::ScopedPluginRegistrations m_scope;
    eevs::FlatMap<eevs::ClientId, ActiveRecordingSession> m_activeRecordings;
    std::mutex m_savedClipsMutex;
    eevs::FlatMap<std::string, SavedRecordingData> m_savedClips;
    SpscRingBuffer<EncodeRecordingJob, 64> m_jobQueue;
    std::jthread m_workerThread;
    uint64_t m_nextJobId = 1;
};

} // namespace

EEVS_DECLARE_PLUGIN(VoiceRecorderPlugin)
