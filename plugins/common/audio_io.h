#pragma once

#include "eevs/host_api.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eevs::plugins::common {

#pragma pack(push, 1)
struct EevsClipFileHeader {
    char     magic[4]   = {'E', 'E', 'V', 'S'};
    uint16_t version    = 1;
    uint16_t reserved   = 0;
    uint32_t frameCount = 0;
    uint32_t durationMs = 0;
};
#pragma pack(pop)

static_assert(sizeof(EevsClipFileHeader) == 16, "EevsClipFileHeader must be 16 bytes");

[[nodiscard]] std::vector<float> ResampleMonoLinear(
    std::span<const float> inputSamples,
    uint32_t inputSampleRate,
    uint32_t outputSampleRate
);

void Downsample48kTo16kAppend(
    const float* samples,
    size_t sampleCount,
    uint32_t sampleRate,
    std::vector<float>& outSamples16k,
    size_t maxSamples16k
);

[[nodiscard]] std::vector<std::vector<uint8_t>> EncodePcmToOpusFrames(
    std::span<const float> pcmSamples,
    uint32_t sampleRate = 48000
);

[[nodiscard]] std::vector<float> DecodeOpusFramesToPcm(
    std::span<const std::vector<uint8_t>> opusFrames
);

[[nodiscard]] std::vector<uint8_t> EncodeMonoFloatToWavMemory(
    std::span<const float> monoPcm,
    uint32_t sampleRate = 48000
);

[[nodiscard]] bool ExtractFloatPcmFromWavMemory(
    const void* wavBytes,
    size_t wavLen,
    uint32_t fallbackRate,
    std::vector<float>& outMonoPcm,
    uint32_t& outSampleRate,
    std::string* outError = nullptr
);

[[nodiscard]] bool ReadWavFileToMonoFloat(
    const std::filesystem::path& wavPath,
    std::vector<float>& outMonoPcm,
    uint32_t& outSampleRate,
    std::string* outError = nullptr
);

[[nodiscard]] bool WriteMonoFloatToWavFile(
    const std::filesystem::path& wavPath,
    std::span<const float> monoPcm,
    uint32_t sampleRate = 48000,
    std::string* outError = nullptr
);

[[nodiscard]] bool ReadEevsClipFile(
    const std::filesystem::path& filePath,
    std::vector<std::vector<uint8_t>>& outOpusFrames,
    uint32_t* outDurationMs = nullptr,
    std::string* outError = nullptr
);

[[nodiscard]] bool WriteEevsClipFile(
    const std::filesystem::path& filePath,
    std::span<const std::vector<uint8_t>> opusFrames,
    std::string* outError = nullptr
);

[[nodiscard]] std::string LoadClipFileToHost(
    IHostContext* host,
    const std::filesystem::path& filePath,
    std::string_view requestedClipId = {},
    uint32_t* outDurationMs = nullptr,
    std::string* outError = nullptr
);

} // namespace eevs::plugins::common
