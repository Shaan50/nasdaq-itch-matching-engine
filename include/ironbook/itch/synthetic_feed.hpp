#pragma once

// Generates a self-consistent synthetic ITCH 5.0 stream (used by tests and benchmarks
// that must not depend on the multi-GB NASDAQ file).
//
// The generator keeps its own independent std::map model of every book so that what it
// emits is always legal: executions hit the head of the best level, cancels never exceed
// the resting quantity, adds never cross the opposite side, replaces keep the side.
// The message mix roughly follows a real NASDAQ day (adds and deletes dominate).

#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "ironbook/itch/encoder.hpp"

namespace ib::itch {

struct SyntheticFeedConfig {
  int symbols = 16;
  std::uint64_t messages = 1'000'000;
  std::uint64_t seed = 1;
  // Relative weights of each message kind.
  double w_add = 0.44, w_delete = 0.36, w_replace = 0.08, w_cancel = 0.03, w_execute = 0.06,
         w_execute_price = 0.01, w_trade = 0.02;
  int max_live_per_symbol = 4000;
};

class SyntheticFeed {
 public:
  explicit SyntheticFeed(const SyntheticFeedConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
    books_.resize(static_cast<std::size_t>(cfg.symbols));
    for (int s = 0; s < cfg.symbols; ++s) books_[static_cast<std::size_t>(s)].mid = 100'000 + 50'000 * static_cast<Price>(s % 7);
  }

  std::vector<std::uint8_t> generate() {
    Encoder enc;
    Timestamp ts = 3ull * 3600 * 1'000'000'000ull;
    enc.system_event(ts, 'O');
    for (int s = 0; s < cfg_.symbols; ++s) enc.stock_directory(locate(s), ++ts, symbol(s));
    enc.system_event(++ts, 'S');
    ts = 34'200ull * 1'000'000'000ull;  // 09:30
    enc.system_event(ts, 'Q');
    for (int s = 0; s < cfg_.symbols; ++s) enc.trading_action(locate(s), ++ts, symbol(s), 'T');

    const double weights[] = {cfg_.w_add, cfg_.w_delete, cfg_.w_replace, cfg_.w_cancel,
                              cfg_.w_execute, cfg_.w_execute_price, cfg_.w_trade};
    std::discrete_distribution<int> kind(std::begin(weights), std::end(weights));
    std::uniform_int_distribution<int> pick_sym(0, cfg_.symbols - 1);

    for (std::uint64_t n = 0; n < cfg_.messages; ++n) {
      ts += 1 + rng_() % 2000;
      const int s = pick_sym(rng_);
      Book& b = books_[static_cast<std::size_t>(s)];
      int k = kind(rng_);
      if (b.live.size() < 8 || (k != 0 && b.live.empty())) k = 0;
      if (k == 0 && static_cast<int>(b.live.size()) >= cfg_.max_live_per_symbol) k = 1;
      switch (k) {
        case 0: add(enc, s, ts); break;
        case 1: remove_random(enc, s, ts); break;
        case 2: replace_random(enc, s, ts); break;
        case 3: cancel_random(enc, s, ts); break;
        case 4: execute_best(enc, s, ts, false); break;
        case 5: execute_best(enc, s, ts, true); break;
        default: hidden_trade(enc, s, ts); break;
      }
    }
    enc.system_event(ts + 1, 'M');
    enc.system_event(ts + 2, 'E');
    enc.system_event(ts + 3, 'C');
    return enc.take();
  }

  static Locate locate(int s) noexcept { return static_cast<Locate>(s + 1); }
  static std::string symbol(int s) { return "SYM" + std::to_string(s); }

 private:
  struct Live {
    Side side;
    Price price;
    Qty qty;
  };
  struct Book {
    Price mid;
    std::map<Price, std::deque<OrderId>, std::greater<>> bids;
    std::map<Price, std::deque<OrderId>> asks;
    std::unordered_map<OrderId, Live> live;
    std::vector<OrderId> ids;  // for O(1) random choice (lazy deletion)
  };

  Qty random_qty() { return static_cast<Qty>(100 * (1 + rng_() % 10) + (rng_() % 4 == 0 ? rng_() % 100 : 0)); }

  Price random_price(Book& b, Side side) {
    // Geometric distance from the mid in 1-cent ticks (100 in Price(4) units).
    std::geometric_distribution<int> dist(0.25);
    const auto ticks = static_cast<Price>(dist(rng_));
    if (rng_() % 64 == 0) b.mid += (rng_() % 2 ? 100u : -100u);
    Price p = side == Side::Buy ? b.mid - 100 * (ticks + 1) : b.mid + 100 * (ticks + 1);
    // Never cross the opposite side.
    if (side == Side::Buy && !b.asks.empty() && p >= b.asks.begin()->first) p = b.asks.begin()->first - 100;
    if (side == Side::Sell && !b.bids.empty() && p <= b.bids.begin()->first) p = b.bids.begin()->first + 100;
    return p;
  }

  void insert(Book& b, OrderId id, Side side, Price p, Qty q) {
    if (side == Side::Buy) b.bids[p].push_back(id);
    else b.asks[p].push_back(id);
    b.live[id] = {side, p, q};
    b.ids.push_back(id);
  }

  void erase(Book& b, OrderId id) {
    const Live l = b.live.at(id);
    auto drop = [&](auto& levels) {
      auto it = levels.find(l.price);
      auto& q = it->second;
      for (auto qi = q.begin(); qi != q.end(); ++qi)
        if (*qi == id) {
          q.erase(qi);
          break;
        }
      if (q.empty()) levels.erase(it);
    };
    l.side == Side::Buy ? drop(b.bids) : drop(b.asks);
    b.live.erase(id);
  }

  OrderId pick_live(Book& b) {
    for (;;) {
      const std::size_t i = rng_() % b.ids.size();
      const OrderId id = b.ids[i];
      if (b.live.contains(id)) return id;
      b.ids[i] = b.ids.back();
      b.ids.pop_back();
    }
  }

  void add(Encoder& enc, int s, Timestamp ts) {
    Book& b = books_[static_cast<std::size_t>(s)];
    const Side side = rng_() % 2 ? Side::Buy : Side::Sell;
    const Price p = random_price(b, side);
    const Qty q = random_qty();
    const OrderId id = next_ref_++;
    if (rng_() % 10 == 0) enc.add_mpid(locate(s), ts, id, side, q, symbol(s), p, "MPID");
    else enc.add(locate(s), ts, id, side, q, symbol(s), p);
    insert(b, id, side, p, q);
  }

  void remove_random(Encoder& enc, int s, Timestamp ts) {
    Book& b = books_[static_cast<std::size_t>(s)];
    const OrderId id = pick_live(b);
    enc.del(locate(s), ts, id);
    erase(b, id);
  }

  void replace_random(Encoder& enc, int s, Timestamp ts) {
    Book& b = books_[static_cast<std::size_t>(s)];
    const OrderId id = pick_live(b);
    const Side side = b.live.at(id).side;
    erase(b, id);
    const Price p = random_price(b, side);
    const Qty q = random_qty();
    const OrderId nu = next_ref_++;
    enc.replace(locate(s), ts, id, nu, q, p);
    insert(b, nu, side, p, q);
  }

  void cancel_random(Encoder& enc, int s, Timestamp ts) {
    Book& b = books_[static_cast<std::size_t>(s)];
    const OrderId id = pick_live(b);
    Live& l = b.live.at(id);
    if (l.qty <= 1) {
      enc.del(locate(s), ts, id);
      erase(b, id);
      return;
    }
    const Qty c = 1 + static_cast<Qty>(rng_() % (l.qty - 1));
    enc.cancel(locate(s), ts, id, c);
    l.qty -= c;
  }

  void execute_best(Encoder& enc, int s, Timestamp ts, bool with_price) {
    Book& b = books_[static_cast<std::size_t>(s)];
    const bool buy_side = rng_() % 2;
    OrderId id;
    if (buy_side && !b.bids.empty()) id = b.bids.begin()->second.front();
    else if (!b.asks.empty()) id = b.asks.begin()->second.front();
    else if (!b.bids.empty()) id = b.bids.begin()->second.front();
    else return add(enc, s, ts);
    Live& l = b.live.at(id);
    const Qty q = rng_() % 2 ? l.qty : 1 + static_cast<Qty>(rng_() % l.qty);
    if (with_price) enc.executed_price(locate(s), ts, id, q, ++match_, true, l.price);
    else enc.executed(locate(s), ts, id, q, ++match_);
    if (q == l.qty) erase(b, id);
    else l.qty -= q;
  }

  void hidden_trade(Encoder& enc, int s, Timestamp ts) {
    Book& b = books_[static_cast<std::size_t>(s)];
    enc.trade(locate(s), ts, 0, rng_() % 2 ? Side::Buy : Side::Sell, random_qty(), symbol(s), b.mid, ++match_);
  }

  SyntheticFeedConfig cfg_;
  std::mt19937_64 rng_;
  std::vector<Book> books_;
  OrderId next_ref_ = 1;
  std::uint64_t match_ = 0;
};

inline std::vector<std::uint8_t> make_synthetic_feed(const SyntheticFeedConfig& cfg) {
  return SyntheticFeed(cfg).generate();
}

}  // namespace ib::itch
