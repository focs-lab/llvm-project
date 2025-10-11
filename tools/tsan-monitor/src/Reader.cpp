#include "Reader.h"

#include <chrono>
#include <iostream>

#include "Constants.h"

namespace monitor {

Reader::Reader(std::shared_ptr<Channel> channel,
               std::shared_ptr<EventQueue> queue,
               std::atomic<bool>& stop)
    : channel_(std::move(channel)),
      queue_(std::move(queue)),
      stop_(stop) {}

Reader::~Reader() { Stop(); }

void Reader::Start() {
  thread_ = std::thread(&Reader::Run, this);
}

void Reader::Stop() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Reader::Run() {
  while (!stop_.load(std::memory_order_acquire)) {
    Event event;
    if (!channel_->TryRead(event)) {
      std::this_thread::sleep_for(kReaderIdleSleep);
      continue;
    }
    queue_->Push(std::move(event));
  }
}

}  // namespace monitor
