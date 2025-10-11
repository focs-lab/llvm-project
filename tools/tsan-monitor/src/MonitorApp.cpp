#include "MonitorApp.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <thread>

#include "Channel.h"
#include "Constants.h"
#include "Reader.h"

namespace monitor {

struct MonitorApp::ThreadState {
  int tid;
  std::shared_ptr<Channel> channel;
  std::shared_ptr<EventQueue> queue;
  std::unique_ptr<Reader> reader;
};

MonitorApp::MonitorApp(MonitorOptions options)
    : options_(std::move(options)),
      analyzer_(options_.verbose),
      scheduler_(analyzer_, stop_),
      directory_(options_.directory) {}

MonitorApp::~MonitorApp() {
  StopAllReaders();
  scheduler_.Stop();
}

int MonitorApp::Run() {
  if (directory_.empty()) {
    std::cerr << "monitor directory not specified" << std::endl;
    return EXIT_FAILURE;
  }
  if (!std::filesystem::exists(directory_)) {
    std::cerr << "monitor directory " << directory_ << " does not exist"
              << std::endl;
    return EXIT_FAILURE;
  }

  scheduler_.Start();

  while (!stop_.load(std::memory_order_acquire)) {
    RefreshChannels();
    if (stop_.load(std::memory_order_acquire)) {
      break;
    }
    std::this_thread::sleep_for(options_.refresh_interval);
  }

  StopAllReaders();
  scheduler_.Stop();
  return EXIT_SUCCESS;
}

void MonitorApp::RefreshChannels() {
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    const auto filename = entry.path().filename().string();
    char* endptr = nullptr;
    long tid_value = std::strtol(filename.c_str(), &endptr, 10);
    if (endptr == nullptr || *endptr != '\0') {
      continue;
    }
    if (tid_value < 0 || tid_value > std::numeric_limits<int>::max()) {
      continue;
    }
    const int tid = static_cast<int>(tid_value);

    {
      std::lock_guard<std::mutex> lock(threads_mutex_);
      if (threads_.find(tid) != threads_.end()) {
        continue;
      }
    }

    std::string error;
    auto channel = Channel::Open(entry.path(), tid, error);
    if (!channel) {
      std::cerr << "failed to open channel for tid " << tid << ": " << error
                << std::endl;
      continue;
    }

    if (tid == 0 && !handshake_sent_.exchange(true)) {
      // Thread 0 call only
      channel->SignalReady();
    }

    auto queue = scheduler_.RegisterQueue(tid);
    auto reader = std::make_unique<Reader>(channel, queue, stop_);
    reader->Start();

    auto state = std::make_unique<ThreadState>();
    state->tid = tid;
    state->channel = std::move(channel);
    state->queue = std::move(queue);
    state->reader = std::move(reader);

    {
      std::lock_guard<std::mutex> lock(threads_mutex_);
      threads_.emplace(tid, std::move(state));
    }
  }
}

void MonitorApp::StopAllReaders() {
  std::lock_guard<std::mutex> lock(threads_mutex_);
  for (auto& [tid, state] : threads_) {
    if (state && state->reader) {
      state->reader->Stop();
    }
  }
  threads_.clear();
}

}  // namespace monitor
