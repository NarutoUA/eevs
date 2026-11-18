#pragma once

#include "eevs/types.h"

namespace eevs {

enum class TapStage : uint8_t {
    PreDsp         = 0, ///< Decoded PCM before per-speaker ingress DSP.
    PostIngressDsp = 1  ///< Decoded PCM after per-speaker ingress DSP.
};

/**
 * @brief Read-only observer for incoming Opus frames, decoded PCM frames, and VAD state changes.
 *
 * All callbacks run on the real-time voice thread and must not block.
 */
class IAudioTap {
public:
    /** @brief Return `true` to request decoded 48kHz float PCM via `OnPcmFrame` for `speakerClientId`. */
    [[nodiscard]] virtual bool WantsDecodedPcm(ClientId speakerClientId) const noexcept {
        (void)speakerClientId;
        return false;
    }

    [[nodiscard]] virtual TapStage GetPcmTapStage() const noexcept {
        return TapStage::PreDsp;
    }

    virtual void OnOpusFrame(const SpeakerContext& speaker, OpusFrameView opus) noexcept {
        (void)speaker;
        (void)opus;
    }

    virtual void OnPcmFrame(const SpeakerContext& speaker, PcmFrameView pcm) noexcept {
        (void)speaker;
        (void)pcm;
    }

    virtual void OnTalkStateChanged(ClientId speakerClientId, NetId speakerNetId, bool isTalking) noexcept {
        (void)speakerClientId;
        (void)speakerNetId;
        (void)isTalking;
    }

protected:
    ~IAudioTap() = default;
};

} // namespace eevs
