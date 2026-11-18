#include "core/server_config.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <vector>

namespace eevs {

namespace {

std::optional<uint32_t> ParseUintToken(std::string_view token) noexcept {
    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), val);
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return val;
}

bool ParseBoolToken(std::string_view token, bool defaultValue = true) noexcept {
    if (token == "1" || token == "true" || token == "TRUE" || token == "on" || token == "yes") {
        return true;
    }
    if (token == "0" || token == "false" || token == "FALSE" || token == "off" || token == "no") {
        return false;
    }
    return defaultValue;
}

std::string_view Trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

// Strips inline '#' or '//' comments outside of double quotes and tokenizes whitespace-separated arguments
std::vector<std::string_view> TokenizeConfigLine(std::string_view line) {
    std::vector<std::string_view> tokens;
    line = Trim(line);
    if (line.empty() || line.front() == '#') {
        return tokens;
    }

    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }

        // Comment start outside quotes
        if (line[i] == '#' || (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')) {
            break;
        }

        if (line[i] == '"') {
            const size_t start = ++i;
            while (i < line.size() && line[i] != '"') {
                ++i;
            }
            tokens.push_back(line.substr(start, i - start));
            if (i < line.size() && line[i] == '"') {
                ++i;
            }
        } else {
            const size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '#') {
                if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
                    break;
                }
                ++i;
            }
            tokens.push_back(line.substr(start, i - start));
        }
    }

    return tokens;
}

} // namespace

bool ServerConfig::ParseHostPort(std::string_view endpoint, std::string& outHost, uint16_t& outPort) noexcept {
    endpoint = Trim(endpoint);
    if (endpoint.empty()) {
        return false;
    }

    const size_t colonPos = endpoint.rfind(':');
    if (colonPos == std::string_view::npos || colonPos == 0 || colonPos + 1 >= endpoint.size()) {
        return false;
    }

    std::string_view hostPart = endpoint.substr(0, colonPos);
    std::string_view portPart = endpoint.substr(colonPos + 1);

    uint32_t parsedPort = 0;
    auto [ptr, ec] = std::from_chars(portPart.data(), portPart.data() + portPart.size(), parsedPort);
    if (ec != std::errc{} || ptr != portPart.data() + portPart.size() || parsedPort == 0 || parsedPort > 65535) {
        return false;
    }

    outHost = std::string(hostPart);
    outPort = static_cast<uint16_t>(parsedPort);
    return true;
}

bool ServerConfig::LoadFromFile(
    const std::filesystem::path& configPath,
    ServerConfig& outConfig,
    std::string* outError
) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        if (outError) {
            *outError = "Could not open config file: " + configPath.string();
        }
        return false;
    }

    std::string line;
    size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;

        // Strip UTF-8 BOM on the first line if present
        std::string_view lineView(line);
        if (lineNumber == 1 && lineView.size() >= 3 &&
            static_cast<uint8_t>(lineView[0]) == 0xEF &&
            static_cast<uint8_t>(lineView[1]) == 0xBB &&
            static_cast<uint8_t>(lineView[2]) == 0xBF) {
            lineView.remove_prefix(3);
        }

        std::vector<std::string_view> tokens = TokenizeConfigLine(lineView);
        if (tokens.empty()) {
            continue;
        }

        std::string_view key = tokens[0];
        constexpr size_t valStart = 1;

        if (key != "sv_licenseKey" && valStart < tokens.size()) {
            std::string combinedVal;
            for (size_t i = valStart; i < tokens.size(); ++i) {
                if (i > valStart) {
                    combinedVal.push_back(' ');
                }
                combinedVal.append(tokens[i]);
            }
            outConfig.properties[std::string(key)] = std::move(combinedVal);
        }

        if (key == "sv_licenseKey") {
            if (valStart >= tokens.size() || tokens[valStart].empty()) {
                if (outError) {
                    *outError = "Line " + std::to_string(lineNumber) + ": 'sv_licenseKey' requires a license key value.";
                }
                return false;
            }
            outConfig.licenseKey = std::string(tokens[valStart]);
        } else if (key == "voice_external_host") {
            if (valStart + 1 >= tokens.size()) {
                if (outError) {
                    *outError = "Line " + std::to_string(lineNumber) +
                                ": 'voice_external_host' expects '<voiceHost:port> <gatewayHost:port>'.";
                }
                return false;
            }

            if (!ParseHostPort(tokens[valStart], outConfig.voiceBindHost, outConfig.voiceBindPort)) {
                if (outError) {
                    *outError = "Line " + std::to_string(lineNumber) +
                                ": Invalid voice host:port '" + std::string(tokens[valStart]) + "'.";
                }
                return false;
            }

            if (!ParseHostPort(tokens[valStart + 1], outConfig.gatewayHost, outConfig.gatewayPort)) {
                if (outError) {
                    *outError = "Line " + std::to_string(lineNumber) +
                                ": Invalid gateway host:port '" + std::string(tokens[valStart + 1]) + "'.";
                }
                return false;
            }
        } else if (key == "eevs_control_host" && valStart < tokens.size()) {
            if (!ParseHostPort(tokens[valStart], outConfig.controlBindHost, outConfig.controlBindPort)) {
                if (outError) {
                    *outError = "Line " + std::to_string(lineNumber) +
                                ": Invalid control host:port '" + std::string(tokens[valStart]) + "'.";
                }
                return false;
            }
        } else if (key == "eevs_control_enabled" && valStart < tokens.size()) {
            outConfig.controlEnabled = ParseBoolToken(tokens[valStart], true);
        } else if (key == "eevs_control_key" && valStart < tokens.size()) {
            if (tokens[valStart] != "changeme") {
                outConfig.controlKey = std::string(tokens[valStart]);
            }
        } else if (key == "eevs_max_peers" && valStart < tokens.size()) {
            if (auto v = ParseUintToken(tokens[valStart])) {
                outConfig.maxPeers = std::clamp<size_t>(*v, 1u, 4095u);
            }
        } else if (key == "eevs_gateway_reconnect" && valStart < tokens.size()) {
            outConfig.gatewayAutoReconnect = ParseBoolToken(tokens[valStart], true);
        } else if (key == "eevs_vad_hangover_ms" && valStart < tokens.size()) {
            if (auto v = ParseUintToken(tokens[valStart])) {
                outConfig.vadHangoverMs = std::clamp<uint32_t>(*v, 20u, 5000u);
            }
        } else if (key == "voice_inBitrate" && valStart < tokens.size()) {
            if (auto v = ParseUintToken(tokens[valStart])) {
                outConfig.opusBitrate = static_cast<int>(std::clamp<uint32_t>(*v, 16000u, 128000u));
            }
        } else if (key == "eevs_opus_complexity" && valStart < tokens.size()) {
            if (auto v = ParseUintToken(tokens[valStart])) {
                outConfig.opusComplexity = static_cast<int>(std::clamp<uint32_t>(*v, 1u, 10u));
            }
        } else if (key == "eevs_plugins_dir" && valStart < tokens.size()) {
            outConfig.pluginsDir = std::filesystem::path(tokens[valStart]);
        } else if (key == "eevs_clip_cache_mb" && valStart < tokens.size()) {
            if (auto mbVal = ParseUintToken(tokens[valStart]); mbVal && *mbVal > 0) {
                outConfig.clipCacheMaxMb = static_cast<size_t>(*mbVal);
            }
        } else if (key == "eevs_log_level" && valStart < tokens.size()) {
            outConfig.logLevel = std::string(tokens[valStart]);
        } else if (key == "eevs_log_file" && valStart < tokens.size()) {
            outConfig.logFile = std::filesystem::path(tokens[valStart]);
        }
    }

    if (outConfig.licenseKey.empty()) {
        if (outError) {
            *outError = "Config file '" + configPath.string() + "' is missing required 'sv_licenseKey'.";
        }
        return false;
    }

    return true;
}

} // namespace eevs
