#include "Analyzer.h"

#include <format>
#include <iostream>

namespace monitor {
  AnalyzerResult Analyzer::Process(const Event &event) {
    if (verbose_) {
      PrintEvent(event);
    }
    if (IsTermination(event)) {
      return AnalyzerResult::kTerminate;
    }

    if (event.id == EventId::kRead || event.id == EventId::kWrite) {
      ThreadState &thread_state = GetThreadState(event.tid);
      const std::uint64_t current_clock = thread_state.clock.Tick(event.tid);
      const Epoch current_epoch{event.tid, current_clock};
      AddressState &address_state = GetAddressState(event.address);

      bool race_detected = false;
      if (event.id == EventId::kRead) {
        race_detected = HandleRead(event, thread_state, address_state,
                                   current_epoch);
      } else {
        race_detected = HandleWrite(event, thread_state, address_state,
                                    current_epoch);
      }

      if (race_detected && race_action_ == RaceAction::kStop) {
        return AnalyzerResult::kStopOnRace;
      }
    }
    return AnalyzerResult::kContinue;
  }

  Analyzer::ThreadState &Analyzer::GetThreadState(int tid) {
    return threads_[tid];
  }

  AddressState &Analyzer::GetAddressState(std::uint64_t address) {
    return addresses_[address];
  }

  bool Analyzer::HandleRead(const Event &event, ThreadState &thread_state,
                            AddressState &address_state,
                            const Epoch &current_epoch) {
    bool race_detected = false;
    if (address_state.last_write &&
        address_state.last_write->tid != current_epoch.tid &&
        !HappensBefore(*address_state.last_write, thread_state.clock)) {
      race_detected = ReportRace(RaceKind::kReadWrite, *address_state.last_write,
                                 address_state.last_write_value, current_epoch,
                                 event.nargs > 0 ? event.args[0] : 0,
                                 event.address);
    }

    address_state.AddOrUpdateRead(current_epoch,
                                  event.nargs > 0 ? event.args[0] : 0);
    return race_detected;
  }

  bool Analyzer::HandleWrite(const Event &event, ThreadState &thread_state,
                             AddressState &address_state,
                             const Epoch &current_epoch) {
    bool race_detected = false;
    if (address_state.last_write &&
        address_state.last_write->tid != current_epoch.tid &&
        !HappensBefore(*address_state.last_write, thread_state.clock)) {
      race_detected = ReportRace(RaceKind::kWriteWrite, *address_state.last_write,
                                 address_state.last_write_value, current_epoch,
                                 event.nargs > 0 ? event.args[0] : 0,
                                 event.address);
    }

    for (const auto &read_entry: address_state.read_set) {
      if (read_entry.epoch.tid == current_epoch.tid) {
        continue;
      }
      if (!HappensBefore(read_entry.epoch, thread_state.clock)) {
        race_detected = ReportRace(RaceKind::kWriteRead, read_entry.epoch,
                                   read_entry.value, current_epoch,
                                   event.nargs > 0 ? event.args[0] : 0,
                                   event.address) || race_detected;
      }
    }

    address_state.last_write = current_epoch;
    address_state.last_write_value = event.nargs > 0 ? event.args[0] : 0;
    address_state.ClearReads();
    return race_detected;
  }

  bool Analyzer::ReportRace(RaceKind kind, const Epoch &first,
                            std::uint64_t first_value, const Epoch &second,
                            std::uint64_t second_value, std::uint64_t address) {
    RaceEventInfo info;
    info.kind = kind;
    info.address = address;
    info.first = first;
    info.second = second;
    info.first_value = first_value;
    info.second_value = second_value;
    report_.OnRace(info);
    return true;
  }

  bool Analyzer::HappensBefore(const Epoch &epoch,
                               const VectorClock &clock) {
    if (!epoch.IsValid()) {
      return true;
    }
    return clock.Get(epoch.tid) >= epoch.clock;
  }

  void Analyzer::PrintEvent(const Event &event) {
    std::string message = std::format("[tid={}] {} lap={} addr=0x{:012x} nargs={}",
                                      event.tid, EventName(event.id),
                                      static_cast<int>(event.lap),
                                      event.address, event.nargs);
    for (std::size_t i = 0; i < event.nargs; ++i) {
      message += std::format(" arg{}=0x{:x}", i, event.args[i]);
    }
    message += std::format(" index={}", event.index);
    std::cout << message << std::endl;
  }
} // namespace monitor
