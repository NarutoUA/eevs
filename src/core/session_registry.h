#pragma once

#include "core/containers.h"
#include "eevs/interfaces/i_session_listener.h"
#include "eevs/types.h"
#include "network/protocol.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

struct _ENetPeer;

namespace eevs {

struct VoicePlayerSession {
    ClientId clientId = 0;
    NetId netId = 0;
    std::array<uint8_t, 32> token{};
    uint32_t tokenJoaat = 0;
    _ENetPeer* peer = nullptr;
    bool authenticated = false;
    bool registered = false;
};

class SessionRegistry {
public:
    using PeerDisconnectCallback = std::function<void(_ENetPeer*)>;
    using ClientUnregisteredCallback = std::function<void(ClientId, NetId)>;

    SessionRegistry();
    ~SessionRegistry() = default;

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    void SetPeerDisconnectCallback(PeerDisconnectCallback callback) {
        m_onDisconnectPeer = std::move(callback);
    }

    void SetClientUnregisteredCallback(ClientUnregisteredCallback callback) {
        m_onClientUnregistered = std::move(callback);
    }

    // Plugin ISessionListener registration
    void RegisterSessionListener(ISessionListener* listener);
    void UnregisterSessionListener(ISessionListener* listener);

    // Client registration lifecycle
    void RegisterPlayerToken(ClientId clientId, NetId netId, const std::array<uint8_t, 32>& token);
    void UnregisterPlayer(ClientId clientId);
    void UnregisterAllPlayers();

    // Game client ENet peer binding
    void BindPeer(ClientId clientId, _ENetPeer* peer, protocol::PeerId enetPeerId);
    std::optional<ClientId> UnbindPeer(protocol::PeerId enetPeerId, bool wasTimeout = false);
    void ClearAllPeers();

    [[nodiscard]] bool IsTokenJoaatAllowed(uint32_t tokenJoaat) const;

    [[nodiscard]] VoicePlayerSession* FindByClientId(ClientId clientId) noexcept;
    [[nodiscard]] const VoicePlayerSession* FindByClientId(ClientId clientId) const noexcept;

    [[nodiscard]] VoicePlayerSession* FindByNetId(NetId netId) noexcept;
    [[nodiscard]] const VoicePlayerSession* FindByNetId(NetId netId) const noexcept;

    [[nodiscard]] VoicePlayerSession* FindByPeerId(protocol::PeerId enetPeerId) noexcept;
    [[nodiscard]] const VoicePlayerSession* FindByPeerId(protocol::PeerId enetPeerId) const noexcept;

    [[nodiscard]] std::optional<ClientId> ResolveClientIdByNetId(NetId netId) const noexcept;
    [[nodiscard]] std::optional<ClientId> ResolveClientIdByPeerId(protocol::PeerId enetPeerId) const noexcept;

    [[nodiscard]] std::span<const ClientId> GetActiveClientIds() const noexcept {
        return m_activeClientIds;
    }

    [[nodiscard]] size_t GetRegisteredClientCount() const noexcept {
        return m_activeClientIds.size();
    }

private:
    static constexpr uint16_t kInvalidIndex = 0xFFFFu;

    std::vector<VoicePlayerSession> m_sessionsByClientId;
    std::vector<uint16_t> m_netIdToClientId;
    std::vector<uint16_t> m_peerIdToClientId;
    std::vector<ClientId> m_activeClientIds;
    FlatMultiset<uint32_t> m_allowedTokenJoaats;
    std::vector<ISessionListener*> m_sessionListeners;
    PeerDisconnectCallback m_onDisconnectPeer;
    ClientUnregisteredCallback m_onClientUnregistered;
};

} // namespace eevs
