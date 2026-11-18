# EEVS C++ Plugin SDK (`include/eevs`)

The headers in [`include/eevs/`](./eevs) define the public, ABI-safe interface for building native shared-library plugins (`.dll` on Windows, `.so` on Linux) for EEVS.

---

## Header Overview

| Header | Description |
|---|---|
| [`eevs/plugin.h`](./eevs/plugin.h) | `IPlugin` lifecycle interface, `PluginDeleter`, and `EEVS_DECLARE_PLUGIN(PluginClass)` export macro |
| [`eevs/host_api.h`](./eevs/host_api.h) | `IHostContext` and focused sub-services (`ISessionRoutingService`, `IControlApiService`, `IClipService`, `IEventBusService`) |
| [`eevs/plugin_scope.h`](./eevs/plugin_scope.h) | `ScopedPluginRegistrations` RAII helper for automatic reverse-LIFO unregistration on unload |
| [`eevs/types.h`](./eevs/types.h) | Core types (`ClientId`, `NetId`, `RoutingMode`, `OpusFrameView`, `PcmFrameView`, `PcmFrameBuffer`, `BatchRouteCommand`) |
| [`eevs/interfaces/i_dsp_processor.h`](./eevs/interfaces/i_dsp_processor.h) | `IDspProcessor` in-place 48 kHz float PCM processor (`PerSpeakerIngress` and `PerRouteBranch`) |
| [`eevs/interfaces/i_audio_tap.h`](./eevs/interfaces/i_audio_tap.h) | `IAudioTap` read-only observer for raw Opus frames, `PreDsp`/`PostIngressDsp` PCM frames, and VAD transitions |
| [`eevs/interfaces/i_voice_packet_filter.h`](./eevs/interfaces/i_voice_packet_filter.h) | `IVoicePacketFilter` pre-decode packet filter (`Allow`, `TapOnlyNoForward`, `DropSilent`) |
| [`eevs/interfaces/i_route_provider.h`](./eevs/interfaces/i_route_provider.h) | `IRouteProvider` dynamic per-speaker listener route modifier |
| [`eevs/interfaces/i_audio_source.h`](./eevs/interfaces/i_audio_source.h) | `IAudioSource` 10ms timer source and `IAudioInjector` outbound Opus/PCM frame injector |
| [`eevs/interfaces/i_session_listener.h`](./eevs/interfaces/i_session_listener.h) | `ISessionListener` observer for client registration, UDP authentication, and disconnection |

---

## Sub-Services on `IHostContext`

During `IPlugin::OnLoad(IHostContext* host)`, plugins access host functionality through five single-responsibility sub-interfaces:

- **`host->GetSessions()` (`ISessionRoutingService`)**:
  - `ResolveClientId(NetId) -> std::optional<ClientId>`
  - `ResolveNetId(ClientId) -> std::optional<NetId>`
  - `IsSpeakerTalking(ClientId) -> bool`
  - `SetCustomRouteByNetId(speakerNetId, listenerNetId, mode)`
  - `ClearCustomRouteByNetId(speakerNetId, listenerNetId)`
  - `ApplyBatchRoutes(std::span<const BatchRouteCommand>)`
- **`host->GetControlApi()` (`IControlApiService`)**:
  - `RegisterEndpoint<&MyPlugin::HandleSync>("/my/endpoint", this)`
  - `RegisterAsyncEndpoint<&MyPlugin::HandleAsync>("/my/async", this)`
  - `CompleteAsyncRequest(requestHandle, statusCode, jsonStringView)`
- **`host->GetClips()` (`IClipService`)**:
  - `StoreOpusClip(...)`, `StorePcmClip(clipId, std::span<const float>, sampleRate, ...)`
  - `PlayClip(clipId, senderNetId, mode, std::span<const ClientId> listeners)`
  - `StopPlayback(playbackId)`, `StopAllPlaybacksForSender(senderNetId)`
- **`host->GetEvents()` (`IEventBusService`)**:
  - `Subscribe<&MyPlugin::OnEvent>(topic, this)` / `Unsubscribe<&MyPlugin::OnEvent>(topic, this)`
  - `Publish(topic, subjectNetId, jsonPayload)`
- **`host->GetInjector()` (`IAudioInjector`)**:
  - `InjectOpusFrame(senderNetId, fallbackMode, std::span<const uint8_t> opusData, std::span<const ClientId> listeners)`
  - `InjectPcmFrame(senderNetId, fallbackMode, std::span<const float> pcmSamples, std::span<const ClientId> listeners)`

---

## Minimal Plugin Example

```cpp
#include "eevs/plugin.h"
#include "eevs/plugin_scope.h"

#include <algorithm>
#include <cstring>
#include <string_view>

class GainLimiterPlugin final
    : public eevs::IPlugin
    , public eevs::IDspProcessor {
public:
    const char* GetName() const noexcept override { return "gain_limiter"; }
    const char* GetVersion() const noexcept override { return "1.0.0"; }

    bool OnLoad(eevs::IHostContext* host) noexcept override {
        m_host = host;
        m_scope.Bind(host);

        m_scope.AddDspProcessor(this, eevs::DspPriority::kPostLimiter);
        m_scope.AddEndpoint<&GainLimiterPlugin::HandleStatus>("/limiter/status", this);
        return true;
    }

    void OnUnload() noexcept override {
        m_scope.Reset();
        m_host = nullptr;
    }

    // IDspProcessor
    eevs::DspStage GetStage() const noexcept override {
        return eevs::DspStage::PerSpeakerIngress;
    }

    bool ShouldProcess(eevs::ClientId, eevs::NetId, eevs::RoutingMode) const noexcept override {
        return m_enabled;
    }

    void ProcessPcm(const eevs::SpeakerContext&, eevs::RoutingMode, eevs::PcmFrameBuffer& pcm) noexcept override {
        for (float& sample : pcm.AsSpan()) {
            sample = std::clamp(sample, -0.85f, 0.85f);
        }
    }

private:
    bool HandleStatus(
        std::string_view /*method*/,
        std::string_view /*path*/,
        std::string_view /*body*/,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    ) {
        outStatusCode = 200;
        constexpr std::string_view kJson = R"({"ok":true,"enabled":true})";
        const size_t len = std::min(kJson.size(), responseCap - 1);
        std::memcpy(responseBuf, kJson.data(), len);
        responseBuf[len] = '\0';
        return true;
    }

    eevs::IHostContext* m_host = nullptr;
    eevs::ScopedPluginRegistrations m_scope;
    bool m_enabled = true;
};

EEVS_DECLARE_PLUGIN(GainLimiterPlugin)
```

---

## Threading & Real-Time Guarantees

- All `IDspProcessor`, `IAudioTap`, `IVoicePacketFilter`, `IRouteProvider`, and `IAudioSource` methods execute on the **real-time 10ms voice thread** and are marked `noexcept`. Avoid blocking I/O or heavy heap allocations inside these callbacks.
- Long-running tasks (such as disk I/O, audio transcoding, or external HTTP requests) should use `RegisterAsyncEndpoint` and offload work to a background thread, completing the request via `host->GetControlApi().CompleteAsyncRequest(handle, status, json)` when finished.
