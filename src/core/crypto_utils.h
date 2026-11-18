#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eevs::crypto {

uint32_t ComputeJoaat(const uint8_t* data, size_t length);
uint32_t ComputeJoaat(std::string_view data);
std::array<uint8_t, 32> ComputeSha256(std::string_view key);
bool IsAllZeros32(const uint8_t* bytes);
bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept;
std::string ToHexString(const uint8_t* data, size_t len);

} // namespace eevs::crypto
