#pragma once

#include "eevs/types.h"

#include <cstddef>
#include <cstdint>

namespace eevs {

enum class PacketFilterVerdict : uint8_t {
    Allow            = 0, ///< Process and forward the packet normally.
    TapOnlyNoForward = 1, ///< Pass the packet to VAD and `IAudioTap`, but suppress forwarding to listeners.
    DropSilent       = 2  ///< Drop the packet immediately before VAD, taps, DSP, or forwarding.
};

struct VoiceIngressPacketView {
    ClientId      senderClientId = 0;
    NetId         senderNetId    = 0;
    uint32_t      sequenceNumber = 0;
    OpusFrameView opus{};
};

/**
 * @brief Pre-decode ingress voice packet filter.
 *
 * Evaluated on the voice thread before decoding or routing. When multiple filters are
 * registered, the most restrictive verdict takes precedence (`DropSilent` > `TapOnlyNoForward` > `Allow`).
 */
class IVoicePacketFilter {
public:
    [[nodiscard]] virtual PacketFilterVerdict InspectIngressPacket(
        const VoiceIngressPacketView& packet
    ) noexcept = 0;

protected:
    ~IVoicePacketFilter() = default;
};

} // namespace eevs
