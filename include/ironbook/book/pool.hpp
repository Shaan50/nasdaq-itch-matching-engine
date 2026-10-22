#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "ironbook/common/compiler.hpp"
#include "ironbook/common/mapped_file.hpp"

namespace ib {

// Index 0 is the null handle in every pool / intrusive list.
inline constexpr std::uint32_t kNil = 0;

// Fixed-capacity object pool addressed by 32-bit indices.
//
//  * Storage is a single VirtualArena reservation, so objects never move and a handle is
//    4 bytes instead of an 8-byte pointer (halves the size of intrusive links).
//  * Freed slots form an intrusive LIFO free list threaded through the slot itself:
//    the most recently freed (cache-hot) slot is reused first.
//  * alloc()/free() are a handful of instructions and never call malloc.
template <class T>
class IndexPool {
  static_assert(std::is_trivially_copyable_v<T>, "pool objects are raw memory");
  static_assert(sizeof(T) >= sizeof(std::uint32_t));

 public:
  explicit IndexPool(std::uint32_t capacity, bool prefault = false)
      : arena_(static_cast<std::size_t>(capacity) * sizeof(T)),
        base_(static_cast<T*>(arena_.data())),
        capacity_(capacity) {
    if (prefault) arena_.prefault(arena_.size());
  }

  IB_ALWAYS_INLINE std::uint32_t alloc() {
    std::uint32_t idx;
    if (free_head_ != kNil) {
      idx = free_head_;
      std::memcpy(&free_head_, &base_[idx], sizeof free_head_);
    } else {
      if (IB_UNLIKELY(next_ >= capacity_)) exhausted();
      idx = next_++;
    }
    ++live_;
    return idx;
  }

  IB_ALWAYS_INLINE void free(std::uint32_t idx) noexcept {
    assert(idx != kNil && idx < next_);
    std::memcpy(&base_[idx], &free_head_, sizeof free_head_);
    free_head_ = idx;
    --live_;
  }

  IB_ALWAYS_INLINE T& operator[](std::uint32_t idx) noexcept { return base_[idx]; }
  IB_ALWAYS_INLINE const T& operator[](std::uint32_t idx) const noexcept { return base_[idx]; }

  // Wire the first n slots (or the whole reservation) into RAM.
  bool lock(std::uint32_t n) { return arena_.lock(static_cast<std::size_t>(n < capacity_ ? n : capacity_) * sizeof(T)); }

  std::uint32_t live() const noexcept { return live_; }
  std::uint32_t high_water() const noexcept { return next_ - 1; }
  std::uint32_t capacity() const noexcept { return capacity_; }

 private:
  [[noreturn]] IB_COLD static void exhausted() {
    throw std::length_error("IndexPool capacity exhausted; raise the configured capacity");
  }

  VirtualArena arena_;
  T* base_;
  std::uint32_t capacity_;
  std::uint32_t next_ = 1;  // slot 0 reserved as kNil
  std::uint32_t free_head_ = kNil;
  std::uint32_t live_ = 0;
};

}  // namespace ib
