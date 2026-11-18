#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

namespace eevs::plugins::common {

inline constexpr glz::opts kRelaxedJsonOpts{.error_on_unknown_keys = false};

struct ErrorRespDto {
    bool ok = false;
    std::string_view error;
};

inline void CopyStringToResponseBuffer(std::string_view str, char* responseBuf, size_t responseCap) noexcept {
    if (!responseBuf || responseCap == 0) {
        return;
    }
    const size_t copyLen = std::min(str.size(), responseCap - 1);
    std::memcpy(responseBuf, str.data(), copyLen);
    responseBuf[copyLen] = '\0';
}

template <typename T>
void WriteJsonToResponseBuffer(const T& dto, char* responseBuf, size_t responseCap) noexcept {
    std::string json;
    (void)glz::write_json(dto, json);
    CopyStringToResponseBuffer(json, responseBuf, responseCap);
}

} // namespace eevs::plugins::common
