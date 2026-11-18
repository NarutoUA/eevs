#include "core/voice_engine.h"
#include "core/logger.h"
#include "core/profiler.h"
#include "network/enet.h"

namespace eevs {

VoiceEngine::VoiceEngine()
    : m_sessions()
    , m_routing(&m_sessions)
    , m_codecPool()
    , m_clipManager(m_codecPool)
    , m_gatewayClient(m_sessions, m_routing)
    , m_voiceServer(m_sessions)
    , m_synthesizer(m_sessions, m_routing, m_voiceServer, m_codecPool)
    , m_audioPipeline(m_sessions, m_routing, m_voiceServer, m_synthesizer, m_codecPool)
    , m_controlApi(*this)
    , m_pluginManager(m_sessions, m_routing, m_audioPipeline, m_synthesizer, m_clipManager, m_controlApi) {
    log::Initialize();
    enet_initialize();
    m_voiceServer.SetAudioPipeline(&m_audioPipeline);
    m_sessions.SetClientUnregisteredCallback([this](ClientId clientId, NetId netId) {
        m_audioPipeline.OnClientUnregistered(clientId);
        m_synthesizer.ClearSequence(netId);
        m_codecPool.ReleaseSyntheticNetId(netId);
    });
    m_synthesizer.RegisterTickCallback([this](PacketSynthesizer&) {
        m_pluginManager.TickAudioSources();
    });
}

VoiceEngine::~VoiceEngine() {
    Shutdown();
    enet_deinitialize();
}

void VoiceEngine::Shutdown() {
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;

    EEVS_LOG_INFO("[VoiceEngine] Initiating graceful shutdown...");
    StopControlApi();
    Disconnect();
    m_routing.ClearAllSyncTargets();
    m_sessions.UnregisterAllPlayers();
    StopVoiceServer();
    m_pluginManager.UnloadAll();
    m_sessions.SetClientUnregisteredCallback(nullptr);
    m_voiceServer.SetAudioPipeline(nullptr);
    EEVS_LOG_INFO("[VoiceEngine] Shutdown complete.");
}

bool VoiceEngine::Connect(std::string_view gatewayHost, uint16_t gatewayPort, std::string_view licenseKey) {
    if (!m_controlApi.VerifyToken("")) {
        // Auth already configured via StartControlApi
    } else if (!licenseKey.empty()) {
        m_controlApi.ConfigureAuth(licenseKey);
    }
    return m_gatewayClient.Connect(gatewayHost, gatewayPort, licenseKey);
}

bool VoiceEngine::StartVoiceServer(std::string_view bindHost, uint16_t bindPort, size_t maxPeers) {
    return m_voiceServer.Start(bindHost, bindPort, maxPeers);
}

bool VoiceEngine::StartControlApi(
    std::string_view bindHost,
    uint16_t bindPort,
    std::string_view licenseKey,
    std::string_view explicitControlKey
) {
    if (!licenseKey.empty() || !explicitControlKey.empty()) {
        m_controlApi.ConfigureAuth(licenseKey, explicitControlKey);
    }
    return m_controlApi.Start(bindHost, bindPort);
}

size_t VoiceEngine::LoadPlugins(const std::filesystem::path& directoryPath) {
    return m_pluginManager.LoadPluginsFromDirectory(directoryPath);
}

void VoiceEngine::Disconnect() {
    m_gatewayClient.Disconnect();
}

void VoiceEngine::StopVoiceServer() {
    m_voiceServer.Stop();
}

void VoiceEngine::StopControlApi() {
    m_controlApi.Stop();
}

void VoiceEngine::Poll(uint32_t timeoutMs) {
    EEVS_PROFILE_SCOPE("VoiceEngine::Poll");
    m_gatewayClient.Poll(m_voiceServer.IsRunning() ? 0 : timeoutMs);
    m_voiceServer.Poll(timeoutMs);
    m_controlApi.Poll();
    m_pluginManager.DispatchPendingEvents();
    m_synthesizer.Tick();
    m_audioPipeline.TickVad();
    EEVS_PROFILE_FRAME("MainLoop");
}

bool VoiceEngine::SetCustomRouteByNetId(
    NetId speakerNetId,
    NetId listenerNetId,
    RoutingMode mode,
    RouteOverrideAction action
) {
    auto listenerClientId = m_sessions.ResolveClientIdByNetId(listenerNetId);
    if (!listenerClientId) {
        return false;
    }

    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(speakerNetId)) {
        m_routing.SetCustomRoute(*speakerClientId, *listenerClientId, mode, action);
    } else {
        // Speaker is a synthetic / NPC NetID not backed by a player ClientID
        m_routing.SetSyntheticNetRoute(speakerNetId, *listenerClientId, mode);
    }
    return true;
}

bool VoiceEngine::ClearCustomRouteByNetId(NetId speakerNetId, NetId listenerNetId) {
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

bool VoiceEngine::ClearSpeakerCustomRoutesByNetId(NetId speakerNetId) {
    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(speakerNetId)) {
        m_routing.ClearAllCustomRoutesForSpeaker(*speakerClientId);
        return true;
    }
    m_routing.ClearAllSyntheticNetRoutes(speakerNetId);
    return true;
}

void VoiceEngine::ClearAllCustomRoutes() {
    m_routing.ClearAllCustomRoutes();
    m_routing.ClearAllSyntheticRoutes();
}

void VoiceEngine::SetCustomRoute(
    ClientId speakerClientId,
    ClientId listenerClientId,
    RoutingMode mode,
    RouteOverrideAction action
) {
    m_routing.SetCustomRoute(speakerClientId, listenerClientId, mode, action);
}

size_t VoiceEngine::ApplyBatchRoutes(std::span<const BatchRouteCommand> commands) {
    if (commands.empty()) {
        return 0;
    }

    size_t applied = 0;
    for (const BatchRouteCommand& cmd : commands) {
        bool ok = false;
        switch (cmd.op) {
            case BatchRouteOp::Add:
                ok = SetCustomRouteByNetId(cmd.fromNetId, cmd.toNetId, cmd.mode, RouteOverrideAction::ForceMode);
                break;
            case BatchRouteOp::Merge:
                ok = SetCustomRouteByNetId(cmd.fromNetId, cmd.toNetId, cmd.mode, RouteOverrideAction::MergeMode);
                break;
            case BatchRouteOp::Block:
                ok = SetCustomRouteByNetId(cmd.fromNetId, cmd.toNetId, RoutingMode::None, RouteOverrideAction::Block);
                break;
            case BatchRouteOp::Remove:
                ok = ClearCustomRouteByNetId(cmd.fromNetId, cmd.toNetId);
                break;
            case BatchRouteOp::ClearSpeaker:
                ok = ClearSpeakerCustomRoutesByNetId(cmd.fromNetId);
                break;
        }
        if (ok) {
            ++applied;
        }
    }
    return applied;
}

bool VoiceEngine::SetSpeakerCustomTargetsByNetId(
    NetId speakerNetId,
    std::span<const NetId> listenerNetIds,
    std::span<const RoutingMode> modes,
    RouteOverrideAction action
) {
    if (listenerNetIds.size() != modes.size()) {
        return false;
    }

    std::vector<ResolvedRoute> resolvedTargets;
    resolvedTargets.reserve(listenerNetIds.size());

    for (size_t i = 0; i < listenerNetIds.size(); ++i) {
        if (auto listenerClientId = m_sessions.ResolveClientIdByNetId(listenerNetIds[i])) {
            resolvedTargets.push_back(ResolvedRoute{*listenerClientId, modes[i]});
        }
    }

    if (auto speakerClientId = m_sessions.ResolveClientIdByNetId(speakerNetId)) {
        m_routing.ReplaceCustomTargetsForSpeaker(*speakerClientId, resolvedTargets, action);
    } else {
        m_routing.ReplaceSyntheticNetTargets(speakerNetId, resolvedTargets);
    }
    return true;
}

size_t VoiceEngine::InjectOpusFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    std::span<const uint8_t> opusPayload,
    std::span<const ClientId> explicitListeners
) {
    return m_synthesizer.InjectOpusFrame(senderNetId, fallbackMode, opusPayload, explicitListeners);
}

size_t VoiceEngine::InjectPcmFrame(
    NetId senderNetId,
    RoutingMode fallbackMode,
    std::span<const float> pcmSamples,
    std::span<const ClientId> explicitListeners
) {
    return m_synthesizer.InjectPcmFrame(senderNetId, fallbackMode, pcmSamples, explicitListeners);
}

} // namespace eevs
