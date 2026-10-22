// itch_replay: reconstruct every order book for a trading day and validate it.
//
//   itch_replay FILE [--validate] [--diff] [--pipeline] [--max N]
//                    [--book SYMBOL --at HH:MM:SS[,HH:MM:SS...] --depth 10]
//                    [--tape 20] [--bbo-csv out.csv --bbo-symbols AAPL,MSFT --bbo-every-ms 1000]
//
//   --validate   run FeedValidator (priority/crossing/integrity checks, trade tape)
//   --diff       replay through NaiveMarket and Market in lockstep, comparing the touched
//                symbol's top of book after every message and every book digest at the end
//   --pipeline   use the two-thread parser -> SPSC -> book pipeline

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"
#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/book/naive_market.hpp"
#include "ironbook/book/validation.hpp"
#include "ironbook/common/mapped_file.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/concurrency/thread_utils.hpp"
#include "ironbook/itch/file_reader.hpp"
#include "ironbook/itch/symbols.hpp"
#include "ironbook/pipeline/pipeline.hpp"

using namespace ib;

namespace {

MarketConfig full_day_config() {
  MarketConfig c;
  c.max_orders = 1u << 26;
  c.max_levels = 1u << 24;
  c.expected_live_orders = 1u << 23;
  return c;
}

void print_book(const Market& m, Locate loc, std::string_view sym, std::size_t depth, Timestamp t) {
  std::vector<LevelView> bids, asks;
  m.depth(loc, Side::Buy, depth, bids);
  m.depth(loc, Side::Sell, depth, asks);
  std::printf("\n%.*s @ %s\n", static_cast<int>(sym.size()), sym.data(), cli::format_time(t).c_str());
  std::printf("  %8s %10s %12s | %-12s %-10s %-8s\n", "orders", "bid qty", "bid", "ask", "ask qty", "orders");
  for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
    char l[64] = "", r[64] = "";
    if (i < bids.size())
      std::snprintf(l, sizeof l, "%8u %10llu %12s", bids[i].orders,
                    static_cast<unsigned long long>(bids[i].qty), format_price(bids[i].price).c_str());
    if (i < asks.size())
      std::snprintf(r, sizeof r, "%-12s %-10llu %-8u", format_price(asks[i].price).c_str(),
                    static_cast<unsigned long long>(asks[i].qty), asks[i].orders);
    std::printf("  %32s | %s\n", l, r);
  }
}

// Wraps the feed handler with time-triggered side effects (book snapshots, BBO sampling)
// and tracks the peak number of live orders.
template <class Inner>
struct Driver {
  Driver(Inner& i, Market& m) : inner(i), market(m) {}
  Inner& inner;
  Market& market;
  itch::SymbolTable symbols;
  std::vector<Timestamp> snapshot_times;
  std::size_t next_snapshot = 0;
  std::optional<Locate> book_loc;
  std::string book_sym;
  std::size_t depth = 10;
  FILE* bbo_csv = nullptr;
  std::vector<Locate> bbo_locs;
  Timestamp bbo_every = 0, next_bbo = 0;
  std::size_t peak_orders = 0;
  std::uint64_t msgs = 0;

  template <class M>
  void on(const M& m) {
    if constexpr (std::is_same_v<M, itch::StockDirectory>) {
      symbols.on(m);
      if (!book_sym.empty() && itch::trim(m.stock) == book_sym) book_loc = m.h.locate;
    }
    const Timestamp t = m.h.timestamp;
    while (next_snapshot < snapshot_times.size() && t >= snapshot_times[next_snapshot]) {
      if (book_loc) print_book(market, *book_loc, book_sym, depth, snapshot_times[next_snapshot]);
      ++next_snapshot;
    }
    if (bbo_csv && t >= next_bbo && t > 0) {
      if (next_bbo == 0) next_bbo = t;
      while (next_bbo <= t) {
        write_bbo(next_bbo);
        next_bbo += bbo_every;
      }
    }
    if constexpr (requires { inner.on(m); }) inner.on(m);
    if constexpr (std::is_same_v<M, itch::AddOrder>) {
      if ((++msgs & 0xFFFF) == 0) peak_orders = std::max(peak_orders, market.order_count());
    }
  }

  void write_bbo(Timestamp t) {
    for (Locate loc : bbo_locs) {
      const auto b = market.best(loc, Side::Buy);
      const auto a = market.best(loc, Side::Sell);
      const auto name = symbols.name(loc);
      std::fprintf(bbo_csv, "%llu,%.*s,%s,%llu,%s,%llu\n", static_cast<unsigned long long>(t),
                   static_cast<int>(name.size()), name.data(), b ? format_price(b->price).c_str() : "",
                   static_cast<unsigned long long>(b ? b->qty : 0), a ? format_price(a->price).c_str() : "",
                   static_cast<unsigned long long>(a ? a->qty : 0));
    }
  }
};

// Lockstep naive-vs-optimized differential replay.
struct Lockstep {
  NaiveMarket& naive;
  Market& fast;
  std::uint64_t checkpoint_every = 20'000'000;
  FeedHandler<NaiveMarket> a{naive};
  FeedHandler<Market> b{fast};
  std::uint64_t compared = 0, mismatches = 0, msgs = 0;
  std::uint64_t checkpoints = 0, books_digested = 0, orders_digested = 0, digest_mismatches = 0;

  // Full-book digest of every symbol (every level, every order id/qty in FIFO order).
  void checkpoint() {
    ++checkpoints;
    for (std::size_t l = 0; l < kMaxLocates; ++l) {
      const auto loc = static_cast<Locate>(l);
      if (fast.book(loc).empty() && !naive.best(loc, Side::Buy) && !naive.best(loc, Side::Sell)) continue;
      ++books_digested;
      if (naive.digest(loc) != fast.digest(loc)) ++digest_mismatches;
    }
    orders_digested += fast.order_count();
  }

  template <class M>
  void on(const M& m) {
    if constexpr (requires { a.on(m); }) {
      a.on(m);
      b.on(m);
      ++compared;
      const Locate loc = m.h.locate;
      for (Side s : {Side::Buy, Side::Sell}) {
        const auto x = naive.best(loc, s);
        const auto y = fast.best(loc, s);
        if (x.has_value() != y.has_value() || (x && !(*x == *y))) ++mismatches;
      }
    }
    if (++msgs % checkpoint_every == 0) checkpoint();
  }
};

void print_feed_stats(const FeedStats& f) {
  std::printf("\nfeed: adds=%llu exec=%llu exec_px=%llu cancel=%llu delete=%llu replace=%llu trade=%llu cross=%llu\n",
              static_cast<unsigned long long>(f.adds), static_cast<unsigned long long>(f.executes),
              static_cast<unsigned long long>(f.executes_with_price), static_cast<unsigned long long>(f.cancels),
              static_cast<unsigned long long>(f.deletes), static_cast<unsigned long long>(f.replaces),
              static_cast<unsigned long long>(f.trades), static_cast<unsigned long long>(f.crosses));
  std::printf("integrity: unknown_order=%llu duplicate_add=%llu overfill=%llu\n",
              static_cast<unsigned long long>(f.unknown_order), static_cast<unsigned long long>(f.duplicate_add),
              static_cast<unsigned long long>(f.overfill));
}

void print_validation(const FeedValidator& v, const itch::SymbolTable& symbols, std::size_t tape_n) {
  const auto& r = v.report();
  auto pct = [](std::uint64_t a, std::uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
  std::printf("\nvalidation:\n");
  std::printf("  executions matched to a resting order : %llu (unknown %llu, locate mismatch %llu)\n",
              static_cast<unsigned long long>(r.executions), static_cast<unsigned long long>(r.exec_unknown),
              static_cast<unsigned long long>(r.locate_mismatch));
  std::printf("  continuous 'E' executions checked     : %llu\n", static_cast<unsigned long long>(r.e_checked));
  std::printf("    at best price level                 : %llu (%.4f%%)\n", static_cast<unsigned long long>(r.e_at_best), pct(r.e_at_best, r.e_checked));
  std::printf("    at head of FIFO queue               : %llu (%.4f%%)\n", static_cast<unsigned long long>(r.e_at_head), pct(r.e_at_head, r.e_checked));
  std::printf("    from an Order Replace: %.2f%% of all checked, %.2f%% of the non-head ones\n",
              pct(r.e_from_replace, r.e_checked), pct(r.e_not_head_from_replace, r.e_checked - r.e_at_head));
  const std::uint64_t nh = r.e_checked - r.e_at_head;
  std::printf("    non-head: 1 order skipped %.1f%%; skipped head MPID-attributed %.1f%% (base rate %.1f%%); odd lot %.1f%% (base rate %.1f%%)\n",
              pct(r.e_not_head_ahead1, nh), pct(r.e_not_head_skipped_mpid, nh), pct(r.e_mpid_heads, r.e_checked),
              pct(r.e_not_head_skipped_oddlot, nh), pct(r.e_oddlot_heads, r.e_checked));
  std::printf("  'C' executions printable / non-print  : %llu / %llu (price != limit: %llu)\n",
              static_cast<unsigned long long>(r.c_printable), static_cast<unsigned long long>(r.c_nonprintable),
              static_cast<unsigned long long>(r.c_price_improved));
  std::printf("  crossed-book checks (continuous)      : %llu, crossed %llu (%.5f%%), locked %llu\n",
              static_cast<unsigned long long>(r.cross_checks), static_cast<unsigned long long>(r.crossed),
              pct(r.crossed, r.cross_checks), static_cast<unsigned long long>(r.locked));

  if (tape_n == 0) return;
  std::vector<std::pair<std::uint64_t, Locate>> by_vol;
  for (std::size_t l = 0; l < v.tape().size(); ++l)
    if (v.tape()[l].volume) by_vol.emplace_back(v.tape()[l].volume, static_cast<Locate>(l));
  std::sort(by_vol.rbegin(), by_vol.rend());
  std::uint64_t total = 0;
  for (auto& [vol, l] : by_vol) total += vol;
  std::printf("\nNASDAQ-executed volume: %s shares across %zu symbols. Top %zu:\n",
              cli::human(static_cast<double>(total)).c_str(), by_vol.size(), tape_n);
  std::printf("  %-8s %14s %10s %10s %10s %10s\n", "symbol", "volume", "trades", "vwap", "low", "high");
  for (std::size_t i = 0; i < by_vol.size() && i < tape_n; ++i) {
    const auto& t = v.tape()[by_vol[i].second];
    const auto name = symbols.name(by_vol[i].second);
    std::printf("  %-8.*s %14llu %10llu %10.4f %10s %10s\n", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(t.volume), static_cast<unsigned long long>(t.trades), t.vwap(),
                format_price(t.low).c_str(), format_price(t.high).c_str());
  }
}

// Directory messages arrive first; resolves BBO symbols once the directory is complete.
template <class D>
struct Resolve {
  D& d;
  const std::vector<std::string>& syms;
  bool done = false;
  template <class M>
  void on(const M& m) {
    if constexpr (!std::is_same_v<M, itch::StockDirectory>) {
      if (!done && !syms.empty()) {
        for (const auto& s : syms)
          if (auto loc = d.symbols.find(s)) d.bbo_locs.push_back(*loc);
        done = true;
      }
    }
    d.on(m);
  }
};

}  // namespace

int main(int argc, char** argv) {
  pin_current_thread(0);  // P-core QoS on macOS / CPU 0 on Linux
  cli::Args args(argc, argv);
  if (args.positional().empty()) {
    std::fprintf(stderr,
                 "usage: itch_replay FILE [--validate] [--diff] [--pipeline] [--max N]\n"
                 "       [--book SYM --at HH:MM:SS,... --depth N] [--tape N]\n"
                 "       [--bbo-csv OUT --bbo-symbols A,B --bbo-every-ms MS]\n");
    return 2;
  }
  const std::string path = args.positional()[0];
  const auto max = args.get_u64("max", ~0ull);
  Market market(full_day_config());

  if (args.has("diff")) {
    NaiveMarket naive;
    Lockstep ls{naive, market, args.get_u64("checkpoint-every", 20'000'000)};
    Stopwatch sw;
    const auto st = itch::replay_file(path, ls, max);
    std::printf("lockstep replay: %llu messages in %.1f s, %llu top-of-book comparisons\n",
                static_cast<unsigned long long>(st.messages), sw.elapsed_sec(),
                static_cast<unsigned long long>(ls.compared));
    ls.checkpoint();  // end of day
    std::printf("top-of-book mismatches: %llu\n", static_cast<unsigned long long>(ls.mismatches));
    std::printf("full-book digests: %llu checkpoints, %llu book snapshots (%llu resting orders) compared, %llu mismatches\n",
                static_cast<unsigned long long>(ls.checkpoints), static_cast<unsigned long long>(ls.books_digested),
                static_cast<unsigned long long>(ls.orders_digested),
                static_cast<unsigned long long>(ls.digest_mismatches));
    const auto digest_mismatch = ls.digest_mismatches;
    std::printf("live orders: naive=%zu optimized=%zu\n", naive.order_count(), market.order_count());
    print_feed_stats(ls.b.stats());
    return (ls.mismatches || digest_mismatch) ? 1 : 0;
  }

  FeedValidator validator;
  FeedHandler<Market, FeedValidator> fh(market, validator);
  Driver<FeedHandler<Market, FeedValidator>> drv(fh, market);
  drv.book_sym = args.get("book");
  drv.depth = args.get_u64("depth", 10);
  for (const auto& t : cli::split(args.get("at"))) drv.snapshot_times.push_back(cli::parse_time(t));
  std::sort(drv.snapshot_times.begin(), drv.snapshot_times.end());

  FILE* bbo = nullptr;
  std::vector<std::string> bbo_syms;
  if (args.has("bbo-csv")) {
    bbo = std::fopen(args.get("bbo-csv").c_str(), "w");
    std::fprintf(bbo, "ts_ns,symbol,bid,bid_qty,ask,ask_qty\n");
    bbo_syms = cli::split(args.get("bbo-symbols", "AAPL"));
    drv.bbo_every = args.get_u64("bbo-every-ms", 1000) * 1'000'000ull;
  }

  Stopwatch sw;
  itch::ParseStats st;
  if (args.has("pipeline")) {
    if (args.has("validate") || args.has("book") || bbo)
      std::fprintf(stderr, "note: --pipeline replays without validation/snapshots\n");
    MappedFile f(path);
    FeedHandler<Market> plain(market);
    PipelineConfig pc;
    pc.latency_sample_every = 1024;
    const auto ps = run_pipeline(f.bytes(), plain, pc, max);
    st = ps.parse;
    print_feed_stats(plain.stats());
    std::printf("pipeline latency (parser emit -> book apply): %s\n",
                ps.latency.summary(CycleClock::ns_per_tick()).c_str());
  } else if (args.has("validate") || args.has("book") || bbo) {
    Resolve<Driver<FeedHandler<Market, FeedValidator>>> resolve{drv, bbo_syms};
    if (bbo) drv.bbo_csv = bbo;
    st = itch::replay_file(path, resolve, max);
    print_feed_stats(fh.stats());
  } else {
    FeedHandler<Market> plain(market);
    st = itch::replay_file(path, plain, max);
    print_feed_stats(plain.stats());
  }
  const double sec = sw.elapsed_sec();
  if (bbo) std::fclose(bbo);

  std::printf("\nreplayed %llu messages (%.2f GB) in %.2f s -> %s msg/s, %.1f ns/msg\n",
              static_cast<unsigned long long>(st.messages), static_cast<double>(st.bytes) / 1e9, sec,
              cli::human(static_cast<double>(st.messages) / sec).c_str(), sec * 1e9 / static_cast<double>(st.messages));
  std::printf("live orders at end: %zu (peak sampled: %zu), order-index capacity %zu, rehashes %zu\n",
              market.order_count(), std::max(drv.peak_orders, market.order_count()), market.index().capacity(),
              market.index().rehashes());
  std::printf("order pool high water: %u, level pool high water: %u\n", market.pools().orders.high_water(),
              market.pools().levels.high_water());
  if (args.has("validate")) print_validation(validator, drv.symbols, args.get_u64("tape", 15));
  return 0;
}
