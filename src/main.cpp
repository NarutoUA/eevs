#include "core/logger.h"
#include "core/server_config.h"
#include "core/voice_engine.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_shutdownRequested{false};

void SignalHandler(int /*sig*/) noexcept {
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

#if defined(_WIN32)
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_shutdownRequested.store(true, std::memory_order_relaxed);
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

void InstallSignalHandlers() noexcept {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
}

void PrintUsage(const char* progName) noexcept {
    const char* bin = progName ? progName : "eevs";
    EEVS_LOG_INFO("Usage: {} [config_file] (default: eevs.cfg)", bin);
}

} // namespace

int main(int argc, char* argv[]) {
    eevs::log::Initialize();
    InstallSignalHandlers();

    std::filesystem::path configPath;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            eevs::log::Shutdown();
            return 0;
        } else if (configPath.empty()) {
            configPath = arg;
        } else {
            EEVS_LOG_ERROR("[Config] Unexpected argument: {}", argv[i]);
            PrintUsage(argv[0]);
            eevs::log::Shutdown();
            return 1;
        }
    }

    if (configPath.empty()) {
        configPath = "eevs.cfg";
    }

    eevs::ServerConfig config{};
    std::string configError;
    if (!eevs::ServerConfig::LoadFromFile(configPath, config, &configError)) {
        EEVS_LOG_ERROR("[Config] {}", configError);
        eevs::log::Shutdown();
        return 1;
    }

    // Reconfigure logger with level and optional file sink from config
    if (!config.logFile.empty()) {
        eevs::log::Initialize(config.logLevel, config.logFile);
    } else {
        eevs::log::SetLevel(config.logLevel);
    }

    EEVS_LOG_INFO("[Config] Loaded '{}' (voice={}:{}, gateway={}:{}, maxPeers={})",
                  configPath.string(),
                  config.voiceBindHost,
                  config.voiceBindPort,
                  config.gatewayHost,
                  config.gatewayPort,
                  config.maxPeers);

    eevs::VoiceEngine voiceEngine;
    voiceEngine.GetPluginManager().SetConfigProperties(config.properties);
    voiceEngine.GetCodecPool().ConfigureEncoderSettings(config.opusBitrate, config.opusComplexity);
    voiceEngine.GetAudioPipeline().SetVadHangoverMs(config.vadHangoverMs);
    voiceEngine.GetGatewayClient().SetAutoReconnect(config.gatewayAutoReconnect);
    voiceEngine.GetClipManager().SetMaxTransientBytes(config.clipCacheMaxMb * 1024u * 1024u);

    if (!voiceEngine.StartVoiceServer(config.voiceBindHost, config.voiceBindPort, config.maxPeers)) {
        eevs::log::Shutdown();
        return 1;
    }

    if (config.controlEnabled) {
        const uint16_t controlPort = config.controlBindPort != 0
            ? config.controlBindPort
            : static_cast<uint16_t>(config.voiceBindPort + 1);
        voiceEngine.StartControlApi(
            config.controlBindHost,
            controlPort,
            config.licenseKey,
            config.controlKey
        );
    }

    voiceEngine.LoadPlugins(config.pluginsDir);

    if (!voiceEngine.Connect(config.gatewayHost, config.gatewayPort, config.licenseKey)) {
        eevs::log::Shutdown();
        return 1;
    }

    while (g_shutdownRequested.load(std::memory_order_relaxed) == false) {
        voiceEngine.Poll(1);
    }

    voiceEngine.Shutdown();
    eevs::log::Shutdown();
    return 0;
}
