#pragma once

/**
 * @file voice_udp_server.h
 * @brief Public ENet + raw UDP fast-path server (`30123`) accepting FiveM game clients (`ClientVoicePacket` ingress -> `ServerVoicePacket` egress).
 */

#include "network/enet.h"
#include "network/protocol.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

#if defined(_WIN32)
#define EEVS_ENET_CALLBACK __cdecl
#else
#define EEVS_ENET_CALLBACK
#endif

namespace eevs {

class AudioPipeline;
class SessionRegistry;

class VoiceUdpServer {
public:
    explicit VoiceUdpServer(SessionRegistry& sessions);
    ~VoiceUdpServer();

    VoiceUdpServer(const VoiceUdpServer&) = delete;
    VoiceUdpServer& operator=(const VoiceUdpServer&) = delete;

    void SetAudioPipeline(AudioPipeline* pipeline) noexcept {
        m_pipeline = pipeline;
    }

    bool Start(std::string_view bindHost, uint16_t bindPort, size_t maxPeers = 4095);
    void Stop();
    void Poll(uint32_t timeoutMs);

    [[nodiscard]] bool IsRunning() const noexcept { return m_voiceHost != nullptr; }

    void DisconnectPeerNow(_ENetPeer* peer);
    bool SendRawUdpToPeer(const _ENetPeer* peer, const uint8_t* data, size_t length, size_t nonZeroPrefixBytes = 0);

private:
    struct EgressDatagram {
        ENetAddress destAddress{};
        uint16_t length = 0;
        uint16_t dirtyPrefixLen = 0;
        std::array<uint8_t, protocol::kDefaultMtu> payload{};
    };

    struct alignas(64) EgressWorkerQueue {
        static constexpr size_t kCapacity = 8192; // Power of 2
        static constexpr size_t kMask = kCapacity - 1;

        std::vector<EgressDatagram> ring{kCapacity};
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};
        alignas(64) std::atomic<bool> waiting{false};
        std::binary_semaphore wakeSem{0};
    };

    void StartEgressWorkers();
    void StopEgressWorkers();
    void EgressWorkerLoop(std::stop_token st, size_t workerIndex);

    static int EEVS_ENET_CALLBACK VoiceRawReceiveIntercept(_ENetHost* host, void* event);

    void HandleGameClientConnect(_ENetPeer* peer, uint32_t tokenJoaat);
    void HandleGameClientPacket(_ENetPeer* peer, uint8_t channelId, const uint8_t* data, size_t length);
    void HandleGameClientAuth(_ENetPeer* peer, const uint8_t* data, size_t length);
    void HandleGameClientDisconnect(_ENetPeer* peer, bool wasTimeout);
    bool SendClientHandshakeDone(_ENetPeer* peer);

    void HandleVoiceAudioPacket(uint16_t senderClientId, const uint8_t* data, size_t length, bool fromFastPath);

    SessionRegistry& m_sessions;
    AudioPipeline* m_pipeline = nullptr;
    _ENetHost* m_voiceHost = nullptr;
    uint32_t m_lastEnetServiceMs = 0;
    size_t m_nextWorkerIdx = 0;
    std::vector<std::unique_ptr<EgressWorkerQueue>> m_egressQueues;
    std::vector<std::jthread> m_egressThreads;
};

} // namespace eevs
