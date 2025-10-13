#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "State.h"

namespace monitor {
    enum class RaceKind {
        kReadWrite,
        kWriteRead,
        kWriteWrite,
    };

    struct RaceEventInfo {
        RaceKind kind;
        std::uint64_t address = 0;
        Epoch first;
        Epoch second;
        std::uint64_t first_value = 0;
        std::uint64_t second_value = 0;
    };

    class Report {
    public:
        explicit Report(std::filesystem::path output_dir = "report");

        void OnRace(const RaceEventInfo &info);

    private:
        std::string MakeKey(const RaceEventInfo &info) const;

        std::string Format(const RaceEventInfo &info) const;

        void Emit(const RaceEventInfo &info, const std::string &content);

        std::filesystem::path output_dir_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::size_t> counters_;
    };
} // namespace monitor
