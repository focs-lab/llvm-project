#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "Analyzer.h"
#include "Constants.h"
#include "Reader.h"
#include "Scheduler.h"
#include "SyncTokenManager.h"

namespace monitor {
// MonitorOptions describes how the out-of-process monitor should attach to the
// instrumented program (target pid, where its channel files live, how often to
// scan for new threads, verbosity, and whether a detected race stops execution).
struct MonitorOptions {
  pid_t pid = -1;
  std::filesystem::path directory;
  bool verbose = false;
  std::chrono::milliseconds refresh_interval = kDefaultRefreshInterval;
  RaceAction race_action = RaceAction::kStop;
};

// MonitorApp drives the entire monitoring session:
//   * discovers per-thread channel files emitted by the runtime
//   * spawns Reader threads that translate ring-buffer slots into Events
//   * feeds events through the Scheduler/Analyzer pipeline
//   * emits race reports and propagates stop/continue decisions
// It exits once the instrumented process terminates and all events are drained.
class MonitorApp {
 public:
  explicit MonitorApp(MonitorOptions options);

  ~MonitorApp();

  int Run();

 private:
  void RefreshChannels();

  void StopAllReaders();

  bool AllChannelsRegistered() const;

  struct ThreadState;

  MonitorOptions options_;
  std::atomic<bool> stop_{false};
  SyncTokenManager sync_tokens_;
  Analyzer analyzer_;
  Scheduler scheduler_;

  std::filesystem::path directory_;
  std::atomic<bool> handshake_sent_{false};

  mutable std::mutex threads_mutex_;
  std::unordered_map<int, std::unique_ptr<ThreadState> > threads_;
};
}  // namespace monitor
