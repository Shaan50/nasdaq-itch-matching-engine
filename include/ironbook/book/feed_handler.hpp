#pragma once

// Translates ITCH order-book messages into Market operations.
//
// MarketT is any type with the add/execute/cancel/remove/replace/find API (Market or
// NaiveMarket). Observer hooks are bound statically and every hook is optional, so a
// plain replay pays nothing for validation support.

#include <cstdint>

#include "ironbook/book/book_types.hpp"
#include "ironbook/common/compiler.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib {

struct FeedStats {
  std::uint64_t adds = 0, executes = 0, executes_with_price = 0, cancels = 0, deletes = 0,
                replaces = 0, trades = 0, crosses = 0;
  std::uint64_t unknown_order = 0;  // E/C/X/D/U referencing an order we do not hold
  std::uint64_t duplicate_add = 0;
  std::uint64_t overfill = 0;       // executed/cancelled more than the resting qty

  std::uint64_t book_messages() const noexcept {
    return adds + executes + executes_with_price + cancels + deletes + replaces;
  }
  std::uint64_t errors() const noexcept { return unknown_order + duplicate_add + overfill; }
};

struct NullObserver {};

template <class MarketT, class Observer = NullObserver>
class FeedHandler {
 public:
  explicit FeedHandler(MarketT& m) : market_(m), obs_(null_) {}
  FeedHandler(MarketT& m, Observer& obs) : market_(m), obs_(obs) {}

  void on(const itch::AddOrder& m) {
    ++stats_.adds;
    if (!market_.add(m.h.locate, m.ref, m.side, m.price, m.shares)) ++stats_.duplicate_add;
    if constexpr (requires { obs_.after_add(m, market_); }) obs_.after_add(m, market_);
  }

  void on(const itch::OrderExecuted& m) {
    ++stats_.executes;
    if constexpr (requires { obs_.before_execute(m, market_); }) obs_.before_execute(m, market_);
    count(market_.execute(m.ref, m.shares));
  }

  void on(const itch::OrderExecutedWithPrice& m) {
    ++stats_.executes_with_price;
    if constexpr (requires { obs_.before_execute(m, market_); }) obs_.before_execute(m, market_);
    count(market_.execute(m.ref, m.shares));
  }

  void on(const itch::OrderCancel& m) {
    ++stats_.cancels;
    count(market_.cancel(m.ref, m.shares));
  }

  void on(const itch::OrderDelete& m) {
    ++stats_.deletes;
    count(market_.remove(m.ref));
  }

  void on(const itch::OrderReplace& m) {
    ++stats_.replaces;
    count(market_.replace(m.orig_ref, m.new_ref, m.shares, m.price));
    if constexpr (requires { obs_.after_replace(m, market_); }) obs_.after_replace(m, market_);
  }

  void on(const itch::Trade& m) {
    ++stats_.trades;
    if constexpr (requires { obs_.on(m); }) obs_.on(m);
  }
  void on(const itch::CrossTrade& m) {
    ++stats_.crosses;
    if constexpr (requires { obs_.on(m); }) obs_.on(m);
  }
  void on(const itch::SystemEvent& m) {
    if constexpr (requires { obs_.on(m); }) obs_.on(m);
  }
  void on(const itch::StockDirectory& m) {
    if constexpr (requires { obs_.on(m); }) obs_.on(m);
  }
  void on(const itch::StockTradingAction& m) {
    if constexpr (requires { obs_.on(m); }) obs_.on(m);
  }

  const FeedStats& stats() const noexcept { return stats_; }
  MarketT& market() noexcept { return market_; }

 private:
  IB_ALWAYS_INLINE void count(OpStatus s) noexcept {
    if (IB_LIKELY(s == OpStatus::Ok)) return;
    if (s == OpStatus::UnknownOrder) ++stats_.unknown_order;
    else if (s == OpStatus::Overfill) ++stats_.overfill;
    else if (s == OpStatus::DuplicateId) ++stats_.duplicate_add;
  }

  MarketT& market_;
  [[no_unique_address]] Observer null_{};
  Observer& obs_;
  FeedStats stats_;
};

}  // namespace ib
