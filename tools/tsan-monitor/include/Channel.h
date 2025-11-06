#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "Event.h"

namespace monitor {
class Channel {
 public:
  using Slot = std::atomic<std::uint64_t>;

  static std::shared_ptr<Channel> Open(const std::filesystem::path& path,
                                       int tid, std::string& error);

  ~Channel();

  Channel(const Channel&) = delete;

  Channel& operator=(const Channel&) = delete;

  int tid() const { return tid_; }
  const std::filesystem::path& path() const { return path_; }

  void SignalReady();

  bool TryRead(Event& event);

 private:
  Channel(const std::filesystem::path& path, int tid, Slot* slots);

  bool DecodeHeader(std::uint64_t header, Event& event) const;

  std::filesystem::path path_;  // Path to channel file
  int tid_ = -1;                // Thread ID for this channel
  Slot* slots_ = nullptr;        // Memory-mapped circular buffer
  std::uint64_t next_index_ = 0; // Logical index in circular buffer
  bool aligned_ = false;         // Whether lap alignment has been performed
};
}  // namespace monitor
