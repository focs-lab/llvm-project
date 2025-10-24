#include "SyncTokenManager.h"

#include <algorithm>
#include <utility>

namespace monitor {
namespace {
VectorClock CopyClock(const VectorClock& clock) {
  VectorClock copy;
  for (const auto& entry : clock.Entries()) {
    copy.Set(entry.first, entry.second);
  }
  return copy;
}
}  // namespace

bool IsMutexEvent(const Event& event) {
  return event.id == EventId::kMutexLock || event.id == EventId::kMutexUnlock;
}

bool IsAtomicEvent(const Event& event) {
  return event.id == EventId::kAtomicLoad ||
         event.id == EventId::kAtomicStore || event.id == EventId::kAtomicRMW ||
         event.id == EventId::kAtomicCAS;
}

std::uint64_t ExtractCount(const Event& event) {
  return event.nargs > 0 ? event.args[0] : 0;
}

std::uint32_t ExtractMemoryOrder(const Event& event) {
  switch (event.id) {
    case EventId::kAtomicLoad:
    case EventId::kAtomicStore:
      return event.nargs >= 3 ? static_cast<std::uint32_t>(event.args[2]) : 0;
    case EventId::kAtomicRMW:
    case EventId::kAtomicCAS:
      return event.nargs >= 4 ? static_cast<std::uint32_t>(event.args[3]) : 0;
    default:
      return 0;
  }
}

bool ExtractSuccess(const Event& event) {
  if (event.id == EventId::kAtomicCAS) {
    return event.nargs >= 5 ? event.args[4] != 0 : false;
  }
  return false;
}

void SyncTokenManager::PublishAtomic(std::uint64_t addr, std::uint64_t count,
                                     const VectorClock& vc, int tid,
                                     std::uint32_t mo) {
  if (!IsReleaseOrder(mo)) {
    return;
  }
  PublishInfo info;
  info.vc = CopyClock(vc);
  info.count = count;
  info.tid = tid;
  info.order = mo;
  atomic_last_by_addr_[addr] = std::move(info);
}

std::optional<VectorClock> SyncTokenManager::TryAcquireAtomic(
    std::uint64_t addr, std::uint64_t /*count*/, std::uint32_t mo) const {
  if (!IsAcquireOrder(mo)) {
    return std::nullopt;
  }
  const auto it = atomic_last_by_addr_.find(addr);
  if (it == atomic_last_by_addr_.end()) {
    return std::nullopt;
  }
  return it->second.vc;
}

void SyncTokenManager::PublishMutexUnlock(std::uint64_t addr,
                                          std::uint64_t count,
                                          const VectorClock& vc, int tid) {
  PublishInfo info;
  info.vc = CopyClock(vc);
  info.count = count;
  info.tid = tid;
  mutex_last_by_addr_[addr] = std::move(info);
}

std::optional<VectorClock> SyncTokenManager::TryAcquireMutexLock(
    std::uint64_t addr, std::uint64_t /*count*/) const {
  const auto it = mutex_last_by_addr_.find(addr);
  if (it == mutex_last_by_addr_.end()) {
    return std::nullopt;
  }
  return it->second.vc;
}

void SyncTokenManager::Reset() {
  atomic_last_by_addr_.clear();
  mutex_last_by_addr_.clear();
}

}  // namespace monitor
