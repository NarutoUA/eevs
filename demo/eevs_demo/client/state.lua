-- ============================================================================
-- EEVS Demo - Client State, Spatial Audio Submix & Shared Helpers
-- ============================================================================

DemoClient = {
    uiOpen = false,
    activeBubbles = {},
    remoteNpcBubbles = {}, -- [pedNetId] = { clipId = '...', speakingUntil = ms, speechText = '...', speechExpiresAt = ms }
    isLocalTalking = false,
    spawnedNpcs = {},
    nextNpcId = 1,
    activeInteractingNpcId = nil,
    hudState = {
        echo = 'off',
        recording = false,
        sttListening = false
    },
    config = {
        modelName = 's_f_y_cop_01',
        roleTitle = 'LSPD Officer',
        preset = 'cop',
        gender = 'female',
        systemPrompt = nil,
        voice = 'female',
        voiceInstruct = 'A sharp, composed female LSPD police officer in her late 20s with a confident, authoritative American voice.',
        voiceSeed = nil,
        voiceGain = 1.0,
        initiateFirst = false,
        broadcastScope = '3d_nearby',
        hearingRadius = 35.0,
        interactDist = 3.5,
        autoStopDist = 20.0,
        despawnDist = 100.0,
        autoEngage = false,
        enabledActions = nil,
        llmModel = nil,
        sttModel = nil,
        vdesModel = nil
    }
}

local npcSpatialSubmixId = nil
local npcSubmixActive = false

local function ensureNpcSpatialSubmix()
    if npcSpatialSubmixId ~= nil then
        return npcSpatialSubmixId
    end
    if type(CreateAudioSubmix) ~= 'function' then
        npcSpatialSubmixId = -1
        return -1
    end
    local submix = CreateAudioSubmix('eevs_npc_3d')
    if submix and submix ~= -1 then
        SetAudioSubmixEffectRadioFx(submix, 0)
        SetAudioSubmixEffectParamInt(submix, 0, GetHashKey('default'), 0)
        AddAudioSubmixOutput(submix, 0)
        npcSpatialSubmixId = submix
    else
        npcSpatialSubmixId = -1
    end
    return npcSpatialSubmixId
end

function DemoClient.UpdateNpcSpatialAudio(npcPos, customMaxDist)
    local localServerId = GetPlayerServerId(PlayerId())
    local camPos = GetGameplayCamCoord()
    local camRot = GetGameplayCamRot(2)

    local dx = npcPos.x - camPos.x
    local dy = npcPos.y - camPos.y
    local dz = npcPos.z - camPos.z
    local dist = math.sqrt(dx * dx + dy * dy + dz * dz)

    local maxDist = tonumber(customMaxDist) or DemoClient.config.hearingRadius or 35.0
    local atten = math.max(0.0, 1.0 - (dist / maxDist))
    atten = atten * atten

    local radZ = math.rad(camRot.z)
    local rightX = math.cos(radZ)
    local rightY = math.sin(radZ)

    local horizLen = math.sqrt(dx * dx + dy * dy)
    local pan = 0.0
    if horizLen > 0.05 then
        pan = (dx * rightX + dy * rightY) / horizLen
        pan = math.max(-1.0, math.min(1.0, pan))
    end

    local leftVol = atten * math.max(0.0, math.min(1.0, 1.0 - math.max(0.0, pan) * 0.85))
    local rightVol = atten * math.max(0.0, math.min(1.0, 1.0 + math.min(0.0, pan) * 0.85))

    local submix = ensureNpcSpatialSubmix()
    if submix ~= -1 and type(SetAudioSubmixOutputVolumes) == 'function' and type(MumbleSetSubmixForServerId) == 'function' then
        if not npcSubmixActive then
            MumbleSetSubmixForServerId(localServerId, submix)
            npcSubmixActive = true
        end
        SetAudioSubmixOutputVolumes(submix, 0, leftVol, rightVol, leftVol * 0.7, rightVol * 0.7, 1.0, 1.0)
    end
    if type(MumbleSetVolumeOverrideByServerId) == 'function' then
        MumbleSetVolumeOverrideByServerId(localServerId, math.max(0.01, atten))
    end
end

function DemoClient.ResetNpcSpatialAudio()
    if not npcSubmixActive then
        return
    end
    npcSubmixActive = false
    local localServerId = GetPlayerServerId(PlayerId())
    if type(MumbleSetSubmixForServerId) == 'function' then
        MumbleSetSubmixForServerId(localServerId, -1)
    end
    if type(MumbleSetVolumeOverrideByServerId) == 'function' then
        MumbleSetVolumeOverrideByServerId(localServerId, -1.0)
    end
end

local function wrapSingleLineFor3D(line, maxVisibleCols)
    -- Extract leading GTA color tag (e.g. ~y~) if present so wrapped continuation lines keep the same color
    local colorPrefix = line:match('^(~%a~)') or ''
    local visibleLen = #(line:gsub('~%a~', ''))
    if visibleLen <= maxVisibleCols then
        return { line }
    end

    local words = {}
    for w in line:gmatch('%S+') do
        table.insert(words, w)
    end
    if #words == 0 then
        return { line }
    end

    local wrapped = {}
    local currentLine = ''
    local currentVisible = 0

    for _, word in ipairs(words) do
        local wordVisible = #(word:gsub('~%a~', ''))
        if currentLine == '' then
            currentLine = word
            currentVisible = wordVisible
        elseif (currentVisible + 1 + wordVisible) <= maxVisibleCols then
            currentLine = currentLine .. ' ' .. word
            currentVisible = currentVisible + 1 + wordVisible
        else
            table.insert(wrapped, currentLine)
            currentLine = (colorPrefix ~= '' and not word:find('^~%a~')) and (colorPrefix .. word) or word
            currentVisible = wordVisible
        end
    end
    if currentLine ~= '' then
        table.insert(wrapped, currentLine)
    end
    return wrapped
end

local function formatAndWrap3DText(rawText, maxVisibleCols)
    local str = tostring(rawText or '')
    local outLines = {}
    for line in (str .. '\n'):gmatch('(.-)\n') do
        local wrapped = wrapSingleLineFor3D(line, maxVisibleCols)
        for _, wl in ipairs(wrapped) do
            table.insert(outLines, wl)
        end
    end
    return table.concat(outLines, '\n'), #outLines
end

function DemoClient.DrawText3D(coords, text, scaleOverride)
    local onScreen, sx, sy = World3dToScreen2d(coords.x, coords.y, coords.z)
    if not onScreen then return end

    local formatted, lineCount = formatAndWrap3DText(text, 56)
    local scale = scaleOverride or 0.42

    -- Shift multi-line blocks slightly upward in screen space so they expand upward rather than downward onto the ped
    if lineCount > 1 then
        sy = sy - ((lineCount - 1) * 0.014 * (scale / 0.42))
    end

    SetTextScale(scale, scale)
    SetTextFont(4)
    SetTextProportional(true)
    SetTextColour(255, 255, 255, 240)
    SetTextOutline()
    SetTextCentre(true)

    -- Use CELL_EMAIL_BCON (~a~~a~...) so texts > 99 bytes are never truncated by GTA V's scaleform buffer limit
    BeginTextCommandDisplayText('CELL_EMAIL_BCON')
    local len = #formatted
    local idx = 1
    local chunks = 0
    while idx <= len and chunks < 10 do
        local nextIdx = math.min(idx + 89, len)
        -- Avoid splitting in the middle of a ~x~ color code
        if nextIdx < len then
            local tail = formatted:sub(nextIdx - 1, nextIdx + 1)
            if tail:find('~', 1, true) then
                local lastSpace = formatted:sub(idx, nextIdx):match('^.*()%s')
                if lastSpace and lastSpace > 10 then
                    nextIdx = idx + lastSpace - 1
                end
            end
        end
        AddTextComponentSubstringPlayerName(formatted:sub(idx, nextIdx))
        idx = nextIdx + 1
        chunks = chunks + 1
    end
    EndTextCommandDisplayText(sx, sy)
end
