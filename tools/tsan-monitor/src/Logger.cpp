#include "Logger.h"

#include <format>
#include <mutex>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace monitor {
    namespace {
        std::once_flag g_logger_once;
    }

    void InitLogger(const std::string &file_path, spdlog::level::level_enum level) {
        std::call_once(g_logger_once, [&]() {
            constexpr std::size_t kMaxSize = 5 * 1024 * 1024; // 5 MB
            constexpr std::size_t kMaxFiles = 3;
            auto logger = spdlog::rotating_logger_mt("tsan-monitor", file_path,
                                                     kMaxSize, kMaxFiles);
            spdlog::set_default_logger(logger);
            spdlog::set_level(level);
            spdlog::flush_on(spdlog::level::info);
            spdlog::info("Logger initialized at {}", file_path);
        });
    }

    void SetLogLevel(spdlog::level::level_enum level) {
        spdlog::set_level(level);
    }

    void SetLogLevelFromString(const std::string &level) {
        spdlog::set_level(spdlog::level::from_str(level));
    }
} // namespace monitor