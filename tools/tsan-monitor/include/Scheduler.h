#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Analyzer.h"
#include "DependencyGraph.h"
#include "EventQueue.h"

namespace monitor {
class Scheduler {
 public:
  Scheduler(Analyzer& analyzer, std::atomic<bool>& stop_flag);

  ~Scheduler();

  Scheduler(const Scheduler&) = delete;

  Scheduler& operator=(const Scheduler&) = delete;

  std::shared_ptr<EventQueue> RegisterQueue(int tid);

  void Start();

  void Stop();

  void Notify();

  bool TerminationRequested() const;

  bool HasPendingEvents() const;

  bool RaceDetected() const;

 private:
  void Run();

  bool QueuesEmptyLocked() const;

  Analyzer& analyzer_;
  std::atomic<bool>& stop_;
  std::atomic<bool> running_{false};
  std::thread thread_;

  mutable std::mutex queues_mutex_;
  std::unordered_map<int, std::shared_ptr<EventQueue> > queues_;

  std::atomic<bool> termination_requested_{false};
  std::atomic<bool> race_detected_{false};

  std::mutex cv_mutex_;
  std::condition_variable cv_;
  std::size_t pending_notifications_ = 0;

  // Stage-3 V3: thread-level dependency graph
  DependencyGraph dep_graph_;
};
}  // namespace monitor
