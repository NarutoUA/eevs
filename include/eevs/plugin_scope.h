#pragma once

/**
 * @file plugin_scope.h
 * @brief RAII helper that tracks plugin capability & endpoint registrations and unregisters them in reverse LIFO order.
 */

#include "eevs/host_api.h"

#include <functional>
#include <string>
#include <vector>

namespace eevs {

class ScopedPluginRegistrations {
public:
    ScopedPluginRegistrations() = default;
    ~ScopedPluginRegistrations() noexcept {
        Reset();
    }

    ScopedPluginRegistrations(const ScopedPluginRegistrations&) = delete;
    ScopedPluginRegistrations& operator=(const ScopedPluginRegistrations&) = delete;

    void Bind(IHostContext* host) noexcept {
        Reset();
        m_host = host;
    }

    /** @brief Unregisters all capabilities and endpoints in reverse registration order. */
    void Reset() noexcept {
        for (auto it = m_cleanupActions.rbegin(); it != m_cleanupActions.rend(); ++it) {
            if (*it) {
                (*it)();
            }
        }
        m_cleanupActions.clear();
        m_host = nullptr;
    }

    void AddAudioTap(IAudioTap* tap) {
        if (!m_host || !tap) return;
        IHostContext* host = m_host;
        host->RegisterAudioTap(tap);
        m_cleanupActions.emplace_back([host, tap]() noexcept { host->UnregisterAudioTap(tap); });
    }

    void AddDspProcessor(IDspProcessor* dsp, int priority = 0) {
        if (!m_host || !dsp) return;
        IHostContext* host = m_host;
        host->RegisterDspProcessor(dsp, priority);
        m_cleanupActions.emplace_back([host, dsp]() noexcept { host->UnregisterDspProcessor(dsp); });
    }

    void AddRouteProvider(IRouteProvider* provider) {
        if (!m_host || !provider) return;
        IHostContext* host = m_host;
        host->RegisterRouteProvider(provider);
        m_cleanupActions.emplace_back([host, provider]() noexcept { host->UnregisterRouteProvider(provider); });
    }

    void AddAudioSource(IAudioSource* source) {
        if (!m_host || !source) return;
        IHostContext* host = m_host;
        host->RegisterAudioSource(source);
        m_cleanupActions.emplace_back([host, source]() noexcept { host->UnregisterAudioSource(source); });
    }

    void AddSessionListener(ISessionListener* listener) {
        if (!m_host || !listener) return;
        IHostContext* host = m_host;
        host->RegisterSessionListener(listener);
        m_cleanupActions.emplace_back([host, listener]() noexcept { host->UnregisterSessionListener(listener); });
    }

    void AddPacketFilter(IVoicePacketFilter* filter) {
        if (!m_host || !filter) return;
        IHostContext* host = m_host;
        host->RegisterPacketFilter(filter);
        m_cleanupActions.emplace_back([host, filter]() noexcept { host->UnregisterPacketFilter(filter); });
    }

    bool AddEndpoint(const char* path, IControlApiService::EndpointHandler handler, void* userData) {
        if (!m_host || !path || !handler) return false;
        IHostContext* host = m_host;
        if (!host->GetControlApi().RegisterEndpoint(path, handler, userData)) {
            return false;
        }
        std::string pathCopy(path);
        m_cleanupActions.emplace_back([host, p = std::move(pathCopy)]() noexcept {
            host->GetControlApi().UnregisterEndpoint(p.c_str());
        });
        return true;
    }

    template <auto MemberFn, typename Self>
    bool AddEndpoint(const char* path, Self* self) {
        if (!m_host || !path || !self) return false;
        IHostContext* host = m_host;
        if (!host->GetControlApi().RegisterEndpoint<MemberFn>(path, self)) {
            return false;
        }
        std::string pathCopy(path);
        m_cleanupActions.emplace_back([host, p = std::move(pathCopy)]() noexcept {
            host->GetControlApi().UnregisterEndpoint(p.c_str());
        });
        return true;
    }

    bool AddAsyncEndpoint(const char* path, IControlApiService::AsyncEndpointHandler handler, void* userData) {
        if (!m_host || !path || !handler) return false;
        IHostContext* host = m_host;
        if (!host->GetControlApi().RegisterAsyncEndpoint(path, handler, userData)) {
            return false;
        }
        std::string pathCopy(path);
        m_cleanupActions.emplace_back([host, p = std::move(pathCopy)]() noexcept {
            host->GetControlApi().UnregisterEndpoint(p.c_str());
        });
        return true;
    }

    template <auto MemberFn, typename Self>
    bool AddAsyncEndpoint(const char* path, Self* self) {
        if (!m_host || !path || !self) return false;
        IHostContext* host = m_host;
        if (!host->GetControlApi().RegisterAsyncEndpoint<MemberFn>(path, self)) {
            return false;
        }
        std::string pathCopy(path);
        m_cleanupActions.emplace_back([host, p = std::move(pathCopy)]() noexcept {
            host->GetControlApi().UnregisterEndpoint(p.c_str());
        });
        return true;
    }

private:
    IHostContext* m_host = nullptr;
    std::vector<std::function<void()>> m_cleanupActions;
};

} // namespace eevs
