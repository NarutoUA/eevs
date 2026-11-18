# EEVS — Enhanced External Voice Server for FiveM Enhanced

EEVS is a high-performance standalone external voice server and plugin framework for FiveM Enhanced. It serves as a drop-in replacement for the [FiveM Enhanced External Voice Server](https://docs.fivem.net/docs/scripting-manual/voice/#external-voice-server), synchronizing player sessions and routing state from `cfx-server`, relaying and processing real-time player voice streams, and exposing an HTTP & WebSocket Control API alongside a native C++ Plugin SDK.

## Video Showcase

[![EEVS Video Showcase](https://img.youtube.com/vi/FEmXwnn63EM/maxresdefault.jpg)](https://www.youtube.com/watch?v=FEmXwnn63EM)

> ▶️ [**Watch the EEVS Showcase on YouTube**](https://www.youtube.com/watch?v=FEmXwnn63EM) — Interactive AI Voice NPCs, Real-Time Signalsmith DSP Voice Effects, and In-Memory Clip Recording.

---

### Core Capabilities
- **Drop-In Replacement & Zero-Transcode Fast-Path Relay**: Fully compatible with `cfx-server`'s `voice_external_host` protocol. When no DSP processor or PCM tap is active for a speaker, raw Opus frames are forwarded directly across resolved routes without decoding.
- **3-Layer Routing Matrix**:
  1. Base graph synchronized from `cfx-server` (`VoiceStreamingSync` delta packets).
  2. Custom asymmetric route overrides (`ForceMode`, `MergeMode`, `Block`) for wiretaps, intercoms, and `Mode 2` custom streaming channels.
  3. Synthetic / non-player `NetId` routes for 3D spatial NPCs and automated dispatches.
- **Lazy Float Audio Pipeline**: On-demand Opus decoding, `PerSpeakerIngress` and `PerRouteBranch` (`2D` vs `3D`) DSP chains, `PreDsp` / `PostIngressDsp` audio taps, configurable `voice_inBitrate` (`16000..128000`, default `48000`), and multi-stream 10ms packet synthesis.
- **LRU In-Memory Clip Cache (`ClipManager`)**: Stores pre-encoded `.eevs` Opus clips and resampled `.wav` clips with configurable transient memory budgets and pinned clips.

---

## Use Cases

Because EEVS sits in the path of every player's session identity (`clientId` $\leftrightarrow$ `netId`), every routing edge (`2D` non-spatial vs. `3D` spatial), and every 10ms Opus audio frame, server developers can build gameplay mechanics that are not possible with standard client-side voice natives alone:

### Asymmetric Routing & Custom Acoustic Topologies
- **One-Way Wiretaps, Surveillance Bugs & Spectating**: Allow detectives, security cameras, or admins to silently monitor a player, vehicle, room, or entire radio channel in `2D` or `3D` without appearing in the channel's member list.
- **Public Address (PA), Intercoms & Megaphones**: Broadcast a speaker's voice one-way into specific prison cells, building interiors, or stadium zones.
- **Custom Acoustic Zones (`Mode 2` Streaming)**: Drive per-room portal occlusion, submarine/aircraft intercoms, or complex multi-party phone call graphs directly from server scripts.

### Server-Side Real-Time DSP & Per-Branch Audio Processing
- **Anti-Metagaming Voice Disguises**: Apply real-time pitch and formant shifting (`PerSpeakerIngress`) when a player wears a mask or uses a voice changer so their natural voice cannot be recognized on the receiving client.
- **Branch-Selective Signal Degradation**: Degrade a player's `2D` radio or phone call with band-pass filtering, static, and packet-loss dropouts when they enter an underground tunnel or jammer radius (`PerRouteBranch`), while keeping their `3D` proximity speech to passengers in the same vehicle completely clear.

### Synthetic Voice Injection & AI Agents
- **3D Talking NPCs & Interactive Characters**: Stream TTS, Voice Design, or pre-recorded `.eevs`/`.wav` audio directly from any networked ped's `NetId` with native 3D spatial attenuation and facial lip-sync.
- **Automated Radio Dispatch & Soundboards**: Inject radio squelch clicks, emergency tones, automated weather/ATC readouts, or procedural AI police dispatch callouts directly into active radio channels.

### Audio Taps, Moderation & Voice-Driven Mechanics
- **Isolated Voicemails & In-Game Recording**: Capture a player's microphone into an in-memory clip (`TapOnlyNoForward`) for phone voicemails, Dictaphones, or radio replay without nearby players hearing them while recording.
- **Rolling "Blackbox" Incident Replays & Live Moderation**: Maintain a rolling ring buffer of recent voice bursts per speaker to export multi-track incident recordings on `/report`, or pipe live audio into speech-to-text (ASR) for automated moderation and subtitles.
- **Acoustic AI Awareness & Voice Biometrics**: Trigger stealth/aggro mechanics based on real-time microphone loudness (RMS dB), pitch/scream detection, or whistle detection so nearby guards or creatures react to players speaking too loudly.
- **External Bridges (Discord / WebRTC Admin Radar)**: Bridge in-game radio channels bidirectionally with Discord voice channels or stream live spatial audio to a browser-based admin dashboard.

---

## Repository Structure

| Path | Description |
|---|---|
| [`src/`](./src) | Core voice engine, network transport, routing matrix, Opus codec pool, and Control API |
| [`include/`](./include/README.md) | Public C++ Plugin SDK headers (`IPlugin`, `IHostContext`, `ScopedPluginRegistrations`, capability interfaces) |
| [`plugins/`](./plugins/README.md) | Example & reference plugins (`voice_effects`, `voice_recorder`, `audio_cpp_client`) showcasing the C++ Plugin SDK |
| [`scripts/eevs/`](./scripts/eevs/README.md) | Core FiveM Server SDK resource (Lua/JS/C# exports, WebSocket bridge, `Eevs*` Voice Channel & Proximity wrappers) |
| [`demo/eevs_demo/`](./demo/eevs_demo/README.md) | In-game NUI sandbox (`/eevs`), CLI commands, real-time DSP controls, and interactive AI/TTS NPCs |
| [`tests/`](./tests) | Automated unit and integration test suite (`doctest` + CTest) |

---

## Building

### Prerequisites
- C++23 compiler (MSVC 2026+, Clang 20+, or GCC 16+)
- CMake 3.21+
- [vcpkg](https://github.com/microsoft/vcpkg) (automatically resolves dependencies from [`vcpkg.json`](./vcpkg.json))

### Configure & Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

### CMake Options
| Option | Default | Description |
|---|---|---|
| `EEVS_BUILD_PLUGINS` | `ON` | Build built-in shared-library plugins (`voice_effects`, `voice_recorder`, `audio_cpp_client`) |
| `EEVS_BUILD_BENCHMARK` | `ON` | Build the synthetic load & latency benchmark tool (`eevs_bench`) |
| `EEVS_BUILD_TESTS` | `ON` | Build the `doctest` unit and integration test suite (`eevs_tests`) |
| `EEVS_BUILD_DOCS` | `ON` | Configure the Doxygen `docs` target |
| `EEVS_ENABLE_AVX2` | `ON` | Enable AVX2 + FMA3 SIMD and fast-math optimizations |
| `EEVS_ENABLE_TRACY` | `OFF` | Enable on-demand [Tracy Profiler](https://github.com/wolfpld/tracy) instrumentation |

### Running Tests & Benchmarks
```bash
# Run unit and integration tests
ctest --test-dir build -C Release --output-on-failure

# Run synthetic load benchmark against a running eevs instance
./build/src/Release/eevs_bench -c eevs.cfg
```

---

## Quick Start

Following the [FiveM Enhanced External Voice Server](https://docs.fivem.net/docs/scripting-manual/voice/#external-voice-server) setup, the license key (`sv_licenseKey`) configured on both your game server (`cfx-server`) and `eevs` **must match**.

### 1. Game Server (`server.cfg` on `cfx-server`)

Configure `voice_external_connect` and the Control API connection settings in your `cfx-server`'s `server.cfg`, and start the `eevs` SDK resource:

```ini
sv_licenseKey "cfxk_your_license_key"

# Format: voice_external_connect <gateway_bind_ip:port> <voice_server_ip:port>
voice_external_connect gatewayBindIP:30122 voiceServerIP:30123

# EEVS Control API connection for the [eevs] resource
set eevs_control_host "voiceServerIP:30124"
set eevs_control_key "your_secret_control_key"

ensure eevs
```

| Parameter | Description |
|---|---|
| First (`gatewayBindIP:30122`) | Starts the `cfx-server` voice gateway on this interface. |
| Second (`voiceServerIP:30123`) | Public IP and port of the `eevs` external voice server that game clients connect to. |

### 2. EEVS Voice Server ([`eevs.cfg`](./eevs.cfg))

Configure [`eevs.cfg`](./eevs.cfg) next to the `eevs` binary:

```ini
sv_licenseKey "cfxk_your_license_key"

# Format: voice_external_host <voice_bind_ip:port> <game_server_gateway_ip:port>
voice_external_host voiceBindIP:30123 gameServerIP:30122

# Optional Control API settings
eevs_control_host controlBindIP:30124
eevs_control_key "your_secret_control_key"
```

| Parameter | Description |
|---|---|
| First (`voiceBindIP:30123`) | Bind address of the `eevs` external voice server. |
| Second (`gameServerIP:30122`) | Address of the gateway server specified in `voice_external_connect` on the game server. |

### 3. Start EEVS

Start `eevs` alongside `cfx-server` (automatically loads `eevs.cfg` when no file argument is passed):
```bash
./eevs
```

To set up the interactive in-game NUI showcase (`eevs_demo`, including TTS, ASR, voice effects and Interactive NPCs), see the [**Showcase & NUI Sandbox Guide (`demo/eevs_demo/README.md`)**](./demo/eevs_demo/README.md).

---

## Submodule Documentation

- [**C++ Plugin SDK (`include/README.md`)**](./include/README.md)
- [**Example Plugins & Control Endpoints (`plugins/README.md`)**](./plugins/README.md)
- [**FiveM Server SDK Resource (`scripts/eevs/README.md`)**](./scripts/eevs/README.md)
- [**Showcase & NUI Sandbox (`demo/eevs_demo/README.md`)**](./demo/eevs_demo/README.md)

---

## Third-Party Libraries

EEVS uses the following open-source libraries:

| Library | Usage | License |
|---|---|---|
| [**libopus**](https://opus-codec.org/) | Real-time audio encoding and decoding | BSD-3-Clause |
| [**uWebSockets**](https://github.com/uNetworking/uWebSockets) & [**uSockets**](https://github.com/uNetworking/uSockets) | HTTP/1.1 & WebSocket Control API server | Apache-2.0 |
| [**Glaze**](https://github.com/stephenberry/glaze) | Compile-time JSON reflection & Base64 serialization | MIT |
| [**Quill**](https://github.com/odygrd/quill) | Low-latency asynchronous logging | MIT |
| [**OpenSSL**](https://www.openssl.org/) | SHA-256 authentication & HTTP client transport | Apache-2.0 |
| [**LZ4**](https://github.com/lz4/lz4) | Gateway routing payload decompression | BSD-2-Clause |
| [**ENet**](https://github.com/zpl-c/enet) (`zpl-c/enet`) | Reliable/unreliable UDP peer transport | MIT |
| [**Signalsmith DSP**](https://github.com/Signalsmith-Audio/dsp) & [**Signalsmith Stretch**](https://github.com/Signalsmith-Audio/signalsmith-stretch) | Real-time filtering, FDN reverb, and pitch/formant shifting (`voice_effects`) | MIT |
| [**CLI11**](https://github.com/CLIUtils/CLI11), [**FTXUI**](https://github.com/ArthurSonzogni/FTXUI) & [**HdrHistogram_c**](https://github.com/HdrHistogram/HdrHistogram_c) | Benchmark CLI, terminal UI dashboard, and latency percentiles (`eevs_bench`) | BSD-3-Clause / MIT / CC0-1.0 |
| [**doctest**](https://github.com/doctest/doctest) | Automated unit and integration test suite (`eevs_tests`) | MIT |

---

## License

EEVS is distributed under a split licensing model (see [`LICENSE`](./LICENSE)):

- **Core Voice Server (`src/`) — [GPL-3.0](./src/LICENSE)** (with a **GPL-3.0 Section 7 Plugin Linking Exception**): Any modifications or redistributed forks of the core `eevs` server binary must disclose their source code under GPLv3. External plugins that interface exclusively through the [`include/eevs/`](./include/eevs) SDK headers may be distributed under any license.
- **C++ Plugin SDK, Example Plugins, FiveM Scripts & Demo (`include/`, `plugins/`, `scripts/`, `demo/`, `tests/`) — [MIT License](./LICENSE.MIT)**: Free to use, modify, and distribute in both open-source and commercial/proprietary plugins or FiveM resources.


