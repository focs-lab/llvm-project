#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "Logger.h"
#include "MonitorApp.h"

namespace {
  void PrintUsage() {
    std::cout << "tsan-monitor\n"
        << "Usage: tsan-monitor <pid>\n\n"
        << "Configure runtime options via TSAN_OPTIONS, for example:\n"
        << "  TSAN_OPTIONS=monitor_dir=/tmp/custom:monitor_refresh_ms=500:monitor_verbose=1\n";
  }

  std::optional<pid_t> ParsePid(std::string_view value) {
    char *end = nullptr;
    const long parsed = std::strtol(std::string(value).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0) {
      return std::nullopt;
    }
    return static_cast<pid_t>(parsed);
  }
} // namespace

struct EnvOverrides {
  std::optional<std::filesystem::path> directory;
  std::optional<std::chrono::milliseconds> refresh_interval;
  std::optional<bool> verbose;
  std::optional<monitor::RaceAction> race_action;
};

std::string_view Trim(std::string_view s) {
  size_t start = 0;
  size_t end = s.size();
  while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(start, end - start);
}

EnvOverrides ParseEnvOverrides() {
  EnvOverrides cfg;
  const char *tsan_opts = std::getenv("TSAN_OPTIONS");
  if (!tsan_opts || !tsan_opts[0])
    return cfg;

  std::string_view opts{tsan_opts};
  size_t pos = 0;
  while (pos <= opts.size()) {
    size_t next = opts.find(':', pos);
    std::string_view token = (next == std::string_view::npos)
                               ? opts.substr(pos)
                               : opts.substr(pos, next - pos);
    token = Trim(token);
    if (!token.empty()) {
      size_t eq = token.find('=');
      if (eq != std::string_view::npos) {
        std::string_view key = Trim(token.substr(0, eq));
        std::string_view value = Trim(token.substr(eq + 1));
        if (key == "monitor_dir" && !value.empty()) {
          cfg.directory = std::filesystem::path(std::string(value));
        } else if (key == "monitor_refresh_ms" && !value.empty()) {
          try {
            long long ms = std::stoll(std::string(value));
            if (ms > 0)
              cfg.refresh_interval = std::chrono::milliseconds(ms);
          } catch (...) {
          }
        } else if (key == "monitor_on_race" && !value.empty()) {
          std::string val_lower(value);
          for (char &c: val_lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (val_lower == "stop")
            cfg.race_action = monitor::RaceAction::kStop;
          else if (val_lower == "continue")
            cfg.race_action = monitor::RaceAction::kContinue;
        } else if (key == "monitor_verbose" && !value.empty()) {
          if (value == "1" || value == "true" || value == "True")
            cfg.verbose = true;
          else if (value == "0" || value == "false" || value == "False")
            cfg.verbose = false;
        }
      }
    }
    if (next == std::string_view::npos)
      break;
    pos = next + 1;
  }
  return cfg;
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  std::string_view arg{argv[1]};
  if (arg == "--help" || arg == "-h") {
    PrintUsage();
    return EXIT_SUCCESS;
  }

  auto pid = ParsePid(arg);
  if (!pid) {
    std::cerr << "invalid pid: " << arg << std::endl;
    PrintUsage();
    return EXIT_FAILURE;
  }

  monitor::MonitorOptions options;
  options.pid = *pid;

  EnvOverrides overrides = ParseEnvOverrides();
  if (overrides.directory)
    options.directory = *overrides.directory;
  if (overrides.refresh_interval)
    options.refresh_interval = *overrides.refresh_interval;
  if (overrides.verbose.has_value())
    options.verbose = *overrides.verbose;
  if (overrides.race_action)
    options.race_action = *overrides.race_action;

  if (options.directory.empty()) {
    options.directory = std::filesystem::path("/tmp") /
                        ("tsan.monitor." + std::to_string(options.pid));
  }

  auto log_level = options.verbose ? spdlog::level::debug : spdlog::level::info;
  monitor::InitLogger("tsan-monitor.log", log_level);

  monitor::MonitorApp app(std::move(options));
  return app.Run();
}
