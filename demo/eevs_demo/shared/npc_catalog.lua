-- ============================================================================
-- EEVS Interactive NPC Catalog (Shared between Server, Client & NUI)
-- ============================================================================
-- Single source of truth for:
--   1. `NPC_ACTION_LIST` & `NPC_ACTIONS_BY_NAME`
--   2. `NPC_PRESETS` & `NPC_PRESET_DISABLED_ACTIONS`
--   3. `BuildOpenAiNpcTools(enabledActions)`
--   4. `GetNpcCatalogPayload()` (sent to NUI so JS never duplicates definitions)
-- ============================================================================

NPC_ACTION_LIST = {
    {
        name = 'follow_player',
        shortDesc = 'Follow behind the player',
        description = 'Walk and follow behind the player.',
        type = 'follow'
    },
    {
        name = 'stop_moving',
        shortDesc = 'Stand still / stop moving',
        description = 'Stand still in place and stop following or walking.',
        type = 'stop_moving'
    },
    {
        name = 'come_closer',
        shortDesc = 'Step closer to the player',
        description = 'Step closer to the player.',
        type = 'come_closer'
    },
    {
        name = 'walk_away',
        shortDesc = 'End conversation & walk away',
        description = 'End the conversation and walk away.',
        type = 'walk_away',
        endInteraction = true
    },
    {
        name = 'flee',
        shortDesc = 'Run away in fear or panic',
        description = 'Run away from the player in fear or panic.',
        type = 'flee',
        endInteraction = true
    },
    {
        name = 'hands_up',
        shortDesc = 'Put hands up to surrender',
        description = 'Put both hands up in the air to surrender or beg for your life.',
        type = 'hands_up',
        duration = 15000
    },
    {
        name = 'salute',
        shortDesc = 'Perform military/police salute',
        description = 'Perform a military/police salute gesture.',
        type = 'anim',
        dict = 'anim@mp_player_intincarsalutestd@ds@',
        anim = 'idle_a',
        duration = 3000,
        flag = 49
    },
    {
        name = 'clipboard',
        shortDesc = 'Pull out a clipboard',
        description = 'Pull out a clipboard to take notes or inspect.',
        type = 'scenario',
        scenario = 'WORLD_HUMAN_CLIPBOARD'
    },
    {
        name = 'cop_idle',
        shortDesc = 'Stand in police patrol stance',
        description = 'Stand in an authoritative police officer patrol stance.',
        type = 'scenario',
        scenario = 'WORLD_HUMAN_COP_IDLES'
    },
    {
        name = 'smoke',
        shortDesc = 'Smoke a cigarette',
        description = 'Light and smoke a cigarette.',
        type = 'scenario',
        scenario = 'WORLD_HUMAN_SMOKING'
    },
    {
        name = 'drink_coffee',
        shortDesc = 'Drink a cup of coffee',
        description = 'Hold and drink a cup of coffee.',
        type = 'scenario',
        scenario = 'WORLD_HUMAN_AA_COFFEE'
    },
    {
        name = 'sit_ground',
        shortDesc = 'Sit down on the ground',
        description = 'Sit down on the ground to relax.',
        type = 'scenario',
        scenario = 'WORLD_HUMAN_PICNIC'
    },
    {
        name = 'stand_up',
        shortDesc = 'Stand back up on feet',
        description = 'Stand back up on your feet, cancel any sitting/cowering/idle posture, and face the player.',
        type = 'stop_moving'
    },
    {
        name = 'kneel_inspect',
        shortDesc = 'Kneel down to inspect',
        description = 'Kneel down to examine injuries or inspect something closely.',
        type = 'scenario',
        scenario = 'CODE_HUMAN_MEDIC_TEND_TO_DEAD'
    },
    {
        name = 'enter_passenger',
        shortDesc = 'Enter passenger seat',
        description = 'Get into the passenger seat of the player\'s vehicle.',
        type = 'enter_vehicle',
        seat = 0
    },
    {
        name = 'enter_driver',
        shortDesc = 'Enter driver seat',
        description = 'Get into the driver seat of the nearby vehicle.',
        type = 'enter_vehicle',
        seat = -1
    },
    {
        name = 'exit_vehicle',
        shortDesc = 'Step out of vehicle',
        description = 'Step out of the current vehicle.',
        type = 'exit_vehicle'
    },
    {
        name = 'drive_to_waypoint',
        shortDesc = 'Drive to player waypoint',
        description = 'Get into the driver seat and drive the vehicle to the player\'s map waypoint.',
        type = 'drive_to_waypoint'
    },
    {
        name = 'repair_vehicle',
        shortDesc = 'Inspect & repair vehicle',
        description = 'Walk up to the player\'s vehicle, inspect the engine, and repair all damage.',
        type = 'repair_vehicle'
    },
    {
        name = 'aim_weapon',
        shortDesc = 'Draw & aim firearm',
        description = 'Draw a firearm and aim it at the player as a warning.',
        type = 'aim_weapon',
        weapon = 'WEAPON_PISTOL'
    },
    {
        name = 'shoot_player',
        shortDesc = 'Shoot pistol at player',
        description = 'Open fire on the player with a pistol ONLY when the player is an enemy/suspect you want to kill. NEVER use this when helping or defending the player.',
        type = 'combat_shoot',
        weapon = 'WEAPON_PISTOL',
        endInteraction = true
    },
    {
        name = 'shoot_smg',
        shortDesc = 'Shoot Micro SMG at player',
        description = 'Open fire on the player with an automatic Micro SMG ONLY when the player is an enemy you want to kill.',
        type = 'combat_shoot',
        weapon = 'WEAPON_MICROSMG',
        endInteraction = true
    },
    {
        name = 'shoot_shotgun',
        shortDesc = 'Shoot shotgun at player',
        description = 'Open fire on the player with a pump shotgun ONLY when the player is an enemy you want to kill.',
        type = 'combat_shoot',
        weapon = 'WEAPON_PUMPSHOTGUN',
        endInteraction = true
    },
    {
        name = 'defend_player',
        shortDesc = 'Defend player against enemies',
        description = 'Protect and defend the player! Equips a weapon, allies with the player, and shoots/attacks any nearby hostile NPCs or enemies threatening the player.',
        type = 'defend_player',
        weapon = 'WEAPON_COMBATPISTOL'
    },
    {
        name = 'call_police_backup',
        shortDesc = 'Radio LSPD backup officers',
        description = 'Call LSPD dispatch on the shoulder radio to spawn armed police backup officers who protect the player and attack nearby enemies.',
        type = 'call_backup'
    },
    {
        name = 'arrest_nearby_npc',
        shortDesc = 'Arrest nearest suspect NPC',
        description = 'Walk up to the nearest other NPC/suspect at gunpoint and force them to kneel and surrender.',
        type = 'arrest_npc'
    },
    {
        name = 'clear_wanted_level',
        shortDesc = 'Clear player wanted stars',
        description = 'Radio police dispatch to clear/remove the player\'s wanted stars and call off police pursuit.',
        type = 'wanted_level',
        stars = 0
    },
    {
        name = 'give_wanted_level',
        shortDesc = 'Report player wanted stars',
        description = 'Report the player to police dispatch as a criminal suspect and assign wanted stars.',
        type = 'wanted_level',
        stars = 2
    },
    {
        name = 'flashlight_inspect',
        shortDesc = 'Shine police flashlight',
        description = 'Equip a police flashlight and shine it around to inspect the area or person.',
        type = 'flashlight'
    },
    {
        name = 'hand_over_cash',
        shortDesc = 'Hand over cash pickup',
        description = 'Hand over cash/money from the register or pockets to the player (e.g. during a store robbery).',
        type = 'hand_over_cash'
    },
    {
        name = 'cower_on_ground',
        shortDesc = 'Cower in terror on ground',
        description = 'Drop down and cower in terror covering your head.',
        type = 'anim',
        dict = 'amb@code_human_cower@male@base',
        anim = 'base',
        duration = 12000,
        flag = 1
    },
    {
        name = 'call_911',
        shortDesc = 'Call 911 on mobile phone',
        description = 'Pull out a mobile phone to call 911 emergency police on the player.',
        type = 'call_911'
    },
    {
        name = 'revive_nearby_npc',
        shortDesc = 'Revive nearest downed NPC',
        description = 'Walk over to the nearest dead or injured NPC, perform emergency CPR/medical treatment, and revive them.',
        type = 'revive_npc'
    },
    {
        name = 'give_armor',
        shortDesc = 'Give body armor to player',
        description = 'Hand a body armor vest / tactical protection to the player.',
        type = 'give_armor'
    },
    {
        name = 'gang_sign',
        shortDesc = 'Throw street gang signs',
        description = 'Throw street gang hand signs to represent your set or intimidate.',
        type = 'anim',
        dict = 'mp_player_int_uppergang_sign_a',
        anim = 'mp_player_int_gang_sign_a',
        duration = 4000,
        flag = 49
    },
    {
        name = 'recruit_follower',
        shortDesc = 'Join player squad as bodyguard',
        description = 'Join the player\'s squad/crew as an armed bodyguard who follows them into vehicles and fights their enemies.',
        type = 'recruit_follower',
        weapon = 'WEAPON_MICROSMG'
    },
    {
        name = 'unlock_vehicle',
        shortDesc = 'Unlock nearby vehicle doors',
        description = 'Unlock the doors of the player\'s nearby vehicle and start its engine.',
        type = 'unlock_vehicle'
    },
    {
        name = 'holster_weapon',
        shortDesc = 'Holster weapon & calm down',
        description = 'Put away any weapon and return to a calm, unarmed stance.',
        type = 'holster_weapon'
    },
    {
        name = 'tase_player',
        shortDesc = 'Fire stun gun (taser) at player',
        description = 'Fire a less-lethal stun gun (taser) at an uncooperative suspect.',
        type = 'tase',
        weapon = 'WEAPON_STUNGUN'
    },
    {
        name = 'attack_melee',
        shortDesc = 'Fight player with fists',
        description = 'Fight the player in hand-to-hand fist combat.',
        type = 'combat_melee',
        weapon = 'WEAPON_UNARMED',
        endInteraction = true
    },
    {
        name = 'attack_bat',
        shortDesc = 'Attack player with baseball bat',
        description = 'Pull out a baseball bat and attack the player in melee combat.',
        type = 'combat_melee',
        weapon = 'WEAPON_BAT',
        endInteraction = true
    },
    {
        name = 'attack_knife',
        shortDesc = 'Attack player with knife',
        description = 'Pull out a knife and attack the player in melee combat.',
        type = 'combat_melee',
        weapon = 'WEAPON_KNIFE',
        endInteraction = true
    },
    {
        name = 'heal_player',
        shortDesc = 'Heal player health & armor',
        description = 'Treat the player\'s injuries and restore their health and armor to full.',
        type = 'heal'
    }
}

NPC_ACTIONS_BY_NAME = {}
for _, def in ipairs(NPC_ACTION_LIST) do
    NPC_ACTIONS_BY_NAME[def.name] = def
end

NPC_PRESETS = {
    cop = {
        title = 'LSPD Officer',
        models = {
            female = 's_f_y_cop_01',
            male = 's_m_y_cop_01'
        },
        voiceInstruct = {
            female = 'A sharp, composed female LSPD police officer in her late 20s with a confident, authoritative American voice.',
            male = 'A stern, authoritative male LSPD police officer in his 30s with a clear, commanding American voice.'
        },
        prompt = 'You are an LSPD police officer on patrol in Los Santos (GTA V). If a citizen asks for help, protection, or backup against enemies, be supportive and protect them. If the citizen themselves disrespects, threatens, or attacks you, be strict and aggressive. Keep spoken replies in-character, natural, and concise (1 to 2 sentences max).',
        disabledActions = {
            'flee', 'hands_up', 'cower_on_ground', 'hand_over_cash', 'call_911',
            'gang_sign', 'shoot_smg', 'attack_bat', 'attack_knife', 'heal_player', 'revive_nearby_npc'
        }
    },
    medic = {
        title = 'Paramedic',
        models = {
            female = 's_f_y_scrubs_01',
            male = 's_m_m_doctor_01'
        },
        voiceInstruct = {
            female = 'A calm, caring female emergency paramedic in her 30s with an urgent, reassuring American voice.',
            male = 'A seasoned, professional male emergency doctor in his 40s with a steady, urgent, reassuring voice.'
        },
        prompt = 'You are an emergency paramedic and doctor from Pillbox Hill Medical Center in Los Santos (GTA V). You care about saving lives, checking injuries, healing citizens, and reviving downed people. Keep spoken replies professional, urgent, and concise (1 to 2 sentences max).',
        disabledActions = {
            'salute', 'cop_idle', 'aim_weapon', 'shoot_player', 'shoot_smg', 'shoot_shotgun',
            'defend_player', 'call_police_backup', 'arrest_nearby_npc', 'clear_wanted_level',
            'flashlight_inspect', 'hand_over_cash', 'gang_sign', 'recruit_follower', 'tase_player',
            'attack_melee', 'attack_bat', 'attack_knife'
        }
    },
    clerk = {
        title = 'Store Clerk',
        models = {
            female = 's_f_y_shop_low',
            male = 'mp_m_shopkeep_01'
        },
        voiceInstruct = {
            female = 'A friendly, casual young female convenience store clerk in her 20s with a warm, natural customer-service voice.',
            male = 'A friendly, laid-back male convenience store clerk in his 30s with a casual, conversational voice.'
        },
        prompt = 'You are a friendly, polite convenience store clerk working the counter in Los Santos (GTA V). During normal conversation or when a customer greets you or asks questions, be welcoming, casual, and helpful with no physical action ("action": "none"). ONLY get scared, panic, hand over cash, surrender, cower, or flee if the player explicitly robs, threatens, or attacks you. Keep spoken replies in-character, natural, and concise (1 to 2 sentences max).',
        disabledActions = {
            'salute', 'cop_idle', 'drive_to_waypoint', 'repair_vehicle', 'shoot_smg', 'defend_player',
            'call_police_backup', 'arrest_nearby_npc', 'clear_wanted_level', 'flashlight_inspect',
            'revive_nearby_npc', 'give_armor', 'gang_sign', 'recruit_follower', 'unlock_vehicle',
            'tase_player', 'attack_knife', 'heal_player'
        }
    },
    gang = {
        title = 'Gang Member',
        models = {
            female = 'g_f_y_ballas_01',
            male = 'g_m_y_famca_01'
        },
        voiceInstruct = {
            female = 'A fierce, streetwise female Los Santos gang member in her 20s with an aggressive, confrontational urban tone.',
            male = 'An aggressive, streetwise male Los Santos gang member in his 20s with a rough, gravelly, hostile urban voice.'
        },
        prompt = 'You are a hostile, hot-headed Los Santos street gang member (GTA V) who hates outsiders stepping onto your turf. You are extremely aggressive and would much rather shoot, aim a gun, brawl, or throw gang signs than waste time talking—unless the player earns your respect and recruits you. Keep any spoken words harsh, streetwise, and very brief (1 short sentence max).',
        disabledActions = {
            'salute', 'clipboard', 'cop_idle', 'kneel_inspect', 'call_police_backup', 'arrest_nearby_npc',
            'clear_wanted_level', 'give_wanted_level', 'flashlight_inspect', 'hand_over_cash',
            'cower_on_ground', 'call_911', 'revive_nearby_npc', 'tase_player', 'heal_player'
        }
    },
    custom = {
        title = 'Custom NPC',
        models = {
            female = 'a_f_y_business_01',
            male = 'a_m_y_business_01'
        },
        voiceInstruct = {
            female = 'A natural, expressive female character in her late 20s speaking clearly.',
            male = 'A natural, expressive male character in his 30s speaking clearly.'
        },
        prompt = nil,
        disabledActions = {}
    }
}

-- Builds the OpenAI `/v1/chat/completions` `tools` array from `NPC_ACTION_LIST`,
-- filtered by `enabledActions` (an optional array of enabled action names; if nil/empty, all actions are included).
function BuildOpenAiNpcTools(enabledActions)
    local allowedSet = nil
    if type(enabledActions) == 'table' and #enabledActions > 0 then
        allowedSet = {}
        for _, name in ipairs(enabledActions) do
            if type(name) == 'string' and name ~= '' then
                allowedSet[string.lower(name)] = true
            end
        end
    end

    local actionNames = { 'none' }
    local actionHelpLines = { '- "none": Speak only without any physical action.' }

    for _, def in ipairs(NPC_ACTION_LIST) do
        if not allowedSet or allowedSet[def.name] then
            table.insert(actionNames, def.name)
            table.insert(actionHelpLines, ('- "%s": %s'):format(def.name, def.description))
        end
    end

    return {
        {
            type = 'function',
            ['function'] = {
                name = 'npc_respond',
                description = 'Respond to the player with spoken dialogue and/or a physical in-game action.',
                parameters = {
                    type = 'object',
                    properties = {
                        speech = {
                            type = 'string',
                            description = 'Spoken in-character words to say out loud (1-2 short sentences), or empty string "" for a silent physical action.'
                        },
                        action = {
                            type = 'string',
                            enum = actionNames,
                            description = 'Physical in-game action to perform:\n' .. table.concat(actionHelpLines, '\n')
                        }
                    },
                    required = { 'speech', 'action' }
                }
            }
        }
    }
end

function GetNpcCatalogPayload()
    local actions = {}
    for _, def in ipairs(NPC_ACTION_LIST) do
        table.insert(actions, {
            name = def.name,
            desc = def.shortDesc or def.description
        })
    end
    return {
        actions = actions,
        presets = NPC_PRESETS
    }
end
