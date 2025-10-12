#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Analyzer.h"
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

 private:
  void Run();

  Analyzer& analyzer_;
  std::atomic<bool>& stop_;
  std::atomic<bool> running_{false};
  std::thread thread_;

  std::mutex queues_mutex_;
  std::unordered_map<int, std::shared_ptr<EventQueue>> queues_;

  std::mutex cv_mutex_;
  std::condition_variable cv_;
  std::size_t pending_notifications_ = 0;
};

}  // namespace monitor
