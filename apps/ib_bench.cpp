// ib_bench: end-to-end replay benchmark harness over a real ITCH file.
//
//   ib_bench FILE --mode MODE [--repeat R] [--max N] [--latency] [--json OUT]
//                 [--table N] [--prefault] [--batch B --d1 D1 --d2 D2]
//
// Modes (each is one row of the optimization log):
//   parse      decode only (handler touches fields; no book)
//   naive      NaiveMarket: std::map levels + std::list FIFO + std::unordered_map index
//   std-index  pooled intrusive book + sorted ladder, std::unordered_map index
//   flat       ... + open-addressing FlatHashMap (--hash murmur|identity|xorfold|fibonacci)
//   batched    flat + decode-ahead batches with two-stage software prefetch
//   pipeline   parser thread -> SPSC ring -> book thread (with staged prefetch)
//
// --mlock wires the loaded input and all book state into RAM so a busy machine cannot
// swap/compress it mid-run (the benchmark host has 16 GB and other workloads).
// --latency records per-message service time with the cycle counter, histogrammed by
// message type (single-threaded modes). --load copies the first --max messages into RAM
// before timing (CPU-bound numbers); otherwise the file is streamed from the mmap.

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "cli.hpp"
#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/common/histogram.hpp"
#include "ironbook/common/mapped_file.hpp"
#include "ironbook/common/profiler.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/concurrency/thread_utils.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/pipeline/batched_replay.hpp"
#include "ironbook/pipeline/pipeline.hpp"

using namespace ib;

namespace {

struct Options {
  std::uint64_t max = ~0ull;
  std::size_t table = 1u << 23;
  bool prefault = false;
  bool latency = false;
  bool mlock = false;
  BatchConfig batch;
};

struct Result {
  std::string mode;
  std::uint64_t messages = 0;
  double seconds = 0;
  std::vector<std::pair<std::string, LatencyHistogram>> latency;  // ticks
  LatencyHistogram queue_latency;
  bool has_queue_latency = false;
};

// Decode-only handler: folds a few fields so the optimizer cannot drop the decode.
struct ParseOnly {
  std::uint64_t acc = 0;
  template <class M>
  IB_ALWAYS_INLINE void on(const M& m) {
    acc += m.h.timestamp;
    if constexpr (requires { m.ref; }) acc ^= m.ref;
    if constexpr (requires { m.shares; }) acc += m.shares;
  }
};

// Per-message-type service-time histograms around an inner handler.
template <class Inner>
struct Timed {
  Inner& inner;
  LatencyHistogram add, exec, cancel, del, replace, other;
  template <class M>
  IB_ALWAYS_INLINE void on(const M& m) {
    const std::uint64_t t0 = CycleClock::now();
    if constexpr (requires { inner.on(m); }) inner.on(m);
    const std::uint64_t dt = CycleClock::now() - t0;
    if constexpr (std::is_same_v<M, itch::AddOrder>) add.record(dt);
    else if constexpr (std::is_same_v<M, itch::OrderExecuted> || std::is_same_v<M, itch::OrderExecutedWithPrice>) exec.record(dt);
    else if constexpr (std::is_same_v<M, itch::OrderCancel>) cancel.record(dt);
    else if constexpr (std::is_same_v<M, itch::OrderDelete>) del.record(dt);
    else if constexpr (std::is_same_v<M, itch::OrderReplace>) replace.record(dt);
    else other.record(dt);
  }
  void export_to(Result& r) const {
    LatencyHistogram all;
    for (const auto* h : {&add, &exec, &cancel, &del, &replace, &other}) all.merge(*h);
    r.latency = {{"all", all},       {"add", add},         {"execute", exec}, {"cancel", cancel},
                 {"delete", del},    {"replace", replace}, {"other", other}};
  }
};

MarketConfig market_config(const Options& o) {
  MarketConfig c;
  c.max_orders = 1u << 25;
  c.max_levels = 1u << 23;
  c.expected_live_orders = o.table;
  c.prefault = o.prefault;
  return c;
}

// Order/level pools are sized for the busiest prefix of the day (peak ~2M live orders).
constexpr std::uint32_t kLockOrders = 3u << 20, kLockLevels = 1u << 20;

template <class MarketT>
void lock(MarketT& m, const Options& o) {
  if constexpr (requires { m.lock_memory(1u, 1u); }) {
    if (o.mlock && !m.lock_memory(kLockOrders, kLockLevels)) std::fprintf(stderr, "WARNING: mlock(book) failed\n");
  }
}

template <class H>
std::uint64_t replay(std::span<const std::uint8_t> data, H& h, const Options& o) {
  itch::ParseStats st;
  itch::parse_frames(data, h, st, o.max);
  return st.messages;
}

template <class MarketT>
Result run_market(const std::string& mode, std::span<const std::uint8_t> data, const Options& o) {
  Result r;
  r.mode = mode;
  auto make = [&] {
    if constexpr (std::is_same_v<MarketT, NaiveMarket>) return std::make_unique<NaiveMarket>();
    else return std::make_unique<MarketT>(market_config(o));
  };
  auto m = make();
  lock(*m, o);
  FeedHandler<MarketT> fh(*m);
  Stopwatch sw;
  if (o.latency) {
    Timed<FeedHandler<MarketT>> t{fh, {}, {}, {}, {}, {}, {}};
    r.messages = replay(data, t, o);
    r.seconds = sw.elapsed_sec();
    t.export_to(r);
  } else {
    r.messages = replay(data, fh, o);
    r.seconds = sw.elapsed_sec();
  }
  if constexpr (requires { m->index().mean_probe_length(); }) {
    std::printf("    index: live=%zu capacity=%zu load=%.3f mean_probe=%.2f longest_cluster=%zu\n",
                m->index().size(), m->index().capacity(),
                static_cast<double>(m->index().size()) / static_cast<double>(m->index().capacity()),
                m->index().mean_probe_length(), m->index().longest_cluster());
  }
  if (fh.stats().errors()) std::fprintf(stderr, "WARNING: %llu feed errors\n", static_cast<unsigned long long>(fh.stats().errors()));
  return r;
}

void print(const Result& r) {
  const double mps = static_cast<double>(r.messages) / r.seconds;
  std::printf("%-10s %12llu msgs  %7.3f s  %8.2f M msg/s  %7.1f ns/msg\n", r.mode.c_str(),
              static_cast<unsigned long long>(r.messages), r.seconds, mps / 1e6, 1e9 / mps);
  const double npt = CycleClock::ns_per_tick();
  for (const auto& [name, h] : r.latency)
    if (h.count()) std::printf("    %-8s %s\n", name.c_str(), h.summary(npt).c_str());
  if (r.has_queue_latency) std::printf("    queue    %s\n", r.queue_latency.summary(npt).c_str());
}

void write_json(FILE* f, const Result& r, const Options& o, double median, const std::string& label) {
  const double npt = CycleClock::ns_per_tick();
  auto hist = [&](const LatencyHistogram& h) {
    std::fprintf(f, "{\"n\":%llu,\"mean\":%.2f,\"p50\":%.1f,\"p90\":%.1f,\"p99\":%.1f,\"p999\":%.1f,\"p9999\":%.1f,\"max\":%.1f,\"buckets\":[",
                 static_cast<unsigned long long>(h.count()), h.mean() * npt, static_cast<double>(h.percentile(0.5)) * npt,
                 static_cast<double>(h.percentile(0.9)) * npt, static_cast<double>(h.percentile(0.99)) * npt,
                 static_cast<double>(h.percentile(0.999)) * npt, static_cast<double>(h.percentile(0.9999)) * npt,
                 static_cast<double>(h.max()) * npt);
    bool first = true;
    for (auto [ub, c] : h.nonzero_buckets()) {
      std::fprintf(f, "%s[%.1f,%llu]", first ? "" : ",", static_cast<double>(ub) * npt, static_cast<unsigned long long>(c));
      first = false;
    }
    std::fprintf(f, "]}");
  };
  std::fprintf(f, "{\"label\":\"%s\",\"median_msgs_per_sec\":%.1f,\"mode\":\"%s\",\"messages\":%llu,\"seconds\":%.6f,\"msgs_per_sec\":%.1f,\"ns_per_msg\":%.3f,"
               "\"table\":%zu,\"prefault\":%s,\"batch\":%zu,\"d1\":%zu,\"d2\":%zu,\"clock\":\"%s\",\"ns_per_tick\":%.4f",
               label.c_str(), median, r.mode.c_str(), static_cast<unsigned long long>(r.messages), r.seconds,
               static_cast<double>(r.messages) / r.seconds, r.seconds * 1e9 / static_cast<double>(r.messages), o.table,
               o.prefault ? "true" : "false", o.batch.batch, o.batch.d1, o.batch.d2, CycleClock::source_name(), npt);
  if (!r.latency.empty()) {
    std::fprintf(f, ",\"latency\":{");
    bool first = true;
    for (const auto& [name, h] : r.latency) {
      std::fprintf(f, "%s\"%s\":", first ? "" : ",", name.c_str());
      hist(h);
      first = false;
    }
    std::fprintf(f, "}");
  }
  if (r.has_queue_latency) {
    std::fprintf(f, ",\"queue_latency\":");
    hist(r.queue_latency);
  }
  std::fprintf(f, "}\n");
}

}  // namespace

int main(int argc, char** argv) {
  pin_current_thread(0);  // P-core QoS on macOS / CPU 0 on Linux
  cli::Args args(argc, argv);
  if (args.positional().empty()) {
    std::fprintf(stderr, "usage: ib_bench FILE --mode parse|naive|std-index|flat|batched|pipeline [--hash H]\n"
                         "       [--repeat R] [--max N] [--load] [--mlock] [--latency] [--json OUT] [--table N] [--prefault]\n"
                         "       [--batch B] [--d1 D] [--d2 D] [--queue N] [--qbatch N] [--sample K]\n");
    return 2;
  }
  Options o;
  o.max = args.get_u64("max", ~0ull);
  o.table = args.get_u64("table", 1u << 23);
  o.prefault = args.has("prefault");
  o.latency = args.has("latency");
  o.mlock = args.has("mlock");
  o.batch.batch = args.get_u64("batch", 256);
  o.batch.d1 = args.get_u64("d1", 16);
  o.batch.d2 = args.get_u64("d2", 8);
  const std::string mode = args.get("mode", "flat");
  const auto repeat = args.get_u64("repeat", 1);

  MappedFile file(args.positional()[0]);
  std::span<const std::uint8_t> data = file.bytes();
  // --load copies exactly the first --max messages into an owned, fully touched buffer,
  // so CPU-bound runs never page-fault or hit the page cache. Without it the file is
  // streamed through the mmap (end-to-end numbers that include I/O).
  VirtualArena loaded;
  if (args.has("load")) {
    itch::FrameCursor cur(data);
    std::uint64_t n = 0;
    while (n < o.max && !cur.next().empty()) ++n;
    const auto len = static_cast<std::size_t>(cur.position() - data.data());
    loaded = VirtualArena(len);
    std::memcpy(loaded.data(), data.data(), len);
    if (o.mlock && !loaded.lock(len)) std::fprintf(stderr, "WARNING: mlock(input) failed\n");
    data = {static_cast<const std::uint8_t*>(loaded.data()), len};
    std::printf("loaded %llu messages (%.2f GB) into memory\n", static_cast<unsigned long long>(n),
                static_cast<double>(len) / 1e9);
  }
  std::printf("clock: %s (%.3f ns/tick)\n", CycleClock::source_name(), CycleClock::ns_per_tick());

  FILE* json = args.has("json") ? std::fopen(args.get("json").c_str(), "a") : nullptr;
  ScopedProfile profile;  // IRONBOOK_PROFILE=out.prof samples the timed region only
  Result best;
  std::vector<double> rates;
  for (std::uint64_t rep = 0; rep < repeat; ++rep) {
    Result r;
    if (mode == "parse") {
      r.mode = mode;
      ParseOnly h;
      Stopwatch sw;
      if (o.latency) {
        Timed<ParseOnly> t{h, {}, {}, {}, {}, {}, {}};
        r.messages = replay(data, t, o);
        r.seconds = sw.elapsed_sec();
        t.export_to(r);
      } else {
        r.messages = replay(data, h, o);
        r.seconds = sw.elapsed_sec();
      }
      do_not_optimize(h.acc);
    } else if (mode == "naive") {
      r = run_market<NaiveMarket>(mode, data, o);
    } else if (mode == "std-index") {
      r = run_market<BasicMarket<StdIndex>>(mode, data, o);
    } else if (mode == "flat" || mode == "batched" || mode == "pipeline") {
      const std::string hash = args.get("hash", "murmur");
      auto run = [&]<class H>(H) {
        using M = BasicMarket<FlatHashMap<H>>;
        const std::string label = mode + "/" + H::name;
        if (mode == "flat") return run_market<M>(label, data, o);
        Result res;
        res.mode = label;
        auto m = std::make_unique<M>(market_config(o));
        lock(*m, o);
        FeedHandler<M> fh(*m);
        if (mode == "batched") {
          std::uint64_t dec = 0, app = 0;
          BatchConfig bc = o.batch;
          if (args.has("split")) {
            bc.decode_ticks = &dec;
            bc.apply_ticks = &app;
          }
          Stopwatch sw;
          res.messages = batched_replay(data, fh, bc, o.max).messages;
          res.seconds = sw.elapsed_sec();
          if (args.has("split")) {
            const double n = static_cast<double>(res.messages);
            std::printf("    phase split: decode %.1f ns/msg, apply %.1f ns/msg\n",
                        CycleClock::to_ns(dec) / n, CycleClock::to_ns(app) / n);
          }
          return res;
        }
        PipelineConfig pc;
        pc.queue_capacity = args.get_u64("queue", 1u << 16);
        pc.batch = args.get_u64("qbatch", 256);
        pc.prefetch_d1 = o.batch.d1;
        pc.prefetch_d2 = o.batch.d2;
        pc.latency_sample_every = args.get_u64("sample", 0);
        const auto ps = run_pipeline(data, fh, pc, o.max);
        res.messages = ps.parse.messages;
        res.seconds = ps.seconds;
        res.queue_latency = ps.latency;
        res.has_queue_latency = pc.latency_sample_every != 0;
        std::printf("    producer full-spins=%llu consumer empty-spins=%llu\n",
                    static_cast<unsigned long long>(ps.producer_full_spins),
                    static_cast<unsigned long long>(ps.consumer_empty_spins));
        return res;
      };
      if (args.get("probe", "robinhood") == "linear") {
        // Ablation: plain linear probing (pre-Robin-Hood) for the optimization log.
        if (hash == "identity") r = run_market<BasicMarket<FlatHashMap<IdentityHash, Probing::Linear>>>(mode + "/identity-linear", data, o);
        else r = run_market<BasicMarket<FlatHashMap<Murmur3Mix, Probing::Linear>>>(mode + "/murmur-linear", data, o);
      } else if (hash == "identity") r = run(IdentityHash{});
      else if (hash == "xorfold") r = run(XorFoldHash{});
      else if (hash == "fibonacci") r = run(FibonacciHash{});
      else r = run(Murmur3Mix{});
    } else {
      std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
      return 2;
    }
    print(r);
    rates.push_back(static_cast<double>(r.messages) / r.seconds);
    if (rep == 0 || r.seconds < best.seconds) best = std::move(r);
  }
  std::sort(rates.begin(), rates.end());
  const double median = rates[rates.size() / 2];
  if (repeat > 1)
    std::printf("best of %llu: %.2f M msg/s   median: %.2f M msg/s\n", static_cast<unsigned long long>(repeat),
                rates.back() / 1e6, median / 1e6);
  if (json) {
    write_json(json, best, o, median, args.get("label", best.mode));
    std::fclose(json);
  }
  return 0;
}
