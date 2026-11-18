#include "audio/opus_codec_pool.h"

#include <opus/opus.h>

namespace eevs {

OpusCodecPool::OpusCodecPool()
    : m_clientDecoders(kMaxClients) {
    for (auto& vec : m_clientEncodersByMode) {
        vec.resize(kMaxClients);
    }
}

void OpusCodecPool::DecoderDeleter::operator()(OpusDecoder* dec) const noexcept {
    if (dec) {
        opus_decoder_destroy(dec);
    }
}

void OpusCodecPool::EncoderDeleter::operator()(OpusEncoder* enc) const noexcept {
    if (enc) {
        opus_encoder_destroy(enc);
    }
}

OpusDecoder* OpusCodecPool::GetOrCreateDecoder(uint16_t speakerId) {
    if (speakerId < kMaxClients) {
        UniqueDecoder& slot = m_clientDecoders[speakerId];
        if (slot) {
            return slot.get();
        }
        int err = OPUS_OK;
        OpusDecoder* rawDec = opus_decoder_create(static_cast<opus_int32>(kSampleRate), kChannels, &err);
        if (err != OPUS_OK || !rawDec) {
            return nullptr;
        }
        slot.reset(rawDec);
        return slot.get();
    }

    auto it = m_extraDecoders.find(speakerId);
    if (it != m_extraDecoders.end()) {
        return it->second.get();
    }

    int err = OPUS_OK;
    OpusDecoder* rawDec = opus_decoder_create(static_cast<opus_int32>(kSampleRate), kChannels, &err);
    if (err != OPUS_OK || !rawDec) {
        return nullptr;
    }

    auto [insertedIt, _] = m_extraDecoders.emplace(speakerId, UniqueDecoder(rawDec));
    return insertedIt->second.get();
}

OpusCodecPool::UniqueEncoder OpusCodecPool::CreateConfiguredEncoder() const {
    int err = OPUS_OK;
    OpusEncoder* rawEnc = opus_encoder_create(
        static_cast<opus_int32>(kSampleRate),
        kChannels,
        OPUS_APPLICATION_VOIP,
        &err
    );
    if (err != OPUS_OK || !rawEnc) {
        return nullptr;
    }

    opus_encoder_ctl(rawEnc, OPUS_SET_BITRATE(m_bitrate));
    opus_encoder_ctl(rawEnc, OPUS_SET_COMPLEXITY(m_complexity));
    opus_encoder_ctl(rawEnc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(rawEnc, OPUS_SET_VBR(1));
    opus_encoder_ctl(rawEnc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(rawEnc, OPUS_SET_PACKET_LOSS_PERC(5));
    return UniqueEncoder(rawEnc);
}

OpusEncoder* OpusCodecPool::GetOrCreateEncoder(uint32_t encoderKey) {
    const uint16_t speakerId = static_cast<uint16_t>((encoderKey >> 8u) & 0xFFFFu);
    const uint8_t branchTag = static_cast<uint8_t>(encoderKey & 0xFFu);

    if (speakerId < kMaxClients && branchTag < 4u) {
        UniqueEncoder& slot = m_clientEncodersByMode[branchTag][speakerId];
        if (!slot) {
            slot = CreateConfiguredEncoder();
        }
        return slot.get();
    }

    auto it = m_extraEncoders.find(encoderKey);
    if (it != m_extraEncoders.end()) {
        return it->second.get();
    }

    UniqueEncoder enc = CreateConfiguredEncoder();
    if (!enc) {
        return nullptr;
    }

    auto [insertedIt, _] = m_extraEncoders.emplace(encoderKey, std::move(enc));
    return insertedIt->second.get();
}

int OpusCodecPool::DecodeFloat(
    uint16_t speakerId,
    std::span<const uint8_t> opusData,
    std::span<float> outPcm,
    bool decodeFec
) {
    if (opusData.empty() || outPcm.empty()) {
        return OPUS_BAD_ARG;
    }

    OpusDecoder* decoder = GetOrCreateDecoder(speakerId);
    if (!decoder) {
        return OPUS_ALLOC_FAIL;
    }

    return opus_decode_float(
        decoder,
        opusData.data(),
        static_cast<opus_int32>(opusData.size()),
        outPcm.data(),
        static_cast<int>(outPcm.size()),
        decodeFec ? 1 : 0
    );
}

int OpusCodecPool::EncodeFloat(
    uint32_t encoderKey,
    std::span<const float> inPcm,
    std::span<uint8_t> outOpus
) {
    if (inPcm.empty() || outOpus.empty()) {
        return OPUS_BAD_ARG;
    }

    OpusEncoder* encoder = GetOrCreateEncoder(encoderKey);
    if (!encoder) {
        return OPUS_ALLOC_FAIL;
    }

    return opus_encode_float(
        encoder,
        inPcm.data(),
        static_cast<int>(inPcm.size()),
        outOpus.data(),
        static_cast<opus_int32>(outOpus.size())
    );
}

void OpusCodecPool::ResetSpeakerState(uint16_t speakerId) noexcept {
    if (speakerId < kMaxClients) {
        if (m_clientDecoders[speakerId]) {
            opus_decoder_ctl(m_clientDecoders[speakerId].get(), OPUS_RESET_STATE);
        }
        for (uint8_t mode = 0; mode < 4; ++mode) {
            if (m_clientEncodersByMode[mode][speakerId]) {
                opus_encoder_ctl(m_clientEncodersByMode[mode][speakerId].get(), OPUS_RESET_STATE);
            }
        }
        return;
    }

    if (auto decIt = m_extraDecoders.find(speakerId); decIt != m_extraDecoders.end() && decIt->second) {
        opus_decoder_ctl(decIt->second.get(), OPUS_RESET_STATE);
    }

    for (uint8_t mode = 0; mode <= 3; ++mode) {
        uint32_t encKey = MakeEncoderKey(speakerId, static_cast<RoutingMode>(mode));
        if (auto encIt = m_extraEncoders.find(encKey); encIt != m_extraEncoders.end() && encIt->second) {
            opus_encoder_ctl(encIt->second.get(), OPUS_RESET_STATE);
        }
    }
}

void OpusCodecPool::ReleaseSpeaker(uint16_t speakerId) noexcept {
    if (speakerId < kMaxClients) {
        m_clientDecoders[speakerId].reset();
        for (uint8_t mode = 0; mode < 4; ++mode) {
            m_clientEncodersByMode[mode][speakerId].reset();
        }
        return;
    }

    m_extraDecoders.erase(speakerId);
    for (uint8_t mode = 0; mode <= 3; ++mode) {
        m_extraEncoders.erase(MakeEncoderKey(speakerId, static_cast<RoutingMode>(mode)));
    }
}

void OpusCodecPool::ReleaseSyntheticNetId(NetId senderNetId) noexcept {
    constexpr uint8_t kSyntheticEncoderTag = 0xFEu;
    const uint32_t encoderKey = (static_cast<uint32_t>(senderNetId) << 8u) | kSyntheticEncoderTag;
    m_extraEncoders.erase(encoderKey);
}

} // namespace eevs
