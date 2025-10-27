#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "State.h"

namespace monitor {
enum class RaceKind {
  kReadWrite,
  kWriteRead,
  kWriteWrite,
};

struct RaceEventInfo {
  RaceKind kind;
  std::uint64_t address = 0;
  Epoch first;
  Epoch second;
  std::uint64_t first_value = 0;
  std::uint64_t second_value = 0;
};

class Report {
 public:
  explicit Report(std::filesystem::path output_dir = "report");

  void OnRace(const RaceEventInfo& info);

  // Set directory like /tmp/tsan.monitor.<pid> for writing the sentinel file.
  void SetSentinelDirectory(std::filesystem::path dir) {
    sentinel_dir_ = std::move(dir);
  }

  // Set Origin process PID for signal sending
  void SetOriginPid(pid_t pid) { origin_pid_ = pid; }

 private:
  std::string MakeKey(const RaceEventInfo& info) const;

  std::string Format(const RaceEventInfo& info) const;

  void Emit(const RaceEventInfo& info, const std::string& content);

  // One-shot write to stderr (first race only) to avoid spamming.
  void EmitToStderrOnce(const std::string& content);

  // Create the sentinel file at /tmp/tsan.monitor.<pid>/race_found
  void CreateRaceSentinel();

  // Send SIGUSR1 signal to Origin process (first race only)
  void SendRaceSignal();

  std::filesystem::path output_dir_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::size_t> counters_;
  bool emitted_once_ =
      false;  // Single-threaded; used only to prevent duplicates
  std::filesystem::path sentinel_dir_;
  pid_t origin_pid_ = -1;  // Origin process PID for signal sending
};
}  // namespace monitor
