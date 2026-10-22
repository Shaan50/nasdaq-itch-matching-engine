#include <gtest/gtest.h>

#include <random>
#include <variant>
#include <vector>

#include "ironbook/engine/matching_engine.hpp"
#include "ironbook/engine/reference_engine.hpp"

using namespace ib;
using namespace ib::engine;

namespace {

struct Accepted { OrderId id; friend bool operator==(const Accepted&, const Accepted&) = default; };
struct Rejected { OrderId id; RejectReason r; friend bool operator==(const Rejected&, const Rejected&) = default; };
struct Cancelled { OrderId id; Qty q; CancelReason r; friend bool operator==(const Cancelled&, const Cancelled&) = default; };
struct Rested { OrderId id; Qty q; friend bool operator==(const Rested&, const Rested&) = default; };
using Event = std::variant<Accepted, Rejected, Fill, Cancelled, Rested>;

struct Recorder {
  std::vector<Event> ev;
  void on_accepted(const NewOrder& o) { ev.emplace_back(Accepted{o.id}); }
  void on_rejected(OrderId id, RejectReason r) { ev.emplace_back(Rejected{id, r}); }
  void on_fill(const Fill& f) { ev.emplace_back(f); }
  void on_cancelled(OrderId id, Qty q, CancelReason r) { ev.emplace_back(Cancelled{id, q, r}); }
  void on_rested(OrderId id, Qty q) { ev.emplace_back(Rested{id, q}); }
  std::vector<Fill> fills() const {
    std::vector<Fill> out;
    for (const auto& e : ev)
      if (auto* f = std::get_if<Fill>(&e)) out.push_back(*f);
    return out;
  }
  void clear() { ev.clear(); }
};

EngineConfig small(StpMode stp = StpMode::None) {
  EngineConfig c;
  c.instruments = 2;
  c.max_orders = 1u << 18;
  c.max_levels = 1u << 16;
  c.expected_live_orders = 1u << 14;
  c.stp = stp;
  c.prefault = false;
  return c;
}

NewOrder limit(OrderId id, Side s, Price p, Qty q, OwnerId owner = 0) {
  return {id, owner, 0, s, OrderType::Limit, p, q};
}

class EngineTest : public ::testing::Test {
 protected:
  Recorder rec;
  MatchingEngine<Recorder> eng{small(), &rec};
};

}  // namespace

TEST_F(EngineTest, LimitOrderRestsWhenNotCrossing) {
  EXPECT_TRUE(eng.submit(limit(1, Side::Buy, 100, 10)));
  EXPECT_TRUE(eng.submit(limit(2, Side::Sell, 101, 10)));
  EXPECT_EQ(eng.best(0, Side::Buy)->price, 100u);
  EXPECT_EQ(eng.best(0, Side::Sell)->price, 101u);
  EXPECT_TRUE(rec.fills().empty());
  EXPECT_EQ(eng.live_orders(), 2u);
}

TEST_F(EngineTest, PriceTimePriorityAcrossLevels) {
  eng.submit(limit(1, Side::Sell, 101, 5));
  eng.submit(limit(2, Side::Sell, 100, 5));
  eng.submit(limit(3, Side::Sell, 100, 5));
  eng.submit(limit(4, Side::Sell, 102, 5));
  rec.clear();
  eng.submit(limit(10, Side::Buy, 101, 12));
  const auto f = rec.fills();
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(f[0].maker, 2u);  // best price, earliest
  EXPECT_EQ(f[1].maker, 3u);
  EXPECT_EQ(f[2].maker, 1u);
  EXPECT_EQ(f[2].qty, 2u);
  EXPECT_EQ(f[2].maker_remaining, 3u);
  EXPECT_EQ(f[2].taker_remaining, 0u);
  for (const auto& x : f) EXPECT_EQ(x.taker, 10u);
  EXPECT_EQ(f[0].price, 100u);  // trades at the resting price
  EXPECT_EQ(f[2].price, 101u);
  EXPECT_EQ(*eng.order_qty(1), 3u);
  EXPECT_FALSE(eng.best(0, Side::Buy));  // fully filled, did not rest
}

TEST_F(EngineTest, LimitRemainderRestsAfterPartialMatch) {
  eng.submit(limit(1, Side::Sell, 100, 5));
  eng.submit(limit(2, Side::Buy, 100, 8));
  EXPECT_FALSE(eng.best(0, Side::Sell));
  EXPECT_EQ(*eng.best(0, Side::Buy), (LevelView{100, 3, 1}));
}

TEST_F(EngineTest, MarketOrderSweepsAndNeverRests) {
  eng.submit(limit(1, Side::Buy, 100, 5));
  eng.submit(limit(2, Side::Buy, 99, 5));
  rec.clear();
  eng.submit({3, 0, 0, Side::Sell, OrderType::Market, 0, 20});
  EXPECT_EQ(rec.fills().size(), 2u);
  EXPECT_FALSE(eng.best(0, Side::Buy));
  EXPECT_FALSE(eng.best(0, Side::Sell));
  EXPECT_EQ(std::get<Cancelled>(rec.ev.back()), (Cancelled{3, 10, CancelReason::ImmediateOrCancel}));
}

TEST_F(EngineTest, IocRespectsLimitAndCancelsRemainder) {
  eng.submit(limit(1, Side::Sell, 100, 5));
  eng.submit(limit(2, Side::Sell, 105, 5));
  rec.clear();
  eng.submit({3, 0, 0, Side::Buy, OrderType::IOC, 102, 10});
  ASSERT_EQ(rec.fills().size(), 1u);
  EXPECT_EQ(std::get<Cancelled>(rec.ev.back()), (Cancelled{3, 5, CancelReason::ImmediateOrCancel}));
  EXPECT_FALSE(eng.best(0, Side::Buy));
  EXPECT_EQ(eng.best(0, Side::Sell)->price, 105u);
}

TEST_F(EngineTest, FokAllOrNothing) {
  eng.submit(limit(1, Side::Sell, 100, 5));
  eng.submit(limit(2, Side::Sell, 101, 5));
  rec.clear();
  EXPECT_FALSE(eng.submit({3, 0, 0, Side::Buy, OrderType::FOK, 101, 11}));
  EXPECT_EQ(std::get<Rejected>(rec.ev.back()).r, RejectReason::FokInsufficientLiquidity);
  EXPECT_EQ(eng.best(0, Side::Sell)->qty, 5u);  // untouched
  EXPECT_TRUE(eng.submit({4, 0, 0, Side::Buy, OrderType::FOK, 101, 10}));
  EXPECT_EQ(rec.fills().size(), 2u);
  EXPECT_FALSE(eng.best(0, Side::Sell));
}

TEST_F(EngineTest, PostOnlyRejectedIfCrossing) {
  eng.submit(limit(1, Side::Sell, 100, 5));
  EXPECT_FALSE(eng.submit({2, 0, 0, Side::Buy, OrderType::PostOnly, 100, 5}));
  EXPECT_TRUE(eng.submit({3, 0, 0, Side::Buy, OrderType::PostOnly, 99, 5}));
  EXPECT_EQ(eng.best(0, Side::Buy)->price, 99u);
}

TEST_F(EngineTest, RejectsInvalidInput) {
  EXPECT_FALSE(eng.submit(limit(1, Side::Buy, 100, 0)));
  EXPECT_FALSE(eng.submit(limit(1, Side::Buy, 0, 10)));
  EXPECT_FALSE(eng.submit({1, 0, 9, Side::Buy, OrderType::Limit, 100, 10}));
  EXPECT_TRUE(eng.submit(limit(1, Side::Buy, 100, 10)));
  EXPECT_FALSE(eng.submit(limit(1, Side::Buy, 100, 10)));
  EXPECT_FALSE(eng.cancel(42));
  EXPECT_EQ(eng.stats().rejects, 5u);
}

TEST_F(EngineTest, CancelAndModify) {
  eng.submit(limit(1, Side::Buy, 100, 10));
  eng.submit(limit(2, Side::Buy, 100, 10));
  EXPECT_TRUE(eng.modify(1, 100, 4));  // reduce in place: keeps priority
  rec.clear();
  eng.submit(limit(3, Side::Sell, 100, 4));
  ASSERT_EQ(rec.fills().size(), 1u);
  EXPECT_EQ(rec.fills()[0].maker, 1u);
  EXPECT_TRUE(eng.modify(2, 100, 20));  // increase: loses priority (re-queued)
  eng.submit(limit(4, Side::Buy, 100, 1));
  EXPECT_TRUE(eng.cancel(4));
  EXPECT_FALSE(eng.cancel(4));
  EXPECT_TRUE(eng.modify(2, 101, 20));  // price move
  EXPECT_EQ(eng.best(0, Side::Buy)->price, 101u);
  rec.clear();
  eng.submit(limit(5, Side::Sell, 102, 1));
  EXPECT_TRUE(eng.modify(5, 101, 1));  // repriced into the bid: trades immediately
  ASSERT_EQ(rec.fills().size(), 1u);
  EXPECT_EQ(rec.fills()[0].maker, 2u);
  EXPECT_TRUE(eng.check_invariants());
}

TEST(EngineStp, CancelRestingRemovesOwnOrdersAndKeepsMatching) {
  Recorder rec;
  MatchingEngine<Recorder> eng(small(StpMode::CancelResting), &rec);
  eng.submit(limit(1, Side::Sell, 100, 5, /*owner=*/7));
  eng.submit(limit(2, Side::Sell, 100, 5, /*owner=*/8));
  rec.clear();
  eng.submit(limit(3, Side::Buy, 100, 5, 7));
  ASSERT_EQ(rec.fills().size(), 1u);
  EXPECT_EQ(rec.fills()[0].maker, 2u);
  EXPECT_FALSE(eng.order_qty(1));
}

TEST(EngineStp, CancelIncomingStopsAtOwnOrder) {
  Recorder rec;
  MatchingEngine<Recorder> eng(small(StpMode::CancelIncoming), &rec);
  eng.submit(limit(1, Side::Sell, 100, 5, 8));
  eng.submit(limit(2, Side::Sell, 100, 5, 7));
  eng.submit(limit(3, Side::Sell, 100, 5, 8));
  rec.clear();
  eng.submit(limit(4, Side::Buy, 100, 15, 7));
  ASSERT_EQ(rec.fills().size(), 1u);
  EXPECT_EQ(std::get<Cancelled>(rec.ev.back()), (Cancelled{4, 10, CancelReason::SelfTradePrevention}));
  EXPECT_EQ(*eng.order_qty(2), 5u);
}

// Randomized differential test against the std::map reference engine: identical inputs
// must produce byte-identical event streams and identical books, for every STP mode.
class EngineDifferential : public ::testing::TestWithParam<StpMode> {};

TEST_P(EngineDifferential, MatchesReferenceEngine) {
  Recorder ra, rb;
  MatchingEngine<Recorder> fast(small(GetParam()), &ra);
  ReferenceEngine<Recorder> ref(2, GetParam(), &rb);
  std::mt19937_64 rng(static_cast<std::uint64_t>(GetParam()) + 1000);
  std::vector<OrderId> ids;
  OrderId next = 1;
  for (int i = 0; i < 150'000; ++i) {
    const int op = static_cast<int>(rng() % 100);
    const auto inst = static_cast<InstrumentId>(rng() % 2);
    const Side s = rng() % 2 ? Side::Buy : Side::Sell;
    const Price mid = 10'000;
    const auto off = static_cast<Price>(rng() % 20);
    const Price p = s == Side::Buy ? mid - 10 + off : mid + 10 - off;
    const auto q = static_cast<Qty>(1 + rng() % 300);
    const auto owner = static_cast<OwnerId>(rng() % 4);
    if (op < 55) {
      NewOrder o{next, owner, inst, s, OrderType::Limit, p, q};
      if (op < 5) o.type = OrderType::Market;
      else if (op < 10) o.type = OrderType::IOC;
      else if (op < 13) o.type = OrderType::FOK;
      else if (op < 17) o.type = OrderType::PostOnly;
      if (rng() % 500 == 0) o.qty = 0;  // occasional invalid
      ASSERT_EQ(fast.submit(o), ref.submit(o));
      ids.push_back(next++);
    } else if (op < 85 && !ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];
      ASSERT_EQ(fast.cancel(id), ref.cancel(id));
    } else if (!ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];
      const Price np = rng() % 2 ? p : p + 1;
      ASSERT_EQ(fast.modify(id, np, q), ref.modify(id, np, q));
    }
    ASSERT_EQ(ra.ev.size(), rb.ev.size()) << "step " << i;
    if (i % 1000 == 0) {
      for (InstrumentId k = 0; k < 2; ++k) {
        for (Side sd : {Side::Buy, Side::Sell}) {
          std::vector<LevelView> da, db;
          fast.depth(k, sd, 100, da);
          ref.depth(k, sd, 100, db);
          ASSERT_EQ(da, db) << "step " << i;
        }
      }
    }
  }
  ASSERT_EQ(ra.ev, rb.ev);
  EXPECT_EQ(fast.live_orders(), ref.live_orders());
  EXPECT_TRUE(fast.check_invariants());
  EXPECT_GT(fast.stats().fills, 10'000u);
}

INSTANTIATE_TEST_SUITE_P(AllStpModes, EngineDifferential,
                         ::testing::Values(StpMode::None, StpMode::CancelResting, StpMode::CancelIncoming));
