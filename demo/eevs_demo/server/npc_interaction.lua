-- ============================================================================
-- EEVS Demo - Server NPC Interaction Session Manager & Dynamic 3D Proximity
-- ============================================================================

local activeSpeakingNpcBroadcasts = {}
local npcRouteSyncLoopRunning = false
local DEFAULT_AUDIO_STREAM_RADIUS = 45.0

local function computeNearbyRouteTargetsForNpc(npcPedNetId, fallbackSrc, radius)
    radius = radius or DEFAULT_AUDIO_STREAM_RADIUS
    local originCoords = nil

    if npcPedNetId and tonumber(npcPedNetId) and tonumber(npcPedNetId) > 0 then
        local ent = NetworkGetEntityFromNetworkId(tonumber(npcPedNetId))
        if ent and ent ~= 0 and DoesEntityExist(ent) then
            originCoords = GetEntityCoords(ent)
        end
    end

    if not originCoords and fallbackSrc and fallbackSrc > 0 then
        local pPed = GetPlayerPed(fallbackSrc)
        if pPed and pPed ~= 0 and DoesEntityExist(pPed) then
            originCoords = GetEntityCoords(pPed)
        end
    end

    local targets = {}
    for _, pidStr in ipairs(GetPlayers()) do
        local pid = tonumber(pidStr)
        if pid then
            if not originCoords then
                table.insert(targets, { toNetId = pid, mode = '3D' })
            else
                local ped = GetPlayerPed(pid)
                if ped and ped ~= 0 and DoesEntityExist(ped) then
                    if #(GetEntityCoords(ped) - originCoords) <= radius then
                        table.insert(targets, { toNetId = pid, mode = '3D' })
                    end
                end
            end
        end
    end

    return targets
end

local function ensureNpcProximityRouteLoop()
    if npcRouteSyncLoopRunning then
        return
    end
    npcRouteSyncLoopRunning = true
    CreateThread(function()
        while true do
            local now = GetGameTimer()
            local speakersPayload = {}
            local anyActive = false

            for pedNetId, info in pairs(activeSpeakingNpcBroadcasts) do
                if now >= info.expiresAt then
                    activeSpeakingNpcBroadcasts[pedNetId] = nil
                    table.insert(speakersPayload, {
                        fromNetId = pedNetId,
                        targets = {}
                    })
                else
                    anyActive = true
                    table.insert(speakersPayload, {
                        fromNetId = pedNetId,
                        targets = computeNearbyRouteTargetsForNpc(pedNetId, info.fallbackSrc, info.radius or DEFAULT_AUDIO_STREAM_RADIUS)
                    })
                end
            end

            if #speakersPayload > 0 then
                DemoServer.EevsRequest('POST', '/route/sync', { speakers = speakersPayload }, function() end)
            end

            if not anyActive then
                npcRouteSyncLoopRunning = false
                break
            end
            Wait(250)
        end
    end)
end

function DemoServer.StartDynamicNpcProximityBroadcast(npcPedNetId, fallbackSrc, durationMs, customRadius, readyCb)
    if type(customRadius) == 'function' then
        readyCb = customRadius
        customRadius = nil
    end
    local pedNet = tonumber(npcPedNetId) or 0
    if pedNet <= 0 then
        if readyCb then readyCb() end
        return
    end

    local radius = tonumber(customRadius) or DEFAULT_AUDIO_STREAM_RADIUS
    local dur = math.max(1000, tonumber(durationMs) or 3000)
    activeSpeakingNpcBroadcasts[pedNet] = {
        expiresAt = GetGameTimer() + dur + 500,
        fallbackSrc = fallbackSrc,
        radius = radius
    }

    local initialTargets = computeNearbyRouteTargetsForNpc(pedNet, fallbackSrc, radius)
    DemoServer.EevsRequest('POST', '/route/sync', {
        speakers = {
            {
                fromNetId = pedNet,
                targets = initialTargets
            }
        }
    }, function()
        ensureNpcProximityRouteLoop()
        if readyCb then readyCb() end
    end)
end

function DemoServer.UpdateActiveNpcBroadcastExpiry(npcPedNetId, fallbackSrc, durationMs, customRadius)
    local pedNet = tonumber(npcPedNetId) or 0
    if pedNet <= 0 then return end
    activeSpeakingNpcBroadcasts[pedNet] = {
        expiresAt = GetGameTimer() + (tonumber(durationMs) or 2500) + 500,
        fallbackSrc = fallbackSrc,
        radius = tonumber(customRadius) or DEFAULT_AUDIO_STREAM_RADIUS
    }
end

-- Synthesizes speech via EEVS Voice Design (`/vdes/generate`) and plays it back using an options table
function DemoServer.SpeakReplyViaEevs(src, replyText, actionName, actionArgs, opts, cb)
    opts = type(opts) == 'table' and opts or {}
    actionName = actionName or 'none'
    actionArgs = type(actionArgs) == 'table' and actionArgs or {}
    replyText = (replyText or ''):gsub('^%s*(.-)%s*$', '%1')

    local npcId = opts.npcId and tonumber(opts.npcId) or nil
    local npcPedNetId = opts.npcPedNetId and tonumber(opts.npcPedNetId) or nil
    local broadcastScope = (type(opts.broadcastScope) == 'string' and opts.broadcastScope ~= '') and opts.broadcastScope or '3d_nearby'
    local effectiveRadius = tonumber(opts.hearingRadius) or DEFAULT_AUDIO_STREAM_RADIUS

    if replyText == '' then
        if npcId and actionName ~= 'none' then
            TriggerClientEvent('eevs_demo:npcExecuteAction', src, npcId, actionName, actionArgs)
        end
        cb(200, {
            ok = true,
            backend = 'llm + silent action',
            reply = '',
            action = actionName,
            actionArgs = actionArgs,
            clipId = '',
            playbackId = 0,
            durationMs = 0
        })
        return
    end

    DemoServer.EevsRequest('POST', '/vdes/generate', {
        text = replyText,
        voice = opts.voice or 'default',
        instruct = (type(opts.voiceInstruct) == 'string' and opts.voiceInstruct ~= '') and opts.voiceInstruct or nil,
        seed = tonumber(opts.voiceSeed) or nil,
        gain = tonumber(opts.voiceGain) or 1.0,
        options = opts.options
    }, function(ttsStatus, ttsData)
        if ttsStatus ~= 200 or not ttsData or not ttsData.ok or not ttsData.clipId then
            if npcId and actionName ~= 'none' then
                TriggerClientEvent('eevs_demo:npcExecuteAction', src, npcId, actionName, actionArgs)
            end
            cb(ttsStatus or 500, {
                ok = false,
                reply = replyText,
                action = actionName,
                actionArgs = actionArgs,
                error = (ttsData and ttsData.error) or 'Voice Design generation failed'
            })
            return
        end

        local hasNpcNetId = (npcPedNetId and npcPedNetId > 0)
        local effectiveSenderNetId = hasNpcNetId and npcPedNetId or src
        local playMode = '3D'
        local listeners = nil

        if broadcastScope == '2d_private' then
            playMode = '2D'
            listeners = { src }
        elseif broadcastScope == '2d_all' then
            playMode = '2D'
            listeners = nil
        elseif broadcastScope == '3d_private' then
            playMode = '3D'
            listeners = { src }
        else
            playMode = npcId and '3D' or (opts.mode or '3D')
            listeners = (playMode == '2D') and { src } or nil
        end

        local clipDurMs = ttsData.durationMs or 2500
        local bubbleTarget = (broadcastScope == '3d_private' or broadcastScope == '2d_private') and src or -1

        local function triggerPlayback()
            EEVS.PlayClip(ttsData.clipId, effectiveSenderNetId, playMode, listeners, function(_playStatus, playRes)
                if npcId then
                    if actionName and actionName ~= 'none' then
                        TriggerClientEvent('eevs_demo:npcExecuteAction', src, npcId, actionName, actionArgs)
                    end
                    TriggerClientEvent('eevs_demo:npcSpeaking', bubbleTarget, {
                        npcId = npcId,
                        clipId = ttsData.clipId,
                        durationMs = clipDurMs,
                        actionName = 'none',
                        speechText = replyText,
                        npcPedNetId = effectiveSenderNetId,
                        ownerServerId = src,
                        hearingRadius = effectiveRadius
                    })
                else
                    TriggerClientEvent('eevs_demo:showSpeechBubble', bubbleTarget, src, '[AI] ' .. replyText)
                end
                cb(200, {
                    ok = true,
                    backend = 'llm + ' .. tostring(ttsData.backend or 'audio.cpp (vdes)'),
                    reply = replyText,
                    action = actionName,
                    actionArgs = actionArgs,
                    clipId = ttsData.clipId,
                    playbackId = type(playRes) == 'table' and playRes.playbackId or 0,
                    durationMs = clipDurMs
                })
            end)
        end

        if npcId and hasNpcNetId and broadcastScope == '3d_nearby' then
            DemoServer.StartDynamicNpcProximityBroadcast(effectiveSenderNetId, src, clipDurMs, effectiveRadius, triggerPlayback)
        else
            triggerPlayback()
        end
    end)
end

-- Initialize default 3D Spatial voice channel (Mode 1, 20.0m range) using EEVS Voice Channel Wrapper
CreateThread(function()
    DemoServer.defaultProximityChannel = EevsCreateVoiceChannel(1, 20.0, { name = 'Default Proximity (20m)' })
    if DemoServer.defaultProximityChannel and DemoServer.defaultProximityChannel ~= 65535 then
        for _, playerSrc in ipairs(GetPlayers()) do
            local numSrc = tonumber(playerSrc)
            if numSrc then
                EevsAddPlayerToVoiceChannel(DemoServer.defaultProximityChannel, numSrc)
            end
        end
    end
end)

AddEventHandler('playerJoining', function()
    local src = source
    if DemoServer.defaultProximityChannel and DemoServer.defaultProximityChannel ~= 65535 then
        EevsAddPlayerToVoiceChannel(DemoServer.defaultProximityChannel, src)
    end
end)

function DemoServer.StopNpcInteractionForPlayer(src, reason, notifyClient)
    local session = DemoServer.activeNpcInteractionByPlayer[src]
    if not session then
        return
    end
    DemoServer.activeNpcInteractionByPlayer[src] = nil

    DemoServer.EevsRequest('POST', '/stt/stop', { netId = src, wait = false }, function() end)
    EEVS.StopClip({ senderNetId = src }, function() end)

    if notifyClient then
        TriggerClientEvent('eevs_demo:npcThinking', src, session.npcId, false, nil)
        TriggerClientEvent('eevs_demo:npcInteractionState', src, false, session.npcId, reason or 'stopped')
    end
end

AddEventHandler('playerDropped', function()
    local src = source
    DemoServer.lastRecordedClipByPlayer[src] = nil
    DemoServer.StopNpcInteractionForPlayer(src, 'disconnected', false)
end)

local function runNpcConversationTurn(src, session, userPrompt, displayTranscript)
    DemoServer.QueryLlmApi(userPrompt, session.systemPrompt, session.llmModel, session.enabledActions, function(llmOk, replyOrErr, actionName, actionArgs)
        local activeNow = DemoServer.activeNpcInteractionByPlayer[src]
        if not activeNow or activeNow.npcId ~= session.npcId then
            return
        end
        if not llmOk then
            activeNow.busy = false
            TriggerClientEvent('eevs_demo:npcThinking', src, activeNow.npcId, false, nil)
            DemoServer.NotifyPlayer(src, ('NPC LLM failed: %s'):format(tostring(replyOrErr)), true)
            return
        end
        if replyOrErr ~= '' then
            TriggerClientEvent('eevs_demo:npcThinking', src, activeNow.npcId, true, 'Generating audio...')
        end
        local vdesOptions = activeNow.vdesModel and { model = activeNow.vdesModel } or nil
        DemoServer.SpeakReplyViaEevs(src, replyOrErr, actionName, actionArgs, {
            npcId = activeNow.npcId,
            npcPedNetId = activeNow.pedNetId,
            voice = activeNow.voice,
            voiceInstruct = activeNow.voiceInstruct,
            voiceSeed = activeNow.voiceSeed,
            voiceGain = activeNow.voiceGain,
            broadcastScope = activeNow.broadcastScope,
            hearingRadius = activeNow.hearingRadius,
            mode = '3D',
            options = vdesOptions
        }, function(_status, data)
            local stillActive = DemoServer.activeNpcInteractionByPlayer[src]
            if stillActive then
                stillActive.busy = false
            end
            TriggerClientEvent('eevs_demo:npcThinking', src, activeNow.npcId, false, nil)
            if data and data.ok then
                TriggerClientEvent('eevs_demo:pushEvent', src, 'npc.converse_turn', src, {
                    npcId = activeNow.npcId,
                    transcript = displayTranscript,
                    reply = replyOrErr,
                    action = actionName,
                    actionArgs = actionArgs,
                    clipId = data.clipId,
                    durationMs = data.durationMs,
                    backend = data.backend
                })
            end
        end)
    end)
end

RegisterNetEvent('eevs_demo:npcInteractStart', function(cfg)
    local src = source
    if type(cfg) ~= 'table' then return end

    local numNpcId = tonumber(cfg.npcId) or 1
    local session = {
        npcId = numNpcId,
        pedNetId = tonumber(cfg.pedNetId) or 0,
        label = tostring(cfg.label or ('NPC #' .. tostring(numNpcId))),
        systemPrompt = (type(cfg.systemPrompt) == 'string' and cfg.systemPrompt ~= '') and cfg.systemPrompt or nil,
        voice = (type(cfg.voice) == 'string' and cfg.voice ~= '') and cfg.voice or 'default',
        voiceInstruct = (type(cfg.voiceInstruct) == 'string' and cfg.voiceInstruct ~= '') and cfg.voiceInstruct or nil,
        voiceSeed = tonumber(cfg.voiceSeed) or nil,
        voiceGain = tonumber(cfg.voiceGain) or 1.0,
        broadcastScope = (type(cfg.broadcastScope) == 'string' and cfg.broadcastScope ~= '') and cfg.broadcastScope or '3d_nearby',
        hearingRadius = tonumber(cfg.hearingRadius) or DEFAULT_AUDIO_STREAM_RADIUS,
        llmModel = (type(cfg.llmModel) == 'string' and cfg.llmModel ~= '') and cfg.llmModel or nil,
        vdesModel = (type(cfg.vdesModel) == 'string' and cfg.vdesModel ~= '') and cfg.vdesModel or nil,
        sttModel = (type(cfg.sttModel) == 'string' and cfg.sttModel ~= '') and cfg.sttModel or nil,
        enabledActions = (type(cfg.enabledActions) == 'table') and cfg.enabledActions or nil,
        busy = false
    }
    DemoServer.activeNpcInteractionByPlayer[src] = session
    TriggerClientEvent('eevs_demo:npcInteractionState', src, true, numNpcId, 'started')
    TriggerClientEvent(
        'eevs_demo:notify',
        src,
        ('Interacting with %s (seed=%s) — Hold [N] and speak! Press [E] or walk away to stop.'):format(
            session.label,
            tostring(session.voiceSeed or 'auto')
        )
    )

    if cfg.initiateFirst == true then
        session.busy = true
        TriggerClientEvent('eevs_demo:npcThinking', src, session.npcId, true, 'Thinking...')
        local openingPrompt = '[The player just walked up to you and initiated an interaction. Speak first in-character to greet, question, or confront them based on your personality.]'
        runNpcConversationTurn(src, session, openingPrompt, '(Approached NPC)')
    end
end)

RegisterNetEvent('eevs_demo:npcUpdateSession', function(patch)
    local src = source
    local session = DemoServer.activeNpcInteractionByPlayer[src]
    if not session or type(patch) ~= 'table' then
        return
    end
    if patch.npcId and session.npcId ~= tonumber(patch.npcId) then
        return
    end
    if patch.llmModel ~= nil then
        session.llmModel = (type(patch.llmModel) == 'string' and patch.llmModel ~= '') and patch.llmModel or nil
    end
    if patch.sttModel ~= nil then
        session.sttModel = (type(patch.sttModel) == 'string' and patch.sttModel ~= '') and patch.sttModel or nil
    end
    if patch.vdesModel ~= nil then
        session.vdesModel = (type(patch.vdesModel) == 'string' and patch.vdesModel ~= '') and patch.vdesModel or nil
    end
    if type(patch.voiceInstruct) == 'string' and patch.voiceInstruct ~= '' then
        session.voiceInstruct = patch.voiceInstruct
    end
    if tonumber(patch.voiceSeed) then
        session.voiceSeed = tonumber(patch.voiceSeed)
    end
    if tonumber(patch.voiceGain) then
        session.voiceGain = tonumber(patch.voiceGain)
    end
    if type(patch.enabledActions) == 'table' then
        session.enabledActions = patch.enabledActions
    end
    if type(patch.broadcastScope) == 'string' and patch.broadcastScope ~= '' then
        session.broadcastScope = patch.broadcastScope
    end
    if tonumber(patch.hearingRadius) then
        session.hearingRadius = tonumber(patch.hearingRadius)
    end
end)

RegisterNetEvent('eevs_demo:npcInteractStop', function(reason)
    local src = source
    local session = DemoServer.activeNpcInteractionByPlayer[src]
    if session then
        local label = session.label
        DemoServer.StopNpcInteractionForPlayer(src, reason or 'manual', true)
        TriggerClientEvent(
            'eevs_demo:notify',
            src,
            ('Stopped interaction with %s (%s)'):format(label, tostring(reason or 'manual'))
        )
    end
end)

-- ============================================================================
-- Real-Time EEVS WebSocket Push Events -> Broadcast & Hands-Free NPC PTT
-- ============================================================================

EEVS.OnTalkState(function(netId, talking, clientId)
    TriggerClientEvent('eevs_demo:pushEvent', -1, 'voice.talk_state', netId, {
        netId = netId,
        clientId = clientId,
        talking = talking
    })

    local playerSrc = (clientId and clientId > 0 and DemoServer.activeNpcInteractionByPlayer[clientId]) and clientId or netId
    local session = DemoServer.activeNpcInteractionByPlayer[playerSrc]
    if not session or session.busy then
        return
    end

    local sttOptions = session.sttModel and { model = session.sttModel } or nil
    if talking then
        DemoServer.EevsRequest('POST', '/stt/start', {
            netId = playerSrc,
            language = 'en',
            isolate = false,
            options = sttOptions
        }, function() end)
    else
        session.busy = true
        TriggerClientEvent('eevs_demo:npcThinking', playerSrc, session.npcId, true, 'Transcribing speech...')
        DemoServer.EevsRequest('POST', '/stt/stop', {
            netId = playerSrc,
            wait = true,
            options = sttOptions
        }, function(sttStatus, sttData)
            local currentSession = DemoServer.activeNpcInteractionByPlayer[playerSrc]
            if not currentSession then
                return
            end

            if sttStatus ~= 200 or not sttData or not sttData.ok or not sttData.text or sttData.text == '' then
                currentSession.busy = false
                TriggerClientEvent('eevs_demo:npcThinking', playerSrc, currentSession.npcId, false, nil)
                return
            end

            local transcript = sttData.text
            TriggerClientEvent('eevs_demo:showSpeechBubble', -1, playerSrc, transcript)
            TriggerClientEvent('eevs_demo:npcThinking', playerSrc, currentSession.npcId, true, 'Thinking...')
            runNpcConversationTurn(playerSrc, currentSession, transcript, transcript)
        end)
    end
end)

EEVS.OnEvent(function(topic, netId, data)
    if topic == 'voice.talk_state' then
        return
    end
    TriggerClientEvent('eevs_demo:pushEvent', -1, topic, netId, data)
    if topic == 'stt.completed' and data and data.text and data.text ~= '' and netId and netId > 0 then
        TriggerClientEvent('eevs_demo:showSpeechBubble', -1, netId, data.text)
    end
end)
