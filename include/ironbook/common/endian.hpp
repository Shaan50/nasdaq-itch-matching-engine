#pragma once

#include <cstdint>
#include <cstring>

#include "ironbook/common/compiler.hpp"

// Unaligned big-endian loads/stores. memcpy compiles to a single (unaligned) load and
// the bswap to `rev` (arm64) / `movbe`/`bswap` (x86), so each field costs ~1 cycle.
namespace ib::be {

IB_ALWAYS_INLINE std::uint16_t load16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
}

IB_ALWAYS_INLINE std::uint32_t load32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap32(v);
}

IB_ALWAYS_INLINE std::uint64_t load64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap64(v);
}

// ITCH timestamps are 6-byte big-endian integers.
IB_ALWAYS_INLINE std::uint64_t load48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(load16(p)) << 32) | load32(p + 2);
}

IB_ALWAYS_INLINE void store16(std::uint8_t* p, std::uint16_t v) noexcept {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, sizeof v);
}

IB_ALWAYS_INLINE void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, sizeof v);
}

IB_ALWAYS_INLINE void store64(std::uint8_t* p, std::uint64_t v) noexcept {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, sizeof v);
}

IB_ALWAYS_INLINE void store48(std::uint8_t* p, std::uint64_t v) noexcept {
  store16(p, static_cast<std::uint16_t>(v >> 32));
  store32(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace ib::be
