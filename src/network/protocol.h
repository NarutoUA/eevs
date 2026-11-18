#pragma once

#include <cstddef>
#include <cstdint>

namespace eevs::protocol {

constexpr uint32_t kDefaultMtu = 1300;
constexpr size_t kChannelCount = 4;
constexpr uint32_t kPingIntervalMs = 500;
constexpr uint32_t kTimeoutLimit = 32;
constexpr uint32_t kTimeoutMinimumMs = 500;
constexpr uint32_t kTimeoutMaximumMs = 30000;
constexpr uint32_t kUnauthenticatedTimeoutLimit = 4;
constexpr uint32_t kUnauthenticatedTimeoutMinimumMs = 1500;
constexpr uint32_t kUnauthenticatedTimeoutMaximumMs = 3000;
constexpr uint32_t kThrottleIntervalMs = 1000;
constexpr uint32_t kThrottleAcceleration = 32;
constexpr uint32_t kThrottleDeceleration = 0;
constexpr size_t kMaxSyncCompressedSize = 256 * 1024;
constexpr size_t kDecompressBufferSize = 2 * 1024 * 1024;
constexpr size_t kMaxVoicePeers = 4095;
constexpr int kVoiceSocketBufferBytes = 8 * 1024 * 1024;

/** @brief ENet peer slot index on the external voice UDP server (0 .. kMaxPeers - 1). */
using PeerId = uint16_t;

/** @brief Maximum number of concurrent ENet peer slots (0 .. 4095). */
inline constexpr size_t kMaxPeers = 4096;

constexpr uint8_t kControlPacketMarker = 0xFF;
constexpr uint16_t kVoiceAuthPacketId = 7;
constexpr uint16_t kVoiceHandshakeDonePacketId = 8;

enum class VoicePacketId : uint8_t {
    ClientVoice        = 24, // ClientVoicePacket (Game Client -> Voice Server)
    ServerVoice        = 25, // ServerVoicePacket (Voice Server -> Target Game Clients)
    VoiceStreamingSync = 26  // VoiceStreamingSyncPacket (FXServer Gateway -> Voice Server)
};

constexpr uint8_t kVoiceClientAudioPacketId   = static_cast<uint8_t>(VoicePacketId::ClientVoice);
constexpr uint8_t kVoiceServerAudioPacketId   = static_cast<uint8_t>(VoicePacketId::ServerVoice);
constexpr uint8_t kVoiceStreamingSyncPacketId = static_cast<uint8_t>(VoicePacketId::VoiceStreamingSync);

#pragma pack(push, 1)
struct VoiceAuthPacket {
    uint8_t marker;          // kControlPacketMarker
    uint16_t packetId;       // kVoiceAuthPacketId (LE)
    uint16_t clientIdOrZero; // 0 for Gateway Auth; player clientId (LE) for Game Client Auth
    uint8_t secret[32];      // SHA256(sv_licenseKey) for Gateway; player token[32] for Game Client
};

struct VoiceHandshakeDonePacket {
    uint8_t marker;          // kControlPacketMarker
    uint16_t packetId;       // kVoiceHandshakeDonePacketId (LE)
};

struct FastPathVoicePrefix {
    uint16_t peerId;         // ENet peer slot index on Voice Server
    uint8_t tokenPrefix[16]; // First 16 bytes of player's 32-byte token
    uint8_t packetId;        // VoicePacketId::ClientVoice on ingress, rewritten to VoicePacketId::ServerVoice on egress
};

struct ServerVoicePacketHeader {
    uint8_t packetId;        // VoicePacketId::ServerVoice
    uint16_t senderNetId;    // Speaker or synthetic NPC NetID (LE)
    uint32_t sequenceNumber; // Monotonic per-speaker counter (LE)
    uint8_t routingMode;     // 1 = Spatial3D, 2 = NonSpatial, 3 = Both
};
#pragma pack(pop)

static_assert(sizeof(VoiceAuthPacket) == 37, "VoiceAuthPacket must be 37 bytes");
static_assert(sizeof(VoiceHandshakeDonePacket) == 3, "VoiceHandshakeDonePacket must be 3 bytes");
static_assert(sizeof(FastPathVoicePrefix) == 19, "FastPathVoicePrefix must be 19 bytes");
static_assert(sizeof(ServerVoicePacketHeader) == 8, "ServerVoicePacketHeader must be 8 bytes");

} // namespace eevs::protocol
