#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "Channel.h"
#include "EventQueue.h"

namespace monitor {

class Reader {
 public:
  Reader(std::shared_ptr<Channel> channel,
         std::shared_ptr<EventQueue> queue,
         std::atomic<bool>& stop);
  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  std::shared_ptr<Channel> channel_;
  std::shared_ptr<EventQueue> queue_;
  std::atomic<bool>& stop_;
  std::thread thread_;
};

}  // namespace monitor
