#pragma once

/**
 * @file fx_gateway_client.h
 * @brief ENet client connecting to FXServer's voice gateway (`30122`) with SHA-256 auth, LZ4 `VoiceStreamingSync` decoding, and exponential backoff auto-reconnect.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace eevs {

class RoutingMatrix;
class SessionRegistry;

class FxGatewayClient {
public:
    static constexpr auto kInitialReconnectDelay = std::chrono::milliseconds(1000);
    static constexpr auto kMaxReconnectDelay     = std::chrono::milliseconds(15000);
    static constexpr auto kConnectAttemptTimeout = std::chrono::milliseconds(5000);

    FxGatewayClient(SessionRegistry& sessions, RoutingMatrix& routing);
    ~FxGatewayClient();

    FxGatewayClient(const FxGatewayClient&) = delete;
    FxGatewayClient& operator=(const FxGatewayClient&) = delete;

    bool Connect(std::string_view gatewayHost, uint16_t gatewayPort, std::string_view licenseKey);
    void Disconnect();
    void Poll(uint32_t timeoutMs);

    void SetAutoReconnect(bool enabled) noexcept { m_autoReconnect = enabled; }
    [[nodiscard]] bool IsAutoReconnectEnabled() const noexcept { return m_autoReconnect; }

    [[nodiscard]] bool IsConnected() const noexcept { return m_connected; }
    [[nodiscard]] bool IsAuthenticated() const noexcept { return m_authenticated; }

private:
    bool InitiateConnectAttempt();
    void CleanupHostAndPeer();
    void ScheduleReconnect();
    bool SendGatewayAuthPacket();
    void HandleGatewayPacket(const uint8_t* data, size_t length);
    void HandleVoiceStreamingSync(const uint8_t* payload, size_t length);

    SessionRegistry& m_sessions;
    RoutingMatrix& m_routing;
    _ENetHost* m_gatewayHost = nullptr;
    _ENetPeer* m_gatewayPeer = nullptr;

    std::string m_gatewayHostName;
    uint16_t m_gatewayPort = 0;
    std::array<uint8_t, 32> m_licenseSha256{};
    uint32_t m_licenseJoaat = 0;

    bool m_connected = false;
    bool m_authenticated = false;
    bool m_connecting = false;
    bool m_autoReconnect = true;
    bool m_hasTargetConfig = false;
    uint32_t m_reconnectAttempts = 0;
    std::chrono::steady_clock::time_point m_nextReconnectTime{};
    std::chrono::steady_clock::time_point m_connectStartTime{};

    std::vector<uint8_t> m_decompressBuffer;
};

} // namespace eevs
