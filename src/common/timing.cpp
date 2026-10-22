#include "ironbook/common/timing.hpp"

#include <cstdio>
#include <thread>

#include "ironbook/common/types.hpp"

namespace ib {

namespace {

double calibrate() {
#if defined(__APPLE__) && !defined(__x86_64__)
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  return static_cast<double>(tb.denom) / static_cast<double>(tb.numer);  // ticks per ns
#elif defined(__aarch64__)
  // The generic timer advertises its frequency architecturally.
  std::uint64_t freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  if (freq != 0) return static_cast<double>(freq) / 1e9;
#endif
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::uint64_t c0 = CycleClock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::uint64_t c1 = CycleClock::now();
  const auto t1 = clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  return static_cast<double>(c1 - c0) / ns;
}

}  // namespace

double CycleClock::ticks_per_ns() {
  static const double value = calibrate();
  return value;
}

const char* CycleClock::source_name() {
#if defined(__x86_64__)
  return "rdtsc";
#elif defined(__APPLE__)
  return "mach_absolute_time";
#elif defined(__aarch64__)
  return "cntvct_el0";
#else
  return "steady_clock";
#endif
}

std::string format_price(Price p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / kPriceScale, p % kPriceScale);
  return buf;
}

}  // namespace ib
