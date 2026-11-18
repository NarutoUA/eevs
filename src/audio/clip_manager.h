#pragma once

#include "core/containers.h"
#include "eevs/types.h"
#include "network/protocol.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eevs {

class OpusCodecPool;

struct AudioClip {
    std::string clipId;
    uint32_t durationMs = 0;
    size_t byteSize = 0;               // Total RAM bytes of opusFrames
    mutable uint64_t lastAccessSeq = 0; // Monotonic LRU access counter
    bool pinned = false;               // If true, never evicted by LRU byte budget
    std::vector<std::vector<uint8_t>> opusFrames; // 10ms Opus frames @ 48kHz
};

class ClipManager {
public:
    static constexpr size_t kDefaultMaxTransientBytes = 64 * 1024 * 1024; // 64 MB default LRU cache budget

    explicit ClipManager(OpusCodecPool& codecs);
    ~ClipManager() = default;

    ClipManager(const ClipManager&) = delete;
    ClipManager& operator=(const ClipManager&) = delete;

    // Configures the maximum RAM budget (in bytes) for unpinned transient clips
    void SetMaxTransientBytes(size_t maxBytes) noexcept;
    [[nodiscard]] size_t GetMaxTransientBytes() const noexcept;
    [[nodiscard]] size_t GetCurrentTransientBytes() const noexcept;
    [[nodiscard]] size_t GetTotalClipBytes() const noexcept;

    // Resamples (if needed) and encodes mono float PCM into 10ms 48kHz Opus frames
    [[nodiscard]] static std::vector<std::vector<uint8_t>> EncodePcmToOpusFrames(
        std::span<const float> pcmSamples,
        uint32_t sampleRate = 48000
    );

    // Stores pre-encoded 10ms Opus frames under `requestedClipId` (or generates an ID if empty)
    std::string StoreOpusClip(
        std::string_view requestedClipId,
        std::vector<std::vector<uint8_t>> opusFrames,
        bool pinned = false
    );

    // Resamples (if needed) and encodes mono float PCM into 10ms 48kHz Opus frames
    std::string StorePcmClip(
        std::string_view requestedClipId,
        std::span<const float> pcmSamples,
        uint32_t sampleRate = 48000,
        uint32_t* outDurationMs = nullptr,
        bool pinned = false
    );

    bool SetClipPinned(std::string_view clipId, bool pinned);
    bool DeleteClip(std::string_view clipId);
    void ClearAllClips();

    [[nodiscard]] const AudioClip* FindClip(std::string_view clipId) const noexcept;
    [[nodiscard]] bool HasClip(std::string_view clipId) const noexcept;
    [[nodiscard]] uint32_t GetClipDurationMs(std::string_view clipId) const noexcept;

    [[nodiscard]] const FlatMap<std::string, AudioClip>& GetAllClips() const noexcept {
        return m_clips;
    }

private:
    std::string GenerateClipId();
    static size_t ComputeFramesByteSize(std::span<const std::vector<uint8_t>> opusFrames) noexcept;
    void EvictLruTransientClipsLocked(size_t incomingBytes);

    OpusCodecPool& m_codecs;
    mutable std::mutex m_mutex;
    uint64_t m_nextClipCounter = 1;
    mutable uint64_t m_lruClock = 1;
    size_t m_maxTransientBytes = kDefaultMaxTransientBytes;
    size_t m_currentTransientBytes = 0;
    size_t m_totalClipBytes = 0;
    FlatMap<std::string, AudioClip> m_clips;
};

} // namespace eevs
