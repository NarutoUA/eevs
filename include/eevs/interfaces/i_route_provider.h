#pragma once

#include "eevs/types.h"

#include <cstddef>

namespace eevs {

/**
 * @brief Interface for inspecting or modifying a speaker's resolved listener routes.
 */
class IRouteProvider {
public:
    /**
     * @param routes      Mutable buffer of `ResolvedRoute` entries; `[0 .. inOutCount)` holds current routes.
     * @param inOutCount  Current route count on entry; updated route count (`<= maxCapacity`) on return.
     * @param maxCapacity Maximum number of entries `routes` can hold.
     */
    virtual void ResolveSpeakerRoutes(
        ClientId speakerClientId,
        NetId speakerNetId,
        ResolvedRoute* routes,
        size_t& inOutCount,
        size_t maxCapacity
    ) noexcept = 0;

protected:
    ~IRouteProvider() = default;
};

} // namespace eevs
