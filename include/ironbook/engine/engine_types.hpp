#pragma once

#include <cstdint>

#include "ironbook/common/types.hpp"

namespace ib::engine {

using InstrumentId = std::uint16_t;
using OwnerId = std::uint32_t;

enum class OrderType : std::uint8_t {
  Limit,     // match what crosses, rest the remainder (GTC)
  Market,    // match at any price, cancel the remainder; never rests
  IOC,       // immediate-or-cancel: limit price, cancel the remainder
  FOK,       // fill-or-kill: execute in full immediately or not at all
  PostOnly,  // add liquidity only: rejected if it would cross
};

// Self-trade prevention when an incoming order would match a resting order of the same
// (non-zero) owner.
enum class StpMode : std::uint8_t {
  None,            // allow self-trades
  CancelResting,   // cancel the resting order and keep matching
  CancelIncoming,  // cancel the remainder of the incoming order
};

enum class RejectReason : std::uint8_t {
  DuplicateOrderId,
  InvalidQuantity,
  InvalidPrice,
  UnknownInstrument,
  PostOnlyWouldCross,
  FokInsufficientLiquidity,
  UnknownOrder,
};

enum class CancelReason : std::uint8_t {
  UserRequested,
  ImmediateOrCancel,  // unfilled remainder of IOC / market
  SelfTradePrevention,
  Replaced,
};

struct NewOrder {
  OrderId id = 0;
  OwnerId owner = 0;
  InstrumentId instrument = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;  // ignored for Market
  Qty qty = 0;
};

struct Fill {
  std::uint64_t match_id;
  InstrumentId instrument;
  OrderId taker;
  OrderId maker;
  OwnerId taker_owner;
  OwnerId maker_owner;
  Side taker_side;
  Price price;       // always the resting (maker) price
  Qty qty;
  Qty maker_remaining;
  Qty taker_remaining;
  friend bool operator==(const Fill&, const Fill&) = default;
};

constexpr bool crosses(Side taker, Price limit, Price resting) noexcept {
  return taker == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace ib::engine
