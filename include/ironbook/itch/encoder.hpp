#pragma once

// ITCH 5.0 encoder. Produces byte-exact, length-prefixed frames; used by the unit tests
// (round-trip checks, hand-built scenarios) and by the synthetic feed generator.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "ironbook/common/endian.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib::itch {

class Encoder {
 public:
  const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
  std::vector<std::uint8_t> take() noexcept { return std::move(buf_); }
  void clear() noexcept { buf_.clear(); }
  std::size_t size() const noexcept { return buf_.size(); }

  void system_event(Timestamp ts, char code) {
    auto* p = begin(MsgType::SystemEvent, 0, ts);
    p[11] = static_cast<std::uint8_t>(code);
  }

  void stock_directory(Locate loc, Timestamp ts, std::string_view symbol,
                       std::uint32_t round_lot = 100) {
    auto* p = begin(MsgType::StockDirectory, loc, ts);
    put_stock(p + 11, symbol);
    p[19] = 'Q';
    p[20] = 'N';
    be::store32(p + 21, round_lot);
    p[25] = 'N';
    p[26] = 'C';
    p[27] = 'Z';
    p[28] = ' ';
    p[29] = 'P';
    p[30] = 'N';
    p[31] = ' ';
    p[32] = '1';
    p[33] = 'N';
    be::store32(p + 34, 0);
    p[38] = 'N';
  }

  void trading_action(Locate loc, Timestamp ts, std::string_view symbol, char state) {
    auto* p = begin(MsgType::StockTradingAction, loc, ts);
    put_stock(p + 11, symbol);
    p[19] = static_cast<std::uint8_t>(state);
    p[20] = ' ';
    std::memset(p + 21, ' ', 4);
  }

  void add(Locate loc, Timestamp ts, OrderId ref, Side side, Qty shares, std::string_view symbol,
           Price price) {
    auto* p = begin(MsgType::AddOrder, loc, ts);
    put_add_body(p, ref, side, shares, symbol, price);
  }

  void add_mpid(Locate loc, Timestamp ts, OrderId ref, Side side, Qty shares,
                std::string_view symbol, Price price, std::string_view mpid) {
    auto* p = begin(MsgType::AddOrderMpid, loc, ts);
    put_add_body(p, ref, side, shares, symbol, price);
    std::memset(p + 36, ' ', 4);
    std::memcpy(p + 36, mpid.data(), mpid.size() < 4 ? mpid.size() : 4);
  }

  void executed(Locate loc, Timestamp ts, OrderId ref, Qty shares, std::uint64_t match) {
    auto* p = begin(MsgType::OrderExecuted, loc, ts);
    be::store64(p + 11, ref);
    be::store32(p + 19, shares);
    be::store64(p + 23, match);
  }

  void executed_price(Locate loc, Timestamp ts, OrderId ref, Qty shares, std::uint64_t match,
                      bool printable, Price price) {
    auto* p = begin(MsgType::OrderExecutedWithPrice, loc, ts);
    be::store64(p + 11, ref);
    be::store32(p + 19, shares);
    be::store64(p + 23, match);
    p[31] = printable ? 'Y' : 'N';
    be::store32(p + 32, price);
  }

  void cancel(Locate loc, Timestamp ts, OrderId ref, Qty shares) {
    auto* p = begin(MsgType::OrderCancel, loc, ts);
    be::store64(p + 11, ref);
    be::store32(p + 19, shares);
  }

  void del(Locate loc, Timestamp ts, OrderId ref) {
    auto* p = begin(MsgType::OrderDelete, loc, ts);
    be::store64(p + 11, ref);
  }

  void replace(Locate loc, Timestamp ts, OrderId orig, OrderId new_ref, Qty shares, Price price) {
    auto* p = begin(MsgType::OrderReplace, loc, ts);
    be::store64(p + 11, orig);
    be::store64(p + 19, new_ref);
    be::store32(p + 27, shares);
    be::store32(p + 31, price);
  }

  void trade(Locate loc, Timestamp ts, OrderId ref, Side side, Qty shares, std::string_view symbol,
             Price price, std::uint64_t match) {
    auto* p = begin(MsgType::Trade, loc, ts);
    put_add_body(p, ref, side, shares, symbol, price);
    be::store64(p + 36, match);
  }

  void cross(Locate loc, Timestamp ts, std::uint64_t shares, std::string_view symbol, Price price,
             std::uint64_t match, char type) {
    auto* p = begin(MsgType::CrossTrade, loc, ts);
    be::store64(p + 11, shares);
    put_stock(p + 19, symbol);
    be::store32(p + 27, price);
    be::store64(p + 31, match);
    p[39] = static_cast<std::uint8_t>(type);
  }

  void broken(Locate loc, Timestamp ts, std::uint64_t match) {
    auto* p = begin(MsgType::BrokenTrade, loc, ts);
    be::store64(p + 11, match);
  }

  // Raw frame (for malformed-input tests).
  void raw(const std::uint8_t* payload, std::uint16_t len) {
    const std::size_t at = buf_.size();
    buf_.resize(at + 2 + len);
    be::store16(buf_.data() + at, len);
    std::memcpy(buf_.data() + at + 2, payload, len);
  }

  std::uint16_t tracking = 0;

 private:
  std::uint8_t* begin(MsgType t, Locate loc, Timestamp ts) {
    const std::size_t len = kMessageLength[static_cast<unsigned char>(t)];
    const std::size_t at = buf_.size();
    buf_.resize(at + 2 + len, 0);
    std::uint8_t* f = buf_.data() + at;
    be::store16(f, static_cast<std::uint16_t>(len));
    std::uint8_t* p = f + 2;
    p[0] = static_cast<std::uint8_t>(t);
    be::store16(p + 1, loc);
    be::store16(p + 3, tracking);
    be::store48(p + 5, ts);
    return p;
  }

  static void put_stock(std::uint8_t* p, std::string_view s) {
    std::memset(p, ' ', 8);
    std::memcpy(p, s.data(), s.size() < 8 ? s.size() : 8);
  }

  static void put_add_body(std::uint8_t* p, OrderId ref, Side side, Qty shares,
                           std::string_view symbol, Price price) {
    be::store64(p + 11, ref);
    p[19] = side == Side::Buy ? 'B' : 'S';
    be::store32(p + 20, shares);
    put_stock(p + 24, symbol);
    be::store32(p + 32, price);
  }

  std::vector<std::uint8_t> buf_;
};

}  // namespace ib::itch
