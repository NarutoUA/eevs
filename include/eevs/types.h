#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eevs {

/** @brief Internal client slot identifier (`0 .. kMaxClients - 1`). */
using ClientId = uint16_t;

/** @brief Game server network ID (player server ID or synthetic speaker ID). */
using NetId    = uint16_t;

/** @brief Maximum number of concurrent client slots (`2^13`). */
inline constexpr size_t kMaxClients = 8192;

enum class RoutingMode : uint8_t {
    None       = 0,
    Spatial3D  = 1, ///< 3D positional audio.
    NonSpatial = 2, ///< 2D non-positional audio.
    Both       = 3  ///< Both 3D positional and 2D non-positional flags set.
};

[[nodiscard]] constexpr RoutingMode operator|(RoutingMode lhs, RoutingMode rhs) noexcept {
    return static_cast<RoutingMode>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

[[nodiscard]] constexpr RoutingMode operator&(RoutingMode lhs, RoutingMode rhs) noexcept {
    return static_cast<RoutingMode>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
}

[[nodiscard]] constexpr RoutingMode operator~(RoutingMode mode) noexcept {
    return static_cast<RoutingMode>(~static_cast<uint8_t>(mode) & 0x03u);
}

struct ResolvedRoute {
    ClientId    targetClientId = 0;
    RoutingMode mode           = RoutingMode::None;
};

struct SpeakerContext {
    ClientId clientId       = 0;     ///< `0` when `isSynthetic` is `true`.
    NetId    netId          = 0;
    uint32_t sequenceNumber = 0;
    bool     isSynthetic    = false;
};

enum class BatchRouteOp : uint8_t {
    Add          = 0, ///< Replace route override on `(fromNetId -> toNetId)`.
    Merge        = 1, ///< Bitwise-OR `mode` into route on `(fromNetId -> toNetId)`.
    Block        = 2, ///< Block route on `(fromNetId -> toNetId)`.
    Remove       = 3, ///< Remove custom override on `(fromNetId -> toNetId)`.
    ClearSpeaker = 4  ///< Remove all custom overrides for `fromNetId`.
};

struct BatchRouteCommand {
    NetId        fromNetId = 0;
    NetId        toNetId   = 0; ///< Ignored when `op == BatchRouteOp::ClearSpeaker`.
    RoutingMode  mode      = RoutingMode::NonSpatial;
    BatchRouteOp op        = BatchRouteOp::Add;
};

/** @brief Non-owning view over a single 10ms compressed Opus frame. */
struct OpusFrameView {
    const uint8_t* data = nullptr;
    size_t         size = 0;

    [[nodiscard]] constexpr std::span<const uint8_t> AsSpan() const noexcept {
        return {data, size};
    }
    [[nodiscard]] constexpr bool Empty() const noexcept {
        return !data || size == 0;
    }
};

/** @brief Non-owning read-only view over a decoded mono float PCM frame (`[-1.0f, 1.0f]`). */
struct PcmFrameView {
    const float* samples     = nullptr;
    size_t       sampleCount = 0;
    uint32_t     sampleRate  = 48000;

    [[nodiscard]] constexpr std::span<const float> AsSpan() const noexcept {
        return {samples, sampleCount};
    }
    [[nodiscard]] constexpr bool Empty() const noexcept {
        return !samples || sampleCount == 0;
    }
};

/** @brief Mutable view over a decoded mono float PCM buffer (`[-1.0f, 1.0f]`). */
struct PcmFrameBuffer {
    float*   samples     = nullptr;
    size_t   sampleCount = 0;
    uint32_t sampleRate  = 48000;

    [[nodiscard]] constexpr std::span<float> AsSpan() const noexcept {
        return {samples, sampleCount};
    }
    [[nodiscard]] constexpr bool Empty() const noexcept {
        return !samples || sampleCount == 0;
    }
};

} // namespace eevs
