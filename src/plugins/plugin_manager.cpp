#include "plugins/plugin_manager.h"
#include "audio/audio_pipeline.h"
#include "audio/clip_manager.h"
#include "audio/packet_synthesizer.h"
#include "core/logger.h"
#include "core/routing_matrix.h"
#include "core/session_registry.h"
#include "network/control_api_server.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace eevs {

namespace {
#if defined(_WIN32)
    constexpr std::string_view kSharedLibraryExtension = ".dll";

    void* LoadSharedLib(const std::filesystem::path& path) noexcept {
        return reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
    }

    void* GetSharedLibSymbol(void* handle, const char* symbolName) noexcept {
        if (!handle || !symbolName) {
            return nullptr;
        }
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), symbolName));
    }

    void UnloadSharedLib(void* handle) noexcept {
        if (handle) {
            FreeLibrary(reinterpret_cast<HMODULE>(handle));
        }
    }
#else
    constexpr std::string_view kSharedLibraryExtension = ".so";

    void* LoadSharedLib(const std::filesystem::path& path) noexcept {
        return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }

    void* GetSharedLibSymbol(void* handle, const char* symbolName) noexcept {
        if (!handle || !symbolName) {
            return nullptr;
        }
        return dlsym(handle, symbolName);
    }

    void UnloadSharedLib(void* handle) noexcept {
        if (handle) {
            dlclose(handle);
        }
    }
#endif
}

PluginManager::PluginManager(
    SessionRegistry& sessions,
    RoutingMatrix& routing,
    AudioPipeline& pipeline,
    PacketSynthesizer& synthesizer,
    ClipManager& clipManager,
    ControlApiServer& controlApi
)
    : m_sessions(sessions)
    , m_routing(routing)
    , m_pipeline(pipeline)
    , m_synthesizer(synthesizer)
    , m_clipManager(clipManager)
    , m_controlApi(controlApi) {
    m_pipeline.RegisterAudioTap(this);
    m_sessions.RegisterSessionListener(this);
}

PluginManager::~PluginManager() {
    UnloadAll();
    m_sessions.UnregisterSessionListener(this);
    m_pipeline.UnregisterAudioTap(this);
}

bool PluginManager::LoadPluginFromFile(const std::filesystem::path& libraryPath) {
    std::error_code ec;
    if (!std::filesystem::exists(libraryPath, ec) || !std::filesystem::is_regular_file(libraryPath, ec)) {
        EEVS_LOG_WARN("[PluginManager] Plugin file not found: {}", libraryPath.string());
        return false;
    }

    void* moduleHandle = LoadSharedLib(libraryPath);
    if (!moduleHandle) {
        EEVS_LOG_ERROR("[PluginManager] Failed to load shared library: {}", libraryPath.string());
        return false;
    }

    auto createFn = reinterpret_cast<CreatePluginFn>(GetSharedLibSymbol(moduleHandle, kCreatePluginSymbolName));
    if (!createFn) {
        EEVS_LOG_ERROR("[PluginManager] Missing entrypoint '{}' in {}",
                       kCreatePluginSymbolName, libraryPath.string());
        UnloadSharedLib(moduleHandle);
        return false;
    }

    IPlugin* plugin = createFn();
    if (!plugin) {
        EEVS_LOG_ERROR("[PluginManager] Entrypoint '{}' returned nullptr in {}",
                       kCreatePluginSymbolName, libraryPath.string());
        UnloadSharedLib(moduleHandle);
        return false;
    }

    const char* name = plugin->GetName() ? plugin->GetName() : "UnnamedPlugin";
    const char* version = plugin->GetVersion() ? plugin->GetVersion() : "0.0.0";

    if (!plugin->OnLoad(this)) {
        EEVS_LOG_ERROR("[PluginManager] Plugin '{}' ({}) OnLoad() failed.", name, version);
        plugin->Destroy();
        UnloadSharedLib(moduleHandle);
        return false;
    }

    m_plugins.push_back(LoadedPlugin{
        .name = name,
        .version = version,
        .path = libraryPath,
        .moduleHandle = moduleHandle,
        .instance = plugin,
        .isDynamic = true
    });

    EEVS_LOG_INFO("[PluginManager] Loaded plugin '{}' v{} ({})",
                  name, version, libraryPath.filename().string());
    return true;
}

size_t PluginManager::LoadPluginsFromDirectory(const std::filesystem::path& directoryPath) {
    std::error_code ec;
    if (!std::filesystem::exists(directoryPath, ec) || !std::filesystem::is_directory(directoryPath, ec)) {
        return 0;
    }

    size_t loadedCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension().string() == kSharedLibraryExtension) {
            if (LoadPluginFromFile(entry.path())) {
                ++loadedCount;
            }
        }
    }
    return loadedCount;
}

bool PluginManager::LoadStaticPlugin(std::unique_ptr<IPlugin, PluginDeleter> plugin) {
    if (!plugin) {
        return false;
    }

    const char* name = plugin->GetName() ? plugin->GetName() : "StaticPlugin";
    const char* version = plugin->GetVersion() ? plugin->GetVersion() : "0.0.0";

    if (!plugin->OnLoad(this)) {
        EEVS_LOG_ERROR("[PluginManager] Static plugin '{}' ({}) OnLoad() failed.", name, version);
        return false;
    }

    m_plugins.push_back(LoadedPlugin{
        .name = name,
        .version = version,
        .path = {},
        .moduleHandle = nullptr,
        .instance = plugin.release(),
        .isDynamic = false
    });

    EEVS_LOG_INFO("[PluginManager] Loaded static plugin '{}' v{}", name, version);
    return true;
}

void PluginManager::UnloadAll() {
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (it->instance) {
            it->instance->OnUnload();
            it->instance->Destroy();
            it->instance = nullptr;
        }
        if (it->isDynamic && it->moduleHandle) {
            UnloadSharedLib(it->moduleHandle);
            it->moduleHandle = nullptr;
        }
    }
    m_plugins.clear();
    m_audioSources.clear();
    m_eventSubscribers.clear();
    {
        std::scoped_lock lock(m_eventQueueMutex);
        m_pendingEvents.clear();
    }
    {
        std::scoped_lock lock(m_controlQueueMutex);
        m_pendingControlRequests.clear();
    }
    {
        std::vector<DeferredControlCallback> remainingDeferred;
        {
            std::scoped_lock lock(m_deferredRequestsMutex);
            remainingDeferred.reserve(m_deferredRequests.size());
            for (const auto& [_, cb] : m_deferredRequests) {
                remainingDeferred.push_back(cb);
            }
            m_deferredRequests.clear();
        }
        static constexpr std::string_view kShutdownJson = R"({"ok":false,"error":"Server shutting down"})";
        for (const DeferredControlCallback& cb : remainingDeferred) {
            if (cb.onComplete) {
                cb.onComplete(cb.userData, 503, kShutdownJson.data(), kShutdownJson.size());
            }
        }
    }
}

void PluginManager::TickAudioSources() {
    for (IAudioSource* source : m_audioSources) {
        source->OnTick10ms(*this);
    }
}

void PluginManager::DispatchPendingEvents() {
    const auto now = std::chrono::steady_clock::now();

    // 1. Drain and execute any async control requests queued from background threads (e.g. uWebSockets HTTP/WS thread)
    std::vector<QueuedControlRequest> localControlReqs;
    {
        std::scoped_lock lock(m_controlQueueMutex);
        if (!m_pendingControlRequests.empty()) {
            localControlReqs.swap(m_pendingControlRequests);
        }
    }

    for (const QueuedControlRequest& req : localControlReqs) {
        AsyncControlRequestHandle reqHandle = 0;
        if (req.onComplete) {
            std::scoped_lock lock(m_deferredRequestsMutex);
            reqHandle = m_nextRequestHandle++;
            m_deferredRequests[reqHandle] = DeferredControlCallback{
                .onComplete = req.onComplete,
                .userData = req.userData,
                .createdAt = now
            };
        }

        uint16_t statusCode = 200;
        std::string jsonResponse;
        const bool completedSynchronously = m_controlApi.DispatchRequest(
            req.method,
            req.path,
            req.body,
            statusCode,
            jsonResponse,
            reqHandle
        );

        if (completedSynchronously && reqHandle != 0) {
            CompleteAsyncRequest(reqHandle, statusCode, jsonResponse.c_str(), jsonResponse.size());
        }
    }

    // 2. Reap any orphaned deferred control requests older than 30 seconds so memory/connections never leak
    std::vector<DeferredControlCallback> expiredCallbacks;
    {
        std::scoped_lock lock(m_deferredRequestsMutex);
        if (!m_deferredRequests.empty()) {
            constexpr auto kDeferredTimeout = std::chrono::seconds(30);
            for (auto it = m_deferredRequests.begin(); it != m_deferredRequests.end();) {
                if ((now - it->second.createdAt) >= kDeferredTimeout) {
                    expiredCallbacks.push_back(it->second);
                    it = m_deferredRequests.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    if (!expiredCallbacks.empty()) {
        static constexpr std::string_view kTimeoutJson = R"({"ok":false,"error":"Async plugin request timed out"})";
        for (const DeferredControlCallback& cb : expiredCallbacks) {
            if (cb.onComplete) {
                cb.onComplete(cb.userData, 504, kTimeoutJson.data(), kTimeoutJson.size());
            }
        }
    }

    // 3. Drain and dispatch queued pub/sub events (both exact topic matches and wildcard "*")
    std::vector<QueuedPluginEvent> localEvents;
    {
        std::scoped_lock lock(m_eventQueueMutex);
        if (m_pendingEvents.empty()) {
            return;
        }
        localEvents.swap(m_pendingEvents);
    }

    for (const QueuedPluginEvent& evt : localEvents) {
        if (auto it = m_eventSubscribers.find(evt.topic); it != m_eventSubscribers.end()) {
            for (const EventSubscriber& sub : it->second) {
                if (sub.callback) {
                    sub.callback(
                        sub.userData,
                        evt.topic.c_str(),
                        evt.subjectNetId,
                        evt.jsonPayload.c_str()
                    );
                }
            }
        }

        if (evt.topic != "*") {
            if (auto wildcardIt = m_eventSubscribers.find("*"); wildcardIt != m_eventSubscribers.end()) {
                for (const EventSubscriber& sub : wildcardIt->second) {
                    if (sub.callback) {
                        sub.callback(
                            sub.userData,
                            evt.topic.c_str(),
                            evt.subjectNetId,
                            evt.jsonPayload.c_str()
                        );
                    }
                }
            }
        }
    }
}

void PluginManager::SetConfigProperty(std::string_view key, std::string_view value) {
    if (key.empty() || key == "sv_licenseKey") {
        return;
    }
    m_configProperties[std::string(key)] = std::string(value);
}

void PluginManager::SetConfigProperties(const FlatMap<std::string, std::string>& properties) {
    for (const auto& [k, v] : properties) {
        SetConfigProperty(k, v);
    }
}

const char* PluginManager::GetConfigProperty(const char* key, const char* defaultValue) const noexcept {
    if (!key || key[0] == '\0' || std::string_view(key) == "sv_licenseKey") {
        return defaultValue;
    }
    auto it = m_configProperties.find(std::string(key));
    return (it != m_configProperties.end()) ? it->second.c_str() : defaultValue;
}

void PluginManager::Log(LogLevel level, const char* pluginName, const char* message) {
    const char* tag = pluginName ? pluginName : "Unknown";
    const char* msg = message ? message : "";
    switch (level) {
        case LogLevel::Debug:
            EEVS_LOG_DEBUG("[Plugin:{}] {}", tag, msg);
            break;
        case LogLevel::Info:
            EEVS_LOG_INFO("[Plugin:{}] {}", tag, msg);
            break;
        case LogLevel::Warn:
            EEVS_LOG_WARN("[Plugin:{}] {}", tag, msg);
            break;
        case LogLevel::Error:
            EEVS_LOG_ERROR("[Plugin:{}] {}", tag, msg);
            break;
    }
}

void PluginManager::RegisterAudioTap(IAudioTap* tap) {
    m_pipeline.RegisterAudioTap(tap);
}

void PluginManager::UnregisterAudioTap(IAudioTap* tap) {
    m_pipeline.UnregisterAudioTap(tap);
}

void PluginManager::RegisterDspProcessor(IDspProcessor* dsp, int priority) {
    m_pipeline.RegisterDspProcessor(dsp, priority);
}

void PluginManager::UnregisterDspProcessor(IDspProcessor* dsp) {
    m_pipeline.UnregisterDspProcessor(dsp);
}

void PluginManager::RegisterRouteProvider(IRouteProvider* provider) {
    m_routing.RegisterRouteProvider(provider);
}

void PluginManager::UnregisterRouteProvider(IRouteProvider* provider) {
    m_routing.UnregisterRouteProvider(provider);
}

void PluginManager::RegisterAudioSource(IAudioSource* source) {
    if (!source) {
        return;
    }
    if (std::ranges::find(m_audioSources, source) == m_audioSources.end()) {
        m_audioSources.push_back(source);
    }
}

void PluginManager::UnregisterAudioSource(IAudioSource* source) {
    std::erase(m_audioSources, source);
}

void PluginManager::RegisterSessionListener(ISessionListener* listener) {
    m_sessions.RegisterSessionListener(listener);
}

void PluginManager::UnregisterSessionListener(ISessionListener* listener) {
    m_sessions.UnregisterSessionListener(listener);
}

void PluginManager::RegisterPacketFilter(IVoicePacketFilter* filter) {
    m_pipeline.RegisterPacketFilter(filter);
}

void PluginManager::UnregisterPacketFilter(IVoicePacketFilter* filter) {
    m_pipeline.UnregisterPacketFilter(filter);
}

bool PluginManager::ResolveClientIdByNetId(NetId netId, ClientId& outClientId) const noexcept {
    if (auto clientId = m_sessions.ResolveClientIdByNetId(netId)) {
        outClientId = *clientId;
        return true;
    }
    return false;
}

bool PluginManager::ResolveNetIdByClientId(ClientId clientId, NetId& outNetId) const noexcept {
    if (const VoicePlayerSession* session = m_sessions.FindByClientId(clientId)) {
        outNetId = session->netId;
        return true;
    }
    return false;
}

bool PluginManager::IsSpeakerTalking(ClientId clientId) const noexcept {
    return m_pipeline.IsSpeakerTalking(clientId);
}

bool PluginManager::SetCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId, RoutingMode mode) {
    auto listenerClientId = m_sessions.ResolveClientIdByNetId(listenerNetId);
    if (!listenerClientId) {
        return false;
    }
    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(speakerNetId)) {
        m_routing.SetCustomRoute(*speakerClientId, *listenerClientId, mode, RouteOverrideAction::ForceMode);
    } else {
        m_routing.SetSyntheticNetRoute(speakerNetId, *listenerClientId, mode);
    }
    return true;
}

bool PluginManager::ClearCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId) {
    auto listenerClientId = m_sessions.ResolveClientIdByNetId(listenerNetId);
    if (!listenerClientId) {
        return false;
    }
    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(speakerNetId)) {
        m_routing.ClearCustomRoute(*speakerClientId, *listenerClientId);
    }
    m_routing.ClearSyntheticNetRoute(speakerNetId, *listenerClientId);
    return true;
}

size_t PluginManager::ApplyBatchRoutes(const BatchRouteCommand* commands, size_t count) {
    if (!commands || count == 0) {
        return 0;
    }

    size_t applied = 0;
    for (size_t i = 0; i < count; ++i) {
        const BatchRouteCommand& cmd = commands[i];
        bool ok = false;
        switch (cmd.op) {
            case BatchRouteOp::Add:
                ok = SetCustomRouteByNetId(cmd.fromNetId, cmd.toNetId, cmd.mode);
                break;
            case BatchRouteOp::Merge: {
                if (auto listenerClientId = m_sessions.ResolveClientIdByNetId(cmd.toNetId)) {
                    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(cmd.fromNetId)) {
                        m_routing.SetCustomRoute(*speakerClientId, *listenerClientId, cmd.mode, RouteOverrideAction::MergeMode);
                    } else {
                        m_routing.SetSyntheticNetRoute(cmd.fromNetId, *listenerClientId, cmd.mode);
                    }
                    ok = true;
                }
                break;
            }
            case BatchRouteOp::Block: {
                if (auto listenerClientId = m_sessions.ResolveClientIdByNetId(cmd.toNetId)) {
                    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(cmd.fromNetId)) {
                        m_routing.BlockRoute(*speakerClientId, *listenerClientId);
                        ok = true;
                    }
                }
                break;
            }
            case BatchRouteOp::Remove:
                ok = ClearCustomRouteByNetId(cmd.fromNetId, cmd.toNetId);
                break;
            case BatchRouteOp::ClearSpeaker:
                if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(cmd.fromNetId)) {
                    m_routing.ClearAllCustomRoutesForSpeaker(*speakerClientId);
                } else {
                    m_routing.ClearAllSyntheticNetRoutes(cmd.fromNetId);
                }
                ok = true;
                break;
        }
        if (ok) {
            ++applied;
        }
    }
    return applied;
}

void PluginManager::NotifyRoutesChanged(ClientId speakerClientId) {
    m_routing.RefreshSpeakerRoutes(speakerClientId);
}

size_t PluginManager::InjectOpusFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    const uint8_t* opusData,
    size_t opusLen,
    const ClientId* explicitListeners,
    size_t listenerCount
) noexcept {
    if (!opusData || opusLen == 0) {
        return 0;
    }

    std::span<const ClientId> listenersSpan = (explicitListeners && listenerCount > 0)
        ? std::span<const ClientId>(explicitListeners, listenerCount)
        : std::span<const ClientId>{};

    return m_synthesizer.InjectOpusFrame(
        senderNetId,
        fallbackMode,
        std::span<const uint8_t>(opusData, opusLen),
        listenersSpan
    );
}

size_t PluginManager::InjectPcmFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    const float* pcmSamples,
    size_t sampleCount,
    const ClientId* explicitListeners,
    size_t listenerCount
) noexcept {
    if (!pcmSamples || sampleCount == 0) {
        return 0;
    }

    std::span<const ClientId> listenersSpan = (explicitListeners && listenerCount > 0)
        ? std::span<const ClientId>(explicitListeners, listenerCount)
        : std::span<const ClientId>{};

    return m_synthesizer.InjectPcmFrame(
        senderNetId,
        fallbackMode,
        std::span<const float>(pcmSamples, sampleCount),
        listenersSpan
    );
}

bool PluginManager::RegisterEndpoint(
    const char* path,
    EndpointHandler handler,
    void* userData
) {
    if (!path || !handler) {
        return false;
    }
    return m_controlApi.RegisterEndpoint(path, handler, userData);
}

void PluginManager::UnregisterEndpoint(const char* path) {
    if (path) {
        m_controlApi.UnregisterEndpoint(path);
    }
}

bool PluginManager::RegisterAsyncEndpoint(
    const char* path,
    AsyncEndpointHandler handler,
    void* userData
) {
    if (!path || !handler) {
        return false;
    }
    return m_controlApi.RegisterAsyncEndpoint(path, handler, userData);
}

void PluginManager::CompleteAsyncRequest(
    AsyncRequestHandle requestHandle,
    uint16_t statusCode,
    const char* jsonResponse,
    size_t responseLen
) {
    if (requestHandle == 0) {
        return;
    }

    DeferredControlCallback cb{};
    {
        std::scoped_lock lock(m_deferredRequestsMutex);
        auto it = m_deferredRequests.find(requestHandle);
        if (it == m_deferredRequests.end()) {
            return; // Already completed
        }
        cb = it->second;
        m_deferredRequests.erase(it);
    }

    if (cb.onComplete) {
        cb.onComplete(cb.userData, statusCode, jsonResponse ? jsonResponse : "{}", responseLen);
    }
}

void PluginManager::EnqueueControlRequest(
    const char* method,
    const char* path,
    const char* body,
    size_t bodyLen,
    ControlRequestCompletionCallback onComplete,
    void* userData
) {
    if (!path || path[0] == '\0') {
        return;
    }

    std::scoped_lock lock(m_controlQueueMutex);
    m_pendingControlRequests.push_back(QueuedControlRequest{
        .method = method ? method : "POST",
        .path = path,
        .body = (body && bodyLen > 0) ? std::string(body, bodyLen) : std::string{},
        .onComplete = onComplete,
        .userData = userData
    });
}

bool PluginManager::StoreOpusClip(
    const char* requestedClipId,
    const uint8_t* const* framePtrs,
    const uint16_t* frameSizes,
    size_t frameCount,
    char* outClipIdBuf,
    size_t outClipIdCap,
    uint32_t* outDurationMs
) {
    if (!framePtrs || !frameSizes || frameCount == 0) {
        return false;
    }

    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        if (framePtrs[i] && frameSizes[i] > 0) {
            frames.emplace_back(framePtrs[i], framePtrs[i] + frameSizes[i]);
        }
    }
    if (frames.empty()) {
        return false;
    }

    const uint32_t durMs = static_cast<uint32_t>(frames.size() * 10);
    std::string_view reqId = requestedClipId ? std::string_view(requestedClipId) : std::string_view{};
    std::string finalId = m_clipManager.StoreOpusClip(reqId, std::move(frames));
    if (finalId.empty()) {
        return false;
    }

    if (outDurationMs) {
        *outDurationMs = durMs;
    }
    if (outClipIdBuf && outClipIdCap > 0) {
        const size_t copyLen = std::min(finalId.size(), outClipIdCap - 1);
        std::memcpy(outClipIdBuf, finalId.data(), copyLen);
        outClipIdBuf[copyLen] = '\0';
    }
    return true;
}

bool PluginManager::StorePcmClip(
    const char* requestedClipId,
    const float* pcmSamples,
    size_t sampleCount,
    uint32_t sampleRate,
    char* outClipIdBuf,
    size_t outClipIdCap,
    uint32_t* outDurationMs
) {
    if (!pcmSamples || sampleCount == 0 || sampleRate == 0) {
        return false;
    }

    std::string_view reqId = requestedClipId ? std::string_view(requestedClipId) : std::string_view{};
    std::string finalId = m_clipManager.StorePcmClip(
        reqId,
        std::span<const float>(pcmSamples, sampleCount),
        sampleRate,
        outDurationMs
    );

    if (finalId.empty()) {
        return false;
    }

    if (outClipIdBuf && outClipIdCap > 0) {
        const size_t copyLen = std::min(finalId.size(), outClipIdCap - 1);
        std::memcpy(outClipIdBuf, finalId.data(), copyLen);
        outClipIdBuf[copyLen] = '\0';
    }
    return true;
}

bool PluginManager::DeleteClip(const char* clipId) {
    if (!clipId) {
        return false;
    }
    return m_clipManager.DeleteClip(clipId);
}

bool PluginManager::HasClip(const char* clipId) const noexcept {
    if (!clipId) {
        return false;
    }
    return m_clipManager.HasClip(clipId);
}

uint32_t PluginManager::GetClipDurationMs(const char* clipId) const noexcept {
    if (!clipId) {
        return 0;
    }
    return m_clipManager.GetClipDurationMs(clipId);
}

uint64_t PluginManager::PlayClip(
    const char* clipId,
    NetId senderNetId,
    RoutingMode mode,
    const ClientId* explicitListeners,
    size_t listenerCount
) {
    if (!clipId) {
        return 0;
    }

    const AudioClip* clip = m_clipManager.FindClip(clipId);
    if (!clip) {
        return 0;
    }

    std::vector<ClientId> listeners;
    if (explicitListeners && listenerCount > 0) {
        listeners.assign(explicitListeners, explicitListeners + listenerCount);
    }

    return m_synthesizer.PlayClip(*clip, senderNetId, mode, std::move(listeners));
}

bool PluginManager::StopPlayback(uint64_t playbackId) {
    return m_synthesizer.StopPlayback(playbackId);
}

size_t PluginManager::StopAllPlaybacksForSender(NetId senderNetId) {
    return m_synthesizer.StopAllPlaybacksForSender(senderNetId);
}

void PluginManager::Subscribe(const char* topic, EventCallback callback, void* userData) {
    if (!topic || !callback || topic[0] == '\0') {
        return;
    }

    auto& subs = m_eventSubscribers[std::string(topic)];
    for (const EventSubscriber& sub : subs) {
        if (sub.callback == callback && sub.userData == userData) {
            return;
        }
    }
    subs.push_back(EventSubscriber{callback, userData});
}

void PluginManager::Unsubscribe(const char* topic, EventCallback callback, void* userData) {
    if (!topic) {
        return;
    }
    auto it = m_eventSubscribers.find(std::string(topic));
    if (it == m_eventSubscribers.end()) {
        return;
    }
    std::erase_if(it->second, [callback, userData](const EventSubscriber& sub) {
        return sub.callback == callback && sub.userData == userData;
    });
}

void PluginManager::Publish(const char* topic, NetId subjectNetId, const char* jsonPayload) {
    if (!topic || topic[0] == '\0') {
        return;
    }

    std::scoped_lock lock(m_eventQueueMutex);
    m_pendingEvents.push_back(QueuedPluginEvent{
        .topic = topic,
        .subjectNetId = subjectNetId,
        .jsonPayload = jsonPayload ? jsonPayload : "{}"
    });
}

void PluginManager::OnTalkStateChanged(ClientId speakerClientId, NetId speakerNetId, bool isTalking) noexcept {
    if (m_eventSubscribers.empty()) {
        return;
    }
    std::string payload = std::format(
        R"({{"clientId":{},"netId":{},"talking":{}}})",
        speakerClientId,
        speakerNetId,
        isTalking ? "true" : "false"
    );
    Publish("voice.talk_state", speakerNetId, payload.c_str());
}

void PluginManager::OnClientRegistered(ClientId clientId, NetId netId) noexcept {
    if (m_eventSubscribers.empty()) {
        return;
    }
    std::string payload = std::format(R"({{"clientId":{},"netId":{}}})", clientId, netId);
    Publish("session.registered", netId, payload.c_str());
}

void PluginManager::OnClientAuthenticated(ClientId clientId, NetId netId) noexcept {
    if (m_eventSubscribers.empty()) {
        return;
    }
    std::string payload = std::format(R"({{"clientId":{},"netId":{}}})", clientId, netId);
    Publish("session.authenticated", netId, payload.c_str());
}

void PluginManager::OnClientDisconnected(ClientId clientId, NetId netId, bool wasTimeout) noexcept {
    if (m_eventSubscribers.empty()) {
        return;
    }
    std::string payload = std::format(
        R"({{"clientId":{},"netId":{},"timeout":{}}})",
        clientId,
        netId,
        wasTimeout ? "true" : "false"
    );
    Publish("session.disconnected", netId, payload.c_str());
}

void PluginManager::OnClientUnregistered(ClientId clientId, NetId netId) noexcept {
    if (m_eventSubscribers.empty()) {
        return;
    }
    std::string payload = std::format(R"({{"clientId":{},"netId":{}}})", clientId, netId);
    Publish("session.unregistered", netId, payload.c_str());
}

} // namespace eevs
