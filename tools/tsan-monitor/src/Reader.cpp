#include "Reader.h"

#include <spdlog/spdlog.h>

#include <chrono>

#include "Constants.h"

namespace monitor {
Reader::Reader(std::shared_ptr<Channel> channel,
               std::shared_ptr<EventQueue> queue, std::atomic<bool>& stop)
    : channel_(std::move(channel)), queue_(std::move(queue)), stop_(stop) {}

void Reader::Start() { thread_ = std::thread(&Reader::Run, this); }

void Reader::Stop() {
  if (thread_.joinable()) {
    thread_.join();
  }
  SPDLOG_DEBUG("Reader {} stopped", channel_->tid());
}

void Reader::Run() {
  // Tight polling loop: read channel slots into events and forward them to the
  // central EventQueue. Sleeping keeps CPU usage in check when the producer is
  // idle.
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
