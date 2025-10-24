#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "Constants.h"

namespace monitor {
enum class EventId : std::uint8_t {
  kUnknown = 0,
  kRead = 1,
  kWrite = 2,
  kVptrUpdate = 3,
  kVptrLoad = 4,
  kMemset = 5,
  kMemcpy = 6,
  kAtomicLoad = 7,
  kAtomicStore = 8,
  kAtomicRMW = 9,
  kAtomicCAS = 10,
  kAtomicFence = 11,
  kReturn = 12,
  kAtExit = 13,
  kMonitorReady = 14,
  kMutexLock = 20,
  kMutexUnlock = 21,
  kThreadSpawn = 25,
  kThreadJoin = 26,
  kThreadExit = 27,
  kThreadStart = 28,
  kIgnoreBegin = 0xfe,
  kIgnoreEnd = 0xff,
  kProgramEndMarker =
      0xfd  // sentinel produced by monitor when seeing kProgramEndedMagic
};

struct Event {
  int tid = -1;
  EventId id = EventId::kUnknown;
  std::uint8_t lap = 0;
  std::uint64_t address = 0;
  std::array<std::uint64_t, kMaxEventArgs> args{};
  std::size_t nargs = 0;
  std::uint64_t raw_header = 0;
  std::uint64_t index = 0;
};

std::size_t EventArgCount(EventId id);

std::string_view EventName(EventId id);

bool IsTermination(const Event& event);
}  // namespace monitor
