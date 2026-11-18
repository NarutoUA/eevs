-- ============================================================================
-- EEVS Demo - Server CLI Commands (`/eevs_*`) & NUI RPC Dispatcher
-- ============================================================================

local notifyPlayer = DemoServer.NotifyPlayer
local eevsRequest = DemoServer.EevsRequest

-- ============================================================================
-- 1. Self-Echo / Loopback Test: /eevs_echo [2d|3d|off]
-- ============================================================================
RegisterCommand('eevs_echo', function(source, args)
    if source == 0 then return end
    local arg = string.lower(args[1] or '2d')

    if arg == 'off' or arg == '0' or arg == 'stop' then
        EEVS.RemoveRoute(source, source, function(status, _res)
            notifyPlayer(source, ('Echo loopback disabled (status=%d)'):format(status))
            TriggerClientEvent('eevs_demo:stateUpdate', source, { echo = 'off' })
        end)
    else
        local mode = (arg == '3d') and '3D' or '2D'
        EEVS.AddRoute(source, source, mode, 'force', function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('Echo loopback enabled (%s)'):format(mode))
                TriggerClientEvent('eevs_demo:stateUpdate', source, { echo = mode })
            else
                notifyPlayer(source, ('Echo failed (%d): %s'):format(status, res.error or 'session not authenticated'))
            end
        end)
    end
end, false)

-- ============================================================================
-- 2. Voice Recorder / Dictaphone: /eevs_rec <start|stop|play|stop_play|wav|eevs|load>
-- ============================================================================
RegisterCommand('eevs_rec', function(source, args)
    if source == 0 then return end
    local action = string.lower(args[1] or 'start')

    if action == 'start' then
        local clipId = (args[2] and args[2] ~= 'isolate' and args[2] ~= '1' and args[2] ~= 'true') and args[2] or ''
        local isolate = (args[2] == 'isolate' or args[2] == '1' or args[2] == 'true'
            or args[3] == 'isolate' or args[3] == '1' or args[3] == 'true')
        eevsRequest('POST', '/recorder/start', {
            netId = source,
            clipId = clipId,
            isolate = isolate,
            maxSeconds = 30
        }, function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('Recording started (isolate=%s)'):format(tostring(isolate)))
                TriggerClientEvent('eevs_demo:stateUpdate', source, { recording = true })
            else
                notifyPlayer(source, ('Recorder start failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'stop' then
        eevsRequest('POST', '/recorder/stop', {
            netId = source,
            wait = true
        }, function(status, res)
            TriggerClientEvent('eevs_demo:stateUpdate', source, { recording = false })
            if status == 200 and res.ok then
                DemoServer.lastRecordedClipByPlayer[source] = res.clipId
                notifyPlayer(source, ('Saved clip "%s" (%d ms)'):format(res.clipId, res.durationMs or 0))
                TriggerClientEvent('eevs_demo:stateUpdate', source, { lastClipId = res.clipId })
            else
                notifyPlayer(source, ('Recorder stop failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'play' then
        local clipId = args[2] or DemoServer.lastRecordedClipByPlayer[source]
        local mode = string.upper(args[3] or '2D')
        if not clipId or clipId == '' then
            notifyPlayer(source, 'No clipId specified or recorded yet!')
            return
        end
        local listeners = (mode == '2D') and { source } or nil
        EEVS.PlayClip(clipId, source, mode, listeners, function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('Playing clip "%s" (%s, %d ms)'):format(clipId, mode, res.durationMs or 0))
            else
                notifyPlayer(source, ('Play clip failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'stop_play' or action == 'silence' then
        EEVS.StopClip({ senderNetId = source }, function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('Stopped active clip playbacks (count=%d)'):format(res.stoppedCount or 0))
            else
                notifyPlayer(source, ('Stop clip failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'wav' or action == 'save' or action == 'eevs' then
        local clipId = args[2] or DemoServer.lastRecordedClipByPlayer[source]
        if not clipId then
            notifyPlayer(source, 'No clipId specified or recorded yet!')
            return
        end
        local format = (action == 'eevs') and 'eevs' or 'wav'
        local endpoint = (action == 'eevs') and '/clip/save' or '/clip/save_wav'
        eevsRequest('POST', endpoint, {
            clipId = clipId,
            file = args[3],
            format = format
        }, function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('Saved clip "%s" to disk: %s'):format(clipId, res.file or ''))
            else
                notifyPlayer(source, ('Save clip failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'load' then
        local filePath = args[2]
        if not filePath or filePath == '' then
            notifyPlayer(source, 'Usage: /eevs_rec load <file.wav|file.eevs> [clipId]')
            return
        end
        eevsRequest('POST', '/clip/load', {
            file = filePath,
            clipId = args[3] or ''
        }, function(status, res)
            if status == 200 and res.ok then
                DemoServer.lastRecordedClipByPlayer[source] = res.clipId
                notifyPlayer(source, ('Loaded clip "%s" (%d ms) from %s'):format(res.clipId, res.durationMs or 0, filePath))
                TriggerClientEvent('eevs_demo:stateUpdate', source, { lastClipId = res.clipId })
            else
                notifyPlayer(source, ('Load clip failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    end
end, false)

-- ============================================================================
-- 3. Speech-to-Text (STT): /eevs_stt <start|stop> [language] [isolate]
-- ============================================================================
RegisterCommand('eevs_stt', function(source, args)
    if source == 0 then return end
    local action = string.lower(args[1] or 'start')

    if action == 'start' then
        local lang = args[2] or 'en'
        local isolate = (args[3] == 'isolate' or args[3] == '1')
        eevsRequest('POST', '/stt/start', {
            netId = source,
            language = lang,
            isolate = isolate
        }, function(status, res)
            if status == 200 and res.ok then
                notifyPlayer(source, ('STT listening started (lang=%s)...'):format(lang))
                TriggerClientEvent('eevs_demo:stateUpdate', source, { sttListening = true })
            else
                notifyPlayer(source, ('STT start failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    elseif action == 'stop' then
        eevsRequest('POST', '/stt/stop', {
            netId = source,
            wait = true
        }, function(status, res)
            TriggerClientEvent('eevs_demo:stateUpdate', source, { sttListening = false })
            if status == 200 and res.ok then
                local text = res.text or ''
                notifyPlayer(source, ('STT [%s]: "%s"'):format(res.backend or 'whisper', text))
                TriggerClientEvent('eevs_demo:showSpeechBubble', -1, source, text)
                TriggerClientEvent('eevs_demo:stateUpdate', source, { lastTranscript = text })
            else
                notifyPlayer(source, ('STT stop failed (%d): %s'):format(status, res.error or 'unknown'))
            end
        end)
    end
end, false)

-- ============================================================================
-- 4. Text-to-Speech (TTS): /eevs_tts <2d|3d> <text...>
-- ============================================================================
RegisterCommand('eevs_tts', function(source, args)
    if source == 0 then return end
    if #args < 2 then
        notifyPlayer(source, 'Usage: /eevs_tts <2d|3d> <text...>')
        return
    end

    local modeArg = string.upper(args[1])
    local mode = (modeArg == '2D') and '2D' or '3D'
    local text = table.concat(args, ' ', 2)

    eevsRequest('POST', '/tts/speak', {
        senderNetId = source,
        mode = mode,
        text = text
    }, function(status, res)
        if status == 200 and res.ok then
            notifyPlayer(source, ('TTS speaking (%s, clip=%s, %d ms)'):format(mode, res.clipId or '', res.durationMs or 0))
            TriggerClientEvent('eevs_demo:showSpeechBubble', -1, source, '[TTS] ' .. text)
        else
            notifyPlayer(source, ('TTS failed (%d): %s'):format(status, res.error or 'unknown'))
        end
    end)
end, false)

-- ============================================================================
-- 5. Asymmetric Wiretap / Listen Route: /eevs_wiretap <add|merge|remove|block|clear> <targetNetId> [2d|3d]
-- ============================================================================
RegisterCommand('eevs_wiretap', function(source, args)
    if source == 0 then return end
    local op = string.lower(args[1] or 'add')
    local targetNetId = tonumber(args[2])
    local mode = string.upper(args[3] or '2D')

    if op == 'clear_all' then
        EEVS.ClearRoutes(nil, function(status, _res)
            notifyPlayer(source, status == 200 and 'Cleared all custom routes' or 'Failed to clear routes')
        end)
        return
    end

    if not targetNetId then
        notifyPlayer(source, 'Usage: /eevs_wiretap <add|merge|remove|block> <targetServerId> [2d|3d]')
        return
    end

    local endpoint = '/route/add'
    local payload = {
        fromNetId = targetNetId,
        toNetId = source,
        mode = mode
    }

    if op == 'merge' then
        payload.action = 'merge'
    elseif op == 'remove' or op == 'clear' then
        endpoint = '/route/remove'
    elseif op == 'block' then
        endpoint = '/route/block'
    end

    eevsRequest('POST', endpoint, payload, function(status, res)
        if status == 200 and res.ok then
            notifyPlayer(source, ('Route %s: speaker=%d -> listener=%d (%s)'):format(op, targetNetId, source, mode))
        else
            notifyPlayer(source, ('Route %s failed (%d): %s'):format(op, status, res.error or 'unknown'))
        end
    end)
end, false)

-- ============================================================================
-- 6. Real-Time Signalsmith Voice Effects: /eevs_fx <preset|off> [all|2d|3d]
-- ============================================================================
RegisterCommand('eevs_fx', function(source, args)
    if source == 0 then return end
    local preset = string.lower(args[1] or 'mask')
    local branch = string.upper(args[2] or '')
    if branch ~= '2D' and branch ~= '3D' then
        branch = (preset == 'tunnel_phone' or preset == 'radio') and '2D' or 'all'
    end

    if preset == 'off' or preset == 'clear' or preset == 'none' then
        eevsRequest('POST', '/effects/clear', { netId = source }, function(status, _res)
            notifyPlayer(source, status == 200 and 'Cleared all real-time voice effects on your character' or 'Failed to clear voice effects')
        end)
        return
    end

    eevsRequest('POST', '/effects/set', {
        netId = source,
        preset = preset,
        targetBranch = branch
    }, function(status, res)
        if status == 200 and res.ok then
            notifyPlayer(source, ('Voice Effect ON: preset=%s (branch=%s)'):format(res.preset or preset, res.targetBranch or branch))
        else
            notifyPlayer(source, ('Voice Effect failed (%d): %s'):format(status, res and res.error or 'unknown'))
        end
    end)
end, false)

-- ============================================================================
-- 7. Talker Proximity Range:
--    /eevs_range [whisper|normal|shout|megaphone|default|<meters>|cycle]
-- ============================================================================
local PROXIMITY_CYCLE_MODES = {
    { mode = 'whisper',   range = 3.0,  label = 'Whisper (3m)' },
    { mode = 'normal',    range = 12.0, label = 'Normal (12m)' },
    { mode = 'shout',     range = 28.0, label = 'Shout (28m)' },
    { mode = 'megaphone', range = 75.0, label = 'Megaphone PA (75m + DSP)' }
}
local demoPlayerProximityState = {} -- [src] = { mode = 'unlimited', autoMegaphoneActive = false }

AddEventHandler('playerDropped', function()
    local src = tonumber(source)
    if src then
        demoPlayerProximityState[src] = nil
    end
end)

local function applyPlayerProximityPreset(src, arg)
    local prevState = demoPlayerProximityState[src] or { mode = 'unlimited', autoMegaphoneActive = false }
    local curMode = prevState.mode
    local key = string.lower(tostring(arg or 'cycle'))

    local chosen = nil
    if key == 'cycle' or key == '' then
        local idx = 2 -- default to normal (12m)
        for i, m in ipairs(PROXIMITY_CYCLE_MODES) do
            if m.mode == curMode then
                idx = (i % #PROXIMITY_CYCLE_MODES) + 1
                break
            end
        end
        chosen = PROXIMITY_CYCLE_MODES[idx]
    elseif key == 'default' or key == 'unlimited' or key == 'max' or key == 'off' then
        chosen = { mode = 'unlimited', range = 0.0, label = 'Channel Max (0.0m)' }
    else
        for _, m in ipairs(PROXIMITY_CYCLE_MODES) do
            if m.mode == key then
                chosen = m
                break
            end
        end
        if not chosen then
            local customMeters = tonumber(key)
            if customMeters and customMeters >= 0.0 and customMeters <= 512.0 then
                chosen = {
                    mode = customMeters == 0.0 and 'unlimited' or 'custom',
                    range = customMeters,
                    label = customMeters == 0.0 and 'Channel Max (0.0m)' or string.format('Custom (%.1fm)', customMeters)
                }
            end
        end
    end

    if not chosen then
        return false, 'Usage: /eevs_range [whisper|normal|shout|megaphone|default|<0..512>]'
    end

    EevsSetPlayerVoiceProximityRange(src, chosen.range)

    local autoMegaphoneActive = prevState.autoMegaphoneActive
    if chosen.mode == 'megaphone' then
        autoMegaphoneActive = true
        eevsRequest('POST', '/effects/set', { netId = src, preset = 'megaphone', targetBranch = 'all' })
    elseif autoMegaphoneActive then
        autoMegaphoneActive = false
        eevsRequest('POST', '/effects/clear', { netId = src, targetBranch = 'all' })
    end

    demoPlayerProximityState[src] = {
        mode = chosen.mode,
        autoMegaphoneActive = autoMegaphoneActive
    }

    TriggerClientEvent('eevs_demo:stateUpdate', src, {
        proximityRange = chosen.range,
        proximityMode = chosen.mode,
        proximityLabel = chosen.label
    })
    TriggerClientEvent('eevs_demo:showProximityRing', src, chosen.range, chosen.mode)
    return true, chosen
end

RegisterCommand('eevs_range', function(source, args)
    if source == 0 then return end
    local ok, res = applyPlayerProximityPreset(source, args[1] or 'cycle')
    if ok then
        notifyPlayer(source, ('Voice Proximity: %s'):format(res.label))
    else
        notifyPlayer(source, res)
    end
end, false)

RegisterNetEvent('eevs_demo:cycleProximityRange', function()
    local src = source
    if not src or src <= 0 then return end
    local ok, res = applyPlayerProximityPreset(src, 'cycle')
    if ok then
        notifyPlayer(src, ('Voice Proximity: %s'):format(res.label))
    end
end)

-- ============================================================================
-- NUI Callbacks Forwarded from Client
-- ============================================================================
local function handleNuiLlmTurn(src, reqId, userPrompt, payload, npcId, npcPedNetId, transcriptToInclude)
    if npcId then
        TriggerClientEvent('eevs_demo:npcThinking', src, npcId, true, 'Thinking...')
    end

    local llmModel = payload and payload.llmModel
    local enabledActions = payload and payload.enabledActions
    DemoServer.QueryLlmApi(userPrompt, payload and payload.systemPrompt, llmModel, enabledActions, function(llmOk, replyOrErr, actionName, actionArgs)
        if not llmOk then
            if npcId then
                TriggerClientEvent('eevs_demo:npcThinking', src, npcId, false, nil)
            end
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, 502, {
                ok = false,
                transcript = transcriptToInclude,
                error = replyOrErr
            })
            return
        end
        if npcId and replyOrErr ~= '' then
            TriggerClientEvent('eevs_demo:npcThinking', src, npcId, true, 'Generating audio...')
        end
        DemoServer.SpeakReplyViaEevs(src, replyOrErr, actionName, actionArgs, {
            npcId = npcId,
            npcPedNetId = npcPedNetId,
            voice = payload and payload.voice,
            voiceInstruct = payload and payload.voiceInstruct,
            voiceSeed = payload and payload.voiceSeed,
            voiceGain = payload and payload.voiceGain,
            broadcastScope = payload and payload.broadcastScope,
            hearingRadius = payload and payload.hearingRadius,
            mode = payload and payload.mode,
            options = payload and payload.options
        }, function(status, data)
            if npcId then
                TriggerClientEvent('eevs_demo:npcThinking', src, npcId, false, nil)
            end
            if transcriptToInclude and type(data) == 'table' then
                data.transcript = transcriptToInclude
            end
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, status, data)
        end)
    end)
end

RegisterNetEvent('eevs_demo:nuiRequest', function(reqId, method, path, payload)
    local src = source
    local npcId = nil
    local npcPedNetId = nil

    if type(payload) == 'table' then
        if payload.npcId then
            npcId = tonumber(payload.npcId)
            payload.npcId = nil
        end
        if payload.npcPedNetId then
            npcPedNetId = tonumber(payload.npcPedNetId)
            payload.npcPedNetId = nil
        end
        if payload.netId == 'self' then payload.netId = src end
        if payload.senderNetId == 'self' then
            payload.senderNetId = (npcPedNetId and npcPedNetId > 0) and npcPedNetId or src
        end
        if payload.fromNetId == 'self' then payload.fromNetId = src end
        if payload.toNetId == 'self' then payload.toNetId = src end
        if path == '/clip/play' and not payload.listenerNetIds then
            if npcId then
                payload.mode = '3D'
            elseif payload.mode == '2D' then
                payload.listenerNetIds = { src }
            end
        end
    end

    if path == '/llm/models' then
        DemoServer.FetchLlmModelsList(function(ok, models, baseUrl)
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, 200, {
                ok = true,
                backend = ok and 'OpenAI API' or 'OpenAI API [offline]',
                url = baseUrl,
                models = models
            })
        end)
        return
    elseif path == '/player/proximity' then
        local targetArg = payload and (payload.mode or payload.range) or 'cycle'
        local ok, chosenOrErr = applyPlayerProximityPreset(src, targetArg)
        if ok then
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, 200, {
                ok = true,
                range = chosenOrErr.range,
                mode = chosenOrErr.mode,
                label = chosenOrErr.label
            })
        else
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, 400, {
                ok = false,
                error = chosenOrErr
            })
        end
        return
    elseif path == '/ai/chat' then
        local prompt = (payload and payload.prompt) or ''
        if prompt == '' then
            TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, 400, { ok = false, error = 'Missing prompt' })
            return
        end
        handleNuiLlmTurn(src, reqId, prompt, payload, npcId, npcPedNetId, nil)
        return
    elseif path == '/ai/converse/stop' then
        if npcId then
            TriggerClientEvent('eevs_demo:npcThinking', src, npcId, true, 'Transcribing speech...')
        end
        eevsRequest('POST', '/stt/stop', {
            netId = src,
            wait = true,
            options = payload and (payload.sttOptions or payload.options)
        }, function(sttStatus, sttData)
            if sttStatus ~= 200 or not sttData or not sttData.ok or not sttData.text or sttData.text == '' then
                if npcId then
                    TriggerClientEvent('eevs_demo:npcThinking', src, npcId, false, nil)
                end
                TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, sttStatus or 400, {
                    ok = false,
                    error = (sttData and sttData.error) or 'No speech transcribed'
                })
                return
            end

            local transcript = sttData.text
            TriggerClientEvent('eevs_demo:showSpeechBubble', -1, src, transcript)
            handleNuiLlmTurn(src, reqId, transcript, payload, npcId, npcPedNetId, transcript)
        end)
        return
    end

    if path == '/clip/play' and npcId and npcPedNetId and npcPedNetId > 0 then
        DemoServer.StartDynamicNpcProximityBroadcast(npcPedNetId, src, 8000, function()
            eevsRequest(method, path, payload, function(status, data)
                if status == 200 and data and data.ok then
                    local dur = data.durationMs or 2500
                    DemoServer.UpdateActiveNpcBroadcastExpiry(npcPedNetId, src, dur)
                    TriggerClientEvent('eevs_demo:npcSpeaking', -1, {
                        npcId = npcId,
                        clipId = payload and payload.clipId or 'clip',
                        durationMs = dur,
                        actionName = 'none',
                        npcPedNetId = npcPedNetId,
                        ownerServerId = src
                    })
                end
                TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, status, data)
            end)
        end)
        return
    end

    eevsRequest(method, path, payload, function(status, data)
        if path == '/clip/play' and status == 200 and data and data.ok and npcId then
            TriggerClientEvent('eevs_demo:npcSpeaking', -1, {
                npcId = npcId,
                clipId = payload and payload.clipId or 'clip',
                durationMs = data.durationMs or 2500,
                actionName = 'none',
                npcPedNetId = npcPedNetId,
                ownerServerId = src
            })
        elseif path == '/tts/speak' and status == 200 and data.ok and payload and payload.text then
            TriggerClientEvent('eevs_demo:showSpeechBubble', -1, src, '[TTS] ' .. payload.text)
        elseif path == '/stt/stop' and status == 200 and data.ok and data.text then
            TriggerClientEvent('eevs_demo:showSpeechBubble', -1, src, data.text)
        elseif path == '/route/add' and status == 200 and data and data.ok and payload and payload.fromNetId == src and payload.toNetId == src then
            TriggerClientEvent('eevs_demo:stateUpdate', src, { echo = string.upper(tostring(payload.mode or '2D')) })
        elseif path == '/route/remove' and status == 200 and data and data.ok and payload and payload.fromNetId == src and payload.toNetId == src then
            TriggerClientEvent('eevs_demo:stateUpdate', src, { echo = 'off' })
        elseif path == '/route/clear' and status == 200 and data and data.ok and (not payload or not payload.fromNetId or payload.fromNetId == src) then
            TriggerClientEvent('eevs_demo:stateUpdate', src, { echo = 'off' })
        end
        TriggerClientEvent('eevs_demo:nuiResponse', src, reqId, status, data)
    end)
end)
