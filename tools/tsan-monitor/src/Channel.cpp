#include "Channel.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "Constants.h"

namespace monitor {
namespace {
constexpr std::uint64_t kAddrMask = (1ULL << 48) - 1;
constexpr std::uint64_t kLapMask = 0xF;
}  // namespace

std::shared_ptr<Channel> Channel::Open(const std::filesystem::path& path,
                                       int tid, std::string& error) {
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    error = std::string("open failed: ") + std::strerror(errno);
    return nullptr;
  }

  struct stat st = {};
  if (fstat(fd, &st) != 0) {
    error = std::string("fstat failed: ") + std::strerror(errno);
    ::close(fd);
    return nullptr;
  }
  if (st.st_size == 0) {
    error = "file size is zero";
    ::close(fd);
    return nullptr;
  }

  // Size must be 0x1000 * 64 (aka. 32768 * 8B = 256KB)
  void* mapping =
      ::mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    error = std::string("mmap failed: ") + std::strerror(errno);
    ::close(fd);
    return nullptr;
  }

  auto* slots = reinterpret_cast<Slot*>(mapping);
  auto deleter = [fd, mapping,
                  size = static_cast<std::size_t>(st.st_size)](Channel* c) {
    SPDLOG_DEBUG("Channel {} closed", c->tid_);
    delete c;
    ::munmap(mapping, size);
    ::close(fd);
  };

  auto channel =
      std::shared_ptr<Channel>(new Channel(path, tid, slots), deleter);
  return channel;
}

Channel::Channel(const std::filesystem::path& path, int tid, Slot* slots)
    : path_(path), tid_(tid), slots_(slots) {}

Channel::~Channel() = default;

void Channel::SignalReady() {
  if (!slots_) {
    return;
  }
  slots_[0].store(kMonitorReadyMagic, std::memory_order_release);
}

bool Channel::DecodeHeader(std::uint64_t header, Event& event) const {
  if (header == 0) {
    return false;
  }
  if (header == kMonitorReadyMagic) {
    event.id = EventId::kMonitorReady;
    event.lap = 0;
    event.address = 0;
    event.nargs = 0;
    return true;
  }
  if (header == kProgramEndedMagic) {
    event.id = EventId::kProgramEndMarker;
    event.lap = 0;
    event.address = 0;
    event.nargs = 0;
    return true;
  }

  const std::uint8_t raw_eid = static_cast<std::uint8_t>((header >> 56) & 0xFF);
  if (raw_eid == 0)
    return false;

  const auto eid = static_cast<EventId>(raw_eid);
  const std::uint8_t lap = static_cast<std::uint8_t>((header >> 52) & kLapMask);
  if (lap > kLapMask)
    return false;

  const std::uint64_t addr = header & kAddrMask;
  const std::size_t nargs = EventArgCount(eid);
  if (nargs > kMaxEventArgs)
    return false;

  event.id = eid;
  event.lap = lap;
  event.address = addr;
  event.nargs = nargs;
  return true;
}

bool Channel::TryRead(Event& event) {
  if (!slots_) {
    return false;
  }

  const std::size_t slot_index =
      static_cast<std::size_t>(next_index_ & kSlotMask);
  const std::uint64_t header =
      slots_[slot_index].load(std::memory_order_acquire);
  if (header == 0) {
    return false;
  }

  if (!DecodeHeader(header, event)) {
    return false;
  }

  event.raw_header = header;
  event.index = next_index_;
  event.tid = tid_;

  if (event.id == EventId::kMonitorReady ||
      event.id == EventId::kProgramEndMarker) {
    ++next_index_;
    return true;
  }

  {
    const std::uint8_t observed_lap = event.lap;
    std::uint8_t expected_lap = static_cast<std::uint8_t>((next_index_ / kSlotsPerChannel) & kLapMask);

    SPDLOG_DEBUG("Channel {} inspecting slot {} index {} lap {} expected {}", tid_,
                 static_cast<unsigned>(slot_index), static_cast<unsigned long long>(next_index_),
                 static_cast<int>(observed_lap), static_cast<int>(expected_lap));

    if (!aligned_) {
      const std::size_t slot = static_cast<std::size_t>(next_index_ & kSlotMask);
      next_index_ = static_cast<std::uint64_t>(observed_lap) * kSlotsPerChannel + slot;
      aligned_ = true;
      expected_lap = observed_lap;
      event.index = next_index_;
      SPDLOG_DEBUG("Channel {} initial align -> index {} lap {}", tid_,
                   static_cast<unsigned long long>(next_index_), static_cast<int>(observed_lap));
    } else {
      if (observed_lap < expected_lap) {
        SPDLOG_DEBUG("Channel {} waiting: observed lap {} < expected {}", tid_,
                     static_cast<int>(observed_lap), static_cast<int>(expected_lap));
        return false;
      }
      if (observed_lap > expected_lap) {
        SPDLOG_WARN("Channel {} catching up: expected lap {} but saw {}", tid_,
                    static_cast<int>(expected_lap), static_cast<int>(observed_lap));
        const std::size_t slot = static_cast<std::size_t>(next_index_ & kSlotMask);
        next_index_ = static_cast<std::uint64_t>(observed_lap) * kSlotsPerChannel + slot;
        expected_lap = observed_lap;
        event.index = next_index_;
        SPDLOG_DEBUG("Channel {} advanced index -> {} lap {}", tid_,
                     static_cast<unsigned long long>(next_index_), static_cast<int>(observed_lap));
      }
    }
  }

  const std::size_t nargs = event.nargs;
  if (nargs > kMaxEventArgs) {
    ++next_index_;
    return false;
  }
  for (std::size_t i = 0; i < nargs; ++i) {
    const std::size_t arg_slot =
        static_cast<std::size_t>((next_index_ + 1 + i) & kSlotMask);
    event.args[i] = slots_[arg_slot].load(std::memory_order_relaxed);
  }

  next_index_ += 1 + nargs;
  return true;
}
}  // namespace monitor
