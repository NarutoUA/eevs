#pragma once

#include "audio/audio_pipeline.h"
#include "audio/clip_manager.h"
#include "audio/opus_codec_pool.h"
#include "audio/packet_synthesizer.h"
#include "core/routing_matrix.h"
#include "core/session_registry.h"
#include "network/control_api_server.h"
#include "network/fx_gateway_client.h"
#include "network/voice_udp_server.h"
#include "plugins/plugin_manager.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace eevs {

/**
 * @brief Top-level external voice server engine coordinating FXServer gateway sync,
 *        game client ENet/UDP voice transport, audio DSP/taps, clip synthesis, and plugins.
 */
class VoiceEngine {
public:
    VoiceEngine();
    ~VoiceEngine();

    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    // Connects to the game server's voice gateway (e.g. 127.0.0.1:30122)
    bool Connect(std::string_view gatewayHost, uint16_t gatewayPort, std::string_view licenseKey);

    // Starts the external voice ENet server to accept game clients (e.g. 0.0.0.0:30123)
    bool StartVoiceServer(std::string_view bindHost, uint16_t bindPort, size_t maxPeers = 4095);

    // Starts the local HTTP/JSON Control API server (e.g. 127.0.0.1:30124)
    bool StartControlApi(
        std::string_view bindHost = "127.0.0.1",
        uint16_t bindPort = 30124,
        std::string_view licenseKey = {},
        std::string_view explicitControlKey = {}
    );

    // Loads shared library plugins (.dll / .so) from the given directory
    size_t LoadPlugins(const std::filesystem::path& directoryPath);

    void Disconnect();
    void StopVoiceServer();
    void StopControlApi();
    void Shutdown();
    void Poll(uint32_t timeoutMs = 15);

    [[nodiscard]] bool IsConnected() const noexcept { return m_gatewayClient.IsConnected(); }
    [[nodiscard]] bool IsAuthenticated() const noexcept { return m_gatewayClient.IsAuthenticated(); }
    [[nodiscard]] bool IsVoiceServerRunning() const noexcept { return m_voiceServer.IsRunning(); }
    [[nodiscard]] bool IsControlApiRunning() const noexcept { return m_controlApi.IsRunning(); }

    // Custom Routing & Mode 2 API (by NetID or ClientID)
    void SetCustomRoute(
        ClientId speakerClientId,
        ClientId listenerClientId,
        RoutingMode mode,
        RouteOverrideAction action = RouteOverrideAction::ForceMode
    );
    bool SetCustomRouteByNetId(
        NetId speakerNetId,
        NetId listenerNetId,
        RoutingMode mode,
        RouteOverrideAction action = RouteOverrideAction::ForceMode
    );
    bool ClearCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId);
    bool ClearSpeakerCustomRoutesByNetId(NetId speakerNetId);
    void ClearAllCustomRoutes();

    size_t ApplyBatchRoutes(std::span<const BatchRouteCommand> commands);
    bool SetSpeakerCustomTargetsByNetId(
        NetId speakerNetId,
        std::span<const NetId> listenerNetIds,
        std::span<const RoutingMode> modes,
        RouteOverrideAction action = RouteOverrideAction::ForceMode
    );

    // Synthetic ServerVoicePacket Audio Injection API (Opus or 48kHz mono float PCM)
    size_t InjectOpusFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const uint8_t> opusPayload,
        std::span<const ClientId> explicitListeners = {}
    );

    size_t InjectPcmFrame(
        NetId senderNetId,
        RoutingMode fallbackMode,
        std::span<const float> pcmSamples,
        std::span<const ClientId> explicitListeners = {}
    );

    [[nodiscard]] SessionRegistry& GetSessions() noexcept { return m_sessions; }
    [[nodiscard]] const SessionRegistry& GetSessions() const noexcept { return m_sessions; }

    [[nodiscard]] RoutingMatrix& GetRouting() noexcept { return m_routing; }
    [[nodiscard]] const RoutingMatrix& GetRouting() const noexcept { return m_routing; }

    [[nodiscard]] FxGatewayClient& GetGatewayClient() noexcept { return m_gatewayClient; }
    [[nodiscard]] const FxGatewayClient& GetGatewayClient() const noexcept { return m_gatewayClient; }

    [[nodiscard]] VoiceUdpServer& GetVoiceServer() noexcept { return m_voiceServer; }
    [[nodiscard]] const VoiceUdpServer& GetVoiceServer() const noexcept { return m_voiceServer; }

    [[nodiscard]] OpusCodecPool& GetCodecPool() noexcept { return m_codecPool; }
    [[nodiscard]] const OpusCodecPool& GetCodecPool() const noexcept { return m_codecPool; }

    [[nodiscard]] ClipManager& GetClipManager() noexcept { return m_clipManager; }
    [[nodiscard]] const ClipManager& GetClipManager() const noexcept { return m_clipManager; }

    [[nodiscard]] PacketSynthesizer& GetSynthesizer() noexcept { return m_synthesizer; }
    [[nodiscard]] const PacketSynthesizer& GetSynthesizer() const noexcept { return m_synthesizer; }

    [[nodiscard]] AudioPipeline& GetAudioPipeline() noexcept { return m_audioPipeline; }
    [[nodiscard]] const AudioPipeline& GetAudioPipeline() const noexcept { return m_audioPipeline; }

    [[nodiscard]] ControlApiServer& GetControlApi() noexcept { return m_controlApi; }
    [[nodiscard]] const ControlApiServer& GetControlApi() const noexcept { return m_controlApi; }

    [[nodiscard]] PluginManager& GetPluginManager() noexcept { return m_pluginManager; }
    [[nodiscard]] const PluginManager& GetPluginManager() const noexcept { return m_pluginManager; }

private:
    SessionRegistry m_sessions;
    RoutingMatrix m_routing;
    OpusCodecPool m_codecPool;
    ClipManager m_clipManager;
    FxGatewayClient m_gatewayClient;
    VoiceUdpServer m_voiceServer;
    PacketSynthesizer m_synthesizer;
    AudioPipeline m_audioPipeline;
    ControlApiServer m_controlApi;
    PluginManager m_pluginManager;
    bool m_shuttingDown = false;
};

} // namespace eevs
