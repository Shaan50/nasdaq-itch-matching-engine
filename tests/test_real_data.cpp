// Tests against real NASDAQ data. tests/data/ holds small slices cut from the
// 2019-12-30 TotalView-ITCH 5.0 sample file by `itch_slice` (see README). Tests skip
// if a slice is missing.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/book/validation.hpp"
#include "ironbook/common/mapped_file.hpp"
#include "ironbook/itch/file_reader.hpp"
#include "ironbook/itch/symbols.hpp"
#include "ironbook/pipeline/pipeline.hpp"

using namespace ib;

namespace {

std::string slice(const char* name) { return std::string(IRONBOOK_TEST_DATA_DIR) + "/" + name; }

struct Counter {
  itch::SymbolTable symbols;
  std::uint64_t adds = 0;
  void on(const itch::StockDirectory& m) { symbols.on(m); }
  void on(const itch::AddOrder&) { ++adds; }
};

MarketConfig cfg() {
  MarketConfig c;
  c.max_orders = 1u << 22;
  c.max_levels = 1u << 20;
  c.expected_live_orders = 1u << 20;
  return c;
}

// Replays a slice through naive + optimized markets (with validation) and checks that
// both agree on every book and that the feed is internally consistent.
void check_slice(const std::string& path) {
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "missing " << path;
  NaiveMarket naive;
  Market fast(cfg());
  FeedValidator va, vb;
  FeedHandler<NaiveMarket, FeedValidator> a(naive, va);
  FeedHandler<Market, FeedValidator> b(fast, vb);
  const auto sa = itch::replay_file(path, a);
  const auto sb = itch::replay_file(path, b);
  EXPECT_GT(sa.messages, 10'000u);
  EXPECT_EQ(sa.messages, sb.messages);
  EXPECT_EQ(sa.unknown + sa.bad_length, 0u);
  EXPECT_EQ(a.stats().errors(), 0u);
  EXPECT_EQ(b.stats().errors(), 0u);
  EXPECT_EQ(naive.order_count(), fast.order_count());
  for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
    ASSERT_EQ(naive.digest(static_cast<Locate>(loc)), fast.digest(static_cast<Locate>(loc))) << loc;
  }
  EXPECT_EQ(vb.report().crossed, 0u);
  EXPECT_EQ(vb.report().e_at_best, vb.report().e_checked);

  // The pipelined replay of the same bytes must agree too.
  MappedFile f(path);
  Market piped(cfg());
  FeedHandler<Market> pfh(piped);
  run_pipeline(f.bytes(), pfh);
  for (std::size_t loc = 0; loc < kMaxLocates; ++loc)
    ASSERT_EQ(piped.digest(static_cast<Locate>(loc)), fast.digest(static_cast<Locate>(loc)));
}

}  // namespace

TEST(RealData, OpeningSliceDirectoryAndParse) {
  const auto path = slice("open_slice.itch");
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "missing " << path;
  Counter c;
  const auto st = itch::replay_file(path, c);
  EXPECT_EQ(st.unknown + st.bad_length, 0u);
  EXPECT_GT(c.symbols.size(), 8000u);  // full directory is sent before the open
  EXPECT_TRUE(c.symbols.find("AAPL").has_value());
  EXPECT_TRUE(c.symbols.find("MSFT").has_value());
  EXPECT_GT(c.adds, 0u);
}

TEST(RealData, OpeningSliceBooksAgree) { check_slice(slice("open_slice.itch")); }

TEST(RealData, FullDaySymbolSliceBooksAgree) { check_slice(slice("symbols_slice.itch")); }
