#pragma once

// Price-time priority matching engine for synthetic orders.
//
// Reuses the optimized book core (pooled 32-byte orders, intrusive FIFO levels, sorted
// SoA price ladders, flat order-id index). Events are delivered to a statically bound
// Listener, so an engine with a NullListener compiles down to pure book manipulation.
//
// Listener hooks (all optional):
//   on_accepted(const NewOrder&)
//   on_rejected(OrderId, RejectReason)
//   on_fill(const Fill&)
//   on_cancelled(OrderId, Qty remaining, CancelReason)
//   on_rested(OrderId, Qty resting)

#include <memory>
#include <optional>
#include <vector>

#include "ironbook/book/flat_hash_map.hpp"
#include "ironbook/book/order_book.hpp"
#include "ironbook/engine/engine_types.hpp"

namespace ib::engine {

struct NullListener {};

struct EngineConfig {
  InstrumentId instruments = 1;
  std::uint32_t max_orders = 1u << 22;
  std::uint32_t max_levels = 1u << 20;
  std::size_t expected_live_orders = 1u << 20;
  StpMode stp = StpMode::None;
  bool prefault = true;
};

struct EngineStats {
  std::uint64_t orders = 0, fills = 0, cancels = 0, rejects = 0, rested = 0;
  std::uint64_t volume = 0;
};

// Client order ids carry no ordering guarantee, so the index uses a full-avalanche hash
// (see the identity-hash clustering pathology in docs/OPTIMIZATION_LOG.md).
template <class Listener = NullListener, class Hash = Murmur3Mix>
class MatchingEngine {
 public:
  explicit MatchingEngine(const EngineConfig& cfg = {}, Listener* listener = nullptr)
      : cfg_(cfg),
        pools_(std::make_unique<BookPools>(cfg.max_orders, cfg.max_levels, cfg.prefault)),
        index_(cfg.expected_live_orders),
        books_(cfg.instruments),
        listener_(listener) {
    for (auto& b : books_) b.attach(pools_.get());
  }

  // Submits a new order. Returns false if it was rejected.
  bool submit(const NewOrder& o) {
    ++stats_.orders;
    if (IB_UNLIKELY(o.instrument >= books_.size())) return reject(o.id, RejectReason::UnknownInstrument);
    if (IB_UNLIKELY(o.qty == 0)) return reject(o.id, RejectReason::InvalidQuantity);
    if (IB_UNLIKELY(o.id == 0 || index_.find(o.id) != kNil))
      return reject(o.id, RejectReason::DuplicateOrderId);
    if (IB_UNLIKELY(o.type != OrderType::Market && o.price == 0))
      return reject(o.id, RejectReason::InvalidPrice);

    OrderBook& book = books_[o.instrument];
    const Price limit = o.type == OrderType::Market ? (o.side == Side::Buy ? kMaxPrice : 0) : o.price;

    if (o.type == OrderType::PostOnly && would_cross(book, o.side, limit))
      return reject(o.id, RejectReason::PostOnlyWouldCross);
    if (o.type == OrderType::FOK && available(book, o.side, limit, o.qty, o.owner) < o.qty)
      return reject(o.id, RejectReason::FokInsufficientLiquidity);

    notify_accepted(o);
    bool stp_cancelled = false;
    const Qty remaining = match(book, o, limit, stp_cancelled);

    if (remaining == 0) return true;
    if (o.type == OrderType::Limit || o.type == OrderType::PostOnly) {
      if (!stp_cancelled) {
        rest(book, o, remaining);
        return true;
      }
    }
    notify_cancelled(o.id, remaining,
                     stp_cancelled ? CancelReason::SelfTradePrevention : CancelReason::ImmediateOrCancel);
    return true;
  }

  bool cancel(OrderId id) {
    const std::uint32_t oi = index_.erase(id);
    if (oi == kNil) {
      reject(id, RejectReason::UnknownOrder);
      return false;
    }
    Order& o = pools_->orders[oi];
    const Qty qty = o.qty;
    books_[o.locate].unlink(oi);
    pools_->orders.free(oi);
    notify_cancelled(id, qty, CancelReason::UserRequested);
    return true;
  }

  // Amend a resting order. Reducing quantity at the same price keeps time priority;
  // any price change or quantity increase is a cancel/replace that loses priority and
  // may trade immediately if the new price crosses.
  bool modify(OrderId id, Price new_price, Qty new_qty) {
    const std::uint32_t oi = index_.find(id);
    if (oi == kNil) {
      reject(id, RejectReason::UnknownOrder);
      return false;
    }
    Order& o = pools_->orders[oi];
    const Price cur_price = pools_->levels[o.level].price;
    if (new_qty == 0) return cancel(id);
    if (new_price == cur_price && new_qty <= o.qty) {
      if (new_qty < o.qty) books_[o.locate].reduce(oi, o.qty - new_qty);
      return true;
    }
    const NewOrder replacement{id, o.owner, o.locate, o.side, OrderType::Limit, new_price, new_qty};
    index_.erase(id);
    books_[o.locate].unlink(oi);
    pools_->orders.free(oi);
    notify_cancelled(id, 0, CancelReason::Replaced);
    return submit(replacement);
  }

  std::optional<LevelView> best(InstrumentId inst, Side s) const { return books_[inst].best(s); }
  void depth(InstrumentId inst, Side s, std::size_t n, std::vector<LevelView>& out) const {
    books_[inst].depth(s, n, out);
  }
  std::optional<Qty> order_qty(OrderId id) const {
    const std::uint32_t oi = index_.find(id);
    if (oi == kNil) return std::nullopt;
    return pools_->orders[oi].qty;
  }
  std::size_t live_orders() const noexcept { return index_.size(); }
  const EngineStats& stats() const noexcept { return stats_; }
  const OrderBook& book(InstrumentId inst) const noexcept { return books_[inst]; }
  bool check_invariants() const {
    for (const auto& b : books_)
      if (!b.check_invariants()) return false;
    return true;
  }

 private:
  static bool would_cross(const OrderBook& book, Side side, Price limit) {
    const auto opp = book.best(opposite(side));
    return opp && crosses(side, limit, opp->price);
  }

  // Shares that an incoming order could take immediately (excluding self-trades that
  // STP would cancel).
  Qty available(const OrderBook& book, Side side, Price limit, Qty want, OwnerId owner) const {
    const PriceLadder& lad = book.ladder(opposite(side));
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < lad.size() && total < want; ++i) {
      const Level& l = pools_->levels[lad.from_top(i)];
      if (!crosses(side, limit, l.price)) break;
      if (cfg_.stp == StpMode::None || owner == 0) {
        total += l.total;
      } else {
        for (std::uint32_t oi = l.head; oi != kNil && total < want; oi = pools_->orders[oi].next) {
          const Order& m = pools_->orders[oi];
          if (m.owner == owner) {
            if (cfg_.stp == StpMode::CancelIncoming) return static_cast<Qty>(total);
            continue;
          }
          total += m.qty;
        }
      }
    }
    return total > want ? want : static_cast<Qty>(total);
  }

  // Matches the incoming order against the opposite side; returns the unfilled qty.
  IB_ALWAYS_INLINE Qty match(OrderBook& book, const NewOrder& o, Price limit, bool& stp_cancelled) {
    Qty qty = o.qty;
    PriceLadder& opp = book.ladder(opposite(o.side));
    while (qty > 0) {
      const std::uint32_t li = opp.best();
      if (li == kNil) break;
      Level& lvl = pools_->levels[li];
      if (!crosses(o.side, limit, lvl.price)) break;
      const std::uint32_t mi = lvl.head;
      Order& maker = pools_->orders[mi];

      if (IB_UNLIKELY(cfg_.stp != StpMode::None && o.owner != 0 && maker.owner == o.owner)) {
        if (cfg_.stp == StpMode::CancelIncoming) {
          stp_cancelled = true;
          break;
        }
        const OrderId mid = maker.id;
        const Qty mq = maker.qty;
        index_.erase(mid);
        book.unlink(mi);
        pools_->orders.free(mi);
        notify_cancelled(mid, mq, CancelReason::SelfTradePrevention);
        continue;
      }

      const Qty fill = qty < maker.qty ? qty : maker.qty;
      qty -= fill;
      const Price px = lvl.price;
      const OrderId maker_id = maker.id;
      const OwnerId maker_owner = maker.owner;
      const Qty maker_left = maker.qty - fill;
      if (maker_left == 0) {
        index_.erase(maker_id);
        book.unlink(mi);  // may free lvl
        pools_->orders.free(mi);
      } else {
        book.reduce(mi, fill);
      }
      ++stats_.fills;
      stats_.volume += fill;
      if constexpr (requires(Listener& l, const Fill& f) { l.on_fill(f); }) {
        if (listener_)
          listener_->on_fill(Fill{++match_id_, o.instrument, o.id, maker_id, o.owner, maker_owner,
                                  o.side, px, fill, maker_left, qty});
      }
    }
    return qty;
  }

  void rest(OrderBook& book, const NewOrder& o, Qty qty) {
    const std::uint32_t oi = pools_->orders.alloc();
    Order& r = pools_->orders[oi];
    r.id = o.id;
    r.qty = qty;
    r.owner = o.owner;
    r.locate = o.instrument;
    r.side = o.side;
    r.flags = 0;
    index_.insert(o.id, oi);
    book.link(oi, o.price);
    ++stats_.rested;
    if constexpr (requires(Listener& l) { l.on_rested(o.id, qty); }) {
      if (listener_) listener_->on_rested(o.id, qty);
    }
  }

  bool reject(OrderId id, RejectReason r) {
    ++stats_.rejects;
    if constexpr (requires(Listener& l) { l.on_rejected(id, r); }) {
      if (listener_) listener_->on_rejected(id, r);
    }
    return false;
  }
  void notify_accepted(const NewOrder& o) {
    if constexpr (requires(Listener& l) { l.on_accepted(o); }) {
      if (listener_) listener_->on_accepted(o);
    }
  }
  void notify_cancelled(OrderId id, Qty qty, CancelReason r) {
    ++stats_.cancels;
    if constexpr (requires(Listener& l) { l.on_cancelled(id, qty, r); }) {
      if (listener_) listener_->on_cancelled(id, qty, r);
    }
  }

  EngineConfig cfg_;
  std::unique_ptr<BookPools> pools_;
  FlatHashMap<Hash> index_;
  std::vector<OrderBook> books_;
  Listener* listener_;
  EngineStats stats_;
  std::uint64_t match_id_ = 0;
};

}  // namespace ib::engine
