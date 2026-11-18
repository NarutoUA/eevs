#include "audio_io.h"
#include "containers.h"
#include "json_response.h"
#include "spsc_ring_buffer.h"

#include "eevs/interfaces/i_audio_tap.h"
#include "eevs/interfaces/i_session_listener.h"
#include "eevs/interfaces/i_voice_packet_filter.h"
#include "eevs/plugin.h"
#include "eevs/plugin_scope.h"

#include <glaze/base64/base64.hpp>
#include <glaze/glaze.hpp>
#include <openssl/bio.h>
#include <openssl/http.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace eevs::plugins::audio_cpp_client {

using eevs::plugins::common::CopyStringToResponseBuffer;
using eevs::plugins::common::ErrorRespDto;
using eevs::plugins::common::kRelaxedJsonOpts;
using eevs::plugins::common::WriteJsonToResponseBuffer;
using eevs::plugins::common::SpscRingBuffer;

struct SttStartReqDto {
    std::optional<uint16_t> netId;
    std::string language = "en";
    bool isolate = false;
    bool wait = true;
    glz::raw_json options;
};

struct SttStartRespDto {
    bool ok = true;
    uint16_t netId = 0;
    std::string backend = "audio.cpp";
    bool capturing = true;
};

struct SttCompletedDto {
    bool ok = true;
    uint64_t jobId = 0;
    uint16_t netId = 0;
    uint32_t durationMs = 0;
    float confidence = 0.95f;
    float rmsDb = -96.0f;
    std::string backend = "audio.cpp";
    std::string text;
    std::string error;
};

struct TtsReqDto {
    std::optional<uint16_t> senderNetId;
    std::string text;
    std::string voice;
    std::string instruct;
    std::optional<uint64_t> seed;
    std::optional<float> gain;
    std::string mode = "3D";
    std::string clipId;
    glz::raw_json options;
};

struct TtsClipRespDto {
    bool ok = true;
    std::string clipId;
    std::string backend = "audio.cpp";
    uint32_t durationMs = 0;
};

struct TtsSpeakRespDto {
    bool ok = true;
    std::string clipId;
    std::string backend = "audio.cpp";
    uint64_t playbackId = 0;
    uint32_t durationMs = 0;
};

struct RequestOptionsModelDto {
    std::string model;
};

struct AudioCppParamDescriptorDto {
    std::string name;
};

struct AudioCppModelItemDto {
    std::string id;
    std::string family;
    std::string task;
    std::string mode;
    bool loaded = false;
    glz::raw_json params;
};

struct AudioCppModelsListDto {
    std::vector<AudioCppModelItemDto> data;
};

struct AudioCppVoicesListDto {
    std::vector<std::string> voices;
};

struct AudioCppModelsRootDto {
    std::string models_root;
    std::string default_models_root;
};

struct AudioCppPackageManifestDto {
    std::string package_id;
};

struct ModelsEndpointRespDto {
    bool ok = true;
    std::string backend = "audio.cpp";
    std::string url;
    std::vector<std::string> models;
    std::vector<std::string> voices;
    std::vector<AudioCppModelItemDto> modelDetails;
};

struct AudioCppSpeechReqDto {
    std::string model;
    std::string input;
    std::optional<std::string> voice;
    std::optional<std::string> voice_ref;
    std::optional<std::string> reference_text;
    std::optional<std::string> instructions;
    std::optional<uint64_t> seed;
    glz::raw_json options;
};

struct AudioCppUploadRespDto {
    std::string path;
    size_t bytes = 0;
};

struct AudioCppTaskInnerReqDto {
    std::optional<std::string> text;
    std::optional<std::string> language;
    std::optional<std::string> audio_base64;
    std::optional<std::string> voice;
    glz::raw_json options;
};

struct AudioCppTaskRunReqDto {
    std::string model;
    AudioCppTaskInnerReqDto request;
};

struct AudioCppErrorInnerDto {
    std::string message;
    std::string type;
};

struct AudioCppTaskResultDto {
    std::string text;
    std::string language;
    std::string audio;
    std::optional<AudioCppErrorInnerDto> error;
};
} // namespace eevs::plugins::audio_cpp_client

namespace {

using namespace eevs::plugins::audio_cpp_client;

float ComputeRmsDb(std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return -96.0f;
    }

    double sumSq = 0.0;
    for (float s : samples) {
        sumSq += static_cast<double>(s) * static_cast<double>(s);
    }

    const double rms = std::sqrt(sumSq / static_cast<double>(samples.size()));
    return (rms <= 1e-5) ? -96.0f : static_cast<float>(20.0 * std::log10(rms));
}

eevs::RoutingMode ParseModeString(std::string_view modeStr) noexcept {
    if (modeStr == "2D" || modeStr == "2d" || modeStr == "radio") {
        return eevs::RoutingMode::NonSpatial;
    }

    if (modeStr == "BOTH" || modeStr == "both") {
        return eevs::RoutingMode::Both;
    }

    return eevs::RoutingMode::Spatial3D;
}

// Performs an HTTP GET (when reqBody is empty) or POST (when reqBody is non-empty) via OpenSSL's HTTP client
std::optional<std::string> HttpTransfer(
    const std::string& host,
    const std::string& port,
    const char* path,
    std::string_view reqBody = {},
    const char* contentType = "application/json",
    int timeoutSec = 25
) {
    constexpr size_t kMaxHttpResponseBytes = 64ull * 1024ull * 1024ull; // 64 MB for uncompressed WAV audio
    BIO* reqBio = reqBody.empty()
        ? nullptr
        : BIO_new_mem_buf(reqBody.data(), static_cast<int>(reqBody.size()));
    BIO* respBio = OSSL_HTTP_transfer(
        nullptr,
        host.c_str(),
        port.c_str(),
        path,
        /*use_ssl=*/0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        /*buf_size=*/0,
        /*headers=*/nullptr,
        reqBio ? contentType : nullptr,
        reqBio,
        /*expected_content_type=*/nullptr,
        /*expect_asn1=*/0,
        kMaxHttpResponseBytes,
        timeoutSec,
        /*keep_alive=*/0
    );
    BIO_free(reqBio);
    if (!respBio) {
        return std::nullopt;
    }

    std::string out;
    char buf[8192];
    for (;;) {
        const int n = BIO_read(respBio, buf, sizeof(buf));
        if (n <= 0) {
            if (BIO_should_retry(respBio)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }

    BIO_free(respBio);
    return out;
}

struct ActiveMicCapture {
    eevs::ClientId clientId = 0;
    eevs::NetId netId = 0;
    std::string language = "en";
    std::string optionsJson;
    bool isolate = false;
    size_t maxSamples16k = 16000 * 30;
    std::vector<float> samples16k;
};

enum class AsyncJobKind : uint8_t {
    SttTranscribe = 0,
    TtsGenerate   = 1,
    TtsSpeak      = 2,
    VdesGenerate  = 3,
    VdesSpeak     = 4
};

struct AsyncAiJob {
    AsyncJobKind kind = AsyncJobKind::SttTranscribe;
    uint64_t jobId = 0;
    eevs::NetId netId = 0;
    eevs::IHostContext::AsyncControlRequestHandle requestHandle = 0;
    std::string text;
    std::string language = "en";
    std::string voice = "female";
    std::string instruct;
    std::optional<uint64_t> seed;
    float gain = 1.0f;
    std::string requestedClipId;
    std::string optionsJson;
    eevs::RoutingMode mode = eevs::RoutingMode::Spatial3D;
    bool speak = true;
    std::vector<float> samples16k;
};

class AudioCppClientPlugin final
    : public eevs::IPlugin
    , public eevs::IAudioTap
    , public eevs::IVoicePacketFilter
    , public eevs::ISessionListener {

public:
    [[nodiscard]] const char* GetName() const noexcept override { return "audio_cpp_client"; }
    [[nodiscard]] const char* GetVersion() const noexcept override { return "2.0.0"; }

    bool OnLoad(eevs::IHostContext* host) noexcept override {
        m_host = host;
        m_scope.Bind(host);
        m_aiHost = m_host->GetConfigProperty("eevs_audiocpp_host", m_host->GetConfigProperty("audiocpp_host", "127.0.0.1"));
        m_aiPort = m_host->GetConfigProperty("eevs_audiocpp_port", m_host->GetConfigProperty("audiocpp_port", "8080"));
        m_defaultTtsModel = m_host->GetConfigProperty("eevs_audiocpp_tts_model", m_host->GetConfigProperty("audiocpp_tts_model", ""));
        m_defaultVdesModel = m_host->GetConfigProperty("eevs_audiocpp_vdes_model", m_host->GetConfigProperty("audiocpp_vdes_model", ""));
        m_defaultSttModel = m_host->GetConfigProperty("eevs_audiocpp_stt_model", m_host->GetConfigProperty("audiocpp_stt_model", ""));

        m_workerThread = std::jthread([this](std::stop_token st) { WorkerLoop(st); });

        m_scope.AddPacketFilter(this);
        m_scope.AddAudioTap(this);
        m_scope.AddSessionListener(this);

        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleModelsAsyncHttp>("/stt/models", this);
        m_scope.AddEndpoint<&AudioCppClientPlugin::HandleSttStartHttp>("/stt/start", this);
        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleSttStopAsyncHttp>("/stt/stop", this);
        m_scope.AddEndpoint<&AudioCppClientPlugin::HandleSttResultHttp>("/stt/result", this);

        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleModelsAsyncHttp>("/tts/models", this);
        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleTtsGenerateAsyncHttp>("/tts/generate", this);
        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleTtsSpeakAsyncHttp>("/tts/speak", this);

        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleModelsAsyncHttp>("/vdes/models", this);
        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleVdesGenerateAsyncHttp>("/vdes/generate", this);
        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleVdesSpeakAsyncHttp>("/vdes/speak", this);

        m_scope.AddAsyncEndpoint<&AudioCppClientPlugin::HandleModelsAsyncHttp>("/audiocpp/info", this);

        std::string msg = std::format(
            "Loaded audio.cpp client plugin (audiocpp_server=http://{}:{}, endpoints: /stt/*, /tts/*, /vdes/*)",
            m_aiHost,
            m_aiPort
        );
        m_host->Log(eevs::LogLevel::Info, GetName(), msg.c_str());
        return true;
    }

    void OnUnload() noexcept override {
        m_workerThread = std::jthread{};
        m_scope.Reset();
        m_host = nullptr;

        m_activeCaptures.clear();
        std::scoped_lock lock(m_resultsMutex);
        m_resultsByNetId.clear();
    }

    [[nodiscard]] eevs::PacketFilterVerdict InspectIngressPacket(
        const eevs::VoiceIngressPacketView& packet
    ) noexcept override {
        auto it = m_activeCaptures.find(packet.senderClientId);
        if (it != m_activeCaptures.end() && it->second.isolate) {
            return eevs::PacketFilterVerdict::TapOnlyNoForward;
        }

        return eevs::PacketFilterVerdict::Allow;
    }

    [[nodiscard]] bool WantsDecodedPcm(eevs::ClientId speakerClientId) const noexcept override {
        return m_activeCaptures.contains(speakerClientId);
    }

    [[nodiscard]] eevs::TapStage GetPcmTapStage() const noexcept override {
        return eevs::TapStage::PreDsp;
    }

    void OnPcmFrame(
        const eevs::SpeakerContext& speaker,
        eevs::PcmFrameView pcm
    ) noexcept override {
        auto it = m_activeCaptures.find(speaker.clientId);
        if (it == m_activeCaptures.end() || !pcm.samples || pcm.sampleCount == 0 || pcm.sampleRate == 0) {
            return;
        }

        ActiveMicCapture& cap = it->second;
        eevs::plugins::common::Downsample48kTo16kAppend(
            pcm.samples,
            pcm.sampleCount,
            pcm.sampleRate,
            cap.samples16k,
            cap.maxSamples16k
        );
    }

    void OnClientUnregistered(eevs::ClientId clientId, eevs::NetId netId) noexcept override {
        m_activeCaptures.erase(clientId);
        std::scoped_lock lock(m_resultsMutex);
        m_resultsByNetId.erase(netId);
    }

private:
    void CompleteWithError(eevs::IHostContext::AsyncControlRequestHandle handle, uint16_t status, std::string_view msg) {
        if (!m_host || handle == 0) {
            return;
        }

        std::string errJson;
        (void)glz::write_json(ErrorRespDto{false, msg}, errJson);
        m_host->GetControlApi().CompleteAsyncRequest(handle, status, errJson.c_str(), errJson.size());
    }

    std::string UnreachableWorkerMessage() const {
        return std::format(
            "audiocpp_server unreachable at http://{}:{} (start audiocpp_server --ui --ui-management)",
            m_aiHost,
            m_aiPort
        );
    }

    static std::string ExtractErrorFromResponse(const AudioCppTaskResultDto& dto, std::string_view rawBody) {
        if (dto.error && !dto.error->message.empty()) {
            return dto.error->message;
        }
        return rawBody.empty() ? "audiocpp_server request failed" : std::string(rawBody);
    }

    // Discovers packages downloaded via audiocpp_server's WebUI (`models_root`/.audiocpp-package-*.json)
    // and registers them with `POST /v1/models/load` if `/v1/models` is currently empty.
    void AutoRegisterInstalledPackagesIfEmpty() {
        auto rootResp = HttpTransfer(m_aiHost, m_aiPort, "/v1/ui/models-root", {}, "application/json", 2);
        if (!rootResp || rootResp->empty()) {
            return;
        }

        AudioCppModelsRootDto rootDto{};
        if (glz::read<kRelaxedJsonOpts>(rootDto, *rootResp)) {
            return;
        }

        std::string rootDir = !rootDto.models_root.empty() ? rootDto.models_root : rootDto.default_models_root;
        if (rootDir.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::path rootPath(rootDir);
        if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec)) {
            return;
        }

        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(rootPath, ec)) {
            if (!dirEntry.is_regular_file(ec)) {
                continue;
            }
            const std::string fname = dirEntry.path().filename().string();
            if (!fname.starts_with(".audiocpp-package-") || !fname.ends_with(".json")) {
                continue;
            }

            std::ifstream f(dirEntry.path());
            if (!f.is_open()) {
                continue;
            }
            std::string jsonText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            AudioCppPackageManifestDto pkg{};
            if (glz::read<kRelaxedJsonOpts>(pkg, jsonText) || pkg.package_id.empty()) {
                continue;
            }

            std::string initialTask = "tts";
            if (pkg.package_id.find("voicedesign") != std::string::npos ||
                pkg.package_id.find("vdesign") != std::string::npos ||
                pkg.package_id.find("voicegen") != std::string::npos) {
                initialTask = "vdes";
            }

            // Try registering as initialTask first; if the model requires an explicit ASR session (like Canary), retry with task="asr"
            std::string loadReqTts = std::format(
                R"({{"id":"{}","package_id":"{}","task":"{}","mode":"offline"}})",
                pkg.package_id,
                pkg.package_id,
                initialTask
            );
            auto loadResp = HttpTransfer(m_aiHost, m_aiPort, "/v1/models/load", loadReqTts, "application/json", 5);
            if (loadResp && loadResp->find("\"error\"") != std::string::npos) {
                std::string loadReqAsr = std::format(
                    R"({{"id":"{}","package_id":"{}","task":"asr","mode":"offline"}})",
                    pkg.package_id,
                    pkg.package_id
                );
                (void)HttpTransfer(m_aiHost, m_aiPort, "/v1/models/load", loadReqAsr, "application/json", 5);
            }
        }
    }

    AudioCppModelsListDto FetchAvailableModels(bool includeParams = false) {
        const char* path = includeParams ? "/v1/models?include_params=true" : "/v1/models";
        AudioCppModelsListDto listDto{};
        if (auto respBody = HttpTransfer(m_aiHost, m_aiPort, path, {}, "application/json", 3)) {
            if (!glz::read<kRelaxedJsonOpts>(listDto, *respBody) && listDto.data.empty()) {
                AutoRegisterInstalledPackagesIfEmpty();
                if (auto retryResp = HttpTransfer(m_aiHost, m_aiPort, path, {}, "application/json", 3)) {
                    (void)glz::read<kRelaxedJsonOpts>(listDto, *retryResp);
                }
            }
        }
        return listDto;
    }

    std::vector<std::string> FetchLibraryVoices() {
        AudioCppVoicesListDto voicesDto{};
        if (auto respBody = HttpTransfer(m_aiHost, m_aiPort, "/v1/audio/voices", {}, "application/json", 2)) {
            (void)glz::read<kRelaxedJsonOpts>(voicesDto, *respBody);
        }
        return voicesDto.voices;
    }

    // Filters caller-supplied options JSON so only keys declared in the target model's `params`
    // are forwarded to audiocpp_server (stripping routing keys like `model`, `voice`, etc.).
    static glz::raw_json FilterOptionsForModel(
        std::string_view rawOptions,
        const AudioCppModelItemDto* modelMeta,
        std::string_view explicitInstruct = {},
        std::optional<uint64_t> explicitSeed = std::nullopt,
        std::string_view explicitReferenceText = {}
    ) {
        while (!rawOptions.empty() && (rawOptions.front() == ' ' || rawOptions.front() == '\t' || rawOptions.front() == '\r' || rawOptions.front() == '\n')) {
            rawOptions.remove_prefix(1);
        }
        while (!rawOptions.empty() && (rawOptions.back() == ' ' || rawOptions.back() == '\t' || rawOptions.back() == '\r' || rawOptions.back() == '\n')) {
            rawOptions.remove_suffix(1);
        }

        std::map<std::string, glz::raw_json> kvMap;
        if (rawOptions.size() >= 2 && rawOptions.front() == '{' && rawOptions.back() == '}') {
            (void)glz::read<kRelaxedJsonOpts>(kvMap, rawOptions);
        }

        // Always strip EEVS / wrapper routing keys that are not audiocpp model request options
        kvMap.erase("model");
        kvMap.erase("voice");
        kvMap.erase("gain");

        if (!explicitInstruct.empty()) {
            std::string quotedInstruct;
            (void)glz::write_json(explicitInstruct, quotedInstruct);
            kvMap["instruct"] = glz::raw_json{std::move(quotedInstruct)};
        }
        if (explicitSeed.has_value()) {
            kvMap["seed"] = glz::raw_json{std::to_string(*explicitSeed)};
        }
        if (!explicitReferenceText.empty()) {
            std::string quotedRefText;
            (void)glz::write_json(explicitReferenceText, quotedRefText);
            kvMap["reference_text"] = glz::raw_json{std::move(quotedRefText)};
        }

        if (kvMap.empty()) {
            return glz::raw_json{"{}"};
        }

        if (modelMeta && !modelMeta->params.str.empty()) {
            std::vector<AudioCppParamDescriptorDto> allowedParams;
            if (!glz::read<kRelaxedJsonOpts>(allowedParams, modelMeta->params.str)) {
                for (auto it = kvMap.begin(); it != kvMap.end();) {
                    const bool allowed = std::ranges::any_of(allowedParams, [&](const AudioCppParamDescriptorDto& p) {
                        return p.name == it->first;
                    });
                    if (!allowed) {
                        it = kvMap.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        if (kvMap.empty()) {
            return glz::raw_json{"{}"};
        }

        std::string filteredJson;
        (void)glz::write_json(kvMap, filteredJson);
        return glz::raw_json{ filteredJson.empty() ? "{}" : std::move(filteredJson) };
    }

    static bool IsVoiceDesignCapableModel(const AudioCppModelItemDto& m) {
        if (m.id.empty()) {
            return false;
        }
        if (m.task == "vdes" || m.task == "design") {
            return true;
        }
        if (m.id.find("voicedesign") != std::string::npos ||
            m.id.find("vdesign") != std::string::npos ||
            m.id.find("voicegen") != std::string::npos) {
            return true;
        }
        if (m.task != "tts") {
            return false;
        }
        // Only treat TTS models with an `instruct` parameter (excluding base voice-clone checkpoints like `qwen3_tts_0_6b_base`) as Voice Design capable
        if (m.id.find("_base") != std::string::npos) {
            return false;
        }
        if (!m.params.str.empty()) {
            std::vector<AudioCppParamDescriptorDto> allowedParams;
            if (!glz::read<kRelaxedJsonOpts>(allowedParams, m.params.str)) {
                return std::ranges::any_of(allowedParams, [](const AudioCppParamDescriptorDto& p) {
                    return p.name == "instruct";
                });
            }
        }
        return m.family == "breeze_tts" || m.family == "irodori_tts" || m.family == "omnivoice" || m.family == "ming3_tts";
    }

    std::string ResolveModelForTask(std::string_view optionsJson, std::string_view wantedTask) {
        if (!optionsJson.empty()) {
            RequestOptionsModelDto optModel{};
            if (!glz::read<kRelaxedJsonOpts>(optModel, optionsJson) && !optModel.model.empty()) {
                return optModel.model;
            }
        }

        if (wantedTask == "tts" && !m_defaultTtsModel.empty()) {
            return m_defaultTtsModel;
        }
        if (wantedTask == "vdes" && !m_defaultVdesModel.empty()) {
            return m_defaultVdesModel;
        }
        if (wantedTask == "asr" && !m_defaultSttModel.empty()) {
            return m_defaultSttModel;
        }

        AudioCppModelsListDto modelsList = FetchAvailableModels(true);
        if (modelsList.data.empty()) {
            return {};
        }

        // For Voice Design (`vdes`), prefer dedicated VoiceDesign checkpoints (`voicedesign`/`vdesign`/`task == "vdes"`) first,
        // then any other Voice Design capable model (e.g., BreezeTTS 2).
        if (wantedTask == "vdes") {
            for (const auto& m : modelsList.data) {
                if (IsVoiceDesignCapableModel(m) &&
                    (m.task == "vdes" || m.id.find("voicedesign") != std::string::npos || m.id.find("vdesign") != std::string::npos)) {
                    return m.id;
                }
            }
            for (const auto& m : modelsList.data) {
                if (IsVoiceDesignCapableModel(m)) {
                    return m.id;
                }
            }
        }

        for (const auto& m : modelsList.data) {
            if (m.task == wantedTask && !m.id.empty()) {
                return m.id;
            }
        }

        return modelsList.data.front().id;
    }

    static void NormalizeAndApplyGainPcm(std::vector<float>& pcm, float userGain) noexcept {
        if (pcm.empty()) {
            return;
        }

        float peak = 0.0f;
        for (float s : pcm) {
            peak = std::max(peak, std::abs(s));
        }

        // Normalize quiet or hot voice seeds toward a consistent 0.85 (-1.4 dBFS) reference peak
        // (capped at 4.0x auto-boost so background noise floor isn't over-amplified)
        float autoNormScale = 1.0f;
        if (peak > 0.01f) {
            constexpr float kTargetPeak = 0.85f;
            autoNormScale = std::clamp(kTargetPeak / peak, 0.25f, 4.0f);
        }

        const float clampedUserGain = std::clamp(userGain, 0.05f, 4.0f);
        const float totalScale = autoNormScale * clampedUserGain;
        if (std::abs(totalScale - 1.0f) < 1e-3f && peak <= 0.95f) {
            return;
        }

        for (float& s : pcm) {
            float v = s * totalScale;
            // Soft-knee saturation above 0.85f so loud gain settings (>1.0x) never hard-clip
            const float absV = std::abs(v);
            if (absV > 0.85f) {
                const float sign = (v >= 0.0f) ? 1.0f : -1.0f;
                v = sign * (0.85f + 0.14f * std::tanh((absV - 0.85f) / 0.14f));
            }
            s = std::clamp(v, -0.99f, 0.99f);
        }
    }

    bool StoreWavBytesAsClip(
        std::string_view wavBytes,
        std::string_view requestedClipId,
        float gain,
        std::string& outClipId,
        uint32_t& outDurationMs,
        std::string& outError
    ) {
        std::vector<float> pcmFloat;
        uint32_t sampleRate = 24000;
        if (!eevs::plugins::common::ExtractFloatPcmFromWavMemory(
                wavBytes.data(),
                wavBytes.size(),
                24000,
                pcmFloat,
                sampleRate,
                &outError
            ) || pcmFloat.empty()) {
            if (outError.empty()) {
                outError = "Failed to decode WAV audio returned by audiocpp_server";
            }

            return false;
        }

        NormalizeAndApplyGainPcm(pcmFloat, gain);

        char clipIdBuf[128]{};

        if (!m_host->GetClips().StorePcmClip(
                std::string(requestedClipId).c_str(),
                pcmFloat.data(),
                pcmFloat.size(),
                sampleRate,
                clipIdBuf,
                sizeof(clipIdBuf),
                &outDurationMs
            )) {
            outError = "Failed to store synthesized PCM clip in ClipManager";
            return false;
        }

        outClipId = clipIdBuf;
        return true;
    }

    void ProcessSttJob(const AsyncAiJob& job) {
        const auto durationMs = static_cast<uint32_t>((job.samples16k.size() * 1000ull) / 16000ull);
        const float rmsDb = ComputeRmsDb(job.samples16k);
        if (job.samples16k.size() < 800 || rmsDb < -62.0f) {
            SttCompletedDto emptyRes{
                .ok = false,
                .jobId = job.jobId,
                .netId = job.netId,
                .durationMs = durationMs,
                .confidence = 0.0f,
                .rmsDb = rmsDb,
                .backend = "audio.cpp",
                .text = "",
                .error = "No speech detected in captured audio (Hold Push-to-Talk [N] while speaking)"
            };

            std::string jsonOut;
            (void)glz::write_json(emptyRes, jsonOut);
            m_host->GetEvents().Publish("stt.completed", job.netId, jsonOut.c_str());
            if (job.requestHandle != 0) {
                m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 200, jsonOut.c_str(), jsonOut.size());
            }

            return;
        }

        AudioCppModelsListDto allModels = FetchAvailableModels(true);
        std::string modelId = ResolveModelForTask(job.optionsJson, "asr");
        if (modelId.empty()) {
            CompleteWithError(
                job.requestHandle,
                404,
                "No ASR/STT model configured in audiocpp_server (load an ASR model in audio.cpp WebUI or config)"
            );
            return;
        }

        const AudioCppModelItemDto* modelMeta = nullptr;
        for (const auto& m : allModels.data) {
            if (m.id == modelId) {
                modelMeta = &m;
                break;
            }
        }

        std::vector<uint8_t> wavBytes = eevs::plugins::common::EncodeMonoFloatToWavMemory(job.samples16k, 16000);
        AudioCppTaskRunReqDto reqDto{
            .model = modelId,
            .request = {
                .text = std::nullopt,
                .language = (job.language.empty() || job.language == "auto")
                    ? std::nullopt
                    : std::optional<std::string>(job.language),
                .audio_base64 = glz::write_base64(
                    std::string_view(reinterpret_cast<const char*>(wavBytes.data()), wavBytes.size())
                ),
                .voice = std::nullopt,
                .options = FilterOptionsForModel(job.optionsJson, modelMeta)
            }
        };

        std::string reqJson;
        (void)glz::write_json(reqDto, reqJson);
        auto respBody = HttpTransfer(m_aiHost, m_aiPort, "/v1/tasks/run", reqJson);
        if (!respBody) {
            CompleteWithError(job.requestHandle, 502, UnreachableWorkerMessage());
            return;
        }

        AudioCppTaskResultDto resDto{};
        if (glz::read<kRelaxedJsonOpts>(resDto, *respBody) || resDto.error.has_value()) {
            CompleteWithError(job.requestHandle, 500, ExtractErrorFromResponse(resDto, *respBody));
            return;
        }

        SttCompletedDto doneDto{
            .ok = true,
            .jobId = job.jobId,
            .netId = job.netId,
            .durationMs = durationMs,
            .confidence = 0.95f,
            .rmsDb = rmsDb,
            .backend = std::format("audio.cpp ({})", modelId),
            .text = std::move(resDto.text),
            .error = ""
        };

        {
            std::scoped_lock lock(m_resultsMutex);
            m_resultsByNetId[job.netId] = doneDto;
        }

        std::string jsonOut;
        (void)glz::write_json(doneDto, jsonOut);
        m_host->GetEvents().Publish("stt.completed", job.netId, jsonOut.c_str());
        if (job.requestHandle != 0) {
            m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 200, jsonOut.c_str(), jsonOut.size());
        }
    }

    void ProcessTtsJob(const AsyncAiJob& job) {
        const bool isVoiceDesign = (job.kind == AsyncJobKind::VdesGenerate || job.kind == AsyncJobKind::VdesSpeak);
        const std::string_view wantedTask = isVoiceDesign ? "vdes" : "tts";

        AudioCppModelsListDto allModels = FetchAvailableModels(true);
        std::vector<std::string> candidateModels;
        std::string primaryModel = ResolveModelForTask(job.optionsJson, wantedTask);
        if (!primaryModel.empty()) {
            candidateModels.push_back(primaryModel);
        }

        // If no explicit model was forced in optionsJson, add other registered models of the same task as fallbacks
        RequestOptionsModelDto explicitOpt{};
        const bool hasExplicitModel = (!job.optionsJson.empty() &&
                                       !glz::read<kRelaxedJsonOpts>(explicitOpt, job.optionsJson) &&
                                       !explicitOpt.model.empty());
        if (!hasExplicitModel) {
            for (const auto& m : allModels.data) {
                const bool matchesTask = isVoiceDesign
                    ? IsVoiceDesignCapableModel(m)
                    : (m.task == wantedTask && !m.id.empty());
                if (matchesTask && std::ranges::find(candidateModels, m.id) == candidateModels.end()) {
                    candidateModels.push_back(m.id);
                }
            }
        }

        if (candidateModels.empty()) {
            CompleteWithError(
                job.requestHandle,
                404,
                isVoiceDesign
                    ? "No Voice Design (vdes) model installed in audiocpp_server (install a Voice Design model in /audio_cpp WebUI)"
                    : "No TTS model installed in audiocpp_server (install a TTS model in /audio_cpp WebUI)"
            );
            return;
        }

        std::vector<std::string> libraryVoices = FetchLibraryVoices();

        std::string decodedWav;
        std::string backend;
        std::string lastError = UnreachableWorkerMessage();

        for (const std::string& modelId : candidateModels) {
            const AudioCppModelItemDto* modelMeta = nullptr;
            for (const auto& m : allModels.data) {
                if (m.id == modelId) {
                    modelMeta = &m;
                    break;
                }
            }

            std::optional<std::string> voiceOpt;
            const bool isPureVoiceDesignJob = isVoiceDesign || (modelMeta && IsVoiceDesignCapableModel(*modelMeta) && !job.instruct.empty());
            if (!isPureVoiceDesignJob && !job.voice.empty() && job.voice != "default" && job.voice != "female" && job.voice != "male") {
                voiceOpt = job.voice;
            } else if (!isPureVoiceDesignJob && modelMeta && (modelMeta->family == "qwen3_tts" || modelMeta->family == "chatterbox" || modelMeta->family == "breeze_tts") && !libraryVoices.empty()) {
                // Clone-based TTS models (like Qwen3 base TTS) require a reference voice from /v1/audio/voices
                std::string preferredSubstr = (job.voice == "male") ? "man" : "woman";
                for (const std::string& v : libraryVoices) {
                    if (v.find(preferredSubstr) != std::string::npos) {
                        voiceOpt = v;
                        break;
                    }
                }
                if (!voiceOpt) {
                    voiceOpt = libraryVoices.front();
                }
            }

            // Voice Identity Anchor Cache for Voice Design + Seed:
            // Generative Voice Design models sample codec tokens autoregressively on `input`, so different dialogue lines
            // with the same `seed` + `instruct` can drift in pitch/timbre unless anchored to a reference audio prompt.
            // For models that support acoustic reference conditioning, cache the first generated utterance for
            // `(modelId, seed, instruct)` via `/v1/ui/upload` and pass `voice_ref` + `reference_text` on subsequent turns.
            std::optional<std::string> anchorVoiceRef;
            std::string anchorRefText;
            std::string anchorKey;
            const bool supportsVoiceRefAnchor = isPureVoiceDesignJob &&
                job.seed.has_value() &&
                !job.instruct.empty() &&
                modelMeta &&
                (modelMeta->family == "breeze_tts" || modelMeta->family == "irodori_tts" || modelMeta->family == "omnivoice" || modelMeta->family == "ming3_tts");

            if (supportsVoiceRefAnchor) {
                anchorKey = std::format("{}|{}|{}", modelId, *job.seed, job.instruct);
                auto cacheIt = m_voiceDesignAnchors.find(anchorKey);
                if (cacheIt != m_voiceDesignAnchors.end() && std::filesystem::exists(cacheIt->second.voiceRefPath)) {
                    anchorVoiceRef = cacheIt->second.voiceRefPath;
                    anchorRefText = cacheIt->second.referenceText;
                }
            }

            AudioCppSpeechReqDto reqDto{
                .model = modelId,
                .input = job.text,
                .voice = anchorVoiceRef ? std::nullopt : voiceOpt,
                .voice_ref = anchorVoiceRef,
                .reference_text = !anchorRefText.empty() ? std::optional<std::string>(anchorRefText) : std::nullopt,
                .instructions = !job.instruct.empty() ? std::optional<std::string>(job.instruct) : std::nullopt,
                .seed = job.seed,
                .options = FilterOptionsForModel(job.optionsJson, modelMeta, job.instruct, job.seed, anchorRefText)
            };

            std::string reqJson;
            (void)glz::write_json(reqDto, reqJson);
            auto respBody = HttpTransfer(m_aiHost, m_aiPort, "/v1/audio/speech", reqJson);
            if (!respBody) {
                continue;
            }

            if (respBody->size() >= 4 && std::string_view(*respBody).substr(0, 4) == "RIFF") {
                decodedWav = std::move(*respBody);
                backend = std::format("audio.cpp ({})", modelId);
                if (supportsVoiceRefAnchor && !anchorVoiceRef.has_value() && !anchorKey.empty()) {
                    if (auto upResp = HttpTransfer(m_aiHost, m_aiPort, "/v1/ui/upload", decodedWav, "audio/wav", 5)) {
                        AudioCppUploadRespDto upDto{};
                        if (!glz::read<kRelaxedJsonOpts>(upDto, *upResp) && !upDto.path.empty()) {
                            m_voiceDesignAnchors[anchorKey] = VoiceDesignAnchor{
                                .voiceRefPath = std::move(upDto.path),
                                .referenceText = job.text
                            };
                        }
                    }
                }
                break;
            }

            AudioCppTaskResultDto resDto{};
            if (!glz::read<kRelaxedJsonOpts>(resDto, *respBody) && !resDto.error && !resDto.audio.empty()) {
                decodedWav = glz::read_base64(resDto.audio);
                backend = std::format("audio.cpp ({})", modelId);
                break;
            }

            lastError = ExtractErrorFromResponse(resDto, *respBody);
        }

        if (decodedWav.empty()) {
            CompleteWithError(job.requestHandle, 500, lastError);
            return;
        }

        std::string clipId;
        uint32_t durationMs = 0;
        std::string wavErr;
        if (!StoreWavBytesAsClip(decodedWav, job.requestedClipId, job.gain, clipId, durationMs, wavErr)) {
            CompleteWithError(job.requestHandle, 500, wavErr);
            return;
        }

        const char* eventTopic = isVoiceDesign ? "vdes.clip_ready" : "tts.clip_ready";
        if (job.kind == AsyncJobKind::TtsGenerate || job.kind == AsyncJobKind::VdesGenerate || !job.speak) {
            TtsClipRespDto respDto{true, clipId, backend, durationMs};

            std::string jsonOut;
            (void)glz::write_json(respDto, jsonOut);
            m_host->GetEvents().Publish(eventTopic, 0, jsonOut.c_str());
            m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 200, jsonOut.c_str(), jsonOut.size());
            return;
        }

        const uint64_t playbackId = m_host->GetClips().PlayClip(clipId.c_str(), job.netId, job.mode);
        TtsSpeakRespDto respDto{true, clipId, backend, playbackId, durationMs};

        std::string jsonOut;
        (void)glz::write_json(respDto, jsonOut);
        m_host->GetEvents().Publish(eventTopic, job.netId, jsonOut.c_str());
        m_host->GetControlApi().CompleteAsyncRequest(job.requestHandle, 200, jsonOut.c_str(), jsonOut.size());
    }

    void WorkerLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            AsyncAiJob job{};

            if (!m_jobQueue.TryPop(job)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (!m_host) {
                continue;
            }

            switch (job.kind) {
                case AsyncJobKind::SttTranscribe:
                    ProcessSttJob(job);
                    break;
                case AsyncJobKind::TtsGenerate:
                case AsyncJobKind::TtsSpeak:
                case AsyncJobKind::VdesGenerate:
                case AsyncJobKind::VdesSpeak:
                    ProcessTtsJob(job);
                    break;
            }
        }
    }

    bool HandleModelsAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view path,
        std::string_view /*body*/
    ) {
        std::string_view reqPath(!path.empty() ? path : "/tts/models");
        const bool wantStt = (reqPath.find("/stt/") != std::string_view::npos);
        const bool wantVdes = (reqPath.find("/vdes/") != std::string_view::npos);
        const std::string serverUrl = std::format("http://{}:{}", m_aiHost, m_aiPort);

        AudioCppModelsListDto listDto = FetchAvailableModels(true);
        if (!listDto.data.empty() || HttpTransfer(m_aiHost, m_aiPort, "/health", {}, "application/json", 2).has_value()) {
            ModelsEndpointRespDto outDto{
                .ok = true,
                .backend = "audio.cpp",
                .url = serverUrl,
                .models = {},
                .voices = wantStt ? std::vector<std::string>{} : FetchLibraryVoices(),
                .modelDetails = {}
            };

            // If Voice Design (`/vdes/models`), put dedicated VoiceDesign checkpoints (`voicedesign`/`vdesign`/`task == "vdes"`) first
            if (wantVdes) {
                for (const auto& m : listDto.data) {
                    if (IsVoiceDesignCapableModel(m) &&
                        (m.task == "vdes" || m.id.find("voicedesign") != std::string::npos || m.id.find("vdesign") != std::string::npos)) {
                        outDto.models.push_back(m.id);
                        outDto.modelDetails.push_back(m);
                    }
                }
            }

            for (const auto& m : listDto.data) {
                if (m.id.empty() || std::ranges::find(outDto.models, m.id) != outDto.models.end()) {
                    continue;
                }
                const bool matches = wantStt
                    ? (m.task == "asr" || m.task == "stt" || m.task == "s2s")
                    : wantVdes
                        ? IsVoiceDesignCapableModel(m)
                        : ((m.task == "tts" || m.task == "s2s") && m.id.find("voicedesign") == std::string::npos && m.id.find("vdesign") == std::string::npos);
                if (matches) {
                    outDto.models.push_back(m.id);
                    outDto.modelDetails.push_back(m);
                }
            }

            std::string jsonOut;
            (void)glz::write_json(outDto, jsonOut);
            m_host->GetControlApi().CompleteAsyncRequest(requestHandle, 200, jsonOut);
            return true;
        }

        std::string jsonOut;
        (void)glz::write_json(
            ModelsEndpointRespDto{
                .ok = true,
                .backend = "audio.cpp [offline]",
                .url = serverUrl,
                .models = {},
                .voices = {},
                .modelDetails = {}
            },
            jsonOut
        );
        m_host->GetControlApi().CompleteAsyncRequest(requestHandle, 200, jsonOut);
        return true;
    }

    bool HandleSttStartHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        SttStartReqDto req{};

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

        ActiveMicCapture cap{
            .clientId = clientId,
            .netId = netId,
            .language = req.language.empty() ? "en" : std::move(req.language),
            .optionsJson = std::move(req.options.str),
            .isolate = req.isolate,
            .maxSamples16k = 16000 * 30,
            .samples16k = {}
        };

        cap.samples16k.reserve(16000 * 5);
        m_activeCaptures[clientId] = std::move(cap);
        outStatusCode = 200;
        WriteJsonToResponseBuffer(SttStartRespDto{true, netId, "audio.cpp", true}, responseBuf, responseCap);
        return true;
    }

    bool HandleSttStopAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        SttStartReqDto req{};

        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || !req.netId) {
            CompleteWithError(requestHandle, 400, "Missing or invalid netId");
            return true;
        }

        const auto netId = *req.netId;
        auto clientIdOpt = m_host->GetSessions().ResolveClientId(netId);
        if (!clientIdOpt) {
            CompleteWithError(requestHandle, 404, "Unknown player netId");
            return true;
        }

        auto it = m_activeCaptures.find(*clientIdOpt);
        if (it == m_activeCaptures.end()) {
            CompleteWithError(requestHandle, 404, "No active STT capture for netId");
            return true;
        }

        ActiveMicCapture cap = std::move(it->second);
        m_activeCaptures.erase(it);
        AsyncAiJob job{
            .kind = AsyncJobKind::SttTranscribe,
            .jobId = m_nextJobId++,
            .netId = netId,
            .requestHandle = requestHandle,
            .language = std::move(cap.language),
            .optionsJson = !req.options.str.empty() ? std::move(req.options.str) : std::move(cap.optionsJson),
            .samples16k = std::move(cap.samples16k)
        };

        if (!m_jobQueue.TryPush(std::move(job))) {
            CompleteWithError(requestHandle, 503, "AI worker queue is full");
        }

        return true;
    }

    bool HandleSttResultHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        SttStartReqDto req{};

        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || !req.netId) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Missing netId"}, responseBuf, responseCap);
            return true;
        }

        std::scoped_lock lock(m_resultsMutex);
        auto it = m_resultsByNetId.find(*req.netId);
        if (it == m_resultsByNetId.end()) {
            outStatusCode = 404;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "No STT result found for netId"}, responseBuf, responseCap);
            return true;
        }

        outStatusCode = 200;
        WriteJsonToResponseBuffer(it->second, responseBuf, responseCap);
        return true;
    }

    bool HandleTtsGenerateAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        return EnqueueTtsRequest(requestHandle, reqBody, false, false);
    }

    bool HandleTtsSpeakAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        return EnqueueTtsRequest(requestHandle, reqBody, true, false);
    }

    bool HandleVdesGenerateAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        return EnqueueTtsRequest(requestHandle, reqBody, false, true);
    }

    bool HandleVdesSpeakAsyncHttp(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody
    ) {
        return EnqueueTtsRequest(requestHandle, reqBody, true, true);
    }

    bool EnqueueTtsRequest(
        eevs::IHostContext::AsyncControlRequestHandle requestHandle,
        std::string_view reqBody,
        bool speak,
        bool isVoiceDesign
    ) {
        TtsReqDto req{};

        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || req.text.empty()) {
            CompleteWithError(requestHandle, 400, "Missing or invalid text");
            return true;
        }

        if (speak && !req.senderNetId) {
            CompleteWithError(requestHandle, 400, "Missing senderNetId");
            return true;
        }

        const AsyncJobKind jobKind = isVoiceDesign
            ? (speak ? AsyncJobKind::VdesSpeak : AsyncJobKind::VdesGenerate)
            : (speak ? AsyncJobKind::TtsSpeak : AsyncJobKind::TtsGenerate);

        AsyncAiJob job{
            .kind = jobKind,
            .jobId = m_nextJobId++,
            .netId = req.senderNetId.value_or(0),
            .requestHandle = requestHandle,
            .text = std::move(req.text),
            .voice = req.voice.empty() ? "default" : std::move(req.voice),
            .instruct = std::move(req.instruct),
            .seed = req.seed,
            .gain = req.gain.value_or(1.0f),
            .requestedClipId = std::move(req.clipId),
            .optionsJson = std::move(req.options.str),
            .mode = ParseModeString(req.mode),
            .speak = speak
        };

        if (!m_jobQueue.TryPush(std::move(job))) {
            CompleteWithError(requestHandle, 503, "AI worker queue is full");
        }

        return true;
    }

    struct VoiceDesignAnchor {
        std::string voiceRefPath;
        std::string referenceText;
    };

    eevs::IHostContext* m_host = nullptr;
    eevs::ScopedPluginRegistrations m_scope;
    std::string m_aiHost = "127.0.0.1";
    std::string m_aiPort = "8080";
    std::string m_defaultTtsModel;
    std::string m_defaultVdesModel;
    std::string m_defaultSttModel;
    eevs::FlatMap<std::string, VoiceDesignAnchor> m_voiceDesignAnchors;
    eevs::FlatMap<eevs::ClientId, ActiveMicCapture> m_activeCaptures;
    std::mutex m_resultsMutex;
    eevs::FlatMap<eevs::NetId, SttCompletedDto> m_resultsByNetId;
    SpscRingBuffer<AsyncAiJob, 64> m_jobQueue;
    std::jthread m_workerThread;
    uint64_t m_nextJobId = 1;
};
} // namespace

EEVS_DECLARE_PLUGIN(AudioCppClientPlugin)
