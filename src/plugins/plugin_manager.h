#pragma once

#include "core/containers.h"
#include "eevs/host_api.h"
#include "eevs/plugin.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace eevs {

class AudioPipeline;
class ClipManager;
class ControlApiServer;
class PacketSynthesizer;
class RoutingMatrix;
class SessionRegistry;

class PluginManager final
    : public IHostContext
    , public ISessionRoutingService
    , public IControlApiService
    , public IClipService
    , public IEventBusService
    , public IAudioInjector
    , private IAudioTap
    , private ISessionListener {
public:
    PluginManager(
        SessionRegistry& sessions,
        RoutingMatrix& routing,
        AudioPipeline& pipeline,
        PacketSynthesizer& synthesizer,
        ClipManager& clipManager,
        ControlApiServer& controlApi
    );
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Loads a single shared library (.dll on Windows, .so on Linux)
    bool LoadPluginFromFile(const std::filesystem::path& libraryPath);

    // Scans a directory and loads all shared library plugins found
    size_t LoadPluginsFromDirectory(const std::filesystem::path& directoryPath);

    // Registers an in-process static plugin instance
    bool LoadStaticPlugin(std::unique_ptr<IPlugin, PluginDeleter> plugin);

    // Unloads all loaded plugins in reverse initialization order
    void UnloadAll();

    // Invoked every 10ms by PacketSynthesizer to tick registered IAudioSource plugins
    void TickAudioSources();

    // Dispatches queued cross-plugin events on the main VoiceEngine poll thread
    void DispatchPendingEvents();

    [[nodiscard]] size_t GetLoadedPluginCount() const noexcept { return m_plugins.size(); }

    void SetConfigProperty(std::string_view key, std::string_view value);
    void SetConfigProperties(const FlatMap<std::string, std::string>& properties);

    // IHostContext
    [[nodiscard]] uint32_t GetApiVersion() const noexcept override { return kApiVersion; }
    void Log(LogLevel level, const char* pluginName, const char* message) override;
    [[nodiscard]] const char* GetConfigProperty(
        const char* key,
        const char* defaultValue = nullptr
    ) const noexcept override;

    void RegisterAudioTap(IAudioTap* tap) override;
    void UnregisterAudioTap(IAudioTap* tap) override;

    void RegisterDspProcessor(IDspProcessor* dsp, int priority = 0) override;
    void UnregisterDspProcessor(IDspProcessor* dsp) override;

    void RegisterRouteProvider(IRouteProvider* provider) override;
    void UnregisterRouteProvider(IRouteProvider* provider) override;

    void RegisterAudioSource(IAudioSource* source) override;
    void UnregisterAudioSource(IAudioSource* source) override;

    void RegisterSessionListener(ISessionListener* listener) override;
    void UnregisterSessionListener(ISessionListener* listener) override;

    void RegisterPacketFilter(IVoicePacketFilter* filter) override;
    void UnregisterPacketFilter(IVoicePacketFilter* filter) override;

    [[nodiscard]] ISessionRoutingService& GetSessions() noexcept override { return *this; }
    [[nodiscard]] const ISessionRoutingService& GetSessions() const noexcept override { return *this; }
    [[nodiscard]] IControlApiService& GetControlApi() noexcept override { return *this; }
    [[nodiscard]] IClipService& GetClips() noexcept override { return *this; }
    [[nodiscard]] const IClipService& GetClips() const noexcept override { return *this; }
    [[nodiscard]] IEventBusService& GetEvents() noexcept override { return *this; }
    [[nodiscard]] IAudioInjector& GetInjector() noexcept override { return *this; }

    // ISessionRoutingService implementation
    [[nodiscard]] bool ResolveClientIdByNetId(NetId netId, ClientId& outClientId) const noexcept override;
    [[nodiscard]] bool ResolveNetIdByClientId(ClientId clientId, NetId& outNetId) const noexcept override;
    [[nodiscard]] bool IsSpeakerTalking(ClientId clientId) const noexcept override;

    bool SetCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId, RoutingMode mode) override;
    bool ClearCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId) override;
    size_t ApplyBatchRoutes(const BatchRouteCommand* commands, size_t count) override;
    void NotifyRoutesChanged(ClientId speakerClientId) override;

    // IAudioInjector implementation
    size_t InjectOpusFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        const uint8_t* opusData,
        size_t opusLen,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) noexcept override;

    size_t InjectPcmFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        const float* pcmSamples,
        size_t sampleCount,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) noexcept override;

    // IControlApiService implementation
    bool RegisterEndpoint(
        const char* path,
        EndpointHandler handler,
        void* userData
    ) override;

    void UnregisterEndpoint(const char* path) override;

    bool RegisterAsyncEndpoint(
        const char* path,
        AsyncEndpointHandler handler,
        void* userData
    ) override;

    void CompleteAsyncRequest(
        AsyncRequestHandle requestHandle,
        uint16_t statusCode,
        const char* jsonResponse,
        size_t responseLen
    ) override;

    // Internal host helper used by ControlApiServer to marshal HTTP/WS requests onto the main poll thread
    using ControlRequestCompletionCallback = void(*)(
        void* userData,
        uint16_t statusCode,
        const char* jsonResponse,
        size_t responseLen
    ) noexcept;

    void EnqueueControlRequest(
        const char* method,
        const char* path,
        const char* body,
        size_t bodyLen,
        ControlRequestCompletionCallback onComplete = nullptr,
        void* userData = nullptr
    );

    // IClipService implementation
    bool StoreOpusClip(
        const char* requestedClipId,
        const uint8_t* const* framePtrs,
        const uint16_t* frameSizes,
        size_t frameCount,
        char* outClipIdBuf = nullptr,
        size_t outClipIdCap = 0,
        uint32_t* outDurationMs = nullptr
    ) override;

    bool StorePcmClip(
        const char* requestedClipId,
        const float* pcmSamples,
        size_t sampleCount,
        uint32_t sampleRate,
        char* outClipIdBuf = nullptr,
        size_t outClipIdCap = 0,
        uint32_t* outDurationMs = nullptr
    ) override;

    bool DeleteClip(const char* clipId) override;
    [[nodiscard]] bool HasClip(const char* clipId) const noexcept override;
    [[nodiscard]] uint32_t GetClipDurationMs(const char* clipId) const noexcept override;

    uint64_t PlayClip(
        const char* clipId,
        NetId senderNetId,
        RoutingMode mode,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) override;

    bool StopPlayback(uint64_t playbackId) override;
    size_t StopAllPlaybacksForSender(NetId senderNetId) override;

    // IEventBusService implementation
    void Subscribe(const char* topic, EventCallback callback, void* userData) override;
    void Unsubscribe(const char* topic, EventCallback callback, void* userData) override;
    void Publish(const char* topic, NetId subjectNetId, const char* jsonPayload) override;

private:
    struct LoadedPlugin {
        std::string name;
        std::string version;
        std::filesystem::path path;
        void* moduleHandle = nullptr;
        IPlugin* instance = nullptr;
        bool isDynamic = false;
    };

    struct EventSubscriber {
        PluginEventCallback callback = nullptr;
        void* userData = nullptr;
    };

    struct QueuedPluginEvent {
        std::string topic;
        NetId subjectNetId = 0;
        std::string jsonPayload;
    };

    struct QueuedControlRequest {
        std::string method;
        std::string path;
        std::string body;
        ControlRequestCompletionCallback onComplete = nullptr;
        void* userData = nullptr;
    };

    struct DeferredControlCallback {
        ControlRequestCompletionCallback onComplete = nullptr;
        void* userData = nullptr;
        std::chrono::steady_clock::time_point createdAt{};
    };

    // Internal event bus bridges for core VAD & session lifecycle events
    void OnTalkStateChanged(ClientId speakerClientId, NetId speakerNetId, bool isTalking) noexcept override;
    void OnClientRegistered(ClientId clientId, NetId netId) noexcept override;
    void OnClientAuthenticated(ClientId clientId, NetId netId) noexcept override;
    void OnClientDisconnected(ClientId clientId, NetId netId, bool wasTimeout) noexcept override;
    void OnClientUnregistered(ClientId clientId, NetId netId) noexcept override;

    SessionRegistry& m_sessions;
    RoutingMatrix& m_routing;
    AudioPipeline& m_pipeline;
    PacketSynthesizer& m_synthesizer;
    ClipManager& m_clipManager;
    ControlApiServer& m_controlApi;

    std::vector<LoadedPlugin> m_plugins;
    FlatMap<std::string, std::string> m_configProperties;
    std::vector<IAudioSource*> m_audioSources;
    FlatMap<std::string, std::vector<EventSubscriber>> m_eventSubscribers;

    std::mutex m_eventQueueMutex;
    std::vector<QueuedPluginEvent> m_pendingEvents;

    std::mutex m_controlQueueMutex;
    std::vector<QueuedControlRequest> m_pendingControlRequests;

    std::mutex m_deferredRequestsMutex;
    AsyncControlRequestHandle m_nextRequestHandle = 1;
    FlatMap<AsyncControlRequestHandle, DeferredControlCallback> m_deferredRequests;
};

} // namespace eevs
