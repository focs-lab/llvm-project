#include "MonitorApp.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <thread>

#include <spdlog/spdlog.h>

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
      analyzer_(options_.verbose, options_.race_action),
      scheduler_(analyzer_, stop_),
      directory_(options_.directory) {
  }

  MonitorApp::~MonitorApp() {
    StopAllReaders();
    scheduler_.Stop();
    spdlog::debug("MonitorApp stopped");
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

      if (scheduler_.TerminationRequested()) {
        if (!scheduler_.HasPendingEvents() && AllChannelsRegistered()) {
          stop_.store(true, std::memory_order_release);
          spdlog::debug("MonitorApp stopping after program end");
          break;
        }
      }

      if (stop_.load(std::memory_order_acquire)) {
        break;
      }

      std::this_thread::sleep_for(options_.refresh_interval);
    }

    return EXIT_SUCCESS;
  }

  void MonitorApp::RefreshChannels() {
    for (const auto &entry: std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto filename = entry.path().filename().string();
      char *endptr = nullptr;
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

      spdlog::debug("MonitorApp: registered channel {}", tid);
    }
  }

  void MonitorApp::StopAllReaders() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto &[tid, state]: threads_) {
      if (state && state->reader) {
        state->reader->Stop();
      }
    }
    threads_.clear();
  }

  bool MonitorApp::AllChannelsRegistered() const {
    for (const auto &entry: std::filesystem::directory_iterator(directory_)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto filename = entry.path().filename().string();
      char *endptr = nullptr;
      long tid_value = std::strtol(filename.c_str(), &endptr, 10);
      if (endptr == nullptr || *endptr != '\0') {
        continue;
      }
      if (tid_value < 0 || tid_value > std::numeric_limits<int>::max()) {
        continue;
      }
      const int tid = static_cast<int>(tid_value);
      std::lock_guard<std::mutex> lock(threads_mutex_);
      if (threads_.find(tid) == threads_.end()) {
        spdlog::debug("MonitorApp: channel {} not yet registered", tid);
        return false;
      }
    }
    spdlog::debug("MonitorApp: all channels registered");
    return true;
  }
} // namespace monitor
