# EEVS Showcase & NUI Sandbox (`demo/eevs_demo`)

`eevs_demo` is an interactive FiveM showcase resource demonstrating EEVS's routing matrix, clip recorder, Signalsmith DSP voice effects, [`audio.cpp`](https://github.com/0xShug0/audio.cpp) TTS/STT/Voice Design, and tool-calling OpenAI LLM NPCs.

---

## Installation

1. Copy or symlink `demo/eevs_demo` into your FiveM `resources/` folder (requires `eevs` resource started before it).
2. Install and start [`audio.cpp` (`audiocpp_server`)](https://github.com/0xShug0/audio.cpp), then configure its host and port in [`eevs.cfg`](../../eevs.cfg) (used by the `audio_cpp_client` plugin for TTS, STT, and Voice Design):
   ```ini
   eevs_audiocpp_host "127.0.0.1"
   eevs_audiocpp_port "8080"
   ```
3. Optional LLM convars in your `cfx-server`'s `server.cfg` (for Interactive NPCs). `eevs_llm_url` can point to **any OpenAI-compatible API endpoint**, such as [LiteLLM](https://github.com/BerriAI/litellm), `llama-server` (`llama.cpp`), Ollama, vLLM, Groq, or OpenAI:
   ```ini
   set eevs_llm_url "http://127.0.0.1:8081/v1"
   set eevs_llm_key "your_api_key_if_required"
   set eevs_llm_model "gpt-4o-mini"
   ```
4. Add `ensure eevs_demo` to your `cfx-server`'s `server.cfg`.

---

## In-Game Hotkeys & Commands

| Command / Hotkey | Description |
|---|---|
| `/eevs` or **`[F7]`** | Open the EEVS NUI Sandbox & Visualizer (Push-to-Talk **`[N]`** stays active while browsing) |
| `/audio_cpp [url]` | Open the embedded `audio.cpp` WebUI & Model Manager modal inside FiveM |
| **`[Z]`** or `/eevs_range [mode]` | Cycle spatial talker proximity range (`Whisper 3m` $\rightarrow$ `Normal 12m` $\rightarrow$ `Shout 28m` $\rightarrow$ `Megaphone PA 75m + DSP` $\rightarrow$ `default`) |
| **Hold `[J]`** | Push-to-Transcribe (STT) hotkey |
| `/eevs_echo [2d\|3d\|off]` | Route your own microphone back to yourself for solo loopback testing |
| `/eevs_fx <preset\|off> [all\|2d\|3d]` | Apply a real-time Signalsmith DSP preset (`mask`, `radio`, `tunnel_phone`, `megaphone`, `robot`, `demon`, `helium`, `underwater`, `cave_reverb`) |
| `/eevs_rec <start\|stop\|play\|stop_play\|wav\|eevs\|load>` | Record your microphone, play clips in 2D/3D, or export/import `.wav` and `.eevs` files |
| `/eevs_tts <2d\|3d> <text...>` | Synthesize and broadcast speech from your character via `audio.cpp` |
| `/eevs_stt <start\|stop> [lang] [isolate]` | Start or stop speech-to-text capture |
| `/eevs_wiretap <add\|merge\|remove\|block\|clear_all> <targetId> [2d\|3d]` | Configure asymmetric routing overrides |

---

## NUI Control Panel Tabs (`/eevs`)

1. **Routing & Sessions**: Live player session list with real-time VAD indicators and custom `2D`/`3D`/`Both` route matrix editor.
2. **Recorder & Clips**: Isolated/live microphone recorder, NPC voice actor stage, `.wav`/`.eevs` disk I/O, and multi-track simultaneous clip mixer.
3. **Voice Effects**: Real-time `signalsmith-dsp` & `signalsmith-stretch` preset selector and custom parameter overrides with per-branch (`all`, `2D`, `3D`) targeting.
4. **Speech Synthesis (TTS)**: Dynamic model & parameter discovery from `audio.cpp` (`/v1/models?include_params=true`).
5. **Voice Design**: Natural-language voice prompt + deterministic `seed` synthesis (`task: vdes`).
6. **Speech-to-Text (STT)**: Live ASR transcription with RMS dB telemetry.
7. **Interactive NPC**: Spawn in-world NPCs (*LSPD Officer*, *Paramedic*, *Store Clerk*, *Gang Member*, or *Custom*) with unique Voice Design seeds, per-NPC gain sliders, dynamic `3d_nearby` spatial routing, 3D speech bubbles, facial lip-sync, and OpenAI function-calling (`npc_respond`) physical game actions.
