#include "core/logger.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace eevs::log {

namespace {
    std::atomic<quill::Logger*> g_logger{nullptr};

    quill::LogLevel ParseLogLevel(std::string_view levelStr) noexcept {
        if (levelStr == "debug" || levelStr == "DEBUG" || levelStr == "trace") {
            return quill::LogLevel::Debug;
        }
        if (levelStr == "warn" || levelStr == "WARN" || levelStr == "warning") {
            return quill::LogLevel::Warning;
        }
        if (levelStr == "error" || levelStr == "ERROR") {
            return quill::LogLevel::Error;
        }
        return quill::LogLevel::Info;
    }
}

void Initialize(std::string_view levelStr, const std::filesystem::path& logFilePath) {
    if (!quill::Backend::is_running()) {
        quill::BackendOptions backendOpts{};
        quill::Backend::start(backendOpts);
    }

    std::vector<std::shared_ptr<quill::Sink>> sinks;

    quill::ConsoleSinkConfig consoleConfig{};
    consoleConfig.set_stream("stdout");
    sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>("eevs_stdout", consoleConfig));

    if (!logFilePath.empty()) {
        std::error_code ec;
        if (logFilePath.has_parent_path()) {
            std::filesystem::create_directories(logFilePath.parent_path(), ec);
        }
        quill::FileSinkConfig fileConfig{};
        fileConfig.set_open_mode('a');
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::FileSink>(logFilePath.string(), fileConfig));
    }

    quill::PatternFormatterOptions fmtOpts{
        "%(time) [%(log_level:<5)] %(message)",
        "%H:%M:%S.%Qms"
    };

    const char* loggerName = logFilePath.empty() ? "eevs" : "eevs_with_file";
    quill::Logger* logger = quill::Frontend::create_or_get_logger(
        loggerName,
        std::move(sinks),
        fmtOpts
    );
    logger->set_log_level(ParseLogLevel(levelStr));

    g_logger.store(logger, std::memory_order_release);
}

void SetLevel(std::string_view levelStr) noexcept {
    if (quill::Logger* logger = g_logger.load(std::memory_order_acquire)) {
        logger->set_log_level(ParseLogLevel(levelStr));
    }
}

void Shutdown() {
    if (quill::Logger* logger = g_logger.load(std::memory_order_acquire)) {
        logger->flush_log();
    }
    if (quill::Backend::is_running()) {
        quill::Backend::stop();
    }
}

quill::Logger* Get() noexcept {
    quill::Logger* logger = g_logger.load(std::memory_order_acquire);
    if (!logger) {
        Initialize();
        logger = g_logger.load(std::memory_order_acquire);
    }
    return logger;
}

} // namespace eevs::log
