#pragma once

#include "core/containers.h"
#include "eevs/host_api.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace eevs {

class VoiceEngine;

class ControlApiServer {
public:
    explicit ControlApiServer(VoiceEngine& engine);
    ~ControlApiServer();

    ControlApiServer(const ControlApiServer&) = delete;
    ControlApiServer& operator=(const ControlApiServer&) = delete;

    /**
     * @brief Starts the uWebSockets HTTP/1.1 + WebSocket (`/ws`) control server on a dedicated background event loop.
     */
    bool Start(std::string_view bindHost = "127.0.0.1", uint16_t bindPort = 30124);
    void Stop();
    void Poll();

    /**
     * @brief Configures shared secret authentication for HTTP and WebSocket control clients.
     */
    void ConfigureAuth(std::string_view licenseKey, std::string_view explicitControlKey = {});
    [[nodiscard]] bool VerifyToken(std::string_view candidateToken) const noexcept;

    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t GetBindPort() const noexcept { return m_bindPort; }

    bool RegisterEndpoint(
        std::string_view path,
        IHostContext::ControlEndpointHandler handler,
        void* userData
    );
    bool RegisterAsyncEndpoint(
        std::string_view path,
        IHostContext::AsyncControlEndpointHandler handler,
        void* userData
    );
    void UnregisterEndpoint(std::string_view path);

    /**
     * @brief Broadcasts an event to all authenticated WebSocket (`/ws`) subscribers.
     */
    void BroadcastWebSocketEvent(std::string_view topic, NetId subjectNetId, std::string_view jsonPayload);

    /**
     * @brief Transport-agnostic command dispatcher (executes on main VoiceEngine poll thread).
     *
     * Returns `true` if the request was completed synchronously (`outStatus` and `outJsonBody` populated),
     * or `false` if an async endpoint took ownership of `requestHandle` and will complete it later via
     * `IHostContext::CompleteAsyncControlRequest(requestHandle, ...)`.
     */
    bool DispatchRequest(
        std::string_view method,
        std::string_view path,
        std::string_view body,
        uint16_t& outStatus,
        std::string& outJsonBody,
        IHostContext::AsyncControlRequestHandle requestHandle = 0
    );

private:
    struct CustomEndpoint {
        IHostContext::ControlEndpointHandler syncHandler = nullptr;
        IHostContext::AsyncControlEndpointHandler asyncHandler = nullptr;
        void* userData = nullptr;
    };

    struct UwsContext;

    static void EEVS_CALL OnWildcardHostEvent(
        void* userData,
        const char* topic,
        NetId subjectNetId,
        const char* jsonPayload
    );

    void RunEventLoopThread(std::string bindHost, uint16_t bindPort);

    VoiceEngine& m_engine;
    std::unique_ptr<UwsContext> m_uws;
    std::jthread m_ioThread;
    uint16_t m_bindPort = 0;
    std::atomic<bool> m_running{false};
    bool m_requireAuth = false;
    bool m_subscribedToHostEvents = false;
    std::string m_primarySecret;
    std::string m_primarySecretSha256Hex;
    FlatMap<std::string, CustomEndpoint> m_customEndpoints;
};

} // namespace eevs
