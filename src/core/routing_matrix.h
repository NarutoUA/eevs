#pragma once

#include "core/containers.h"
#include "eevs/interfaces/i_route_provider.h"
#include "eevs/types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace eevs {

class SessionRegistry;

enum class RouteOverrideAction : uint8_t {
    ForceMode = 0, // Replaces FXServer sync mode with custom mode
    MergeMode = 1, // Bitwise-ORs custom mode with FXServer sync mode
    Block     = 2  // Blocks this listener even if FXServer sync includes them
};

struct RouteOverrideRule {
    RoutingMode         mode   = RoutingMode::None;
    RouteOverrideAction action = RouteOverrideAction::ForceMode;
};

class RoutingMatrix {
public:
    RoutingMatrix();
    explicit RoutingMatrix(const SessionRegistry* sessions);
    ~RoutingMatrix() = default;

    RoutingMatrix(const RoutingMatrix&) = delete;
    RoutingMatrix& operator=(const RoutingMatrix&) = delete;

    void SetSessionRegistry(const SessionRegistry* sessions) noexcept {
        m_sessions = sessions;
    }

    void RegisterRouteProvider(IRouteProvider* provider);
    void UnregisterRouteProvider(IRouteProvider* provider);
    void RefreshSpeakerRoutes(ClientId speakerClientId);
    void RefreshAllSpeakerRoutes();

    // Base graph synced from FXServer VoiceStreamingSync
    void AddSyncTarget(ClientId speakerClientId, uint16_t rawAddedEntry);
    void RemoveSyncTarget(ClientId speakerClientId, uint16_t rawRemovedEntry);
    void ApplySyncDeltaBatch(
        ClientId speakerClientId,
        std::span<const uint16_t> rawAddedEntries,
        std::span<const uint16_t> rawRemovedEntries
    );
    void ClearClient(ClientId clientId);
    void ClearAllSyncTargets();

    // Custom route overrides
    void SetCustomRoute(
        ClientId speakerClientId,
        ClientId listenerClientId,
        RoutingMode mode,
        RouteOverrideAction action = RouteOverrideAction::ForceMode
    );
    void BlockRoute(ClientId speakerClientId, ClientId listenerClientId);
    void ClearCustomRoute(ClientId speakerClientId, ClientId listenerClientId);
    void ClearAllCustomRoutesForSpeaker(ClientId speakerClientId);
    void ClearAllCustomRoutes();

    void ReplaceCustomTargetsForSpeaker(
        ClientId speakerClientId,
        std::span<const ResolvedRoute> targets,
        RouteOverrideAction action = RouteOverrideAction::ForceMode
    );

    // Synthetic / non-player NetId routes
    void SetSyntheticNetRoute(NetId speakerNetId, ClientId listenerClientId, RoutingMode mode);
    void ReplaceSyntheticNetTargets(NetId speakerNetId, std::span<const ResolvedRoute> targets);
    void ClearSyntheticNetRoute(NetId speakerNetId, ClientId listenerClientId);
    void ClearAllSyntheticNetRoutes(NetId speakerNetId);
    void ClearAllSyntheticRoutes();

    [[nodiscard]] std::span<const ResolvedRoute> GetResolvedRoutes(ClientId speakerClientId) const noexcept;
    [[nodiscard]] std::span<const ResolvedRoute> GetSyntheticNetRoutes(NetId speakerNetId) const noexcept;

private:
    struct SpeakerRouteState {
        FlatMap<ClientId, uint16_t> syncRawEntries; // listenerClientId -> rawEntry (id | (mode << 13))
        FlatMap<ClientId, RouteOverrideRule> overrides;
        mutable std::vector<ResolvedRoute> resolved;
        mutable bool dirty = false;
    };

    struct SyntheticRouteState {
        FlatMap<ClientId, RoutingMode> listeners;
        std::vector<ResolvedRoute> resolved;
    };

    void RebuildSpeakerRoutes(ClientId speakerClientId, const SpeakerRouteState& state) const;
    static void RebuildSyntheticRoutes(SyntheticRouteState& state);

    const SessionRegistry* m_sessions = nullptr;
    std::vector<IRouteProvider*> m_routeProviders;
    std::vector<SpeakerRouteState> m_speakerRoutes;
    FlatMap<NetId, SyntheticRouteState> m_syntheticRoutes;
};

} // namespace eevs
