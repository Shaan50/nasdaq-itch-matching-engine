#pragma once

#include <cstdint>

#include "ironbook/common/types.hpp"

namespace ib {

// Aggregated view of one price level.
struct LevelView {
  Price price = 0;
  std::uint64_t qty = 0;
  std::uint32_t orders = 0;
  friend bool operator==(const LevelView&, const LevelView&) = default;
};

// Snapshot of a resting order's position in its book (used by feed validation).
struct OrderView {
  Locate locate = 0;
  Side side = Side::Buy;
  Price price = 0;
  Qty qty = 0;
  bool at_level_head = false;  // first in time priority at its price
  bool at_best_level = false;  // its price is the best on its side
};

enum class OpStatus : std::uint8_t {
  Ok,
  UnknownOrder,   // referenced order id is not live
  DuplicateId,    // add with an id that is already live
  Overfill,       // execute/cancel more shares than remain (order removed)
};

// FNV-1a style mixing for book state digests (differential testing).
struct Digest {
  std::uint64_t h = 1469598103934665603ull;
  void mix(std::uint64_t v) noexcept {
    h ^= v;
    h *= 1099511628211ull;
  }
};

}  // namespace ib
