#pragma once

// Feed-consistency validation for reconstructed books.
//
// ITCH never states the book explicitly; it only reports deltas. These checks use the
// feed's own execution messages as ground truth for what the book must look like:
//
//  1. Referential integrity: every E/C/X/D/U refers to an order we hold, on the same
//     stock locate, and never executes/cancels more shares than are resting.
//  2. Price priority: a displayed order executed at its limit price ('E') must sit at
//     the best price level on its side (otherwise a better-priced order was skipped).
//  3. Time priority: within that level it should be first in the FIFO queue.
//  4. No crossed book: during continuous trading for a symbol in 'T' (trading) state,
//     best bid < best ask after every add/replace.
//
// Executions during auction crosses ('C' with printable = N) legitimately violate 2/3
// and are tracked separately.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "ironbook/book/book_types.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib {

struct SymbolTape {
  std::uint64_t volume = 0;
  std::uint64_t trades = 0;
  double notional = 0;  // dollars
  Price high = 0, low = kMaxPrice, last = 0;

  void print(Qty qty, Price px) noexcept {
    volume += qty;
    ++trades;
    notional += static_cast<double>(qty) * to_double(px);
    if (px > high) high = px;
    if (px < low) low = px;
    last = px;
  }
  void cross(std::uint64_t qty, Price px) noexcept {
    if (qty == 0) return;
    volume += qty;
    ++trades;
    notional += static_cast<double>(qty) * to_double(px);
    last = px;
  }
  double vwap() const noexcept { return volume ? notional / static_cast<double>(volume) : 0.0; }
};

struct ValidationReport {
  std::uint64_t executions = 0;            // E + C seen with a known order
  std::uint64_t exec_unknown = 0;
  std::uint64_t locate_mismatch = 0;
  std::uint64_t e_checked = 0;             // continuous-market 'E' executions
  std::uint64_t e_at_best = 0;
  std::uint64_t e_at_head = 0;
  // Priority diagnostics: how many checked executions hit an order created by an Order
  // Replace ('U'), overall and among the non-FIFO-head executions.
  std::uint64_t e_from_replace = 0;
  std::uint64_t e_not_head_from_replace = 0;
  std::uint64_t e_not_head_ahead1 = 0;          // exactly one order skipped
  std::uint64_t e_not_head_skipped_mpid = 0;    // skipped head carried an MPID (F message)
  std::uint64_t e_not_head_skipped_oddlot = 0;  // skipped head < 100 shares
  std::uint64_t e_mpid_heads = 0;               // base rate: head of the level is attributed
  std::uint64_t e_oddlot_heads = 0;             // base rate: head of the level < 100 shares
  std::uint64_t c_printable = 0, c_nonprintable = 0, c_price_improved = 0;
  std::uint64_t cross_checks = 0;
  std::uint64_t crossed = 0;
  std::uint64_t locked = 0;
};

class FeedValidator {
 public:
  FeedValidator() : tape_(kMaxLocates), state_(kMaxLocates, ' ') {}

  template <class M>
  void before_execute(const itch::OrderExecuted& m, M& market) {
    const auto v = market.find(m.ref);
    if (!v) {
      ++r_.exec_unknown;
      return;
    }
    ++r_.executions;
    if (v->locate != m.h.locate) ++r_.locate_mismatch;
    tape_[m.h.locate].print(m.shares, v->price);
    if (in_continuous(m.h.locate)) {
      ++r_.e_checked;
      r_.e_at_best += v->at_best_level;
      r_.e_at_head += v->at_level_head;
      const bool replaced = from_replace(m.ref);
      r_.e_from_replace += replaced;
      if (!v->at_level_head) r_.e_not_head_from_replace += replaced;
      if constexpr (requires { market.queue_info(m.ref); }) {
        if (const auto q = market.queue_info(m.ref)) {
          r_.e_mpid_heads += test(mpid_, q->head);
          r_.e_oddlot_heads += q->head_qty < 100;
          if (!v->at_level_head) {
            r_.e_not_head_ahead1 += q->ahead == 1;
            r_.e_not_head_skipped_mpid += test(mpid_, q->head);
            r_.e_not_head_skipped_oddlot += q->head_qty < 100;
          }
        }
      }
    }
  }

  template <class M>
  void before_execute(const itch::OrderExecutedWithPrice& m, M& market) {
    const auto v = market.find(m.ref);
    if (!v) {
      ++r_.exec_unknown;
      return;
    }
    ++r_.executions;
    if (v->locate != m.h.locate) ++r_.locate_mismatch;
    if (m.printable) {
      ++r_.c_printable;
      tape_[m.h.locate].print(m.shares, m.price);
    } else {
      ++r_.c_nonprintable;  // part of a cross; volume is reported by the 'Q' message
    }
    if (m.price != v->price) ++r_.c_price_improved;
  }

  template <class M>
  void after_add(const itch::AddOrder& m, M& market) {
    if (m.has_mpid) set(mpid_, m.ref);
    check_cross(m.h.locate, market);
  }
  template <class M>
  void after_replace(const itch::OrderReplace& m, M& market) {
    mark_replace(m.new_ref);
    check_cross(m.h.locate, market);
  }

  void on(const itch::Trade& m) { tape_[m.h.locate].print(m.shares, m.price); }
  void on(const itch::CrossTrade& m) { tape_[m.h.locate].cross(m.shares, m.price); }
  void on(const itch::SystemEvent& m) {
    if (m.event_code == 'Q') market_open_ = true;
    if (m.event_code == 'M') market_open_ = false;
  }
  void on(const itch::StockTradingAction& m) { state_[m.h.locate] = m.trading_state; }

  const ValidationReport& report() const noexcept { return r_; }
  const std::vector<SymbolTape>& tape() const noexcept { return tape_; }

 private:
  bool in_continuous(Locate loc) const noexcept { return market_open_ && state_[loc] == 'T'; }

  // Order refs are dense per day (~3e8), so a bitmap is far smaller than a hash set.
  static void set(std::vector<std::uint64_t>& bits, OrderId ref) {
    const std::size_t w = static_cast<std::size_t>(ref >> 6);
    if (w >= bits.size()) bits.resize(std::max(w + 1, bits.size() * 2), 0);
    bits[w] |= std::uint64_t{1} << (ref & 63);
  }
  static bool test(const std::vector<std::uint64_t>& bits, OrderId ref) noexcept {
    const std::size_t w = static_cast<std::size_t>(ref >> 6);
    return w < bits.size() && ((bits[w] >> (ref & 63)) & 1);
  }
  void mark_replace(OrderId ref) { set(replaced_, ref); }
  bool from_replace(OrderId ref) const noexcept { return test(replaced_, ref); }


  template <class M>
  void check_cross(Locate loc, M& market) {
    if (!in_continuous(loc)) return;
    ++r_.cross_checks;
    const auto bid = market.best(loc, Side::Buy);
    const auto ask = market.best(loc, Side::Sell);
    if (bid && ask) {
      if (bid->price > ask->price) ++r_.crossed;
      else if (bid->price == ask->price) ++r_.locked;
    }
  }

  ValidationReport r_;
  std::vector<SymbolTape> tape_;
  std::vector<char> state_;
  std::vector<std::uint64_t> replaced_;
  std::vector<std::uint64_t> mpid_;
  bool market_open_ = false;
};

}  // namespace ib
