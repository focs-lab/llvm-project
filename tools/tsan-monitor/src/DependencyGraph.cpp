#include "DependencyGraph.h"

#include <spdlog/spdlog.h>

namespace monitor {

void DependencyGraph::OnSpawnProcessed(int parent_tid, int child_tid) {
  spawned_children_.insert(child_tid);
  SPDLOG_DEBUG("[DepGraph] Spawn processed: parent={} child={}", parent_tid,
               child_tid);
}

bool DependencyGraph::IsSpawnGateOpen(int tid) const {
  if (tid == 0)  // main thread considered spawned
    return true;
  return spawned_children_.count(tid) != 0;
}

void DependencyGraph::OnExitProcessed(int tid) {
  exited_threads_.insert(tid);
  SPDLOG_DEBUG("[DepGraph] Exit processed for thread {}", tid);

  // Unblock all parents waiting for this child; do not replay events here.
  auto it = parents_waiting_on_.find(tid);
  if (it != parents_waiting_on_.end()) {
    std::vector<int> to_unblock;
    to_unblock.reserve(it->second.size());
    for (int parent_tid : it->second) {
      // Only unblock now; Scheduler will naturally process the Join still at
      // the queue head. For nested scenarios, TryConsumeJoinDependency will
      // re-check descendants upon retry.
      auto itp = blocked_on_join_.find(parent_tid);
      if (itp != blocked_on_join_.end() && itp->second == tid) {
        blocked_on_join_.erase(itp);
        to_unblock.push_back(parent_tid);
      }
    }
    SPDLOG_DEBUG("[DepGraph] Unblocked {} parent(s) waiting on child {}",
                 to_unblock.size(), tid);
    parents_waiting_on_.erase(it);
  }
}

bool DependencyGraph::HasExited(int tid) const {
  return exited_threads_.count(tid) != 0;
}

bool DependencyGraph::TryConsumeJoinDependency(int parent_tid, int child_tid) {
  if (!HasExited(child_tid)) {
    blocked_on_join_[parent_tid] = child_tid;
    parents_waiting_on_[child_tid].insert(parent_tid);
    SPDLOG_DEBUG("[DepGraph] Join parent={} waiting for child={}", parent_tid,
                 child_tid);
    return false;
  }

  SPDLOG_DEBUG("[DepGraph] Join parent={} child={} ready", parent_tid,
               child_tid);
  return true;
}

bool DependencyGraph::IsSpawnBlocked(int tid) const {
  // Thread is spawn-blocked if it hasn't been spawned yet
  // Exception: main thread (tid=0) is never spawn-blocked
  return tid != 0 && !IsSpawnGateOpen(tid);
}

bool DependencyGraph::IsJoinBlocked(int tid) const {
  return blocked_on_join_.count(tid) != 0;
}

bool DependencyGraph::IsThreadBlocked(int tid) const {
  return IsSpawnBlocked(tid) || IsJoinBlocked(tid);
}

std::optional<int> DependencyGraph::GetBlockingOnChild(int tid) const {
  auto it = blocked_on_join_.find(tid);
  if (it == blocked_on_join_.end()) return std::nullopt;
  return it->second;
}

}  // namespace monitor
