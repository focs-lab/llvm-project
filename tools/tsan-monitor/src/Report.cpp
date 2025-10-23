#include "Report.h"

#include <algorithm>
#include <chrono>
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
  std::lock_guard<std::mutex> lock(mutex_);
  auto& counter = counters_[key];
  ++counter;

  Emit(info, Format(info));
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

  return std::format(
      "Race detected: {}\n"
      "Address        : 0x{:x}\n"
      "First  Thread  : tid={} clock={} {} value=0x{:x}\n"
      "Second Thread  : tid={} clock={} {} value=0x{:x}\n"
      "Conflict       : thread {} {} conflicts with thread {} {} on the same "
      "address.\n",
      RaceKindToString(info.kind), info.address, info.first.tid,
      info.first.clock, access(true), info.first_value, info.second.tid,
      info.second.clock, access(false), info.second_value, info.first.tid,
      access(true), info.second.tid, access(false));
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
}  // namespace monitor
