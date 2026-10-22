#include <gtest/gtest.h>

#include <set>

#include "ironbook/book/order_book.hpp"
#include "ironbook/book/pool.hpp"

using namespace ib;

TEST(IndexPool, NeverHandsOutNilAndReusesLifo) {
  IndexPool<Order> pool(16);
  const auto a = pool.alloc();
  const auto b = pool.alloc();
  const auto c = pool.alloc();
  EXPECT_NE(a, kNil);
  EXPECT_EQ(pool.live(), 3u);
  pool.free(b);
  pool.free(a);
  EXPECT_EQ(pool.alloc(), a);  // most recently freed first
  EXPECT_EQ(pool.alloc(), b);
  EXPECT_EQ(pool.live(), 3u);
  EXPECT_EQ(pool.high_water(), 3u);
  (void)c;
}

TEST(IndexPool, ExhaustionThrows) {
  IndexPool<Level> pool(4);  // slots 1..3 usable
  pool.alloc();
  pool.alloc();
  pool.alloc();
  EXPECT_THROW(pool.alloc(), std::length_error);
}

TEST(IndexPool, ObjectsAreStableAndDistinct) {
  IndexPool<Order> pool(1u << 16, /*prefault=*/true);
  std::set<std::uint32_t> seen;
  for (int i = 0; i < 1000; ++i) {
    const auto idx = pool.alloc();
    ASSERT_TRUE(seen.insert(idx).second);
    pool[idx].id = static_cast<OrderId>(idx) * 7;
  }
  for (auto idx : seen) EXPECT_EQ(pool[idx].id, static_cast<OrderId>(idx) * 7);
}
