#pragma once

#include "eevs/types.h"

namespace eevs {

/**
 * @brief Observer notified of client session registration and connection lifecycle events.
 */
class ISessionListener {
public:
    virtual void OnClientRegistered(ClientId clientId, NetId netId) noexcept {
        (void)clientId;
        (void)netId;
    }

    virtual void OnClientAuthenticated(ClientId clientId, NetId netId) noexcept {
        (void)clientId;
        (void)netId;
    }

    virtual void OnClientDisconnected(ClientId clientId, NetId netId, bool wasTimeout) noexcept {
        (void)clientId;
        (void)netId;
        (void)wasTimeout;
    }

    virtual void OnClientUnregistered(ClientId clientId, NetId netId) noexcept {
        (void)clientId;
        (void)netId;
    }

protected:
    ~ISessionListener() = default;
};

} // namespace eevs
