#pragma once

#include <cstdint>

namespace eevs {

inline constexpr uint32_t kApiVersion = 1;
inline constexpr const char* kCreatePluginSymbolName = "EevsCreatePlugin_v1";

} // namespace eevs

#if defined(_WIN32)
#define EEVS_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#define EEVS_CALL __cdecl
#else
#define EEVS_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#define EEVS_CALL
#endif
