/**
 * @file voice_effects_plugin.cpp
 * @brief Real-Time DSP Voice Effects & Acoustic Environment Plugin for EEVS
 *
 * Powered by `signalsmith-dsp` (BiquadStatic filters, interpolated Delay lines, Hadamard FDN Reverb)
 * and `signalsmith-stretch` (STFT phase-vocoder pitch & formant shifting).
 *
 * Registers two `IDspProcessor` stages in `AudioPipeline`:
 *   1. `DspStage::PerSpeakerIngress`: Per-speaker voice effects applied before routing.
 *   2. `DspStage::PerRouteBranch`: Route-specific transmission effects (e.g., applying a `2D` radio or phone filter
 *      while leaving `3D` proximity speech unmodified).
 *
 * Exposes Control API endpoints:
 *   - `GET  /effects/list`  -> Lists available presets and active player DSP configurations
 *   - `POST /effects/set`   -> Configures an effect preset or custom DSP parameters for a player (`netId` + `targetBranch`)
 *   - `POST /effects/clear` -> Clears effects for a player (`netId`, optional `targetBranch`) or all players
 */

#include "eevs/plugin.h"
#include "eevs/plugin_scope.h"
#include "containers.h"
#include "json_response.h"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <signalsmith-dsp/delay.h>
#include <signalsmith-dsp/filters.h>
#include <signalsmith-dsp/mix.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace eevs::plugins::voice_effects {

constexpr float kSampleRate = 48000.0f;
constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

struct EffectSetReqDto {
    std::optional<uint16_t> netId;
    std::string preset = "none";
    std::string targetBranch = "all"; // "all" (PerSpeakerIngress), "2D" (NonSpatial branch), "3D" (Spatial3D branch)
    std::optional<float> pitchSemitones;
    std::optional<float> tonalityLimit;
    std::optional<float> highpassHz;
    std::optional<float> lowpassHz;
    std::optional<float> midPeakHz;
    std::optional<float> midPeakDb;
    std::optional<float> drive;
    std::optional<float> noiseLevel;
    std::optional<float> dropoutRate;
    std::optional<int> crushDownsample;
    std::optional<float> ringModHz;
    std::optional<float> echoDelayMs;
    std::optional<float> echoFeedback;
    std::optional<float> reverbWet;
    std::optional<float> reverbDecay;
    std::optional<float> subOctaveMix;
    std::optional<float> wetMix;
    std::optional<float> outputGain;
};

struct EffectClearReqDto {
    std::optional<uint16_t> netId;
    std::string targetBranch; // "", "all", "2D", "3D", or "everything"
};

struct PresetDescriptorDto {
    std::string id;
    std::string label;
    std::string defaultBranch;
    std::string description;
};

struct ActiveEffectItemDto {
    uint16_t clientId = 0;
    uint16_t netId = 0;
    std::string targetBranch;
    std::string preset;
    float pitchSemitones = 0.0f;
    float highpassHz = 20.0f;
    float lowpassHz = 20000.0f;
    float drive = 0.0f;
    float noiseLevel = 0.0f;
    float dropoutRate = 0.0f;
    float reverbWet = 0.0f;
    float wetMix = 1.0f;
    float outputGain = 1.0f;
};

struct EffectsListRespDto {
    bool ok = true;
    std::vector<PresetDescriptorDto> presets;
    std::vector<ActiveEffectItemDto> active;
};

struct EffectAppliedRespDto {
    bool ok = true;
    uint16_t clientId = 0;
    uint16_t netId = 0;
    std::string preset;
    std::string targetBranch;
};

struct SimpleOkRespDto {
    bool ok = true;
    size_t cleared = 0;
};

struct EffectConfig {
    bool enabled = false;
    std::string preset = "none";
    std::string targetBranch = "all";
    float pitchSemitones = 0.0f;
    float tonalityLimit = 0.0f;
    float highpassHz = 20.0f;
    float lowpassHz = 20000.0f;
    float midPeakHz = 1500.0f;
    float midPeakDb = 0.0f;
    float drive = 0.0f;
    float noiseLevel = 0.0f;
    float dropoutRate = 0.0f;
    int crushDownsample = 1;
    float ringModHz = 0.0f;
    float echoDelayMs = 0.0f;
    float echoFeedback = 0.0f;
    float reverbWet = 0.0f;
    float reverbDecay = 0.5f;
    float subOctaveMix = 0.0f;
    float wetMix = 1.0f;
    float outputGain = 1.0f;
};

} // namespace eevs::plugins::voice_effects

namespace {

using namespace eevs::plugins::voice_effects;
using namespace eevs::plugins::common;

// Applies preset defaults before overlaying any explicit user-supplied parameter overrides
EffectConfig BuildConfigFromRequest(const EffectSetReqDto& req) {
    EffectConfig cfg{};
    cfg.preset = req.preset.empty() ? "custom" : req.preset;
    cfg.targetBranch = req.targetBranch.empty() ? "all" : req.targetBranch;
    cfg.enabled = (cfg.preset != "none" && cfg.preset != "off");

    if (cfg.preset == "mask") {
        cfg.pitchSemitones = -3.5f;
        cfg.tonalityLimit = 0.14f;
        cfg.highpassHz = 140.0f;
        cfg.lowpassHz = 4400.0f;
        cfg.drive = 0.18f;
        cfg.outputGain = 1.05f;
    } else if (cfg.preset == "helium") {
        cfg.pitchSemitones = 5.5f;
        cfg.tonalityLimit = 0.25f;
        cfg.highpassHz = 220.0f;
        cfg.outputGain = 1.0f;
    } else if (cfg.preset == "demon") {
        cfg.pitchSemitones = -6.0f;
        cfg.tonalityLimit = 0.10f;
        cfg.subOctaveMix = 0.45f;
        cfg.highpassHz = 70.0f;
        cfg.lowpassHz = 3200.0f;
        cfg.drive = 0.35f;
        cfg.reverbWet = 0.28f;
        cfg.reverbDecay = 0.72f;
        cfg.outputGain = 1.15f;
    } else if (cfg.preset == "robot") {
        cfg.ringModHz = 55.0f;
        cfg.echoDelayMs = 11.5f;
        cfg.echoFeedback = 0.52f;
        cfg.highpassHz = 240.0f;
        cfg.lowpassHz = 5200.0f;
        cfg.drive = 0.22f;
        cfg.outputGain = 1.05f;
    } else if (cfg.preset == "megaphone") {
        cfg.highpassHz = 520.0f;
        cfg.lowpassHz = 3600.0f;
        cfg.midPeakHz = 1650.0f;
        cfg.midPeakDb = 8.5f;
        cfg.drive = 0.62f;
        cfg.echoDelayMs = 68.0f;
        cfg.echoFeedback = 0.26f;
        cfg.outputGain = 1.25f;
    } else if (cfg.preset == "radio") {
        if (req.targetBranch.empty()) {
            cfg.targetBranch = "2D";
        }
        cfg.highpassHz = 380.0f;
        cfg.lowpassHz = 3100.0f;
        cfg.midPeakHz = 1800.0f;
        cfg.midPeakDb = 4.5f;
        cfg.drive = 0.28f;
        cfg.noiseLevel = 0.0025f;
        cfg.outputGain = 1.08f;
    } else if (cfg.preset == "tunnel_phone") {
        if (req.targetBranch.empty()) {
            cfg.targetBranch = "2D";
        }
        cfg.highpassHz = 450.0f;
        cfg.lowpassHz = 2300.0f;
        cfg.midPeakHz = 1350.0f;
        cfg.midPeakDb = 3.5f;
        cfg.drive = 0.20f;
        cfg.noiseLevel = 0.0045f;
        cfg.dropoutRate = 0.35f;
        cfg.crushDownsample = 3;
        cfg.outputGain = 1.05f;
    } else if (cfg.preset == "underwater") {
        cfg.highpassHz = 60.0f;
        cfg.lowpassHz = 520.0f;
        cfg.midPeakHz = 320.0f;
        cfg.midPeakDb = 4.5f;
        cfg.reverbWet = 0.35f;
        cfg.reverbDecay = 0.65f;
        cfg.outputGain = 1.20f;
    } else if (cfg.preset == "cave_reverb") {
        cfg.highpassHz = 110.0f;
        cfg.lowpassHz = 9000.0f;
        cfg.echoDelayMs = 52.0f;
        cfg.echoFeedback = 0.22f;
        cfg.reverbWet = 0.45f;
        cfg.reverbDecay = 0.80f;
        cfg.outputGain = 1.05f;
    }

    // Apply explicit overrides
    if (req.pitchSemitones)  cfg.pitchSemitones  = std::clamp(*req.pitchSemitones, -12.0f, 12.0f);
    if (req.tonalityLimit)   cfg.tonalityLimit   = std::clamp(*req.tonalityLimit, 0.0f, 0.5f);
    if (req.highpassHz)      cfg.highpassHz      = std::clamp(*req.highpassHz, 20.0f, 4000.0f);
    if (req.lowpassHz)       cfg.lowpassHz       = std::clamp(*req.lowpassHz, 200.0f, 20000.0f);
    if (req.midPeakHz)       cfg.midPeakHz       = std::clamp(*req.midPeakHz, 200.0f, 8000.0f);
    if (req.midPeakDb)       cfg.midPeakDb       = std::clamp(*req.midPeakDb, -18.0f, 18.0f);
    if (req.drive)           cfg.drive           = std::clamp(*req.drive, 0.0f, 1.0f);
    if (req.noiseLevel)      cfg.noiseLevel      = std::clamp(*req.noiseLevel, 0.0f, 0.25f);
    if (req.dropoutRate)     cfg.dropoutRate     = std::clamp(*req.dropoutRate, 0.0f, 0.95f);
    if (req.crushDownsample) cfg.crushDownsample = std::clamp(*req.crushDownsample, 1, 16);
    if (req.ringModHz)       cfg.ringModHz       = std::clamp(*req.ringModHz, 0.0f, 1000.0f);
    if (req.echoDelayMs)     cfg.echoDelayMs     = std::clamp(*req.echoDelayMs, 0.0f, 250.0f);
    if (req.echoFeedback)    cfg.echoFeedback    = std::clamp(*req.echoFeedback, 0.0f, 0.85f);
    if (req.reverbWet)       cfg.reverbWet       = std::clamp(*req.reverbWet, 0.0f, 1.0f);
    if (req.reverbDecay)     cfg.reverbDecay     = std::clamp(*req.reverbDecay, 0.1f, 0.95f);
    if (req.subOctaveMix)    cfg.subOctaveMix    = std::clamp(*req.subOctaveMix, 0.0f, 1.0f);
    if (req.wetMix)          cfg.wetMix          = std::clamp(*req.wetMix, 0.0f, 1.0f);
    if (req.outputGain)      cfg.outputGain      = std::clamp(*req.outputGain, 0.1f, 3.0f);

    return cfg;
}

// Per-speaker / per-branch stateful DSP chain using signalsmith-dsp & signalsmith-stretch
struct DspChainInstance {
    EffectConfig config{};

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    signalsmith::stretch::SignalsmithStretch<float> subStretch;
    bool stretchInitialized = false;
    bool subStretchInitialized = false;
    float configuredPitch = 0.0f;
    float configuredTonality = 0.0f;

    signalsmith::filters::BiquadStatic<float> hpf1;
    signalsmith::filters::BiquadStatic<float> hpf2;
    signalsmith::filters::BiquadStatic<float> lpf1;
    signalsmith::filters::BiquadStatic<float> lpf2;
    signalsmith::filters::BiquadStatic<float> midPeak;

    signalsmith::delay::Delay<float> slapbackDelay{ 16384 }; // Up to ~340ms @ 48kHz

    // 4-channel Hadamard Feedback Delay Network (FDN) Reverb
    static constexpr std::array<float, 4> kFdnDelaySamples = { 1399.0f, 1777.0f, 2063.0f, 2543.0f }; // Prime sample lengths (~29..53ms)
    std::array<signalsmith::delay::Delay<float>, 4> fdnDelays{
        signalsmith::delay::Delay<float>(4096),
        signalsmith::delay::Delay<float>(4096),
        signalsmith::delay::Delay<float>(4096),
        signalsmith::delay::Delay<float>(4096)
    };
    std::array<signalsmith::filters::BiquadStatic<float>, 4> fdnDamping{};

    float ringPhase = 0.0f;
    int crushCounter = 0;
    float crushHeldSample = 0.0f;

    // Packet-loss / tunnel cellular dropout state machine
    int dropoutFramesRemaining = 0;
    float dropoutGain = 1.0f;
    std::minstd_rand rng{ 0xEE55BEEFu };

    std::vector<float> dryScratch;
    std::vector<float> stretchOutScratch;
    std::vector<float> subOutScratch;

    DspChainInstance() {
        for (auto& d : fdnDamping) {
            d.lowpass(4200.0 / kSampleRate);
        }
    }

    void ApplyConfig(const EffectConfig& newCfg) {
        config = newCfg;
        hpf1.highpass(config.highpassHz / kSampleRate);
        hpf2.highpass(config.highpassHz / kSampleRate);
        lpf1.lowpass(config.lowpassHz / kSampleRate);
        lpf2.lowpass(config.lowpassHz / kSampleRate);
        if (std::abs(config.midPeakDb) > 0.1f) {
            midPeak.peakDb(config.midPeakHz / kSampleRate, config.midPeakDb, 0.9);
        } else {
            midPeak.peakDb(1500.0 / kSampleRate, 0.0, 1.0);
        }

        if (std::abs(config.pitchSemitones) > 0.05f) {
            if (!stretchInitialized) {
                stretch.presetCheaper(1, kSampleRate);
                stretchInitialized = true;
            }
            if (std::abs(configuredPitch - config.pitchSemitones) > 0.01f ||
                std::abs(configuredTonality - config.tonalityLimit) > 0.01f) {
                stretch.setTransposeSemitones(config.pitchSemitones, config.tonalityLimit);
                configuredPitch = config.pitchSemitones;
                configuredTonality = config.tonalityLimit;
            }
        }

        if (config.subOctaveMix > 0.01f && !subStretchInitialized) {
            subStretch.presetCheaper(1, kSampleRate);
            subStretch.setTransposeSemitones(-12.0f, 0.08f);
            subStretchInitialized = true;
        }
    }

    void ResetState() {
        hpf1.reset();
        hpf2.reset();
        lpf1.reset();
        lpf2.reset();
        midPeak.reset();
        slapbackDelay.reset();
        for (auto& d : fdnDelays) d.reset();
        for (auto& f : fdnDamping) f.reset();
        if (stretchInitialized) stretch.reset();
        if (subStretchInitialized) subStretch.reset();
        dropoutFramesRemaining = 0;
        dropoutGain = 1.0f;
    }

    void ProcessFrame(float* samples, size_t count) {
        if (!config.enabled || !samples || count == 0) {
            return;
        }

        dryScratch.assign(samples, samples + count);

        // 1. Pitch & formant shifting
        if (std::abs(config.pitchSemitones) > 0.05f && stretchInitialized) {
            stretchOutScratch.resize(count);
            const float* inCh[1] = { samples };
            float* outCh[1] = { stretchOutScratch.data() };
            stretch.process(inCh, static_cast<int>(count), outCh, static_cast<int>(count));
            std::copy_n(stretchOutScratch.data(), count, samples);
        }

        // 2. Sub-octave pitch layer
        if (config.subOctaveMix > 0.01f && subStretchInitialized) {
            subOutScratch.resize(count);
            const float* inCh[1] = { dryScratch.data() };
            float* outCh[1] = { subOutScratch.data() };
            subStretch.process(inCh, static_cast<int>(count), outCh, static_cast<int>(count));
            const float subGain = config.subOctaveMix;
            for (size_t i = 0; i < count; ++i) {
                samples[i] = samples[i] * (1.0f - subGain * 0.35f) + subOutScratch[i] * subGain;
            }
        }

        // 3. Packet-loss / dropout envelope gate
        float targetDropoutGain = 1.0f;
        if (config.dropoutRate > 0.01f) {
            if (dropoutFramesRemaining > 0) {
                --dropoutFramesRemaining;
                targetDropoutGain = 0.03f;
            } else {
                std::uniform_real_distribution<float> dist(0.0f, 1.0f);
                // Probability of starting a 30..110ms dropout burst on this 10ms frame
                if (dist(rng) < (config.dropoutRate * 0.22f)) {
                    std::uniform_int_distribution<int> lenDist(2, 9);
                    dropoutFramesRemaining = lenDist(rng);
                    targetDropoutGain = 0.03f;
                }
            }
        }

        const float ringStep = (config.ringModHz > 0.5f) ? (kTwoPi * config.ringModHz / kSampleRate) : 0.0f;
        const bool useHpf = (config.highpassHz > 25.0f);
        const bool useLpf = (config.lowpassHz < 19000.0f);
        const bool useMidPeak = (std::abs(config.midPeakDb) > 0.1f);
        const bool useDrive = (config.drive > 0.01f);
        const bool useNoise = (config.noiseLevel > 0.0005f);
        const bool useCrush = (config.crushDownsample > 1);
        const bool useEcho = (config.echoDelayMs > 1.0f && config.echoFeedback > 0.01f);
        const bool useReverb = (config.reverbWet > 0.01f);
        const float echoSamples = std::clamp(config.echoDelayMs * (kSampleRate / 1000.0f), 1.0f, 15000.0f);
        const float driveGain = 1.0f + config.drive * 7.5f;
        const float driveNorm = 1.0f / std::tanh(driveGain * 0.65f);
        const float fdnFeedback = std::clamp(config.reverbDecay * 0.88f, 0.1f, 0.90f);

        std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

        for (size_t i = 0; i < count; ++i) {
            float x = samples[i];
            const float env = std::abs(x);

            // Smooth dropout envelope (prevents harsh clicks when tunnel cuts out)
            dropoutGain = dropoutGain * 0.96f + targetDropoutGain * 0.04f;
            x *= dropoutGain;

            // Sample-rate decimation & quantization
            if (useCrush) {
                if (++crushCounter >= config.crushDownsample) {
                    crushCounter = 0;
                    crushHeldSample = std::round(x * 512.0f) * (1.0f / 512.0f);
                }
                x = crushHeldSample;
            }

            // Ring modulation
            if (ringStep > 0.0f) {
                ringPhase += ringStep;
                if (ringPhase > kTwoPi) ringPhase -= kTwoPi;
                x = x * (0.35f + 0.65f * std::sin(ringPhase));
            }

            // Envelope-gated carrier noise injected pre-filter
            if (useNoise) {
                const float gate = std::clamp((env - 0.008f) * 7.0f, 0.0f, 1.0f);
                x += noiseDist(rng) * config.noiseLevel * gate;
            }

            // 4th-order high-pass / low-pass and parametric mid peak
            if (useHpf) {
                x = hpf2(hpf1(x));
            }
            if (useMidPeak) {
                x = midPeak(x);
            }
            if (useLpf) {
                x = lpf2(lpf1(x));
            }

            // Soft-clipping saturation
            if (useDrive) {
                x = std::tanh(x * driveGain) * driveNorm * 0.82f;
            }

            // Feedback delay line
            if (useEcho) {
                const float delayed = slapbackDelay.read(echoSamples);
                const float nextVal = x + delayed * config.echoFeedback;
                slapbackDelay.write(std::clamp(nextVal, -1.5f, 1.5f));
                x = x * 0.80f + delayed * 0.35f;
            }

            // 4-channel Hadamard FDN reverb
            if (useReverb) {
                std::array<float, 4> fdnOut{};
                for (size_t ch = 0; ch < 4; ++ch) {
                    fdnOut[ch] = fdnDamping[ch](fdnDelays[ch].read(kFdnDelaySamples[ch]));
                }
                const float wetSample = (fdnOut[0] + fdnOut[1] + fdnOut[2] + fdnOut[3]) * 0.5f;
                signalsmith::mix::Hadamard<float, 4>::inPlace(fdnOut);
                for (size_t ch = 0; ch < 4; ++ch) {
                    fdnDelays[ch].write(std::clamp(x * 0.35f + fdnOut[ch] * fdnFeedback, -1.5f, 1.5f));
                }
                x = x * (1.0f - config.reverbWet * 0.45f) + wetSample * config.reverbWet;
            }

            // Wet/Dry blend + output gain + soft-knee brickwall limiter
            float finalSample = (dryScratch[i] * (1.0f - config.wetMix) + x * config.wetMix) * config.outputGain;
            const float absOut = std::abs(finalSample);
            if (absOut > 0.85f) {
                const float sign = (finalSample >= 0.0f) ? 1.0f : -1.0f;
                finalSample = sign * (0.85f + 0.14f * std::tanh((absOut - 0.85f) / 0.14f));
            }
            samples[i] = std::clamp(finalSample, -0.99f, 0.99f);
        }
    }
};

struct SpeakerEffectsSlot {
    eevs::ClientId clientId = 0;
    eevs::NetId netId = 0;
    DspChainInstance ingressChain;  // DspStage::PerSpeakerIngress ("all")
    DspChainInstance branch2dChain; // DspStage::PerRouteBranch (RoutingMode::NonSpatial / Both)
    DspChainInstance branch3dChain; // DspStage::PerRouteBranch (RoutingMode::Spatial3D)
};

class VoiceEffectsPlugin final
    : public eevs::IPlugin
    , public eevs::ISessionListener {
public:
    [[nodiscard]] const char* GetName() const noexcept override { return "voice_effects"; }
    [[nodiscard]] const char* GetVersion() const noexcept override { return "1.0.0"; }

    bool OnLoad(eevs::IHostContext* host) noexcept override {
        m_host = host;
        m_scope.Bind(host);

        m_ingressProcessor.owner = this;
        m_branchProcessor.owner = this;

        m_scope.AddDspProcessor(&m_ingressProcessor, eevs::DspPriority::kVoiceTransform);
        m_scope.AddDspProcessor(&m_branchProcessor, eevs::DspPriority::kEnvironment);
        m_scope.AddSessionListener(this);

        m_scope.AddEndpoint<&VoiceEffectsPlugin::HandleEffectsListHttp>("/effects/list", this);
        m_scope.AddEndpoint<&VoiceEffectsPlugin::HandleEffectsSetHttp>("/effects/set", this);
        m_scope.AddEndpoint<&VoiceEffectsPlugin::HandleEffectsClearHttp>("/effects/clear", this);

        m_host->Log(
            eevs::LogLevel::Info,
            GetName(),
            "Loaded Signalsmith DSP & Stretch real-time voice effects plugin (/effects/list, /effects/set, /effects/clear)"
        );
        return true;
    }

    void OnUnload() noexcept override {
        m_scope.Reset();
        m_host = nullptr;
        m_slotsByClientId.clear();
    }

    void OnClientUnregistered(eevs::ClientId clientId, eevs::NetId /*netId*/) noexcept override {
        m_slotsByClientId.erase(clientId);
    }

private:
    struct IngressDspAdapter final : public eevs::IDspProcessor {
        VoiceEffectsPlugin* owner = nullptr;
        [[nodiscard]] eevs::DspStage GetStage() const noexcept override {
            return eevs::DspStage::PerSpeakerIngress;
        }
        [[nodiscard]] bool ShouldProcess(
            eevs::ClientId speakerClientId,
            eevs::NetId /*speakerNetId*/,
            eevs::RoutingMode /*mode*/
        ) const noexcept override {
            return owner && owner->ShouldProcessIngress(speakerClientId);
        }
        void ProcessPcm(
            const eevs::SpeakerContext& speaker,
            eevs::RoutingMode /*mode*/,
            eevs::PcmFrameBuffer& pcm
        ) noexcept override {
            if (owner) {
                owner->ProcessIngressPcm(speaker, pcm);
            }
        }
    };

    struct BranchDspAdapter final : public eevs::IDspProcessor {
        VoiceEffectsPlugin* owner = nullptr;
        [[nodiscard]] eevs::DspStage GetStage() const noexcept override {
            return eevs::DspStage::PerRouteBranch;
        }
        [[nodiscard]] bool ShouldProcess(
            eevs::ClientId speakerClientId,
            eevs::NetId /*speakerNetId*/,
            eevs::RoutingMode mode
        ) const noexcept override {
            return owner && owner->ShouldProcessBranch(speakerClientId, mode);
        }
        void ProcessPcm(
            const eevs::SpeakerContext& speaker,
            eevs::RoutingMode mode,
            eevs::PcmFrameBuffer& pcm
        ) noexcept override {
            if (owner) {
                owner->ProcessBranchPcm(speaker, mode, pcm);
            }
        }
    };

    [[nodiscard]] bool ShouldProcessIngress(eevs::ClientId speakerClientId) const noexcept {
        auto it = m_slotsByClientId.find(speakerClientId);
        return it != m_slotsByClientId.end() && it->second.ingressChain.config.enabled;
    }

    [[nodiscard]] bool ShouldProcessBranch(eevs::ClientId speakerClientId, eevs::RoutingMode mode) const noexcept {
        auto it = m_slotsByClientId.find(speakerClientId);
        if (it == m_slotsByClientId.end()) {
            return false;
        }
        if (mode == eevs::RoutingMode::NonSpatial || mode == eevs::RoutingMode::Both) {
            return it->second.branch2dChain.config.enabled;
        }
        if (mode == eevs::RoutingMode::Spatial3D) {
            return it->second.branch3dChain.config.enabled;
        }
        return false;
    }

    void ProcessIngressPcm(const eevs::SpeakerContext& speaker, eevs::PcmFrameBuffer& pcm) {
        auto it = m_slotsByClientId.find(speaker.clientId);
        if (it == m_slotsByClientId.end() || !it->second.ingressChain.config.enabled) {
            return;
        }
        it->second.netId = speaker.netId;
        it->second.ingressChain.ProcessFrame(pcm.samples, pcm.sampleCount);
    }

    void ProcessBranchPcm(const eevs::SpeakerContext& speaker, eevs::RoutingMode mode, eevs::PcmFrameBuffer& pcm) {
        auto it = m_slotsByClientId.find(speaker.clientId);
        if (it == m_slotsByClientId.end()) {
            return;
        }
        it->second.netId = speaker.netId;
        if ((mode == eevs::RoutingMode::NonSpatial || mode == eevs::RoutingMode::Both) &&
            it->second.branch2dChain.config.enabled) {
            it->second.branch2dChain.ProcessFrame(pcm.samples, pcm.sampleCount);
        } else if (mode == eevs::RoutingMode::Spatial3D && it->second.branch3dChain.config.enabled) {
            it->second.branch3dChain.ProcessFrame(pcm.samples, pcm.sampleCount);
        }
    }

    static std::vector<PresetDescriptorDto> GetBuiltinPresetCatalog() {
        return {
            { "mask",         "🎭 Anonymous Mask",       "all", "Pitch & formant disguise (-3.5 st) + warm filter so masked voice cannot be recognized" },
            { "radio",        "📻 Police Walkie-Talkie", "2D",  "Authentic 2D radio band-pass (380-3100Hz) + overdrive + carrier static hiss" },
            { "tunnel_phone", "🚇 Tunnel Phone Signal",  "2D",  "Degrades ONLY 2D phone/radio with packet dropouts, 8kHz codec crush & static while 3D proximity stays clean" },
            { "megaphone",    "📢 Police Megaphone",     "3D",  "Resonant horn band-pass + overdrive clipper + 68ms street slapback echo" },
            { "robot",        "🤖 Cyber Android",        "all", "55Hz ring modulator + 11.5ms metallic comb resonance" },
            { "demon",        "👹 Demon / Monster",      "all", "-6st pitch shift + -12st sub-octave growl + dark cavern FDN reverb" },
            { "helium",       "🎈 Helium Chipmunk",      "all", "+5.5st phase-vocoder pitch shift with formant preservation" },
            { "underwater",   "🌊 Underwater / Scuba",   "all", "520Hz muffled low-pass + resonant bubble filter + FDN reverb" },
            { "cave_reverb",  "🏛 Tunnel / Cave Reverb", "3D",  "4-channel Hadamard Feedback Delay Network (FDN) acoustic reverb" }
        };
    }

    static ActiveEffectItemDto MakeActiveItem(
        eevs::ClientId clientId,
        eevs::NetId netId,
        std::string_view branchName,
        const EffectConfig& c
    ) {
        return ActiveEffectItemDto{
            .clientId = clientId,
            .netId = netId,
            .targetBranch = std::string(branchName),
            .preset = c.preset,
            .pitchSemitones = c.pitchSemitones,
            .highpassHz = c.highpassHz,
            .lowpassHz = c.lowpassHz,
            .drive = c.drive,
            .noiseLevel = c.noiseLevel,
            .dropoutRate = c.dropoutRate,
            .reverbWet = c.reverbWet,
            .wetMix = c.wetMix,
            .outputGain = c.outputGain
        };
    }

    bool HandleEffectsListHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view /*reqBody*/,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        EffectsListRespDto dto{
            .ok = true,
            .presets = GetBuiltinPresetCatalog(),
            .active = {}
        };

        for (const auto& [cid, slot] : m_slotsByClientId) {
            if (slot.ingressChain.config.enabled) {
                dto.active.push_back(MakeActiveItem(cid, slot.netId, "all", slot.ingressChain.config));
            }
            if (slot.branch2dChain.config.enabled) {
                dto.active.push_back(MakeActiveItem(cid, slot.netId, "2D", slot.branch2dChain.config));
            }
            if (slot.branch3dChain.config.enabled) {
                dto.active.push_back(MakeActiveItem(cid, slot.netId, "3D", slot.branch3dChain.config));
            }
        }

        outStatusCode = 200;
        WriteJsonToResponseBuffer(dto, responseBuf, responseCap);
        return true;
    }

    bool HandleEffectsSetHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        EffectSetReqDto req{};
        if (glz::read<kRelaxedJsonOpts>(req, reqBody) || !req.netId) {
            outStatusCode = 400;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Missing or invalid netId"}, responseBuf, responseCap);
            return true;
        }

        const eevs::NetId netId = *req.netId;
        auto clientIdOpt = m_host->GetSessions().ResolveClientId(netId);
        if (!clientIdOpt) {
            outStatusCode = 404;
            WriteJsonToResponseBuffer(ErrorRespDto{false, "Speaker netId is not registered in EEVS"}, responseBuf, responseCap);
            return true;
        }
        const eevs::ClientId clientId = *clientIdOpt;

        EffectConfig cfg = BuildConfigFromRequest(req);
        SpeakerEffectsSlot& slot = m_slotsByClientId[clientId];
        slot.clientId = clientId;
        slot.netId = netId;

        if (cfg.targetBranch == "2D" || cfg.targetBranch == "2d") {
            cfg.targetBranch = "2D";
            slot.branch2dChain.ApplyConfig(cfg);
        } else if (cfg.targetBranch == "3D" || cfg.targetBranch == "3d") {
            cfg.targetBranch = "3D";
            slot.branch3dChain.ApplyConfig(cfg);
        } else {
            cfg.targetBranch = "all";
            slot.ingressChain.ApplyConfig(cfg);
        }

        EffectAppliedRespDto resp{
            .ok = true,
            .clientId = clientId,
            .netId = netId,
            .preset = cfg.preset,
            .targetBranch = cfg.targetBranch
        };
        std::string evJson;
        (void)glz::write_json(resp, evJson);
        m_host->GetEvents().Publish("effects.changed", netId, evJson.c_str());

        outStatusCode = 200;
        WriteJsonToResponseBuffer(resp, responseBuf, responseCap);
        return true;
    }

    bool HandleEffectsClearHttp(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view reqBody,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        EffectClearReqDto req{};
        if (!reqBody.empty()) {
            (void)glz::read<kRelaxedJsonOpts>(req, reqBody);
        }

        if (!req.netId || req.targetBranch == "everything") {
            const size_t count = m_slotsByClientId.size();
            m_slotsByClientId.clear();
            m_host->GetEvents().Publish("effects.cleared", 0, R"({"ok":true})");
            outStatusCode = 200;
            WriteJsonToResponseBuffer(SimpleOkRespDto{true, count}, responseBuf, responseCap);
            return true;
        }

        auto clientIdOpt = m_host->GetSessions().ResolveClientId(*req.netId);
        if (!clientIdOpt) {
            outStatusCode = 200;
            WriteJsonToResponseBuffer(SimpleOkRespDto{true, 0}, responseBuf, responseCap);
            return true;
        }

        auto it = m_slotsByClientId.find(*clientIdOpt);
        if (it != m_slotsByClientId.end()) {
            if (req.targetBranch == "2D" || req.targetBranch == "2d") {
                it->second.branch2dChain.config.enabled = false;
                it->second.branch2dChain.ResetState();
            } else if (req.targetBranch == "3D" || req.targetBranch == "3d") {
                it->second.branch3dChain.config.enabled = false;
                it->second.branch3dChain.ResetState();
            } else {
                m_slotsByClientId.erase(it);
            }
        }

        m_host->GetEvents().Publish("effects.cleared", *req.netId, R"({"ok":true})");
        outStatusCode = 200;
        WriteJsonToResponseBuffer(SimpleOkRespDto{true, 1}, responseBuf, responseCap);
        return true;
    }

    eevs::IHostContext* m_host = nullptr;
    eevs::ScopedPluginRegistrations m_scope;
    IngressDspAdapter m_ingressProcessor{};
    BranchDspAdapter m_branchProcessor{};
    eevs::FlatMap<eevs::ClientId, SpeakerEffectsSlot> m_slotsByClientId;
};

} // namespace

EEVS_DECLARE_PLUGIN(VoiceEffectsPlugin)
