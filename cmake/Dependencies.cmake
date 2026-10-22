# Third-party dependencies. By default the pinned upstream releases are fetched and built
# for the target architecture (a system install may be the wrong arch -- e.g. Intel
# Homebrew on Apple silicon). -DIRONBOOK_SYSTEM_DEPS=ON uses installed packages.

include(FetchContent)

# Apple clang searches /usr/local/include *before* -isystem directories, so a different
# GTest/benchmark version installed there (Intel Homebrew) would shadow the fetched
# headers. Exporting the fetched include dirs as plain -I directories avoids that.
macro(ironbook_unsystem)
  foreach(_t ${ARGN})
    set_property(TARGET ${_t} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
  endforeach()
endmacro()
find_package(ZLIB REQUIRED)

if(IRONBOOK_BUILD_TESTS)
  if(IRONBOOK_SYSTEM_DEPS)
    find_package(GTest CONFIG QUIET)
  endif()
  if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found on system; fetching v1.15.2")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
    ironbook_unsystem(gtest gtest_main)
  endif()
endif()

if(IRONBOOK_BUILD_BENCHMARKS)
  if(IRONBOOK_SYSTEM_DEPS)
    find_package(benchmark CONFIG QUIET)
  endif()
  if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found on system; fetching v1.9.1")
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(benchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(benchmark)
    ironbook_unsystem(benchmark benchmark_main)
  endif()
endif()
