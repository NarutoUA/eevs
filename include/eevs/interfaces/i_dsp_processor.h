#pragma once

#include "eevs/types.h"

namespace eevs {

enum class DspStage : uint8_t {
    PerSpeakerIngress = 0, ///< Applied once per speaker frame before routing.
    PerRouteBranch    = 1  ///< Applied per outbound `RoutingMode` branch.
};

/**
 * @brief Standard priority tiers for ordering multiple `IDspProcessor` instances (lower runs first).
 */
namespace DspPriority {
    inline constexpr int kPreProcess     = 100;
    inline constexpr int kVoiceTransform = 300;
    inline constexpr int kEnvironment    = 500;
    inline constexpr int kPostLimiter    = 900;
} // namespace DspPriority

/**
 * @brief Real-time in-place PCM processor.
 *
 * Opus decoding and re-encoding are bypassed when no registered `IDspProcessor` or `IAudioTap`
 * requests decoded PCM for a frame.
 */
class IDspProcessor {
public:
    [[nodiscard]] virtual DspStage GetStage() const noexcept = 0;

    /**
     * @brief Returns `true` if `ProcessPcm` should run for the given speaker and routing mode.
     * @note `mode` is `RoutingMode::None` during `DspStage::PerSpeakerIngress`.
     */
    [[nodiscard]] virtual bool ShouldProcess(ClientId speakerClientId, NetId speakerNetId, RoutingMode mode) const noexcept = 0;

    /** @brief Modifies `pcm.samples[0 .. pcm.sampleCount)` in-place. */
    virtual void ProcessPcm(const SpeakerContext& speaker, RoutingMode mode, PcmFrameBuffer& pcm) noexcept = 0;

protected:
    ~IDspProcessor() = default;
};

} // namespace eevs
