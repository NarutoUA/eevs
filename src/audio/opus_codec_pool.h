#pragma once

/**
 * @file opus_codec_pool.h
 * @brief Lazily allocated per-speaker `OpusDecoder` and per-branch `OpusEncoder` pool (`48kHz` mono VoIP).
 */

#include "core/containers.h"
#include "eevs/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace eevs {

class OpusCodecPool {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr int kChannels = 1;
    static constexpr size_t kFrameSamples10ms = 480;   // 10ms @ 48kHz
    static constexpr size_t kMaxFrameSamples = 2880;   // 60ms @ 48kHz max Opus frame
    static constexpr size_t kMaxOpusPacketBytes = 1200;
    static constexpr int kDefaultBitrate = 48000;
    static constexpr int kDefaultComplexity = 5;

    OpusCodecPool();
    ~OpusCodecPool() = default;

    OpusCodecPool(const OpusCodecPool&) = delete;
    OpusCodecPool& operator=(const OpusCodecPool&) = delete;

    void ConfigureEncoderSettings(int bitrate, int complexity) noexcept {
        m_bitrate = bitrate;
        m_complexity = complexity;
    }

    [[nodiscard]] static constexpr uint32_t MakeEncoderKey(uint16_t speakerId, RoutingMode branchMode) noexcept {
        return (static_cast<uint32_t>(speakerId) << 8u) | static_cast<uint32_t>(branchMode);
    }

    // Decodes an Opus packet into mono float PCM [-1.0f, 1.0f].
    // Lazily creates a per-speaker OpusDecoder on first use.
    // Returns number of decoded samples (> 0), or negative Opus error code.
    int DecodeFloat(
        uint16_t speakerId,
        std::span<const uint8_t> opusData,
        std::span<float> outPcm,
        bool decodeFec = false
    );

    // Encodes mono float PCM [-1.0f, 1.0f] (e.g. 480 samples for 10ms) into Opus bytes.
    // Lazily creates a stateful OpusEncoder for `encoderKey` on first use.
    // Returns number of encoded bytes (> 0), or negative Opus error code.
    int EncodeFloat(
        uint32_t encoderKey,
        std::span<const float> inPcm,
        std::span<uint8_t> outOpus
    );

    // Resets decoder and branch encoder states (`OPUS_RESET_STATE`) when a talk burst ends.
    void ResetSpeakerState(uint16_t speakerId) noexcept;

    // Destroys decoder and branch encoders when a client unregisters.
    void ReleaseSpeaker(uint16_t speakerId) noexcept;

    // Destroys synthetic encoder associated with a NetId when that NetId unregisters.
    void ReleaseSyntheticNetId(NetId senderNetId) noexcept;

private:
    struct DecoderDeleter {
        void operator()(OpusDecoder* dec) const noexcept;
    };

    struct EncoderDeleter {
        void operator()(OpusEncoder* enc) const noexcept;
    };

    using UniqueDecoder = std::unique_ptr<OpusDecoder, DecoderDeleter>;
    using UniqueEncoder = std::unique_ptr<OpusEncoder, EncoderDeleter>;

    [[nodiscard]] OpusDecoder* GetOrCreateDecoder(uint16_t speakerId);
    [[nodiscard]] OpusEncoder* GetOrCreateEncoder(uint32_t encoderKey);
    [[nodiscard]] UniqueEncoder CreateConfiguredEncoder() const;

    int m_bitrate = kDefaultBitrate;
    int m_complexity = kDefaultComplexity;

    std::vector<UniqueDecoder> m_clientDecoders;
    std::array<std::vector<UniqueEncoder>, 4> m_clientEncodersByMode;

    // Fallback maps for synthetic / non-player NetIds (>= kMaxClients or tag 0xFE)
    FlatMap<uint16_t, UniqueDecoder> m_extraDecoders;
    FlatMap<uint32_t, UniqueEncoder> m_extraEncoders;
};

} // namespace eevs
