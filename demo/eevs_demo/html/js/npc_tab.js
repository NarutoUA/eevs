// ============================================================================
// EEVS NUI - Interactive NPC & LLM Tab (Catalog synced from shared/npc_catalog.lua)
// ============================================================================
(() => {
    const EevsUI = window.EevsUI;
    const { dom } = EevsUI;

    // Default fallback catalog (Used in standalone browser mode; in FiveM NUI mode,
    // `EevsUI.applyNpcCatalog` populates this directly from `shared/npc_catalog.lua`)
    let npcActionsCatalog = [
        { name: 'follow_player', desc: 'Follow behind the player' },
        { name: 'stop_moving', desc: 'Stand still / stop moving' },
        { name: 'come_closer', desc: 'Step closer to the player' },
        { name: 'walk_away', desc: 'End conversation & walk away' },
        { name: 'flee', desc: 'Run away in fear or panic' },
        { name: 'hands_up', desc: 'Put hands up to surrender' },
        { name: 'salute', desc: 'Perform military/police salute' },
        { name: 'clipboard', desc: 'Pull out a clipboard' },
        { name: 'cop_idle', desc: 'Stand in police patrol stance' },
        { name: 'smoke', desc: 'Smoke a cigarette' },
        { name: 'drink_coffee', desc: 'Drink a cup of coffee' },
        { name: 'sit_ground', desc: 'Sit down on the ground' },
        { name: 'stand_up', desc: 'Stand back up on feet' },
        { name: 'kneel_inspect', desc: 'Kneel down to inspect' },
        { name: 'enter_passenger', desc: 'Enter passenger seat' },
        { name: 'enter_driver', desc: 'Enter driver seat' },
        { name: 'exit_vehicle', desc: 'Step out of vehicle' },
        { name: 'drive_to_waypoint', desc: 'Drive to player waypoint' },
        { name: 'repair_vehicle', desc: 'Inspect & repair vehicle' },
        { name: 'aim_weapon', desc: 'Draw & aim firearm' },
        { name: 'shoot_player', desc: 'Shoot pistol at player' },
        { name: 'shoot_smg', desc: 'Shoot Micro SMG at player' },
        { name: 'shoot_shotgun', desc: 'Shoot shotgun at player' },
        { name: 'defend_player', desc: 'Defend player against enemies' },
        { name: 'call_police_backup', desc: 'Radio LSPD backup officers' },
        { name: 'arrest_nearby_npc', desc: 'Arrest nearest suspect NPC' },
        { name: 'clear_wanted_level', desc: 'Clear player wanted stars' },
        { name: 'give_wanted_level', desc: 'Report player wanted stars' },
        { name: 'flashlight_inspect', desc: 'Shine police flashlight' },
        { name: 'hand_over_cash', desc: 'Hand over cash pickup' },
        { name: 'cower_on_ground', desc: 'Cower in terror on ground' },
        { name: 'call_911', desc: 'Call 911 on mobile phone' },
        { name: 'revive_nearby_npc', desc: 'Revive nearest downed NPC' },
        { name: 'give_armor', desc: 'Give body armor to player' },
        { name: 'gang_sign', desc: 'Throw street gang signs' },
        { name: 'recruit_follower', desc: 'Join player squad as bodyguard' },
        { name: 'unlock_vehicle', desc: 'Unlock nearby vehicle doors' },
        { name: 'holster_weapon', desc: 'Holster weapon & calm down' },
        { name: 'tase_player', desc: 'Fire stun gun (taser) at player' },
        { name: 'attack_melee', desc: 'Fight player with fists' },
        { name: 'attack_bat', desc: 'Attack player with baseball bat' },
        { name: 'attack_knife', desc: 'Attack player with knife' },
        { name: 'heal_player', desc: 'Heal player health & armor' }
    ];

    let npcPresets = {
        cop: {
            title: 'LSPD Officer',
            models: { female: 's_f_y_cop_01', male: 's_m_y_cop_01' },
            voiceInstruct: {
                female: 'A sharp, composed female LSPD police officer in her late 20s with a confident, authoritative American voice.',
                male: 'A stern, authoritative male LSPD police officer in his 30s with a clear, commanding American voice.'
            },
            prompt: 'You are an LSPD police officer on patrol in Los Santos (GTA V). If a citizen asks for help, protection, or backup against enemies, be supportive and protect them. If the citizen themselves disrespects, threatens, or attacks you, be strict and aggressive. Keep spoken replies in-character, natural, and concise (1 to 2 sentences max).',
            disabledActions: [
                'flee', 'hands_up', 'cower_on_ground', 'hand_over_cash', 'call_911',
                'gang_sign', 'shoot_smg', 'attack_bat', 'attack_knife', 'heal_player', 'revive_nearby_npc'
            ]
        },
        medic: {
            title: 'Paramedic',
            models: { female: 's_f_y_scrubs_01', male: 's_m_m_doctor_01' },
            voiceInstruct: {
                female: 'A calm, caring female emergency paramedic in her 30s with an urgent, reassuring American voice.',
                male: 'A seasoned, professional male emergency doctor in his 40s with a steady, urgent, reassuring voice.'
            },
            prompt: 'You are an emergency paramedic and doctor from Pillbox Hill Medical Center in Los Santos (GTA V). You care about saving lives, checking injuries, healing citizens, and reviving downed people. Keep spoken replies professional, urgent, and concise (1 to 2 sentences max).',
            disabledActions: [
                'salute', 'cop_idle', 'aim_weapon', 'shoot_player', 'shoot_smg', 'shoot_shotgun',
                'defend_player', 'call_police_backup', 'arrest_nearby_npc', 'clear_wanted_level',
                'flashlight_inspect', 'hand_over_cash', 'gang_sign', 'recruit_follower', 'tase_player',
                'attack_melee', 'attack_bat', 'attack_knife'
            ]
        },
        clerk: {
            title: 'Store Clerk',
            models: { female: 's_f_y_shop_low', male: 'mp_m_shopkeep_01' },
            voiceInstruct: {
                female: 'A friendly, casual young female convenience store clerk in her 20s with a warm, natural customer-service voice.',
                male: 'A friendly, laid-back male convenience store clerk in his 30s with a casual, conversational voice.'
            },
            prompt: 'You are a friendly, polite convenience store clerk working the counter in Los Santos (GTA V). During normal conversation or when a customer greets you or asks questions, be welcoming, casual, and helpful with no physical action ("action": "none"). ONLY get scared, panic, hand over cash, surrender, cower, or flee if the player explicitly robs, threatens, or attacks you. Keep spoken replies in-character, natural, and concise (1 to 2 sentences max).',
            disabledActions: [
                'salute', 'cop_idle', 'drive_to_waypoint', 'repair_vehicle', 'shoot_smg', 'defend_player',
                'call_police_backup', 'arrest_nearby_npc', 'clear_wanted_level', 'flashlight_inspect',
                'revive_nearby_npc', 'give_armor', 'gang_sign', 'recruit_follower', 'unlock_vehicle',
                'tase_player', 'attack_knife', 'heal_player'
            ]
        },
        gang: {
            title: 'Gang Member',
            models: { female: 'g_f_y_ballas_01', male: 'g_m_y_famca_01' },
            voiceInstruct: {
                female: 'A fierce, streetwise female Los Santos gang member in her 20s with an aggressive, confrontational urban tone.',
                male: 'An aggressive, streetwise male Los Santos gang member in his 20s with a rough, gravelly, hostile urban voice.'
            },
            prompt: 'You are a hostile, hot-headed Los Santos street gang member (GTA V) who hates outsiders stepping onto your turf. You are extremely aggressive and would much rather shoot, aim a gun, brawl, or throw gang signs than waste time talking—unless the player earns your respect and recruits you. Keep any spoken words harsh, streetwise, and very brief (1 short sentence max).',
            disabledActions: [
                'salute', 'clipboard', 'cop_idle', 'kneel_inspect', 'call_police_backup', 'arrest_nearby_npc',
                'clear_wanted_level', 'give_wanted_level', 'flashlight_inspect', 'hand_over_cash',
                'cower_on_ground', 'call_911', 'revive_nearby_npc', 'tase_player', 'heal_player'
            ]
        },
        custom: {
            title: 'Custom NPC',
            models: { female: 'a_f_y_business_01', male: 'a_m_y_business_01' },
            voiceInstruct: {
                female: 'A natural, expressive female character in her late 20s speaking clearly.',
                male: 'A natural, expressive male character in his 30s speaking clearly.'
            },
            prompt: null,
            disabledActions: []
        }
    };

    let catalogInitializedFromLua = false;
    EevsUI.applyNpcCatalog = function applyNpcCatalog(catalog) {
        if (!catalog || catalogInitializedFromLua) return;
        if (Array.isArray(catalog.actions) && catalog.actions.length > 0) {
            npcActionsCatalog = catalog.actions;
        }
        if (catalog.presets && typeof catalog.presets === 'object') {
            npcPresets = catalog.presets;
        }
        catalogInitializedFromLua = true;
        updateNpcPresetUi(false, true);
    };

    EevsUI.renderNpcSelectors = function renderNpcSelectors(npcs) {
        EevsUI.spawnedNpcs = Array.isArray(npcs) ? npcs : [];
        const list = EevsUI.spawnedNpcs;
        if (dom.npcCountBadge) {
            dom.npcCountBadge.textContent = `${list.length} NPC${list.length === 1 ? '' : 's'}`;
        }
        if (dom.npcListCountBadge) {
            dom.npcListCountBadge.textContent = `${list.length} active`;
        }
        for (const sel of [dom.npcActiveSelect, dom.llmNpcSelect]) {
            if (!sel) continue;
            const prev = sel.value;
            sel.innerHTML = '<option value="self">👤 Yourself (Local Player)</option>';
            for (const n of list) {
                const opt = document.createElement('option');
                opt.value = `npc:${n.npcId}`;
                opt.textContent = `🧍 ${n.label}`;
                sel.appendChild(opt);
            }
            if (prev && Array.from(sel.options).some((o) => o.value === prev)) {
                sel.value = prev;
            } else if (list.length > 0) {
                sel.value = `npc:${list[list.length - 1].npcId}`;
            }
        }
        if (dom.npcManageList) {
            dom.npcManageList.innerHTML = '';
            if (list.length === 0) {
                dom.npcManageList.innerHTML = '<div class="empty-state" style="padding:10px;">No spawned NPCs. Click 🧍＋ Spawn NPC to create one.</div>';
            } else {
                for (const n of list) {
                    const genderLabel = (n.gender === 'male') ? '♂ Male' : '♀ Female';
                    const gainVal = (typeof n.voiceGain === 'number' && !Number.isNaN(n.voiceGain))
                        ? n.voiceGain
                        : 1.0;
                    const row = document.createElement('div');
                    row.className = 'npc-manage-row';
                    row.innerHTML = `
                        <div class="npc-manage-meta">
                            <strong style="color:#fff;">🧍 ${n.label}</strong>
                            <span class="pill pill-neutral">${genderLabel}</span>
                            <span class="mini-badge">Seed: ${n.voiceSeed || 'auto'}</span>
                        </div>
                        <div class="npc-manage-actions">
                            <label class="npc-gain-control" title="Adjust this NPC's voice volume multiplier">
                                <span>🔊 Gain</span>
                                <input type="range" min="0.2" max="3.0" step="0.1" value="${gainVal.toFixed(1)}" data-gain-npc-id="${n.npcId}" />
                                <span class="npc-gain-val" id="npc-gain-label-${n.npcId}">${gainVal.toFixed(1)}x</span>
                            </label>
                            <button type="button" class="btn btn-xs btn-danger-soft" data-delete-npc-id="${n.npcId}">🗑 Delete</button>
                        </div>
                    `;
                    dom.npcManageList.appendChild(row);
                }
            }
        }
    };

    if (dom.npcManageList) {
        dom.npcManageList.addEventListener('input', (ev) => {
            const slider = ev.target.closest('[data-gain-npc-id]');
            if (!slider) return;
            const npcId = parseInt(slider.getAttribute('data-gain-npc-id'), 10);
            const val = parseFloat(slider.value) || 1.0;
            const lbl = document.getElementById(`npc-gain-label-${npcId}`);
            if (lbl) {
                lbl.textContent = `${val.toFixed(1)}x`;
            }
        });

        dom.npcManageList.addEventListener('change', async (ev) => {
            const slider = ev.target.closest('[data-gain-npc-id]');
            if (!slider) return;
            const npcId = parseInt(slider.getAttribute('data-gain-npc-id'), 10);
            const voiceGain = parseFloat(slider.value) || 1.0;
            if (!npcId) return;
            await EevsUI.sendNpcAction('setNpcGain', { npcId, voiceGain });
            EevsUI.logEvent(`Updated NPC #${npcId} voice gain to ${voiceGain.toFixed(1)}x`);
        });

        dom.npcManageList.addEventListener('click', async (ev) => {
            const btn = ev.target.closest('[data-delete-npc-id]');
            if (!btn) return;
            const npcId = parseInt(btn.getAttribute('data-delete-npc-id'), 10);
            if (!npcId) return;
            const res = await EevsUI.sendNpcAction('delete', { npcId });
            if (res && res.ok) {
                EevsUI.logEvent(`Deleted NPC #${npcId}`);
            }
        });
    }

    function updateNpcToolsCountBadge() {
        if (!dom.npcToolsGrid || !dom.npcToolsCountBadge) return;
        const boxes = dom.npcToolsGrid.querySelectorAll('input[type="checkbox"][data-action-name]');
        let enabledCount = 0;
        boxes.forEach((cb) => {
            if (cb.checked) enabledCount++;
        });
        dom.npcToolsCountBadge.textContent = `${enabledCount} / ${boxes.length} enabled`;
    }

    function renderNpcToolsCheckboxes(presetKey = 'cop') {
        if (!dom.npcToolsGrid) return;
        const preset = npcPresets[presetKey] || npcPresets.custom;
        const disabledSet = new Set(Array.isArray(preset.disabledActions) ? preset.disabledActions : []);
        dom.npcToolsGrid.innerHTML = '';

        for (const item of npcActionsCatalog) {
            const label = document.createElement('label');
            label.className = 'tool-check-item';
            label.title = item.desc || item.name;

            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.dataset.actionName = item.name;
            cb.checked = !disabledSet.has(item.name);
            cb.addEventListener('change', () => {
                updateNpcToolsCountBadge();
                EevsUI.syncNpcConfigToClient();
            });

            const span = document.createElement('span');
            span.textContent = item.name;

            label.appendChild(cb);
            label.appendChild(span);
            dom.npcToolsGrid.appendChild(label);
        }
        updateNpcToolsCountBadge();
    }

    function getEnabledNpcActions() {
        if (!dom.npcToolsGrid) {
            return npcActionsCatalog.map((a) => a.name);
        }
        const checked = [];
        dom.npcToolsGrid.querySelectorAll('input[type="checkbox"][data-action-name]').forEach((cb) => {
            if (cb.checked && cb.dataset.actionName) {
                checked.push(cb.dataset.actionName);
            }
        });
        return checked;
    }

    const btnNpcToolsAll = document.getElementById('btnNpcToolsAll');
    if (btnNpcToolsAll) {
        btnNpcToolsAll.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            if (!dom.npcToolsGrid) return;
            dom.npcToolsGrid.querySelectorAll('input[type="checkbox"][data-action-name]').forEach((cb) => {
                cb.checked = true;
            });
            updateNpcToolsCountBadge();
            EevsUI.syncNpcConfigToClient();
        });
    }

    const btnNpcToolsNone = document.getElementById('btnNpcToolsNone');
    if (btnNpcToolsNone) {
        btnNpcToolsNone.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            if (!dom.npcToolsGrid) return;
            dom.npcToolsGrid.querySelectorAll('input[type="checkbox"][data-action-name]').forEach((cb) => {
                cb.checked = false;
            });
            updateNpcToolsCountBadge();
            EevsUI.syncNpcConfigToClient();
        });
    }

    function rollRandomVoiceSeed() {
        return Math.floor(100000 + Math.random() * 899999);
    }

    function updateNpcPresetUi(updateVoicePrompt = false, resetRoleTools = false) {
        const presetKey = dom.llmPersonaSelect ? dom.llmPersonaSelect.value : 'cop';
        const preset = npcPresets[presetKey] || npcPresets.cop;
        const isCustom = presetKey === 'custom';

        if (dom.llmCustomModelField) {
            dom.llmCustomModelField.classList.toggle('hidden', !isCustom);
        }

        const currentGender = (dom.llmGenderSelect && dom.llmGenderSelect.value === 'male') ? 'male' : 'female';
        if (dom.llmGenderSelect) {
            const femModel = isCustom
                ? (dom.llmCustomModelInput && dom.llmCustomModelInput.value.trim() ? dom.llmCustomModelInput.value.trim() : 'custom')
                : preset.models.female;
            const maleModel = isCustom
                ? (dom.llmCustomModelInput && dom.llmCustomModelInput.value.trim() ? dom.llmCustomModelInput.value.trim() : 'custom')
                : preset.models.male;
            dom.llmGenderSelect.innerHTML = `
                <option value="female">♀ Female (${femModel})</option>
                <option value="male">♂ Male (${maleModel})</option>
            `;
            dom.llmGenderSelect.value = currentGender;
        }

        if (updateVoicePrompt && dom.llmVoiceInstructInput && preset.voiceInstruct && preset.voiceInstruct[currentGender]) {
            dom.llmVoiceInstructInput.value = preset.voiceInstruct[currentGender];
        }

        if (resetRoleTools) {
            renderNpcToolsCheckboxes(presetKey);
        }
    }

    function getSelectedNpcVdesModel() {
        if (dom.llmVdesModelSelect && dom.llmVdesModelSelect.value) {
            return dom.llmVdesModelSelect.value;
        }
        return dom.vdesModelSelect ? dom.vdesModelSelect.value : '';
    }

    function getSelectedNpcSttModel() {
        if (dom.llmSttModelSelect && dom.llmSttModelSelect.value) {
            return dom.llmSttModelSelect.value;
        }
        return dom.sttModelSelect ? dom.sttModelSelect.value : '';
    }

    EevsUI.getSelectedNpcSpawnConfig = function getSelectedNpcSpawnConfig() {
        const presetKey = dom.llmPersonaSelect ? dom.llmPersonaSelect.value : 'cop';
        const preset = npcPresets[presetKey] || npcPresets.cop;
        const gender = (dom.llmGenderSelect && dom.llmGenderSelect.value === 'male') ? 'male' : 'female';
        let modelName = preset.models[gender] || 's_f_y_cop_01';
        if (presetKey === 'custom' && dom.llmCustomModelInput && dom.llmCustomModelInput.value.trim() !== '') {
            modelName = dom.llmCustomModelInput.value.trim();
        }
        const defaultInstruct = (preset.voiceInstruct && preset.voiceInstruct[gender]) || '';
        const voiceInstruct = (dom.llmVoiceInstructInput && dom.llmVoiceInstructInput.value.trim() !== '')
            ? dom.llmVoiceInstructInput.value.trim()
            : defaultInstruct;
        const rawSeed = dom.llmNpcSeedInput && dom.llmNpcSeedInput.value !== ''
            ? parseInt(dom.llmNpcSeedInput.value, 10)
            : rollRandomVoiceSeed();
        const voiceSeed = Number.isNaN(rawSeed) ? rollRandomVoiceSeed() : rawSeed;
        const rawGain = dom.llmNpcGainInput && dom.llmNpcGainInput.value !== ''
            ? parseFloat(dom.llmNpcGainInput.value)
            : 1.0;
        const voiceGain = Number.isNaN(rawGain) ? 1.0 : Math.max(0.2, Math.min(3.0, rawGain));
        const initiateFirst = !!(dom.llmNpcInitiateFirst && dom.llmNpcInitiateFirst.checked);
        const broadcastScope = dom.llmBroadcastScopeSelect ? dom.llmBroadcastScopeSelect.value : '3d_nearby';
        const hearingRadius = Math.max(5, Math.min(150, parseFloat(dom.llmHearingRadiusInput ? dom.llmHearingRadiusInput.value : '35') || 35));
        const interactDist = Math.max(1.5, Math.min(15, parseFloat(dom.llmInteractDistInput ? dom.llmInteractDistInput.value : '3.5') || 3.5));
        const autoStopDist = Math.max(5, Math.min(100, parseFloat(dom.llmAutoStopDistInput ? dom.llmAutoStopDistInput.value : '20') || 20));
        const rawDespawn = dom.llmDespawnDistInput && dom.llmDespawnDistInput.value !== '' ? parseFloat(dom.llmDespawnDistInput.value) : 100;
        const despawnDist = Number.isNaN(rawDespawn) ? 100 : Math.max(0, Math.min(500, rawDespawn));
        const autoEngage = !!(dom.llmAutoEngageProximity && dom.llmAutoEngageProximity.checked);

        return {
            preset: presetKey,
            roleTitle: preset.title,
            gender,
            modelName,
            systemPrompt: dom.llmSystemPromptInput ? dom.llmSystemPromptInput.value.trim() : (preset.prompt || ''),
            voiceInstruct,
            voiceSeed,
            voiceGain,
            initiateFirst,
            broadcastScope,
            hearingRadius,
            interactDist,
            autoStopDist,
            despawnDist,
            autoEngage,
            voice: gender,
            enabledActions: getEnabledNpcActions()
        };
    };

    function updateNpcBroadcastSummaryBadge() {
        if (!dom.npcBroadcastSummaryBadge) return;
        const cfg = EevsUI.getSelectedNpcSpawnConfig();
        const scopeLabels = {
            '3d_nearby': '3D Nearby',
            '3d_private': '3D Private',
            '2d_private': '2D Private',
            '2d_all': '2D Server-Wide'
        };
        const scopeText = scopeLabels[cfg.broadcastScope] || '3D Nearby';
        dom.npcBroadcastSummaryBadge.textContent = `${scopeText} • Hear ${cfg.hearingRadius}m • Interact ${cfg.interactDist}m`;
    }

    let llmHistoryStarted = false;
    EevsUI.appendLlmDialogue = function appendLlmDialogue(userText, npcReply, clipId, durationMs, actionName, backendName) {
        if (!dom.llmChatLog) return;
        if (!llmHistoryStarted) {
            dom.llmChatLog.innerHTML = '';
            llmHistoryStarted = true;
        }
        const hasAction = actionName && actionName !== 'none';
        const displayReply = npcReply && npcReply.trim() !== ''
            ? npcReply
            : '<em>(Silent physical action)</em>';
        const entry = document.createElement('div');
        entry.style.marginBottom = '8px';
        entry.innerHTML = `
            <div style="color:#93c5fd;font-size:12px;"><strong>You:</strong> ${userText}</div>
            <div style="color:#34d399;font-size:13px;margin-top:2px;">
                <strong>NPC:</strong> ${displayReply}
                ${hasAction ? `<span class="pill pill-accent" style="margin-left:6px;font-size:10px;padding:1px 6px;">⚡ ${actionName}</span>` : ''}
                ${clipId ? `<span class="mini-badge" style="margin-left:6px;">${clipId} (${durationMs || 0}ms)</span>` : ''}
                ${backendName ? `<span class="item-meta" style="margin-left:6px;">[${backendName}]</span>` : ''}
            </div>
        `;
        dom.llmChatLog.appendChild(entry);
        dom.llmChatLog.scrollTop = dom.llmChatLog.scrollHeight;
    };

    function getSelectedLlmNpcId() {
        if (dom.llmNpcSelect && dom.llmNpcSelect.value && dom.llmNpcSelect.value.startsWith('npc:')) {
            return parseInt(dom.llmNpcSelect.value.slice(4), 10) || null;
        }
        return EevsUI.getSelectedNpcId();
    }

    EevsUI.syncNpcConfigToClient = function syncNpcConfigToClient() {
        updateNpcBroadcastSummaryBadge();
        if (!EevsUI.isFiveMNui) return;
        const spawnCfg = EevsUI.getSelectedNpcSpawnConfig();
        EevsUI.sendNpcAction('setConfig', {
            ...spawnCfg,
            llmModel: dom.llmModelSelect ? dom.llmModelSelect.value : '',
            sttModel: getSelectedNpcSttModel(),
            vdesModel: getSelectedNpcVdesModel()
        });
    };

    const btnRefreshLlmModels = document.getElementById('btnRefreshLlmModels');
    if (btnRefreshLlmModels) {
        btnRefreshLlmModels.addEventListener('click', EevsUI.refreshLlmModels);
    }
    const btnRefreshLlmSttModels = document.getElementById('btnRefreshLlmSttModels');
    if (btnRefreshLlmSttModels) {
        btnRefreshLlmSttModels.addEventListener('click', EevsUI.refreshSttModels);
    }
    const btnRefreshLlmVdesModels = document.getElementById('btnRefreshLlmVdesModels');
    if (btnRefreshLlmVdesModels) {
        btnRefreshLlmVdesModels.addEventListener('click', EevsUI.refreshVdesModels);
    }
    if (dom.llmModelSelect) {
        dom.llmModelSelect.addEventListener('change', EevsUI.syncNpcConfigToClient);
    }
    if (dom.llmSttModelSelect) {
        dom.llmSttModelSelect.addEventListener('change', () => {
            if (dom.sttModelSelect && dom.sttModelSelect.value !== dom.llmSttModelSelect.value) {
                dom.sttModelSelect.value = dom.llmSttModelSelect.value;
                EevsUI.refreshActiveSttParamUi();
            }
            EevsUI.syncNpcConfigToClient();
        });
    }
    if (dom.llmVdesModelSelect) {
        dom.llmVdesModelSelect.addEventListener('change', () => {
            if (dom.vdesModelSelect && dom.vdesModelSelect.value !== dom.llmVdesModelSelect.value) {
                dom.vdesModelSelect.value = dom.llmVdesModelSelect.value;
                EevsUI.refreshActiveVdesParamUi();
            }
            EevsUI.syncNpcConfigToClient();
        });
    }

    if (dom.llmPersonaSelect) {
        dom.llmPersonaSelect.addEventListener('change', () => {
            const preset = npcPresets[dom.llmPersonaSelect.value];
            updateNpcPresetUi(true, true);
            if (preset && preset.prompt && dom.llmSystemPromptInput) {
                dom.llmSystemPromptInput.value = preset.prompt;
            }
            if (dom.llmNpcSeedInput) {
                dom.llmNpcSeedInput.value = String(rollRandomVoiceSeed());
            }
            EevsUI.syncNpcConfigToClient();
        });
    }

    if (dom.llmGenderSelect) {
        dom.llmGenderSelect.addEventListener('change', () => {
            updateNpcPresetUi(true, false);
            if (dom.llmNpcSeedInput) {
                dom.llmNpcSeedInput.value = String(rollRandomVoiceSeed());
            }
            EevsUI.syncNpcConfigToClient();
        });
    }

    const btnLlmRandomSeed = document.getElementById('btnLlmRandomSeed');
    if (btnLlmRandomSeed && dom.llmNpcSeedInput) {
        btnLlmRandomSeed.addEventListener('click', () => {
            dom.llmNpcSeedInput.value = String(rollRandomVoiceSeed());
            EevsUI.syncNpcConfigToClient();
        });
    }

    for (const el of [
        dom.llmNpcSeedInput,
        dom.llmNpcGainInput,
        dom.llmNpcInitiateFirst,
        dom.llmBroadcastScopeSelect,
        dom.llmHearingRadiusInput,
        dom.llmInteractDistInput,
        dom.llmAutoStopDistInput,
        dom.llmDespawnDistInput,
        dom.llmAutoEngageProximity,
        dom.llmVoiceInstructInput,
        dom.llmSystemPromptInput
    ]) {
        if (el) {
            el.addEventListener('change', EevsUI.syncNpcConfigToClient);
        }
    }

    if (dom.llmCustomModelInput) {
        dom.llmCustomModelInput.addEventListener('input', () => {
            updateNpcPresetUi(false, false);
            EevsUI.syncNpcConfigToClient();
        });
    }

    updateNpcPresetUi(true, true);
    updateNpcBroadcastSummaryBadge();

    document.getElementById('btnLlmSpawnNpc').addEventListener('click', async () => {
        if (dom.llmNpcSeedInput) {
            dom.llmNpcSeedInput.value = String(rollRandomVoiceSeed());
        }
        const spawnCfg = EevsUI.getSelectedNpcSpawnConfig();
        const res = await EevsUI.sendNpcAction('spawn', spawnCfg);
        if (res.ok && res.npc) {
            EevsUI.logEvent(`Spawned ${res.npc.label} [${spawnCfg.modelName}, seed=${spawnCfg.voiceSeed}] in front of you!`);
        } else {
            EevsUI.logEvent(`Spawn NPC failed: ${EevsUI.formatError(res)}`);
        }
    });

    const btnLlmDeleteNpcs = document.getElementById('btnLlmDeleteNpcs');
    if (btnLlmDeleteNpcs) {
        btnLlmDeleteNpcs.addEventListener('click', async () => {
            const res = await EevsUI.sendNpcAction('clear');
            EevsUI.logEvent(`Deleted ${res.removed ?? 0} spawned NPC(s)`);
        });
    }

    document.getElementById('btnLlmTalkStart').addEventListener('click', async () => {
        const language = document.getElementById('sttLangSelect') ? document.getElementById('sttLangSelect').value : 'en';
        const selectedSttModel = getSelectedNpcSttModel();
        const options = selectedSttModel ? { model: selectedSttModel } : undefined;

        if (dom.llmStatusText) {
            dom.llmStatusText.textContent = '● Listening... Hold [N] to speak to NPC';
        }
        const res = await EevsUI.sendControlRequest('POST', '/stt/start', {
            netId: 'self',
            language,
            isolate: false,
            options
        });
        EevsUI.logEvent(res.ok ? 'Listening for NPC voice conversation (Hold [N] to speak, then click Stop & NPC Reply)' : `Mic start error: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnConverseStop').addEventListener('click', async () => {
        const spawnCfg = EevsUI.getSelectedNpcSpawnConfig();
        const llmModel = dom.llmModelSelect ? dom.llmModelSelect.value : '';
        const selectedSttModel = getSelectedNpcSttModel();
        const selectedVdesModel = getSelectedNpcVdesModel();
        const sttOptions = selectedSttModel ? { model: selectedSttModel } : undefined;
        const vdesOptions = selectedVdesModel ? { model: selectedVdesModel } : undefined;
        const npcId = getSelectedLlmNpcId();

        if (dom.llmStatusText) {
            dom.llmStatusText.textContent = '⏳ STT (audio.cpp) -> LLM -> Voice Design...';
        }
        const res = await EevsUI.sendControlRequest('POST', '/ai/converse/stop', {
            senderNetId: 'self',
            systemPrompt: spawnCfg.systemPrompt,
            enabledActions: spawnCfg.enabledActions,
            llmModel,
            voiceInstruct: spawnCfg.voiceInstruct,
            voiceSeed: spawnCfg.voiceSeed,
            voiceGain: spawnCfg.voiceGain,
            broadcastScope: spawnCfg.broadcastScope,
            hearingRadius: spawnCfg.hearingRadius,
            voice: spawnCfg.voice,
            mode: spawnCfg.broadcastScope.startsWith('2d') ? '2D' : '3D',
            npcId,
            speak: true,
            sttOptions,
            options: vdesOptions
        });
        if (res.ok) {
            if (dom.llmStatusText) {
                dom.llmStatusText.textContent = `✓ Replied (${res.durationMs || 0}ms, action=${res.action || 'none'}, ${res.backend || 'vdes'})`;
            }
            EevsUI.appendLlmDialogue(res.transcript || '(Voice)', res.reply || '', res.clipId, res.durationMs, res.action, res.backend);
            EevsUI.logEvent(`NPC reply [action=${res.action || 'none'}, ${res.backend || 'vdes'}] (${res.durationMs || 0}ms): "${res.reply}"`);
            EevsUI.refreshState();
        } else {
            const errMsg = EevsUI.formatError(res);
            if (dom.llmStatusText) {
                dom.llmStatusText.textContent = `Error: ${errMsg}`;
            }
            EevsUI.logEvent(`NPC Converse error: ${errMsg}`);
        }
    });

    document.getElementById('btnAiChatSpeak').addEventListener('click', async () => {
        const prompt = dom.llmPromptInput ? dom.llmPromptInput.value.trim() : '';
        const spawnCfg = EevsUI.getSelectedNpcSpawnConfig();
        const llmModel = dom.llmModelSelect ? dom.llmModelSelect.value : '';
        const selectedVdesModel = getSelectedNpcVdesModel();
        const options = selectedVdesModel ? { model: selectedVdesModel } : undefined;
        const npcId = getSelectedLlmNpcId();
        if (!prompt) return;

        if (dom.llmStatusText) {
            dom.llmStatusText.textContent = '⏳ Querying LLM API & designing voice...';
        }
        EevsUI.logEvent('Querying LLM API (Lua) & synthesizing via audio.cpp Voice Design...');
        const res = await EevsUI.sendControlRequest('POST', '/ai/chat', {
            senderNetId: 'self',
            prompt,
            systemPrompt: spawnCfg.systemPrompt,
            enabledActions: spawnCfg.enabledActions,
            llmModel,
            voiceInstruct: spawnCfg.voiceInstruct,
            voiceSeed: spawnCfg.voiceSeed,
            voiceGain: spawnCfg.voiceGain,
            broadcastScope: spawnCfg.broadcastScope,
            hearingRadius: spawnCfg.hearingRadius,
            voice: spawnCfg.voice,
            mode: spawnCfg.broadcastScope.startsWith('2d') ? '2D' : '3D',
            npcId,
            speak: true,
            options
        });
        if (res.ok) {
            if (dom.llmStatusText) {
                dom.llmStatusText.textContent = `✓ Replied (${res.durationMs || 0}ms, action=${res.action || 'none'}, ${res.backend || 'vdes'})`;
            }
            EevsUI.appendLlmDialogue(prompt, res.reply || '', res.clipId, res.durationMs, res.action, res.backend);
            EevsUI.logEvent(`NPC Reply [action=${res.action || 'none'}, ${res.backend || 'vdes'}] (${res.durationMs}ms): "${res.reply}"`);
            EevsUI.refreshState();
        } else {
            const errMsg = EevsUI.formatError(res);
            if (dom.llmStatusText) {
                dom.llmStatusText.textContent = `Error: ${errMsg}`;
            }
            EevsUI.logEvent(`AI Chat error: ${errMsg}`);
        }
    });
})();
