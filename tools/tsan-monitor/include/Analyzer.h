#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <unordered_map>

#include "Constants.h"
#include "Event.h"
#include "Report.h"
#include "State.h"

namespace monitor {
    enum class AnalyzerResult {
        kContinue,
        kTerminate,
        kStopOnRace,
    };

    class Analyzer {
    public:
        explicit Analyzer(bool verbose, RaceAction race_action)
            : verbose_(verbose), race_action_(race_action) {
        }

        AnalyzerResult Process(const Event &event);

    private:
        struct ThreadState {
            VectorClock clock;
        };

        ThreadState &GetThreadState(int tid);

        AddressState &GetAddressState(std::uint64_t address);

        bool HandleRead(const Event &event, ThreadState &thread_state,
                        AddressState &address_state, const Epoch &current_epoch);

        bool HandleWrite(const Event &event, ThreadState &thread_state,
                         AddressState &address_state, const Epoch &current_epoch);

        bool ReportRace(RaceKind kind, const Epoch &first, std::uint64_t first_value,
                        const Epoch &second, std::uint64_t second_value,
                        std::uint64_t address);

        static bool HappensBefore(const Epoch &epoch, const VectorClock &clock);

        void PrintEvent(const Event &event);

        bool verbose_ = false;
        RaceAction race_action_ = RaceAction::kContinue;
        std::unordered_map<int, ThreadState> threads_;
        std::unordered_map<std::uint64_t, AddressState> addresses_;
        Report report_;
    };
} // namespace monitor
