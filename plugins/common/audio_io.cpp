#include "audio_io.h"

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

namespace eevs::plugins::common {

namespace {

constexpr uint32_t kOpusSampleRate = 48000;
constexpr int kOpusChannels = 1;
constexpr size_t kOpusFrameSamples10ms = 480;
constexpr size_t kMaxOpusPacketBytes = 1272;
constexpr int kDefaultOpusBitrate = 48000;
constexpr int kDefaultOpusComplexity = 5;

uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

} // namespace

std::vector<float> ResampleMonoLinear(
    std::span<const float> inputSamples,
    uint32_t inputSampleRate,
    uint32_t outputSampleRate
) {
    if (inputSamples.empty() || inputSampleRate == 0 || outputSampleRate == 0) {
        return {};
    }
    if (inputSampleRate == outputSampleRate) {
        return std::vector<float>(inputSamples.begin(), inputSamples.end());
    }

    const double ratio = static_cast<double>(outputSampleRate) / static_cast<double>(inputSampleRate);
    const size_t targetCount = static_cast<size_t>(static_cast<double>(inputSamples.size()) * ratio);
    if (targetCount == 0) {
        return {};
    }

    std::vector<float> resampled(targetCount);
    for (size_t i = 0; i < targetCount; ++i) {
        const double srcPos = static_cast<double>(i) / ratio;
        const size_t idx0 = std::min(static_cast<size_t>(srcPos), inputSamples.size() - 1);
        const size_t idx1 = std::min(idx0 + 1, inputSamples.size() - 1);
        const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));
        resampled[i] = inputSamples[idx0] + frac * (inputSamples[idx1] - inputSamples[idx0]);
    }
    return resampled;
}

void Downsample48kTo16kAppend(
    const float* samples,
    size_t sampleCount,
    uint32_t sampleRate,
    std::vector<float>& outSamples16k,
    size_t maxSamples16k
) {
    if (!samples || sampleCount == 0 || sampleRate == 0 || outSamples16k.size() >= maxSamples16k) {
        return;
    }

    if (sampleRate == 48000) {
        for (size_t i = 0; i + 2 < sampleCount && outSamples16k.size() < maxSamples16k; i += 3) {
            const float avg = (samples[i] + samples[i + 1] + samples[i + 2]) * (1.0f / 3.0f);
            outSamples16k.push_back(avg);
        }
    } else {
        const double ratio = 16000.0 / static_cast<double>(sampleRate);
        const size_t outCount = static_cast<size_t>(static_cast<double>(sampleCount) * ratio);
        for (size_t i = 0; i < outCount && outSamples16k.size() < maxSamples16k; ++i) {
            const size_t srcIdx = std::min(static_cast<size_t>(static_cast<double>(i) / ratio), sampleCount - 1);
            outSamples16k.push_back(samples[srcIdx]);
        }
    }
}

std::vector<std::vector<uint8_t>> EncodePcmToOpusFrames(
    std::span<const float> pcmSamples,
    uint32_t sampleRate
) {
    if (pcmSamples.empty() || sampleRate == 0) {
        return {};
    }

    std::vector<float> resampled;
    std::span<const float> samples48k = pcmSamples;
    if (sampleRate != kOpusSampleRate) {
        resampled = ResampleMonoLinear(pcmSamples, sampleRate, kOpusSampleRate);
        if (resampled.empty()) {
            return {};
        }
        samples48k = resampled;
    }

    int opusErr = OPUS_OK;
    OpusEncoder* rawEnc = opus_encoder_create(
        static_cast<opus_int32>(kOpusSampleRate),
        kOpusChannels,
        OPUS_APPLICATION_VOIP,
        &opusErr
    );
    if (opusErr != OPUS_OK || !rawEnc) {
        return {};
    }

    opus_encoder_ctl(rawEnc, OPUS_SET_BITRATE(kDefaultOpusBitrate));
    opus_encoder_ctl(rawEnc, OPUS_SET_COMPLEXITY(kDefaultOpusComplexity));
    opus_encoder_ctl(rawEnc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    const size_t totalFrames = (samples48k.size() + kOpusFrameSamples10ms - 1) / kOpusFrameSamples10ms;
    std::vector<std::vector<uint8_t>> encodedFrames;
    encodedFrames.reserve(totalFrames);

    std::array<float, kOpusFrameSamples10ms> paddedFrame{};
    std::array<uint8_t, kMaxOpusPacketBytes> opusScratch{};

    for (size_t f = 0; f < totalFrames; ++f) {
        const size_t offset = f * kOpusFrameSamples10ms;
        const size_t remaining = samples48k.size() - offset;
        const float* framePtr = nullptr;

        if (remaining >= kOpusFrameSamples10ms) {
            framePtr = samples48k.data() + offset;
        } else {
            paddedFrame.fill(0.0f);
            std::copy_n(samples48k.data() + offset, remaining, paddedFrame.data());
            framePtr = paddedFrame.data();
        }

        const int encodedBytes = opus_encode_float(
            rawEnc,
            framePtr,
            static_cast<int>(kOpusFrameSamples10ms),
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

std::vector<float> DecodeOpusFramesToPcm(
    std::span<const std::vector<uint8_t>> opusFrames
) {
    if (opusFrames.empty()) {
        return {};
    }

    int opusErr = OPUS_OK;
    OpusDecoder* rawDec = opus_decoder_create(
        static_cast<opus_int32>(kOpusSampleRate),
        kOpusChannels,
        &opusErr
    );
    if (opusErr != OPUS_OK || !rawDec) {
        return {};
    }

    std::vector<float> pcmOut;
    pcmOut.reserve(opusFrames.size() * kOpusFrameSamples10ms);

    std::array<float, 5760> decodeScratch{};

    for (const auto& frame : opusFrames) {
        if (frame.empty()) {
            pcmOut.insert(pcmOut.end(), kOpusFrameSamples10ms, 0.0f);
            continue;
        }

        const int decodedSamples = opus_decode_float(
            rawDec,
            frame.data(),
            static_cast<opus_int32>(frame.size()),
            decodeScratch.data(),
            static_cast<int>(decodeScratch.size()),
            0
        );

        if (decodedSamples > 0) {
            pcmOut.insert(pcmOut.end(), decodeScratch.data(), decodeScratch.data() + decodedSamples);
        } else {
            const int plcSamples = opus_decode_float(
                rawDec,
                nullptr,
                0,
                decodeScratch.data(),
                static_cast<int>(kOpusFrameSamples10ms),
                0
            );
            if (plcSamples > 0) {
                pcmOut.insert(pcmOut.end(), decodeScratch.data(), decodeScratch.data() + plcSamples);
            } else {
                pcmOut.insert(pcmOut.end(), kOpusFrameSamples10ms, 0.0f);
            }
        }
    }

    opus_decoder_destroy(rawDec);
    return pcmOut;
}

bool ExtractFloatPcmFromWavMemory(
    const void* wavBytes,
    size_t wavLen,
    uint32_t fallbackRate,
    std::vector<float>& outMonoPcm,
    uint32_t& outSampleRate,
    std::string* outError
) {
    if (!wavBytes || wavLen < 44) {
        if (outError) {
            *outError = "WAV buffer is smaller than 44 bytes";
        }
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(wavBytes);
    if (std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        if (outError) {
            *outError = "Invalid RIFF/WAVE header";
        }
        return false;
    }

    uint16_t audioFormat = 1;
    uint16_t numChannels = 1;
    uint32_t sampleRate = fallbackRate > 0 ? fallbackRate : 24000u;
    uint16_t bitsPerSample = 16;
    const uint8_t* dataPtr = nullptr;
    size_t dataSize = 0;

    size_t offset = 12;
    while (offset + 8 <= wavLen) {
        const uint8_t* chunkHdr = bytes + offset;
        const uint32_t chunkLen = ReadLe32(chunkHdr + 4);
        const size_t chunkDataOffset = offset + 8;

        if (std::memcmp(chunkHdr, "fmt ", 4) == 0 && chunkDataOffset + 16 <= wavLen) {
            audioFormat = ReadLe16(bytes + chunkDataOffset);
            numChannels = std::max<uint16_t>(1, ReadLe16(bytes + chunkDataOffset + 2));
            sampleRate = ReadLe32(bytes + chunkDataOffset + 4);
            bitsPerSample = ReadLe16(bytes + chunkDataOffset + 14);
            if (audioFormat == 0xFFFEu && chunkLen >= 40 && chunkDataOffset + 26 <= wavLen) {
                audioFormat = ReadLe16(bytes + chunkDataOffset + 24);
            }
        } else if (std::memcmp(chunkHdr, "data", 4) == 0) {
            dataPtr = bytes + chunkDataOffset;
            const size_t avail = wavLen - chunkDataOffset;
            dataSize = (chunkLen == 0xFFFFFFFFu || chunkLen > avail) ? avail : static_cast<size_t>(chunkLen);
            break;
        }

        if (chunkDataOffset + chunkLen > wavLen) {
            break;
        }
        offset = chunkDataOffset + chunkLen + (chunkLen & 1u);
    }

    if (!dataPtr || dataSize == 0 || numChannels == 0) {
        if (outError) {
            *outError = "WAV buffer is missing valid 'fmt ' or 'data' chunk";
        }
        return false;
    }

    if ((audioFormat != 1 && audioFormat != 3) ||
        (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)) {
        if (outError) {
            *outError = std::format(
                "Unsupported WAV encoding (format={}, bitsPerSample={})",
                audioFormat,
                bitsPerSample
            );
        }
        return false;
    }

    const size_t bytesPerSample = bitsPerSample / 8u;
    const size_t bytesPerFrame = bytesPerSample * numChannels;
    const size_t frameCount = dataSize / bytesPerFrame;
    if (frameCount == 0) {
        if (outError) {
            *outError = "WAV buffer contains 0 audio frames";
        }
        return false;
    }

    outSampleRate = sampleRate > 0 ? sampleRate : 24000u;
    outMonoPcm.resize(frameCount);

    for (size_t i = 0; i < frameCount; ++i) {
        const uint8_t* frameBase = dataPtr + i * bytesPerFrame;
        float monoAccum = 0.0f;

        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            const uint8_t* sPtr = frameBase + ch * bytesPerSample;
            float sampleVal = 0.0f;

            if (audioFormat == 3 && bitsPerSample == 32) {
                std::memcpy(&sampleVal, sPtr, sizeof(float));
            } else if (audioFormat == 1 && bitsPerSample == 16) {
                const auto v = static_cast<int16_t>(ReadLe16(sPtr));
                sampleVal = static_cast<float>(v) / 32768.0f;
            } else if (audioFormat == 1 && bitsPerSample == 24) {
                const int32_t v = static_cast<int32_t>(sPtr[0]) |
                                  (static_cast<int32_t>(sPtr[1]) << 8) |
                                  (static_cast<int32_t>(static_cast<int8_t>(sPtr[2])) << 16);
                sampleVal = static_cast<float>(v) / 8388608.0f;
            } else if (audioFormat == 1 && bitsPerSample == 32) {
                const auto v = static_cast<int32_t>(ReadLe32(sPtr));
                sampleVal = static_cast<float>(static_cast<double>(v) / 2147483648.0);
            } else if (audioFormat == 1 && bitsPerSample == 8) {
                sampleVal = (static_cast<float>(sPtr[0]) - 128.0f) / 128.0f;
            }

            monoAccum += sampleVal;
        }

        outMonoPcm[i] = std::clamp(monoAccum / static_cast<float>(numChannels), -1.0f, 1.0f);
    }

    return true;
}

bool ReadWavFileToMonoFloat(
    const std::filesystem::path& wavPath,
    std::vector<float>& outMonoPcm,
    uint32_t& outSampleRate,
    std::string* outError
) {
    std::ifstream file(wavPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (outError) {
            *outError = "Could not open WAV file: " + wavPath.string();
        }
        return false;
    }

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < 44) {
        if (outError) {
            *outError = "File is too small to be a valid WAV file: " + wavPath.string();
        }
        return false;
    }

    std::vector<uint8_t> bytes(fileSize);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fileSize))) {
        if (outError) {
            *outError = "Failed to read WAV file: " + wavPath.string();
        }
        return false;
    }

    return ExtractFloatPcmFromWavMemory(bytes.data(), bytes.size(), 48000, outMonoPcm, outSampleRate, outError);
}

std::vector<uint8_t> EncodeMonoFloatToWavMemory(
    std::span<const float> monoPcm,
    uint32_t sampleRate
) {
    if (monoPcm.empty() || sampleRate == 0) {
        return {};
    }

    constexpr uint16_t kAudioFormatPcm = 1;
    constexpr uint16_t kNumChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kNumChannels * (kBitsPerSample / 8u);
    const uint32_t byteRate = sampleRate * kBlockAlign;
    const uint32_t dataChunkSize = static_cast<uint32_t>(monoPcm.size() * sizeof(int16_t));
    const uint32_t riffChunkSize = 36u + dataChunkSize;
    const uint32_t fmtChunkSize = 16u;

    std::vector<uint8_t> wav(44u + dataChunkSize);
    uint8_t* p = wav.data();

    std::memcpy(p + 0, "RIFF", 4);
    std::memcpy(p + 4, &riffChunkSize, sizeof(riffChunkSize));
    std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "fmt ", 4);
    std::memcpy(p + 16, &fmtChunkSize, sizeof(fmtChunkSize));
    std::memcpy(p + 20, &kAudioFormatPcm, sizeof(kAudioFormatPcm));
    std::memcpy(p + 22, &kNumChannels, sizeof(kNumChannels));
    std::memcpy(p + 24, &sampleRate, sizeof(sampleRate));
    std::memcpy(p + 28, &byteRate, sizeof(byteRate));
    std::memcpy(p + 32, &kBlockAlign, sizeof(kBlockAlign));
    std::memcpy(p + 34, &kBitsPerSample, sizeof(kBitsPerSample));
    std::memcpy(p + 36, "data", 4);
    std::memcpy(p + 40, &dataChunkSize, sizeof(dataChunkSize));

    auto* pcm16 = reinterpret_cast<int16_t*>(p + 44);
    for (size_t i = 0; i < monoPcm.size(); ++i) {
        const float clamped = std::clamp(monoPcm[i], -1.0f, 1.0f);
        pcm16[i] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
    }

    return wav;
}

bool WriteMonoFloatToWavFile(
    const std::filesystem::path& wavPath,
    std::span<const float> monoPcm,
    uint32_t sampleRate,
    std::string* outError
) {
    std::vector<uint8_t> wavBytes = EncodeMonoFloatToWavMemory(monoPcm, sampleRate);
    if (wavBytes.empty()) {
        if (outError) {
            *outError = "Cannot write empty PCM buffer to WAV file";
        }
        return false;
    }

    std::error_code ec;
    if (wavPath.has_parent_path()) {
        std::filesystem::create_directories(wavPath.parent_path(), ec);
    }

    std::ofstream out(wavPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (outError) {
            *outError = "Could not open output WAV file for writing: " + wavPath.string();
        }
        return false;
    }

    out.write(reinterpret_cast<const char*>(wavBytes.data()), static_cast<std::streamsize>(wavBytes.size()));
    if (!out.good()) {
        if (outError) {
            *outError = "Failed while writing WAV file: " + wavPath.string();
        }
        return false;
    }
    return true;
}

bool ReadEevsClipFile(
    const std::filesystem::path& filePath,
    std::vector<std::vector<uint8_t>>& outOpusFrames,
    uint32_t* outDurationMs,
    std::string* outError
) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) {
        if (outError) {
            *outError = "Could not open .eevs clip file: " + filePath.string();
        }
        return false;
    }

    EevsClipFileHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        if (outError) {
            *outError = "File is too small to contain an EEVS header: " + filePath.string();
        }
        return false;
    }

    if (std::memcmp(hdr.magic, "EEVS", 4) != 0 || hdr.version != 1 || hdr.frameCount == 0) {
        if (outError) {
            *outError = "Invalid EEVS file header or version: " + filePath.string();
        }
        return false;
    }

    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(hdr.frameCount);

    for (uint32_t i = 0; i < hdr.frameCount; ++i) {
        uint16_t frameLen = 0;
        if (!in.read(reinterpret_cast<char*>(&frameLen), sizeof(frameLen)) || frameLen == 0 || frameLen > 2048) {
            if (outError) {
                *outError = "Corrupted Opus frame length inside .eevs file: " + filePath.string();
            }
            return false;
        }

        std::vector<uint8_t> frame(frameLen);
        if (!in.read(reinterpret_cast<char*>(frame.data()), frameLen)) {
            if (outError) {
                *outError = "Unexpected EOF while reading .eevs Opus frame: " + filePath.string();
            }
            return false;
        }
        frames.push_back(std::move(frame));
    }

    outOpusFrames = std::move(frames);
    if (outDurationMs) {
        *outDurationMs = static_cast<uint32_t>(outOpusFrames.size() * 10);
    }
    return true;
}

bool WriteEevsClipFile(
    const std::filesystem::path& filePath,
    std::span<const std::vector<uint8_t>> opusFrames,
    std::string* outError
) {
    if (opusFrames.empty()) {
        if (outError) {
            *outError = "Cannot save empty Opus frame list";
        }
        return false;
    }

    std::error_code ec;
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), ec);
    }

    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (outError) {
            *outError = "Could not open output file for writing: " + filePath.string();
        }
        return false;
    }

    EevsClipFileHeader hdr{};
    hdr.version = 1;
    hdr.reserved = 0;
    hdr.frameCount = static_cast<uint32_t>(opusFrames.size());
    hdr.durationMs = static_cast<uint32_t>(opusFrames.size() * 10);

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    for (const auto& frame : opusFrames) {
        const auto frameLen = static_cast<uint16_t>(std::min<size_t>(frame.size(), 0xFFFFu));
        out.write(reinterpret_cast<const char*>(&frameLen), sizeof(frameLen));
        if (frameLen > 0) {
            out.write(reinterpret_cast<const char*>(frame.data()), frameLen);
        }
    }

    if (!out.good()) {
        if (outError) {
            *outError = "Failed while writing .eevs clip file: " + filePath.string();
        }
        return false;
    }

    return true;
}

std::string LoadClipFileToHost(
    IHostContext* host,
    const std::filesystem::path& filePath,
    std::string_view requestedClipId,
    uint32_t* outDurationMs,
    std::string* outError
) {
    if (!host) {
        return {};
    }

    std::string ext = filePath.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::string reqId = requestedClipId.empty()
        ? filePath.stem().string()
        : std::string(requestedClipId);
    char clipIdBuf[128]{};

    if (ext == ".wav") {
        std::vector<float> monoPcm;
        uint32_t sampleRate = 0;
        if (!ReadWavFileToMonoFloat(filePath, monoPcm, sampleRate, outError)) {
            return {};
        }
        if (!host->GetClips().StorePcmClip(
                reqId.c_str(),
                monoPcm.data(),
                monoPcm.size(),
                sampleRate,
                clipIdBuf,
                sizeof(clipIdBuf),
                outDurationMs
            )) {
            if (outError) {
                *outError = "Host StorePcmClip failed";
            }
            return {};
        }
        return clipIdBuf[0] != '\0' ? std::string(clipIdBuf) : reqId;
    }

    std::vector<std::vector<uint8_t>> frames;
    if (!ReadEevsClipFile(filePath, frames, outDurationMs, outError)) {
        return {};
    }

    std::vector<const uint8_t*> ptrs(frames.size());
    std::vector<uint16_t> sizes(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        ptrs[i] = frames[i].data();
        sizes[i] = static_cast<uint16_t>(frames[i].size());
    }

    if (!host->GetClips().StoreOpusClip(
            reqId.c_str(),
            ptrs.data(),
            sizes.data(),
            ptrs.size(),
            clipIdBuf,
            sizeof(clipIdBuf),
            outDurationMs
        )) {
        if (outError) {
            *outError = "Host StoreOpusClip failed";
        }
        return {};
    }

    return clipIdBuf[0] != '\0' ? std::string(clipIdBuf) : reqId;
}

} // namespace eevs::plugins::common
