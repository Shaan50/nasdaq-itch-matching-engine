// Google Benchmark micro-benchmarks for the individual components. End-to-end replay of
// the real NASDAQ file is measured by the custom harness (apps/ib_bench.cpp).

#include <benchmark/benchmark.h>

#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/flat_hash_map.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/common/histogram.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/concurrency/spsc_queue.hpp"
#include "ironbook/engine/matching_engine.hpp"
#include "ironbook/engine/reference_engine.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/itch/synthetic_feed.hpp"
#include "ironbook/pipeline/batched_replay.hpp"

using namespace ib;

namespace {

const std::vector<std::uint8_t>& synthetic_feed() {
  static const auto feed = [] {
    itch::SyntheticFeedConfig c;
    c.symbols = 64;
    c.messages = 2'000'000;
    c.seed = 7;
    return itch::make_synthetic_feed(c);
  }();
  return feed;
}

MarketConfig bench_market() {
  MarketConfig c;
  c.max_orders = 1u << 22;
  c.max_levels = 1u << 20;
  c.expected_live_orders = 1u << 18;
  return c;
}

// ------------------------------------------------------------------ parser
struct Sink {
  std::uint64_t acc = 0;
  template <class M>
  void on(const M& m) {
    acc += m.h.timestamp;
    if constexpr (requires { m.ref; }) acc ^= m.ref;
  }
};

void BM_ParseSyntheticFeed(benchmark::State& st) {
  const auto& feed = synthetic_feed();
  std::uint64_t msgs = 0;
  for (auto _ : st) {
    Sink s;
    itch::ParseStats ps;
    itch::parse_frames(std::span(feed), s, ps);
    benchmark::DoNotOptimize(s.acc);
    msgs += ps.messages;
  }
  st.SetItemsProcessed(static_cast<std::int64_t>(msgs));
  st.SetBytesProcessed(st.iterations() * static_cast<std::int64_t>(feed.size()));
}
BENCHMARK(BM_ParseSyntheticFeed)->Unit(benchmark::kMillisecond);

// ------------------------------------------------------------------ order-id index
// Simulates the book's index workload: a sliding window of live sequential ids, each
// step inserting one new id, looking up one random live id, and erasing the oldest.
template <class Map>
void index_churn(benchmark::State& st, Map& m) {
  const auto live = static_cast<std::uint64_t>(st.range(0));
  std::mt19937_64 rng(1);
  std::uint64_t next = 1;
  for (; next <= live; ++next) m.insert(next, static_cast<std::uint32_t>(next));
  std::uint64_t acc = 0;
  for (auto _ : st) {
    m.insert(next, static_cast<std::uint32_t>(next));
    acc += m.find(next - 1 - rng() % live);
    m.erase(next - live);
    ++next;
  }
  benchmark::DoNotOptimize(acc);
  st.SetItemsProcessed(st.iterations());
}

template <class H, Probing P = Probing::RobinHood>
void BM_FlatHashMapChurn(benchmark::State& st) {
  FlatHashMap<H, P> m(static_cast<std::size_t>(st.range(0)) * 2);
  index_churn(st, m);
}
void BM_StdUnorderedMapChurn(benchmark::State& st) {
  StdIndex m(static_cast<std::size_t>(st.range(0)) * 2);
  index_churn(st, m);
}
BENCHMARK(BM_StdUnorderedMapChurn)->Arg(1 << 16)->Arg(1 << 21);
BENCHMARK(BM_FlatHashMapChurn<Murmur3Mix>)->Arg(1 << 16)->Arg(1 << 21);
BENCHMARK(BM_FlatHashMapChurn<IdentityHash>)->Arg(1 << 16)->Arg(1 << 21);
BENCHMARK(BM_FlatHashMapChurn<XorFoldHash>)->Arg(1 << 16)->Arg(1 << 21);
// Plain linear probing: deletes scan the whole dense run of sequential keys.
BENCHMARK(BM_FlatHashMapChurn<IdentityHash, Probing::Linear>)->Arg(1 << 12)->Arg(1 << 16);
BENCHMARK(BM_FlatHashMapChurn<Murmur3Mix, Probing::Linear>)->Arg(1 << 16)->Arg(1 << 21);

// ------------------------------------------------------------------ price ladder
// Add+remove a level at distance `range(0)` from the touch of a 200-level ladder.
void BM_LadderInsertEraseAtDepth(benchmark::State& st) {
  PriceLadder lad;
  for (std::uint32_t k = 0; k < 200; ++k) lad.insert_at(lad.size(), 1000 + 2 * k, k + 1);
  const auto depth = static_cast<std::uint32_t>(st.range(0));
  const std::uint32_t key = 1000 + 2 * (199 - depth) + 1;  // between two existing levels
  for (auto _ : st) {
    std::size_t pos;
    benchmark::DoNotOptimize(lad.find(key, pos));
    lad.insert_at(pos, key, 999);
    lad.erase(key);
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_LadderInsertEraseAtDepth)->Arg(0)->Arg(4)->Arg(16)->Arg(64)->Arg(190);

// ------------------------------------------------------------------ book replay
template <class M>
void BM_ReplaySyntheticFeed(benchmark::State& st) {
  const auto& feed = synthetic_feed();
  std::uint64_t msgs = 0;
  for (auto _ : st) {
    st.PauseTiming();
    auto m = [] {
      if constexpr (std::is_same_v<M, NaiveMarket>) return std::make_unique<M>();
      else return std::make_unique<M>(bench_market());
    }();
    FeedHandler<M> fh(*m);
    st.ResumeTiming();
    itch::ParseStats ps;
    itch::parse_frames(std::span(feed), fh, ps);
    msgs += ps.messages;
    st.PauseTiming();
    m.reset();
    st.ResumeTiming();
  }
  st.SetItemsProcessed(static_cast<std::int64_t>(msgs));
}
BENCHMARK(BM_ReplaySyntheticFeed<NaiveMarket>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ReplaySyntheticFeed<BasicMarket<StdIndex>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ReplaySyntheticFeed<BasicMarket<FlatHashMap<Murmur3Mix>>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ReplaySyntheticFeed<Market>)->Unit(benchmark::kMillisecond);

void BM_BatchedReplaySyntheticFeed(benchmark::State& st) {
  const auto& feed = synthetic_feed();
  BatchConfig bc;
  bc.d1 = static_cast<std::size_t>(st.range(0));
  bc.d2 = bc.d1 / 2;
  std::uint64_t msgs = 0;
  for (auto _ : st) {
    st.PauseTiming();
    auto m = std::make_unique<BasicMarket<FlatHashMap<Murmur3Mix>>>(bench_market());
    FeedHandler<BasicMarket<FlatHashMap<Murmur3Mix>>> fh(*m);
    st.ResumeTiming();
    msgs += batched_replay(std::span(feed), fh, bc).messages;
    st.PauseTiming();
    m.reset();
    st.ResumeTiming();
  }
  st.SetItemsProcessed(static_cast<std::int64_t>(msgs));
}
BENCHMARK(BM_BatchedReplaySyntheticFeed)->Arg(0)->Arg(8)->Arg(16)->Unit(benchmark::kMillisecond);

// ------------------------------------------------------------------ SPSC queue
struct Msg32 {
  std::uint64_t a, b, c, d;
};

void BM_SpscThroughput(benchmark::State& st) {
  const auto batch = static_cast<std::size_t>(st.range(0));
  constexpr std::uint64_t kItems = 10'000'000;
  for (auto _ : st) {
    SpscQueue<Msg32> q(1u << 14);
    std::thread consumer([&] {
      std::uint64_t got = 0, sum = 0;
      while (got < kItems) {
        const Msg32* p;
        const std::size_t n = q.peek(p);
        for (std::size_t i = 0; i < n; ++i) sum += p[i].a;
        q.release(n);
        got += n;
      }
      benchmark::DoNotOptimize(sum);
    });
    for (std::uint64_t i = 0; i < kItems;) {
      Msg32* w;
      std::size_t n = q.reserve(w);
      n = std::min<std::size_t>({n, batch, static_cast<std::size_t>(kItems - i)});
      for (std::size_t j = 0; j < n; ++j) w[j] = Msg32{i + j, 0, 0, 0};
      q.commit(n);
      i += n;
    }
    consumer.join();
  }
  st.SetItemsProcessed(st.iterations() * static_cast<std::int64_t>(kItems));
}
BENCHMARK(BM_SpscThroughput)->Arg(1)->Arg(16)->Arg(256)->Unit(benchmark::kMillisecond)->UseRealTime();

// Round-trip latency: ping-pong one item through two queues.
void BM_SpscPingPong(benchmark::State& st) {
  SpscQueue<std::uint64_t> to(1024), from(1024);
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    std::uint64_t v;
    while (!stop.load(std::memory_order_relaxed)) {
      if (to.try_pop(v)) {
        while (!from.try_push(v)) cpu_relax();
      }
    }
  });
  std::uint64_t v = 0, r;
  for (auto _ : st) {
    while (!to.try_push(v)) cpu_relax();
    while (!from.try_pop(r)) cpu_relax();
    ++v;
  }
  stop = true;
  echo.join();
  st.SetLabel("round trip");
}
BENCHMARK(BM_SpscPingPong)->UseRealTime();

// ------------------------------------------------------------------ matching engine
struct NoopListener {};

template <class Engine>
void engine_flow(benchmark::State& st, Engine& eng) {
  std::mt19937_64 rng(3);
  OrderId id = 1;
  std::vector<OrderId> live;
  for (auto _ : st) {
    const auto r = rng() % 100;
    const Side s = rng() % 2 ? Side::Buy : Side::Sell;
    const auto off = static_cast<Price>(rng() % 10);
    if (r < 55) {
      const Price p = s == Side::Buy ? 10000 - 1 - off : 10000 + 1 + off;
      eng.submit({id, 0, 0, s, engine::OrderType::Limit, p, 100});
      live.push_back(id++);
    } else if (r < 70) {
      const Price p = s == Side::Buy ? 10000 + 3 : 10000 - 3;
      eng.submit({id++, 0, 0, s, engine::OrderType::IOC, p, 300});
    } else if (!live.empty()) {
      const std::size_t k = rng() % live.size();
      eng.cancel(live[k]);
      live[k] = live.back();
      live.pop_back();
    }
  }
  st.SetItemsProcessed(st.iterations());
}

void BM_MatchingEngine(benchmark::State& st) {
  engine::EngineConfig c;
  c.max_orders = 1u << 22;
  c.prefault = true;
  engine::MatchingEngine<NoopListener> eng(c, nullptr);
  engine_flow(st, eng);
}
void BM_ReferenceEngine(benchmark::State& st) {
  NoopListener l;
  engine::ReferenceEngine<NoopListener> eng(1, engine::StpMode::None, &l);
  engine_flow(st, eng);
}
BENCHMARK(BM_MatchingEngine);
BENCHMARK(BM_ReferenceEngine);

// ------------------------------------------------------------------ instrumentation cost
void BM_CycleClockRead(benchmark::State& st) {
  for (auto _ : st) benchmark::DoNotOptimize(CycleClock::now());
}
BENCHMARK(BM_CycleClockRead);

void BM_HistogramRecord(benchmark::State& st) {
  LatencyHistogram h;
  std::uint64_t v = 1;
  for (auto _ : st) {
    h.record(v);
    v = v * 6364136223846793005ull + 1442695040888963407ull;
    v >>= 50;
  }
  benchmark::DoNotOptimize(h.count());
}
BENCHMARK(BM_HistogramRecord);

}  // namespace
