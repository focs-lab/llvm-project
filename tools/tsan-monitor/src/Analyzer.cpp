#include "Analyzer.h"

#include <spdlog/spdlog.h>

#include <format>
#include <iostream>

namespace monitor {
// Process() is the single entry point from the Scheduler: it updates per-thread
// logical clocks, mirrors synchronization edges, and raises reports once a
// conflicting access pair is observed. It must be deterministic because the
// monitor can replay the channel offline when debugging failures.
AnalyzerResult Analyzer::Process(const Event& event) {
  if (verbose_) {
    // PrintEvent(event);
  }
  if (IsTermination(event)) {
    return AnalyzerResult::kTerminate;
  }

  if (event.id == EventId::kThreadSpawn) {
    ThreadState& parent_state = GetThreadState(event.tid);
    HandleThreadSpawn(event, parent_state);
    return AnalyzerResult::kContinue;
  }
  if (event.id == EventId::kThreadStart) {
    ThreadState& child_state = GetThreadState(event.tid);
    HandleThreadStart(event, child_state);
    return AnalyzerResult::kContinue;
  }
  if (event.id == EventId::kThreadJoin) {
    ThreadState& parent_state = GetThreadState(event.tid);
    HandleThreadJoin(event, parent_state);
    return AnalyzerResult::kContinue;
  }
  if (event.id == EventId::kThreadExit) {
    ThreadState& thread_state = GetThreadState(event.tid);
    HandleThreadExit(event, thread_state);
    return AnalyzerResult::kContinue;
  }

  if (IsMutexEvent(event)) {
    ThreadState& thread_state = GetThreadState(event.tid);
    if (event.id == EventId::kMutexLock) {
      HandleMutexLock(event, thread_state);
    } else {
      HandleMutexUnlock(event, thread_state);
    }
    return AnalyzerResult::kContinue;
  }

  if (IsAtomicEvent(event)) {
    ThreadState& thread_state = GetThreadState(event.tid);
    switch (event.id) {
      case EventId::kAtomicLoad:
        HandleAtomicLoad(event, thread_state);
        break;
      case EventId::kAtomicStore:
        HandleAtomicStore(event, thread_state);
        break;
      case EventId::kAtomicRMW:
        HandleAtomicRMW(event, thread_state);
        break;
      case EventId::kAtomicCAS:
        HandleAtomicCAS(event, thread_state);
        break;
      default:
        break;
    }
    return AnalyzerResult::kContinue;
  }

  if (event.id == EventId::kRead || event.id == EventId::kWrite) {
    ThreadState& thread_state = GetThreadState(event.tid);
    const std::uint64_t current_clock = thread_state.clock.Tick(event.tid);
    SPDLOG_DEBUG("[Analyzer] Tick: T{} -> clock={} VC={}", event.tid,
                 current_clock, thread_state.clock.ToString());

    const Epoch current_epoch{event.tid, current_clock};
    AddressState& address_state = GetAddressState(event.address);

    bool race_detected = false;
    if (event.id == EventId::kRead) {
      race_detected =
          HandleRead(event, thread_state, address_state, current_epoch);
    } else {
      race_detected =
          HandleWrite(event, thread_state, address_state, current_epoch);
    }

    if (race_detected && race_action_ == RaceAction::kStop) {
      return AnalyzerResult::kStopOnRace;
    }
  }

  return AnalyzerResult::kContinue;
}

Analyzer::ThreadState& Analyzer::GetThreadState(int tid) {
  // The map auto-creates entries; Analyzer::ThreadState has sensible defaults.
  return threads_[tid];
}

AddressState& Analyzer::GetAddressState(std::uint64_t address) {
  // AddressState keeps ownership of the vector clock snapshots per location.
  return addresses_[address];
}

// Reads check the last write, then snapshot their own epoch/value into the
// read-set so a later write can detect cross-thread conflicts.
bool Analyzer::HandleRead(const Event& event, ThreadState& thread_state,
                          AddressState& address_state,
                          const Epoch& current_epoch) {
  bool race_detected = false;
  if (address_state.last_write &&
      address_state.last_write->tid != current_epoch.tid &&
      !HappensBefore(*address_state.last_write, thread_state.clock)) {
    race_detected =
        ReportRace(RaceKind::kReadWrite, *address_state.last_write,
                   address_state.last_write_value, current_epoch,
                   event.nargs > 0 ? event.args[0] : 0, event.address);
  }

  const std::uint64_t value = event.nargs > 0 ? event.args[0] : 0;
  address_state.AddOrUpdateRead(current_epoch, value);
  SPDLOG_DEBUG(
      "[Analyzer] Read: T{} addr=0x{:x} epoch=({}, {}) val=0x{:x} VC={}",
      event.tid, event.address, current_epoch.tid, current_epoch.clock, value,
      thread_state.clock.ToString());
  return race_detected;
}

// Writes compare against the previous writer and every outstanding reader, then
// become the new "last write" for the address, clearing transient readers.
bool Analyzer::HandleWrite(const Event& event, ThreadState& thread_state,
                           AddressState& address_state,
                           const Epoch& current_epoch) {
  bool race_detected = false;
  if (address_state.last_write &&
      address_state.last_write->tid != current_epoch.tid &&
      !HappensBefore(*address_state.last_write, thread_state.clock)) {
    race_detected =
        ReportRace(RaceKind::kWriteWrite, *address_state.last_write,
                   address_state.last_write_value, current_epoch,
                   event.nargs > 0 ? event.args[0] : 0, event.address);
  }

  for (const auto& read_entry : address_state.read_set) {
    if (read_entry.epoch.tid == current_epoch.tid) {
      continue;
    }
    if (!HappensBefore(read_entry.epoch, thread_state.clock)) {
      race_detected =
          ReportRace(RaceKind::kWriteRead, read_entry.epoch, read_entry.value,
                     current_epoch, event.nargs > 0 ? event.args[0] : 0,
                     event.address) ||
          race_detected;
    }
  }

  address_state.last_write = current_epoch;
  address_state.last_write_value = event.nargs > 0 ? event.args[0] : 0;
  address_state.ClearReads();
  SPDLOG_DEBUG(
      "[Analyzer] Write: T{} addr=0x{:x} epoch=({}, {}) val=0x{:x} VC={}",
      event.tid, event.address, current_epoch.tid, current_epoch.clock,
      address_state.last_write_value, thread_state.clock.ToString());
  return race_detected;
}

bool Analyzer::ReportRace(RaceKind kind, const Epoch& first,
                          std::uint64_t first_value, const Epoch& second,
                          std::uint64_t second_value, std::uint64_t address) {
  RaceEventInfo info;
  info.kind = kind;
  info.address = address;
  info.first = first;
  info.second = second;
  info.first_value = first_value;
  info.second_value = second_value;
  report_.OnRace(info);
  return true;
}

bool Analyzer::HappensBefore(const Epoch& epoch, const VectorClock& clock) {
  if (!epoch.IsValid()) {
    return true;
  }
  return clock.Get(epoch.tid) >= epoch.clock;
}

void Analyzer::PrintEvent(const Event& event) {
  std::string message =
      std::format("[tid={}] {} lap={} addr=0x{:012x} nargs={}", event.tid,
                  EventName(event.id), static_cast<int>(event.lap),
                  event.address, event.nargs);
  for (std::size_t i = 0; i < event.nargs; ++i) {
    message += std::format(" arg{}=0x{:x}", i, event.args[i]);
  }
  message += std::format(" index={}", event.index);
  std::cout << message << std::endl;
}

void Analyzer::HandleThreadSpawn(const Event& event,
                                 Analyzer::ThreadState& parent_state) {
  const int child_tid = static_cast<int>(event.args[0]);
  ThreadState& child_state = GetThreadState(child_tid);

  SPDLOG_DEBUG("[Analyzer] Spawn: parent T{} VC before tick={}", event.tid,
               parent_state.clock.ToString());

  const std::uint64_t parent_clock = parent_state.clock.Tick(event.tid);
  child_state.pending_spawn_clock = parent_state.clock;
  child_state.has_pending_spawn = true;
  child_state.pending_parent_tid = event.tid;
  child_state.clock = VectorClock();

  SPDLOG_DEBUG(
      "[Analyzer] Spawn: parent T{} ticked -> clock={} VC={} (stored for child "
      "T{}):{}",
      event.tid, parent_clock, parent_state.clock.ToString(), child_tid,
      child_state.pending_spawn_clock.ToString());
}

void Analyzer::HandleThreadStart(const Event& event,
                                 Analyzer::ThreadState& child_state) {
  SPDLOG_DEBUG("[Analyzer] Start: child T{} VC before acquire={} pending={}",
               event.tid, child_state.clock.ToString(),
               child_state.has_pending_spawn);

  if (child_state.has_pending_spawn) {
    child_state.clock.Merge(child_state.pending_spawn_clock);
    child_state.has_pending_spawn = false;
    SPDLOG_DEBUG("[Analyzer] Start: child T{} merged parent({}) VC -> {}",
                 event.tid, child_state.pending_parent_tid,
                 child_state.clock.ToString());
    child_state.pending_parent_tid = -1;
    child_state.pending_spawn_clock = VectorClock();
  }

  const std::uint64_t child_clock = child_state.clock.Tick(event.tid);
  child_state.alive = true;
  SPDLOG_DEBUG("[Analyzer] Start: child T{} ticked -> clock={} VC={}",
               event.tid, child_clock, child_state.clock.ToString());
}

void Analyzer::HandleThreadJoin(const Event& event,
                                Analyzer::ThreadState& parent_state) {
  const int child_tid = static_cast<int>(event.args[0]);
  ThreadState& child_state = GetThreadState(child_tid);

  SPDLOG_DEBUG("[Analyzer] Join: parent T{} VC before={}", event.tid,
               parent_state.clock.ToString());
  SPDLOG_DEBUG("[Analyzer] Join: child T{} VC={}", child_tid,
               child_state.clock.ToString());

  for (const auto& entry : child_state.clock.Entries()) {
    const int t = entry.first;
    const std::uint64_t v = entry.second;
    const std::uint64_t cur = parent_state.clock.Get(t);
    if (v > cur) {
      parent_state.clock.Set(t, v);
    }
  }

  SPDLOG_DEBUG("[Analyzer] Join: parent T{} VC after merge={}", event.tid,
               parent_state.clock.ToString());

  const std::uint64_t parent_clock = parent_state.clock.Tick(event.tid);
  SPDLOG_DEBUG("[Analyzer] Join: parent T{} ticked -> clock={} VC={}",
               event.tid, parent_clock, parent_state.clock.ToString());

  child_state.alive = false;
}

void Analyzer::HandleThreadExit(const Event& event,
                                Analyzer::ThreadState& thread_state) {
  SPDLOG_DEBUG("[Analyzer] Exit: thread T{} VC={}", event.tid,
               thread_state.clock.ToString());
}

void Analyzer::HandleMutexLock(const Event& event,
                               Analyzer::ThreadState& thread_state) {
  const std::uint64_t count = ExtractCount(event);
  if (sync_token_mgr_) {
    if (auto published =
            sync_token_mgr_->TryAcquireMutexLock(event.address, count)) {
      thread_state.clock.Merge(*published);
    }
  }
  thread_state.clock.Tick(event.tid);
  SPDLOG_DEBUG("[Analyzer] MutexLock: T{} addr=0x{:x} count={} VC={}",
               event.tid, event.address, count, thread_state.clock.ToString());
}

void Analyzer::HandleMutexUnlock(const Event& event,
                                 Analyzer::ThreadState& thread_state) {
  const std::uint64_t count = ExtractCount(event);
  if (sync_token_mgr_) {
    sync_token_mgr_->PublishMutexUnlock(event.address, count,
                                        thread_state.clock, event.tid);
  }
  thread_state.clock.Tick(event.tid);
  SPDLOG_DEBUG("[Analyzer] MutexUnlock: T{} addr=0x{:x} count={} VC={}",
               event.tid, event.address, count, thread_state.clock.ToString());
}

void Analyzer::HandleAtomicLoad(const Event& event,
                                Analyzer::ThreadState& thread_state) {
  const std::uint64_t count = ExtractCount(event);
  const std::uint32_t mo = ExtractMemoryOrder(event);
  if (sync_token_mgr_) {
    if (auto published =
            sync_token_mgr_->TryAcquireAtomic(event.address, count, mo)) {
      thread_state.clock.Merge(*published);
    }
  }
  thread_state.clock.Tick(event.tid);
  SPDLOG_DEBUG("[Analyzer] AtomicLoad: T{} addr=0x{:x} count={} mo={} VC={}",
               event.tid, event.address, count, mo,
               thread_state.clock.ToString());
}

void Analyzer::HandleAtomicStore(const Event& event,
                                 Analyzer::ThreadState& thread_state) {
  //
  // ATOMIC STORE HANDLING
  // =====================
  // Atomic stores with release semantics establish happens-before relationships.
  // We publish synchronization tokens that later acquire operations can consume.
  //
  const std::uint64_t count = ExtractCount(event);
  const std::uint32_t mo = ExtractMemoryOrder(event);

  // For release semantics, publish synchronization token
  if (sync_token_mgr_ && IsReleaseOrder(mo)) {
    sync_token_mgr_->PublishAtomic(event.address, count, thread_state.clock,
                                   event.tid, mo);
  }

  // Advance thread's vector clock
  thread_state.clock.Tick(event.tid);

  SPDLOG_DEBUG("[Analyzer] AtomicStore: T{} addr=0x{:x} count={} mo={} VC={}",
               event.tid, event.address, count, mo,
               thread_state.clock.ToString());
}

void Analyzer::HandleAtomicRMW(const Event& event,
                               Analyzer::ThreadState& thread_state) {
  const std::uint64_t count = ExtractCount(event);
  const std::uint32_t mo = ExtractMemoryOrder(event);
  if (sync_token_mgr_) {
    if (IsAcquireOrder(mo)) {
      if (auto published =
              sync_token_mgr_->TryAcquireAtomic(event.address, count, mo)) {
        thread_state.clock.Merge(*published);
      }
    }
    if (IsReleaseOrder(mo)) {
      sync_token_mgr_->PublishAtomic(event.address, count, thread_state.clock,
                                     event.tid, mo);
    }
  }
  thread_state.clock.Tick(event.tid);
  SPDLOG_DEBUG("[Analyzer] AtomicRMW: T{} addr=0x{:x} count={} mo={} VC={}",
               event.tid, event.address, count, mo,
               thread_state.clock.ToString());
}

void Analyzer::HandleAtomicCAS(const Event& event,
                               Analyzer::ThreadState& thread_state) {
  const std::uint64_t count = ExtractCount(event);
  const std::uint32_t mo = ExtractMemoryOrder(event);
  const bool success = ExtractSuccess(event);
  if (sync_token_mgr_) {
    if (IsAcquireOrder(mo)) {
      if (auto published =
              sync_token_mgr_->TryAcquireAtomic(event.address, count, mo)) {
        thread_state.clock.Merge(*published);
      }
    }
    if (success && IsReleaseOrder(mo)) {
      sync_token_mgr_->PublishAtomic(event.address, count, thread_state.clock,
                                     event.tid, mo);
    }
  }
  thread_state.clock.Tick(event.tid);
  SPDLOG_DEBUG(
      "[Analyzer] AtomicCAS: T{} addr=0x{:x} count={} mo={} success={} VC={}",
      event.tid, event.address, count, mo, success,
      thread_state.clock.ToString());
}

}  // namespace monitor
