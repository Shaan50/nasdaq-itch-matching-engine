#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <stdexcept>

#include "ironbook/common/compiler.hpp"
#include "ironbook/common/mapped_file.hpp"

namespace ib {

// Hash policies for 64-bit order ids. Each maps a key to a home slot in a table of
// 2^bits slots.
struct IdentityHash {
  // NASDAQ assigns order reference numbers from one day-wide increasing sequence, so
  // consecutive adds land in adjacent slots (great locality). Pathological when the table
  // is small: ref r and the long-lived ref r - 2^bits share a home slot, so the dense
  // window of recent orders piles into clusters left by old ones.
  static constexpr const char* name = "identity";
  IB_ALWAYS_INLINE static std::size_t slot(std::uint64_t k, unsigned /*bits*/) noexcept {
    return static_cast<std::size_t>(k);
  }
};

struct XorFoldHash {
  // Identity within each 2^bits-aligned block of refs (keeps the locality of sequential
  // adds) but XORs in the block number, so refs exactly one table-size apart no longer
  // collide. Best of both for ITCH-style keys.
  static constexpr const char* name = "xor-fold";
  IB_ALWAYS_INLINE static std::size_t slot(std::uint64_t k, unsigned bits) noexcept {
    return static_cast<std::size_t>(k ^ (k >> bits) ^ (k >> (2 * bits)));
  }
};

struct FibonacciHash {
  // Multiplicative hashing by 2^64/phi, taking the high bits.
  static constexpr const char* name = "fibonacci";
  IB_ALWAYS_INLINE static std::size_t slot(std::uint64_t k, unsigned bits) noexcept {
    return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> (64 - bits));
  }
};

struct Murmur3Mix {
  // Full-avalanche finalizer: robust to any key pattern, no locality.
  static constexpr const char* name = "murmur3-fmix";
  IB_ALWAYS_INLINE static std::size_t slot(std::uint64_t k, unsigned /*bits*/) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return static_cast<std::size_t>(k);
  }
};

enum class Probing : std::uint8_t {
  Linear,     // first-fit linear probing, backward-shift delete scans to cluster end
  RobinHood,  // linear probing with Robin Hood displacement (default)
};

// Open-addressing hash map from uint64 key (non-zero) to uint32 value (non-zero).
//
//  * Linear probing over a flat array of 16-byte slots: a lookup usually touches one
//    cache line, with no pointer chasing (vs. std::unordered_map's node per entry).
//  * Robin Hood insertion keeps probe distances non-decreasing along every cluster
//    ("take from the rich"). That invariant makes the two operations a book hammers
//    bounded by the local displacement instead of the cluster length:
//      - a miss stops as soon as it meets an entry closer to home than itself;
//      - backward-shift deletion stops at the first entry already at its home slot.
//    With plain linear probing a delete must scan to the end of the cluster, which is
//    catastrophic for dense runs of sequential keys (see docs/OPTIMIZATION_LOG.md).
//  * Each slot stores its probe distance in otherwise-padding bytes, so neither
//    operation rehashes the keys it walks past.
//  * Capacity is fixed up front; growth is supported (and counted) but should never
//    trigger on the hot path when the table is sized for peak live orders.
template <class Hash = IdentityHash, Probing P = Probing::RobinHood>
class FlatHashMap {
 public:
  static constexpr std::uint32_t kMissing = 0;

  explicit FlatHashMap(std::size_t expected = 1u << 20) {
    std::size_t cap = std::bit_ceil(expected * 2);
    if (cap < 16) cap = 16;
    allocate(cap);
  }

  FlatHashMap(FlatHashMap&& o) noexcept { *this = std::move(o); }
  FlatHashMap& operator=(FlatHashMap&& o) noexcept {
    arena_ = std::move(o.arena_);
    slots_ = std::exchange(o.slots_, nullptr);
    mask_ = o.mask_;
    bits_ = o.bits_;
    size_ = std::exchange(o.size_, 0);
    grow_at_ = o.grow_at_;
    rehashes_ = o.rehashes_;
    return *this;
  }
  FlatHashMap(const FlatHashMap&) = delete;
  FlatHashMap& operator=(const FlatHashMap&) = delete;

  // Returns the mapped value or kMissing.
  IB_ALWAYS_INLINE std::uint32_t find(std::uint64_t key) const noexcept {
    std::size_t i = slot_of(key);
    for (std::uint32_t d = 0;; ++d) {
      const Slot& s = slots_[i];
      if (s.key == key) return s.val;
      if (s.key == 0) return kMissing;
      if constexpr (P == Probing::RobinHood) {
        if (s.dist < d) return kMissing;
      }
      i = (i + 1) & mask_;
    }
  }

  // Inserts key -> val; returns false (and leaves the map unchanged) if key exists.
  IB_ALWAYS_INLINE bool insert(std::uint64_t key, std::uint32_t val) {
    if (IB_UNLIKELY(size_ >= grow_at_)) grow();
    std::size_t i = slot_of(key);
    Slot cur{key, val, 0};
    bool displaced = false;  // once we swap, `cur` is an existing entry: no dup check
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == 0) {
        s = cur;
        ++size_;
        return true;
      }
      if (!displaced && s.key == key) return false;
      if constexpr (P == Probing::RobinHood) {
        // Displacing is only legal once we know the key is absent -- which the invariant
        // guarantees here: a lookup for `key` would have stopped at this slot too.
        if (s.dist < cur.dist) {
          std::swap(s, cur);
          displaced = true;
        }
      }
      ++cur.dist;
      i = (i + 1) & mask_;
    }
  }

  // Overwrites the value for an existing key (no-op if absent).
  IB_ALWAYS_INLINE void assign(std::uint64_t key, std::uint32_t val) noexcept {
    if (Slot* s = locate(key)) s->val = val;
  }

  // Removes key; returns its value or kMissing.
  IB_ALWAYS_INLINE std::uint32_t erase(std::uint64_t key) noexcept {
    Slot* found = locate(key);
    if (!found) return kMissing;
    const std::uint32_t val = found->val;
    std::size_t hole = static_cast<std::size_t>(found - slots_);
    if constexpr (P == Probing::RobinHood) {
      // Shift the following entries back by one until one is at home (or empty).
      for (;;) {
        const std::size_t j = (hole + 1) & mask_;
        Slot& s = slots_[j];
        if (s.key == 0 || s.dist == 0) break;
        slots_[hole] = s;
        --slots_[hole].dist;
        hole = j;
      }
    } else {
      // Plain linear probing: any later entry of the cluster may belong in the hole,
      // so the scan must continue to the cluster's end.
      std::size_t j = hole;
      for (;;) {
        j = (j + 1) & mask_;
        const Slot& s = slots_[j];
        if (s.key == 0) break;
        const std::size_t home = slot_of(s.key);
        if (((j - home) & mask_) >= ((j - hole) & mask_)) {
          slots_[hole] = s;
          slots_[hole].dist = static_cast<std::uint32_t>((hole - home) & mask_);
          hole = j;
        }
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return val;
  }

  IB_ALWAYS_INLINE void prefetch(std::uint64_t key) const noexcept {
    IB_PREFETCH_R(&slots_[slot_of(key)]);
  }

  bool lock() { return arena_.lock(arena_.size()); }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return mask_ + 1; }
  std::size_t rehashes() const noexcept { return rehashes_; }

  // Longest run of occupied slots (diagnostic only).
  std::size_t longest_cluster() const noexcept {
    std::size_t best = 0, run = 0;
    for (std::size_t i = 0; i <= 2 * mask_ + 1; ++i) {  // wrap once
      if (slots_[i & mask_].key) best = std::max(best, ++run);
      else run = 0;
    }
    return std::min(best, mask_ + 1);
  }

  // Mean probe length (1 = found in home slot) over live keys (diagnostic only).
  double mean_probe_length() const noexcept {
    if (size_ == 0) return 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i <= mask_; ++i)
      if (slots_[i].key) total += slots_[i].dist + 1u;
    return static_cast<double>(total) / static_cast<double>(size_);
  }

  // Verifies stored distances and (for Robin Hood) the ordering invariant. Tests only.
  bool check_invariants() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i <= mask_; ++i) {
      const Slot& s = slots_[i];
      if (!s.key) continue;
      ++n;
      if (s.dist != ((i - slot_of(s.key)) & mask_)) return false;
      if constexpr (P == Probing::RobinHood) {
        const Slot& prev = slots_[(i - 1) & mask_];
        if (s.dist > 0 && (!prev.key || prev.dist + 1 < s.dist)) return false;
      }
    }
    return n == size_;
  }

 private:
  struct Slot {
    std::uint64_t key = 0;
    std::uint32_t val = 0;
    std::uint32_t dist = 0;  // probe distance from the home slot
  };

  IB_ALWAYS_INLINE std::size_t slot_of(std::uint64_t key) const noexcept {
    return Hash::slot(key, bits_) & mask_;
  }

  IB_ALWAYS_INLINE Slot* locate(std::uint64_t key) const noexcept {
    std::size_t i = slot_of(key);
    for (std::uint32_t d = 0;; ++d) {
      Slot& s = slots_[i];
      if (s.key == key) return &s;
      if (s.key == 0) return nullptr;
      if constexpr (P == Probing::RobinHood) {
        if (s.dist < d) return nullptr;
      }
      i = (i + 1) & mask_;
    }
  }

  // Backed by an anonymous mapping: zero pages are committed lazily (an empty table of
  // any size is free), Linux gets transparent huge pages, and the table can be mlock'ed.
  void allocate(std::size_t cap) {
    arena_ = VirtualArena(cap * sizeof(Slot));
    slots_ = static_cast<Slot*>(arena_.data());
    mask_ = cap - 1;
    bits_ = static_cast<unsigned>(std::countr_zero(cap));
    grow_at_ = cap / 2;  // max load factor 0.5
  }

  IB_COLD void grow() {
    VirtualArena old_arena = std::move(arena_);
    const Slot* old = slots_;
    const std::size_t old_cap = mask_ + 1;
    allocate(old_cap * 2);
    size_ = 0;
    for (std::size_t i = 0; i < old_cap; ++i)
      if (old[i].key) insert(old[i].key, old[i].val);
    ++rehashes_;
  }

  VirtualArena arena_;
  Slot* slots_ = nullptr;
  std::size_t mask_ = 0;
  unsigned bits_ = 0;
  std::size_t size_ = 0;
  std::size_t grow_at_ = 0;
  std::size_t rehashes_ = 0;
};

}  // namespace ib
