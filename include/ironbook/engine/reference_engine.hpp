#pragma once

// Reference matching engine: the same semantics as MatchingEngine implemented as
// directly as possible with std::map + std::list. Used as the oracle in randomized
// differential tests (identical inputs must produce identical event streams) and as
// the baseline in matching benchmarks.

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ironbook/book/book_types.hpp"
#include "ironbook/engine/engine_types.hpp"

namespace ib::engine {

template <class Listener>
class ReferenceEngine {
 public:
  ReferenceEngine(InstrumentId instruments, StpMode stp, Listener* l)
      : books_(instruments), stp_(stp), l_(l) {}

  bool submit(const NewOrder& o) {
    if (o.instrument >= books_.size()) return reject(o.id, RejectReason::UnknownInstrument);
    if (o.qty == 0) return reject(o.id, RejectReason::InvalidQuantity);
    if (o.id == 0 || index_.contains(o.id)) return reject(o.id, RejectReason::DuplicateOrderId);
    if (o.type != OrderType::Market && o.price == 0) return reject(o.id, RejectReason::InvalidPrice);
    Book& b = books_[o.instrument];
    const Price limit = o.type == OrderType::Market ? (o.side == Side::Buy ? kMaxPrice : 0) : o.price;

    if (o.type == OrderType::PostOnly) {
      if (auto bp = best_price(b, opposite(o.side)); bp && crosses(o.side, limit, *bp))
        return reject(o.id, RejectReason::PostOnlyWouldCross);
    }
    if (o.type == OrderType::FOK && available(b, o, limit) < o.qty)
      return reject(o.id, RejectReason::FokInsufficientLiquidity);

    if constexpr (requires { l_->on_accepted(o); }) l_->on_accepted(o);
    Qty qty = o.qty;
    bool stp_cancel = false;
    if (o.side == Side::Buy) qty = sweep(b.asks, b, o, limit, qty, stp_cancel);
    else qty = sweep(b.bids, b, o, limit, qty, stp_cancel);

    if (qty == 0) return true;
    if ((o.type == OrderType::Limit || o.type == OrderType::PostOnly) && !stp_cancel) {
      auto& q = o.side == Side::Buy ? b.bids[o.price] : b.asks[o.price];
      q.push_back({o.id, o.owner, qty});
      index_[o.id] = {o.instrument, o.side, o.price, std::prev(q.end())};
      if constexpr (requires { l_->on_rested(o.id, qty); }) l_->on_rested(o.id, qty);
      return true;
    }
    cancelled(o.id, qty, stp_cancel ? CancelReason::SelfTradePrevention : CancelReason::ImmediateOrCancel);
    return true;
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return reject(id, RejectReason::UnknownOrder);
    const Qty q = erase(it->second, id);
    index_.erase(it);
    cancelled(id, q, CancelReason::UserRequested);
    return true;
  }

  bool modify(OrderId id, Price price, Qty qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return reject(id, RejectReason::UnknownOrder);
    if (qty == 0) return cancel(id);
    Loc loc = it->second;
    Resting& r = *loc.it;
    if (price == loc.price && qty <= r.qty) {
      r.qty = qty;
      return true;
    }
    const OwnerId owner = r.owner;
    erase(loc, id);
    index_.erase(it);
    cancelled(id, 0, CancelReason::Replaced);
    return submit({id, owner, loc.instrument, loc.side, OrderType::Limit, price, qty});
  }

  std::optional<LevelView> best(InstrumentId inst, Side s) const {
    const Book& b = books_[inst];
    if (s == Side::Buy) {
      if (b.bids.empty()) return std::nullopt;
      return view(*b.bids.begin());
    }
    if (b.asks.empty()) return std::nullopt;
    return view(*b.asks.begin());
  }

  void depth(InstrumentId inst, Side s, std::size_t n, std::vector<LevelView>& out) const {
    out.clear();
    const Book& b = books_[inst];
    auto collect = [&](const auto& side) {
      for (const auto& kv : side) {
        if (out.size() == n) break;
        out.push_back(view(kv));
      }
    };
    s == Side::Buy ? collect(b.bids) : collect(b.asks);
  }

  std::size_t live_orders() const noexcept { return index_.size(); }

 private:
  struct Resting {
    OrderId id;
    OwnerId owner;
    Qty qty;
  };
  using Queue = std::list<Resting>;
  struct Book {
    std::map<Price, Queue, std::greater<>> bids;
    std::map<Price, Queue, std::less<>> asks;
  };
  struct Loc {
    InstrumentId instrument;
    Side side;
    Price price;
    typename Queue::iterator it;  // O(1) cancel/amend, as any sane std::list book does
  };

  template <class KV>
  static LevelView view(const KV& kv) {
    std::uint64_t t = 0;
    for (const auto& r : kv.second) t += r.qty;
    return {kv.first, t, static_cast<std::uint32_t>(kv.second.size())};
  }

  static std::optional<Price> best_price(const Book& b, Side s) {
    if (s == Side::Buy) return b.bids.empty() ? std::nullopt : std::optional<Price>(b.bids.begin()->first);
    return b.asks.empty() ? std::nullopt : std::optional<Price>(b.asks.begin()->first);
  }

  Qty available(const Book& b, const NewOrder& o, Price limit) const {
    std::uint64_t total = 0;
    auto scan = [&](const auto& side) {
      for (const auto& [px, q] : side) {
        if (!crosses(o.side, limit, px)) return;
        for (const Resting& r : q) {
          if (stp_ != StpMode::None && o.owner != 0 && r.owner == o.owner) {
            if (stp_ == StpMode::CancelIncoming) return;
            continue;
          }
          total += r.qty;
          if (total >= o.qty) return;
        }
      }
    };
    o.side == Side::Buy ? scan(b.asks) : scan(b.bids);
    return total >= o.qty ? o.qty : static_cast<Qty>(total);
  }

  template <class Side_>
  Qty sweep(Side_& side, Book&, const NewOrder& o, Price limit, Qty qty, bool& stp_cancel) {
    while (qty > 0 && !side.empty()) {
      auto lvl = side.begin();
      if (!crosses(o.side, limit, lvl->first)) break;
      Queue& q = lvl->second;
      Resting& m = q.front();
      if (stp_ != StpMode::None && o.owner != 0 && m.owner == o.owner) {
        if (stp_ == StpMode::CancelIncoming) {
          stp_cancel = true;
          break;
        }
        const OrderId mid = m.id;
        const Qty mq = m.qty;
        index_.erase(mid);
        q.pop_front();
        if (q.empty()) side.erase(lvl);
        cancelled(mid, mq, CancelReason::SelfTradePrevention);
        continue;
      }
      const Qty fill = std::min(qty, m.qty);
      qty -= fill;
      m.qty -= fill;
      const Fill f{++match_id_, o.instrument, o.id, m.id, o.owner, m.owner, o.side, lvl->first, fill,
                   m.qty, qty};
      if (m.qty == 0) {
        index_.erase(m.id);
        q.pop_front();
        if (q.empty()) side.erase(lvl);
      }
      if constexpr (requires { l_->on_fill(f); }) l_->on_fill(f);
    }
    return qty;
  }

  Qty erase(const Loc& loc, OrderId) {
    Book& b = books_[loc.instrument];
    auto drop = [&](auto& side) -> Qty {
      auto lvl = side.find(loc.price);
      const Qty qty = loc.it->qty;
      lvl->second.erase(loc.it);
      if (lvl->second.empty()) side.erase(lvl);
      return qty;
    };
    return loc.side == Side::Buy ? drop(b.bids) : drop(b.asks);
  }

  bool reject(OrderId id, RejectReason r) {
    if constexpr (requires { l_->on_rejected(id, r); }) l_->on_rejected(id, r);
    return false;
  }
  void cancelled(OrderId id, Qty q, CancelReason r) {
    if constexpr (requires { l_->on_cancelled(id, q, r); }) l_->on_cancelled(id, q, r);
  }

  std::vector<Book> books_;
  std::unordered_map<OrderId, Loc> index_;
  StpMode stp_;
  Listener* l_;
  std::uint64_t match_id_ = 0;
};

}  // namespace ib::engine
