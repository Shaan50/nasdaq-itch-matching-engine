#pragma once

// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Design points:
//  * Power-of-two capacity; head/tail are free-running 64-bit counters (never wrap in
//    practice), so full/empty need no extra flag and indexing is a mask.
//  * Producer-owned and consumer-owned state live on separate cache lines (128 B apart
//    to defeat adjacent-line prefetch), eliminating false sharing.
//  * Each side keeps a *cached* copy of the other side's index and only reloads the
//    shared atomic when the cache says the queue is full/empty. In steady state this
//    removes nearly all cross-core cache-line traffic on the indices.
//  * Only acquire/release ordering; no RMW instructions, no fences, no locks.
//  * Zero-copy batch API: reserve()/commit() on the producer and peek()/release() on the
//    consumer expose contiguous slot ranges, so a batch of N items costs one atomic
//    store per side instead of N.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "ironbook/common/compiler.hpp"

namespace ib {

template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue stores trivially copyable items");

 public:
  explicit SpscQueue(std::size_t capacity)
      : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
        mask_(capacity_ - 1),
        slots_(static_cast<T*>(::operator new[](capacity_ * sizeof(T),
                                                std::align_val_t{kNoFalseSharing}))) {}

  ~SpscQueue() { ::operator delete[](slots_, std::align_val_t{kNoFalseSharing}); }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // ---------------------------------------------------------------- producer side
  IB_ALWAYS_INLINE bool try_push(const T& item) noexcept {
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    if (IB_UNLIKELY(tail - prod_.cached_head == capacity_)) {
      prod_.cached_head = cons_.head.load(std::memory_order_acquire);
      if (tail - prod_.cached_head == capacity_) return false;
    }
    slots_[tail & mask_] = item;
    prod_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Contiguous writable slots starting at the tail (may be fewer than free space when the
  // free region wraps). Returns 0 when full.
  IB_ALWAYS_INLINE std::size_t reserve(T*& out) noexcept {
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    std::size_t free = capacity_ - (tail - prod_.cached_head);
    if (free == 0) {
      prod_.cached_head = cons_.head.load(std::memory_order_acquire);
      free = capacity_ - (tail - prod_.cached_head);
      if (free == 0) return 0;
    }
    const std::size_t idx = tail & mask_;
    out = slots_ + idx;
    return std::min(free, capacity_ - idx);
  }

  // Publishes n slots previously obtained from reserve().
  IB_ALWAYS_INLINE void commit(std::size_t n) noexcept {
    prod_.tail.store(prod_.tail.load(std::memory_order_relaxed) + n, std::memory_order_release);
  }

  // ---------------------------------------------------------------- consumer side
  IB_ALWAYS_INLINE bool try_pop(T& out) noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    if (IB_UNLIKELY(head == cons_.cached_tail)) {
      cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
      if (head == cons_.cached_tail) return false;
    }
    out = slots_[head & mask_];
    cons_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  // Contiguous readable slots starting at the head. Returns 0 when empty.
  IB_ALWAYS_INLINE std::size_t peek(const T*& out) noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    std::size_t avail = cons_.cached_tail - head;
    if (avail == 0) {
      cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
      avail = cons_.cached_tail - head;
      if (avail == 0) return 0;
    }
    const std::size_t idx = head & mask_;
    out = slots_ + idx;
    return std::min(avail, capacity_ - idx);
  }

  // Frees n slots previously obtained from peek().
  IB_ALWAYS_INLINE void release(std::size_t n) noexcept {
    cons_.head.store(cons_.head.load(std::memory_order_relaxed) + n, std::memory_order_release);
  }

  // ---------------------------------------------------------------- either side
  std::size_t capacity() const noexcept { return capacity_; }
  // Approximate when called concurrently.
  std::size_t size() const noexcept {
    return prod_.tail.load(std::memory_order_acquire) - cons_.head.load(std::memory_order_acquire);
  }
  bool empty() const noexcept { return size() == 0; }

 private:
  struct alignas(kNoFalseSharing) Producer {
    std::atomic<std::size_t> tail{0};
    std::size_t cached_head = 0;
  };
  struct alignas(kNoFalseSharing) Consumer {
    std::atomic<std::size_t> head{0};
    std::size_t cached_tail = 0;
  };

  const std::size_t capacity_;
  const std::size_t mask_;
  T* const slots_;
  Producer prod_;
  Consumer cons_;
};

}  // namespace ib
