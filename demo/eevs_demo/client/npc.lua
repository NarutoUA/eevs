-- ============================================================================
-- EEVS Demo - Client NPC Spawning, Config Sync & Physical Action Executor
-- ============================================================================

local DEFAULT_NPC_MODEL = 's_f_y_cop_01'

function DemoClient.GetNpcSummaryList()
    local list = {}
    for _, npc in ipairs(DemoClient.spawnedNpcs) do
        if DoesEntityExist(npc.ped) and not npc.disabled and not IsEntityDead(npc.ped) and not IsPedDeadOrDying(npc.ped, true) then
            table.insert(list, {
                npcId = npc.npcId,
                pedNetId = npc.pedNetId,
                label = npc.label,
                roleTitle = npc.roleTitle or 'NPC',
                gender = npc.gender or npc.voice or 'female',
                modelName = npc.modelName or DEFAULT_NPC_MODEL,
                voiceSeed = npc.voiceSeed or 0,
                voiceGain = npc.voiceGain or 1.0
            })
        end
    end
    return list
end

function DemoClient.GetNpcById(npcId)
    local target = tonumber(npcId)
    if not target then
        return nil
    end
    for _, npc in ipairs(DemoClient.spawnedNpcs) do
        if npc.npcId == target and DoesEntityExist(npc.ped) then
            if NetworkGetEntityIsNetworked(npc.ped) then
                npc.pedNetId = PedToNet(npc.ped)
            end
            return npc
        end
    end
    return nil
end

function DemoClient.ApplyConfigPatch(data)
    if type(data) ~= 'table' then return end
    local cfg = DemoClient.config

    if data.systemPrompt and data.systemPrompt ~= '' then cfg.systemPrompt = data.systemPrompt end
    if data.voice and data.voice ~= '' then cfg.voice = data.voice end
    if data.gender and data.gender ~= '' then cfg.gender = data.gender end
    if data.voiceInstruct and data.voiceInstruct ~= '' then cfg.voiceInstruct = data.voiceInstruct end
    if data.voiceSeed then cfg.voiceSeed = tonumber(data.voiceSeed) end
    if data.voiceGain then cfg.voiceGain = tonumber(data.voiceGain) or 1.0 end
    if data.initiateFirst ~= nil then cfg.initiateFirst = (data.initiateFirst == true) end
    if data.broadcastScope and data.broadcastScope ~= '' then cfg.broadcastScope = data.broadcastScope end
    if data.hearingRadius then cfg.hearingRadius = tonumber(data.hearingRadius) or 35.0 end
    if data.interactDist then cfg.interactDist = math.max(1.5, math.min(15.0, tonumber(data.interactDist) or 3.5)) end
    if data.autoStopDist then cfg.autoStopDist = math.max(5.0, math.min(100.0, tonumber(data.autoStopDist) or 20.0)) end
    if data.despawnDist ~= nil then cfg.despawnDist = math.max(0.0, math.min(500.0, tonumber(data.despawnDist) or 100.0)) end
    if data.autoEngage ~= nil then cfg.autoEngage = (data.autoEngage == true) end
    if type(data.enabledActions) == 'table' then cfg.enabledActions = data.enabledActions end
    if data.llmModel ~= nil then cfg.llmModel = (data.llmModel ~= '') and data.llmModel or nil end
    if data.sttModel ~= nil then cfg.sttModel = (data.sttModel ~= '') and data.sttModel or nil end
    if data.vdesModel ~= nil then cfg.vdesModel = (data.vdesModel ~= '') and data.vdesModel or nil end
    if data.modelName and data.modelName ~= '' then cfg.modelName = data.modelName end
    if data.roleTitle and data.roleTitle ~= '' then cfg.roleTitle = data.roleTitle end
    if data.preset and data.preset ~= '' then cfg.preset = data.preset end
end

function DemoClient.BuildInteractionSessionPayload(npc)
    local cfg = DemoClient.config
    return {
        npcId = npc.npcId,
        pedNetId = npc.pedNetId,
        label = npc.label,
        systemPrompt = npc.systemPrompt or cfg.systemPrompt,
        voice = npc.voice or cfg.voice,
        voiceInstruct = npc.voiceInstruct or cfg.voiceInstruct,
        voiceSeed = npc.voiceSeed or cfg.voiceSeed,
        voiceGain = npc.voiceGain or cfg.voiceGain,
        initiateFirst = (npc.initiateFirst ~= nil) and npc.initiateFirst or cfg.initiateFirst,
        broadcastScope = npc.broadcastScope or cfg.broadcastScope,
        hearingRadius = npc.hearingRadius or cfg.hearingRadius,
        enabledActions = npc.enabledActions or cfg.enabledActions,
        llmModel = cfg.llmModel,
        vdesModel = cfg.vdesModel,
        sttModel = cfg.sttModel
    }
end

function DemoClient.SpawnNpcInFront(opts)
    opts = type(opts) == 'table' and opts or {}
    local cfg = DemoClient.config

    local modelName = (type(opts.modelName) == 'string' and opts.modelName ~= '') and opts.modelName or cfg.modelName or DEFAULT_NPC_MODEL
    local roleTitle = (type(opts.roleTitle) == 'string' and opts.roleTitle ~= '') and opts.roleTitle or cfg.roleTitle or 'NPC'
    local rolePreset = (type(opts.preset) == 'string' and opts.preset ~= '') and opts.preset or cfg.preset or 'cop'
    local npcSystemPrompt = (type(opts.systemPrompt) == 'string' and opts.systemPrompt ~= '') and opts.systemPrompt or cfg.systemPrompt
    local npcVoice = (type(opts.voice) == 'string' and opts.voice ~= '') and opts.voice or cfg.voice or 'default'
    local npcGender = (type(opts.gender) == 'string' and opts.gender ~= '') and opts.gender or npcVoice
    local npcVoiceInstruct = (type(opts.voiceInstruct) == 'string' and opts.voiceInstruct ~= '') and opts.voiceInstruct or cfg.voiceInstruct
    local npcVoiceSeed = tonumber(opts.voiceSeed)
    if not npcVoiceSeed or npcVoiceSeed <= 0 then
        npcVoiceSeed = math.random(100000, 999999)
    end
    local npcVoiceGain = tonumber(opts.voiceGain) or cfg.voiceGain or 1.0
    local npcInitiateFirst = (opts.initiateFirst ~= nil) and (opts.initiateFirst == true) or cfg.initiateFirst
    local npcBroadcastScope = (type(opts.broadcastScope) == 'string' and opts.broadcastScope ~= '') and opts.broadcastScope or cfg.broadcastScope
    local npcHearingRadius = tonumber(opts.hearingRadius) or cfg.hearingRadius or 35.0
    local npcEnabledActions = (type(opts.enabledActions) == 'table') and opts.enabledActions or cfg.enabledActions

    local modelHash = GetHashKey(modelName)
    if not IsModelInCdimage(modelHash) or not IsModelValid(modelHash) then
        return nil, ('Invalid ped model: %s'):format(tostring(modelName))
    end

    RequestModel(modelHash)
    local timeout = GetGameTimer() + 4000
    while not HasModelLoaded(modelHash) and GetGameTimer() < timeout do
        Wait(10)
    end
    if not HasModelLoaded(modelHash) then
        return nil, 'Failed to load ped model ' .. modelName
    end

    local playerPed = PlayerPedId()
    local count = #DemoClient.spawnedNpcs
    local offsetX = ((count % 3) - 1) * 1.15
    local offsetY = 2.4 + math.floor(count / 3) * 1.3
    local spawnPos = GetOffsetFromEntityInWorldCoords(playerPed, offsetX, offsetY, 0.0)
    local foundGround, groundZ = GetGroundZFor_3dCoord(spawnPos.x, spawnPos.y, spawnPos.z + 2.0, false)
    local finalZ = foundGround and groundZ or (spawnPos.z - 1.0)
    local heading = (GetEntityHeading(playerPed) + 180.0) % 360.0

    local ped = CreatePed(4, modelHash, spawnPos.x, spawnPos.y, finalZ, heading, true, true)
    SetModelAsNoLongerNeeded(modelHash)

    if not DoesEntityExist(ped) then
        return nil, 'CreatePed failed'
    end

    SetEntityAsMissionEntity(ped, true, true)
    NetworkRegisterEntityAsNetworked(ped)
    SetBlockingOfNonTemporaryEvents(ped, true)
    SetPedFleeAttributes(ped, 0, false)
    SetPedCombatAttributes(ped, 17, true)
    SetEntityInvincible(ped, true)
    FreezeEntityPosition(ped, true)
    TaskLookAtEntity(ped, playerPed, -1, 2048, 3)

    local pedNetId = NetworkGetEntityIsNetworked(ped) and PedToNet(ped) or 0
    local npcId = DemoClient.nextNpcId
    DemoClient.nextNpcId = npcId + 1

    local entry = {
        npcId = npcId,
        ped = ped,
        pedNetId = pedNetId,
        modelName = modelName,
        roleTitle = roleTitle,
        rolePreset = rolePreset,
        gender = npcGender,
        systemPrompt = npcSystemPrompt,
        voice = npcVoice,
        voiceInstruct = npcVoiceInstruct,
        voiceSeed = npcVoiceSeed,
        voiceGain = npcVoiceGain,
        initiateFirst = npcInitiateFirst,
        broadcastScope = npcBroadcastScope,
        hearingRadius = npcHearingRadius,
        enabledActions = npcEnabledActions,
        label = ('%s #%d (NetID #%d)'):format(roleTitle, npcId, pedNetId),
        speakingUntil = 0,
        activeClipId = nil
    }
    table.insert(DemoClient.spawnedNpcs, entry)
    return entry, nil
end

function DemoClient.DeleteSpawnedNpcById(targetNpcId)
    targetNpcId = tonumber(targetNpcId)
    if not targetNpcId then
        return false
    end
    for i = #DemoClient.spawnedNpcs, 1, -1 do
        local npc = DemoClient.spawnedNpcs[i]
        if npc.npcId == targetNpcId then
            if DemoClient.activeInteractingNpcId == npc.npcId then
                DemoClient.activeInteractingNpcId = nil
                TriggerServerEvent('eevs_demo:npcInteractStop', 'deleted')
            end
            if DoesEntityExist(npc.ped) then
                DeleteEntity(npc.ped)
            end
            table.remove(DemoClient.spawnedNpcs, i)
            if #DemoClient.spawnedNpcs == 0 then
                DemoClient.ResetNpcSpatialAudio()
            end
            return true
        end
    end
    return false
end

function DemoClient.ClearAllSpawnedNpcs()
    if DemoClient.activeInteractingNpcId ~= nil then
        DemoClient.activeInteractingNpcId = nil
        TriggerServerEvent('eevs_demo:npcInteractStop', 'cleared')
    end
    local removed = 0
    for _, npc in ipairs(DemoClient.spawnedNpcs) do
        if DoesEntityExist(npc.ped) then
            DeleteEntity(npc.ped)
            removed = removed + 1
        end
    end
    DemoClient.spawnedNpcs = {}
    DemoClient.ResetNpcSpatialAudio()
    return removed
end

AddEventHandler('onResourceStop', function(resName)
    if resName == GetCurrentResourceName() then
        DemoClient.ClearAllSpawnedNpcs()
    end
end)

-- ============================================================================
-- Physical GTA V Native Action Helpers & Executor
-- ============================================================================

local function getTargetVehicleForPlayer(playerPed)
    local veh = GetVehiclePedIsIn(playerPed, false)
    if veh and veh ~= 0 and DoesEntityExist(veh) then
        return veh
    end
    veh = GetVehiclePedIsIn(playerPed, true)
    if veh and veh ~= 0 and DoesEntityExist(veh) then
        return veh
    end
    local pos = GetEntityCoords(playerPed)
    veh = GetClosestVehicle(pos.x, pos.y, pos.z, 12.0, 0, 71)
    if veh and veh ~= 0 and DoesEntityExist(veh) then
        return veh
    end
    return 0
end

local function findNearestTargetPed(originPed, playerPed, maxRadius, onlyDeadOrInjured, preferHostile)
    local originCoords = GetEntityCoords(originPed)
    local bestPed = 0
    local bestScore = 999999.0
    local peds = GetGamePool('CPed')
    for _, cand in ipairs(peds) do
        if cand ~= originPed and cand ~= playerPed and DoesEntityExist(cand) and not IsPedAPlayer(cand) then
            local isDead = IsEntityDead(cand) or IsPedDeadOrDying(cand, true)
            if onlyDeadOrInjured then
                if isDead then
                    local dist = #(originCoords - GetEntityCoords(cand))
                    if dist <= maxRadius and dist < bestScore then
                        bestScore = dist
                        bestPed = cand
                    end
                end
            elseif not isDead then
                local dist = #(originCoords - GetEntityCoords(cand))
                if dist <= maxRadius then
                    local score = dist
                    if preferHostile then
                        if IsPedInCombat(cand, playerPed) or IsPedInCombat(cand, originPed) then
                            score = score - 500.0
                        elseif IsPedArmed(cand, 7) then
                            score = score - 150.0
                        end
                    end
                    if score < bestScore then
                        bestScore = score
                        bestPed = cand
                    end
                end
            end
        end
    end
    return bestPed
end

local function playGiveItemAnim(ped)
    local dict = 'mp_common'
    RequestAnimDict(dict)
    CreateThread(function()
        local timeout = GetGameTimer() + 2000
        while not HasAnimDictLoaded(dict) and GetGameTimer() < timeout do
            Wait(10)
        end
        if DoesEntityExist(ped) and HasAnimDictLoaded(dict) then
            TaskPlayAnim(ped, dict, 'givetake1_a', 8.0, -8.0, 2200, 49, 0.0, false, false, false)
        end
    end)
end

local function allyPedWithPlayer(ped, playerPed, wepName)
    SetBlockingOfNonTemporaryEvents(ped, false)
    SetEntityInvincible(ped, false)
    SetPedCombatAttributes(ped, 17, false)
    SetPedCombatAttributes(ped, 46, true)
    SetPedCombatAttributes(ped, 5, true)
    local playerRelGroup = GetPedRelationshipGroupHash(playerPed)
    SetPedRelationshipGroupHash(ped, playerRelGroup)
    SetPedAsGroupMember(ped, GetPlayerGroup(PlayerId()))
    SetPedNeverLeavesGroup(ped, true)
    if wepName and wepName ~= '' then
        local wep = GetHashKey(wepName)
        GiveWeaponToPed(ped, wep, 180, false, true)
        SetCurrentPedWeapon(ped, wep, true)
    end
end

function DemoClient.ExecuteNpcAction(npc, actionName, actionArgs)
    if not npc or not DoesEntityExist(npc.ped) then
        return
    end
    actionName = string.lower(tostring(actionName or 'none'))
    if actionName == '' or actionName == 'none' then
        return
    end

    local def = NPC_ACTIONS_BY_NAME and NPC_ACTIONS_BY_NAME[actionName]
    if not def then
        return
    end

    actionArgs = type(actionArgs) == 'table' and actionArgs or {}
    local ped = npc.ped
    local playerPed = PlayerPedId()
    npc.currentAction = actionName
    FreezeEntityPosition(ped, false)

    if def.endInteraction and DemoClient.activeInteractingNpcId == npc.npcId then
        DemoClient.activeInteractingNpcId = nil
        TriggerServerEvent('eevs_demo:npcInteractStop', 'NPC action: ' .. actionName)
    end

    local kind = def.type
    if kind == 'scenario' and def.scenario then
        ClearPedTasks(ped)
        TaskStartScenarioInPlace(ped, def.scenario, 0, true)
    elseif kind == 'anim' and def.dict and def.anim then
        ClearPedTasks(ped)
        local dict = def.dict
        local anim = def.anim
        local dur = tonumber(actionArgs.duration) or def.duration or 3000
        local flag = def.flag or 49
        RequestAnimDict(dict)
        CreateThread(function()
            local timeout = GetGameTimer() + 2000
            while not HasAnimDictLoaded(dict) and GetGameTimer() < timeout do
                Wait(10)
            end
            if DoesEntityExist(ped) and HasAnimDictLoaded(dict) then
                TaskPlayAnim(ped, dict, anim, 8.0, -8.0, dur, flag, 0.0, false, false, false)
            end
        end)
    elseif kind == 'follow' then
        ClearPedTasks(ped)
        TaskFollowToOffsetOfEntity(ped, playerPed, 0.0, -1.6, 0.0, 2.2, -1, 2.0, true)
    elseif kind == 'stop_moving' then
        ClearPedTasks(ped)
        TaskStandStill(ped, -1)
        TaskLookAtEntity(ped, playerPed, -1, 2048, 3)
    elseif kind == 'come_closer' then
        ClearPedTasks(ped)
        TaskGoToEntity(ped, playerPed, -1, 1.4, 1.2, 1073741824, 0)
    elseif kind == 'walk_away' then
        ClearPedTasks(ped)
        TaskWanderStandard(ped, 10.0, 10)
    elseif kind == 'flee' then
        ClearPedTasks(ped)
        TaskSmartFleePed(ped, playerPed, 100.0, -1, false, false)
    elseif kind == 'hands_up' then
        ClearPedTasks(ped)
        TaskHandsUp(ped, tonumber(actionArgs.duration) or def.duration or 15000, playerPed, -1, false)
    elseif kind == 'enter_vehicle' then
        local veh = getTargetVehicleForPlayer(playerPed)
        if veh ~= 0 then
            ClearPedTasks(ped)
            TaskEnterVehicle(ped, veh, 12000, def.seat or 0, 1.5, 1, 0)
        end
    elseif kind == 'exit_vehicle' then
        if IsPedInAnyVehicle(ped, false) then
            TaskLeaveAnyVehicle(ped, 0, 0)
        end
    elseif kind == 'drive_to_waypoint' then
        local veh = GetVehiclePedIsIn(ped, false)
        if veh == 0 then
            veh = getTargetVehicleForPlayer(playerPed)
        end
        local blip = GetFirstBlipInfoId(8)
        if veh ~= 0 and DoesBlipExist(blip) then
            local wp = GetBlipInfoIdCoord(blip)
            CreateThread(function()
                if GetPedInVehicleSeat(veh, -1) ~= ped then
                    TaskEnterVehicle(ped, veh, 10000, -1, 2.0, 1, 0)
                    local t = GetGameTimer() + 10000
                    while GetPedInVehicleSeat(veh, -1) ~= ped and GetGameTimer() < t do
                        Wait(100)
                    end
                end
                if DoesEntityExist(ped) and GetPedInVehicleSeat(veh, -1) == ped then
                    TaskVehicleDriveToCoordLongrange(ped, veh, wp.x, wp.y, wp.z, 25.0, 786603, 5.0)
                end
            end)
        end
    elseif kind == 'repair_vehicle' then
        local veh = getTargetVehicleForPlayer(playerPed)
        if veh ~= 0 then
            ClearPedTasks(ped)
            TaskTurnPedToFaceEntity(ped, veh, 1500)
            CreateThread(function()
                Wait(1200)
                if DoesEntityExist(ped) and DoesEntityExist(veh) then
                    TaskStartScenarioInPlace(ped, 'PROP_HUMAN_BUM_BIN', 0, true)
                    Wait(4000)
                    if DoesEntityExist(veh) then
                        SetVehicleFixed(veh)
                        SetVehicleDeformationFixed(veh)
                        SetVehicleUndriveable(veh, false)
                        SetVehicleEngineOn(veh, true, true, false)
                    end
                    if DoesEntityExist(ped) then
                        ClearPedTasks(ped)
                        TaskLookAtEntity(ped, PlayerPedId(), -1, 2048, 3)
                    end
                end
            end)
        end
    elseif kind == 'aim_weapon' then
        ClearPedTasks(ped)
        local wepName = (type(actionArgs.weapon) == 'string' and actionArgs.weapon ~= '') and actionArgs.weapon or def.weapon or 'WEAPON_PISTOL'
        local wep = GetHashKey(wepName)
        GiveWeaponToPed(ped, wep, 60, false, true)
        SetCurrentPedWeapon(ped, wep, true)
        TaskAimGunAtEntity(ped, playerPed, 10000, false)
    elseif kind == 'combat_shoot' then
        ClearPedTasks(ped)
        SetEntityInvincible(ped, false)
        SetPedCombatAttributes(ped, 17, false)
        SetPedCombatAttributes(ped, 46, true)
        local wepName = (type(actionArgs.weapon) == 'string' and actionArgs.weapon ~= '') and actionArgs.weapon or def.weapon or 'WEAPON_PISTOL'
        local wep = GetHashKey(wepName)
        GiveWeaponToPed(ped, wep, 120, false, true)
        SetCurrentPedWeapon(ped, wep, true)
        TaskShootAtEntity(ped, playerPed, 8000, GetHashKey('FIRING_PATTERN_FULL_AUTO'))
        TaskCombatPed(ped, playerPed, 0, 16)
    elseif kind == 'holster_weapon' then
        SetCurrentPedWeapon(ped, GetHashKey('WEAPON_UNARMED'), true)
        ClearPedTasks(ped)
        TaskLookAtEntity(ped, playerPed, -1, 2048, 3)
    elseif kind == 'tase' then
        ClearPedTasks(ped)
        local wep = GetHashKey(def.weapon or 'WEAPON_STUNGUN')
        GiveWeaponToPed(ped, wep, 10, false, true)
        SetCurrentPedWeapon(ped, wep, true)
        TaskShootAtEntity(ped, playerPed, 2500, GetHashKey('FIRING_PATTERN_SINGLE_SHOT'))
    elseif kind == 'combat_melee' then
        ClearPedTasks(ped)
        SetEntityInvincible(ped, false)
        SetPedCombatAttributes(ped, 17, false)
        SetPedCombatAttributes(ped, 46, true)
        local wepName = (type(actionArgs.weapon) == 'string' and actionArgs.weapon ~= '') and actionArgs.weapon or def.weapon or 'WEAPON_UNARMED'
        local wep = GetHashKey(wepName)
        if wepName ~= 'WEAPON_UNARMED' then
            GiveWeaponToPed(ped, wep, 1, false, true)
        end
        SetCurrentPedWeapon(ped, wep, true)
        TaskCombatPed(ped, playerPed, 0, 16)
    elseif kind == 'heal' then
        ClearPedTasks(ped)
        TaskStartScenarioInPlace(ped, 'CODE_HUMAN_MEDIC_TEND_TO_DEAD', 0, true)
        SetEntityHealth(playerPed, GetEntityMaxHealth(playerPed))
        SetPedArmour(playerPed, 100)
        CreateThread(function()
            Wait(3500)
            if DoesEntityExist(ped) then
                ClearPedTasks(ped)
                TaskLookAtEntity(ped, PlayerPedId(), -1, 2048, 3)
            end
        end)
    elseif kind == 'defend_player' then
        ClearPedTasks(ped)
        local wepName = (type(actionArgs.weapon) == 'string' and actionArgs.weapon ~= '') and actionArgs.weapon or def.weapon or 'WEAPON_COMBATPISTOL'
        allyPedWithPlayer(ped, playerPed, wepName)
        local enemyPed = findNearestTargetPed(ped, playerPed, 45.0, false, true)
        if enemyPed ~= 0 and DoesEntityExist(enemyPed) then
            SetEntityInvincible(enemyPed, false)
            TaskCombatPed(ped, enemyPed, 0, 16)
        else
            TaskFollowToOffsetOfEntity(ped, playerPed, 0.8, -1.5, 0.0, 2.5, -1, 2.0, true)
        end
    elseif kind == 'call_backup' then
        ClearPedTasks(ped)
        local dict = 'random@arrests'
        RequestAnimDict(dict)
        local units = math.max(1, math.min(2, tonumber(actionArgs.units) or 1))
        CreateThread(function()
            local timeout = GetGameTimer() + 2000
            while not HasAnimDictLoaded(dict) and GetGameTimer() < timeout do
                Wait(10)
            end
            if DoesEntityExist(ped) and HasAnimDictLoaded(dict) then
                TaskPlayAnim(ped, dict, 'generic_radio_chatter', 8.0, -8.0, 2500, 49, 0.0, false, false, false)
            end
            Wait(1600)
            allyPedWithPlayer(ped, playerPed, 'WEAPON_COMBATPISTOL')
            for i = 1, units do
                local backupNpc = DemoClient.SpawnNpcInFront({
                    preset = 'cop',
                    roleTitle = 'LSPD Backup',
                    modelName = (i % 2 == 1) and 's_m_y_cop_01' or 's_f_y_cop_01',
                    voice = (i % 2 == 1) and 'male' or 'female'
                })
                if backupNpc and DoesEntityExist(backupNpc.ped) then
                    FreezeEntityPosition(backupNpc.ped, false)
                    allyPedWithPlayer(backupNpc.ped, playerPed, 'WEAPON_COMBATPISTOL')
                    local enemyPed = findNearestTargetPed(backupNpc.ped, playerPed, 45.0, false, true)
                    if enemyPed ~= 0 and DoesEntityExist(enemyPed) then
                        SetEntityInvincible(enemyPed, false)
                        TaskCombatPed(backupNpc.ped, enemyPed, 0, 16)
                    else
                        TaskFollowToOffsetOfEntity(backupNpc.ped, playerPed, (i == 1) and -1.2 or 1.2, -2.0, 0.0, 2.5, -1, 2.0, true)
                    end
                end
            end
            SendNUIMessage({ type = 'npcList', npcs = DemoClient.GetNpcSummaryList() })
        end)
    elseif kind == 'arrest_npc' then
        local suspect = findNearestTargetPed(ped, playerPed, 30.0, false, true)
        if suspect ~= 0 and DoesEntityExist(suspect) then
            ClearPedTasks(ped)
            local wep = GetHashKey('WEAPON_COMBATPISTOL')
            GiveWeaponToPed(ped, wep, 60, false, true)
            SetCurrentPedWeapon(ped, wep, true)
            TaskGoToEntityWhileAimingAtEntity(ped, suspect, suspect, 2.0, false, 2.5, 4.0, true, false, GetHashKey('FIRING_PATTERN_FULL_AUTO'))
            FreezeEntityPosition(suspect, false)
            ClearPedTasks(suspect)
            TaskHandsUp(suspect, 20000, ped, -1, false)
        end
    elseif kind == 'wanted_level' then
        local stars = math.max(0, math.min(5, tonumber(actionArgs.stars) or def.stars or 0))
        local pid = PlayerId()
        if stars == 0 then
            ClearPlayerWantedLevel(pid)
        else
            SetPlayerWantedLevel(pid, stars, false)
            SetPlayerWantedLevelNow(pid, false)
        end
    elseif kind == 'flashlight' then
        ClearPedTasks(ped)
        local wep = GetHashKey('WEAPON_FLASHLIGHT')
        GiveWeaponToPed(ped, wep, 1, false, true)
        SetCurrentPedWeapon(ped, wep, true)
        TaskLookAtEntity(ped, playerPed, -1, 2048, 3)
    elseif kind == 'hand_over_cash' then
        ClearPedTasks(ped)
        TaskTurnPedToFaceEntity(ped, playerPed, 1000)
        playGiveItemAnim(ped)
        local dropCoords = GetOffsetFromEntityInWorldCoords(ped, 0.0, 0.85, 0.2)
        CreateAmbientPickup(GetHashKey('PICKUP_MONEY_VARIABLE'), dropCoords.x, dropCoords.y, dropCoords.z, 0, 500, 1, false, true)
    elseif kind == 'call_911' then
        ClearPedTasks(ped)
        TaskStartScenarioInPlace(ped, 'WORLD_HUMAN_STAND_MOBILE', 0, true)
        local pid = PlayerId()
        if GetPlayerWantedLevel(pid) < 2 then
            SetPlayerWantedLevel(pid, 2, false)
            SetPlayerWantedLevelNow(pid, false)
        end
    elseif kind == 'revive_npc' then
        local downedPed = findNearestTargetPed(ped, playerPed, 35.0, true, false)
        if downedPed ~= 0 and DoesEntityExist(downedPed) then
            ClearPedTasks(ped)
            local dPos = GetEntityCoords(downedPed)
            TaskGoToCoordAnyMeans(ped, dPos.x, dPos.y, dPos.z, 2.0, 0, false, 786603, 0xbf800000)
            CreateThread(function()
                local t = GetGameTimer() + 8000
                while DoesEntityExist(ped) and DoesEntityExist(downedPed) and #(GetEntityCoords(ped) - GetEntityCoords(downedPed)) > 2.2 and GetGameTimer() < t do
                    Wait(100)
                end
                if DoesEntityExist(ped) then
                    TaskStartScenarioInPlace(ped, 'CODE_HUMAN_MEDIC_TEND_TO_DEAD', 0, true)
                    Wait(3500)
                    if DoesEntityExist(downedPed) then
                        local pos = GetEntityCoords(downedPed)
                        local heading = GetEntityHeading(downedPed)
                        ResurrectPed(downedPed)
                        ReviveInjuredPed(downedPed)
                        SetEntityHealth(downedPed, 200)
                        ClearPedBloodDamage(downedPed)
                        SetEntityCoordsNoOffset(downedPed, pos.x, pos.y, pos.z + 0.2, false, false, false)
                        SetEntityHeading(downedPed, heading)
                        ClearPedTasksImmediately(downedPed)
                        for _, existingNpc in ipairs(DemoClient.spawnedNpcs) do
                            if existingNpc.ped == downedPed then
                                existingNpc.disabled = false
                                SendNUIMessage({ type = 'npcList', npcs = DemoClient.GetNpcSummaryList() })
                                break
                            end
                        end
                    end
                    if DoesEntityExist(ped) then
                        ClearPedTasks(ped)
                        TaskLookAtEntity(ped, PlayerPedId(), -1, 2048, 3)
                    end
                end
            end)
        end
    elseif kind == 'give_armor' then
        ClearPedTasks(ped)
        TaskTurnPedToFaceEntity(ped, playerPed, 1000)
        playGiveItemAnim(ped)
        SetPedArmour(playerPed, 100)
    elseif kind == 'recruit_follower' then
        ClearPedTasks(ped)
        local wepName = (type(actionArgs.weapon) == 'string' and actionArgs.weapon ~= '') and actionArgs.weapon or def.weapon or 'WEAPON_MICROSMG'
        allyPedWithPlayer(ped, playerPed, wepName)
        TaskFollowToOffsetOfEntity(ped, playerPed, 0.0, -1.6, 0.0, 2.5, -1, 2.0, true)
    elseif kind == 'unlock_vehicle' then
        local veh = getTargetVehicleForPlayer(playerPed)
        if veh ~= 0 and DoesEntityExist(veh) then
            playGiveItemAnim(ped)
            SetVehicleDoorsLocked(veh, 1)
            SetVehicleDoorsLockedForAllPlayers(veh, false)
            SetVehicleNeedsToBeHotwired(veh, false)
            SetVehicleEngineOn(veh, true, true, false)
        end
    end
end

-- ============================================================================
-- Server -> Client NPC Net Events
-- ============================================================================

RegisterNetEvent('eevs_demo:npcInteractionState', function(active, npcId, _reason)
    if active then
        DemoClient.activeInteractingNpcId = tonumber(npcId)
    elseif DemoClient.activeInteractingNpcId == tonumber(npcId) or not npcId then
        DemoClient.activeInteractingNpcId = nil
    end
end)

RegisterNetEvent('eevs_demo:npcExecuteAction', function(npcId, actionName, actionArgs)
    local npc = DemoClient.GetNpcById(npcId)
    if npc then
        npc.thinking = false
        npc.thinkingStage = nil
        DemoClient.ExecuteNpcAction(npc, actionName, actionArgs)
    end
end)

RegisterNetEvent('eevs_demo:npcThinking', function(npcId, isThinking, stageText)
    local npc = DemoClient.GetNpcById(npcId)
    if npc then
        npc.thinking = (isThinking == true)
        npc.thinkingStage = stageText or 'Thinking...'
    end
end)

RegisterNetEvent('eevs_demo:npcSpeaking', function(payload)
    if type(payload) ~= 'table' then return end
    local npcId = tonumber(payload.npcId)
    local clipId = payload.clipId or 'clip'
    local dur = tonumber(payload.durationMs) or 2500
    local actionName = payload.actionName
    local speechText = payload.speechText
    local actionArgs = payload.actionArgs
    local npcPedNetId = tonumber(payload.npcPedNetId) or 0
    local ownerServerId = tonumber(payload.ownerServerId)
    local hearingRadius = tonumber(payload.hearingRadius) or 35.0

    local matchedLocal = false
    local localServerId = GetPlayerServerId(PlayerId())

    for _, npc in ipairs(DemoClient.spawnedNpcs) do
        local isMatch = (ownerServerId == localServerId and npc.npcId == npcId)
            or (npcPedNetId > 0 and npc.pedNetId == npcPedNetId)

        if isMatch and DoesEntityExist(npc.ped) then
            matchedLocal = true
            npc.thinking = false
            npc.thinkingStage = nil
            npc.speakingUntil = GetGameTimer() + dur
            npc.activeClipId = clipId
            if speechText and speechText ~= '' then
                npc.speechText = tostring(speechText)
                npc.speechExpiresAt = GetGameTimer() + math.max(dur + 2000, 6000)
            end
            RequestAnimDict('mp_facial')
            RequestAnimDict('facials@gen_male@variations@normal')
            PlayFacialAnim(npc.ped, 'mic_chatter', 'mp_facial')
            if ownerServerId == localServerId and actionName and actionName ~= '' and actionName ~= 'none' then
                DemoClient.ExecuteNpcAction(npc, actionName, actionArgs)
            else
                TaskLookAtEntity(npc.ped, PlayerPedId(), dur + 1000, 2048, 3)
            end
            break
        end
    end

    if not matchedLocal and npcPedNetId > 0 then
        DemoClient.remoteNpcBubbles[npcPedNetId] = {
            clipId = clipId,
            speakingUntil = GetGameTimer() + dur,
            speechText = (speechText and speechText ~= '') and tostring(speechText) or nil,
            speechExpiresAt = GetGameTimer() + math.max(dur + 2000, 6000),
            hearingRadius = hearingRadius
        }
        if NetworkDoesNetworkIdExist(npcPedNetId) then
            local ped = NetToPed(npcPedNetId)
            if ped and ped ~= 0 and DoesEntityExist(ped) then
                RequestAnimDict('mp_facial')
                RequestAnimDict('facials@gen_male@variations@normal')
                PlayFacialAnim(ped, 'mic_chatter', 'mp_facial')
            end
        end
    end
end)
