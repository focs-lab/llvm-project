#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "Event.h"
#include "MemoryOrder.h"
#include "State.h"

namespace monitor {

struct PublishInfo {
  VectorClock vc;
  std::uint64_t count = 0;
  int tid = -1;
  std::uint32_t order = 0;
};

class SyncTokenManager {
 public:
  SyncTokenManager() = default;

  void PublishAtomic(std::uint64_t addr, std::uint64_t count,
                     const VectorClock& vc, int tid, std::uint32_t mo);

  std::optional<VectorClock> TryAcquireAtomic(std::uint64_t addr,
                                              std::uint64_t count,
                                              std::uint32_t mo) const;

  void PublishMutexUnlock(std::uint64_t addr, std::uint64_t count,
                          const VectorClock& vc, int tid);

  std::optional<VectorClock> TryAcquireMutexLock(std::uint64_t addr,
                                                 std::uint64_t count) const;

  void Reset();

 private:
  // Maps from atomic/mutex address to last publish info (VC)
  std::unordered_map<std::uint64_t, PublishInfo> atomic_last_by_addr_;
  std::unordered_map<std::uint64_t, PublishInfo> mutex_last_by_addr_;
};

bool IsMutexEvent(const Event& event);

bool IsAtomicEvent(const Event& event);

std::uint64_t ExtractCount(const Event& event);

std::uint32_t ExtractMemoryOrder(const Event& event);

bool ExtractSuccess(const Event& event);

}  // namespace monitor
