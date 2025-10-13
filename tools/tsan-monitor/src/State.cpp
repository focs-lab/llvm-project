#include "State.h"

namespace monitor {
  std::uint64_t VectorClock::Get(int tid) const {
    const auto it = clock_.find(tid);
    if (it == clock_.end()) {
      return 0;
    }
    return it->second;
  }

  void VectorClock::Set(int tid, std::uint64_t value) {
    clock_[tid] = value;
  }

  std::uint64_t VectorClock::Tick(int tid) {
    std::uint64_t value = Get(tid);
    ++value;
    clock_[tid] = value;
    return value;
  }

  void AddressState::ClearReads() {
    read_set.clear();
  }

  void AddressState::AddOrUpdateRead(const Epoch &epoch, std::uint64_t value) {
    for (auto &entry: read_set) {
      if (entry.epoch.tid == epoch.tid) {
        if (epoch.clock > entry.epoch.clock) {
          entry.epoch.clock = epoch.clock;
          entry.value = value;
        }
        return;
      }
    }
    read_set.push_back(ReadEntry{epoch, value});
  }
} // namespace monitor
