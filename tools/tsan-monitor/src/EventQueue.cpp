#include "EventQueue.h"

namespace monitor {
  EventQueue::EventQueue(NotifyFn notify) : notify_(std::move(notify)) {
  }

  void EventQueue::Push(Event event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(event));
    }
    if (notify_) {
      notify_();
    }
  }

  bool EventQueue::TryPop(Event &event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    event = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool EventQueue::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }
} // namespace monitor
