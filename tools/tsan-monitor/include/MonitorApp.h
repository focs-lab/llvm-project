#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "Analyzer.h"
#include "Constants.h"
#include "Scheduler.h"
#include "Reader.h"

namespace monitor {

struct MonitorOptions {
  pid_t pid = -1;
  std::filesystem::path directory;
  bool verbose = false;
  std::chrono::milliseconds refresh_interval = kDefaultRefreshInterval;
};

class MonitorApp {
 public:
 explicit MonitorApp(MonitorOptions options);
  ~MonitorApp();
  int Run();

 private:
  void RefreshChannels();
  void StopAllReaders();

  struct ThreadState;

  MonitorOptions options_;
  std::atomic<bool> stop_{false};
  Analyzer analyzer_;
  Scheduler scheduler_;

  std::filesystem::path directory_;
  std::atomic<bool> handshake_sent_{false};

  std::mutex threads_mutex_;
  std::unordered_map<int, std::unique_ptr<ThreadState>> threads_;
};

}  // namespace monitor
