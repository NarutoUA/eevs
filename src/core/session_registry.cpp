#include "core/session_registry.h"
#include "core/crypto_utils.h"
#include "core/logger.h"

#include <algorithm>
#include <string>

namespace eevs {

SessionRegistry::SessionRegistry()
    : m_sessionsByClientId(kMaxClients)
    , m_netIdToClientId(65536, kInvalidIndex)
    , m_peerIdToClientId(protocol::kMaxPeers, kInvalidIndex) {
    m_activeClientIds.reserve(2048);
}

void SessionRegistry::RegisterSessionListener(ISessionListener* listener) {
    if (!listener) {
        return;
    }
    if (std::ranges::find(m_sessionListeners, listener) == m_sessionListeners.end()) {
        m_sessionListeners.push_back(listener);
    }
}

void SessionRegistry::UnregisterSessionListener(ISessionListener* listener) {
    std::erase(m_sessionListeners, listener);
}

void SessionRegistry::RegisterPlayerToken(uint16_t clientId, uint16_t netId, const std::array<uint8_t, 32>& token) {
    if (clientId >= kMaxClients) {
        return;
    }

    // If this client is already registered with the same token (e.g. ped netId updated after spawn/respawn),
    // update the netId mapping in-place without disconnecting the active ENet peer.
    VoicePlayerSession& existing = m_sessionsByClientId[clientId];
    if (existing.registered && existing.token == token) {
        if (m_netIdToClientId[existing.netId] == clientId) {
            m_netIdToClientId[existing.netId] = kInvalidIndex;
        }
        existing.netId = netId;
        m_netIdToClientId[netId] = clientId;
        EEVS_LOG_INFO("[VoiceServer] Updated client {} ped netID={}", clientId, netId);
        return;
    }

    UnregisterPlayer(clientId);

    VoicePlayerSession& session = m_sessionsByClientId[clientId];
    session.clientId = clientId;
    session.netId = netId;
    session.token = token;
    session.tokenJoaat = crypto::ComputeJoaat(token.data(), token.size());
    session.peer = nullptr;
    session.authenticated = false;
    session.registered = true;

    m_netIdToClientId[netId] = clientId;
    m_activeClientIds.push_back(clientId);
    m_allowedTokenJoaats.insert(session.tokenJoaat);

    std::string tokenHex = crypto::ToHexString(token.data(), token.size());
    EEVS_LOG_INFO("[VoiceServer] Registered client {} (netID={}, token={}, tokenJoaat=0x{:08X})",
                  clientId, netId, tokenHex, session.tokenJoaat);

    for (ISessionListener* listener : m_sessionListeners) {
        listener->OnClientRegistered(clientId, netId);
    }
}

void SessionRegistry::UnregisterPlayer(uint16_t clientId) {
    if (clientId >= kMaxClients) {
        return;
    }

    VoicePlayerSession& session = m_sessionsByClientId[clientId];
    if (!session.registered) {
        return;
    }

    const NetId netId = session.netId;
    const bool wasAuthenticated = session.authenticated;

    auto joaatIt = m_allowedTokenJoaats.find(session.tokenJoaat);
    if (joaatIt != m_allowedTokenJoaats.end()) {
        m_allowedTokenJoaats.erase(joaatIt);
    }

    if (m_netIdToClientId[netId] == clientId) {
        m_netIdToClientId[netId] = kInvalidIndex;
    }

    if (session.peer) {
        _ENetPeer* peerToDisconnect = session.peer;
        session.peer = nullptr;
        session.authenticated = false;

        for (uint16_t& mappedClient : m_peerIdToClientId) {
            if (mappedClient == clientId) {
                mappedClient = kInvalidIndex;
            }
        }

        if (m_onDisconnectPeer) {
            m_onDisconnectPeer(peerToDisconnect);
        }
    }

    session = VoicePlayerSession{};
    std::erase(m_activeClientIds, clientId);

    if (wasAuthenticated) {
        for (ISessionListener* listener : m_sessionListeners) {
            listener->OnClientDisconnected(clientId, netId, false);
        }
    }
    for (ISessionListener* listener : m_sessionListeners) {
        listener->OnClientUnregistered(clientId, netId);
    }
    if (m_onClientUnregistered) {
        m_onClientUnregistered(clientId, netId);
    }
    EEVS_LOG_INFO("[VoiceServer] Unregistered client {}", clientId);
}

void SessionRegistry::UnregisterAllPlayers() {
    std::vector<ClientId> clientIds = m_activeClientIds;
    for (ClientId clientId : clientIds) {
        UnregisterPlayer(clientId);
    }
}

void SessionRegistry::BindPeer(ClientId clientId, _ENetPeer* peer, protocol::PeerId enetPeerId) {
    if (clientId >= kMaxClients || enetPeerId >= protocol::kMaxPeers) {
        return;
    }

    VoicePlayerSession& session = m_sessionsByClientId[clientId];
    if (!session.registered) {
        return;
    }

    session.peer = peer;
    session.authenticated = (peer != nullptr);
    if (peer) {
        m_peerIdToClientId[enetPeerId] = clientId;
        const NetId netId = session.netId;
        for (ISessionListener* listener : m_sessionListeners) {
            listener->OnClientAuthenticated(clientId, netId);
        }
    }
}

std::optional<uint16_t> SessionRegistry::UnbindPeer(uint16_t enetPeerId, bool wasTimeout) {
    if (enetPeerId >= protocol::kMaxPeers) {
        return std::nullopt;
    }

    const uint16_t clientId = m_peerIdToClientId[enetPeerId];
    if (clientId == kInvalidIndex || clientId >= kMaxClients) {
        return std::nullopt;
    }

    m_peerIdToClientId[enetPeerId] = kInvalidIndex;

    VoicePlayerSession& session = m_sessionsByClientId[clientId];
    const NetId netId = session.netId;
    const bool wasAuthenticated = session.authenticated;
    session.peer = nullptr;
    session.authenticated = false;

    if (wasAuthenticated) {
        for (ISessionListener* listener : m_sessionListeners) {
            listener->OnClientDisconnected(clientId, netId, wasTimeout);
        }
    }

    return clientId;
}

void SessionRegistry::ClearAllPeers() {
    std::ranges::fill(m_peerIdToClientId, kInvalidIndex);
    for (ClientId clientId : m_activeClientIds) {
        m_sessionsByClientId[clientId].peer = nullptr;
        m_sessionsByClientId[clientId].authenticated = false;
    }
}

bool SessionRegistry::IsTokenJoaatAllowed(uint32_t tokenJoaat) const {
    return m_allowedTokenJoaats.find(tokenJoaat) != m_allowedTokenJoaats.end();
}

VoicePlayerSession* SessionRegistry::FindByClientId(uint16_t clientId) noexcept {
    if (clientId >= kMaxClients) {
        return nullptr;
    }
    VoicePlayerSession& session = m_sessionsByClientId[clientId];
    return session.registered ? &session : nullptr;
}

const VoicePlayerSession* SessionRegistry::FindByClientId(uint16_t clientId) const noexcept {
    if (clientId >= kMaxClients) {
        return nullptr;
    }
    const VoicePlayerSession& session = m_sessionsByClientId[clientId];
    return session.registered ? &session : nullptr;
}

VoicePlayerSession* SessionRegistry::FindByNetId(uint16_t netId) noexcept {
    if (auto cid = ResolveClientIdByNetId(netId)) {
        return FindByClientId(*cid);
    }
    return nullptr;
}

const VoicePlayerSession* SessionRegistry::FindByNetId(uint16_t netId) const noexcept {
    if (auto cid = ResolveClientIdByNetId(netId)) {
        return FindByClientId(*cid);
    }
    return nullptr;
}

VoicePlayerSession* SessionRegistry::FindByPeerId(uint16_t enetPeerId) noexcept {
    if (enetPeerId >= protocol::kMaxPeers) {
        return nullptr;
    }
    const uint16_t clientId = m_peerIdToClientId[enetPeerId];
    return clientId != kInvalidIndex ? FindByClientId(clientId) : nullptr;
}

const VoicePlayerSession* SessionRegistry::FindByPeerId(uint16_t enetPeerId) const noexcept {
    if (enetPeerId >= protocol::kMaxPeers) {
        return nullptr;
    }
    const uint16_t clientId = m_peerIdToClientId[enetPeerId];
    return clientId != kInvalidIndex ? FindByClientId(clientId) : nullptr;
}

std::optional<uint16_t> SessionRegistry::ResolveClientIdByNetId(uint16_t netId) const noexcept {
    if (netId == 0) {
        return std::nullopt;
    }
    const uint16_t clientId = m_netIdToClientId[netId];
    if (clientId != kInvalidIndex) {
        return clientId;
    }
    // Also support resolving by FiveM server ID (`source` / `GetPlayerServerId` == `clientId`)
    if (netId < kMaxClients && m_sessionsByClientId[netId].registered) {
        return netId;
    }
    return std::nullopt;
}

std::optional<uint16_t> SessionRegistry::ResolveClientIdByPeerId(uint16_t enetPeerId) const noexcept {
    if (enetPeerId >= protocol::kMaxPeers) {
        return std::nullopt;
    }
    const uint16_t clientId = m_peerIdToClientId[enetPeerId];
    if (clientId == kInvalidIndex) {
        return std::nullopt;
    }
    return clientId;
}

} // namespace eevs
