#pragma once

#include "eevs/api_version.h"
#include "eevs/host_api.h"

namespace eevs {

/**
 * @brief Lifecycle interface implemented by EEVS plugins.
 */
class IPlugin {
public:
    [[nodiscard]] virtual const char* GetName() const noexcept = 0;
    [[nodiscard]] virtual const char* GetVersion() const noexcept = 0;

    /**
     * @param host Host context pointer; valid until `OnUnload()` returns.
     */
    virtual bool OnLoad(IHostContext* host) noexcept = 0;

    /**
     * @brief Called before the plugin is unloaded; stop worker threads and unregister capabilities here.
     */
    virtual void OnUnload() noexcept = 0;

    /** @brief Releases the plugin instance on the plugin binary's own CRT heap. */
    virtual void Destroy() noexcept {
        delete this;
    }

protected:
    virtual ~IPlugin() = default;
};

/**
 * @brief Custom deleter that destroys an `IPlugin` via `IPlugin::Destroy()` on the plugin's own heap.
 */
struct PluginDeleter {
    void operator()(IPlugin* plugin) const noexcept {
        if (plugin) {
            plugin->Destroy();
        }
    }
};

using CreatePluginFn = IPlugin* (EEVS_CALL*)();

} // namespace eevs

#define EEVS_DECLARE_PLUGIN(PluginClass)                            \
    EEVS_PLUGIN_EXPORT ::eevs::IPlugin* EEVS_CALL EevsCreatePlugin_v1() { \
        return new PluginClass();                                   \
    }
