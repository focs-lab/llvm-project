#include "Logger.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <format>
#include <mutex>

namespace monitor {
namespace {
std::once_flag g_logger_once;
}

void InitLogger(const std::string& file_path, spdlog::level::level_enum level) {
  std::call_once(g_logger_once, [&]() {
    constexpr std::size_t kMaxSize = 5 * 1024 * 1024;  // 5 MB
    constexpr std::size_t kMaxFiles = 3;
    auto logger = spdlog::rotating_logger_mt("tsan-monitor", file_path,
                                             kMaxSize, kMaxFiles);
    spdlog::set_default_logger(logger);
    // Use time, level, and file:line only: "[time level file:line]"
    // Example: [12:34:56.789 DEBUG Scheduler.cpp:106] message
    spdlog::set_pattern("[%H:%M:%S.%e %^%l%$ %s:%#] %v");
    spdlog::set_level(level);
    spdlog::flush_on(spdlog::level::info);
    SPDLOG_INFO("Logger initialized at {}", file_path);
  });
}

void SetLogLevel(spdlog::level::level_enum level) { spdlog::set_level(level); }

void SetLogLevelFromString(const std::string& level) {
  spdlog::set_level(spdlog::level::from_str(level));
}
}  // namespace monitor
