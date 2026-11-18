#pragma once

#include "eevs/types.h"

#include <cstddef>
#include <cstdint>

namespace eevs {

/**
 * @brief Interface provided to `IAudioSource::OnTick10ms` for emitting outbound voice frames.
 *
 * If `explicitListeners` is `nullptr`, frames are routed using the active routing table for `senderNetId`.
 */
class IAudioInjector {
public:
    virtual size_t InjectOpusFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        const uint8_t* opusData,
        size_t opusLen,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) noexcept = 0;

    virtual size_t InjectPcmFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        const float* pcmSamples,
        size_t sampleCount,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) noexcept = 0;

    // Non-virtual span convenience overloads (zero vtable/ABI impact)
    size_t InjectOpusFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const uint8_t> opusData,
        std::span<const ClientId> explicitListeners = {}
    ) noexcept {
        return InjectOpusFrame(
            senderNetId,
            fallbackMode,
            opusData.data(),
            opusData.size(),
            explicitListeners.empty() ? nullptr : explicitListeners.data(),
            explicitListeners.size()
        );
    }

    size_t InjectPcmFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const float> pcmSamples,
        std::span<const ClientId> explicitListeners = {}
    ) noexcept {
        return InjectPcmFrame(
            senderNetId,
            fallbackMode,
            pcmSamples.data(),
            pcmSamples.size(),
            explicitListeners.empty() ? nullptr : explicitListeners.data(),
            explicitListeners.size()
        );
    }

protected:
    ~IAudioInjector() = default;
};

/**
 * @brief Periodic 10ms audio source polled on the real-time voice thread.
 */
class IAudioSource {
public:
    virtual void OnTick10ms(IAudioInjector& injector) noexcept = 0;

protected:
    ~IAudioSource() = default;
};

} // namespace eevs
