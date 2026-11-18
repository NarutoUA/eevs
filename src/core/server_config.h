#pragma once

/**
 * @file server_config.h
 * @brief Parser for EEVS configuration files.
 */

#include "core/containers.h"
#include "eevs/types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace eevs {

struct ServerConfig {
    std::string gatewayHost = "127.0.0.1";
    uint16_t gatewayPort = 30122;
    bool gatewayAutoReconnect = true;

    std::string voiceBindHost = "0.0.0.0";
    uint16_t voiceBindPort = 30123;
    size_t maxPeers = 4095;

    bool controlEnabled = true;
    std::string controlBindHost = "127.0.0.1";
    uint16_t controlBindPort = 0; // 0 = auto (voiceBindPort + 1)
    std::string controlKey;       // Optional explicit 'eevs_control_key'; falls back to sv_licenseKey

    uint32_t vadHangoverMs = 150;
    int opusBitrate = 48000;
    int opusComplexity = 5;

    std::string licenseKey;
    std::filesystem::path pluginsDir = "plugins";
    size_t clipCacheMaxMb = 64; // Optional 'eevs_clip_cache_mb' LRU cap for transient clips (default 64 MB)

    std::string logLevel = "info";
    std::filesystem::path logFile;

    // All parsed configuration properties (excluding sv_licenseKey) exposed to plugins via IHostContext
    FlatMap<std::string, std::string> properties;

    // Parses an EEVS configuration file (key-value pairs and '#' or '//' comments)
    [[nodiscard]] static bool LoadFromFile(
        const std::filesystem::path& configPath,
        ServerConfig& outConfig,
        std::string* outError = nullptr
    );

    [[nodiscard]] static bool ParseHostPort(
        std::string_view endpoint,
        std::string& outHost,
        uint16_t& outPort
    ) noexcept;
};

} // namespace eevs
