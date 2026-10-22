#include <gtest/gtest.h>

#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/validation.hpp"
#include "ironbook/itch/synthetic_feed.hpp"
#include "ironbook/pipeline/pipeline.hpp"

using namespace ib;

namespace {
MarketConfig cfg() {
  MarketConfig c;
  c.max_orders = 1u << 20;
  c.max_levels = 1u << 18;
  c.expected_live_orders = 1u << 16;
  return c;
}
}  // namespace

TEST(Events, RoundTripThroughNormalizerAndApply) {
  struct Sink {
    std::vector<BookEvent> v;
    BookEvent& next() { return v.emplace_back(); }
  } sink;
  Normalizer<Sink> norm(sink);
  itch::Encoder enc;
  enc.add(5, 100, 77, Side::Sell, 300, "X", 12345);
  enc.replace(5, 101, 77, 78, 200, 12300);
  enc.executed_price(5, 102, 78, 50, 9, true, 12301);
  itch::ParseStats st;
  itch::parse_frames(std::span(enc.bytes()), norm, st);
  ASSERT_EQ(sink.v.size(), 3u);
  EXPECT_EQ(sink.v[0].type, EventType::Add);
  EXPECT_EQ(static_cast<Side>(sink.v[0].flags), Side::Sell);
  EXPECT_EQ(sink.v[0].aux, 100u);
  EXPECT_EQ(sink.v[1].aux, 78u);  // new ref
  EXPECT_EQ(sink.v[2].price, 12301u);

  Market m(cfg());
  FeedHandler<Market> fh(m);
  for (const auto& e : sink.v) apply_event(e, fh);
  EXPECT_EQ(m.best(5, Side::Sell)->qty, 150u);
  EXPECT_EQ(m.best(5, Side::Sell)->price, 12300u);
}

// The two-thread pipeline must build exactly the same books as single-threaded replay,
// for a range of queue sizes / batch sizes (tiny queues force constant full/empty
// transitions and wrap-arounds).
TEST(Pipeline, MatchesSingleThreadedReplay) {
  itch::SyntheticFeedConfig fc;
  fc.symbols = 16;
  fc.messages = 300'000;
  fc.seed = 21;
  const auto feed = itch::make_synthetic_feed(fc);

  Market ref(cfg());
  FeedHandler<Market> ref_fh(ref);
  itch::ParseStats ref_st;
  itch::parse_frames(std::span(feed), ref_fh, ref_st);

  for (auto [cap, batch, prefetch] : {std::tuple{16u, 1u, 0u}, {64u, 7u, 4u}, {4096u, 256u, 8u}}) {
    Market m(cfg());
    FeedValidator v;
    FeedHandler<Market, FeedValidator> fh(m, v);
    PipelineConfig pc;
    pc.queue_capacity = cap;
    pc.batch = batch;
    pc.prefetch_d1 = prefetch * 2;
    pc.prefetch_d2 = prefetch;
    pc.latency_sample_every = 64;
    const auto st = run_pipeline(std::span(feed), fh, pc);
    EXPECT_EQ(st.parse.messages, ref_st.messages);
    EXPECT_EQ(st.events, ref_st.messages);
    EXPECT_GT(st.latency.count(), 0u);
    EXPECT_EQ(fh.stats().errors(), 0u);
    EXPECT_EQ(m.order_count(), ref.order_count());
    for (int s = 0; s < fc.symbols; ++s) {
      const Locate loc = itch::SyntheticFeed::locate(s);
      ASSERT_EQ(m.digest(loc), ref.digest(loc)) << "cap=" << cap << " symbol=" << s;
    }
    EXPECT_EQ(v.report().e_at_head, v.report().e_checked);
  }
}
