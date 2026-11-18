// ============================================================================
// EEVS NUI - Core Transport, Event Bus & Dynamic Model Parameter Builder
// ============================================================================
(() => {
    const isFiveMNui = typeof GetParentResourceName === 'function';
    const resourceName = isFiveMNui ? GetParentResourceName() : 'eevs_demo';

    const EevsUI = {
        isFiveMNui,
        resourceName,
        selfNetId: 1,
        cachedClips: [],
        spawnedNpcs: [],
        ttsModelsCache: [],
        vdesModelsCache: [],
        sttModelsCache: [],
        modelDetailsById: new Map(),
        libraryVoicesCache: [],
        ws: null,
        wsReqCounter: 1,
        wsPending: new Map(),
        dom: {
            appEl: document.getElementById('app'),
            standaloneBar: document.getElementById('standaloneBar'),
            connBadge: document.getElementById('connBadge'),
            gatewayBadge: document.getElementById('gatewayBadge'),
            pluginsBadge: document.getElementById('pluginsBadge'),
            sessionsBadge: document.getElementById('sessionsBadge'),
            selfNetIdBadge: document.getElementById('selfNetIdBadge'),
            micStatusBox: document.getElementById('micStatusBox'),
            micBadgeText: document.getElementById('micBadgeText'),
            echoStateBadge: document.getElementById('echoStateBadge'),
            recStatusPill: document.getElementById('recStatusPill'),
            clipMemStats: document.getElementById('clipMemStats'),
            clipMemBar: document.getElementById('clipMemBar'),
            clipCountLabel: document.getElementById('clipCountLabel'),
            sessionList: document.getElementById('sessionList'),
            clipList: document.getElementById('clipList'),
            sttOutput: document.getElementById('sttOutput'),
            sttMetricDur: document.getElementById('sttMetricDur'),
            sttMetricConf: document.getElementById('sttMetricConf'),
            sttMetricRms: document.getElementById('sttMetricRms'),
            ttsModelSelect: document.getElementById('ttsModelSelect'),
            ttsVoiceSelect: document.getElementById('ttsVoiceSelect'),
            vdesModelSelect: document.getElementById('vdesModelSelect'),
            vdesSeedInput: document.getElementById('vdesSeedInput'),
            vdesInstructInput: document.getElementById('vdesInstructInput'),
            vdesModeSelect: document.getElementById('vdesModeSelect'),
            vdesTextInput: document.getElementById('vdesTextInput'),
            sttModelSelect: document.getElementById('sttModelSelect'),
            llmModelSelect: document.getElementById('llmModelSelect'),
            llmSttModelSelect: document.getElementById('llmSttModelSelect'),
            llmVdesModelSelect: document.getElementById('llmVdesModelSelect'),
            npcToolsGrid: document.getElementById('npcToolsGrid'),
            npcToolsCountBadge: document.getElementById('npcToolsCountBadge'),
            npcManageList: document.getElementById('npcManageList'),
            npcListCountBadge: document.getElementById('npcListCountBadge'),
            ttsDynamicParamsSection: document.getElementById('ttsDynamicParamsSection'),
            ttsDynamicParamsTitle: document.getElementById('ttsDynamicParamsTitle'),
            ttsDynamicParamsSubtitle: document.getElementById('ttsDynamicParamsSubtitle'),
            ttsDynamicParamsGrid: document.getElementById('ttsDynamicParamsGrid'),
            vdesDynamicParamsSection: document.getElementById('vdesDynamicParamsSection'),
            vdesDynamicParamsTitle: document.getElementById('vdesDynamicParamsTitle'),
            vdesDynamicParamsSubtitle: document.getElementById('vdesDynamicParamsSubtitle'),
            vdesDynamicParamsGrid: document.getElementById('vdesDynamicParamsGrid'),
            sttDynamicParamsSection: document.getElementById('sttDynamicParamsSection'),
            sttDynamicParamsTitle: document.getElementById('sttDynamicParamsTitle'),
            sttDynamicParamsSubtitle: document.getElementById('sttDynamicParamsSubtitle'),
            sttDynamicParamsGrid: document.getElementById('sttDynamicParamsGrid'),
            ttsBackendBadge: document.getElementById('ttsBackendBadge'),
            vdesBackendBadge: document.getElementById('vdesBackendBadge'),
            sttBackendBadge: document.getElementById('sttBackendBadge'),
            llmBackendBadge: document.getElementById('llmBackendBadge'),
            npcCountBadge: document.getElementById('npcCountBadge'),
            npcActiveSelect: document.getElementById('npcActiveSelect'),
            llmNpcSelect: document.getElementById('llmNpcSelect'),
            llmPersonaSelect: document.getElementById('llmPersonaSelect'),
            llmGenderSelect: document.getElementById('llmGenderSelect'),
            llmCustomModelField: document.getElementById('llmCustomModelField'),
            llmCustomModelInput: document.getElementById('llmCustomModelInput'),
            llmNpcSeedInput: document.getElementById('llmNpcSeedInput'),
            llmNpcGainInput: document.getElementById('llmNpcGainInput'),
            llmNpcInitiateFirst: document.getElementById('llmNpcInitiateFirst'),
            llmBroadcastScopeSelect: document.getElementById('llmBroadcastScopeSelect'),
            llmHearingRadiusInput: document.getElementById('llmHearingRadiusInput'),
            llmInteractDistInput: document.getElementById('llmInteractDistInput'),
            llmAutoStopDistInput: document.getElementById('llmAutoStopDistInput'),
            llmDespawnDistInput: document.getElementById('llmDespawnDistInput'),
            llmAutoEngageProximity: document.getElementById('llmAutoEngageProximity'),
            npcBroadcastSummaryBadge: document.getElementById('npcBroadcastSummaryBadge'),
            llmVoiceInstructInput: document.getElementById('llmVoiceInstructInput'),
            llmVoiceSelect: document.getElementById('llmVoiceSelect'),
            llmSystemPromptInput: document.getElementById('llmSystemPromptInput'),
            llmPromptInput: document.getElementById('llmPromptInput'),
            llmChatLog: document.getElementById('llmChatLog'),
            llmStatusText: document.getElementById('llmStatusText'),
            audiocppModal: document.getElementById('audiocppModal'),
            audiocppFrame: document.getElementById('audiocppFrame'),
            audiocppUrlInput: document.getElementById('audiocppUrlInput'),
            eventLog: document.getElementById('eventLog'),
            eventHistoryPills: document.getElementById('eventHistoryPills')
        }
    };

    const { dom } = EevsUI;

    EevsUI.logEvent = function logEvent(msg, pillTopic = null) {
        const time = new Date().toLocaleTimeString();
        if (dom.eventLog) {
            dom.eventLog.textContent = `[${time}] ${msg}`;
        }
        if (pillTopic && dom.eventHistoryPills) {
            const pill = document.createElement('span');
            pill.className = 'event-pill';
            pill.textContent = pillTopic;
            dom.eventHistoryPills.prepend(pill);
            while (dom.eventHistoryPills.children.length > 4) {
                dom.eventHistoryPills.removeChild(dom.eventHistoryPills.lastChild);
            }
        }
    };

    EevsUI.getTargetNetId = function getTargetNetId() {
        if (!isFiveMNui) {
            return parseInt(document.getElementById('targetNetIdInput').value, 10) || 1;
        }
        return 'self';
    };

    EevsUI.formatError = function formatError(res) {
        if (!res) return 'Unknown error';
        return res.error || res.text || res.raw || 'Request failed';
    };

    EevsUI.sendControlRequest = async function sendControlRequest(method, path, body = {}) {
        if (isFiveMNui) {
            try {
                const res = await fetch(`https://${resourceName}/apiRequest`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ method, path, body })
                });
                const wrapped = await res.json();
                const data = wrapped.data || {};
                if (!data.ok && wrapped.status && wrapped.status >= 400 && !data.error) {
                    data.error = data.text || data.raw || `HTTP ${wrapped.status}`;
                }
                return data;
            } catch (err) {
                return { ok: false, error: err.message || 'NUI callback error' };
            }
        }
        if (EevsUI.ws && EevsUI.ws.readyState === WebSocket.OPEN) {
            const resolvedBody = { ...body };
            const netIdNum = EevsUI.getTargetNetId();
            if (resolvedBody.netId === 'self') resolvedBody.netId = netIdNum;
            if (resolvedBody.senderNetId === 'self') resolvedBody.senderNetId = netIdNum;
            if (resolvedBody.fromNetId === 'self') resolvedBody.fromNetId = netIdNum;
            if (resolvedBody.toNetId === 'self') resolvedBody.toNetId = netIdNum;
            if (path === '/clip/play' && resolvedBody.mode === '2D' && !resolvedBody.listenerNetIds) {
                resolvedBody.listenerNetIds = [netIdNum];
            }
            const id = `req_${EevsUI.wsReqCounter++}`;
            return new Promise((resolve) => {
                EevsUI.wsPending.set(id, resolve);
                EevsUI.ws.send(JSON.stringify({ id, method, path, body: resolvedBody }));
                setTimeout(() => {
                    if (EevsUI.wsPending.has(id)) {
                        EevsUI.wsPending.delete(id);
                        resolve({ ok: false, error: 'Request timeout' });
                    }
                }, 15000);
            });
        }
        return { ok: false, error: 'Not connected to EEVS' };
    };

    EevsUI.sendNpcAction = async function sendNpcAction(action, extra = {}) {
        if (!isFiveMNui) {
            EevsUI.logEvent('NPC spawning requires in-game FiveM NUI mode');
            return { ok: false };
        }
        try {
            const res = await fetch(`https://${resourceName}/npcAction`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ action, ...extra })
            });
            const data = await res.json();
            if (data && data.catalog && typeof EevsUI.applyNpcCatalog === 'function') {
                EevsUI.applyNpcCatalog(data.catalog);
            }
            if (data && Array.isArray(data.npcs) && typeof EevsUI.renderNpcSelectors === 'function') {
                EevsUI.renderNpcSelectors(data.npcs);
            }
            return data || { ok: false };
        } catch (err) {
            return { ok: false, error: err.message };
        }
    };

    EevsUI.getSelectedNpcId = function getSelectedNpcId() {
        if (!dom.npcActiveSelect) return null;
        const val = dom.npcActiveSelect.value || 'self';
        if (val.startsWith('npc:')) {
            return parseInt(val.slice(4), 10) || null;
        }
        if (EevsUI.spawnedNpcs.length > 0) {
            return EevsUI.spawnedNpcs[EevsUI.spawnedNpcs.length - 1].npcId;
        }
        return null;
    };

    EevsUI.populateModelSelect = function populateModelSelect(selectEl, models) {
        if (!selectEl) return;
        const previousValue = selectEl.value;
        selectEl.innerHTML = '';
        const defaultOpt = document.createElement('option');
        defaultOpt.value = '';
        defaultOpt.textContent = Array.isArray(models) && models.length > 0
            ? `(Default: ${models[0]})`
            : '(No models registered in audio.cpp — install in /audio_cpp)';
        selectEl.appendChild(defaultOpt);
        if (Array.isArray(models)) {
            for (const m of models) {
                const opt = document.createElement('option');
                opt.value = m;
                opt.textContent = m;
                selectEl.appendChild(opt);
            }
        }
        if (previousValue && Array.isArray(models) && models.includes(previousValue)) {
            selectEl.value = previousValue;
        }
    };

    EevsUI.getEffectiveModelId = function getEffectiveModelId(selectEl, modelsList) {
        if (selectEl && selectEl.value) {
            return selectEl.value;
        }
        return Array.isArray(modelsList) && modelsList.length > 0 ? modelsList[0] : '';
    };

    EevsUI.updateVoiceSelectorsForTtsModel = function updateVoiceSelectorsForTtsModel(modelId) {
        const detail = EevsUI.modelDetailsById.get(modelId);
        const voiceParam = detail && Array.isArray(detail.params)
            ? detail.params.find((p) => p.name === 'voice_id' && Array.isArray(p.values) && p.values.length > 0)
            : null;

        for (const sel of [dom.ttsVoiceSelect, dom.llmVoiceSelect]) {
            if (!sel) continue;
            const prev = sel.value;
            sel.innerHTML = '';

            if (voiceParam) {
                for (const v of voiceParam.values) {
                    const opt = document.createElement('option');
                    opt.value = v;
                    opt.textContent = `🗣 ${v}`;
                    sel.appendChild(opt);
                }
            } else {
                const basePresets = [
                    { value: 'default', label: 'Default (Auto)' },
                    { value: 'female', label: 'Female Preset' },
                    { value: 'male', label: 'Male Preset' }
                ];
                for (const p of basePresets) {
                    const opt = document.createElement('option');
                    opt.value = p.value;
                    opt.textContent = p.label;
                    sel.appendChild(opt);
                }
                if (Array.isArray(EevsUI.libraryVoicesCache) && EevsUI.libraryVoicesCache.length > 0) {
                    for (const lv of EevsUI.libraryVoicesCache) {
                        const opt = document.createElement('option');
                        opt.value = lv;
                        opt.textContent = `🎙 Clone Ref: ${lv}`;
                        sel.appendChild(opt);
                    }
                }
            }

            if (prev && Array.from(sel.options).some((o) => o.value === prev)) {
                sel.value = prev;
            }
        }
    };

    EevsUI.renderDynamicParamsForModel = function renderDynamicParamsForModel(modelId, sectionEl, titleEl, subtitleEl, gridEl, skipNames = []) {
        if (!sectionEl || !gridEl) return;
        gridEl.innerHTML = '';

        const detail = EevsUI.modelDetailsById.get(modelId);
        if (!detail || !Array.isArray(detail.params)) {
            sectionEl.classList.add('hidden');
            return;
        }

        const filteredParams = detail.params.filter((p) => p && p.name && !skipNames.includes(p.name));
        if (filteredParams.length === 0) {
            sectionEl.classList.add('hidden');
            return;
        }

        sectionEl.classList.remove('hidden');
        if (titleEl) {
            titleEl.textContent = `${detail.id.toUpperCase()} PARAMETERS (${detail.family || detail.kind || 'model'})`;
        }
        if (subtitleEl) {
            const notes = Array.isArray(detail.notes) && detail.notes.length > 0
                ? detail.notes[0]
                : `OpenAI API /v1/models?include_params=true (${filteredParams.length} option${filteredParams.length === 1 ? '' : 's'})`;
            subtitleEl.textContent = notes;
        }

        for (const param of filteredParams) {
            const fieldDiv = document.createElement('div');
            fieldDiv.className = 'field';

            const label = document.createElement('label');
            const rangeInfo = (param.min !== undefined || param.max !== undefined)
                ? ` [${param.min ?? ''}..${param.max ?? ''}]`
                : '';
            label.textContent = `${param.name} (${param.type}${rangeInfo})`;
            fieldDiv.appendChild(label);

            let inputEl;
            if (param.type === 'enum' && Array.isArray(param.values)) {
                inputEl = document.createElement('select');
                const autoOpt = document.createElement('option');
                autoOpt.value = '';
                autoOpt.textContent = param.default ? `(Default: ${param.default})` : '(Model Default)';
                inputEl.appendChild(autoOpt);
                for (const val of param.values) {
                    const opt = document.createElement('option');
                    opt.value = val;
                    opt.textContent = val;
                    inputEl.appendChild(opt);
                }
            } else if (param.type === 'bool') {
                inputEl = document.createElement('select');
                const defLabel = param.default ? ` (Default: ${param.default})` : '';
                inputEl.innerHTML = `
                    <option value="">Auto${defLabel}</option>
                    <option value="true">true</option>
                    <option value="false">false</option>
                `;
            } else if (param.type === 'int' || param.type === 'float') {
                inputEl = document.createElement('input');
                inputEl.type = 'number';
                inputEl.step = param.type === 'int' ? '1' : '0.05';
                if (param.min !== undefined) inputEl.min = String(param.min);
                if (param.max !== undefined) inputEl.max = String(param.max);
                inputEl.placeholder = param.default ? `Default: ${param.default}` : '';
            } else {
                inputEl = document.createElement('input');
                inputEl.type = 'text';
                inputEl.placeholder = param.default ? `Default: ${param.default}` : (param.description || '');
            }

            inputEl.dataset.paramName = param.name;
            inputEl.dataset.paramType = param.type;
            if (param.description) {
                inputEl.title = param.description;
            }
            fieldDiv.appendChild(inputEl);

            if (param.description) {
                const hint = document.createElement('span');
                hint.className = 'dynamic-param-hint';
                hint.textContent = param.description;
                fieldDiv.appendChild(hint);
            }

            gridEl.appendChild(fieldDiv);
        }
    };

    EevsUI.collectDynamicParams = function collectDynamicParams(gridEl) {
        const result = {};
        if (!gridEl) return result;
        const inputs = gridEl.querySelectorAll('[data-param-name]');
        for (const el of inputs) {
            const name = el.dataset.paramName;
            const pType = el.dataset.paramType;
            const rawVal = el.value !== undefined ? String(el.value).trim() : '';
            if (!name || rawVal === '') continue;

            if (pType === 'int') {
                const n = parseInt(rawVal, 10);
                if (!Number.isNaN(n)) result[name] = n;
            } else if (pType === 'float') {
                const n = parseFloat(rawVal);
                if (!Number.isNaN(n)) result[name] = n;
            } else if (pType === 'bool') {
                if (rawVal === 'true') result[name] = true;
                else if (rawVal === 'false') result[name] = false;
            } else {
                result[name] = rawVal;
            }
        }
        return result;
    };

    EevsUI.refreshActiveTtsParamUi = function refreshActiveTtsParamUi() {
        const modelId = EevsUI.getEffectiveModelId(dom.ttsModelSelect, EevsUI.ttsModelsCache);
        EevsUI.updateVoiceSelectorsForTtsModel(modelId);
        EevsUI.renderDynamicParamsForModel(
            modelId,
            dom.ttsDynamicParamsSection,
            dom.ttsDynamicParamsTitle,
            dom.ttsDynamicParamsSubtitle,
            dom.ttsDynamicParamsGrid,
            ['voice_id']
        );
    };

    EevsUI.refreshActiveSttParamUi = function refreshActiveSttParamUi() {
        const modelId = EevsUI.getEffectiveModelId(dom.sttModelSelect, EevsUI.sttModelsCache);
        EevsUI.renderDynamicParamsForModel(
            modelId,
            dom.sttDynamicParamsSection,
            dom.sttDynamicParamsTitle,
            dom.sttDynamicParamsSubtitle,
            dom.sttDynamicParamsGrid,
            ['language', 'translate', 'initial_prompt']
        );
    };

    EevsUI.refreshActiveVdesParamUi = function refreshActiveVdesParamUi() {
        const modelId = EevsUI.getEffectiveModelId(dom.vdesModelSelect, EevsUI.vdesModelsCache);
        EevsUI.renderDynamicParamsForModel(
            modelId,
            dom.vdesDynamicParamsSection,
            dom.vdesDynamicParamsTitle,
            dom.vdesDynamicParamsSubtitle,
            dom.vdesDynamicParamsGrid,
            ['instruct', 'seed']
        );
    };

    EevsUI.refreshVdesModels = async function refreshVdesModels() {
        const res = await EevsUI.sendControlRequest('GET', '/vdes/models');
        if (res && res.ok) {
            if (dom.vdesBackendBadge) {
                dom.vdesBackendBadge.textContent = `Backend: ${res.backend || 'audio.cpp (vdes)'}`;
            }
            if (Array.isArray(res.modelDetails)) {
                for (const md of res.modelDetails) {
                    if (md && md.id) EevsUI.modelDetailsById.set(md.id, md);
                }
            }
            EevsUI.vdesModelsCache = res.models || [];
            EevsUI.populateModelSelect(dom.vdesModelSelect, EevsUI.vdesModelsCache);
            EevsUI.populateModelSelect(dom.llmVdesModelSelect, EevsUI.vdesModelsCache);
            EevsUI.refreshActiveVdesParamUi();
        }
    };

    EevsUI.refreshTtsModels = async function refreshTtsModels() {
        const res = await EevsUI.sendControlRequest('GET', '/tts/models');
        if (res && res.ok) {
            if (dom.ttsBackendBadge) {
                dom.ttsBackendBadge.textContent = `Backend: ${res.backend || 'audio.cpp'}`;
            }
            if (res.url && dom.audiocppUrlInput && !dom.audiocppUrlInput.dataset.customized) {
                dom.audiocppUrlInput.value = res.url;
            }
            if (Array.isArray(res.voices)) {
                EevsUI.libraryVoicesCache = res.voices;
            }
            if (Array.isArray(res.modelDetails)) {
                for (const md of res.modelDetails) {
                    if (md && md.id) EevsUI.modelDetailsById.set(md.id, md);
                }
            }
            EevsUI.ttsModelsCache = res.ttsModels || res.models || [];
            EevsUI.populateModelSelect(dom.ttsModelSelect, EevsUI.ttsModelsCache);
            EevsUI.refreshActiveTtsParamUi();
        }
    };

    EevsUI.refreshSttModels = async function refreshSttModels() {
        const res = await EevsUI.sendControlRequest('GET', '/stt/models');
        if (res && res.ok) {
            if (dom.sttBackendBadge) {
                dom.sttBackendBadge.textContent = `Backend: ${res.backend || 'audio.cpp'}`;
            }
            if (Array.isArray(res.modelDetails)) {
                for (const md of res.modelDetails) {
                    if (md && md.id) EevsUI.modelDetailsById.set(md.id, md);
                }
            }
            EevsUI.sttModelsCache = res.sttModels || res.models || [];
            EevsUI.populateModelSelect(dom.sttModelSelect, EevsUI.sttModelsCache);
            EevsUI.populateModelSelect(dom.llmSttModelSelect, EevsUI.sttModelsCache);
            EevsUI.refreshActiveSttParamUi();
        }
    };

    EevsUI.refreshLlmModels = async function refreshLlmModels() {
        const res = await EevsUI.sendControlRequest('GET', '/llm/models');
        if (res && res.ok) {
            if (dom.llmBackendBadge) {
                dom.llmBackendBadge.textContent = `LLM: ${res.backend || 'OpenAI API'}`;
            }
            EevsUI.populateModelSelect(dom.llmModelSelect, res.models || []);
        }
    };

    EevsUI.refreshState = async function refreshState() {
        const [healthRes, sessionsRes, clipsRes] = await Promise.all([
            EevsUI.sendControlRequest('GET', '/health'),
            EevsUI.sendControlRequest('GET', '/sessions'),
            EevsUI.sendControlRequest('GET', '/clip/list')
        ]);
        if (healthRes && healthRes.ok) {
            const gwOk = healthRes.gatewayConnected && healthRes.gatewayAuthenticated;
            dom.gatewayBadge.textContent = gwOk ? 'Gateway: Synced' : 'Gateway: Offline';
            dom.gatewayBadge.className = `pill ${gwOk ? 'pill-ok' : 'pill-danger'}`;
            dom.pluginsBadge.textContent = `Plugins: ${healthRes.plugins ?? 0}`;
            dom.sessionsBadge.textContent = `Sessions: ${healthRes.sessions ?? 0}`;
            const perfEl = document.getElementById('perfTelemetryBadge');
            if (perfEl && healthRes.telemetry) {
                const t = healthRes.telemetry;
                const pipeUs = Number(t.pipelineAvgUs || 0).toFixed(1);
                const pipeMaxUs = Number(t.pipelineMaxUs || 0).toFixed(1);
                const dspUs = Number(t.dspProcessAvgUs || 0).toFixed(1);
                const dspMaxUs = Number(t.dspProcessMaxUs || 0).toFixed(1);
                const decUs = Number(t.opusDecodeAvgUs || 0).toFixed(1);
                const decMaxUs = Number(t.opusDecodeMaxUs || 0).toFixed(1);
                const encUs = Number(t.opusEncodeAvgUs || 0).toFixed(1);
                const encMaxUs = Number(t.opusEncodeMaxUs || 0).toFixed(1);
                perfEl.textContent = `⏱ Pipe: ${pipeUs} µs | DSP: ${dspUs} µs`;

                const setTxt = (id, val) => {
                    const el = document.getElementById(id);
                    if (el) el.textContent = val;
                };
                setTxt('ttPkts', `${t.ingressPackets || 0} / ${t.egressPackets || 0}`);
                setTxt('ttSpeakers', `${t.activeSpeakers || 0}`);
                setTxt('ttDecode', `${decUs} / ${decMaxUs} µs (${t.decodedFrames || 0}f)`);
                setTxt('ttDsp', `${dspUs} / ${dspMaxUs} µs (${t.dspFrames || 0}f)`);
                setTxt('ttEncode', `${encUs} / ${encMaxUs} µs (${t.encodedFrames || 0}f)`);
                setTxt('ttPipe', `${pipeUs} / ${pipeMaxUs} µs`);
            }
        }
        if (sessionsRes && Array.isArray(sessionsRes.sessions) && typeof EevsUI.renderSessions === 'function') {
            EevsUI.renderSessions(sessionsRes.sessions);
        }
        if (clipsRes && Array.isArray(clipsRes.clips) && typeof EevsUI.renderClips === 'function') {
            EevsUI.renderClips(clipsRes);
        }
        if (isFiveMNui) {
            EevsUI.sendNpcAction('list');
        }
        EevsUI.refreshTtsModels();
        EevsUI.refreshSttModels();
        EevsUI.refreshVdesModels();
        EevsUI.refreshLlmModels();
    };

    // Navigation Tabs
    document.querySelectorAll('.nav-item').forEach((btn) => {
        btn.addEventListener('click', () => {
            const targetTab = btn.getAttribute('data-tab');
            document.querySelectorAll('.nav-item').forEach((b) => b.classList.toggle('active', b === btn));
            document.querySelectorAll('.tab-pane').forEach((pane) => {
                pane.classList.toggle('hidden', pane.id !== `tab-${targetTab}`);
            });
            if (targetTab === 'tts') {
                EevsUI.refreshTtsModels();
            } else if (targetTab === 'vdes') {
                EevsUI.refreshVdesModels();
            } else if (targetTab === 'stt') {
                EevsUI.refreshSttModels();
            } else if (targetTab === 'effects' && typeof EevsUI.refreshActiveEffects === 'function') {
                EevsUI.refreshActiveEffects();
            } else if (targetTab === 'llm') {
                EevsUI.refreshLlmModels();
                EevsUI.refreshSttModels();
                EevsUI.refreshVdesModels();
            } else {
                EevsUI.refreshState();
            }
        });
    });

    // Embedded audio.cpp Server WebUI Modal (/audio_cpp)
    EevsUI.openAudioCppModal = async function openAudioCppModal(customUrl = null) {
        if (customUrl && dom.audiocppUrlInput) {
            dom.audiocppUrlInput.value = customUrl;
            dom.audiocppUrlInput.dataset.customized = '1';
        } else if (dom.audiocppUrlInput && !dom.audiocppUrlInput.dataset.customized) {
            const info = await EevsUI.sendControlRequest('GET', '/audiocpp/info');
            if (info && info.url) {
                dom.audiocppUrlInput.value = info.url;
            }
        }

        const targetUrl = (dom.audiocppUrlInput ? dom.audiocppUrlInput.value.trim() : '') || 'http://127.0.0.1:8080';
        dom.appEl.classList.add('hidden');
        if (dom.audiocppModal) {
            dom.audiocppModal.classList.remove('hidden');
        }
        if (dom.audiocppFrame && dom.audiocppFrame.src !== targetUrl) {
            dom.audiocppFrame.src = targetUrl;
        }
        if (isFiveMNui) {
            fetch(`https://${resourceName}/setKeepInput`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ keepInput: false })
            });
        }
    };

    EevsUI.closeAllModals = function closeAllModals() {
        dom.appEl.classList.add('hidden');
        if (dom.audiocppModal) {
            dom.audiocppModal.classList.add('hidden');
        }
        if (isFiveMNui) {
            fetch(`https://${resourceName}/close`, { method: 'POST', body: '{}' });
        }
    };

    document.getElementById('btnOpenAudioCppWebUi').addEventListener('click', () => EevsUI.openAudioCppModal());
    document.getElementById('btnAudioCppReload').addEventListener('click', () => {
        if (dom.audiocppUrlInput && dom.audiocppFrame) {
            dom.audiocppUrlInput.dataset.customized = '1';
            dom.audiocppFrame.src = dom.audiocppUrlInput.value.trim() || 'http://127.0.0.1:8080';
        }
    });
    document.getElementById('btnBackToEevs').addEventListener('click', () => {
        if (dom.audiocppModal) {
            dom.audiocppModal.classList.add('hidden');
        }
        dom.appEl.classList.remove('hidden');
        if (isFiveMNui) {
            fetch(`https://${resourceName}/setKeepInput`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ keepInput: true })
            });
        }
        EevsUI.refreshState();
    });
    document.getElementById('btnCloseAudioCpp').addEventListener('click', EevsUI.closeAllModals);

    document.getElementById('btnRefresh').addEventListener('click', EevsUI.refreshState);
    const perfTelemetryBadge = document.getElementById('perfTelemetryBadge');
    if (perfTelemetryBadge) {
        perfTelemetryBadge.addEventListener('click', async () => {
            await EevsUI.sendControlRequest('POST', '/metrics/reset', {});
            EevsUI.logEvent('Reset pipeline microsecond telemetry counters');
            EevsUI.refreshState();
        });
    }
    document.getElementById('btnClose').addEventListener('click', EevsUI.closeAllModals);
    window.addEventListener('keydown', async (e) => {
        const active = document.activeElement;
        const inTextInput = active && (
            active.tagName === 'TEXTAREA' ||
            active.isContentEditable ||
            (active.tagName === 'INPUT' && !['checkbox', 'radio', 'range', 'button'].includes((active.type || 'text').toLowerCase()))
        );

        if (e.key === 'Escape' && isFiveMNui) {
            if (inTextInput) {
                active.blur();
                return;
            }
            EevsUI.closeAllModals();
            return;
        }

        if ((e.key === 'z' || e.key === 'Z') && !inTextInput && !e.ctrlKey && !e.altKey && !e.metaKey) {
            const panelOpen = !dom.appEl.classList.contains('hidden');
            if (panelOpen) {
                e.preventDefault();
                const res = await EevsUI.sendControlRequest('POST', '/player/proximity', { mode: 'cycle' });
                if (res && res.ok) {
                    if (typeof EevsUI.updateProximityUi === 'function') {
                        EevsUI.updateProximityUi(res.mode, res.range);
                    }
                    EevsUI.logEvent(`Cycled Talker Proximity Range [Z]: ${res.label}`);
                    if (typeof EevsUI.refreshActiveEffects === 'function') {
                        EevsUI.refreshActiveEffects();
                    }
                }
            }
        }
    });

    // Disable NUI KeepInput while typing inside any text input/textarea so keys (T, P, N, Z, etc.)
    // do not trigger game keybinds; re-enable KeepInput on blur so [N] Push-to-Talk remains active.
    if (isFiveMNui) {
        const isTextEntryElement = (el) => {
            if (!el) return false;
            if (el.tagName === 'TEXTAREA' || el.isContentEditable) return true;
            if (el.tagName === 'INPUT') {
                const t = (el.type || 'text').toLowerCase();
                return t !== 'checkbox' && t !== 'radio' && t !== 'range' && t !== 'button';
            }
            return false;
        };

        document.addEventListener('focusin', (e) => {
            if (isTextEntryElement(e.target)) {
                fetch(`https://${resourceName}/setKeepInput`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ keepInput: false })
                });
            }
        });

        document.addEventListener('focusout', () => {
            setTimeout(() => {
                const modalOpen = dom.audiocppModal && !dom.audiocppModal.classList.contains('hidden');
                const panelOpen = !dom.appEl.classList.contains('hidden');
                if (panelOpen && !modalOpen && !isTextEntryElement(document.activeElement)) {
                    fetch(`https://${resourceName}/setKeepInput`, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ keepInput: true })
                    });
                }
            }, 10);
        });
    }

    EevsUI.handleRealtimeEevsEvent = function handleRealtimeEevsEvent(topic, netId, data) {
        EevsUI.logEvent(`Event [${topic}] netId=#${netId || 0}`, topic);
        if (topic === 'voice.talk_state' && data) {
            const row = document.getElementById(`session-net-${data.netId}`)
                || document.querySelector(`[data-client-id="${data.clientId}"]`);
            const badge = document.getElementById(`vad-badge-${data.netId}`)
                || document.querySelector(`[data-vad-client="${data.clientId}"]`);
            if (row && badge) {
                row.classList.toggle('talking', !!data.talking);
                badge.className = `pill ${data.talking ? 'pill-ok' : 'pill-neutral'}`;
                badge.textContent = data.talking ? '🔊 SPEAKING' : 'Idle';
            } else {
                EevsUI.refreshState();
            }
        } else if (topic && topic.startsWith('session.')) {
            EevsUI.refreshState();
        } else if (topic === 'stt.completed' && data && typeof EevsUI.renderSttResult === 'function') {
            EevsUI.renderSttResult(data);
        } else if (topic === 'npc.converse_turn' && data && typeof EevsUI.appendLlmDialogue === 'function') {
            EevsUI.appendLlmDialogue(data.transcript || '(Voice)', data.reply || '', data.clipId, data.durationMs, data.action, data.backend);
            EevsUI.refreshState();
        } else if (topic === 'recorder.clip_ready' || topic === 'tts.clip_ready' || topic === 'vdes.clip_ready' || topic === 'ai.reply_ready') {
            EevsUI.refreshState();
        } else if ((topic === 'effects.changed' || topic === 'effects.cleared') && typeof EevsUI.refreshActiveEffects === 'function') {
            EevsUI.refreshActiveEffects();
        }
    };

    window.addEventListener('message', (event) => {
        const msg = event.data;
        if (!msg || !msg.type) return;
        if (msg.type === 'toggleUi') {
            if (dom.audiocppModal) {
                dom.audiocppModal.classList.add('hidden');
            }
            dom.appEl.classList.toggle('hidden', !msg.open);
            if (msg.serverId) {
                EevsUI.selfNetId = msg.serverId;
                dom.selfNetIdBadge.textContent = `NetID #${EevsUI.selfNetId}`;
            }
            if (msg.npcCatalog && typeof EevsUI.applyNpcCatalog === 'function') {
                EevsUI.applyNpcCatalog(msg.npcCatalog);
            }
            if (msg.hudState && msg.hudState.echo && typeof EevsUI.updateEchoUi === 'function') {
                EevsUI.updateEchoUi(msg.hudState.echo);
            }
            if (msg.hudState && msg.hudState.proximityMode && typeof EevsUI.updateProximityUi === 'function') {
                EevsUI.updateProximityUi(msg.hudState.proximityMode, msg.hudState.proximityRange);
            }
            if (msg.open) {
                EevsUI.refreshState();
            }
        } else if (msg.type === 'toggleAudioCppUi') {
            if (msg.serverId) {
                EevsUI.selfNetId = msg.serverId;
                dom.selfNetIdBadge.textContent = `NetID #${EevsUI.selfNetId}`;
            }
            if (msg.open) {
                EevsUI.openAudioCppModal(msg.url || null);
            } else {
                EevsUI.closeAllModals();
            }
        } else if (msg.type === 'stateUpdate' && msg.hudState) {
            if (msg.hudState.echo && typeof EevsUI.updateEchoUi === 'function') {
                EevsUI.updateEchoUi(msg.hudState.echo);
            }
            if (msg.hudState.proximityMode && typeof EevsUI.updateProximityUi === 'function') {
                EevsUI.updateProximityUi(msg.hudState.proximityMode, msg.hudState.proximityRange);
            }
            if (msg.hudState.recording !== undefined) {
                dom.recStatusPill.textContent = msg.hudState.recording ? '● RECORDING' : 'IDLE';
                dom.recStatusPill.className = `pill ${msg.hudState.recording ? 'pill-danger' : 'pill-neutral'}`;
            }
        } else if (msg.type === 'npcList' && Array.isArray(msg.npcs) && typeof EevsUI.renderNpcSelectors === 'function') {
            EevsUI.renderNpcSelectors(msg.npcs);
        } else if (msg.type === 'talkingState') {
            dom.micStatusBox.classList.toggle('active', !!msg.talking);
            dom.micBadgeText.textContent = msg.talking ? 'SPEAKING [Mic Active]' : 'Mic Idle [Hold N]';
        } else if (msg.type === 'eevsEvent') {
            EevsUI.handleRealtimeEevsEvent(msg.topic, msg.netId, msg.data);
        } else if (msg.type === 'log') {
            EevsUI.logEvent(msg.message);
        }
    });

    if (!isFiveMNui) {
        dom.appEl.classList.remove('hidden');
        dom.standaloneBar.classList.remove('hidden');
        dom.connBadge.classList.remove('hidden');
        document.getElementById('btnWsConnect').addEventListener('click', () => {
            const baseUrl = document.getElementById('wsUrlInput').value.trim();
            const token = document.getElementById('wsTokenInput').value.trim();
            const fullUrl = token ? `${baseUrl}?token=${encodeURIComponent(token)}` : baseUrl;
            EevsUI.selfNetId = EevsUI.getTargetNetId();
            dom.selfNetIdBadge.textContent = `NetID #${EevsUI.selfNetId}`;
            if (EevsUI.ws) EevsUI.ws.close();
            EevsUI.ws = new WebSocket(fullUrl);
            EevsUI.ws.onopen = () => {
                dom.connBadge.innerHTML = '<span class="dot"></span>WS Connected';
                dom.connBadge.className = 'pill pill-ok';
                EevsUI.logEvent(`Connected to ${baseUrl}`);
                EevsUI.refreshState();
            };
            EevsUI.ws.onclose = () => {
                dom.connBadge.innerHTML = '<span class="dot"></span>Disconnected';
                dom.connBadge.className = 'pill pill-danger';
                EevsUI.logEvent('WebSocket disconnected');
            };
            EevsUI.ws.onmessage = (ev) => {
                try {
                    const packet = JSON.parse(ev.data);
                    if (packet.type === 'response' && packet.id && EevsUI.wsPending.has(packet.id)) {
                        const resolve = EevsUI.wsPending.get(packet.id);
                        EevsUI.wsPending.delete(packet.id);
                        resolve(packet.data || {});
                    } else if (packet.type === 'event') {
                        EevsUI.handleRealtimeEevsEvent(packet.topic, packet.netId, packet.data);
                    }
                } catch (_err) {}
            };
        });
    }

    window.EevsUI = EevsUI;
})();
