#pragma once

/**
 * @file logger.h
 * @brief Low-latency asynchronous logging wrapper around `quill::Logger` for EEVS.
 */

#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <filesystem>
#include <string_view>

namespace eevs::log {

/**
 * @brief Initializes the Quill asynchronous logging backend and creates the root EEVS logger.
 * @param levelStr Optional log level (`"debug"`, `"info"`, `"warn"`, `"error"`).
 * @param logFilePath Optional file path to mirror log output to disk alongside `stdout`.
 */
void Initialize(std::string_view levelStr = "info", const std::filesystem::path& logFilePath = {});

/**
 * @brief Updates the active log level dynamically (`"debug"`, `"info"`, `"warn"`, `"error"`).
 */
void SetLevel(std::string_view levelStr) noexcept;

/**
 * @brief Flushes pending log messages and stops the Quill backend thread cleanly.
 */
void Shutdown();

/**
 * @brief Returns the global EEVS `quill::Logger*` instance (initializes lazily if needed).
 */
[[nodiscard]] quill::Logger* Get() noexcept;

} // namespace eevs::log

#define EEVS_LOG_DEBUG(fmt, ...) LOG_DEBUG(::eevs::log::Get(), fmt __VA_OPT__(,) __VA_ARGS__)
#define EEVS_LOG_INFO(fmt, ...)  LOG_INFO(::eevs::log::Get(), fmt __VA_OPT__(,) __VA_ARGS__)
#define EEVS_LOG_WARN(fmt, ...)  LOG_WARNING(::eevs::log::Get(), fmt __VA_OPT__(,) __VA_ARGS__)
#define EEVS_LOG_ERROR(fmt, ...) LOG_ERROR(::eevs::log::Get(), fmt __VA_OPT__(,) __VA_ARGS__)
