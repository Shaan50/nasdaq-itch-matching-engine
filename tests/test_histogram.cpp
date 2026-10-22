#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "ironbook/common/histogram.hpp"

using ib::LatencyHistogram;

TEST(Histogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (std::uint64_t v = 0; v < LatencyHistogram::kSub; ++v) h.record(v);
  EXPECT_EQ(h.count(), LatencyHistogram::kSub);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.max(), LatencyHistogram::kSub - 1);
  EXPECT_EQ(h.percentile(0.5), LatencyHistogram::kSub / 2 - 1);
}

TEST(Histogram, BucketBoundsAreContiguousAndCoverValue) {
  for (unsigned b = 1; b < LatencyHistogram::kBuckets; ++b) {
    ASSERT_EQ(LatencyHistogram::bucket_lower(b), LatencyHistogram::bucket_upper(b - 1) + 1) << b;
  }
  std::mt19937_64 rng(7);
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t v = rng() >> (rng() % 64);
    const unsigned b = LatencyHistogram::bucket_of(v);
    ASSERT_LT(b, LatencyHistogram::kBuckets);
    ASSERT_LE(LatencyHistogram::bucket_lower(b), v);
    ASSERT_GE(LatencyHistogram::bucket_upper(b), v);
  }
  EXPECT_EQ(LatencyHistogram::bucket_of(~0ull), LatencyHistogram::kBuckets - 1);
}

TEST(Histogram, PercentilesWithinRelativeError) {
  std::mt19937_64 rng(42);
  std::lognormal_distribution<double> dist(5.0, 1.2);
  std::vector<std::uint64_t> values;
  LatencyHistogram h;
  for (int i = 0; i < 200000; ++i) {
    const auto v = static_cast<std::uint64_t>(dist(rng));
    values.push_back(v);
    h.record(v);
  }
  std::sort(values.begin(), values.end());
  for (double q : {0.5, 0.9, 0.99, 0.999}) {
    const auto exact = static_cast<double>(values[static_cast<std::size_t>(q * values.size()) - 1]);
    const auto approx = static_cast<double>(h.percentile(q));
    EXPECT_NEAR(approx, exact, std::max(1.0, exact * 0.02)) << "q=" << q;
  }
}

TEST(Histogram, MergeEqualsCombinedRecording) {
  LatencyHistogram a, b, c;
  for (std::uint64_t v = 0; v < 5000; v += 3) {
    a.record(v);
    c.record(v);
  }
  for (std::uint64_t v = 100; v < 90000; v += 7) {
    b.record(v);
    c.record(v);
  }
  a.merge(b);
  EXPECT_EQ(a.count(), c.count());
  EXPECT_EQ(a.max(), c.max());
  for (double q : {0.1, 0.5, 0.99}) EXPECT_EQ(a.percentile(q), c.percentile(q));
}
