// Semantics tests run against BOTH the naive baseline and the optimized Market.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"

using namespace ib;

namespace {
MarketConfig small_cfg() {
  MarketConfig c;
  c.max_orders = 1u << 16;
  c.max_levels = 1u << 14;
  c.expected_live_orders = 1u << 12;
  return c;
}
template <class M>
M make() {
  if constexpr (std::is_same_v<M, NaiveMarket>) return M{};
  else return M{small_cfg()};
}
constexpr Locate L = 7;
}  // namespace

template <class M>
class BookSemantics : public ::testing::Test {
 protected:
  M m = make<M>();
  std::vector<LevelView> d;
};
using Markets = ::testing::Types<NaiveMarket, Market, BasicMarket<FlatHashMap<Murmur3Mix>>, BasicMarket<StdIndex>>;
TYPED_TEST_SUITE(BookSemantics, Markets);

TYPED_TEST(BookSemantics, EmptyBook) {
  EXPECT_FALSE(this->m.best(L, Side::Buy));
  EXPECT_FALSE(this->m.best(L, Side::Sell));
  EXPECT_EQ(this->m.order_count(), 0u);
  EXPECT_EQ(this->m.remove(1), OpStatus::UnknownOrder);
  EXPECT_EQ(this->m.execute(1, 5), OpStatus::UnknownOrder);
}

TYPED_TEST(BookSemantics, BestPricesAndAggregation) {
  auto& m = this->m;
  m.add(L, 1, Side::Buy, 100, 10);
  m.add(L, 2, Side::Buy, 102, 20);
  m.add(L, 3, Side::Buy, 101, 30);
  m.add(L, 4, Side::Buy, 102, 5);
  m.add(L, 5, Side::Sell, 105, 7);
  m.add(L, 6, Side::Sell, 104, 8);
  m.add(L, 7, Side::Sell, 110, 9);
  EXPECT_EQ(*m.best(L, Side::Buy), (LevelView{102, 25, 2}));
  EXPECT_EQ(*m.best(L, Side::Sell), (LevelView{104, 8, 1}));
  m.depth(L, Side::Buy, 10, this->d);
  ASSERT_EQ(this->d.size(), 3u);
  EXPECT_EQ(this->d[0].price, 102u);
  EXPECT_EQ(this->d[1].price, 101u);
  EXPECT_EQ(this->d[2].price, 100u);
  m.depth(L, Side::Sell, 2, this->d);
  ASSERT_EQ(this->d.size(), 2u);
  EXPECT_EQ(this->d[0].price, 104u);
  EXPECT_EQ(this->d[1].price, 105u);
  EXPECT_EQ(m.order_count(), 7u);
}

TYPED_TEST(BookSemantics, BooksAreIsolatedBySymbol) {
  auto& m = this->m;
  m.add(1, 10, Side::Buy, 100, 1);
  m.add(2, 11, Side::Buy, 200, 1);
  EXPECT_EQ(m.best(1, Side::Buy)->price, 100u);
  EXPECT_EQ(m.best(2, Side::Buy)->price, 200u);
  EXPECT_FALSE(m.best(3, Side::Buy));
}

TYPED_TEST(BookSemantics, DuplicateAddRejected) {
  auto& m = this->m;
  EXPECT_TRUE(m.add(L, 1, Side::Buy, 100, 10));
  EXPECT_FALSE(m.add(L, 1, Side::Sell, 200, 10));
  EXPECT_EQ(m.order_count(), 1u);
  EXPECT_FALSE(m.best(L, Side::Sell));
}

TYPED_TEST(BookSemantics, PartialAndFullExecution) {
  auto& m = this->m;
  m.add(L, 1, Side::Sell, 100, 50);
  m.add(L, 2, Side::Sell, 100, 30);
  EXPECT_EQ(m.execute(1, 20), OpStatus::Ok);
  EXPECT_EQ(*m.best(L, Side::Sell), (LevelView{100, 60, 2}));
  EXPECT_EQ(m.find(1)->qty, 30u);
  EXPECT_TRUE(m.find(1)->at_level_head);  // partial fill keeps priority
  EXPECT_EQ(m.execute(1, 30), OpStatus::Ok);
  EXPECT_FALSE(m.find(1));
  EXPECT_TRUE(m.find(2)->at_level_head);
  EXPECT_EQ(*m.best(L, Side::Sell), (LevelView{100, 30, 1}));
  EXPECT_EQ(m.execute(2, 31), OpStatus::Overfill);
  EXPECT_FALSE(m.best(L, Side::Sell));
  EXPECT_EQ(m.order_count(), 0u);
}

TYPED_TEST(BookSemantics, CancelReducesAndRemovesLevel) {
  auto& m = this->m;
  m.add(L, 1, Side::Buy, 100, 10);
  m.add(L, 2, Side::Buy, 99, 10);
  EXPECT_EQ(m.cancel(1, 4), OpStatus::Ok);
  EXPECT_EQ(m.best(L, Side::Buy)->qty, 6u);
  EXPECT_EQ(m.cancel(1, 6), OpStatus::Ok);
  EXPECT_EQ(m.best(L, Side::Buy)->price, 99u);
}

TYPED_TEST(BookSemantics, DeleteFromMiddleOfQueue) {
  auto& m = this->m;
  for (OrderId id = 1; id <= 5; ++id) m.add(L, id, Side::Buy, 100, static_cast<Qty>(id));
  EXPECT_EQ(m.remove(3), OpStatus::Ok);
  EXPECT_EQ(m.remove(1), OpStatus::Ok);
  EXPECT_EQ(m.remove(5), OpStatus::Ok);
  EXPECT_EQ(*m.best(L, Side::Buy), (LevelView{100, 6, 2}));
  EXPECT_TRUE(m.find(2)->at_level_head);
  EXPECT_FALSE(m.find(4)->at_level_head);
  EXPECT_EQ(m.remove(3), OpStatus::UnknownOrder);
}

TYPED_TEST(BookSemantics, ReplaceLosesPriorityAndMovesPrice) {
  auto& m = this->m;
  m.add(L, 1, Side::Buy, 100, 10);
  m.add(L, 2, Side::Buy, 100, 10);
  EXPECT_EQ(m.replace(1, 3, 10, 100), OpStatus::Ok);  // same price, new id -> back of queue
  EXPECT_FALSE(m.find(1));
  EXPECT_TRUE(m.find(2)->at_level_head);
  EXPECT_FALSE(m.find(3)->at_level_head);
  EXPECT_EQ(m.replace(3, 4, 25, 101), OpStatus::Ok);
  EXPECT_EQ(*m.best(L, Side::Buy), (LevelView{101, 25, 1}));
  EXPECT_EQ(m.find(4)->side, Side::Buy);
  EXPECT_EQ(m.find(4)->locate, L);
  EXPECT_EQ(m.replace(99, 100, 1, 1), OpStatus::UnknownOrder);
}

TYPED_TEST(BookSemantics, FindReportsBestLevel) {
  auto& m = this->m;
  m.add(L, 1, Side::Sell, 105, 1);
  m.add(L, 2, Side::Sell, 104, 1);
  EXPECT_FALSE(m.find(1)->at_best_level);
  EXPECT_TRUE(m.find(2)->at_best_level);
  m.remove(2);
  EXPECT_TRUE(m.find(1)->at_best_level);
}

TYPED_TEST(BookSemantics, DeepLevelsBeyondLinearScanWindow) {
  auto& m = this->m;
  // 200 levels inserted in shuffled order exercises both linear and binary search paths.
  std::vector<Price> prices;
  for (Price p = 1000; p < 1200; ++p) prices.push_back(p);
  std::mt19937 rng(3);
  std::shuffle(prices.begin(), prices.end(), rng);
  OrderId id = 1;
  for (Price p : prices) {
    m.add(L, id++, Side::Buy, p, 1);
    m.add(L, id++, Side::Sell, p + 1000, 1);
  }
  m.depth(L, Side::Buy, 1000, this->d);
  ASSERT_EQ(this->d.size(), 200u);
  for (std::size_t i = 0; i < this->d.size(); ++i) EXPECT_EQ(this->d[i].price, 1199 - i);
  m.depth(L, Side::Sell, 1000, this->d);
  for (std::size_t i = 0; i < this->d.size(); ++i) EXPECT_EQ(this->d[i].price, 2000 + i);
  // Remove every other order (deep cancels hit the binary-search erase path).
  for (OrderId k = 1; k < id; k += 4) EXPECT_EQ(m.remove(k), OpStatus::Ok);
  m.depth(L, Side::Buy, 1000, this->d);
  EXPECT_EQ(this->d.size(), 100u);
}

TEST(OptimizedBook, InvariantsHoldThroughChurn) {
  Market m(small_cfg());
  std::mt19937_64 rng(99);
  std::vector<OrderId> live;
  OrderId next = 1;
  for (int i = 0; i < 20000; ++i) {
    if (live.empty() || rng() % 3) {
      const Side s = rng() % 2 ? Side::Buy : Side::Sell;
      const Price p = s == Side::Buy ? 900 + rng() % 100 : 1001 + rng() % 100;
      m.add(L, next, s, p, 1 + rng() % 500);
      live.push_back(next++);
    } else {
      const std::size_t k = rng() % live.size();
      const OrderId id = live[k];
      if (rng() % 2) {
        if (m.execute(id, 1 + rng() % 600) == OpStatus::Ok && m.find(id)) continue;
      } else {
        m.remove(id);
      }
      live[k] = live.back();
      live.pop_back();
    }
    if (i % 500 == 0) ASSERT_TRUE(m.book(L).check_invariants()) << i;
  }
  EXPECT_TRUE(m.book(L).check_invariants());
  EXPECT_EQ(m.pools().orders.live(), m.order_count());
}

// The block-vectorized scan + branchless binary search must equal std::upper_bound for
// every ladder size (all scan/remainder/binary-search paths) and every query position.
TEST(PriceLadder, UpperMatchesStdUpperBoundExhaustively) {
  for (std::uint32_t size = 0; size <= 300; ++size) {
    PriceLadder lad;
    std::vector<std::uint32_t> keys;
    for (std::uint32_t i = 0; i < size; ++i) {
      keys.push_back(10 + 3 * i);
      lad.insert_at(i, 10 + 3 * i, i + 1);
    }
    for (std::uint32_t q = 0; q < 10 + 3 * size + 5; ++q) {
      const auto want = static_cast<std::size_t>(std::upper_bound(keys.begin(), keys.end(), q) - keys.begin());
      ASSERT_EQ(lad.upper(q), want) << "size=" << size << " q=" << q;
    }
  }
}
