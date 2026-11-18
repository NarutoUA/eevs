--- @file eevs.lua
--- @brief EEVS Core Server SDK Lua Helper Library (`@eevs/lib/eevs.lua`).
--- @details Include in any FiveM resource's `fxmanifest.lua`:
--- ```lua
--- server_scripts { '@eevs/lib/eevs.lua', 'server.lua' }
--- ```
--- Every `EEVS.<Method>(...)` call dynamically forwards to `exports['eevs']:<Method>(...)`.
--- All asynchronous control methods support dual-mode execution:
---   - **Callback mode**: Pass `cb(statusCode, responseTable)` as the final argument.
---   - **Coroutine / Await mode**: Omit `cb` to yield via `Citizen.Await` and receive `responseTable, statusCode`.

--- @alias EevsCallback fun(statusCode: integer, response: table)
--- @alias EevsRoutingMode '"2D"'|'"3D"'|'"both"'
--- @alias EevsRouteAction '"force"'|'"merge"'

--- @class EEVS
EEVS = setmetatable({}, {
    __index = function(_self, key)
        return function(...)
            return exports['eevs'][key](exports['eevs'], ...)
        end
    end
})

-- ============================================================================
-- Real-Time Event Subscription Helpers
-- ============================================================================

--- @brief Subscribe to real-time edge-triggered Voice Activity Detection (VAD) state changes.
--- @param handler fun(netId: integer, isTalking: boolean, clientId: integer) Callback invoked when a speaker starts or stops talking.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnTalkState(handler)
    return AddEventHandler('eevs:talkStateChanged', handler)
end

--- @brief Subscribe to player voice authentication events.
--- @param handler fun(netId: integer, clientId: integer) Callback invoked when a client completes authentication with EEVS.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnClientAuthenticated(handler)
    return AddEventHandler('eevs:clientAuthenticated', handler)
end

--- @brief Subscribe to player voice disconnect / timeout events.
--- @param handler fun(netId: integer, clientId: integer, timedOut: boolean) Callback invoked when a client session disconnects.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnClientDisconnected(handler)
    return AddEventHandler('eevs:clientDisconnected', handler)
end

--- @brief Subscribe to player session registration events.
--- @param handler fun(netId: integer, clientId: integer) Callback invoked when a session is registered in EEVS.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnClientRegistered(handler)
    return AddEventHandler('eevs:clientRegistered', handler)
end

--- @brief Subscribe to player session unregistration events.
--- @param handler fun(netId: integer, clientId: integer) Callback invoked when a session is removed from EEVS.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnClientUnregistered(handler)
    return AddEventHandler('eevs:clientUnregistered', handler)
end

--- @brief Subscribe to any EEVS core or C++ plugin event emitted over `/ws`.
--- @param topicOrHandler string|fun(topic: string, subjectNetId: integer, data: table) Specific topic name (e.g. `'stt.completed'`, `'effects.changed'`) or a catch-all callback.
--- @param maybeHandler? fun(subjectNetId: integer, data: table, topic: string) Callback when `topicOrHandler` is a topic string.
--- @return table eventCookie FiveM event handler cookie.
function EEVS.OnEvent(topicOrHandler, maybeHandler)
    if type(topicOrHandler) == 'function' then
        return AddEventHandler('eevs:event', topicOrHandler)
    end
    return AddEventHandler('eevs:event', function(topic, subjectNetId, data)
        if topic == topicOrHandler then
            maybeHandler(subjectNetId, data, topic)
        end
    end)
end

-- ============================================================================
-- Global `Eevs*` Voice Channel & Proximity Native Wrapper Aliases
-- ============================================================================

--- @brief Create a FiveM Enhanced voice channel with EEVS Mode 2 Custom Streaming and wiretap tracking.
--- @details Drop-in wrapper around `CreateVoiceChannel(mode, maxDistance)`.
---   - `mode == 0`: Non-spatial (2D radio/phone, handled natively by FXServer).
---   - `mode == 1`: Spatial (3D distance-culled up to `maxDistance`, handled natively by FXServer).
---   - `mode == 2`: Custom Streaming (automatically meshed and synced to EEVS via `/route/sync`).
--- @param mode integer Channel mode (`0` = Non-Spatial, `1` = Spatial, `2` = Custom Streaming).
--- @param maxDistance? number Maximum audible distance in meters for spatial channels (default `0.0`).
--- @param options? table Optional metadata (`{ name?: string, routingMode?: EevsRoutingMode }`).
--- @return integer channelId Created channel identifier.
function EevsCreateVoiceChannel(mode, maxDistance, options)
    return EEVS.CreateVoiceChannel(mode, maxDistance, options)
end

--- @brief Delete a voice channel and clear any custom EEVS routes associated with its members.
--- @param channelId integer Target voice channel ID.
--- @return boolean deleted `true` if deleted.
function EevsDeleteVoiceChannel(channelId)
    return EEVS.DeleteVoiceChannel(channelId)
end

--- @brief Add a player to a voice channel (and sync custom routes if Mode 2 or wiretapped).
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @return boolean added `true` if added.
function EevsAddPlayerToVoiceChannel(channelId, playerSrc)
    return EEVS.AddPlayerToVoiceChannel(channelId, playerSrc)
end

--- @brief Remove a player from a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @return boolean removed `true` if removed.
function EevsRemovePlayerFromVoiceChannel(channelId, playerSrc)
    return EEVS.RemovePlayerFromVoiceChannel(channelId, playerSrc)
end

--- @brief Mute or unmute a player in a specific voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @param muted boolean Whether the player is muted in this channel.
--- @return boolean ok `true` if updated.
function EevsSetPlayerVoiceChannelMuted(channelId, playerSrc, muted)
    return EEVS.SetPlayerVoiceChannelMuted(channelId, playerSrc, muted)
end

--- @brief Deafen or undeafen a player in a specific voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @param deaf boolean Whether the player is deafened in this channel.
--- @return boolean ok `true` if updated.
function EevsSetPlayerVoiceChannelDeaf(channelId, playerSrc, deaf)
    return EEVS.SetPlayerVoiceChannelDeaf(channelId, playerSrc, deaf)
end

--- @brief Check whether a player is currently a member of a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @return boolean exists `true` if the player is in the channel.
function EevsDoesPlayerExistInVoiceChannel(channelId, playerSrc)
    return EEVS.DoesPlayerExistInVoiceChannel(channelId, playerSrc)
end

--- @brief Check whether a player is muted in a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @return boolean muted `true` if muted.
function EevsIsPlayerVoiceChannelMuted(channelId, playerSrc)
    return EEVS.IsPlayerVoiceChannelMuted(channelId, playerSrc)
end

--- @brief Check whether a player is deafened in a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param playerSrc integer|string Player server ID.
--- @return boolean deaf `true` if deafened.
function EevsIsPlayerVoiceChannelDeaf(channelId, playerSrc)
    return EEVS.IsPlayerVoiceChannelDeaf(channelId, playerSrc)
end

--- @brief Attach a silent one-way wiretap to a voice channel (works on Modes 0, 1, and 2).
--- @details Routes all unmuted members of `channelId` to `listenerSrc` via EEVS without adding `listenerSrc` as a visible member of the channel.
--- @param channelId integer Target voice channel ID to monitor.
--- @param listenerSrc integer|string Server ID of the listening player (e.g. admin/dispatcher).
--- @param mode? EevsRoutingMode Delivery mode (`"2D"` default or `"3D"`).
--- @return boolean ok `true` if wiretap was registered.
function EevsWiretapVoiceChannel(channelId, listenerSrc, mode)
    return EEVS.WiretapVoiceChannel(channelId, listenerSrc, mode)
end

--- @brief Remove a silent one-way wiretap from a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param listenerSrc integer|string Server ID of the listening player.
--- @return boolean ok `true` if wiretap was removed.
function EevsUnwiretapVoiceChannel(channelId, listenerSrc)
    return EEVS.UnwiretapVoiceChannel(channelId, listenerSrc)
end

--- @brief Broadcast a cached EEVS audio clip to all non-deafened members (and wiretappers) of a voice channel.
--- @param channelId integer Target voice channel ID.
--- @param clipId string Clip identifier in EEVS `ClipManager`.
--- @param senderNetId? integer Sender NetID / Server ID for packet synthesis (defaults to first channel member).
--- @param modeOverride? EevsRoutingMode|EevsCallback Optional routing mode override (`"2D"` or `"3D"`), or callback if omitted.
--- @param cb? EevsCallback Optional completion callback.
--- @return table|nil response, integer|nil statusCode
function EevsPlayClipInVoiceChannel(channelId, clipId, senderNetId, modeOverride, cb)
    return EEVS.PlayClipInVoiceChannel(channelId, clipId, senderNetId, modeOverride, cb)
end

--- @brief Set how far a player can be heard in spatial voice channels (`SetPlayerVoiceProximityRange`).
--- @details Clamps `proximityRange` to `[0.0, 512.0]`, invokes FXServer native `SetPlayerVoiceProximityRange`,
--- replicates `Player(playerSrc).state['eevs:proximityRange']`, and triggers `eevs:playerProximityRangeChanged`.
--- A value of `0.0` resets the player to use the maximum distance of each spatial channel.
--- @param playerSrc integer|string Player server ID.
--- @param proximityRange number Audible radius in meters (`0.0 .. 512.0`).
--- @return boolean nativeApplied `true` if the FXServer native succeeded.
function EevsSetPlayerVoiceProximityRange(playerSrc, proximityRange)
    return EEVS.SetPlayerVoiceProximityRange(playerSrc, proximityRange)
end

--- @brief Get the last configured spatial voice proximity range for a player.
--- @param playerSrc integer|string Player server ID.
--- @return number proximityRange Range in meters (`0.0` = channel maximum).
--- @return boolean nativeApplied Whether the FXServer native applied it.
function EevsGetPlayerVoiceProximityRange(playerSrc)
    return EEVS.GetPlayerVoiceProximityRange(playerSrc)
end

return EEVS
