#include "Scheduler.h"

#include <chrono>

#include "Constants.h"

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
}

// Scheduler::Notify is called
//  1. by EventQueue when a new event is pushed
//  2. by Scheduler when a new queue is registered (new channel found)
void Scheduler::Notify() {
  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    ++pending_notifications_;
  }
  cv_.notify_one();
}

void Scheduler::Run() {
  while (!stop_.load(std::memory_order_acquire)) {
    bool progressed = false; // whether we made any progress in this iteration
    {
      std::lock_guard<std::mutex> lock(queues_mutex_);
      for (auto& [tid, queue] : queues_) {
        Event event;
        if (!queue->TryPop(event)) {
          continue;
        }
        progressed = true;
        const AnalyzerResult result = analyzer_.Process(event);
        if (result == AnalyzerResult::kTerminate) {
          stop_.store(true, std::memory_order_release);
          break;
        }
      }
    }

    if (stop_.load(std::memory_order_acquire)) {
      break;
    }

    if (!progressed) {
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, kSchedulerIdleSleep, [&]() {
        // Wake up when stop_ is set or there are pending notifications
        return stop_.load(std::memory_order_acquire) ||
               pending_notifications_ > 0;
      });
      pending_notifications_ = 0;
    }
  }
}

}  // namespace monitor
