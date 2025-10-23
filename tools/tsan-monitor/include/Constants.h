#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace monitor {
constexpr std::size_t kSlotsPerChannel = 4096;
constexpr std::size_t kSlotMask = kSlotsPerChannel - 1;
constexpr std::size_t kMaxEventArgs = 5;

constexpr std::uint64_t kMonitorReadyMagic = 0xcafebeefULL;
constexpr std::uint64_t kProgramEndedMagic = 0xdeaddeadULL;

constexpr std::chrono::milliseconds kDefaultRefreshInterval{200};
constexpr std::chrono::milliseconds kReaderIdleSleep{1};
constexpr std::chrono::milliseconds kSchedulerIdleSleep{5};

enum class RaceAction {
  kContinue,
  kStop,
};
}  // namespace monitor
