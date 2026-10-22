#include "ironbook/common/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ib {

void LatencyHistogram::merge(const LatencyHistogram& o) noexcept {
  for (unsigned i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
  total_ += o.total_;
  sum_ += o.sum_;
  max_ = std::max(max_, o.max_);
  min_ = std::min(min_, o.min_);
}

std::uint64_t LatencyHistogram::percentile(double q) const noexcept {
  if (total_ == 0) return 0;
  q = std::clamp(q, 0.0, 1.0);
  const auto rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(total_)));
  const std::uint64_t target = std::max<std::uint64_t>(rank, 1);
  std::uint64_t seen = 0;
  for (unsigned b = 0; b < kBuckets; ++b) {
    seen += counts_[b];
    if (seen >= target) return std::min(bucket_upper(b), max_);
  }
  return max_;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> LatencyHistogram::nonzero_buckets() const {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  for (unsigned b = 0; b < kBuckets; ++b)
    if (counts_[b]) out.emplace_back(bucket_upper(b), counts_[b]);
  return out;
}

std::string LatencyHistogram::summary(double scale, const char* unit) const {
  char buf[256];
  auto s = [&](std::uint64_t v) { return static_cast<double>(v) * scale; };
  std::snprintf(buf, sizeof buf,
                "n=%llu mean=%.1f p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f p99.99=%.1f max=%.1f %s",
                static_cast<unsigned long long>(total_), mean() * scale, s(percentile(0.5)),
                s(percentile(0.9)), s(percentile(0.99)), s(percentile(0.999)),
                s(percentile(0.9999)), s(max()), unit);
  return buf;
}

}  // namespace ib
