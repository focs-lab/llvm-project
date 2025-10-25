#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <unordered_map>

#include "Constants.h"
#include "Event.h"
#include "Report.h"
#include "State.h"
#include "SyncTokenManager.h"

namespace monitor {
enum class AnalyzerResult {
  kContinue,
  kTerminate,
  kStopOnRace,
};

class Analyzer {
 public:
  explicit Analyzer(bool verbose, RaceAction race_action)
      : verbose_(verbose), race_action_(race_action) {}

  AnalyzerResult Process(const Event& event);

  void SetSyncTokenManager(SyncTokenManager* manager) {
    sync_token_mgr_ = manager;
  }

  // Pass the channels directory (/tmp/tsan.monitor.<pid>) so Report can write the sentinel.
  void SetSentinelDirectory(const std::filesystem::path& dir) {
    report_.SetSentinelDirectory(dir);
  }

  // Set Origin process PID for signal sending
  void SetOriginPid(pid_t pid) {
    report_.SetOriginPid(pid);
  }

  // Get Report instance for direct access
  Report& GetReport() {
    return report_;
  }

 private:
  struct ThreadState {
    VectorClock clock;
    bool alive = true;
    VectorClock pending_spawn_clock;
    bool has_pending_spawn = false;
    int pending_parent_tid = -1;
  };

  ThreadState& GetThreadState(int tid);

  AddressState& GetAddressState(std::uint64_t address);

  bool HandleRead(const Event& event, ThreadState& thread_state,
                  AddressState& address_state, const Epoch& current_epoch);

  bool HandleWrite(const Event& event, ThreadState& thread_state,
                   AddressState& address_state, const Epoch& current_epoch);

  bool ReportRace(RaceKind kind, const Epoch& first, std::uint64_t first_value,
                  const Epoch& second, std::uint64_t second_value,
                  std::uint64_t address);

  static bool HappensBefore(const Epoch& epoch, const VectorClock& clock);

  void PrintEvent(const Event& event);

  // Stage-3: Thread lifecycle HB propagation
  void HandleThreadSpawn(const Event& event, ThreadState& parent_state);
  void HandleThreadStart(const Event& event, ThreadState& child_state);
  void HandleThreadJoin(const Event& event, ThreadState& parent_state);
  void HandleThreadExit(const Event& event, ThreadState& thread_state);

  bool verbose_ = false;
  RaceAction race_action_ = RaceAction::kContinue;
  std::unordered_map<int, ThreadState> threads_;
  std::unordered_map<std::uint64_t, AddressState> addresses_;
  Report report_;
  SyncTokenManager* sync_token_mgr_ = nullptr;

  void HandleMutexLock(const Event& event, ThreadState& thread_state);
  void HandleMutexUnlock(const Event& event, ThreadState& thread_state);
  void HandleAtomicLoad(const Event& event, ThreadState& thread_state);
  void HandleAtomicStore(const Event& event, ThreadState& thread_state);
  void HandleAtomicRMW(const Event& event, ThreadState& thread_state);
  void HandleAtomicCAS(const Event& event, ThreadState& thread_state);
};
}  // namespace monitor
