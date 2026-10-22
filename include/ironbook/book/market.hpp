#pragma once

// Optimized multi-symbol book reconstruction for the ITCH feed.
//
// One global order arena + one global order-id index (ITCH order reference numbers are
// unique across the whole day, not per symbol), and books indexed directly by the
// 16-bit stock locate code, so routing a message is a single array index.

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ironbook/book/book_types.hpp"
#include "ironbook/book/flat_hash_map.hpp"
#include "ironbook/book/order_book.hpp"

namespace ib {

struct MarketConfig {
  std::uint32_t max_orders = 1u << 25;      // arena reservation (virtual; committed lazily)
  std::uint32_t max_levels = 1u << 23;
  // Index capacity = bit_ceil(2x) slots. Identity hashing needs a low load factor
  // (~0.1 at the ~1.9M peak live orders of a NASDAQ day) to keep its locality benefit;
  // see docs/OPTIMIZATION_LOG.md.
  std::size_t expected_live_orders = 1u << 23;
  bool prefault = false;                    // commit pool pages up front
};

// Adapter giving std::unordered_map the FlatHashMap interface (optimization-log baseline).
class StdIndex {
 public:
  static constexpr std::uint32_t kMissing = 0;
  explicit StdIndex(std::size_t expected) { map_.reserve(expected); }
  std::uint32_t find(std::uint64_t k) const {
    auto it = map_.find(k);
    return it == map_.end() ? kMissing : it->second;
  }
  bool insert(std::uint64_t k, std::uint32_t v) { return map_.emplace(k, v).second; }
  std::uint32_t erase(std::uint64_t k) {
    auto it = map_.find(k);
    if (it == map_.end()) return kMissing;
    const std::uint32_t v = it->second;
    map_.erase(it);
    return v;
  }
  void prefetch(std::uint64_t) const noexcept {}
  std::size_t size() const noexcept { return map_.size(); }
  std::size_t capacity() const noexcept { return map_.bucket_count(); }
  std::size_t rehashes() const noexcept { return 0; }
  bool lock() const { return true; }  // node-based: nothing contiguous to wire

 private:
  std::unordered_map<std::uint64_t, std::uint32_t> map_;
};

template <class Index = FlatHashMap<IdentityHash>>
class BasicMarket {
 public:
  explicit BasicMarket(const MarketConfig& cfg = {})
      : pools_(std::make_unique<BookPools>(cfg.max_orders, cfg.max_levels, cfg.prefault)),
        index_(cfg.expected_live_orders),
        books_(kMaxLocates) {
    for (auto& b : books_) b.attach(pools_.get());
  }

  IB_ALWAYS_INLINE bool add(Locate loc, OrderId id, Side side, Price price, Qty qty,
                            std::uint32_t owner = 0) {
    const std::uint32_t oi = pools_->orders.alloc();
    if (IB_UNLIKELY(!index_.insert(id, oi))) {
      pools_->orders.free(oi);
      return false;
    }
    Order& o = pools_->orders[oi];
    o.id = id;
    o.qty = qty;
    o.owner = owner;
    o.locate = loc;
    o.side = side;
    o.flags = 0;
    books_[loc].link(oi, price);
    return true;
  }

  IB_ALWAYS_INLINE OpStatus execute(OrderId id, Qty qty) { return reduce(id, qty); }
  IB_ALWAYS_INLINE OpStatus cancel(OrderId id, Qty qty) { return reduce(id, qty); }

  IB_ALWAYS_INLINE OpStatus remove(OrderId id) {
    const std::uint32_t oi = index_.erase(id);
    if (IB_UNLIKELY(oi == kNil)) return OpStatus::UnknownOrder;
    books_[pools_->orders[oi].locate].unlink(oi);
    pools_->orders.free(oi);
    return OpStatus::Ok;
  }

  // Cancel-replace: the new order loses time priority. The pool slot is recycled in
  // place (no free + alloc) and only the index entry is re-keyed.
  IB_ALWAYS_INLINE OpStatus replace(OrderId orig, OrderId nu, Qty qty, Price price) {
    const std::uint32_t oi = index_.erase(orig);
    if (IB_UNLIKELY(oi == kNil)) return OpStatus::UnknownOrder;
    Order& o = pools_->orders[oi];
    OrderBook& book = books_[o.locate];
    book.unlink(oi);
    if (IB_UNLIKELY(!index_.insert(nu, oi))) {
      pools_->orders.free(oi);
      return OpStatus::DuplicateId;
    }
    o.id = nu;
    o.qty = qty;
    book.link(oi, price);
    return OpStatus::Ok;
  }

  // Two-stage software prefetch used by batched replay: stage 1 pulls the index slot,
  // stage 2 (issued later, once the slot is cached) pulls the order and its level.
  // Wire the order/level pools (first n slots) and the index into RAM.
  bool lock_memory(std::uint32_t orders, std::uint32_t levels) {
    return pools_->orders.lock(orders) && pools_->levels.lock(levels) && index_.lock();
  }

  IB_ALWAYS_INLINE void prefetch(OrderId id) const noexcept { index_.prefetch(id); }
  // Book object (ladder vector headers) for a symbol: stage 1.
  IB_ALWAYS_INLINE void prefetch_book(Locate loc) const noexcept { IB_PREFETCH_R(&books_[loc]); }
  // Touch end of a book's ladders (the header is cached after stage 1): stage 2.
  IB_ALWAYS_INLINE void prefetch_ladders(Locate loc) const noexcept {
    books_[loc].ladder(Side::Buy).prefetch_touch();
    books_[loc].ladder(Side::Sell).prefetch_touch();
  }
  IB_ALWAYS_INLINE void prefetch_order(OrderId id) const noexcept {
    const std::uint32_t oi = index_.find(id);
    if (oi != kNil) {
      const Order& o = pools_->orders[oi];
      IB_PREFETCH_W(&o);
      IB_PREFETCH_W(&pools_->levels[o.level]);
    }
  }

  std::optional<OrderView> find(OrderId id) const {
    const std::uint32_t oi = index_.find(id);
    if (oi == kNil) return std::nullopt;
    const Order& o = pools_->orders[oi];
    const Level& l = pools_->levels[o.level];
    return OrderView{o.locate, o.side, l.price, o.qty, l.head == oi,
                     books_[o.locate].best_level(o.side) == o.level};
  }

  // Orders ahead of `id` in its price level's FIFO, and the head order (diagnostics).
  struct QueueInfo {
    std::uint32_t ahead;
    OrderId head;
    Qty head_qty;
  };
  std::optional<QueueInfo> queue_info(OrderId id) const {
    const std::uint32_t oi = index_.find(id);
    if (oi == kNil) return std::nullopt;
    const Level& l = pools_->levels[pools_->orders[oi].level];
    std::uint32_t ahead = 0;
    for (std::uint32_t p = pools_->orders[oi].prev; p != kNil; p = pools_->orders[p].prev) ++ahead;
    return QueueInfo{ahead, pools_->orders[l.head].id, pools_->orders[l.head].qty};
  }

  std::optional<LevelView> best(Locate loc, Side side) const { return books_[loc].best(side); }

  void depth(Locate loc, Side side, std::size_t n, std::vector<LevelView>& out) const {
    books_[loc].depth(side, n, out);
  }

  std::uint64_t digest(Locate loc) const { return books_[loc].digest(); }
  std::size_t order_count() const noexcept { return index_.size(); }

  const OrderBook& book(Locate loc) const noexcept { return books_[loc]; }
  const BookPools& pools() const noexcept { return *pools_; }
  const Index& index() const noexcept { return index_; }

 private:
  IB_ALWAYS_INLINE OpStatus reduce(OrderId id, Qty qty) {
    const std::uint32_t oi = index_.find(id);
    if (IB_UNLIKELY(oi == kNil)) return OpStatus::UnknownOrder;
    Order& o = pools_->orders[oi];
    if (qty < o.qty) {
      books_[o.locate].reduce(oi, qty);
      return OpStatus::Ok;
    }
    const bool over = qty > o.qty;
    index_.erase(id);
    books_[o.locate].unlink(oi);
    pools_->orders.free(oi);
    return IB_UNLIKELY(over) ? OpStatus::Overfill : OpStatus::Ok;
  }

  std::unique_ptr<BookPools> pools_;
  Index index_;
  std::vector<OrderBook> books_;
};

using Market = BasicMarket<FlatHashMap<IdentityHash>>;

}  // namespace ib
