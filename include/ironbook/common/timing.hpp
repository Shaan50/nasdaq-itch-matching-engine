#pragma once

#include <chrono>
#include <cstdint>

#include "ironbook/common/compiler.hpp"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace ib {

// Raw hardware counter used for latency measurement.
//   x86-64       : rdtsc (invariant TSC, sub-ns resolution)
//   Apple arm64  : mach_absolute_time() (24 MHz -> 41.67 ns resolution, ~4 ns per read).
//                  Direct EL0 reads of cntvct_el0 are NOT usable on recent Apple silicon
//                  + macOS: on an M5 / macOS 26 they return a stale value for ~10^6
//                  consecutive reads and occasionally step backwards (see README).
//   Linux arm64  : cntvct_el0 generic timer
//   other        : std::chrono::steady_clock
struct CycleClock {
  IB_ALWAYS_INLINE static std::uint64_t now() noexcept {
#if defined(__x86_64__)
    std::uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#elif defined(__APPLE__)
    return mach_absolute_time();
#elif defined(__aarch64__)
    std::uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }

  // Counter ticks per nanosecond, calibrated once against steady_clock (cached).
  static double ticks_per_ns();
  static double ns_per_tick() { return 1.0 / ticks_per_ns(); }
  static double to_ns(std::uint64_t ticks) { return static_cast<double>(ticks) * ns_per_tick(); }
  static const char* source_name();
};

// Wall-clock helper for throughput numbers.
class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  void reset() { start_ = std::chrono::steady_clock::now(); }
  double elapsed_sec() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

}  // namespace ib
