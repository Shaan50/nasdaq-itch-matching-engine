#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "ironbook/book/flat_hash_map.hpp"

using namespace ib;

template <class H>
class FlatHashMapTest : public ::testing::Test {};
using Hashes = ::testing::Types<IdentityHash, XorFoldHash, FibonacciHash, Murmur3Mix>;
TYPED_TEST_SUITE(FlatHashMapTest, Hashes);

TYPED_TEST(FlatHashMapTest, BasicInsertFindErase) {
  FlatHashMap<TypeParam> m(16);
  EXPECT_TRUE(m.insert(5, 50));
  EXPECT_FALSE(m.insert(5, 51));
  EXPECT_EQ(m.find(5), 50u);
  EXPECT_EQ(m.find(6), FlatHashMap<TypeParam>::kMissing);
  m.assign(5, 55);
  EXPECT_EQ(m.find(5), 55u);
  EXPECT_EQ(m.erase(5), 55u);
  EXPECT_EQ(m.erase(5), FlatHashMap<TypeParam>::kMissing);
  EXPECT_EQ(m.size(), 0u);
}

// Keys that all collide under identity hashing build one long cluster; deleting from the
// middle must backward-shift correctly or later keys become unreachable.
TYPED_TEST(FlatHashMapTest, BackwardShiftDeletionKeepsClustersReachable) {
  FlatHashMap<TypeParam> m(64);  // capacity 128
  const std::size_t cap = m.capacity();
  std::vector<std::uint64_t> keys;
  for (std::uint64_t i = 1; i <= 40; ++i) keys.push_back(i * cap + 3);  // same home slot
  for (std::size_t i = 0; i < keys.size(); ++i) ASSERT_TRUE(m.insert(keys[i], static_cast<std::uint32_t>(i + 1)));
  for (std::size_t i = 0; i < keys.size(); i += 3) ASSERT_EQ(m.erase(keys[i]), i + 1);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto expect = (i % 3 == 0) ? 0u : static_cast<std::uint32_t>(i + 1);
    ASSERT_EQ(m.find(keys[i]), expect) << i;
  }
}

TYPED_TEST(FlatHashMapTest, RandomOpsMatchStdUnorderedMap) {
  FlatHashMap<TypeParam> m(1024);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(1234);
  std::vector<std::uint64_t> live;
  for (int step = 0; step < 300000; ++step) {
    const int op = static_cast<int>(rng() % 10);
    if (op < 5 || live.empty()) {
      const std::uint64_t k = (rng() % 50000) + 1;
      const auto v = static_cast<std::uint32_t>(rng() | 1);
      const bool inserted = m.insert(k, v);
      ASSERT_EQ(inserted, ref.emplace(k, v).second);
      if (inserted) live.push_back(k);
    } else if (op < 8) {
      const std::size_t i = rng() % live.size();
      const std::uint64_t k = live[i];
      live[i] = live.back();
      live.pop_back();
      ASSERT_EQ(m.erase(k), ref.at(k));
      ref.erase(k);
    } else {
      const std::uint64_t k = (rng() % 50000) + 1;
      auto it = ref.find(k);
      ASSERT_EQ(m.find(k), it == ref.end() ? 0u : it->second);
    }
  }
  EXPECT_EQ(m.size(), ref.size());
  EXPECT_TRUE(m.check_invariants());
  EXPECT_GT(m.rehashes(), 0u);  // started small, must have grown
}

// Both probing schemes must agree with std::unordered_map under heavy collisions.
TEST(FlatHashMap, LinearAndRobinHoodAgreeUnderCollisions) {
  FlatHashMap<IdentityHash, Probing::Linear> lin(256);
  FlatHashMap<IdentityHash, Probing::RobinHood> rh(256);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(5);
  const std::uint64_t cap = rh.capacity();
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t k = 1 + (rng() % 64) * cap + rng() % 40;  // clustered homes
    if (rng() % 2) {
      const auto v = static_cast<std::uint32_t>(i + 1);
      const bool a = lin.insert(k, v), b = rh.insert(k, v), c = ref.emplace(k, v).second;
      ASSERT_EQ(a, c);
      ASSERT_EQ(b, c);
    } else {
      const auto it = ref.find(k);
      const std::uint32_t want = it == ref.end() ? 0u : it->second;
      ASSERT_EQ(lin.erase(k), want);
      ASSERT_EQ(rh.erase(k), want);
      if (it != ref.end()) ref.erase(it);
    }
    if (i % 5000 == 0) {
      ASSERT_TRUE(lin.check_invariants());
      ASSERT_TRUE(rh.check_invariants());
    }
  }
  for (const auto& [k, v] : ref) {
    ASSERT_EQ(lin.find(k), v);
    ASSERT_EQ(rh.find(k), v);
  }
}

// The workload that broke plain linear probing: a sliding window of sequential keys.
// Robin Hood deletion must stay O(1) (every key sits in its home slot).
TEST(FlatHashMap, SlidingWindowDeleteIsConstantTime) {
  FlatHashMap<IdentityHash> m(1u << 16);
  const std::uint64_t window = 1u << 16;
  for (std::uint64_t k = 1; k <= window; ++k) m.insert(k, 1);
  for (std::uint64_t k = window + 1; k <= 20 * window; ++k) {
    m.insert(k, 1);
    ASSERT_EQ(m.erase(k - window), 1u);
  }
  EXPECT_DOUBLE_EQ(m.mean_probe_length(), 1.0);
  EXPECT_TRUE(m.check_invariants());
}

TEST(FlatHashMap, SequentialKeysHaveUnitProbeLength) {
  FlatHashMap<IdentityHash> a(1u << 16);
  FlatHashMap<XorFoldHash> b(1u << 16);
  for (std::uint64_t k = 1; k <= 60000; ++k) {
    a.insert(k, 1);
    b.insert(k + (5ull << 17), 1);  // offset into a later block: still sequential slots
  }
  EXPECT_DOUBLE_EQ(a.mean_probe_length(), 1.0);
  EXPECT_DOUBLE_EQ(b.mean_probe_length(), 1.0);
}

// Keys exactly one table-size apart collide under identity hashing but not xor-fold.
TEST(FlatHashMap, XorFoldBreaksTableSizeAliasing) {
  FlatHashMap<IdentityHash> a(1u << 10);
  FlatHashMap<XorFoldHash> b(1u << 10);
  const std::uint64_t cap = a.capacity();
  for (std::uint64_t i = 1; i <= 200; ++i) {
    a.insert(i * cap + 1, 1);
    b.insert(i * cap + 1, 1);
  }
  EXPECT_GT(a.mean_probe_length(), 50.0);
  EXPECT_LT(b.mean_probe_length(), 2.0);
}
