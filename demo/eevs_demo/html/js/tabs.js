// ============================================================================
// EEVS NUI - Routing, Recorder, Clips, TTS, Voice Design & STT Tabs
// ============================================================================
(() => {
    const EevsUI = window.EevsUI;
    const { dom } = EevsUI;

    EevsUI.renderSessions = function renderSessions(sessions) {
        dom.sessionsBadge.textContent = `Sessions: ${sessions.length}`;
        dom.sessionList.innerHTML = '';

        // Populate Target Speaker NetID combobox in Voice Effects tab
        const fxTargetSelect = document.getElementById('fxTargetNetIdInput');
        if (fxTargetSelect && fxTargetSelect.tagName === 'SELECT') {
            const prevVal = fxTargetSelect.value || 'self';
            fxTargetSelect.innerHTML = `<option value="self">Self (NetID #${EevsUI.selfNetId || 1})</option>`;
            for (const s of sessions) {
                const isSelf = Number(s.clientId) === Number(EevsUI.selfNetId) || Number(s.netId) === Number(EevsUI.selfNetId);
                const opt = document.createElement('option');
                opt.value = String(s.clientId);
                opt.textContent = `Server ID #${s.clientId} (Ped NetID #${s.netId})${isSelf ? ' [YOU]' : ''}`;
                fxTargetSelect.appendChild(opt);
            }
            if (Array.from(fxTargetSelect.options).some((o) => o.value === prevVal)) {
                fxTargetSelect.value = prevVal;
            }
        }

        if (sessions.length === 0) {
            dom.sessionList.innerHTML = '<div class="empty-state">No registered voice sessions yet.<br/>Connect to FiveM server with voice enabled.</div>';
            return;
        }
        for (const s of sessions) {
            const isSelf = Number(s.clientId) === Number(EevsUI.selfNetId) || Number(s.netId) === Number(EevsUI.selfNetId);
            const row = document.createElement('div');
            row.className = `list-item selectable ${s.talking ? 'talking' : ''}`;
            row.id = `session-net-${s.netId}`;
            row.dataset.clientId = String(s.clientId);
            row.innerHTML = `
                <div class="item-main">
                    <div class="item-title">
                        <span>Server ID #${s.clientId}</span>
                        ${isSelf ? '<span class="mini-badge">YOU</span>' : ''}
                        <span class="pill ${s.authenticated ? 'pill-ok' : 'pill-neutral'}" style="font-size:10px;padding:1px 6px;">
                            ${s.authenticated ? 'UDP Auth' : 'Handshake'}
                        </span>
                    </div>
                    <div class="item-meta">Ped NetID: #${s.netId}</div>
                </div>
                <div class="btn-row-tight">
                    <span id="vad-badge-${s.netId}" data-vad-client="${s.clientId}" class="pill ${s.talking ? 'pill-ok' : 'pill-neutral'}">
                        ${s.talking ? '🔊 SPEAKING' : 'Idle'}
                    </span>
                </div>
            `;
            row.addEventListener('click', () => {
                document.getElementById('routeFromInput').value = s.clientId;
                document.getElementById('routeToInput').value = EevsUI.selfNetId;
                EevsUI.logEvent(`Selected Server ID #${s.clientId} (Ped NetID #${s.netId}) in routing form`);
            });
            dom.sessionList.appendChild(row);
        }
    };

    EevsUI.renderClips = function renderClips(clipsData) {
        EevsUI.cachedClips = clipsData.clips || [];
        const totalBytes = clipsData.totalBytes || 0;
        const maxBytes = clipsData.maxTransientBytes || (64 * 1024 * 1024);
        const kb = (totalBytes / 1024).toFixed(1);
        const pct = Math.min(100, (totalBytes / maxBytes) * 100);
        dom.clipMemStats.textContent = `${kb} KB`;
        dom.clipMemBar.style.width = `${Math.max(pct, EevsUI.cachedClips.length > 0 ? 3 : 0)}%`;
        dom.clipCountLabel.textContent = `${EevsUI.cachedClips.length} clip${EevsUI.cachedClips.length === 1 ? '' : 's'} in RAM`;
        dom.clipList.innerHTML = '';
        if (EevsUI.cachedClips.length === 0) {
            dom.clipList.innerHTML = '<div class="empty-state">No audio clips in ClipManager.<br/>Record your mic, synthesize TTS, or load a .wav/.eevs file from disk.</div>';
            return;
        }
        for (const c of EevsUI.cachedClips) {
            const sizeKb = ((c.bytes || 0) / 1024).toFixed(1);
            const row = document.createElement('div');
            row.className = 'list-item';
            row.innerHTML = `
                <div class="item-main">
                    <div class="item-title">
                        <span>${c.clipId}</span>
                        ${c.pinned ? '<span class="mini-badge" title="Pinned in RAM">PINNED</span>' : ''}
                    </div>
                    <div class="item-meta">${c.durationMs} ms &bull; ${c.frames || Math.round(c.durationMs / 10)} Opus frames &bull; ${sizeKb} KB</div>
                </div>
                <div class="btn-row-tight">
                    <button class="btn btn-xs btn-primary" data-play="${c.clipId}" data-mode="2D" title="Play 2D Non-Spatial">▶ 2D</button>
                    <button class="btn btn-xs btn-secondary" data-play="${c.clipId}" data-mode="3D" title="Play 3D Spatial">▶ 3D</button>
                    <button class="btn btn-xs btn-secondary" data-npc-play="${c.clipId}" title="Play through spawned NPC in front of you">🧍▶ NPC</button>
                    <button class="btn btn-xs btn-secondary" data-wav="${c.clipId}" title="Export as .wav in recordings/">💾 .WAV</button>
                    <button class="btn btn-xs btn-secondary" data-eevs="${c.clipId}" title="Export lossless .eevs Opus container in clips/">📦 .EEVS</button>
                    <button class="btn btn-xs btn-danger-soft" data-del="${c.clipId}" title="Delete from RAM">✕</button>
                </div>
            `;
            dom.clipList.appendChild(row);
        }
    };

    dom.clipList.addEventListener('click', async (e) => {
        const btn = e.target.closest('button');
        if (!btn) return;
        const playClipId = btn.getAttribute('data-play');
        const npcPlayClipId = btn.getAttribute('data-npc-play');
        const wavClipId = btn.getAttribute('data-wav');
        const eevsClipId = btn.getAttribute('data-eevs');
        const delClipId = btn.getAttribute('data-del');
        if (playClipId) {
            const mode = btn.getAttribute('data-mode') || '2D';
            const selectedSpeaker = dom.npcActiveSelect ? dom.npcActiveSelect.value : 'self';
            const npcId = selectedSpeaker.startsWith('npc:') ? parseInt(selectedSpeaker.slice(4), 10) : null;
            const res = await EevsUI.sendControlRequest('POST', '/clip/play', {
                clipId: playClipId,
                senderNetId: 'self',
                mode,
                npcId
            });
            EevsUI.logEvent(res.ok ? `Playing '${playClipId}' (${mode}${npcId ? `, NPC #${npcId}` : ''})` : `Play failed: ${EevsUI.formatError(res)}`);
        } else if (npcPlayClipId) {
            let npcId = EevsUI.getSelectedNpcId();
            if (!npcId && EevsUI.isFiveMNui) {
                const spawnCfg = typeof EevsUI.getSelectedNpcSpawnConfig === 'function' ? EevsUI.getSelectedNpcSpawnConfig() : {};
                const spawnRes = await EevsUI.sendNpcAction('spawn', spawnCfg);
                if (spawnRes.ok && spawnRes.npc) {
                    npcId = spawnRes.npc.npcId;
                }
            }
            const res = await EevsUI.sendControlRequest('POST', '/clip/play', {
                clipId: npcPlayClipId,
                senderNetId: 'self',
                mode: '3D',
                npcId: npcId || 1
            });
            EevsUI.logEvent(res.ok ? `NPC #${npcId || 1} speaking clip '${npcPlayClipId}' (3D Spatial)` : `NPC play failed: ${EevsUI.formatError(res)}`);
        } else if (wavClipId) {
            const res = await EevsUI.sendControlRequest('POST', '/clip/save_wav', {
                clipId: wavClipId,
                format: 'wav'
            });
            if (res.ok && res.file) {
                document.getElementById('loadFilePathInput').value = res.file;
            }
            EevsUI.logEvent(res.ok ? `Exported WAV -> '${res.file}'` : `WAV export failed: ${EevsUI.formatError(res)}`);
        } else if (eevsClipId) {
            const res = await EevsUI.sendControlRequest('POST', '/clip/save', {
                clipId: eevsClipId,
                format: 'eevs'
            });
            if (res.ok && res.file) {
                document.getElementById('loadFilePathInput').value = res.file;
            }
            EevsUI.logEvent(res.ok ? `Exported lossless EEVS -> '${res.file}'` : `EEVS export failed: ${EevsUI.formatError(res)}`);
        } else if (delClipId) {
            const res = await EevsUI.sendControlRequest('POST', '/clip/delete', { clipId: delClipId });
            EevsUI.logEvent(res.ok ? `Deleted clip '${delClipId}'` : `Delete failed: ${EevsUI.formatError(res)}`);
            EevsUI.refreshState();
        }
    });

    document.getElementById('btnPlayAll2D').addEventListener('click', async () => {
        if (EevsUI.cachedClips.length === 0) {
            EevsUI.logEvent('No clips available to mix! Record or synthesize at least 1 clip first.');
            return;
        }
        const toPlay = EevsUI.cachedClips.slice(0, 2);
        await Promise.all(toPlay.map((c) => EevsUI.sendControlRequest('POST', '/clip/play', {
            clipId: c.clipId,
            senderNetId: 'self',
            mode: '2D'
        })));
        EevsUI.logEvent(`Triggered simultaneous 2D mix of ${toPlay.map((c) => c.clipId).join(' + ')}`);
    });

    async function stopAllSelfPlaybacks() {
        const res = await EevsUI.sendControlRequest('POST', '/clip/stop', { senderNetId: 'self' });
        EevsUI.logEvent(res.ok ? `Stopped active playbacks (count=${res.stoppedCount ?? 0})` : `Stop failed: ${EevsUI.formatError(res)}`);
    }

    async function clearAllCachedClips() {
        const clearRes = await EevsUI.sendControlRequest('POST', '/clip/clear', {});
        if (clearRes && clearRes.ok) {
            EevsUI.logEvent(`Cleared ${clearRes.cleared ?? EevsUI.cachedClips.length} clip(s) from LRU cache`);
            EevsUI.refreshState();
            return;
        }
        const ids = EevsUI.cachedClips.map((c) => c.clipId).filter(Boolean);
        await Promise.all(ids.map((id) => EevsUI.sendControlRequest('POST', '/clip/delete', { clipId: id })));
        EevsUI.logEvent(`Cleared ${ids.length} clip(s) from LRU cache`);
        EevsUI.refreshState();
    }

    document.getElementById('btnStopClips').addEventListener('click', stopAllSelfPlaybacks);
    const btnClearClipCache = document.getElementById('btnClearClipCache');
    if (btnClearClipCache) {
        btnClearClipCache.addEventListener('click', clearAllCachedClips);
    }

    document.getElementById('btnNpcSpawn').addEventListener('click', async () => {
        const spawnCfg = typeof EevsUI.getSelectedNpcSpawnConfig === 'function' ? EevsUI.getSelectedNpcSpawnConfig() : {};
        const res = await EevsUI.sendNpcAction('spawn', spawnCfg);
        if (res.ok && res.npc) {
            EevsUI.logEvent(`Spawned ${res.npc.label} [${spawnCfg.modelName}] in front of you!`);
        } else {
            EevsUI.logEvent(`Spawn NPC failed: ${EevsUI.formatError(res)}`);
        }
    });

    document.getElementById('btnNpcClear').addEventListener('click', async () => {
        const res = await EevsUI.sendNpcAction('clear');
        EevsUI.logEvent(`Cleared ${res.removed ?? 0} spawned NPC(s)`);
    });

    document.getElementById('btnLoadClipDisk').addEventListener('click', async () => {
        const file = document.getElementById('loadFilePathInput').value.trim();
        const clipId = document.getElementById('loadClipIdInput').value.trim();
        if (!file) {
            EevsUI.logEvent('Enter a .wav or .eevs file path to load');
            return;
        }
        const res = await EevsUI.sendControlRequest('POST', '/clip/load', { file, clipId });
        if (res.ok) {
            EevsUI.logEvent(`Loaded '${res.clipId}' (${res.durationMs}ms) from '${file}'`);
            EevsUI.refreshState();
        } else {
            EevsUI.logEvent(`Load failed: ${EevsUI.formatError(res)}`);
        }
    });

    EevsUI.updateEchoUi = function updateEchoUi(mode) {
        const norm = (mode || 'off').toUpperCase();
        dom.echoStateBadge.textContent = norm;
        document.querySelectorAll('[data-echo]').forEach((btn) => {
            btn.classList.toggle('active', btn.getAttribute('data-echo').toUpperCase() === norm);
        });
    };

    document.querySelectorAll('[data-echo]').forEach((btn) => {
        btn.addEventListener('click', async () => {
            const mode = btn.getAttribute('data-echo');
            if (mode === 'off') {
                await EevsUI.sendControlRequest('POST', '/route/remove', { fromNetId: 'self', toNetId: 'self' });
                EevsUI.updateEchoUi('off');
                EevsUI.logEvent('Self-echo loopback disabled');
            } else {
                const res = await EevsUI.sendControlRequest('POST', '/route/add', { fromNetId: 'self', toNetId: 'self', mode });
                if (res.ok) {
                    EevsUI.updateEchoUi(mode);
                    EevsUI.logEvent(`Self-echo loopback enabled (${mode})`);
                } else {
                    EevsUI.logEvent(`Echo route failed: ${EevsUI.formatError(res)}`);
                }
            }
        });
    });

    const proxRangeBadge = document.getElementById('proxRangeBadge');
    EevsUI.updateProximityUi = function updateProximityUi(mode, rangeMeters) {
        const norm = (mode || 'unlimited').toLowerCase();
        if (proxRangeBadge) {
            if (norm === 'unlimited' || !rangeMeters || Number(rangeMeters) <= 0) {
                proxRangeBadge.textContent = 'MAX';
            } else {
                proxRangeBadge.textContent = `${norm.toUpperCase()} (${Math.round(Number(rangeMeters))}m)`;
            }
        }
        document.querySelectorAll('[data-prox]').forEach((btn) => {
            btn.classList.toggle('active', btn.getAttribute('data-prox').toLowerCase() === norm);
        });
    };

    document.querySelectorAll('[data-prox]').forEach((btn) => {
        btn.addEventListener('click', async () => {
            const mode = btn.getAttribute('data-prox') || 'unlimited';
            const res = await EevsUI.sendControlRequest('POST', '/player/proximity', { mode });
            if (res && res.ok) {
                EevsUI.updateProximityUi(res.mode, res.range);
                EevsUI.logEvent(`Set Talker Proximity Range: ${res.label}`);
                if (typeof EevsUI.refreshActiveEffects === 'function') {
                    EevsUI.refreshActiveEffects();
                }
            } else {
                EevsUI.logEvent(`Proximity range failed: ${EevsUI.formatError(res)}`);
            }
        });
    });

    async function mutateRoute(endpoint) {
        const fromNetId = parseInt(document.getElementById('routeFromInput').value, 10);
        const toNetId = parseInt(document.getElementById('routeToInput').value, 10);
        const mode = document.getElementById('routeModeSelect').value;
        const action = document.getElementById('routeActionSelect').value || 'force';
        if (!fromNetId || !toNetId) {
            EevsUI.logEvent('Specify both Speaker NetID and Listener NetID (or click a session row)');
            return;
        }
        const res = await EevsUI.sendControlRequest('POST', endpoint, { fromNetId, toNetId, mode, action });
        EevsUI.logEvent(`${endpoint} (${fromNetId} -> ${toNetId}, mode=${mode}, action=${action}): ${res.ok ? 'OK' : EevsUI.formatError(res)}`);
    }

    document.getElementById('btnRouteAdd').addEventListener('click', () => mutateRoute('/route/add'));
    document.getElementById('btnRouteBlock').addEventListener('click', () => mutateRoute('/route/block'));
    document.getElementById('btnRouteRemove').addEventListener('click', () => mutateRoute('/route/remove'));
    document.getElementById('btnRouteClearAll').addEventListener('click', async () => {
        const res = await EevsUI.sendControlRequest('POST', '/route/clear', {});
        EevsUI.updateEchoUi('off');
        EevsUI.logEvent(res.ok ? 'Cleared all custom routing overrides' : `Clear failed: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnQuickWiretap').addEventListener('click', async () => {
        const target = parseInt(document.getElementById('routeFromInput').value, 10);
        if (!target) {
            EevsUI.logEvent('Click a session or enter Speaker NetID to wiretap');
            return;
        }
        const res = await EevsUI.sendControlRequest('POST', '/route/add', {
            fromNetId: target,
            toNetId: 'self',
            mode: '2D',
            action: 'force'
        });
        EevsUI.logEvent(res.ok ? `One-way 2D wiretap active: NetID #${target} -> You` : `Wiretap failed: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnQuickIntercom').addEventListener('click', async () => {
        const target = parseInt(document.getElementById('routeToInput').value, 10)
            || parseInt(document.getElementById('routeFromInput').value, 10);
        if (!target) {
            EevsUI.logEvent('Click a session or enter Listener NetID for 2D intercom');
            return;
        }
        const res = await EevsUI.sendControlRequest('POST', '/route/add', {
            fromNetId: 'self',
            toNetId: target,
            mode: '2D',
            action: 'force'
        });
        EevsUI.logEvent(res.ok ? `One-way 2D intercom active: You -> NetID #${target}` : `Intercom failed: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnQuickClearSelf').addEventListener('click', async () => {
        const res = await EevsUI.sendControlRequest('POST', '/route/clear', { fromNetId: 'self' });
        EevsUI.updateEchoUi('off');
        EevsUI.logEvent(res.ok ? 'Cleared custom routes for your speaker NetID' : `Clear failed: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnRecStart').addEventListener('click', async () => {
        const isolate = document.getElementById('recIsolate').checked;
        const clipId = document.getElementById('recClipIdInput').value.trim();
        const res = await EevsUI.sendControlRequest('POST', '/recorder/start', {
            netId: 'self',
            clipId,
            isolate,
            maxSeconds: 30
        });
        if (res.ok) {
            dom.recStatusPill.textContent = '● RECORDING';
            dom.recStatusPill.className = 'pill pill-danger';
            EevsUI.logEvent(`Recording started (isolate=${isolate}). Hold [N] to speak!`);
        } else {
            EevsUI.logEvent(`Recorder start error: ${EevsUI.formatError(res)}`);
        }
    });

    document.getElementById('btnRecStop').addEventListener('click', async () => {
        const res = await EevsUI.sendControlRequest('POST', '/recorder/stop', { netId: 'self', wait: true });
        dom.recStatusPill.textContent = 'IDLE';
        dom.recStatusPill.className = 'pill pill-neutral';
        if (res.ok) {
            EevsUI.logEvent(`Saved recording '${res.clipId}' (${res.durationMs}ms)`);
            EevsUI.refreshState();
        } else {
            EevsUI.logEvent(`Recorder stop error: ${EevsUI.formatError(res)}`);
        }
    });

    // STT Controls
    document.getElementById('btnRefreshSttModels').addEventListener('click', EevsUI.refreshSttModels);
    if (dom.sttModelSelect) {
        dom.sttModelSelect.addEventListener('change', EevsUI.refreshActiveSttParamUi);
    }

    document.getElementById('btnSttStart').addEventListener('click', async () => {
        const isolate = document.getElementById('sttIsolate').checked;
        const translate = document.getElementById('sttTranslate').checked;
        const language = document.getElementById('sttLangSelect').value || 'en';
        const selectedModel = dom.sttModelSelect ? dom.sttModelSelect.value : '';
        const initialPrompt = document.getElementById('sttPromptInput').value.trim();
        const options = EevsUI.collectDynamicParams(dom.sttDynamicParamsGrid);
        if (selectedModel) options.model = selectedModel;
        if (translate) options.translate = true;
        if (initialPrompt) options.initial_prompt = initialPrompt;
        dom.sttOutput.textContent = 'Listening... Hold Push-to-Talk [N] and speak into your microphone.';
        const res = await EevsUI.sendControlRequest('POST', '/stt/start', {
            netId: 'self',
            language,
            isolate,
            options
        });
        EevsUI.logEvent(res.ok ? `STT capture started (lang=${language}, model=${selectedModel || 'auto'})` : `STT error: ${EevsUI.formatError(res)}`);
    });

    document.getElementById('btnSttStop').addEventListener('click', async () => {
        const translate = document.getElementById('sttTranslate').checked;
        const selectedModel = dom.sttModelSelect ? dom.sttModelSelect.value : '';
        const initialPrompt = document.getElementById('sttPromptInput').value.trim();
        const options = EevsUI.collectDynamicParams(dom.sttDynamicParamsGrid);
        if (selectedModel) options.model = selectedModel;
        if (translate) options.translate = true;
        if (initialPrompt) options.initial_prompt = initialPrompt;
        dom.sttOutput.textContent = 'Running STT transcription via audio.cpp...';
        const res = await EevsUI.sendControlRequest('POST', '/stt/stop', {
            netId: 'self',
            wait: true,
            options
        });
        if (res.ok) {
            EevsUI.renderSttResult(res);
            EevsUI.logEvent(`STT completed: "${res.text}"`);
        } else {
            const errMsg = EevsUI.formatError(res);
            dom.sttOutput.textContent = `Error: ${errMsg}`;
            EevsUI.logEvent(`STT error: ${errMsg}`);
        }
    });

    EevsUI.renderSttResult = function renderSttResult(data) {
        dom.sttOutput.textContent = data.text || '(No speech detected)';
        if (data.durationMs !== undefined) {
            dom.sttMetricDur.textContent = `Duration: ${data.durationMs}ms`;
        }
        if (data.confidence !== undefined) {
            dom.sttMetricConf.textContent = `Confidence: ${(Number(data.confidence) * 100).toFixed(1)}%`;
        }
        if (data.rmsDb !== undefined) {
            dom.sttMetricRms.textContent = `RMS: ${Number(data.rmsDb).toFixed(1)} dB`;
        }
    };

    // Neural TTS Controls
    document.getElementById('btnRefreshTtsModels').addEventListener('click', EevsUI.refreshTtsModels);
    if (dom.ttsModelSelect) {
        dom.ttsModelSelect.addEventListener('change', EevsUI.refreshActiveTtsParamUi);
    }
    document.querySelectorAll('[data-tts-prompt]').forEach((chip) => {
        chip.addEventListener('click', () => {
            document.getElementById('ttsTextInput').value = chip.getAttribute('data-tts-prompt');
        });
    });

    document.getElementById('btnTtsSpeak').addEventListener('click', async () => {
        const text = document.getElementById('ttsTextInput').value.trim();
        const mode = document.getElementById('ttsModeSelect').value;
        const voice = document.getElementById('ttsVoiceSelect').value || 'female';
        const selectedModel = dom.ttsModelSelect ? dom.ttsModelSelect.value : '';
        const options = EevsUI.collectDynamicParams(dom.ttsDynamicParamsGrid);
        if (selectedModel) options.model = selectedModel;
        options.voice = voice;
        if (!text) return;
        EevsUI.logEvent('Synthesizing speech via audio.cpp...');
        const res = await EevsUI.sendControlRequest('POST', '/tts/speak', {
            senderNetId: 'self',
            text,
            voice,
            mode,
            options
        });
        if (res.ok) {
            EevsUI.logEvent(`Broadcasting TTS clip '${res.clipId}' (${res.durationMs}ms, ${mode})`);
            EevsUI.refreshState();
        } else {
            EevsUI.logEvent(`TTS error: ${EevsUI.formatError(res)}`);
        }
    });

    const btnTtsCache = document.getElementById('btnTtsCache');
    if (btnTtsCache) {
        btnTtsCache.addEventListener('click', async () => {
            const text = document.getElementById('ttsTextInput').value.trim();
            const voice = document.getElementById('ttsVoiceSelect').value || 'female';
            const selectedModel = dom.ttsModelSelect ? dom.ttsModelSelect.value : '';
            const options = EevsUI.collectDynamicParams(dom.ttsDynamicParamsGrid);
            if (selectedModel) options.model = selectedModel;
            options.voice = voice;
            if (!text) return;
            EevsUI.logEvent('Generating & caching TTS clip via audio.cpp...');
            const res = await EevsUI.sendControlRequest('POST', '/tts/generate', {
                text,
                voice,
                options
            });
            if (res.ok) {
                EevsUI.logEvent(`Cached TTS clip '${res.clipId}' (${res.durationMs}ms)`);
                EevsUI.refreshState();
            } else {
                EevsUI.logEvent(`TTS cache error: ${EevsUI.formatError(res)}`);
            }
        });
    }

    // Voice Design Controls (Tab 3B: #tab-vdes)
    const btnRefreshVdesModels = document.getElementById('btnRefreshVdesModels');
    if (btnRefreshVdesModels) {
        btnRefreshVdesModels.addEventListener('click', EevsUI.refreshVdesModels);
    }
    if (dom.vdesModelSelect) {
        dom.vdesModelSelect.addEventListener('change', () => {
            if (dom.llmVdesModelSelect && dom.llmVdesModelSelect.value !== dom.vdesModelSelect.value) {
                dom.llmVdesModelSelect.value = dom.vdesModelSelect.value;
            }
            EevsUI.refreshActiveVdesParamUi();
            if (typeof EevsUI.syncNpcConfigToClient === 'function') {
                EevsUI.syncNpcConfigToClient();
            }
        });
    }

    const btnVdesRandomSeed = document.getElementById('btnVdesRandomSeed');
    if (btnVdesRandomSeed && dom.vdesSeedInput) {
        btnVdesRandomSeed.addEventListener('click', () => {
            dom.vdesSeedInput.value = String(Math.floor(100000 + Math.random() * 899999));
        });
    }

    document.querySelectorAll('[data-vdes-instruct]').forEach((chip) => {
        chip.addEventListener('click', () => {
            if (dom.vdesInstructInput) {
                dom.vdesInstructInput.value = chip.getAttribute('data-vdes-instruct');
            }
            const chipSeed = chip.getAttribute('data-vdes-seed');
            if (chipSeed && dom.vdesSeedInput) {
                dom.vdesSeedInput.value = chipSeed;
            }
        });
    });

    const btnVdesSpeak = document.getElementById('btnVdesSpeak');
    if (btnVdesSpeak) {
        btnVdesSpeak.addEventListener('click', async () => {
            const text = dom.vdesTextInput ? dom.vdesTextInput.value.trim() : '';
            const instruct = dom.vdesInstructInput ? dom.vdesInstructInput.value.trim() : '';
            const seedVal = dom.vdesSeedInput && dom.vdesSeedInput.value !== '' ? parseInt(dom.vdesSeedInput.value, 10) : undefined;
            const mode = dom.vdesModeSelect ? dom.vdesModeSelect.value : '3D';
            const selectedModel = dom.vdesModelSelect ? dom.vdesModelSelect.value : '';
            const options = EevsUI.collectDynamicParams(dom.vdesDynamicParamsGrid);
            if (selectedModel) options.model = selectedModel;
            if (!text) return;

            EevsUI.logEvent(`Synthesizing designed voice (seed=${seedVal ?? 'auto'}) via audio.cpp...`);
            const res = await EevsUI.sendControlRequest('POST', '/vdes/speak', {
                senderNetId: 'self',
                text,
                instruct,
                seed: Number.isNaN(seedVal) ? undefined : seedVal,
                mode,
                options
            });
            if (res.ok) {
                EevsUI.logEvent(`Broadcasting Voice Design clip '${res.clipId}' (${res.durationMs}ms, ${mode})`);
                EevsUI.refreshState();
            } else {
                EevsUI.logEvent(`Voice Design error: ${EevsUI.formatError(res)}`);
            }
        });
    }

    const btnVdesCache = document.getElementById('btnVdesCache');
    if (btnVdesCache) {
        btnVdesCache.addEventListener('click', async () => {
            const text = dom.vdesTextInput ? dom.vdesTextInput.value.trim() : '';
            const instruct = dom.vdesInstructInput ? dom.vdesInstructInput.value.trim() : '';
            const seedVal = dom.vdesSeedInput && dom.vdesSeedInput.value !== '' ? parseInt(dom.vdesSeedInput.value, 10) : undefined;
            const selectedModel = dom.vdesModelSelect ? dom.vdesModelSelect.value : '';
            const options = EevsUI.collectDynamicParams(dom.vdesDynamicParamsGrid);
            if (selectedModel) options.model = selectedModel;
            if (!text) return;

            EevsUI.logEvent(`Generating & caching Voice Design clip (seed=${seedVal ?? 'auto'})...`);
            const res = await EevsUI.sendControlRequest('POST', '/vdes/generate', {
                text,
                instruct,
                seed: Number.isNaN(seedVal) ? undefined : seedVal,
                options
            });
            if (res.ok) {
                EevsUI.logEvent(`Cached Voice Design clip '${res.clipId}' (${res.durationMs}ms)`);
                EevsUI.refreshState();
            } else {
                EevsUI.logEvent(`Voice Design cache error: ${EevsUI.formatError(res)}`);
            }
        });
    }

    // Real-Time Voice Effects Controls (Tab: #tab-effects — Signalsmith DSP & Stretch)
    const fxPresetSelect = document.getElementById('fxPresetSelect');
    const fxBranchSelect = document.getElementById('fxBranchSelect');
    const fxTargetNetIdInput = document.getElementById('fxTargetNetIdInput');
    const fxPitchInput = document.getElementById('fxPitchInput');
    const fxHighpassInput = document.getElementById('fxHighpassInput');
    const fxLowpassInput = document.getElementById('fxLowpassInput');
    const fxDriveInput = document.getElementById('fxDriveInput');
    const fxNoiseInput = document.getElementById('fxNoiseInput');
    const fxDropoutInput = document.getElementById('fxDropoutInput');
    const fxReverbWetInput = document.getElementById('fxReverbWetInput');
    const fxOutputGainInput = document.getElementById('fxOutputGainInput');
    const fxActiveCountBadge = document.getElementById('fxActiveCountBadge');
    const fxActiveList = document.getElementById('fxActiveList');

    EevsUI.refreshActiveEffects = async function refreshActiveEffects() {
        if (!fxActiveList) return;
        const res = await EevsUI.sendControlRequest('GET', '/effects/list');
        if (!res || !res.ok) return;

        const active = Array.isArray(res.active) ? res.active : [];
        if (fxActiveCountBadge) {
            fxActiveCountBadge.textContent = `${active.length} active`;
        }
        fxActiveList.innerHTML = '';
        if (active.length === 0) {
            fxActiveList.innerHTML = '<div class="empty-state" style="padding:10px;">No active voice effects. Select a preset above and click Apply (enable Solo Self-Echo in the sidebar to hear yourself!).</div>';
            return;
        }

        for (const item of active) {
            const row = document.createElement('div');
            row.className = 'npc-manage-row';
            row.style.marginBottom = '5px';
            row.innerHTML = `
                <div class="npc-manage-meta">
                    <strong style="color:#fff;">Speaker #${item.clientId} (NetID #${item.netId})</strong>
                    <span class="pill pill-accent">${item.preset}</span>
                    <span class="mini-badge">Branch: ${item.targetBranch}</span>
                    <span class="item-meta">pitch=${Number(item.pitchSemitones).toFixed(1)}st &bull; HPF=${Math.round(item.highpassHz)}Hz &bull; LPF=${Math.round(item.lowpassHz)}Hz &bull; drive=${Number(item.drive).toFixed(2)} &bull; dropout=${Number(item.dropoutRate).toFixed(2)} &bull; reverb=${Number(item.reverbWet).toFixed(2)}</span>
                </div>
                <button type="button" class="btn btn-xs btn-danger-soft" data-clear-fx-net="${item.netId}" data-clear-fx-branch="${item.targetBranch}">✕ Remove</button>
            `;
            fxActiveList.appendChild(row);
        }
    };

    if (fxActiveList) {
        fxActiveList.addEventListener('click', async (ev) => {
            const btn = ev.target.closest('[data-clear-fx-net]');
            if (!btn) return;
            const netId = parseInt(btn.getAttribute('data-clear-fx-net'), 10);
            const targetBranch = btn.getAttribute('data-clear-fx-branch') || 'all';
            const res = await EevsUI.sendControlRequest('POST', '/effects/clear', { netId, targetBranch });
            if (res && res.ok) {
                EevsUI.logEvent(`Cleared '${targetBranch}' DSP effect on NetID #${netId}`);
                EevsUI.refreshActiveEffects();
            }
        });
    }

    async function applyVoiceEffectFromForm(overridePreset = null, overrideBranch = null) {
        if (overridePreset && fxPresetSelect) fxPresetSelect.value = overridePreset;
        if (overrideBranch && fxBranchSelect) fxBranchSelect.value = overrideBranch;

        const rawTarget = fxTargetNetIdInput ? fxTargetNetIdInput.value.trim() : 'self';
        const netId = (rawTarget === '' || rawTarget.toLowerCase() === 'self') ? 'self' : (parseInt(rawTarget, 10) || 'self');
        const payload = {
            netId,
            preset: fxPresetSelect ? fxPresetSelect.value : 'mask',
            targetBranch: fxBranchSelect ? fxBranchSelect.value : 'all'
        };

        const addNum = (el, key, isInt = false) => {
            if (!el || el.value === undefined || String(el.value).trim() === '') return;
            const v = isInt ? parseInt(el.value, 10) : parseFloat(el.value);
            if (!Number.isNaN(v)) payload[key] = v;
        };
        addNum(fxPitchInput, 'pitchSemitones');
        addNum(fxHighpassInput, 'highpassHz');
        addNum(fxLowpassInput, 'lowpassHz');
        addNum(fxDriveInput, 'drive');
        addNum(fxNoiseInput, 'noiseLevel');
        addNum(fxDropoutInput, 'dropoutRate');
        addNum(fxReverbWetInput, 'reverbWet');
        addNum(fxOutputGainInput, 'outputGain');

        const res = await EevsUI.sendControlRequest('POST', '/effects/set', payload);
        if (res && res.ok) {
            EevsUI.logEvent(`Applied DSP preset '${res.preset}' (branch=${res.targetBranch}) on speaker #${res.clientId}`);
            EevsUI.refreshActiveEffects();
        } else {
            EevsUI.logEvent(`Effect error: ${EevsUI.formatError(res)}`);
        }
    }

    document.querySelectorAll('[data-fx-preset]').forEach((chip) => {
        chip.addEventListener('click', () => {
            const preset = chip.getAttribute('data-fx-preset');
            const branch = chip.getAttribute('data-fx-branch') || 'all';
            for (const el of [fxPitchInput, fxHighpassInput, fxLowpassInput, fxDriveInput, fxNoiseInput, fxDropoutInput, fxReverbWetInput, fxOutputGainInput]) {
                if (el) el.value = '';
            }
            applyVoiceEffectFromForm(preset, branch);
        });
    });

    const btnFxApply = document.getElementById('btnFxApply');
    if (btnFxApply) {
        btnFxApply.addEventListener('click', () => applyVoiceEffectFromForm());
    }
    const btnFxClearSelf = document.getElementById('btnFxClearSelf');
    if (btnFxClearSelf) {
        btnFxClearSelf.addEventListener('click', async () => {
            const res = await EevsUI.sendControlRequest('POST', '/effects/clear', { netId: 'self' });
            EevsUI.logEvent(res && res.ok ? 'Cleared all voice effects on your character' : `Clear failed: ${EevsUI.formatError(res)}`);
            EevsUI.refreshActiveEffects();
        });
    }
    const btnFxClearAll = document.getElementById('btnFxClearAll');
    if (btnFxClearAll) {
        btnFxClearAll.addEventListener('click', async () => {
            const res = await EevsUI.sendControlRequest('POST', '/effects/clear', { targetBranch: 'everything' });
            EevsUI.logEvent(res && res.ok ? 'Cleared all voice effects across all speakers' : `Clear failed: ${EevsUI.formatError(res)}`);
            EevsUI.refreshActiveEffects();
        });
    }
})();
