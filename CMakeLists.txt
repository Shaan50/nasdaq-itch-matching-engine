cmake_minimum_required(VERSION 3.24)

# On Apple silicon, CMake/Ninja installed by Intel Homebrew (/usr/local) run under Rosetta
# and the compiler they spawn defaults to x86_64 -- silently producing translated
# binaries. Detect the real hardware and target it explicitly.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _ib_arm64
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_ib_arm64 STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
  endif()
endif()

project(Ironbook
  VERSION 1.0.0
  DESCRIPTION "Low-latency NASDAQ ITCH 5.0 order book reconstruction and matching engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IRONBOOK_BUILD_TESTS      "Build GoogleTest unit tests"          ON)
option(IRONBOOK_BUILD_BENCHMARKS "Build Google Benchmark micro-benches"  ON)
option(IRONBOOK_NATIVE           "Tune for the host CPU (-march=native)" ON)
option(IRONBOOK_LTO              "Enable link-time optimization"        OFF)
option(IRONBOOK_SYSTEM_DEPS      "Use installed GTest/benchmark instead of fetching" OFF)
set(IRONBOOK_SANITIZE "" CACHE STRING "Sanitizers: '', 'address;undefined', or 'thread'")
set(IRONBOOK_PGO "" CACHE STRING "Profile-guided optimization: '', 'generate', or path to .profdata")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

# ---------------------------------------------------------------- core library
add_library(ironbook
  src/itch/file_reader.cpp
  src/itch/symbols.cpp
  src/common/timing.cpp
  src/common/mapped_file.cpp
  src/common/histogram.cpp
  src/common/profiler.cpp
)
add_library(ironbook::ironbook ALIAS ironbook)
target_include_directories(ironbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ironbook PUBLIC Threads::Threads ZLIB::ZLIB)
ironbook_configure_target(ironbook)

# ---------------------------------------------------------------- executables
foreach(app itch_replay itch_stats itch_slice ib_bench engine_sim)
  add_executable(${app} apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE ironbook)
  ironbook_configure_target(${app})
endforeach()

# ---------------------------------------------------------------- tests / benches
if(IRONBOOK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IRONBOOK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
