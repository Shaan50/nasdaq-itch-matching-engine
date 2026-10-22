#pragma once

// In-process sampling profiler (no root, no perf/Instruments required).
//
// A SIGPROF interval timer interrupts whichever thread is consuming CPU; the handler
// walks the frame-pointer chain from the interrupted context (async-signal-safe: plain
// loads bounds-checked against the thread's stack) and appends the raw return
// addresses to a preallocated buffer. On stop() the samples are written out together
// with the image load address; scripts/symbolize_profile.py turns them into folded
// stacks (using `atos -i` / `addr2line -i`, so inlined frames are recovered from DWARF)
// for flamegraph.pl.
//
// Enable in the tools with IRONBOOK_PROFILE=out.prof [IRONBOOK_PROFILE_HZ=2000].

#include <cstdint>
#include <string>

namespace ib {

class SamplingProfiler {
 public:
  static constexpr int kMaxDepth = 48;

  // Starts sampling at `hz` samples per CPU-second. Only one profiler may be active.
  static bool start(const std::string& out_path, int hz = 1999,
                    std::size_t max_samples = 1u << 20);
  // Stops sampling and writes the profile. Returns the number of samples written.
  static std::size_t stop();

  // Starts if IRONBOOK_PROFILE is set; returns true if profiling is active.
  static bool start_from_env();
};

// RAII helper: profiles the enclosing scope when IRONBOOK_PROFILE is set.
class ScopedProfile {
 public:
  ScopedProfile() : active_(SamplingProfiler::start_from_env()) {}
  ~ScopedProfile() {
    if (active_) SamplingProfiler::stop();
  }
  ScopedProfile(const ScopedProfile&) = delete;
  ScopedProfile& operator=(const ScopedProfile&) = delete;

 private:
  bool active_;
};

}  // namespace ib
