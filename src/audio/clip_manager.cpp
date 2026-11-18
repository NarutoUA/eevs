#include "audio/clip_manager.h"
#include "audio/opus_codec_pool.h"
#include "core/logger.h"

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace eevs {

ClipManager::ClipManager(OpusCodecPool& codecs)
    : m_codecs(codecs) {
    (void)m_codecs;
}

void ClipManager::SetMaxTransientBytes(size_t maxBytes) noexcept {
    std::scoped_lock lock(m_mutex);
    m_maxTransientBytes = maxBytes > 0 ? maxBytes : kDefaultMaxTransientBytes;
    EvictLruTransientClipsLocked(0);
}

size_t ClipManager::GetMaxTransientBytes() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_maxTransientBytes;
}

size_t ClipManager::GetCurrentTransientBytes() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_currentTransientBytes;
}

size_t ClipManager::GetTotalClipBytes() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_totalClipBytes;
}

std::string ClipManager::GenerateClipId() {
    return std::format("clip_{:06x}", m_nextClipCounter++);
}

size_t ClipManager::ComputeFramesByteSize(std::span<const std::vector<uint8_t>> opusFrames) noexcept {
    size_t total = 0;
    for (const auto& f : opusFrames) {
        total += sizeof(std::vector<uint8_t>) + f.size();
    }
    return total;
}

void ClipManager::EvictLruTransientClipsLocked(size_t incomingBytes) {
    while (!m_clips.empty() && (m_currentTransientBytes + incomingBytes) > m_maxTransientBytes) {
        auto oldestIt = m_clips.end();
        uint64_t oldestSeq = UINT64_MAX;

        for (auto it = m_clips.begin(); it != m_clips.end(); ++it) {
            if (!it->second.pinned && it->second.lastAccessSeq < oldestSeq) {
                oldestSeq = it->second.lastAccessSeq;
                oldestIt = it;
            }
        }

        if (oldestIt == m_clips.end()) {
            break;
        }

        const size_t freedBytes = oldestIt->second.byteSize;
        const std::string evictedId = oldestIt->first;
        m_currentTransientBytes = (m_currentTransientBytes >= freedBytes) ? (m_currentTransientBytes - freedBytes) : 0;
        m_totalClipBytes = (m_totalClipBytes >= freedBytes) ? (m_totalClipBytes - freedBytes) : 0;
        m_clips.erase(oldestIt);

        EEVS_LOG_DEBUG("[ClipManager] Evicted LRU transient clip '{}' ({} bytes freed)", evictedId, freedBytes);
    }
}

std::vector<std::vector<uint8_t>> ClipManager::EncodePcmToOpusFrames(
    std::span<const float> pcmSamples,
    uint32_t sampleRate
) {
    if (pcmSamples.empty() || sampleRate == 0) {
        return {};
    }

    std::vector<float> resampled;
    std::span<const float> samples48k = pcmSamples;

    if (sampleRate != OpusCodecPool::kSampleRate) {
        const double ratio = static_cast<double>(OpusCodecPool::kSampleRate) / static_cast<double>(sampleRate);
        const size_t targetCount = static_cast<size_t>(static_cast<double>(pcmSamples.size()) * ratio);
        if (targetCount == 0) {
            return {};
        }
        resampled.resize(targetCount);
        for (size_t i = 0; i < targetCount; ++i) {
            const double srcPos = static_cast<double>(i) / ratio;
            const size_t idx0 = std::min(static_cast<size_t>(srcPos), pcmSamples.size() - 1);
            const size_t idx1 = std::min(idx0 + 1, pcmSamples.size() - 1);
            const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));
            resampled[i] = pcmSamples[idx0] + frac * (pcmSamples[idx1] - pcmSamples[idx0]);
        }
        samples48k = resampled;
    }

    int opusErr = OPUS_OK;
    OpusEncoder* rawEnc = opus_encoder_create(
        static_cast<opus_int32>(OpusCodecPool::kSampleRate),
        OpusCodecPool::kChannels,
        OPUS_APPLICATION_VOIP,
        &opusErr
    );
    if (opusErr != OPUS_OK || !rawEnc) {
        return {};
    }

    opus_encoder_ctl(rawEnc, OPUS_SET_BITRATE(OpusCodecPool::kDefaultBitrate));
    opus_encoder_ctl(rawEnc, OPUS_SET_COMPLEXITY(OpusCodecPool::kDefaultComplexity));
    opus_encoder_ctl(rawEnc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    const size_t frameLen = OpusCodecPool::kFrameSamples10ms;
    const size_t totalFrames = (samples48k.size() + frameLen - 1) / frameLen;

    std::vector<std::vector<uint8_t>> encodedFrames;
    encodedFrames.reserve(totalFrames);

    std::array<float, OpusCodecPool::kFrameSamples10ms> paddedFrame{};
    std::array<uint8_t, OpusCodecPool::kMaxOpusPacketBytes> opusScratch{};

    for (size_t f = 0; f < totalFrames; ++f) {
        const size_t offset = f * frameLen;
        const size_t remaining = samples48k.size() - offset;
        const float* framePtr = nullptr;

        if (remaining >= frameLen) {
            framePtr = samples48k.data() + offset;
        } else {
            paddedFrame.fill(0.0f);
            std::copy_n(samples48k.data() + offset, remaining, paddedFrame.data());
            framePtr = paddedFrame.data();
        }

        const int encodedBytes = opus_encode_float(
            rawEnc,
            framePtr,
            static_cast<int>(frameLen),
            opusScratch.data(),
            static_cast<opus_int32>(opusScratch.size())
        );
        if (encodedBytes > 0) {
            encodedFrames.emplace_back(opusScratch.data(), opusScratch.data() + encodedBytes);
        }
    }

    opus_encoder_destroy(rawEnc);
    return encodedFrames;
}

std::string ClipManager::StoreOpusClip(
    std::string_view requestedClipId,
    std::vector<std::vector<uint8_t>> opusFrames,
    bool pinned
) {
    if (opusFrames.empty()) {
        return {};
    }

    const size_t newByteSize = ComputeFramesByteSize(opusFrames);
    const uint32_t durationMs = static_cast<uint32_t>(opusFrames.size() * 10);

    std::scoped_lock lock(m_mutex);
    std::string finalId = requestedClipId.empty()
        ? GenerateClipId()
        : std::string(requestedClipId);

    if (auto existingIt = m_clips.find(finalId); existingIt != m_clips.end()) {
        const size_t oldBytes = existingIt->second.byteSize;
        m_totalClipBytes = (m_totalClipBytes >= oldBytes) ? (m_totalClipBytes - oldBytes) : 0;
        if (!existingIt->second.pinned) {
            m_currentTransientBytes = (m_currentTransientBytes >= oldBytes) ? (m_currentTransientBytes - oldBytes) : 0;
        }
        m_clips.erase(existingIt);
    }

    if (!pinned) {
        EvictLruTransientClipsLocked(newByteSize);
    }

    m_totalClipBytes += newByteSize;
    if (!pinned) {
        m_currentTransientBytes += newByteSize;
    }

    m_clips[finalId] = AudioClip{
        .clipId = finalId,
        .durationMs = durationMs,
        .byteSize = newByteSize,
        .lastAccessSeq = m_lruClock++,
        .pinned = pinned,
        .opusFrames = std::move(opusFrames)
    };

    return finalId;
}

std::string ClipManager::StorePcmClip(
    std::string_view requestedClipId,
    std::span<const float> pcmSamples,
    uint32_t sampleRate,
    uint32_t* outDurationMs,
    bool pinned
) {
    std::vector<std::vector<uint8_t>> encodedFrames = EncodePcmToOpusFrames(pcmSamples, sampleRate);
    if (encodedFrames.empty()) {
        return {};
    }

    if (outDurationMs) {
        *outDurationMs = static_cast<uint32_t>(encodedFrames.size() * 10);
    }

    return StoreOpusClip(requestedClipId, std::move(encodedFrames), pinned);
}

bool ClipManager::SetClipPinned(std::string_view clipId, bool pinned) {
    std::scoped_lock lock(m_mutex);
    auto it = m_clips.find(std::string(clipId));
    if (it == m_clips.end()) {
        return false;
    }

    if (it->second.pinned != pinned) {
        if (pinned) {
            m_currentTransientBytes = (m_currentTransientBytes >= it->second.byteSize)
                ? (m_currentTransientBytes - it->second.byteSize)
                : 0;
        } else {
            m_currentTransientBytes += it->second.byteSize;
        }
        it->second.pinned = pinned;
        if (!pinned) {
            EvictLruTransientClipsLocked(0);
        }
    }
    return true;
}

bool ClipManager::DeleteClip(std::string_view clipId) {
    std::scoped_lock lock(m_mutex);
    auto it = m_clips.find(std::string(clipId));
    if (it == m_clips.end()) {
        return false;
    }

    const size_t freedBytes = it->second.byteSize;
    m_totalClipBytes = (m_totalClipBytes >= freedBytes) ? (m_totalClipBytes - freedBytes) : 0;
    if (!it->second.pinned) {
        m_currentTransientBytes = (m_currentTransientBytes >= freedBytes) ? (m_currentTransientBytes - freedBytes) : 0;
    }
    m_clips.erase(it);
    return true;
}

void ClipManager::ClearAllClips() {
    std::scoped_lock lock(m_mutex);
    m_clips.clear();
    m_currentTransientBytes = 0;
    m_totalClipBytes = 0;
}

const AudioClip* ClipManager::FindClip(std::string_view clipId) const noexcept {
    std::scoped_lock lock(m_mutex);
    auto it = m_clips.find(std::string(clipId));
    if (it != m_clips.end()) {
        it->second.lastAccessSeq = m_lruClock++;
        return &it->second;
    }
    return nullptr;
}

bool ClipManager::HasClip(std::string_view clipId) const noexcept {
    return FindClip(clipId) != nullptr;
}

uint32_t ClipManager::GetClipDurationMs(std::string_view clipId) const noexcept {
    std::scoped_lock lock(m_mutex);
    auto it = m_clips.find(std::string(clipId));
    return it != m_clips.end() ? it->second.durationMs : 0;
}

} // namespace eevs
