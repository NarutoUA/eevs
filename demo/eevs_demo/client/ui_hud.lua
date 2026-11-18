-- ============================================================================
-- EEVS Demo - Client NUI Bridge, Commands, HUD & 3D Render Loop
-- ============================================================================

local pendingCallbacks = {}
local nextReqId = 1

local function setUiOpen(open)
    DemoClient.uiOpen = open
    SetNuiFocus(open, open)
    SetNuiFocusKeepInput(open)
    SendNUIMessage({
        type = 'toggleUi',
        open = open,
        serverId = GetPlayerServerId(PlayerId()),
        hudState = DemoClient.hudState,
        npcCatalog = open and GetNpcCatalogPayload() or nil
    })
end

local function setAudioCppUiOpen(open, customUrl)
    DemoClient.uiOpen = open
    SetNuiFocus(open, open)
    SetNuiFocusKeepInput(false)
    SendNUIMessage({
        type = 'toggleAudioCppUi',
        open = open,
        url = customUrl,
        serverId = GetPlayerServerId(PlayerId())
    })
end

RegisterCommand('eevs', function()
    setUiOpen(not DemoClient.uiOpen)
end, false)

RegisterCommand('audio_cpp', function(_source, args)
    setAudioCppUiOpen(true, args and args[1] or nil)
end, false)

RegisterKeyMapping('eevs', 'Open EEVS Voice Sandbox Panel', 'keyboard', 'F7')

RegisterCommand('eevs_help', function()
    local helpLines = {
        '^3================ EEVS Voice Commands & Hotkeys ================^7',
        '^5/eevs^7 (or ^5[F7]^7) - Open interactive EEVS NUI Sandbox & Visualizer (Mic PTT works while open!)',
        '^5/audio_cpp [url]^7 - Open embedded audio.cpp Server WebUI & Model Manager inside FiveM',
        '^5Hold [J]^7 - Push-to-Transcribe (STT) hotkey',
        '^5/eevs_echo [2d|3d|off]^7 - Route your own voice back to yourself for solo testing',
        '^5/eevs_rec start [clipId] [isolate]^7 - Start recording your microphone (hold PTT [N] to speak)',
        '^5/eevs_rec stop^7 - Stop recording and save clip into ClipManager',
        '^5/eevs_rec play [clipId] [2d|3d]^7 - Play back recorded clip (supports simultaneous multi-clip mix)',
        '^5/eevs_rec stop_play^7 - Stop all active clip playbacks on your NetID',
        '^5/eevs_rec wav [clipId] [file.wav]^7 - Export recorded clip to disk as .wav',
        '^5/eevs_rec eevs [clipId] [file.eevs]^7 - Export recorded clip to disk as .eevs Opus container',
        '^5/eevs_rec load <file.wav|file.eevs> [clipId]^7 - Load clip from disk into ClipManager',
        '^5/eevs_stt <start|stop> [lang] [isolate]^7 - Start/stop speech-to-text capture (audio.cpp)',
        '^5/eevs_tts <2d|3d> <text...>^7 - Synthesize and speak text from your character (audio.cpp)',
        '^5/eevs_wiretap <add|merge|remove|block|clear_all> <targetServerId> [2d|3d]^7 - Mutate custom routing graph',
        '^5/eevs_fx <preset|off> [all|2d|3d]^7 - Apply real-time Signalsmith DSP effect (mask, tunnel_phone, megaphone, robot...)',
        '^5/eevs_range [whisper|normal|shout|megaphone|default]^7 (or ^5[Z]^7) - Cycle talker proximity range'
    }
    for _, line in ipairs(helpLines) do
        TriggerEvent('chat:addMessage', { args = { line } })
        print(line:gsub('%^%d', ''))
    end
end, false)

RegisterCommand('eevs_cycle_range', function()
    TriggerServerEvent('eevs_demo:cycleProximityRange')
end, false)

RegisterKeyMapping('eevs_cycle_range', 'Cycle EEVS Talker Proximity Range', 'keyboard', 'Z')

RegisterCommand('+eevs_ptt', function()
    if DemoClient.uiOpen then return end
    ExecuteCommand('eevs_stt start en')
end, false)

RegisterCommand('-eevs_ptt', function()
    if DemoClient.uiOpen then return end
    ExecuteCommand('eevs_stt stop')
end, false)

RegisterKeyMapping('+eevs_ptt', 'EEVS Push-to-Transcribe (STT)', 'keyboard', 'J')

-- Server notifications
RegisterNetEvent('eevs_demo:notify', function(message)
    local text = tostring(message)
    local lower = string.lower(text)
    if lower:find('fail', 1, true) or lower:find('error', 1, true) then
        print('[EEVS] [ERROR] ' .. text)
    end

    BeginTextCommandThefeedPost('STRING')
    AddTextComponentSubstringPlayerName('~b~[EEVS]~s~ ' .. text)
    EndTextCommandThefeedPostTicker(false, false)

    SendNUIMessage({
        type = 'log',
        message = text
    })
end)

RegisterNetEvent('eevs_demo:stateUpdate', function(patch)
    for k, v in pairs(patch) do
        DemoClient.hudState[k] = v
    end
    SendNUIMessage({
        type = 'stateUpdate',
        hudState = DemoClient.hudState
    })
end)

RegisterNetEvent('eevs_demo:pushEvent', function(topic, netId, data)
    SendNUIMessage({
        type = 'eevsEvent',
        topic = topic,
        netId = netId,
        data = data
    })
end)

RegisterNetEvent('eevs_demo:showSpeechBubble', function(speakerServerId, text)
    DemoClient.activeBubbles[speakerServerId] = {
        text = tostring(text),
        expiresAt = GetGameTimer() + 6000
    }
end)

DemoClient.proximityRing = nil
RegisterNetEvent('eevs_demo:showProximityRing', function(rangeMeters, modeName)
    local r = tonumber(rangeMeters) or 0.0
    if r <= 0.0 then
        DemoClient.proximityRing = nil
        return
    end
    DemoClient.proximityRing = {
        range = r,
        mode = tostring(modeName or 'normal'),
        expiresAt = GetGameTimer() + 2400
    }
end)

-- ============================================================================
-- NUI -> Client -> Server RPC Bridge
-- ============================================================================

RegisterNUICallback('close', function(_data, cb)
    DemoClient.uiOpen = false
    SetNuiFocus(false, false)
    SetNuiFocusKeepInput(false)
    cb({ ok = true })
end)

RegisterNUICallback('setKeepInput', function(data, cb)
    if DemoClient.uiOpen then
        SetNuiFocusKeepInput(data and data.keepInput == true)
    end
    cb({ ok = true })
end)

RegisterNUICallback('npcAction', function(data, cb)
    local action = data and data.action or 'list'
    if action == 'spawn' then
        DemoClient.ApplyConfigPatch(data)
        local npc, err = DemoClient.SpawnNpcInFront(data)
        if not npc then
            cb({ ok = false, error = err or 'Failed to spawn NPC' })
            return
        end
        cb({
            ok = true,
            npc = {
                npcId = npc.npcId,
                pedNetId = npc.pedNetId,
                label = npc.label,
                modelName = npc.modelName,
                voiceSeed = npc.voiceSeed,
                voiceGain = npc.voiceGain,
                voiceInstruct = npc.voiceInstruct
            },
            npcs = DemoClient.GetNpcSummaryList()
        })
    elseif action == 'setNpcGain' then
        local targetId = tonumber(data and data.npcId)
        local newGain = tonumber(data and data.voiceGain) or 1.0
        local npc = DemoClient.GetNpcById(targetId)
        if npc then
            npc.voiceGain = newGain
            if DemoClient.activeInteractingNpcId == npc.npcId then
                TriggerServerEvent('eevs_demo:npcUpdateSession', {
                    npcId = npc.npcId,
                    voiceGain = newGain
                })
            end
        end
        cb({ ok = npc ~= nil, npcs = DemoClient.GetNpcSummaryList() })
    elseif action == 'delete' then
        local deleted = DemoClient.DeleteSpawnedNpcById(data and data.npcId)
        cb({ ok = deleted, npcs = DemoClient.GetNpcSummaryList() })
    elseif action == 'clear' then
        local removed = DemoClient.ClearAllSpawnedNpcs()
        cb({ ok = true, removed = removed, npcs = {} })
    elseif action == 'setConfig' then
        DemoClient.ApplyConfigPatch(data)
        if DemoClient.activeInteractingNpcId ~= nil then
            local cfg = DemoClient.config
            TriggerServerEvent('eevs_demo:npcUpdateSession', {
                llmModel = cfg.llmModel,
                sttModel = cfg.sttModel,
                vdesModel = cfg.vdesModel,
                voiceInstruct = cfg.voiceInstruct,
                voiceSeed = cfg.voiceSeed,
                voiceGain = cfg.voiceGain,
                enabledActions = cfg.enabledActions,
                broadcastScope = cfg.broadcastScope,
                hearingRadius = cfg.hearingRadius
            })
        end
        cb({ ok = true })
    else
        cb({
            ok = true,
            npcs = DemoClient.GetNpcSummaryList(),
            catalog = GetNpcCatalogPayload()
        })
    end
end)

RegisterNUICallback('apiRequest', function(data, cb)
    local reqId = nextReqId
    nextReqId = reqId + 1
    pendingCallbacks[reqId] = cb

    local body = data.body or {}
    if type(body) == 'table' and body.npcId then
        local npc = DemoClient.GetNpcById(body.npcId)
        if npc then
            body.npcPedNetId = npc.pedNetId
            if npc.voiceSeed then body.voiceSeed = npc.voiceSeed end
            if npc.voiceGain then body.voiceGain = npc.voiceGain end
            if npc.voiceInstruct and npc.voiceInstruct ~= '' then body.voiceInstruct = npc.voiceInstruct end
            if type(npc.enabledActions) == 'table' and not body.enabledActions then
                body.enabledActions = npc.enabledActions
            end
        end
    end

    TriggerServerEvent(
        'eevs_demo:nuiRequest',
        reqId,
        data.method or 'POST',
        data.path or '/health',
        body
    )
end)

RegisterNetEvent('eevs_demo:nuiResponse', function(reqId, status, responseData)
    local cb = pendingCallbacks[reqId]
    if cb then
        pendingCallbacks[reqId] = nil
        cb({
            status = status,
            data = responseData
        })
    end
end)

-- ============================================================================
-- Main Render & Proximity Loop (3D Speech Bubbles, NPCs & HUD)
-- ============================================================================

CreateThread(function()
    while true do
        local now = GetGameTimer()
        local talkingNow = NetworkIsPlayerTalking(PlayerId()) == 1 or NetworkIsPlayerTalking(PlayerId()) == true

        if talkingNow ~= DemoClient.isLocalTalking then
            DemoClient.isLocalTalking = talkingNow
            SendNUIMessage({
                type = 'talkingState',
                talking = DemoClient.isLocalTalking
            })
        end

        if DemoClient.uiOpen then
            DisableAllControlActions(0)
            DisableAllControlActions(1)
            DisableAllControlActions(2)
            EnableControlAction(0, 249, true) -- INPUT_PUSH_TO_TALK [N]
            EnableControlAction(1, 249, true)
            EnableControlAction(2, 249, true)
        end

        local cfg = DemoClient.config
        local hasActiveVisual = DemoClient.uiOpen
            or DemoClient.isLocalTalking
            or (DemoClient.hudState.echo ~= 'off')
            or DemoClient.hudState.recording
            or DemoClient.hudState.sttListening
            or (DemoClient.proximityRing ~= nil and now < DemoClient.proximityRing.expiresAt)

        -- 0. Draw temporary 3D proximity range cylinder around player when cycling range ([Z])
        if DemoClient.proximityRing then
            if now >= DemoClient.proximityRing.expiresAt then
                DemoClient.proximityRing = nil
            else
                local ped = PlayerPedId()
                local pos = GetEntityCoords(ped)
                local diam = DemoClient.proximityRing.range * 2.0
                local mode = DemoClient.proximityRing.mode
                local cr, cg, cb = 56, 189, 248
                if mode == 'normal' then
                    cr, cg, cb = 52, 211, 153
                elseif mode == 'shout' then
                    cr, cg, cb = 251, 191, 36
                elseif mode == 'megaphone' then
                    cr, cg, cb = 244, 63, 94
                end
                DrawMarker(
                    1,
                    pos.x, pos.y, pos.z - 0.95,
                    0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0,
                    diam, diam, 0.45,
                    cr, cg, cb, 95,
                    false, false, 2, false, nil, nil, false
                )
            end
        end

        -- 1. Draw 3D speech bubbles above players
        for serverId, bubble in pairs(DemoClient.activeBubbles) do
            if now >= bubble.expiresAt then
                DemoClient.activeBubbles[serverId] = nil
            else
                hasActiveVisual = true
                local playerIdx = GetPlayerFromServerId(serverId)
                if playerIdx ~= -1 then
                    local ped = GetPlayerPed(playerIdx)
                    if DoesEntityExist(ped) then
                        local pos = GetEntityCoords(ped)
                        DemoClient.DrawText3D(vector3(pos.x, pos.y, pos.z + 1.15), bubble.text)
                    end
                end
            end
        end

        -- 2. Draw live 3D "[SPEAKING]" indicator above nearby talking players
        for _, playerIdx in ipairs(GetActivePlayers()) do
            if NetworkIsPlayerTalking(playerIdx) then
                hasActiveVisual = true
                local ped = GetPlayerPed(playerIdx)
                if DoesEntityExist(ped) then
                    local pos = GetEntityCoords(ped)
                    DemoClient.DrawText3D(vector3(pos.x, pos.y, pos.z + 1.05), '~g~[SPEAKING]~s~')
                end
            end
        end

        -- 3. Process spawned NPCs (despawn, death, proximity, 3D bubbles & prompts)
        local playerPed = PlayerPedId()
        local playerCoords = GetEntityCoords(playerPed)
        local closestInteractCandidate = nil
        local closestInteractDist = cfg.interactDist

        local npcListChanged = false
        for i = #DemoClient.spawnedNpcs, 1, -1 do
            local npc = DemoClient.spawnedNpcs[i]
            if not DoesEntityExist(npc.ped) then
                if DemoClient.activeInteractingNpcId == npc.npcId then
                    DemoClient.activeInteractingNpcId = nil
                    TriggerServerEvent('eevs_demo:npcInteractStop', 'despawned')
                end
                table.remove(DemoClient.spawnedNpcs, i)
                npcListChanged = true
            else
                local pos = GetEntityCoords(npc.ped)
                local dist = #(playerCoords - pos)

                if cfg.despawnDist > 0 and dist >= cfg.despawnDist then
                    if DemoClient.activeInteractingNpcId == npc.npcId then
                        DemoClient.activeInteractingNpcId = nil
                        TriggerServerEvent('eevs_demo:npcInteractStop', ('NPC >%.0fm away (deleted)'):format(cfg.despawnDist))
                    end
                    DeleteEntity(npc.ped)
                    table.remove(DemoClient.spawnedNpcs, i)
                    npcListChanged = true
                elseif IsEntityDead(npc.ped) or IsPedDeadOrDying(npc.ped, true) then
                    if not npc.disabled then
                        npc.disabled = true
                        npc.thinking = false
                        npc.thinkingStage = nil
                        npc.speakingUntil = 0
                        npc.activeClipId = nil
                        npc.speechText = nil
                        npc.speechExpiresAt = nil
                        if DemoClient.activeInteractingNpcId == npc.npcId then
                            DemoClient.activeInteractingNpcId = nil
                            TriggerServerEvent('eevs_demo:npcInteractStop', 'NPC died')
                        end
                        npcListChanged = true
                    end
                elseif not npc.disabled then
                    hasActiveVisual = true
                    local isInteractingWithThis = (DemoClient.activeInteractingNpcId == npc.npcId)

                    if dist > (cfg.interactDist + 1.5) then
                        npc.autoEngagedInRange = false
                    end

                    if isInteractingWithThis and dist > cfg.autoStopDist then
                        DemoClient.activeInteractingNpcId = nil
                        isInteractingWithThis = false
                        npc.autoEngagedInRange = true
                        TriggerServerEvent('eevs_demo:npcInteractStop', ('Walked >%.0fm away'):format(cfg.autoStopDist))
                    end

                    if not isInteractingWithThis and dist <= closestInteractDist then
                        closestInteractDist = dist
                        closestInteractCandidate = npc
                    end

                    local promptLine = ''
                    if isInteractingWithThis then
                        promptLine = '\n~r~[E] Stop~s~ ~c~(Hold [N] to talk)~s~'
                    elseif dist <= cfg.interactDist then
                        promptLine = '\n~y~[E] Interact~s~'
                    else
                        promptLine = '\n~c~[E] Interact (walk closer)~s~'
                    end

                    local actionTag = (npc.currentAction and npc.currentAction ~= 'none')
                        and (' ~y~[%s]~s~'):format(npc.currentAction)
                        or ''

                    if npc.speechText and npc.speechExpiresAt then
                        if now < npc.speechExpiresAt then
                            DemoClient.DrawText3D(vector3(pos.x, pos.y, pos.z + 1.44), ('~y~%s~s~'):format(npc.speechText), 0.45)
                        else
                            npc.speechText = nil
                            npc.speechExpiresAt = nil
                        end
                    end

                    if now < npc.speakingUntil then
                        DemoClient.DrawText3D(
                            vector3(pos.x, pos.y, pos.z + 1.15),
                            ('~g~[SPEAKING] %s~s~%s\n~b~Clip: %s~s~%s'):format(npc.label, actionTag, tostring(npc.activeClipId or 'clip'), promptLine)
                        )
                    elseif npc.thinking then
                        local dotCount = (math.floor(now / 320) % 3) + 1
                        local dots = string.rep('.', dotCount)
                        local stage = tostring(npc.thinkingStage or 'Thinking'):gsub('%.+$', '')
                        DemoClient.DrawText3D(
                            vector3(pos.x, pos.y, pos.z + 1.15),
                            ('~o~%s~s~%s\n~y~[%s%s]~s~%s'):format(npc.label, actionTag, stage, dots, promptLine)
                        )
                    else
                        if npc.speakingUntil > 0 then
                            npc.speakingUntil = 0
                            npc.activeClipId = nil
                            PlayFacialAnim(npc.ped, 'mood_normal_1', 'facials@gen_male@variations@normal')
                        end
                        local headerColor = isInteractingWithThis and '~g~> ' or '~w~'
                        DemoClient.DrawText3D(
                            vector3(pos.x, pos.y, pos.z + 1.08),
                            ('%s%s~s~%s%s'):format(headerColor, npc.label, actionTag, promptLine)
                        )
                    end
                end
            end
        end

        if npcListChanged then
            SendNUIMessage({ type = 'npcList', npcs = DemoClient.GetNpcSummaryList() })
        end

        -- 4. Draw 3D yellow speech bubbles for remote players' NPCs nearby
        for netIdNum, rem in pairs(DemoClient.remoteNpcBubbles) do
            if now >= rem.speechExpiresAt and now >= rem.speakingUntil then
                if NetworkDoesNetworkIdExist(netIdNum) then
                    local ped = NetToPed(netIdNum)
                    if ped and ped ~= 0 and DoesEntityExist(ped) then
                        PlayFacialAnim(ped, 'mood_normal_1', 'facials@gen_male@variations@normal')
                    end
                end
                DemoClient.remoteNpcBubbles[netIdNum] = nil
            elseif NetworkDoesNetworkIdExist(netIdNum) then
                local ped = NetToPed(netIdNum)
                if ped and ped ~= 0 and DoesEntityExist(ped) then
                    local pos = GetEntityCoords(ped)
                    if #(playerCoords - pos) <= (rem.hearingRadius or 35.0) then
                        hasActiveVisual = true
                        if rem.speechText and now < rem.speechExpiresAt then
                            DemoClient.DrawText3D(vector3(pos.x, pos.y, pos.z + 1.44), ('~y~%s~s~'):format(rem.speechText), 0.45)
                        end
                        if now < rem.speakingUntil then
                            DemoClient.DrawText3D(vector3(pos.x, pos.y, pos.z + 1.15), ('~g~[SPEAKING] NPC~s~\n~b~Clip: %s~s~'):format(tostring(rem.clipId or 'clip')))
                        end
                    end
                end
            end
        end

        -- 5. Handle manual [E] (INPUT_CONTEXT = 38) or Hands-Free Proximity Auto-Engage
        if not DemoClient.uiOpen and #DemoClient.spawnedNpcs > 0 then
            local manualPressed = IsControlJustPressed(0, 38)
            local shouldAutoEngage = (not manualPressed)
                and cfg.autoEngage
                and (DemoClient.activeInteractingNpcId == nil)
                and (closestInteractCandidate ~= nil)
                and (not closestInteractCandidate.autoEngagedInRange)

            if manualPressed and DemoClient.activeInteractingNpcId ~= nil then
                for _, npc in ipairs(DemoClient.spawnedNpcs) do
                    if npc.npcId == DemoClient.activeInteractingNpcId then
                        npc.autoEngagedInRange = true
                        break
                    end
                end
                DemoClient.activeInteractingNpcId = nil
                TriggerServerEvent('eevs_demo:npcInteractStop', 'manual')
            elseif (manualPressed or shouldAutoEngage) and closestInteractCandidate ~= nil then
                closestInteractCandidate.autoEngagedInRange = true
                DemoClient.activeInteractingNpcId = closestInteractCandidate.npcId
                if NetworkGetEntityIsNetworked(closestInteractCandidate.ped) then
                    closestInteractCandidate.pedNetId = PedToNet(closestInteractCandidate.ped)
                end
                TaskLookAtEntity(closestInteractCandidate.ped, playerPed, -1, 2048, 3)
                TriggerServerEvent('eevs_demo:npcInteractStart', DemoClient.BuildInteractionSessionPayload(closestInteractCandidate))
            end
        end

        if hasActiveVisual then
            local statusParts = {}
            if DemoClient.isLocalTalking then
                table.insert(statusParts, '~g~[MIC ACTIVE]~s~')
            else
                table.insert(statusParts, '~c~MIC IDLE [Hold N]~s~')
            end
            if DemoClient.hudState.echo ~= 'off' then
                table.insert(statusParts, ('~g~ECHO: %s~s~'):format(DemoClient.hudState.echo))
            end
            if DemoClient.hudState.proximityRange and DemoClient.hudState.proximityRange > 0 then
                table.insert(statusParts, ('~b~RANGE: %s (%.0fm)~s~'):format(string.upper(DemoClient.hudState.proximityMode or 'custom'), DemoClient.hudState.proximityRange))
            end
            if DemoClient.hudState.recording then
                table.insert(statusParts, '~r~[REC]~s~')
            end
            if DemoClient.hudState.sttListening then
                table.insert(statusParts, '~b~[STT LISTENING]~s~')
            end
            if DemoClient.activeInteractingNpcId ~= nil then
                table.insert(statusParts, ('~g~NPC #%d INTERACTING [E: Stop]~s~'):format(DemoClient.activeInteractingNpcId))
            end

            SetTextFont(4)
            SetTextScale(0.38, 0.38)
            SetTextColour(255, 255, 255, 225)
            SetTextOutline()
            BeginTextCommandDisplayText('STRING')
            AddTextComponentSubstringPlayerName('EEVS | ' .. table.concat(statusParts, '  |  '))
            EndTextCommandDisplayText(0.68, 0.025)
            Wait(0)
        else
            Wait(100)
        end
    end
end)
