// engine_sim: throughput / latency benchmark for the matching engine.
//
//   engine_sim [--orders N] [--instruments K] [--seed S] [--impl fast|reference]
//              [--stp none|resting|incoming] [--json OUT]
//              [--seed-itch FILE --symbol AAPL --at 10:00:00]
//
// A deterministic synthetic order flow (passive/aggressive limits, market, IOC, FOK,
// post-only, cancels, amends around a random-walking mid) is generated up front, so
// only engine work is timed. Every operation is timed individually with the cycle
// counter and histogrammed by type.
//
// --seed-itch first reconstructs the real NASDAQ book for --symbol at time --at from
// the ITCH file and loads every resting order (in exact FIFO priority) into the engine,
// so the synthetic flow trades against a real book shape.

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "cli.hpp"
#include "ironbook/book/feed_handler.hpp"
#include "ironbook/book/market.hpp"
#include "ironbook/common/histogram.hpp"
#include "ironbook/common/profiler.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/concurrency/thread_utils.hpp"
#include "ironbook/engine/matching_engine.hpp"
#include "ironbook/engine/reference_engine.hpp"
#include "ironbook/itch/file_reader.hpp"
#include "ironbook/itch/symbols.hpp"

using namespace ib;
using namespace ib::engine;

namespace {

enum class Op : std::uint8_t { Passive, Aggressive, Market, Ioc, Fok, PostOnly, Cancel, Modify, kCount };
constexpr const char* kOpNames[] = {"passive", "aggressive", "market", "ioc", "fok", "post_only", "cancel", "modify"};

struct Action {
  Op op;
  NewOrder order;  // for cancel/modify: id (+ price/qty for modify)
};

struct Seed {
  std::vector<NewOrder> resting;
  Price mid = 100 * kPriceScale;
};

// Counts fills/cancels so the optimizer cannot drop the event path.
struct CountingListener {
  std::uint64_t fills = 0, volume = 0, cancels = 0, rejects = 0;
  void on_fill(const Fill& f) {
    ++fills;
    volume += f.qty;
  }
  void on_cancelled(OrderId, Qty, CancelReason) { ++cancels; }
  void on_rejected(OrderId, RejectReason) { ++rejects; }
};

std::vector<Action> make_flow(std::uint64_t n, InstrumentId instruments, std::uint64_t seed, Price start_mid,
                              OrderId first_id) {
  std::mt19937_64 rng(seed);
  std::vector<Price> mid(instruments, start_mid);
  std::vector<OrderId> live;  // generator's view (may include already-filled ids)
  live.reserve(n);
  std::vector<Action> flow;
  flow.reserve(n);
  // passive, aggressive, market, ioc, fok, post-only, cancel, modify
  const double w[] = {0.40, 0.08, 0.02, 0.06, 0.02, 0.02, 0.37, 0.03};
  std::discrete_distribution<int> pick(std::begin(w), std::end(w));
  std::geometric_distribution<int> depth(0.3);
  OrderId next = first_id;
  constexpr Price tick = kPriceScale / 100;  // one cent

  for (std::uint64_t i = 0; i < n; ++i) {
    const auto inst = static_cast<InstrumentId>(rng() % instruments);
    if (rng() % 32 == 0) mid[inst] = rng() % 2 ? mid[inst] + tick : mid[inst] - tick;
    const Side side = rng() % 2 ? Side::Buy : Side::Sell;
    const auto qty = static_cast<Qty>(100 * (1 + rng() % 5));
    auto op = static_cast<Op>(pick(rng));
    if ((op == Op::Cancel || op == Op::Modify) && live.empty()) op = Op::Passive;
    Action a{op, {}};
    a.order.instrument = inst;
    a.order.side = side;
    a.order.qty = qty;
    a.order.owner = static_cast<OwnerId>(1 + rng() % 64);
    const auto away = static_cast<Price>(1 + depth(rng)) * tick;
    const Price passive_px = side == Side::Buy ? mid[inst] - away : mid[inst] + away;
    const Price cross_px = side == Side::Buy ? mid[inst] + static_cast<Price>(rng() % 4) * tick
                                             : mid[inst] - static_cast<Price>(rng() % 4) * tick;
    switch (op) {
      case Op::Passive: a.order.type = OrderType::Limit; a.order.price = passive_px; break;
      case Op::Aggressive: a.order.type = OrderType::Limit; a.order.price = cross_px; break;
      case Op::Market: a.order.type = OrderType::Market; a.order.price = 0; break;
      case Op::Ioc: a.order.type = OrderType::IOC; a.order.price = cross_px; break;
      case Op::Fok: a.order.type = OrderType::FOK; a.order.price = cross_px; break;
      case Op::PostOnly: a.order.type = OrderType::PostOnly; a.order.price = passive_px; break;
      case Op::Cancel:
      case Op::Modify: {
        const std::size_t k = rng() % live.size();
        a.order.id = live[k];
        if (op == Op::Cancel) {
          live[k] = live.back();
          live.pop_back();
        } else {
          a.order.price = passive_px;
          a.order.qty = qty;
        }
        flow.push_back(a);
        continue;
      }
      default: break;
    }
    a.order.id = next++;
    if (a.order.type == OrderType::Limit || a.order.type == OrderType::PostOnly) live.push_back(a.order.id);
    flow.push_back(a);
  }
  return flow;
}

struct Stop {};

// Feeds book messages to the handler until the first message at or after `at`.
struct ReplayUntil {
  FeedHandler<Market>& fh;
  itch::SymbolTable& symbols;
  Timestamp at;
  template <class M>
  void on(const M& m) {
    if (m.h.timestamp >= at) throw Stop{};
    if constexpr (std::is_same_v<M, itch::StockDirectory>) symbols.on(m);
    if constexpr (requires { fh.on(m); }) fh.on(m);
  }
};

// Reconstruct `symbol`'s book at time `at` and export every resting order in priority order.
Seed seed_from_itch(const std::string& path, const std::string& symbol, Timestamp at) {
  MarketConfig mc;
  mc.max_orders = 1u << 25;
  mc.max_levels = 1u << 23;
  mc.expected_live_orders = 1u << 22;
  Market market(mc);
  FeedHandler<Market> fh(market);
  itch::SymbolTable symbols;
  ReplayUntil until{fh, symbols, at};
  try {
    itch::replay_file(path, until);
  } catch (const Stop&) {
  }
  Seed seed;
  const auto loc = symbols.find(symbol);
  if (!loc) {
    std::fprintf(stderr, "symbol %s not found\n", symbol.c_str());
    return seed;
  }
  const OrderBook& book = market.book(*loc);
  const BookPools& pools = market.pools();
  for (Side s : {Side::Buy, Side::Sell}) {
    const PriceLadder& lad = book.ladder(s);
    for (std::size_t i = 0; i < lad.size(); ++i) {
      const Level& l = pools.levels[lad.from_top(i)];
      for (std::uint32_t oi = l.head; oi != kNil; oi = pools.orders[oi].next) {
        const Order& o = pools.orders[oi];
        seed.resting.push_back({o.id, 0, 0, s, OrderType::Limit, l.price, o.qty});
      }
    }
  }
  const auto bid = book.best(Side::Buy), ask = book.best(Side::Sell);
  if (bid && ask) seed.mid = (bid->price + ask->price) / 2 / 100 * 100;
  std::printf("seeded %zu resting orders from %s @ %s: bid %s x %llu / ask %s x %llu\n", seed.resting.size(),
              symbol.c_str(), cli::format_time(at).c_str(), bid ? format_price(bid->price).c_str() : "-",
              static_cast<unsigned long long>(bid ? bid->qty : 0), ask ? format_price(ask->price).c_str() : "-",
              static_cast<unsigned long long>(ask ? ask->qty : 0));
  return seed;
}

template <class Engine>
void run(Engine& eng, const Seed& seed, const std::vector<Action>& flow, CountingListener& l, bool timed,
         const char* label, FILE* json) {
  for (const auto& o : seed.resting) eng.submit(o);
  const std::uint64_t f0 = l.fills;

  std::vector<LatencyHistogram> hist(static_cast<std::size_t>(Op::kCount));
  ScopedProfile profile;
  Stopwatch sw;
  for (const Action& a : flow) {
    const std::uint64_t t0 = timed ? CycleClock::now() : 0;
    switch (a.op) {
      case Op::Cancel: eng.cancel(a.order.id); break;
      case Op::Modify: eng.modify(a.order.id, a.order.price, a.order.qty); break;
      default: eng.submit(a.order); break;
    }
    if (timed) hist[static_cast<std::size_t>(a.op)].record(CycleClock::now() - t0);
  }
  const double sec = sw.elapsed_sec();
  const double rate = static_cast<double>(flow.size()) / sec;
  std::printf("%-10s %llu ops in %.3f s -> %.2f M ops/s (%.1f ns/op); fills=%llu volume=%llu live=%zu\n", label,
              static_cast<unsigned long long>(flow.size()), sec, rate / 1e6, 1e9 / rate,
              static_cast<unsigned long long>(l.fills - f0), static_cast<unsigned long long>(l.volume),
              eng.live_orders());
  if (!timed) return;
  const double npt = CycleClock::ns_per_tick();
  LatencyHistogram all;
  for (std::size_t i = 0; i < hist.size(); ++i) {
    all.merge(hist[i]);
    if (hist[i].count()) std::printf("    %-10s %s\n", kOpNames[i], hist[i].summary(npt).c_str());
  }
  std::printf("    %-10s %s\n", "all", all.summary(npt).c_str());
  if (json) {
    std::fprintf(json, "{\"impl\":\"%s\",\"ops\":%zu,\"seconds\":%.6f,\"ops_per_sec\":%.1f", label, flow.size(), sec, rate);
    for (std::size_t i = 0; i <= hist.size(); ++i) {
      const LatencyHistogram& h = i < hist.size() ? hist[i] : all;
      const char* name = i < hist.size() ? kOpNames[i] : "all";
      if (!h.count()) continue;
      std::fprintf(json, ",\"%s\":{\"n\":%llu,\"p50\":%.1f,\"p99\":%.1f,\"p999\":%.1f,\"max\":%.1f}", name,
                   static_cast<unsigned long long>(h.count()), static_cast<double>(h.percentile(0.5)) * npt,
                   static_cast<double>(h.percentile(0.99)) * npt, static_cast<double>(h.percentile(0.999)) * npt,
                   static_cast<double>(h.max()) * npt);
    }
    std::fprintf(json, "}\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  pin_current_thread(0);
  cli::Args args(argc, argv);
  const auto n = args.get_u64("orders", 10'000'000);
  const auto instruments = static_cast<InstrumentId>(args.get_u64("instruments", 1));
  const auto seed_val = args.get_u64("seed", 42);
  const std::string impl = args.get("impl", "fast");
  const bool timed = !args.has("no-latency");
  const std::string stp_s = args.get("stp", "none");
  const StpMode stp = stp_s == "resting" ? StpMode::CancelResting : stp_s == "incoming" ? StpMode::CancelIncoming : StpMode::None;

  Seed seed;
  if (args.has("seed-itch"))
    seed = seed_from_itch(args.get("seed-itch"), args.get("symbol", "AAPL"), cli::parse_time(args.get("at", "10:00:00")));
  OrderId first_id = 1;
  for (const auto& o : seed.resting) first_id = std::max(first_id, o.id + 1);
  const auto flow = make_flow(n, instruments, seed_val, seed.mid, first_id);
  std::printf("flow: %zu actions over %u instrument(s), mid %s, clock %s (%.2f ns/tick)\n", flow.size(), instruments,
              format_price(seed.mid).c_str(), CycleClock::source_name(), CycleClock::ns_per_tick());

  FILE* json = args.has("json") ? std::fopen(args.get("json").c_str(), "a") : nullptr;
  CountingListener l;
  if (impl == "reference") {
    ReferenceEngine<CountingListener> eng(instruments, stp, &l);
    run(eng, seed, flow, l, timed, "reference", json);
  } else {
    EngineConfig cfg;
    cfg.instruments = instruments;
    cfg.stp = stp;
    cfg.max_orders = 1u << 24;
    cfg.max_levels = 1u << 20;
    cfg.expected_live_orders = args.get_u64("expected-live", 1u << 18);
    MatchingEngine<CountingListener> eng(cfg, &l);
    run(eng, seed, flow, l, timed, "fast", json);
    if (!eng.check_invariants()) {
      std::fprintf(stderr, "INVARIANT VIOLATION\n");
      return 1;
    }
  }
  if (json) std::fclose(json);
  std::printf("cancels=%llu rejects=%llu\n", static_cast<unsigned long long>(l.cancels),
              static_cast<unsigned long long>(l.rejects));
  return 0;
}
