# EEVS Example & Reference Plugins (`plugins/`)

This directory contains reference plugins provided as examples to showcase the capabilities of the EEVS C++ Plugin SDK (`IDspProcessor`, `IAudioTap`, `IVoicePacketFilter`, `IClipService`, `IControlApiService`, and `IEventBusService`), along with the shared static helper library (`eevs_plugin_common`).

When `EEVS_BUILD_PLUGINS` is enabled, compiled plugins are output to the `plugins/` directory next to the `eevs` binary. You can use these plugins as-is, customize them for your server, or use them as templates for building your own plugins.

---

## 1. `voice_effects` ([`voice-effects/voice_effects_plugin.cpp`](./voice-effects/voice_effects_plugin.cpp))

Real-time server-side voice effects and acoustic environment processor powered by `signalsmith-dsp` and `signalsmith-stretch`.

### Capabilities
- **Two-Stage Processing**:
  - `targetBranch: "all"` (`DspStage::PerSpeakerIngress`): Transforms the speaker's microphone stream once before routing (e.g. `mask`, `robot`, `demon`, `helium`, `underwater`).
  - `targetBranch: "2D"` or `"3D"` (`DspStage::PerRouteBranch`): Applies DSP only to a specific routing mode branch (e.g. `tunnel_phone` or `radio` on `2D` phone/radio calls while leaving `3D` proximity speech untouched).
- **8-Stage Signal Chain**: Pitch/formant shifting (`SignalsmithStretch`), sub-octave layer, packet-loss dropout gate, sample-rate decimation/quantization, ring modulation, 4th-order Butterworth HPF/LPF + parametric mid-peak EQ, soft-clipping saturation, slapback delay, and 4-channel Hadamard FDN reverb.

### Control API Endpoints
| Method & Path | Payload | Description |
|---|---|---|
| `GET /effects/list` | — | Returns built-in presets and all currently active speaker/branch chains |
| `POST /effects/set` | `{ "netId": 1, "preset": "mask", "targetBranch": "all", ... }` | Applies a preset and optional parameter overrides (`pitchSemitones`, `highpassHz`, `lowpassHz`, `drive`, `noiseLevel`, `dropoutRate`, `reverbWet`, `outputGain`) |
| `POST /effects/clear` | `{ "netId": 1, "targetBranch": "all" }` | Clears effects for a speaker/branch (or `"targetBranch": "everything"` to clear all speakers) |

### Published Events
- `effects.changed`
- `effects.cleared`

---

## 2. `voice_recorder` ([`voice-recorder/voice_recorder_plugin.cpp`](./voice-recorder/voice_recorder_plugin.cpp))

Live per-player microphone recorder and `.wav` / `.eevs` clip serializer.

### Capabilities
- Captures raw 10ms Opus frames (`IAudioTap::OnOpusFrame`) for zero-transcode `.eevs` storage and `PostIngressDsp` PCM frames (`IAudioTap::OnPcmFrame`) when DSP effects are active.
- Supports **Isolated Recording** (`isolate: true` via `IVoicePacketFilter` returning `PacketFilterVerdict::TapOnlyNoForward`), allowing players to record voicemails or test clips without broadcasting to nearby players.
- Exports clips to disk as standard 16-bit PCM `.wav` or lossless `.eevs` Opus containers and loads `.wav`/`.eevs` files from disk into `ClipManager`.

### Control API Endpoints
| Method & Path | Payload | Description |
|---|---|---|
| `POST /recorder/start` | `{ "netId": 1, "clipId": "opt_id", "isolate": true, "maxSeconds": 30 }` | Starts recording a player's voice |
| `POST /recorder/stop` | `{ "netId": 1, "wait": true }` | Stops recording and stores the clip in `ClipManager` |
| `POST /clip/save_wav` | `{ "clipId": "rec_1", "file": "recordings/rec_1.wav" }` | Exports a recorded clip to disk as `.wav` |
| `POST /clip/save` | `{ "clipId": "rec_1", "file": "clips/rec_1.eevs" }` | Exports a recorded clip to disk as `.eevs` |
| `POST /clip/load` | `{ "file": "recordings/rec_1.wav", "clipId": "custom_id" }` | Loads a `.wav` or `.eevs` file from disk into `ClipManager` |

### Configuration Keys (`eevs.cfg`)
- `eevs_recordings_dir` (default: `"recordings"`)
- `eevs_clips_dir` (default: `"clips"`)

### Published Events
- `recorder.clip_ready`

---

## 3. `audio_cpp_client` ([`audio-cpp-client/audio_cpp_client_plugin.cpp`](./audio-cpp-client/audio_cpp_client_plugin.cpp))

Asynchronous client for [`audio.cpp`](https://github.com/ggml-org/audio.cpp) (`audiocpp_server`) providing Speech-to-Text (ASR), Text-to-Speech (TTS), and generative Voice Design (`vdes`).

### Capabilities
- **Speech-to-Text (`/stt/*`)**: Captures `PreDsp` microphone audio, downsamples to 16 kHz mono WAV on the fly, and transcribes via `/v1/tasks/run` (`task: "asr"`).
- **Text-to-Speech (`/tts/*`)**: Synthesizes speech from text and reference voices, normalizes peak gain, stores the resulting Opus frames in `ClipManager`, and optionally broadcasts the audio in-game.
- **Voice Design (`/vdes/*`)**: Generates consistent character voices from natural-language `instruct` prompts and deterministic `seed` values, with automatic acoustic reference anchoring (`/v1/ui/upload`) for models that support reference conditioning.

### Control API Endpoints
| Method & Path | Description |
|---|---|
| `GET /stt/models`, `GET /tts/models`, `GET /vdes/models` | Discovers registered models, voices, and dynamic parameter schemas from `audiocpp_server` |
| `POST /stt/start` / `POST /stt/stop` / `GET /stt/result` | Starts/stops microphone speech capture and returns transcription results |
| `POST /tts/generate` / `POST /tts/speak` | Synthesizes (and optionally plays) TTS audio |
| `POST /vdes/generate` / `POST /vdes/speak` | Synthesizes (and optionally plays) prompt-designed voice audio with per-speaker `seed` and `gain` |

### Configuration Keys (`eevs.cfg`)
- `eevs_audiocpp_host` (default: `"127.0.0.1"`)
- `eevs_audiocpp_port` (default: `"8080"`)
- `eevs_audiocpp_tts_model`, `eevs_audiocpp_vdes_model`, `eevs_audiocpp_stt_model` (optional default model IDs)

### Published Events
- `stt.completed`
- `tts.clip_ready`
- `vdes.clip_ready`
