#pragma once

#include <spdlog/common.h>

#include <string>

namespace monitor {
    void InitLogger(const std::string &file_path,
                    spdlog::level::level_enum level = spdlog::level::info);

    void SetLogLevel(spdlog::level::level_enum level);

    void SetLogLevelFromString(const std::string &level);
} // namespace monitor