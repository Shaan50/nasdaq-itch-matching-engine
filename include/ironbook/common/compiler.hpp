#pragma once

#include <cstddef>

#define IB_LIKELY(x) __builtin_expect(!!(x), 1)
#define IB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define IB_ALWAYS_INLINE inline __attribute__((always_inline))
#define IB_NOINLINE __attribute__((noinline))
#define IB_COLD __attribute__((cold, noinline))
#define IB_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)
#define IB_PREFETCH_W(addr) __builtin_prefetch((addr), 1, 3)

namespace ib {

// Apple M-series cores use 128-byte cache lines (and the L2 prefetcher pulls pairs of
// 64-byte lines on x86), so 128 bytes is the safe padding to rule out false sharing.
#if defined(__aarch64__) && defined(__APPLE__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif
inline constexpr std::size_t kNoFalseSharing = 128;

// Prevent the optimizer from discarding a value in benchmarks.
template <class T>
IB_ALWAYS_INLINE void do_not_optimize(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

IB_ALWAYS_INLINE void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

}  // namespace ib
