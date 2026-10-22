#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "ironbook/concurrency/spsc_queue.hpp"

using ib::SpscQueue;

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
  SpscQueue<int> q(100);
  EXPECT_EQ(q.capacity(), 128u);
}

TEST(SpscQueue, FifoFullAndEmpty) {
  SpscQueue<int> q(4);
  int v;
  EXPECT_FALSE(q.try_pop(v));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(q.try_pop(v));
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, WrapAroundManyTimes) {
  SpscQueue<std::uint64_t> q(8);
  std::uint64_t next_in = 0, next_out = 0, v;
  for (int round = 0; round < 10000; ++round) {
    const int burst = round % 7 + 1;
    for (int i = 0; i < burst; ++i) if (q.try_push(next_in)) ++next_in;
    for (int i = 0; i < burst; ++i) {
      if (q.try_pop(v)) {
        ASSERT_EQ(v, next_out);
        ++next_out;
      }
    }
  }
}

TEST(SpscQueue, ReserveCommitPeekReleaseAreContiguous) {
  SpscQueue<int> q(8);
  int* w;
  ASSERT_EQ(q.reserve(w), 8u);
  for (int i = 0; i < 6; ++i) w[i] = i;
  q.commit(6);
  const int* r;
  ASSERT_EQ(q.peek(r), 6u);
  EXPECT_EQ(r[5], 5);
  q.release(6);
  // Tail is now at index 6: only 2 contiguous slots before the wrap.
  ASSERT_EQ(q.reserve(w), 2u);
  w[0] = 60;
  w[1] = 70;
  q.commit(2);
  ASSERT_EQ(q.reserve(w), 6u);  // wrapped region
  w[0] = 80;
  q.commit(1);
  ASSERT_EQ(q.peek(r), 2u);
  EXPECT_EQ(r[0], 60);
  q.release(2);
  ASSERT_EQ(q.peek(r), 1u);
  EXPECT_EQ(r[0], 80);
}

// Two-thread stress: every value must arrive exactly once, in order. Run under TSan via
// the `tsan` preset to check the memory ordering.
TEST(SpscQueue, ConcurrentStressPreservesOrder) {
  constexpr std::uint64_t kN = 2'000'000;
  SpscQueue<std::uint64_t> q(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN;) {
      std::uint64_t* w;
      std::size_t n = q.reserve(w);
      if (n == 0) continue;
      n = std::min<std::size_t>(n, kN - i);
      n = std::min<std::size_t>(n, (i % 17) + 1);  // vary batch sizes
      for (std::size_t j = 0; j < n; ++j) w[j] = i + j;
      q.commit(n);
      i += n;
    }
  });
  std::uint64_t expect = 0;
  while (expect < kN) {
    if (expect % 2) {
      std::uint64_t v;
      if (q.try_pop(v)) {
        ASSERT_EQ(v, expect);
        ++expect;
      }
    } else {
      const std::uint64_t* r;
      const std::size_t n = q.peek(r);
      for (std::size_t j = 0; j < n; ++j) ASSERT_EQ(r[j], expect + j);
      q.release(n);
      expect += n;
    }
  }
  producer.join();
  EXPECT_TRUE(q.empty());
}
