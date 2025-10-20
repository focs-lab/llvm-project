#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace monitor {
struct Epoch {
  int tid = -1;
  std::uint64_t clock = 0;

  bool IsValid() const { return tid >= 0; }
};

class VectorClock {
 public:
  std::uint64_t Get(int tid) const;

  void Set(int tid, std::uint64_t value);

  std::uint64_t Tick(int tid);

  const std::unordered_map<int, std::uint64_t>& Entries() const {
    return clock_;
  }

  std::string ToString() const;

 private:
  std::unordered_map<int, std::uint64_t> clock_;
};

struct ReadEntry {
  Epoch epoch;
  std::uint64_t value = 0;
};

struct AddressState {
  std::optional<Epoch> last_write;
  std::uint64_t last_write_value = 0;
  std::vector<ReadEntry> read_set;

  void ClearReads();

  void AddOrUpdateRead(const Epoch& epoch, std::uint64_t value);
};
}  // namespace monitor
