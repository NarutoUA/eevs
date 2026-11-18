#pragma once

#include "eevs/api_version.h"
#include "eevs/interfaces/i_audio_source.h"
#include "eevs/interfaces/i_audio_tap.h"
#include "eevs/interfaces/i_dsp_processor.h"
#include "eevs/interfaces/i_route_provider.h"
#include "eevs/interfaces/i_session_listener.h"
#include "eevs/interfaces/i_voice_packet_filter.h"
#include "eevs/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace eevs {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

/**
 * @brief Session identity resolution, VAD queries, and custom routing mutations.
 */
class ISessionRoutingService {
public:
    [[nodiscard]] virtual bool ResolveClientIdByNetId(NetId netId, ClientId& outClientId) const noexcept = 0;
    [[nodiscard]] virtual bool ResolveNetIdByClientId(ClientId clientId, NetId& outNetId) const noexcept = 0;
    [[nodiscard]] virtual bool IsSpeakerTalking(ClientId clientId) const noexcept = 0;

    virtual bool SetCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId, RoutingMode mode) = 0;
    virtual bool ClearCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId) = 0;
    virtual size_t ApplyBatchRoutes(const BatchRouteCommand* commands, size_t count) = 0;
    virtual void NotifyRoutesChanged(ClientId speakerClientId) = 0;

    // Non-virtual convenience helpers (zero vtable/ABI impact)
    [[nodiscard]] std::optional<ClientId> ResolveClientId(NetId netId) const noexcept {
        ClientId id = 0;
        return ResolveClientIdByNetId(netId, id) ? std::optional<ClientId>(id) : std::nullopt;
    }

    [[nodiscard]] std::optional<NetId> ResolveNetId(ClientId clientId) const noexcept {
        NetId id = 0;
        return ResolveNetIdByClientId(clientId, id) ? std::optional<NetId>(id) : std::nullopt;
    }

    size_t ApplyBatchRoutes(std::span<const BatchRouteCommand> commands) {
        return ApplyBatchRoutes(commands.data(), commands.size());
    }

protected:
    ~ISessionRoutingService() = default;
};

/**
 * @brief HTTP/1.1 & WebSocket (`/ws`) control endpoint registration and async completion service.
 */
class IControlApiService {
public:
    using AsyncRequestHandle = uint64_t;

    using EndpointHandler = bool(EEVS_CALL*)(
        void* userData,
        const char* method,
        const char* path,
        const char* body,
        size_t bodyLen,
        char* responseBuf,
        size_t responseCap,
        uint16_t& outStatusCode
    );

    /**
     * @brief Handler for asynchronous control endpoints (`RegisterAsyncEndpoint`).
     *
     * May complete the request immediately or from a worker thread via `CompleteAsyncRequest`.
     */
    using AsyncEndpointHandler = bool(EEVS_CALL*)(
        void* userData,
        AsyncRequestHandle requestHandle,
        const char* method,
        const char* path,
        const char* body,
        size_t bodyLen
    );

    virtual bool RegisterEndpoint(
        const char* path,
        EndpointHandler handler,
        void* userData
    ) = 0;

    virtual void UnregisterEndpoint(const char* path) = 0;

    virtual bool RegisterAsyncEndpoint(
        const char* path,
        AsyncEndpointHandler handler,
        void* userData
    ) = 0;

    /** @brief Thread-safe completion for requests received via `RegisterAsyncEndpoint`. */
    virtual void CompleteAsyncRequest(
        AsyncRequestHandle requestHandle,
        uint16_t statusCode,
        const char* jsonResponse,
        size_t responseLen
    ) = 0;

    // Non-virtual convenience helpers (zero vtable/ABI impact)
    void CompleteAsyncRequest(
        AsyncRequestHandle requestHandle,
        uint16_t statusCode,
        std::string_view jsonResponse
    ) {
        CompleteAsyncRequest(requestHandle, statusCode, jsonResponse.data(), jsonResponse.size());
    }

    /**
     * @brief Registers a member function `bool (Self::*)(std::string_view method, std::string_view path, std::string_view body, char* responseBuf, size_t responseCap, uint16_t& outStatusCode)` as a synchronous control endpoint.
     */
    template <auto MemberFn, typename Self>
    bool RegisterEndpoint(const char* path, Self* self) {
        struct Trampoline {
            static bool EEVS_CALL Invoke(
                void* userData,
                const char* method,
                const char* reqPath,
                const char* body,
                size_t bodyLen,
                char* responseBuf,
                size_t responseCap,
                uint16_t& outStatusCode
            ) {
                auto* instance = static_cast<Self*>(userData);
                return (instance->*MemberFn)(
                    std::string_view(method ? method : ""),
                    std::string_view(reqPath ? reqPath : ""),
                    std::string_view(body ? body : "", bodyLen),
                    responseBuf,
                    responseCap,
                    outStatusCode
                );
            }
        };
        return RegisterEndpoint(path, &Trampoline::Invoke, self);
    }

    /**
     * @brief Registers a member function `bool (Self::*)(AsyncRequestHandle requestHandle, std::string_view method, std::string_view path, std::string_view body)` as an asynchronous control endpoint.
     */
    template <auto MemberFn, typename Self>
    bool RegisterAsyncEndpoint(const char* path, Self* self) {
        struct Trampoline {
            static bool EEVS_CALL Invoke(
                void* userData,
                AsyncRequestHandle requestHandle,
                const char* method,
                const char* reqPath,
                const char* body,
                size_t bodyLen
            ) {
                auto* instance = static_cast<Self*>(userData);
                return (instance->*MemberFn)(
                    requestHandle,
                    std::string_view(method ? method : ""),
                    std::string_view(reqPath ? reqPath : ""),
                    std::string_view(body ? body : "", bodyLen)
                );
            }
        };
        return RegisterAsyncEndpoint(path, &Trampoline::Invoke, self);
    }

protected:
    ~IControlApiService() = default;
};

/**
 * @brief In-memory Opus clip cache (`ClipManager`) & paced 10ms playback service.
 */
class IClipService {
public:
    /** @brief Stores pre-encoded 10ms Opus frames as an in-memory clip without transcoding. */
    virtual bool StoreOpusClip(
        const char* requestedClipId,
        const uint8_t* const* framePtrs,
        const uint16_t* frameSizes,
        size_t frameCount,
        char* outClipIdBuf = nullptr,
        size_t outClipIdCap = 0,
        uint32_t* outDurationMs = nullptr
    ) = 0;

    /** @brief Resamples (if needed) and encodes mono float PCM samples into a 10ms Opus clip. */
    virtual bool StorePcmClip(
        const char* requestedClipId,
        const float* pcmSamples,
        size_t sampleCount,
        uint32_t sampleRate,
        char* outClipIdBuf = nullptr,
        size_t outClipIdCap = 0,
        uint32_t* outDurationMs = nullptr
    ) = 0;

    virtual bool DeleteClip(const char* clipId) = 0;
    [[nodiscard]] virtual bool HasClip(const char* clipId) const noexcept = 0;
    [[nodiscard]] virtual uint32_t GetClipDurationMs(const char* clipId) const noexcept = 0;

    /** @brief Schedules 10ms paced clip playback; returns a non-zero `playbackId` on success. */
    virtual uint64_t PlayClip(
        const char* clipId,
        NetId senderNetId,
        RoutingMode mode,
        const ClientId* explicitListeners = nullptr,
        size_t listenerCount = 0
    ) = 0;

    virtual bool StopPlayback(uint64_t playbackId) = 0;
    virtual size_t StopAllPlaybacksForSender(NetId senderNetId) = 0;

    // Non-virtual convenience helpers (zero vtable/ABI impact)
    bool StorePcmClip(
        const char* requestedClipId,
        std::span<const float> pcmSamples,
        uint32_t sampleRate,
        char* outClipIdBuf = nullptr,
        size_t outClipIdCap = 0,
        uint32_t* outDurationMs = nullptr
    ) {
        return StorePcmClip(
            requestedClipId,
            pcmSamples.data(),
            pcmSamples.size(),
            sampleRate,
            outClipIdBuf,
            outClipIdCap,
            outDurationMs
        );
    }

    uint64_t PlayClip(
        const char* clipId,
        NetId senderNetId,
        RoutingMode mode,
        std::span<const ClientId> explicitListeners
    ) {
        return PlayClip(
            clipId,
            senderNetId,
            mode,
            explicitListeners.empty() ? nullptr : explicitListeners.data(),
            explicitListeners.size()
        );
    }

protected:
    ~IClipService() = default;
};

/**
 * @brief Inter-plugin and WebSocket (`/ws`) pub/sub event bus service.
 */
class IEventBusService {
public:
    using EventCallback = void(EEVS_CALL*)(
        void* userData,
        const char* topic,
        NetId subjectNetId,
        const char* jsonPayload
    );

    /** @brief Subscribes to `topic` (or `"*"` for all topics). */
    virtual void Subscribe(const char* topic, EventCallback callback, void* userData) = 0;
    virtual void Unsubscribe(const char* topic, EventCallback callback, void* userData) = 0;

    /** @brief Thread-safe; queued events are dispatched on the main poll thread. */
    virtual void Publish(const char* topic, NetId subjectNetId, const char* jsonPayload) = 0;

    /**
     * @brief Subscribes a member function `void (Self::*)(std::string_view topic, NetId subjectNetId, std::string_view jsonPayload)`.
     */
    template <auto MemberFn, typename Self>
    void Subscribe(const char* topic, Self* self) {
        struct Trampoline {
            static void EEVS_CALL Invoke(
                void* userData,
                const char* evTopic,
                NetId subjectNetId,
                const char* jsonPayload
            ) {
                auto* instance = static_cast<Self*>(userData);
                (instance->*MemberFn)(
                    std::string_view(evTopic ? evTopic : ""),
                    subjectNetId,
                    std::string_view(jsonPayload ? jsonPayload : "{}")
                );
            }
        };
        Subscribe(topic, &Trampoline::Invoke, self);
    }

    template <auto MemberFn, typename Self>
    void Unsubscribe(const char* topic, Self* self) {
        struct Trampoline {
            static void EEVS_CALL Invoke(
                void* userData,
                const char* evTopic,
                NetId subjectNetId,
                const char* jsonPayload
            ) {
                auto* instance = static_cast<Self*>(userData);
                (instance->*MemberFn)(
                    std::string_view(evTopic ? evTopic : ""),
                    subjectNetId,
                    std::string_view(jsonPayload ? jsonPayload : "{}")
                );
            }
        };
        Unsubscribe(topic, &Trampoline::Invoke, self);
    }

protected:
    ~IEventBusService() = default;
};

/**
 * @brief Host API provided to plugins during `IPlugin::OnLoad`.
 *
 * Manages core logging, configuration, capability registration, and exposes focused
 * sub-service interfaces (`GetSessions()`, `GetControlApi()`, `GetClips()`, `GetEvents()`, `GetInjector()`).
 */
class IHostContext {
public:
    using AsyncControlRequestHandle = IControlApiService::AsyncRequestHandle;
    using ControlEndpointHandler = IControlApiService::EndpointHandler;
    using AsyncControlEndpointHandler = IControlApiService::AsyncEndpointHandler;
    using PluginEventCallback = IEventBusService::EventCallback;

    [[nodiscard]] virtual uint32_t GetApiVersion() const noexcept = 0;

    virtual void Log(LogLevel level, const char* pluginName, const char* message) = 0;

    /**
     * @brief Reads a configuration property from the host (excluding `sv_licenseKey`).
     * @return Null-terminated property value if found, or `defaultValue` if not set.
     */
    [[nodiscard]] virtual const char* GetConfigProperty(
        const char* key,
        const char* defaultValue = nullptr
    ) const noexcept = 0;

    // Capability Registration
    virtual void RegisterAudioTap(IAudioTap* tap) = 0;
    virtual void UnregisterAudioTap(IAudioTap* tap) = 0;

    virtual void RegisterDspProcessor(IDspProcessor* dsp, int priority = 0) = 0;
    virtual void UnregisterDspProcessor(IDspProcessor* dsp) = 0;

    virtual void RegisterRouteProvider(IRouteProvider* provider) = 0;
    virtual void UnregisterRouteProvider(IRouteProvider* provider) = 0;

    virtual void RegisterAudioSource(IAudioSource* source) = 0;
    virtual void UnregisterAudioSource(IAudioSource* source) = 0;

    virtual void RegisterSessionListener(ISessionListener* listener) = 0;
    virtual void UnregisterSessionListener(ISessionListener* listener) = 0;

    virtual void RegisterPacketFilter(IVoicePacketFilter* filter) = 0;
    virtual void UnregisterPacketFilter(IVoicePacketFilter* filter) = 0;

    // Focused Sub-Services
    [[nodiscard]] virtual ISessionRoutingService& GetSessions() noexcept = 0;
    [[nodiscard]] virtual const ISessionRoutingService& GetSessions() const noexcept = 0;

    [[nodiscard]] virtual IControlApiService& GetControlApi() noexcept = 0;

    [[nodiscard]] virtual IClipService& GetClips() noexcept = 0;
    [[nodiscard]] virtual const IClipService& GetClips() const noexcept = 0;

    [[nodiscard]] virtual IEventBusService& GetEvents() noexcept = 0;

    [[nodiscard]] virtual IAudioInjector& GetInjector() noexcept = 0;

protected:
    ~IHostContext() = default;
};

} // namespace eevs
