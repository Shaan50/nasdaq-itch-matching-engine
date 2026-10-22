#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "ironbook/common/compiler.hpp"

namespace ib {

// HDR-style log-linear histogram. Values below 2^kSubBits are recorded exactly; above
// that each power-of-two range is split into 2^kSubBits linear buckets, giving a
// worst-case relative error of 1/2^kSubBits (~1.6%) across the full uint64 range.
// record() is branch-light and allocation-free, so it can sit on the hot path.
class LatencyHistogram {
 public:
  static constexpr unsigned kSubBits = 6;
  static constexpr unsigned kSub = 1u << kSubBits;
  static constexpr unsigned kBuckets = (64 - kSubBits + 1) * kSub;

  IB_ALWAYS_INLINE void record(std::uint64_t v) noexcept {
    ++counts_[bucket_of(v)];
    ++total_;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
    sum_ += v;
  }

  void merge(const LatencyHistogram& o) noexcept;
  void clear() noexcept { *this = LatencyHistogram{}; }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t max() const noexcept { return total_ ? max_ : 0; }
  std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  double mean() const noexcept {
    return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
  }

  // Value at quantile q in [0,1]; returns the upper bound of the containing bucket.
  std::uint64_t percentile(double q) const noexcept;

  // (bucket upper bound, count) for non-empty buckets; used to export CSV for plots.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> nonzero_buckets() const;

  // One-line summary, values converted with `scale` (e.g. ns per tick).
  std::string summary(double scale = 1.0, const char* unit = "ns") const;

  // v < 2^S          -> bucket v (exact)
  // v in [2^m, 2^m+1) -> shift = m - S; bucket = (shift + 1) * 2^S + mantissa(top S bits)
  static constexpr unsigned bucket_of(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<unsigned>(v);
    const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned shift = msb - kSubBits;
    return (shift + 1) * kSub + static_cast<unsigned>((v >> shift) & (kSub - 1));
  }
  static constexpr std::uint64_t bucket_lower(unsigned b) noexcept {
    if (b < kSub) return b;
    const unsigned shift = b / kSub - 1;
    return (static_cast<std::uint64_t>(kSub) + (b % kSub)) << shift;
  }
  static constexpr std::uint64_t bucket_upper(unsigned b) noexcept {
    if (b < kSub) return b;
    const unsigned shift = b / kSub - 1;
    return bucket_lower(b) + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = ~std::uint64_t{0};
};

}  // namespace ib
