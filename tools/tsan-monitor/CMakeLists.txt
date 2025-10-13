cmake_minimum_required(VERSION 3.20)
project(TSanMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TSAN_MONITOR_SOURCES
  src/Analyzer.cpp
  src/Channel.cpp
  src/Event.cpp
  src/EventQueue.cpp
  src/Logger.cpp
  src/MonitorApp.cpp
  src/Reader.cpp
  src/Scheduler.cpp
  src/Report.cpp
  src/State.cpp
  src/Main.cpp
)

add_executable(tsan-monitor
  ${TSAN_MONITOR_SOURCES}
)

target_compile_features(tsan-monitor PRIVATE cxx_std_17)

target_include_directories(tsan-monitor
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(tsan-monitor
  PRIVATE
    spdlog::spdlog
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(tsan-monitor PRIVATE -Wall -Wextra -Wpedantic)
elseif (MSVC)
  target_compile_options(tsan-monitor PRIVATE /W4 /permissive-)
endif()
