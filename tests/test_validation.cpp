#include <gtest/gtest.h>

#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/book/validation.hpp"
#include "ironbook/itch/encoder.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/itch/synthetic_feed.hpp"

using namespace ib;

namespace {
MarketConfig cfg() {
  MarketConfig c;
  c.max_orders = 1u << 20;
  c.max_levels = 1u << 18;
  c.expected_live_orders = 1u << 16;
  return c;
}

template <class M>
FeedValidator replay(const std::vector<std::uint8_t>& bytes, M& m, FeedStats* out = nullptr) {
  FeedValidator v;
  FeedHandler<M, FeedValidator> fh(m, v);
  itch::ParseStats st;
  itch::parse_frames(std::span(bytes), fh, st);
  if (out) *out = fh.stats();
  return v;
}

void open_market(itch::Encoder& e, Locate loc) {
  e.system_event(1, 'Q');
  e.trading_action(loc, 2, "TST", 'T');
}
}  // namespace

TEST(Validation, SyntheticFeedIsFullyConsistent) {
  itch::SyntheticFeedConfig fc;
  fc.messages = 200'000;
  const auto feed = itch::make_synthetic_feed(fc);
  Market m(cfg());
  FeedStats fs;
  const auto v = replay(feed, m, &fs);
  const auto& r = v.report();
  EXPECT_EQ(fs.errors(), 0u);
  EXPECT_EQ(r.exec_unknown, 0u);
  EXPECT_EQ(r.locate_mismatch, 0u);
  EXPECT_GT(r.e_checked, 1000u);
  EXPECT_EQ(r.e_at_best, r.e_checked);
  EXPECT_EQ(r.e_at_head, r.e_checked);
  EXPECT_GT(r.cross_checks, 0u);
  EXPECT_EQ(r.crossed, 0u);
}

TEST(Validation, DetectsOutOfPriorityExecutionAndCrossedBook) {
  itch::Encoder e;
  open_market(e, 1);
  e.add(1, 10, 1, Side::Buy, 100, "TST", 1000);
  e.add(1, 11, 2, Side::Buy, 100, "TST", 1000);
  e.add(1, 12, 3, Side::Buy, 100, "TST", 990);
  e.executed(1, 13, 2, 50, 1);  // at best, but not head of queue
  e.executed(1, 14, 3, 50, 2);  // not at best level
  e.add(1, 15, 4, Side::Sell, 100, "TST", 995);  // crosses the 1000 bid
  e.add(1, 16, 5, Side::Sell, 100, "TST", 1000); // still crossed (bid 1000 > ask 995)
  e.executed(1, 17, 99, 1, 3);  // unknown order

  Market m(cfg());
  FeedStats fs;
  const auto r = replay(e.bytes(), m, &fs).report();
  EXPECT_EQ(r.e_checked, 2u);
  EXPECT_EQ(r.e_at_best, 1u);
  EXPECT_EQ(r.e_at_head, 1u);  // order 3 heads its own (non-best) level
  EXPECT_EQ(r.crossed, 2u);
  EXPECT_EQ(r.exec_unknown, 1u);
  EXPECT_EQ(fs.unknown_order, 1u);
}

TEST(Validation, TapeAggregatesVolumeAndVwap) {
  itch::Encoder e;
  open_market(e, 1);
  e.add(1, 10, 1, Side::Sell, 300, "TST", 100'0000);
  e.executed(1, 11, 1, 100, 1);                         // 100 @ 100.00
  e.executed_price(1, 12, 1, 100, 2, true, 101'0000);   // 100 @ 101.00
  e.executed_price(1, 13, 1, 50, 3, false, 102'0000);   // non-printable: excluded
  e.trade(1, 14, 0, Side::Buy, 200, "TST", 99'0000, 4); // hidden 200 @ 99.00
  e.cross(1, 15, 1000, "TST", 100'0000, 5, 'C');        // closing cross 1000 @ 100.00
  NaiveMarket m;
  const auto v = replay(e.bytes(), m);
  const SymbolTape& t = v.tape()[1];
  EXPECT_EQ(t.volume, 1400u);
  EXPECT_EQ(t.trades, 4u);
  EXPECT_NEAR(t.vwap(), (100.0 * 100 + 100 * 101 + 200 * 99 + 1000 * 100) / 1400.0, 1e-9);
  EXPECT_EQ(v.report().c_nonprintable, 1u);
  EXPECT_EQ(v.report().c_price_improved, 2u);
}
