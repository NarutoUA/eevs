#include "core/routing_matrix.h"
#include "core/session_registry.h"

#include <algorithm>

namespace eevs {

namespace {
    constexpr size_t kMaxPluginRouteTargets = 4096;
}

RoutingMatrix::RoutingMatrix()
    : m_speakerRoutes(kMaxClients) {
}

RoutingMatrix::RoutingMatrix(const SessionRegistry* sessions)
    : m_sessions(sessions)
    , m_speakerRoutes(kMaxClients) {
}

void RoutingMatrix::RegisterRouteProvider(IRouteProvider* provider) {
    if (!provider) {
        return;
    }
    if (std::ranges::find(m_routeProviders, provider) == m_routeProviders.end()) {
        m_routeProviders.push_back(provider);
        RefreshAllSpeakerRoutes();
    }
}

void RoutingMatrix::UnregisterRouteProvider(IRouteProvider* provider) {
    if (std::erase(m_routeProviders, provider) > 0) {
        RefreshAllSpeakerRoutes();
    }
}

void RoutingMatrix::RefreshSpeakerRoutes(ClientId speakerClientId) {
    if (speakerClientId < kMaxClients) {
        m_speakerRoutes[speakerClientId].dirty = true;
    }
}

void RoutingMatrix::RefreshAllSpeakerRoutes() {
    for (SpeakerRouteState& state : m_speakerRoutes) {
        if (!state.syncRawEntries.empty() || !state.overrides.empty() || !state.resolved.empty()) {
            state.dirty = true;
        }
    }
}

void RoutingMatrix::AddSyncTarget(ClientId speakerClientId, uint16_t rawAddedEntry) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    ClientId targetClientId = static_cast<ClientId>(rawAddedEntry & 0x1FFFu);
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    state.syncRawEntries[targetClientId] = rawAddedEntry;
    state.dirty = true;
}

void RoutingMatrix::RemoveSyncTarget(ClientId speakerClientId, uint16_t rawRemovedEntry) {
    if (speakerClientId >= kMaxClients) {
        return;
    }

    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    ClientId targetClientId = static_cast<ClientId>(rawRemovedEntry & 0x1FFFu);
    auto targetIt = state.syncRawEntries.find(targetClientId);
    if (targetIt == state.syncRawEntries.end()) {
        return;
    }

    // Clear matching mode bits (bits 13..15); remove target if no modes remain
    uint16_t remaining = static_cast<uint16_t>(targetIt->second & (~rawRemovedEntry | 0x1FFFu));
    if (remaining > 0x1FFFu) {
        targetIt->second = remaining;
    } else {
        state.syncRawEntries.erase(targetIt);
    }

    state.dirty = true;
}

void RoutingMatrix::ApplySyncDeltaBatch(
    ClientId speakerClientId,
    std::span<const uint16_t> rawAddedEntries,
    std::span<const uint16_t> rawRemovedEntries
) {
    if (speakerClientId >= kMaxClients || (rawAddedEntries.empty() && rawRemovedEntries.empty())) {
        return;
    }

    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];

    for (uint16_t rawAddedEntry : rawAddedEntries) {
        ClientId targetClientId = static_cast<ClientId>(rawAddedEntry & 0x1FFFu);
        state.syncRawEntries[targetClientId] = rawAddedEntry;
    }

    for (uint16_t rawRemovedEntry : rawRemovedEntries) {
        ClientId targetClientId = static_cast<ClientId>(rawRemovedEntry & 0x1FFFu);
        auto targetIt = state.syncRawEntries.find(targetClientId);
        if (targetIt != state.syncRawEntries.end()) {
            uint16_t remaining = static_cast<uint16_t>(targetIt->second & (~rawRemovedEntry | 0x1FFFu));
            if (remaining > 0x1FFFu) {
                targetIt->second = remaining;
            } else {
                state.syncRawEntries.erase(targetIt);
            }
        }
    }

    state.dirty = true;
}

void RoutingMatrix::ClearClient(ClientId clientId) {
    if (clientId < kMaxClients) {
        SpeakerRouteState& speakerState = m_speakerRoutes[clientId];
        speakerState.syncRawEntries.clear();
        speakerState.overrides.clear();
        speakerState.resolved.clear();
        speakerState.dirty = false;
    }

    auto markDirtyIfPresent = [clientId](SpeakerRouteState& state) {
        bool modified = false;
        if (state.syncRawEntries.erase(clientId) > 0) {
            modified = true;
        }
        if (state.overrides.erase(clientId) > 0) {
            modified = true;
        }
        if (modified) {
            state.dirty = true;
        }
    };

    if (m_sessions) {
        for (ClientId activeId : m_sessions->GetActiveClientIds()) {
            if (activeId < kMaxClients) {
                markDirtyIfPresent(m_speakerRoutes[activeId]);
            }
        }
    } else {
        for (SpeakerRouteState& state : m_speakerRoutes) {
            markDirtyIfPresent(state);
        }
    }

    for (auto&& [_, synthState] : m_syntheticRoutes) {
        if (synthState.listeners.erase(clientId) > 0) {
            RebuildSyntheticRoutes(synthState);
        }
    }
}

void RoutingMatrix::ClearAllSyncTargets() {
    for (SpeakerRouteState& state : m_speakerRoutes) {
        if (!state.syncRawEntries.empty()) {
            state.syncRawEntries.clear();
            state.dirty = true;
        }
    }
}

void RoutingMatrix::SetCustomRoute(
    ClientId speakerClientId,
    ClientId listenerClientId,
    RoutingMode mode,
    RouteOverrideAction action
) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    if (mode == RoutingMode::None && action == RouteOverrideAction::ForceMode) {
        state.overrides[listenerClientId] = RouteOverrideRule{RoutingMode::None, RouteOverrideAction::Block};
    } else {
        state.overrides[listenerClientId] = RouteOverrideRule{mode, action};
    }
    state.dirty = true;
}

void RoutingMatrix::BlockRoute(ClientId speakerClientId, ClientId listenerClientId) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    state.overrides[listenerClientId] = RouteOverrideRule{RoutingMode::None, RouteOverrideAction::Block};
    state.dirty = true;
}

void RoutingMatrix::ClearCustomRoute(ClientId speakerClientId, ClientId listenerClientId) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    if (state.overrides.erase(listenerClientId) > 0) {
        state.dirty = true;
    }
}

void RoutingMatrix::ClearAllCustomRoutesForSpeaker(ClientId speakerClientId) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    if (!state.overrides.empty()) {
        state.overrides.clear();
        state.dirty = true;
    }
}

void RoutingMatrix::ClearAllCustomRoutes() {
    for (SpeakerRouteState& state : m_speakerRoutes) {
        if (!state.overrides.empty()) {
            state.overrides.clear();
            state.dirty = true;
        }
    }
}

void RoutingMatrix::ReplaceCustomTargetsForSpeaker(
    ClientId speakerClientId,
    std::span<const ResolvedRoute> targets,
    RouteOverrideAction action
) {
    if (speakerClientId >= kMaxClients) {
        return;
    }
    SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    state.overrides.clear();
    for (const ResolvedRoute& target : targets) {
        if (target.mode == RoutingMode::None && action == RouteOverrideAction::ForceMode) {
            state.overrides[target.targetClientId] = RouteOverrideRule{RoutingMode::None, RouteOverrideAction::Block};
        } else {
            state.overrides[target.targetClientId] = RouteOverrideRule{target.mode, action};
        }
    }
    state.dirty = true;
}

void RoutingMatrix::SetSyntheticNetRoute(NetId speakerNetId, ClientId listenerClientId, RoutingMode mode) {
    if (mode == RoutingMode::None) {
        ClearSyntheticNetRoute(speakerNetId, listenerClientId);
        return;
    }

    SyntheticRouteState& state = m_syntheticRoutes[speakerNetId];
    state.listeners[listenerClientId] = mode;
    RebuildSyntheticRoutes(state);
}

void RoutingMatrix::ReplaceSyntheticNetTargets(NetId speakerNetId, std::span<const ResolvedRoute> targets) {
    if (targets.empty()) {
        m_syntheticRoutes.erase(speakerNetId);
        return;
    }

    SyntheticRouteState& state = m_syntheticRoutes[speakerNetId];
    state.listeners.clear();
    for (const ResolvedRoute& target : targets) {
        if (target.mode != RoutingMode::None) {
            state.listeners[target.targetClientId] = target.mode;
        }
    }
    RebuildSyntheticRoutes(state);
}

void RoutingMatrix::ClearSyntheticNetRoute(NetId speakerNetId, ClientId listenerClientId) {
    auto it = m_syntheticRoutes.find(speakerNetId);
    if (it == m_syntheticRoutes.end()) {
        return;
    }

    if (it->second.listeners.erase(listenerClientId) > 0) {
        if (it->second.listeners.empty()) {
            m_syntheticRoutes.erase(it);
        } else {
            RebuildSyntheticRoutes(it->second);
        }
    }
}

void RoutingMatrix::ClearAllSyntheticNetRoutes(NetId speakerNetId) {
    m_syntheticRoutes.erase(speakerNetId);
}

void RoutingMatrix::ClearAllSyntheticRoutes() {
    m_syntheticRoutes.clear();
}

std::span<const ResolvedRoute> RoutingMatrix::GetResolvedRoutes(ClientId speakerClientId) const noexcept {
    if (speakerClientId >= kMaxClients) {
        return {};
    }
    const SpeakerRouteState& state = m_speakerRoutes[speakerClientId];
    if (state.dirty) {
        RebuildSpeakerRoutes(speakerClientId, state);
    }
    return state.resolved;
}

std::span<const ResolvedRoute> RoutingMatrix::GetSyntheticNetRoutes(NetId speakerNetId) const noexcept {
    auto it = m_syntheticRoutes.find(speakerNetId);
    if (it == m_syntheticRoutes.end()) {
        return {};
    }
    return it->second.resolved;
}

void RoutingMatrix::RebuildSpeakerRoutes(ClientId speakerClientId, const SpeakerRouteState& state) const {
    state.dirty = false;
    state.resolved.clear();
    state.resolved.reserve(state.syncRawEntries.size() + state.overrides.size());

    // 1. Process base FXServer sync entries, applying any overrides on matching listeners
    for (const auto& [listenerClientId, rawEntry] : state.syncRawEntries) {
        auto syncMode = static_cast<RoutingMode>((rawEntry >> 13) & 0x03u);
        RoutingMode finalMode = syncMode;

        auto overrideIt = state.overrides.find(listenerClientId);
        if (overrideIt != state.overrides.end()) {
            const RouteOverrideRule& rule = overrideIt->second;
            switch (rule.action) {
                case RouteOverrideAction::ForceMode:
                    finalMode = rule.mode;
                    break;
                case RouteOverrideAction::MergeMode:
                    finalMode = syncMode | rule.mode;
                    break;
                case RouteOverrideAction::Block:
                    finalMode = RoutingMode::None;
                    break;
            }
        }

        if (finalMode != RoutingMode::None) {
            state.resolved.push_back(ResolvedRoute{listenerClientId, finalMode});
        }
    }

    // 2. Process custom override rules for listeners not present in FXServer sync
    for (const auto& [listenerClientId, rule] : state.overrides) {
        if (state.syncRawEntries.contains(listenerClientId)) {
            continue;
        }
        if (rule.action != RouteOverrideAction::Block && rule.mode != RoutingMode::None) {
            state.resolved.push_back(ResolvedRoute{listenerClientId, rule.mode});
        }
    }

    // 3. Allow registered IRouteProvider plugins to inspect/augment routes
    if (!m_routeProviders.empty()) {
        NetId speakerNetId = 0;
        if (m_sessions) {
            if (const VoicePlayerSession* session = m_sessions->FindByClientId(speakerClientId)) {
                speakerNetId = session->netId;
            }
        }

        size_t count = state.resolved.size();
        const size_t maxCap = std::max(count + 64, kMaxPluginRouteTargets);
        state.resolved.resize(maxCap);

        for (IRouteProvider* provider : m_routeProviders) {
            provider->ResolveSpeakerRoutes(
                speakerClientId,
                speakerNetId,
                state.resolved.data(),
                count,
                maxCap
            );
            count = std::min(count, maxCap);
        }

        state.resolved.resize(count);
    }
}

void RoutingMatrix::RebuildSyntheticRoutes(SyntheticRouteState& state) {
    state.resolved.clear();
    state.resolved.reserve(state.listeners.size());
    for (const auto& [listenerClientId, mode] : state.listeners) {
        if (mode != RoutingMode::None) {
            state.resolved.push_back(ResolvedRoute{listenerClientId, mode});
        }
    }
}

} // namespace eevs
