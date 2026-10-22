#pragma once

// Baseline multi-symbol order book built from standard containers:
//   std::map<Price, Level>  per side   (sorted levels, O(log L) lookup)
//   std::list<Order>        per level  (FIFO queue, one heap node per order)
//   std::unordered_map      order id -> (book, side, level, list iterator)
//
// Deliberately straightforward: it is the correctness oracle that the optimized
// Market is differentially tested against, and the "before" column of the
// optimization log.

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ironbook/book/book_types.hpp"

namespace ib {

class NaiveMarket {
 public:
  NaiveMarket() : books_(kMaxLocates) {}

  bool add(Locate loc, OrderId id, Side side, Price price, Qty qty) {
    if (index_.contains(id)) return false;
    Book& b = book(loc);
    Level& lvl = side == Side::Buy ? b.bids[price] : b.asks[price];
    lvl.orders.push_back({id, qty});
    lvl.total += qty;
    index_.emplace(id, Ref{loc, side, price, std::prev(lvl.orders.end())});
    return true;
  }

  OpStatus execute(OrderId id, Qty qty) { return reduce(id, qty); }
  OpStatus cancel(OrderId id, Qty qty) { return reduce(id, qty); }

  OpStatus remove(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return OpStatus::UnknownOrder;
    erase(it);
    return OpStatus::Ok;
  }

  OpStatus replace(OrderId orig, OrderId nu, Qty qty, Price price) {
    auto it = index_.find(orig);
    if (it == index_.end()) return OpStatus::UnknownOrder;
    const Locate loc = it->second.locate;
    const Side side = it->second.side;
    erase(it);
    return add(loc, nu, side, price, qty) ? OpStatus::Ok : OpStatus::DuplicateId;
  }

  std::optional<OrderView> find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const Ref& r = it->second;
    const Book& b = *books_[r.locate];
    OrderView v{r.locate, r.side, r.price, r.it->qty, false, false};
    if (r.side == Side::Buy) {
      const Level& lvl = b.bids.at(r.price);
      v.at_level_head = lvl.orders.front().id == id;
      v.at_best_level = b.bids.begin()->first == r.price;
    } else {
      const Level& lvl = b.asks.at(r.price);
      v.at_level_head = lvl.orders.front().id == id;
      v.at_best_level = b.asks.begin()->first == r.price;
    }
    return v;
  }

  std::optional<LevelView> best(Locate loc, Side side) const {
    const Book* b = books_[loc].get();
    if (!b) return std::nullopt;
    if (side == Side::Buy) {
      if (b->bids.empty()) return std::nullopt;
      return view(*b->bids.begin());
    }
    if (b->asks.empty()) return std::nullopt;
    return view(*b->asks.begin());
  }

  void depth(Locate loc, Side side, std::size_t n, std::vector<LevelView>& out) const {
    out.clear();
    const Book* b = books_[loc].get();
    if (!b) return;
    auto collect = [&](const auto& levels) {
      for (const auto& kv : levels) {
        if (out.size() == n) break;
        out.push_back(view(kv));
      }
    };
    side == Side::Buy ? collect(b->bids) : collect(b->asks);
  }

  // Order-sensitive digest of a whole book: every level and every order id in FIFO order.
  std::uint64_t digest(Locate loc) const {
    Digest d;
    static const Book kEmpty{};
    const Book* b = books_[loc] ? books_[loc].get() : &kEmpty;  // absent == empty
    auto walk = [&](const auto& levels) {
      for (const auto& [px, lvl] : levels) {
        d.mix(px);
        d.mix(lvl.total);
        for (const Order& o : lvl.orders) {
          d.mix(o.id);
          d.mix(o.qty);
        }
      }
      d.mix(0xFFFF'FFFF'FFFF'FFFFull);
    };
    walk(b->bids);
    walk(b->asks);
    return d.h;
  }

  std::size_t order_count() const noexcept { return index_.size(); }

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  struct Level {
    std::list<Order> orders;
    std::uint64_t total = 0;
  };
  using Bids = std::map<Price, Level, std::greater<>>;
  using Asks = std::map<Price, Level, std::less<>>;
  struct Book {
    Bids bids;
    Asks asks;
  };
  struct Ref {
    Locate locate;
    Side side;
    Price price;
    std::list<Order>::iterator it;
  };
  using Index = std::unordered_map<OrderId, Ref>;

  template <class KV>
  static LevelView view(const KV& kv) {
    return {kv.first, kv.second.total, static_cast<std::uint32_t>(kv.second.orders.size())};
  }

  Book& book(Locate loc) {
    auto& p = books_[loc];
    if (!p) p = std::make_unique<Book>();
    return *p;
  }

  template <class Levels>
  static void erase_from(Levels& levels, const Ref& r) {
    auto lit = levels.find(r.price);
    lit->second.total -= r.it->qty;
    lit->second.orders.erase(r.it);
    if (lit->second.orders.empty()) levels.erase(lit);
  }

  void erase(Index::iterator it) {
    Book& b = *books_[it->second.locate];
    if (it->second.side == Side::Buy) erase_from(b.bids, it->second);
    else erase_from(b.asks, it->second);
    index_.erase(it);
  }

  OpStatus reduce(OrderId id, Qty qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return OpStatus::UnknownOrder;
    Ref& r = it->second;
    if (qty >= r.it->qty) {
      const bool over = qty > r.it->qty;
      erase(it);
      return over ? OpStatus::Overfill : OpStatus::Ok;
    }
    Book& b = *books_[r.locate];
    r.it->qty -= qty;
    if (r.side == Side::Buy) b.bids.find(r.price)->second.total -= qty;
    else b.asks.find(r.price)->second.total -= qty;
    return OpStatus::Ok;
  }

  std::vector<std::unique_ptr<Book>> books_;
  Index index_;
};

}  // namespace ib
