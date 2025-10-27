#include "Report.h"

#include <fcntl.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

namespace monitor {
namespace {
std::string RaceKindToString(RaceKind kind) {
  switch (kind) {
    case RaceKind::kReadWrite:
      return "Read-Write";
    case RaceKind::kWriteRead:
      return "Write-Read";
    case RaceKind::kWriteWrite:
      return "Write-Write";
  }
  return "Unknown";
}

std::string TimestampString() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
  return std::format("{}", ms);
}
}  // namespace

Report::Report(std::filesystem::path output_dir)
    : output_dir_(std::move(output_dir)) {
  std::error_code ec;
  std::filesystem::create_directories(output_dir_, ec);
}

void Report::OnRace(const RaceEventInfo& info) {
  const auto key = MakeKey(info);
  std::string content = Format(info);
  bool first_emit = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& counter = counters_[key];
    ++counter;
    // Always emit a file per race occurrence to preserve history for offline
    // analysis.
    Emit(info, content);
    // On the first race only, also mirror to stderr and create a sentinel file.
    if (!emitted_once_) {
      emitted_once_ = true;
      first_emit = true;
    }
  }
  if (first_emit) {
    EmitToStderrOnce(content);
    CreateRaceSentinel();
    SendRaceSignal();  // 🆕 Send signal to Origin process on first race
  }
}

std::string Report::MakeKey(const RaceEventInfo& info) const {
  const auto min_tid = std::min(info.first.tid, info.second.tid);
  const auto max_tid = std::max(info.first.tid, info.second.tid);
  return std::format("{}:{}:{}:{}", static_cast<int>(info.kind), info.address,
                     min_tid, max_tid);
}

std::string Report::Format(const RaceEventInfo& info) const {
  auto access = [&](bool first) {
    switch (info.kind) {
      case RaceKind::kReadWrite:
        return first ? "write" : "read";
      case RaceKind::kWriteRead:
        return first ? "read" : "write";
      case RaceKind::kWriteWrite:
        return "write";
    }
    return "access";
  };

  // Keep WARNING and SUMMARY lines verbatim for llvm-lit/FileCheck
  // compatibility.
  return std::format(
      "==================\n"
      "WARNING: ThreadSanitizer: data race\n"
      "Race detected: {}\n"
      "Address        : 0x{:x}\n"
      "First  Thread  : tid={} clock={} {} value=0x{:x}\n"
      "Second Thread  : tid={} clock={} {} value=0x{:x}\n"
      "Conflict       : thread {} {} conflicts with thread {} {} on the same "
      "address.\n"
      "SUMMARY: ThreadSanitizer: data race (Thread-{} with Thread-{})\n"
      "==================\n",
      RaceKindToString(info.kind), info.address, info.first.tid,
      info.first.clock, access(true), info.first_value, info.second.tid,
      info.second.clock, access(false), info.second_value, info.first.tid,
      access(true), info.second.tid, access(false), info.first.tid,
      info.second.tid);
}

void Report::Emit(const RaceEventInfo& info, const std::string& content) {
  const auto key = MakeKey(info);
  const auto index = counters_[key];
  const auto timestamp = TimestampString();
  const auto filename = std::format(
      "Race_{}_{}_{:04}.txt", RaceKindToString(info.kind), timestamp, index);
  const auto path = output_dir_ / filename;

  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs) {
    return;
  }
  ofs << content;
}

void Report::EmitToStderrOnce(const std::string& content) {
  // Write as a single chunk to avoid interleaving between lines.
  (void)::write(STDERR_FILENO, content.data(), content.size());
}

void Report::CreateRaceSentinel() {
  if (sentinel_dir_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(sentinel_dir_, ec);
  const auto p = sentinel_dir_ / "race_found";
  int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0) ::close(fd);
}

void Report::SendRaceSignal() {
  if (origin_pid_ <= 0) {
    SPDLOG_WARN("No Origin PID available for signal sending");
    return;
  }

  // Send SIGUSR1 signal to Origin process
  if (kill(origin_pid_, SIGUSR1) == 0) {
    SPDLOG_INFO("Race detection signal sent to Origin process {}", origin_pid_);
  } else {
    SPDLOG_ERROR(
        "Failed to send race detection signal to Origin process {}: {}",
        origin_pid_, strerror(errno));
  }
}
}  // namespace monitor
