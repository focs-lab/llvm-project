#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace monitor {
// Circular buffer configuration - 262,144 slots = 2MB per thread
constexpr std::size_t kSlotsPerChannel = 262144;
constexpr std::size_t kSlotMask = kSlotsPerChannel - 1;
constexpr std::size_t kMaxEventArgs = 5;  // Maximum arguments per event

// Sentinel values for special events in channel
constexpr std::uint64_t kMonitorReadyMagic = 0xcafebeefULL;
constexpr std::uint64_t kProgramEndedMagic = 0xdeaddeadULL;

// Timing intervals for monitor components
constexpr std::chrono::milliseconds kDefaultRefreshInterval{50};  // UI refresh rate
constexpr std::chrono::milliseconds kReaderIdleSleep{1};        // Reader polling interval
constexpr std::chrono::milliseconds kSchedulerIdleSleep{5};    // Scheduler wait interval

enum class RaceAction {
  kContinue,
  kStop,
};
}  // namespace monitor
