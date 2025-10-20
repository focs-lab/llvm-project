#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Event.h"

namespace monitor {

// DependencyGraph models event-order dependencies (spawn/exit/join hierarchy)
// V3 Design: Thread-level blocking instead of event-level defer
class DependencyGraph {
 public:
  DependencyGraph() = default;

  // Spawn gating -----------------------------------------------------------
  void OnSpawnProcessed(int parent_tid, int child_tid);
  bool IsSpawnGateOpen(int tid) const;

  // Exit -> Join dependency ------------------------------------------------
  void OnExitProcessed(int tid);
  bool HasExited(int tid) const;
  bool TryConsumeJoinDependency(int parent_tid, int child_tid);

  // Thread blocking --------------------------------------------------------
  bool IsSpawnBlocked(int tid) const;
  bool IsJoinBlocked(int tid) const;
  bool IsThreadBlocked(int tid) const;
  std::optional<int> GetBlockingOnChild(int tid) const;

 private:
  // Spawn tracking:
  std::unordered_set<int> spawned_children_;

  // Exit tracking:
  std::unordered_set<int> exited_threads_;

  // Join blocking:
  std::unordered_map<int, int> blocked_on_join_;  // p_tid -> c_tid
  std::unordered_map<int, std::unordered_set<int>>
      parents_waiting_on_;  // c_tid -> [p_tid ...]
};

}  // namespace monitor
