#include "core/crypto_utils.h"

#include <openssl/evp.h>

namespace eevs::crypto {

uint32_t ComputeJoaat(const uint8_t* data, size_t length) {
    uint32_t hash = 0;
    for (size_t i = 0; i < length; ++i) {
        hash += static_cast<int8_t>(data[i]);
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

uint32_t ComputeJoaat(std::string_view data) {
    return ComputeJoaat(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::array<uint8_t, 32> ComputeSha256(std::string_view key) {
    std::array<uint8_t, 32> digest{};
    size_t outLen = 0;
    EVP_Q_digest(
        nullptr,
        "SHA256",
        nullptr,
        key.data(),
        key.size(),
        digest.data(),
        &outLen
    );
    return digest;
}

bool IsAllZeros32(const uint8_t* bytes) {
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; ++i) {
        acc |= bytes[i];
    }
    return acc == 0;
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<uint8_t>(diff | (static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i])));
    }
    return diff == 0;
}

std::string ToHexString(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

} // namespace eevs::crypto
