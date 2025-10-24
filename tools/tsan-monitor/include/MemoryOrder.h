#pragma once

#include <cstdint>

namespace monitor {

enum class MemoryOrder : std::uint32_t {
  kRelaxed = 0,
  kConsume = 1,
  kAcquire = 2,
  kRelease = 3,
  kAcquireRelease = 4,
  kSeqCst = 5,
};

inline bool IsAcquireOrder(std::uint32_t mo) {
  return mo == static_cast<std::uint32_t>(MemoryOrder::kConsume) ||
         mo == static_cast<std::uint32_t>(MemoryOrder::kAcquire) ||
         mo == static_cast<std::uint32_t>(MemoryOrder::kAcquireRelease) ||
         mo == static_cast<std::uint32_t>(MemoryOrder::kSeqCst);
}

inline bool IsReleaseOrder(std::uint32_t mo) {
  return mo == static_cast<std::uint32_t>(MemoryOrder::kRelease) ||
         mo == static_cast<std::uint32_t>(MemoryOrder::kAcquireRelease) ||
         mo == static_cast<std::uint32_t>(MemoryOrder::kSeqCst);
}

inline bool IsSeqCstOrder(std::uint32_t mo) {
  return mo == static_cast<std::uint32_t>(MemoryOrder::kSeqCst);
}

}  // namespace monitor
