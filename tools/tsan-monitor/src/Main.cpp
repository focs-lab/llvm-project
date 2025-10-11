#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "MonitorApp.h"

namespace {

void PrintUsage() {
  std::cout << "tsan-monitor\n"
            << "Usage: tsan-monitor --attach <pid> [options]\n\n"
            << "Options:\n"
            << "  --monitor-dir <path>  Override channel directory\n"
            << "  --refresh-ms <ms>     Directory rescan interval\n"
            << "  --verbose             Print decoded events\n"
            << "  --help                Show this message\n";
}

std::optional<pid_t> ParsePid(std::string_view value) {
  char* end = nullptr;
  const long parsed = std::strtol(std::string(value).c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < 0) {
    return std::nullopt;
  }
  return static_cast<pid_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc <= 1) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  monitor::MonitorOptions options;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return EXIT_SUCCESS;
    } else if (arg == "--attach") {
      if (i + 1 >= argc) {
        std::cerr << "--attach requires a pid" << std::endl;
        return EXIT_FAILURE;
      }
      auto pid = ParsePid(argv[++i]);
      if (!pid) {
        std::cerr << "invalid pid: " << argv[i] << std::endl;
        return EXIT_FAILURE;
      }
      options.pid = *pid;
    } else if (arg == "--monitor-dir") {
      if (i + 1 >= argc) {
        std::cerr << "--monitor-dir requires a path" << std::endl;
        return EXIT_FAILURE;
      }
      options.directory = argv[++i];
    } else if (arg == "--refresh-ms") {
      if (i + 1 >= argc) {
        std::cerr << "--refresh-ms requires a value" << std::endl;
        return EXIT_FAILURE;
      }
      auto value_ms = ParsePid(argv[++i]);
      if (!value_ms) {
        std::cerr << "invalid refresh interval" << std::endl;
        return EXIT_FAILURE;
      }
      options.refresh_interval = std::chrono::milliseconds(*value_ms);
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else {
      std::cerr << "unknown option: " << arg << std::endl;
      PrintUsage();
      return EXIT_FAILURE;
    }
  }

  if (options.pid < 0) {
    std::cerr << "--attach <pid> is required" << std::endl;
    return EXIT_FAILURE;
  }

  if (options.directory.empty()) {
    options.directory = std::filesystem::path("/tmp") /
                        ("tsan.monitor." + std::to_string(options.pid));
  }

  monitor::MonitorApp app(std::move(options));
  return app.Run();
}
