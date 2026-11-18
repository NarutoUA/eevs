--- @file server.lua
--- @brief EEVS Core Server SDK Runtime (`scripts/eevs/server.lua`).
--- @details Provides cross-language FiveM server exports (Lua, JS/TS, C#), dual-mode
--- WebSocket/HTTP control dispatch (`/ws` + HTTP fallback), real-time VAD & session
--- state caching, custom routing overrides, clip playback, and FiveM Enhanced
--- Voice Channel & Proximity wrappers.

local function resolveControlBaseUrl()
    local ctrlHost = GetConvar('eevs_control_host', '')
    if ctrlHost and ctrlHost ~= '' then
        ctrlHost = (ctrlHost:gsub('^%s*(.-)%s*$', '%1')):gsub('^["\'](.+)["\']$', '%1')
        if ctrlHost ~= '' then
            if not ctrlHost:find('^https?://') then
                return 'http://' .. ctrlHost
            end
            return ctrlHost
        end
    end
    return GetConvar('eevs_control_url', 'http://127.0.0.1:30124')
end

local EEVS_HOST = resolveControlBaseUrl()
local cachedAuthToken = nil
local wsBridgeConnected = false

-- Real-time in-memory session & VAD state cache (updated via WebSocket events)
local talkingByNetId = {}
local sessionsByNetId = {}

local function trim(s)
    return (s:gsub('^%s*(.-)%s*$', '%1'))
end

local function stripQuotes(s)
    s = trim(s)
    if (#s >= 2) and ((s:sub(1, 1) == '"' and s:sub(-1) == '"') or (s:sub(1, 1) == "'" and s:sub(-1) == "'")) then
        return s:sub(2, -2)
    end
    return s
end

local function resolveAuthToken()
    if cachedAuthToken and cachedAuthToken ~= '' then
        return cachedAuthToken
    end

    local explicitKey = stripQuotes(GetConvar('eevs_control_key', ''))
    if explicitKey ~= '' and explicitKey ~= 'changeme' then
        cachedAuthToken = explicitKey
        return cachedAuthToken
    end

    local licenseConvar = stripQuotes(GetConvar('sv_licenseKey', ''))
    if licenseConvar ~= '' and licenseConvar ~= 'changeme' then
        cachedAuthToken = licenseConvar
        return cachedAuthToken
    end

    local candidateFiles = { 'server.cfg', '../server.cfg', 'eevs.cfg' }
    for _, cfgPath in ipairs(candidateFiles) do
        local f = io.open(cfgPath, 'r')
        if f then
            local foundLicense = nil
            local foundControlKey = nil
            for line in f:lines() do
                local clean = line:gsub('#.*$', ''):gsub('//.*$', '')
                local ctrlMatch = clean:match('eevs_control_key%s+([^%s]+)')
                if ctrlMatch then
                    local val = stripQuotes(ctrlMatch)
                    if val ~= '' and val ~= 'changeme' then
                        foundControlKey = val
                    end
                end
                local licMatch = clean:match('sv_licenseKey%s+([^%s]+)')
                if licMatch then
                    local val = stripQuotes(licMatch)
                    if val ~= '' and val ~= 'changeme' then
                        foundLicense = val
                    end
                end
            end
            f:close()

            if foundControlKey then
                cachedAuthToken = foundControlKey
                return cachedAuthToken
            end
            if foundLicense then
                cachedAuthToken = foundLicense
                return cachedAuthToken
            end
        end
    end

    return ''
end

local function httpFallbackRequest(method, path, payload, cb)
    local token = resolveAuthToken()
    local url = EEVS_HOST .. path
    local bodyStr = payload and json.encode(payload) or ''
    local headers = {
        ['Content-Type'] = 'application/json',
        ['Authorization'] = 'Bearer ' .. token,
        ['X-EEVS-Key'] = token
    }

    PerformHttpRequest(url, function(statusCode, responseBody, _responseHeaders)
        local decoded = nil
        if responseBody and #responseBody > 0 then
            local ok, res = pcall(json.decode, responseBody)
            if ok and type(res) == 'table' then
                decoded = res
            end
        end
        if not decoded then
            local fallbackErr = (responseBody and #responseBody > 0)
                and responseBody
                or (('HTTP %s (EEVS unreachable or returned empty response)'):format(tostring(statusCode or 0)))
            decoded = {
                ok = false,
                error = fallbackErr,
                raw = responseBody
            }
        elseif decoded.ok == false and not decoded.error then
            decoded.error = decoded.text or decoded.raw or ('HTTP ' .. tostring(statusCode or 500))
        end
        if cb then
            cb(statusCode or 0, decoded)
        end
    end, method, bodyStr, headers)
end

-- Dual-mode dispatcher:
--   1. If `cb` is passed, runs asynchronously and invokes `cb(status, data)`.
--   2. If `cb` is omitted/nil, awaits via `promise` and returns `data, status` directly.
local function eevsRequest(method, path, payload, cb)
    local function dispatch(done)
        if wsBridgeConnected then
            TriggerEvent('eevs:internalSendWs', method, path, payload, function(wsStatus, wsData)
                if wsStatus and wsStatus > 0 and type(wsData) == 'table' then
                    if wsData.ok == false and not wsData.error then
                        wsData.error = wsData.text or wsData.raw or ('HTTP ' .. tostring(wsStatus))
                    end
                    done(wsStatus, wsData)
                else
                    httpFallbackRequest(method, path, payload, done)
                end
            end)
        else
            httpFallbackRequest(method, path, payload, done)
        end
    end

    if cb then
        dispatch(cb)
        return
    end

    local p = promise.new()
    dispatch(function(status, data)
        p:resolve({ status = status, data = data })
    end)
    local res = Citizen.Await(p)
    return res.data, res.status
end

-- ============================================================================
-- Real-Time WebSocket Event Bus Handlers (from bridge.js)
-- ============================================================================
AddEventHandler('eevs:internalConnectionState', function(connected)
    wsBridgeConnected = connected == true
    if wsBridgeConnected then
        print('^2[EEVS] Connected to EEVS real-time WebSocket bus (/ws)^7')
        -- Sync initial session & talking state
        eevsRequest('GET', '/sessions', nil, function(status, data)
            if status == 200 and data and type(data.sessions) == 'table' then
                talkingByNetId = {}
                sessionsByNetId = {}
                for _, s in ipairs(data.sessions) do
                    if s.netId then
                        sessionsByNetId[s.netId] = {
                            clientId = s.clientId,
                            netId = s.netId,
                            authenticated = s.authenticated == true
                        }
                        talkingByNetId[s.netId] = (s.talking == true)
                    end
                end
            end
        end)
    end
    TriggerEvent('eevs:connectionStateChanged', wsBridgeConnected)
end)

AddEventHandler('eevs:internalEvent', function(topic, subjectNetId, data)
    if topic == 'voice.talk_state' then
        local isTalking = (data and data.talking == true) or false
        talkingByNetId[subjectNetId] = isTalking
        TriggerEvent('eevs:talkStateChanged', subjectNetId, isTalking, data and data.clientId or 0)
    elseif topic == 'session.registered' then
        sessionsByNetId[subjectNetId] = {
            clientId = data and data.clientId or 0,
            netId = subjectNetId,
            authenticated = false
        }
        TriggerEvent('eevs:clientRegistered', subjectNetId, data and data.clientId or 0)
    elseif topic == 'session.authenticated' then
        sessionsByNetId[subjectNetId] = {
            clientId = data and data.clientId or 0,
            netId = subjectNetId,
            authenticated = true
        }
        TriggerEvent('eevs:clientAuthenticated', subjectNetId, data and data.clientId or 0)
    elseif topic == 'session.disconnected' then
        if sessionsByNetId[subjectNetId] then
            sessionsByNetId[subjectNetId].authenticated = false
        end
        talkingByNetId[subjectNetId] = false
        TriggerEvent('eevs:clientDisconnected', subjectNetId, data and data.clientId or 0, data and data.timeout == true)
    elseif topic == 'session.unregistered' then
        sessionsByNetId[subjectNetId] = nil
        talkingByNetId[subjectNetId] = nil
        TriggerEvent('eevs:clientUnregistered', subjectNetId, data and data.clientId or 0)
    end

    -- Generic event for any plugin topic
    TriggerEvent('eevs:event', topic, subjectNetId, data)
end)

-- ============================================================================
-- Cross-Language Exports (Lua, JS/TS, C#)
-- ============================================================================

--- @brief Send a raw HTTP/WebSocket control request to the EEVS control server.
--- @details Supports both callback mode (`cb(statusCode, data)`) and coroutine await mode
--- (`local data, statusCode = exports.eevs:Request(method, path, payload)` when `cb` is omitted).
--- @param method string HTTP verb (`"GET"`, `"POST"`).
--- @param path string Endpoint path (e.g. `"/health"`, `"/route/add"`, `"/effects/set"`).
--- @param payload? table Optional JSON-serializable request body table.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('Request', eevsRequest)

--- @brief Query EEVS server health, active session counts, and loaded C++ plugins (`GET /health`).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('GetHealth', function(cb)
    return eevsRequest('GET', '/health', nil, cb)
end)

--- @brief Retrieve all registered player voice sessions (`GET /sessions`).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('GetSessions', function(cb)
    return eevsRequest('GET', '/sessions', nil, cb)
end)

--- @brief Instant O(1) synchronous check whether a player is currently speaking (backed by `/ws` VAD events).
--- @param netId integer|string Player server ID / NetID.
--- @return boolean isTalking `true` if the player is actively transmitting voice frames.
exports('IsPlayerTalking', function(netId)
    return talkingByNetId[tonumber(netId) or 0] == true
end)

--- @brief Instant O(1) synchronous check whether a player has completed the voice server handshake.
--- @param netId integer|string Player server ID / NetID.
--- @return boolean isAuthenticated `true` if authenticated with EEVS.
exports('IsPlayerAuthenticated', function(netId)
    local s = sessionsByNetId[tonumber(netId) or 0]
    return (s and s.authenticated == true) or false
end)

-- ============================================================================
-- Asymmetric Routing & Custom Acoustic Matrix Exports
-- ============================================================================

--- @brief Add or merge a directed custom voice route from `fromNetId` to `toNetId` (`POST /route/add`).
--- @param fromNetId integer Speaker NetID / Server ID.
--- @param toNetId integer Listener NetID / Server ID.
--- @param mode? '"2D"'|'"3D"'|'"both"' Routing mode (`"2D"` default, `"3D"`, or `"both"`).
--- @param action? '"force"'|'"merge"'|fun(statusCode: integer, response: table) `"force"` (override FXServer route) or `"merge"` (OR with FXServer route).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('AddRoute', function(fromNetId, toNetId, mode, action, cb)
    if type(action) == 'function' then
        cb = action
        action = nil
    end
    return eevsRequest('POST', '/route/add', {
        fromNetId = tonumber(fromNetId),
        toNetId = tonumber(toNetId),
        mode = mode or '2D',
        action = action or 'force'
    }, cb)
end)

--- @brief Remove a custom route override between `fromNetId` and `toNetId`, reverting to FXServer's default sync (`POST /route/remove`).
--- @param fromNetId integer Speaker NetID / Server ID.
--- @param toNetId integer Listener NetID / Server ID.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('RemoveRoute', function(fromNetId, toNetId, cb)
    return eevsRequest('POST', '/route/remove', {
        fromNetId = tonumber(fromNetId),
        toNetId = tonumber(toNetId)
    }, cb)
end)

--- @brief Explicitly block voice from `fromNetId` to `toNetId` even if FXServer's spatial/channel matrix routes them (`POST /route/block`).
--- @param fromNetId integer Speaker NetID / Server ID.
--- @param toNetId integer Listener NetID / Server ID.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('BlockRoute', function(fromNetId, toNetId, cb)
    return eevsRequest('POST', '/route/block', {
        fromNetId = tonumber(fromNetId),
        toNetId = tonumber(toNetId)
    }, cb)
end)

--- @brief Clear all custom route overrides for a specific speaker, or across all speakers when `fromNetId` is nil (`POST /route/clear`).
--- @param fromNetId? integer|fun(statusCode: integer, response: table) Optional speaker NetID (or callback to clear all).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('ClearRoutes', function(fromNetId, cb)
    if type(fromNetId) == 'function' then
        cb = fromNetId
        fromNetId = nil
    end
    local payload = fromNetId and { fromNetId = tonumber(fromNetId) } or {}
    return eevsRequest('POST', '/route/clear', payload, cb)
end)

--- @brief Apply a batch of custom route mutations in a single atomic request (`POST /route/batch`).
--- @param routes table[] Array of `{ fromNetId: integer, toNetId: integer, action?: '"force"'|'"merge"'|'"block"'|'"clear"', mode?: '"2D"'|'"3D"' }`.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('ApplyBatchRoutes', function(routes, cb)
    return eevsRequest('POST', '/route/batch', {
        routes = routes
    }, cb)
end)

--- @brief Replace the custom listener lists for one or more speakers in a single atomic diff (`POST /route/sync`).
--- @param speakers table[] Array of `{ fromNetId: integer, targets: { toNetId: integer, mode?: '"2D"'|'"3D"', action?: '"force"'|'"merge"' }[] }`.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('SyncSpeakerRoutes', function(speakers, cb)
    return eevsRequest('POST', '/route/sync', {
        speakers = speakers
    }, cb)
end)

-- ============================================================================
-- Audio Injection & ClipManager Playback Exports
-- ============================================================================

--- @brief Inject a single hex-encoded 10ms Opus frame from `senderNetId` (`POST /audio/inject`).
--- @param senderNetId integer Sender NetID (player Server ID or networked NPC Ped NetID).
--- @param opusHex string Hex-encoded 48 kHz mono 10ms Opus payload.
--- @param mode? '"2D"'|'"3D"' Routing mode (`"3D"` default or `"2D"`).
--- @param listenerNetIds? integer[] Optional explicit list of listener Server IDs.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('InjectOpusFrame', function(senderNetId, opusHex, mode, listenerNetIds, cb)
    return eevsRequest('POST', '/audio/inject', {
        senderNetId = tonumber(senderNetId),
        opusHex = opusHex,
        mode = mode or '3D',
        listenerNetIds = listenerNetIds
    }, cb)
end)

--- @brief Queue a burst of hex-encoded 10ms Opus frames for paced 10ms timer playback (`POST /audio/burst`).
--- @param senderNetId integer Sender NetID (player Server ID or networked NPC Ped NetID).
--- @param framesHex string[] Array of hex-encoded 10ms Opus frames.
--- @param mode? '"2D"'|'"3D"' Routing mode (`"3D"` default or `"2D"`).
--- @param listenerNetIds? integer[] Optional explicit list of listener Server IDs.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('QueueOpusBurst', function(senderNetId, framesHex, mode, listenerNetIds, cb)
    return eevsRequest('POST', '/audio/burst', {
        senderNetId = tonumber(senderNetId),
        framesHex = framesHex,
        mode = mode or '3D',
        listenerNetIds = listenerNetIds
    }, cb)
end)

--- @brief List all audio clips currently stored in EEVS `ClipManager` RAM cache (`GET /clip/list`).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('ListClips', function(cb)
    return eevsRequest('GET', '/clip/list', nil, cb)
end)

--- @brief Start paced 10ms playback of a cached clip from `senderNetId` (`POST /clip/play`).
--- @param clipId string Clip identifier in `ClipManager`.
--- @param senderNetId integer Sender NetID (player Server ID or networked NPC Ped NetID).
--- @param mode? '"2D"'|'"3D"' Routing mode (`"3D"` default or `"2D"`).
--- @param listenerNetIds? integer[]|fun(statusCode: integer, response: table) Optional array of listener Server IDs (or callback).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('PlayClip', function(clipId, senderNetId, mode, listenerNetIds, cb)
    if type(listenerNetIds) == 'function' then
        cb = listenerNetIds
        listenerNetIds = nil
    end
    return eevsRequest('POST', '/clip/play', {
        clipId = tostring(clipId),
        senderNetId = tonumber(senderNetId),
        mode = mode or '3D',
        listenerNetIds = listenerNetIds
    }, cb)
end)

--- @brief Stop an active clip playback by `playbackId` or filter (`{ playbackId?, senderNetId?, clipId? }`) (`POST /clip/stop`).
--- @param playbackIdOrOpts integer|table Numeric `playbackId` or filter table (`{ senderNetId?: integer, clipId?: string }`).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('StopClip', function(playbackIdOrOpts, cb)
    local payload = type(playbackIdOrOpts) == 'table'
        and playbackIdOrOpts
        or { playbackId = tonumber(playbackIdOrOpts) }
    return eevsRequest('POST', '/clip/stop', payload, cb)
end)

--- @brief Delete a specific clip from EEVS `ClipManager` (`POST /clip/delete`).
--- @param clipId string Clip identifier to delete.
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('DeleteClip', function(clipId, cb)
    return eevsRequest('POST', '/clip/delete', {
        clipId = tostring(clipId)
    }, cb)
end)

--- @brief Delete all unpinned clips from EEVS `ClipManager` (`POST /clip/clear`).
--- @param cb? fun(statusCode: integer, response: table) Optional async completion callback.
--- @return table|nil response, integer|nil statusCode
exports('ClearClips', function(cb)
    return eevsRequest('POST', '/clip/clear', {}, cb)
end)

-- ============================================================================
-- FiveM Enhanced Voice Channel Wrappers & Mode 2 Custom Streaming Engine
-- ============================================================================
-- Wraps FiveM Enhanced's built-in Voice Channel natives (`CreateVoiceChannel`,
-- `DeleteVoiceChannel`, `AddPlayerToVoiceChannel`, `RemovePlayerFromVoiceChannel`,
-- `SetPlayerVoiceChannelMuted`, `SetPlayerVoiceChannelDeaf`) while adding:
--   1. Native support for `mode == 2` (Custom Streaming) synced via EEVS `/route/sync`
--   2. Channel metadata & DSP profiles (`name`, `routingMode`, `dsp`, `micClicks`, etc.)
--   3. One-way Admin / Dispatch Wiretapping (`WiretapVoiceChannel` / `UnwiretapVoiceChannel`)
--   4. Broadcasting synthetic/TTS clips to an entire channel (`PlayClipInVoiceChannel`)
--   5. Live channel & membership inspection (`GetVoiceChannels`, `GetPlayerVoiceChannels`)
-- ============================================================================

local voiceChannels = {} -- [channelId] = { id, mode, maxDistance, name, routingMode, metadata, members = { [src] = { muted, deaf } }, wiretaps = { [adminSrc] = modeStr } }
local playerProximityBySrc = {} -- [playerSrc] = { range = 0.0, nativeApplied = false }
local nextSyntheticChannelId = 10000

local function setPlayerVoiceProximityRangeWrapper(playerSrc, proximityRange)
    playerSrc = tonumber(playerSrc)
    proximityRange = tonumber(proximityRange) or 0.0
    if not playerSrc or playerSrc <= 0 then return false end

    if proximityRange < 0.0 then proximityRange = 0.0 end
    if proximityRange > 512.0 then proximityRange = 512.0 end

    local nativeApplied = false
    if type(SetPlayerVoiceProximityRange) == 'function' then
        local ok, res = pcall(SetPlayerVoiceProximityRange, tostring(playerSrc), proximityRange + 0.0)
        nativeApplied = (ok and res == true)
    end

    playerProximityBySrc[playerSrc] = {
        range = proximityRange,
        nativeApplied = nativeApplied
    }

    if type(Player) == 'function' then
        pcall(function()
            local ply = Player(playerSrc)
            if ply and ply.state then
                ply.state:set('eevs:proximityRange', proximityRange, true)
            end
        end)
    end

    TriggerEvent('eevs:playerProximityRangeChanged', playerSrc, proximityRange, nativeApplied)
    return nativeApplied
end

local function getPlayerVoiceProximityRangeWrapper(playerSrc)
    playerSrc = tonumber(playerSrc)
    local entry = playerSrc and playerProximityBySrc[playerSrc]
    if not entry then
        return 0.0, false
    end
    return entry.range, entry.nativeApplied
end

local function recomputeCustomChannelRoutes()
    local routesBySpeaker = {}

    for _, ch in pairs(voiceChannels) do
        local isCustomMode = (ch.mode == 2)
        local defaultEdgeMode = ch.routingMode or ((ch.mode == 1) and '3D' or '2D')

        -- 1. Mode 2 (Custom Streaming) full mesh between non-muted speakers and non-deaf listeners
        if isCustomMode then
            for spkSrc, spkState in pairs(ch.members) do
                if not spkState.muted then
                    routesBySpeaker[spkSrc] = routesBySpeaker[spkSrc] or {}
                    for lstSrc, lstState in pairs(ch.members) do
                        if lstSrc ~= spkSrc and not lstState.deaf then
                            routesBySpeaker[spkSrc][lstSrc] = defaultEdgeMode
                        end
                    end
                end
            end
        end

        -- 2. One-way channel wiretaps (supported across channel modes 0, 1, and 2)
        if next(ch.wiretaps) ~= nil then
            for spkSrc, spkState in pairs(ch.members) do
                if not spkState.muted then
                    routesBySpeaker[spkSrc] = routesBySpeaker[spkSrc] or {}
                    for tapListenerSrc, tapMode in pairs(ch.wiretaps) do
                        if tapListenerSrc ~= spkSrc then
                            routesBySpeaker[spkSrc][tapListenerSrc] = tapMode or '2D'
                        end
                    end
                end
            end
        end
    end

    local speakersPayload = {}
    for spkSrc, targetMap in pairs(routesBySpeaker) do
        local targets = {}
        for lstSrc, modeStr in pairs(targetMap) do
            table.insert(targets, { toNetId = lstSrc, mode = modeStr })
        end
        table.insert(speakersPayload, {
            fromNetId = spkSrc,
            targets = targets
        })
    end

    if #speakersPayload > 0 then
        eevsRequest('POST', '/route/sync', { speakers = speakersPayload }, function() end)
    end
end

local function createVoiceChannelWrapper(mode, maxDistance, options)
    mode = tonumber(mode) or 0
    maxDistance = tonumber(maxDistance) or 0.0
    options = type(options) == 'table' and options or {}

    local channelId = nil
    if type(CreateVoiceChannel) == 'function' then
        channelId = CreateVoiceChannel(mode, maxDistance)
    end
    if not channelId or channelId == 65535 or channelId < 0 then
        channelId = nextSyntheticChannelId
        nextSyntheticChannelId = nextSyntheticChannelId + 1
    end

    voiceChannels[channelId] = {
        id = channelId,
        mode = mode,
        maxDistance = maxDistance,
        name = options.name or ('Channel #' .. tostring(channelId)),
        routingMode = options.routingMode or ((mode == 1) and '3D' or '2D'),
        metadata = options,
        members = {},
        wiretaps = {}
    }

    TriggerEvent('eevs:voiceChannelCreated', channelId, mode, maxDistance, options)
    return channelId
end

local function deleteVoiceChannelWrapper(channelId)
    channelId = tonumber(channelId)
    if not channelId then return false end

    local ch = voiceChannels[channelId]
    local affectedSpeakers = {}
    if ch then
        for spkSrc, _ in pairs(ch.members) do
            table.insert(affectedSpeakers, spkSrc)
        end
        voiceChannels[channelId] = nil
    end

    if type(DeleteVoiceChannel) == 'function' and channelId < 10000 then
        DeleteVoiceChannel(channelId)
    end

    for _, spkSrc in ipairs(affectedSpeakers) do
        eevsRequest('POST', '/route/clear', { fromNetId = spkSrc }, function() end)
    end
    recomputeCustomChannelRoutes()
    TriggerEvent('eevs:voiceChannelDeleted', channelId)
    return true
end

local function addPlayerToVoiceChannelWrapper(channelId, playerSrc)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    if not channelId or not playerSrc then return false end

    if not voiceChannels[channelId] then
        voiceChannels[channelId] = {
            id = channelId,
            mode = 0,
            maxDistance = 0.0,
            name = 'Channel #' .. tostring(channelId),
            routingMode = '2D',
            metadata = {},
            members = {},
            wiretaps = {}
        }
    end

    local ch = voiceChannels[channelId]
    ch.members[playerSrc] = ch.members[playerSrc] or { muted = false, deaf = false }

    if type(AddPlayerToVoiceChannel) == 'function' and channelId < 10000 then
        AddPlayerToVoiceChannel(channelId, playerSrc)
    end

    if ch.mode == 2 or next(ch.wiretaps) ~= nil then
        recomputeCustomChannelRoutes()
    end

    TriggerEvent('eevs:playerJoinedVoiceChannel', channelId, playerSrc)
    return true
end

local function removePlayerFromVoiceChannelWrapper(channelId, playerSrc)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    if not channelId or not playerSrc then return false end

    local ch = voiceChannels[channelId]
    local wasCustomOrTapped = false
    if ch and ch.members[playerSrc] then
        wasCustomOrTapped = (ch.mode == 2 or next(ch.wiretaps) ~= nil)
        ch.members[playerSrc] = nil
    end

    if type(RemovePlayerFromVoiceChannel) == 'function' and channelId < 10000 then
        RemovePlayerFromVoiceChannel(channelId, playerSrc)
    end

    if wasCustomOrTapped then
        eevsRequest('POST', '/route/clear', { fromNetId = playerSrc }, function()
            recomputeCustomChannelRoutes()
        end)
    end

    TriggerEvent('eevs:playerLeftVoiceChannel', channelId, playerSrc)
    return true
end

local function setPlayerVoiceChannelMutedWrapper(channelId, playerSrc, muted)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    muted = (muted == true or muted == 1)
    if not channelId or not playerSrc then return false end

    local ch = voiceChannels[channelId]
    if ch and ch.members[playerSrc] then
        ch.members[playerSrc].muted = muted
    end

    if type(SetPlayerVoiceChannelMuted) == 'function' and channelId < 10000 then
        SetPlayerVoiceChannelMuted(channelId, playerSrc, muted)
    end

    if ch and (ch.mode == 2 or next(ch.wiretaps) ~= nil) then
        if muted then
            eevsRequest('POST', '/route/clear', { fromNetId = playerSrc }, function()
                recomputeCustomChannelRoutes()
            end)
        else
            recomputeCustomChannelRoutes()
        end
    end

    TriggerEvent('eevs:playerVoiceChannelMuteChanged', channelId, playerSrc, muted)
    return true
end

local function setPlayerVoiceChannelDeafWrapper(channelId, playerSrc, deaf)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    deaf = (deaf == true or deaf == 1)
    if not channelId or not playerSrc then return false end

    local ch = voiceChannels[channelId]
    if ch and ch.members[playerSrc] then
        ch.members[playerSrc].deaf = deaf
    end

    if type(SetPlayerVoiceChannelDeaf) == 'function' and channelId < 10000 then
        SetPlayerVoiceChannelDeaf(channelId, playerSrc, deaf)
    end

    if ch and ch.mode == 2 then
        recomputeCustomChannelRoutes()
    end

    TriggerEvent('eevs:playerVoiceChannelDeafChanged', channelId, playerSrc, deaf)
    return true
end

local function doesPlayerExistInVoiceChannelWrapper(channelId, playerSrc)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    if not channelId or not playerSrc then return false end

    local ch = voiceChannels[channelId]
    if ch and ch.members[playerSrc] ~= nil then
        return true
    end
    if type(DoesPlayerExistInVoiceChannel) == 'function' and channelId < 10000 then
        return DoesPlayerExistInVoiceChannel(channelId, playerSrc) == true
    end
    return false
end

local function isPlayerVoiceChannelMutedWrapper(channelId, playerSrc)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    local ch = channelId and voiceChannels[channelId]
    if ch and playerSrc and ch.members[playerSrc] then
        return ch.members[playerSrc].muted == true
    end
    if type(IsPlayerVoiceChannelMuted) == 'function' and channelId and channelId < 10000 then
        return IsPlayerVoiceChannelMuted(channelId, playerSrc) == true
    end
    return false
end

local function isPlayerVoiceChannelDeafWrapper(channelId, playerSrc)
    channelId = tonumber(channelId)
    playerSrc = tonumber(playerSrc)
    local ch = channelId and voiceChannels[channelId]
    if ch and playerSrc and ch.members[playerSrc] then
        return ch.members[playerSrc].deaf == true
    end
    if type(IsPlayerVoiceChannelDeaf) == 'function' and channelId and channelId < 10000 then
        return IsPlayerVoiceChannelDeaf(channelId, playerSrc) == true
    end
    return false
end

-- EEVS Channel Extensions: Wiretap a channel or play a clip to all members of a channel
local function wiretapVoiceChannelWrapper(channelId, listenerSrc, mode)
    channelId = tonumber(channelId)
    listenerSrc = tonumber(listenerSrc)
    if not channelId or not listenerSrc or not voiceChannels[channelId] then
        return false
    end
    voiceChannels[channelId].wiretaps[listenerSrc] = mode or '2D'
    recomputeCustomChannelRoutes()
    return true
end

local function unwiretapVoiceChannelWrapper(channelId, listenerSrc)
    channelId = tonumber(channelId)
    listenerSrc = tonumber(listenerSrc)
    if not channelId or not listenerSrc or not voiceChannels[channelId] then
        return false
    end
    voiceChannels[channelId].wiretaps[listenerSrc] = nil
    recomputeCustomChannelRoutes()
    return true
end

local function playClipInVoiceChannelWrapper(channelId, clipId, senderNetId, modeOverride, cb)
    if type(modeOverride) == 'function' then
        cb = modeOverride
        modeOverride = nil
    end
    channelId = tonumber(channelId)
    local ch = channelId and voiceChannels[channelId]
    if not ch then
        if cb then cb(404, { ok = false, error = 'Voice channel not found' }) end
        return nil
    end

    local listeners = {}
    local firstMember = nil
    for memberSrc, state in pairs(ch.members) do
        if not firstMember then firstMember = memberSrc end
        if not state.deaf then
            table.insert(listeners, memberSrc)
        end
    end
    for tapSrc, _ in pairs(ch.wiretaps) do
        table.insert(listeners, tapSrc)
    end

    local effectiveSender = tonumber(senderNetId) or firstMember or 1
    local effectiveMode = modeOverride or ch.routingMode or ((ch.mode == 1) and '3D' or '2D')
    return eevsRequest('POST', '/clip/play', {
        clipId = tostring(clipId),
        senderNetId = effectiveSender,
        mode = effectiveMode,
        listenerNetIds = listeners
    }, cb)
end

local function getVoiceChannelsWrapper()
    local list = {}
    for id, ch in pairs(voiceChannels) do
        local members = {}
        for src, st in pairs(ch.members) do
            table.insert(members, {
                serverId = src,
                muted = st.muted,
                deaf = st.deaf,
                talking = talkingByNetId[src] == true
            })
        end
        table.insert(list, {
            id = id,
            name = ch.name,
            mode = ch.mode,
            maxDistance = ch.maxDistance,
            routingMode = ch.routingMode,
            metadata = ch.metadata,
            members = members
        })
    end
    return list
end

local function getPlayerVoiceChannelsWrapper(playerSrc)
    playerSrc = tonumber(playerSrc)
    local result = {}
    if not playerSrc then return result end
    for id, ch in pairs(voiceChannels) do
        if ch.members[playerSrc] then
            table.insert(result, id)
        end
    end
    return result
end

AddEventHandler('playerDropped', function()
    local src = tonumber(source)
    if not src then return end
    playerProximityBySrc[src] = nil
    local needSync = false
    for _, ch in pairs(voiceChannels) do
        if ch.members[src] then
            ch.members[src] = nil
            if ch.mode == 2 or next(ch.wiretaps) ~= nil then
                needSync = true
            end
        end
        if ch.wiretaps[src] then
            ch.wiretaps[src] = nil
            needSync = true
        end
    end
    if needSync then
        recomputeCustomChannelRoutes()
    end
end)

--- @brief Set how far a player can be heard in spatial voice channels (`SetPlayerVoiceProximityRange`).
--- @param playerSrc integer|string Player server ID.
--- @param proximityRange number Range in meters (`0.0 .. 512.0`, where `0.0` uses the channel's max distance).
--- @return boolean nativeApplied `true` if the FXServer native succeeded.
exports('SetPlayerVoiceProximityRange', setPlayerVoiceProximityRangeWrapper)

--- @brief Get the last configured spatial voice proximity range for a player.
--- @param playerSrc integer|string Player server ID.
--- @return number proximityRange, boolean nativeApplied
exports('GetPlayerVoiceProximityRange', getPlayerVoiceProximityRangeWrapper)

--- @see EevsCreateVoiceChannel
exports('CreateVoiceChannel', createVoiceChannelWrapper)
--- @see EevsDeleteVoiceChannel
exports('DeleteVoiceChannel', deleteVoiceChannelWrapper)
--- @see EevsAddPlayerToVoiceChannel
exports('AddPlayerToVoiceChannel', addPlayerToVoiceChannelWrapper)
--- @see EevsRemovePlayerFromVoiceChannel
exports('RemovePlayerFromVoiceChannel', removePlayerFromVoiceChannelWrapper)
--- @see EevsSetPlayerVoiceChannelMuted
exports('SetPlayerVoiceChannelMuted', setPlayerVoiceChannelMutedWrapper)
--- @see EevsSetPlayerVoiceChannelDeaf
exports('SetPlayerVoiceChannelDeaf', setPlayerVoiceChannelDeafWrapper)
--- @see EevsDoesPlayerExistInVoiceChannel
exports('DoesPlayerExistInVoiceChannel', doesPlayerExistInVoiceChannelWrapper)
--- @see EevsIsPlayerVoiceChannelMuted
exports('IsPlayerVoiceChannelMuted', isPlayerVoiceChannelMutedWrapper)
--- @see EevsIsPlayerVoiceChannelDeaf
exports('IsPlayerVoiceChannelDeaf', isPlayerVoiceChannelDeafWrapper)
--- @see EevsWiretapVoiceChannel
exports('WiretapVoiceChannel', wiretapVoiceChannelWrapper)
--- @see EevsUnwiretapVoiceChannel
exports('UnwiretapVoiceChannel', unwiretapVoiceChannelWrapper)
--- @see EevsPlayClipInVoiceChannel
exports('PlayClipInVoiceChannel', playClipInVoiceChannelWrapper)

--- @brief Return a snapshot of all active EEVS-tracked voice channels and their members.
--- @return table[] channels
exports('GetVoiceChannels', getVoiceChannelsWrapper)

--- @brief Return an array of channel IDs that `playerSrc` currently belongs to.
--- @param playerSrc integer|string Player server ID.
--- @return integer[] channelIds
exports('GetPlayerVoiceChannels', getPlayerVoiceChannelsWrapper)
