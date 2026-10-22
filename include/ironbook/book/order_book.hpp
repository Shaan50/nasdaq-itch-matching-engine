#pragma once

// Optimized single-instrument limit order book.
//
//   Order  (32 B) ── pooled, 2 per 64 B line; doubly-linked FIFO via 32-bit indices
//   Level  (32 B) ── pooled; aggregate qty/count + head/tail of its FIFO queue
//   PriceLadder   ── per side, sorted structure-of-arrays {key[], level[]} with the best
//                    price at the BACK, so the touch is always the last element and
//                    inserting/removing levels near the touch moves almost nothing.
//
// Complexities (L = levels between the touch and the target price):
//   cancel / delete / execute / reduce  O(1)   (order handle -> level handle, no search)
//   add to existing level               O(min(L, 8)) linear scan from the touch, else
//                                       O(log L) binary search for deep prices
//   add creating a level / level removal  same search + memmove of the levels behind it
//   best bid/ask                        O(1)
//
// Pools and the order-id index are owned by the caller (Market or MatchingEngine) so
// many books share one contiguous order arena.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "ironbook/book/book_types.hpp"
#include "ironbook/book/pool.hpp"
#include "ironbook/common/compiler.hpp"

namespace ib {

struct Order {
  OrderId id;
  Qty qty;
  std::uint32_t level;  // owning Level handle
  std::uint32_t prev;   // FIFO neighbours (kNil-terminated)
  std::uint32_t next;
  std::uint32_t owner;  // participant id for the matching engine (0 for feed orders)
  Locate locate;
  Side side;
  std::uint8_t flags;
};
static_assert(sizeof(Order) == 32, "Order must stay 32 bytes (2 per cache line)");

struct Level {
  std::uint64_t total;  // shares resting at this price
  Price price;
  std::uint32_t count;
  std::uint32_t head;   // oldest order (highest time priority)
  std::uint32_t tail;
  std::uint64_t reserved;
};
static_assert(sizeof(Level) == 32, "Level must stay 32 bytes");

struct BookPools {
  BookPools(std::uint32_t max_orders, std::uint32_t max_levels, bool prefault = false)
      : orders(max_orders, prefault), levels(max_levels, prefault) {}
  IndexPool<Order> orders;
  IndexPool<Level> levels;
};

// Price keys are normalized so that "better" is always numerically larger:
//   bid key = price, ask key = ~price.
IB_ALWAYS_INLINE constexpr std::uint32_t ladder_key(Side s, Price p) noexcept {
  return s == Side::Buy ? p : ~p;
}

class PriceLadder {
 public:
  static constexpr std::size_t kBlock = 16;
  static constexpr std::size_t kScanBlocks = 4;  // vector-scan the 64 levels nearest the touch

  PriceLadder() = default;

  IB_ALWAYS_INLINE bool empty() const noexcept { return keys_.empty(); }
  IB_ALWAYS_INLINE std::size_t size() const noexcept { return keys_.size(); }
  IB_ALWAYS_INLINE std::uint32_t best() const noexcept {
    return levels_.empty() ? kNil : levels_.back();
  }
  // Prefetch the touch end of both arrays (where most searches start).
  IB_ALWAYS_INLINE void prefetch_touch() const noexcept {
    if (!keys_.empty()) {
      IB_PREFETCH_R(keys_.data() + keys_.size() - 1);
      IB_PREFETCH_R(levels_.data() + levels_.size() - 1);
    }
  }

  // i-th level from the touch (0 = best).
  IB_ALWAYS_INLINE std::uint32_t from_top(std::size_t i) const noexcept {
    return levels_[levels_.size() - 1 - i];
  }

  // First index whose key is greater than `key` (keys are ascending, best at the back).
  //
  // Measured on the 2019-12-30 feed, 81% of adds and 66% of level removals land within
  // 8 levels of the touch, but the tail is long (mean depth 157, max 3,694). So we scan
  // from the back in fixed 16-key blocks, counting keys > `key` with a branch-free loop
  // the compiler vectorizes (NEON / SSE / AVX): one well-predicted branch per block
  // instead of one unpredictable branch per key. Past kScanBlocks blocks a branchless
  // binary search takes over.
  IB_ALWAYS_INLINE std::size_t upper(std::uint32_t key) const noexcept {
    const std::uint32_t* k = keys_.data();
    std::size_t hi = keys_.size();
    if (hi == 0 || k[hi - 1] <= key) return hi;  // at or beyond the touch: 45% of adds
    for (std::size_t b = 0; b < kScanBlocks && hi >= kBlock; ++b) {
      std::uint32_t greater = 0;
      for (std::size_t j = 0; j < kBlock; ++j) greater += k[hi - kBlock + j] > key;
      if (greater < kBlock) return hi - greater;
      hi -= kBlock;
    }
    if (hi < kBlock) {  // short remainder: plain count over what is left
      std::size_t greater = 0;
      for (std::size_t j = 0; j < hi; ++j) greater += k[j] > key;
      return hi - greater;
    }
    // Branchless upper_bound over [0, hi): the loop trip count depends only on hi.
    const std::uint32_t* base = k;
    std::size_t n = hi;
    while (n > 1) {
      const std::size_t half = n / 2;
      base = (base[half - 1] <= key) ? base + half : base;  // compiles to csel
      n -= half;
    }
    return static_cast<std::size_t>(base - k) + (*base <= key);
  }

  // Returns the level handle for `key`, or kNil plus the insertion position.
  IB_ALWAYS_INLINE std::uint32_t find(std::uint32_t key, std::size_t& pos) const noexcept {
    pos = upper(key);
    return (pos > 0 && keys_[pos - 1] == key) ? levels_[pos - 1] : kNil;
  }

  IB_ALWAYS_INLINE void insert_at(std::size_t pos, std::uint32_t key, std::uint32_t level) {
    if (IB_UNLIKELY(keys_.capacity() == 0)) reserve_initial();
    keys_.insert(keys_.begin() + static_cast<std::ptrdiff_t>(pos), key);
    levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(pos), level);
  }

  IB_ALWAYS_INLINE void erase(std::uint32_t key) noexcept {
    if (keys_.back() == key) {  // fast path: the touch
      keys_.pop_back();
      levels_.pop_back();
      return;
    }
    const std::size_t pos = upper(key) - 1;
    assert(keys_[pos] == key);
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(pos));
    levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(pos));
  }

 private:
  IB_COLD void reserve_initial() {
    keys_.reserve(64);
    levels_.reserve(64);
  }

  std::vector<std::uint32_t> keys_;
  std::vector<std::uint32_t> levels_;
};

class OrderBook {
 public:
  OrderBook() = default;
  explicit OrderBook(BookPools* pools) : pools_(pools) {}
  void attach(BookPools* pools) noexcept { pools_ = pools; }

  // Appends an already-populated order (id/qty/side/locate/owner set) to the tail of its
  // price level, creating the level if needed.
  IB_ALWAYS_INLINE void link(std::uint32_t oi, Price price) {
    Order& o = pools_->orders[oi];
    PriceLadder& ladder = ladders_[side_index(o.side)];
    const std::uint32_t key = ladder_key(o.side, price);
    std::size_t pos;
    std::uint32_t li = ladder.find(key, pos);
    if (li == kNil) {
      li = pools_->levels.alloc();
      pools_->levels[li] = Level{0, price, 0, kNil, kNil, 0};
      ladder.insert_at(pos, key, li);
    }
    Level& lvl = pools_->levels[li];
    o.level = li;
    o.next = kNil;
    o.prev = lvl.tail;
    if (lvl.tail != kNil) pools_->orders[lvl.tail].next = oi;
    else lvl.head = oi;
    lvl.tail = oi;
    ++lvl.count;
    lvl.total += o.qty;
  }

  // Removes the order from its level (freeing the level if it empties). Does not free
  // the order slot, so callers can relink it (replace) or recycle it.
  IB_ALWAYS_INLINE void unlink(std::uint32_t oi) noexcept {
    Order& o = pools_->orders[oi];
    Level& lvl = pools_->levels[o.level];
    if (o.prev != kNil) pools_->orders[o.prev].next = o.next;
    else lvl.head = o.next;
    if (o.next != kNil) pools_->orders[o.next].prev = o.prev;
    else lvl.tail = o.prev;
    lvl.total -= o.qty;
    if (--lvl.count == 0) {
      ladders_[side_index(o.side)].erase(ladder_key(o.side, lvl.price));
      pools_->levels.free(o.level);
    }
  }

  // Partial reduction that leaves the order resting (qty < o.qty). Keeps time priority.
  IB_ALWAYS_INLINE void reduce(std::uint32_t oi, Qty qty) noexcept {
    Order& o = pools_->orders[oi];
    assert(qty < o.qty);
    o.qty -= qty;
    pools_->levels[o.level].total -= qty;
  }

  IB_ALWAYS_INLINE std::uint32_t best_level(Side s) const noexcept {
    return ladders_[side_index(s)].best();
  }
  const PriceLadder& ladder(Side s) const noexcept { return ladders_[side_index(s)]; }
  PriceLadder& ladder(Side s) noexcept { return ladders_[side_index(s)]; }

  std::optional<LevelView> best(Side s) const noexcept {
    const std::uint32_t li = best_level(s);
    if (li == kNil) return std::nullopt;
    return view(li);
  }

  LevelView view(std::uint32_t li) const noexcept {
    const Level& l = pools_->levels[li];
    return {l.price, l.total, l.count};
  }

  void depth(Side s, std::size_t n, std::vector<LevelView>& out) const {
    out.clear();
    const PriceLadder& lad = ladders_[side_index(s)];
    for (std::size_t i = 0; i < lad.size() && i < n; ++i) out.push_back(view(lad.from_top(i)));
  }

  std::uint64_t digest() const noexcept {
    Digest d;
    for (Side s : {Side::Buy, Side::Sell}) {
      const PriceLadder& lad = ladders_[side_index(s)];
      for (std::size_t i = 0; i < lad.size(); ++i) {
        const Level& l = pools_->levels[lad.from_top(i)];
        d.mix(l.price);
        d.mix(l.total);
        for (std::uint32_t oi = l.head; oi != kNil; oi = pools_->orders[oi].next) {
          d.mix(pools_->orders[oi].id);
          d.mix(pools_->orders[oi].qty);
        }
      }
      d.mix(0xFFFF'FFFF'FFFF'FFFFull);
    }
    return d.h;
  }

  // Exhaustive structural check (tests / debug): links, aggregates, ordering.
  bool check_invariants() const;

  bool empty() const noexcept { return ladders_[0].empty() && ladders_[1].empty(); }

 private:
  BookPools* pools_ = nullptr;
  PriceLadder ladders_[2];
};

inline bool OrderBook::check_invariants() const {
  for (Side s : {Side::Buy, Side::Sell}) {
    const PriceLadder& lad = ladders_[side_index(s)];
    for (std::size_t i = 0; i < lad.size(); ++i) {
      const std::uint32_t li = lad.from_top(i);
      const Level& l = pools_->levels[li];
      // Walking away from the touch, keys must strictly decrease.
      if (i > 0 && ladder_key(s, l.price) >= ladder_key(s, pools_->levels[lad.from_top(i - 1)].price))
        return false;
      std::uint64_t total = 0;
      std::uint32_t count = 0;
      std::uint32_t prev = kNil;
      for (std::uint32_t oi = l.head; oi != kNil; oi = pools_->orders[oi].next) {
        const Order& o = pools_->orders[oi];
        if (o.prev != prev || o.level != li || o.side != s || o.qty == 0) return false;
        total += o.qty;
        ++count;
        prev = oi;
      }
      if (prev != l.tail || total != l.total || count != l.count || count == 0) return false;
    }
  }
  return true;
}

}  // namespace ib
