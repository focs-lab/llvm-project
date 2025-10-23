#include "Scheduler.h"

#include <spdlog/spdlog.h>

#include <chrono>

#include "Constants.h"
#include "SyncTokenManager.h"

namespace monitor {
Scheduler::Scheduler(Analyzer& analyzer, std::atomic<bool>& stop_flag)
    : analyzer_(analyzer), stop_(stop_flag) {}

Scheduler::~Scheduler() { Stop(); }

std::shared_ptr<EventQueue> Scheduler::RegisterQueue(int tid) {
  auto queue = std::make_shared<EventQueue>([this]() { this->Notify(); });
  {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    queues_.emplace(tid, queue);
  }
  Notify();
  return queue;
}

void Scheduler::Start() {
  if (running_.exchange(true)) {
    return;
  }
  thread_ = std::thread(&Scheduler::Run, this);
}

void Scheduler::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_.store(true, std::memory_order_release);
  Notify();
  if (thread_.joinable()) {
    thread_.join();
  }
  SPDLOG_DEBUG("Scheduler stopped");
}

bool Scheduler::TerminationRequested() const {
  return termination_requested_.load(std::memory_order_acquire);
}

bool Scheduler::HasPendingEvents() const {
  std::lock_guard<std::mutex> lock(queues_mutex_);
  return !QueuesEmptyLocked();
}

bool Scheduler::RaceDetected() const {
  return race_detected_.load(std::memory_order_acquire);
}

void Scheduler::Notify() {
  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    ++pending_notifications_;
  }
  cv_.notify_one();
}

void Scheduler::Run() {
  while (!stop_.load(std::memory_order_acquire)) {
    bool progressed = false;
    {
      std::lock_guard<std::mutex> lock(queues_mutex_);
      auto handle_result = [&](AnalyzerResult result) -> bool {
        if (result == AnalyzerResult::kTerminate) {
          termination_requested_.store(true, std::memory_order_release);
          SPDLOG_DEBUG("Scheduler observed termination marker");
        } else if (result == AnalyzerResult::kStopOnRace) {
          race_detected_.store(true, std::memory_order_release);
          stop_.store(true, std::memory_order_release);
          SPDLOG_DEBUG("Scheduler stopping due to race detection");
          return true;
        }
        return false;
      };

      for (auto& [tid, queue] : queues_) {
        Event event;
        if (!queue->TryPeek(event)) {
          continue;
        }

        AnalyzerResult result = AnalyzerResult::kContinue;

        if (event.id == EventId::kProgramEndMarker) {
          (void)queue->TryPop(event);
          progressed = true;
          result = analyzer_.Process(event);
          if (handle_result(result)) {
            break;
          }
          continue;
        }

        if (dep_graph_.IsThreadBlocked(tid)) {
          if (dep_graph_.IsSpawnBlocked(tid)) {
            SPDLOG_DEBUG("[Scheduler] Thread {} spawn-blocked", tid);
          } else if (dep_graph_.IsJoinBlocked(tid)) {
            SPDLOG_DEBUG("[Scheduler] Thread {} join-blocked on child {}", tid,
                         dep_graph_.GetBlockingOnChild(tid).value_or(-1));
          }
          continue;
        }

        if (event.id == EventId::kThreadSpawn) {
          (void)queue->TryPop(event);
          progressed = true;
          const int child_tid = static_cast<int>(event.args[0]);
          dep_graph_.OnSpawnProcessed(tid, child_tid);
          result = analyzer_.Process(event);
        } else if (event.id == EventId::kThreadExit) {
          (void)queue->TryPop(event);
          progressed = true;
          dep_graph_.OnExitProcessed(tid);
          result = analyzer_.Process(event);
        } else if (event.id == EventId::kThreadJoin) {
          const int child_tid = static_cast<int>(event.args[0]);
          if (!dep_graph_.TryConsumeJoinDependency(tid, child_tid)) {
            // Join not ready - thread becomes blocked
            // Event stays in queue, entire thread queue will be skipped next
            // iteration
            SPDLOG_DEBUG(
                "[Scheduler] Join parent={} child={} not ready, thread blocked",
                tid, child_tid);
            continue;
          }
          // Join ready - pop and process
          (void)queue->TryPop(event);
          progressed = true;
          result = analyzer_.Process(event);
        } else if (IsAtomicEvent(event) || IsMutexEvent(event)) {
          if (!IsSyncEventReady(event)) {
            SPDLOG_DEBUG(
                "[Scheduler] T{} sync event not ready addr=0x{:x} count={}",
                tid, event.address, ExtractCount(event));
            continue;
          }
          (void)queue->TryPop(event);
          progressed = true;
          result = analyzer_.Process(event);
          AdvanceSyncCounter(event);
        } else {
          // Regular event - just process (spawn gate already checked above)
          (void)queue->TryPop(event);
          progressed = true;
          result = analyzer_.Process(event);
        }
        if (handle_result(result)) {
          break;
        }
      }
    }

    if (!progressed) {
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, kSchedulerIdleSleep, [&]() {
        return stop_.load(std::memory_order_acquire) ||
               pending_notifications_ > 0;
      });
      pending_notifications_ = 0;
    }

    if (stop_.load(std::memory_order_acquire)) {
      break;
    }
  }
}

bool Scheduler::QueuesEmptyLocked() const {
  for (const auto& [tid, queue] : queues_) {
    if (!queue->Empty()) {
      return false;
    }
  }
  return true;
}

bool Scheduler::IsSyncEventReady(const Event& event) {
  if (IsAtomicEvent(event)) {
    auto& expected = atomic_next_counter_[event.address];
    if (expected == 0) {
      expected = ExtractCount(event);
      return true;
    }
    return ExtractCount(event) == expected;
  }
  if (IsMutexEvent(event)) {
    auto& expected = mutex_next_counter_[event.address];
    if (expected == 0) {
      expected = ExtractCount(event);
      return true;
    }
    return ExtractCount(event) == expected;
  }
  return true;
}

void Scheduler::AdvanceSyncCounter(const Event& event) {
  if (IsAtomicEvent(event)) {
    auto& expected = atomic_next_counter_[event.address];
    if (expected == 0) {
      expected = ExtractCount(event);
    }
    ++expected;
  } else if (IsMutexEvent(event)) {
    auto& expected = mutex_next_counter_[event.address];
    if (expected == 0) {
      expected = ExtractCount(event);
    }
    ++expected;
  }
}
}  // namespace monitor
