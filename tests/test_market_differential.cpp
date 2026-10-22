// Differential test: the optimized Market must reproduce the naive std::map/std::list
// baseline exactly (every level, every order, FIFO order) on long synthetic feeds.

#include <gtest/gtest.h>

#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/itch/synthetic_feed.hpp"

using namespace ib;

namespace {

// Applies each message to both markets and compares the top of book of the touched
// symbol after every message.
struct Lockstep {
  NaiveMarket& naive;
  Market& fast;
  FeedHandler<NaiveMarket> a{naive};
  FeedHandler<Market> b{fast};
  std::uint64_t compared = 0;
  bool ok = true;

  template <class M>
  void on(const M& m) {
    if constexpr (requires { a.on(m); }) {
      a.on(m);
      b.on(m);
      check(m.h.locate);
    }
  }
  void check(Locate loc) {
    ++compared;
    for (Side s : {Side::Buy, Side::Sell}) {
      const auto x = naive.best(loc, s);
      const auto y = fast.best(loc, s);
      if (x.has_value() != y.has_value() || (x && !(*x == *y))) ok = false;
    }
  }
};

void run(const itch::SyntheticFeedConfig& cfg) {
  const auto feed = itch::make_synthetic_feed(cfg);
  NaiveMarket naive;
  MarketConfig mc;
  mc.max_orders = 1u << 20;
  mc.max_levels = 1u << 18;
  mc.expected_live_orders = 1u << 16;
  Market fast(mc);
  Lockstep ls{naive, fast};
  itch::ParseStats st;
  itch::parse_frames(std::span(feed), ls, st);

  ASSERT_TRUE(ls.ok);
  EXPECT_GT(ls.compared, cfg.messages / 2);
  EXPECT_EQ(ls.a.stats().errors(), 0u);
  EXPECT_EQ(ls.b.stats().errors(), 0u);
  EXPECT_EQ(naive.order_count(), fast.order_count());
  for (int s = 0; s < cfg.symbols; ++s) {
    const Locate loc = itch::SyntheticFeed::locate(s);
    EXPECT_EQ(naive.digest(loc), fast.digest(loc)) << "symbol " << s;
    EXPECT_TRUE(fast.book(loc).check_invariants());
  }
}

}  // namespace

TEST(MarketDifferential, ManySymbolsShallowBooks) {
  itch::SyntheticFeedConfig cfg;
  cfg.symbols = 32;
  cfg.messages = 400'000;
  cfg.seed = 11;
  run(cfg);
}

TEST(MarketDifferential, FewSymbolsDeepBooks) {
  itch::SyntheticFeedConfig cfg;
  cfg.symbols = 2;
  cfg.messages = 300'000;
  cfg.max_live_per_symbol = 20'000;
  cfg.w_add = 0.55;
  cfg.seed = 12;
  run(cfg);
}

TEST(MarketDifferential, ReplaceHeavy) {
  itch::SyntheticFeedConfig cfg;
  cfg.symbols = 4;
  cfg.messages = 200'000;
  cfg.w_replace = 0.4;
  cfg.seed = 13;
  run(cfg);
}
