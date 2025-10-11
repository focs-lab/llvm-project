#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

#include "Event.h"

namespace monitor {

class EventQueue {
 public:
  using NotifyFn = std::function<void()>;

  explicit EventQueue(NotifyFn notify);

  void Push(Event event);
  bool TryPop(Event& event);
  bool Empty() const;

 private:
  NotifyFn notify_;
  mutable std::mutex mutex_;
  std::deque<Event> queue_;
};

}  // namespace monitor
