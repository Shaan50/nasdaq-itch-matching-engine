#pragma once

// NASDAQ TotalView-ITCH 5.0 message definitions.
//
// Wire format: every message in the binary file is framed by a 2-byte big-endian length
// followed by the payload. Every payload starts with the same 11-byte header:
//
//   offset 0  : Message Type        (1, alpha)
//   offset 1  : Stock Locate        (2, uint16)
//   offset 3  : Tracking Number     (2, uint16)
//   offset 5  : Timestamp           (6, ns since midnight)
//
// Integers are big-endian; prices are Price(4) with 4 implied decimals unless noted.
// The decoded structs below are host-endian PODs produced by the parser.

#include <array>
#include <cstdint>
#include <string_view>

#include "ironbook/common/types.hpp"

namespace ib::itch {

enum class MsgType : char {
  SystemEvent = 'S',
  StockDirectory = 'R',
  StockTradingAction = 'H',
  RegShoRestriction = 'Y',
  MarketParticipantPosition = 'L',
  MwcbDeclineLevel = 'V',
  MwcbStatus = 'W',
  IpoQuotingPeriod = 'K',
  LuldAuctionCollar = 'J',
  OperationalHalt = 'h',
  AddOrder = 'A',
  AddOrderMpid = 'F',
  OrderExecuted = 'E',
  OrderExecutedWithPrice = 'C',
  OrderCancel = 'X',
  OrderDelete = 'D',
  OrderReplace = 'U',
  Trade = 'P',
  CrossTrade = 'Q',
  BrokenTrade = 'B',
  Noii = 'I',
  Rpii = 'N',
  DirectListingPriceDiscovery = 'O',
};

// Payload length (excluding the 2-byte frame) for every message type; 0 = unknown.
inline constexpr std::array<std::uint8_t, 128> kMessageLength = [] {
  std::array<std::uint8_t, 128> t{};
  t['S'] = 12; t['R'] = 39; t['H'] = 25; t['Y'] = 20; t['L'] = 26; t['V'] = 35;
  t['W'] = 12; t['K'] = 28; t['J'] = 35; t['h'] = 21; t['A'] = 36; t['F'] = 40;
  t['E'] = 31; t['C'] = 36; t['X'] = 23; t['D'] = 19; t['U'] = 35; t['P'] = 44;
  t['Q'] = 40; t['B'] = 19; t['I'] = 50; t['N'] = 20; t['O'] = 48;
  return t;
}();

inline constexpr std::size_t kHeaderLen = 11;

using Stock = std::array<char, 8>;  // space-padded, not NUL-terminated

inline std::string_view trim(const Stock& s) noexcept {
  std::size_t n = s.size();
  while (n > 0 && s[n - 1] == ' ') --n;
  return {s.data(), n};
}

struct Header {
  MsgType type;
  Locate locate;
  std::uint16_t tracking;
  Timestamp timestamp;
};

// --- Order book messages -----------------------------------------------------------

struct AddOrder {
  Header h;
  OrderId ref;
  Side side;
  Qty shares;
  Stock stock;
  Price price;
  std::array<char, 4> mpid;  // only populated for 'F'; zeroed for 'A'
  bool has_mpid;
};

struct OrderExecuted {
  Header h;
  OrderId ref;
  Qty shares;
  std::uint64_t match;
};

struct OrderExecutedWithPrice {
  Header h;
  OrderId ref;
  Qty shares;
  std::uint64_t match;
  bool printable;
  Price price;
};

struct OrderCancel {
  Header h;
  OrderId ref;
  Qty shares;
};

struct OrderDelete {
  Header h;
  OrderId ref;
};

struct OrderReplace {
  Header h;
  OrderId orig_ref;
  OrderId new_ref;
  Qty shares;
  Price price;
};

// --- Trade messages ----------------------------------------------------------------

struct Trade {  // execution against a non-displayed order; does not touch the book
  Header h;
  OrderId ref;
  Side side;
  Qty shares;
  Stock stock;
  Price price;
  std::uint64_t match;
};

struct CrossTrade {
  Header h;
  std::uint64_t shares;
  Stock stock;
  Price price;
  std::uint64_t match;
  char cross_type;  // 'O' open, 'C' close, 'H' halt/IPO, 'I' intraday
};

struct BrokenTrade {
  Header h;
  std::uint64_t match;
};

// --- Administrative / reference data -------------------------------------------------

struct SystemEvent {
  Header h;
  char event_code;  // 'O','S','Q','M','E','C'
};

struct StockDirectory {
  Header h;
  Stock stock;
  char market_category;
  char financial_status;
  std::uint32_t round_lot_size;
  char round_lots_only;
  char issue_classification;
  std::array<char, 2> issue_subtype;
  char authenticity;
  char short_sale_threshold;
  char ipo_flag;
  char luld_tier;
  char etp_flag;
  std::uint32_t etp_leverage;
  char inverse;
};

struct StockTradingAction {
  Header h;
  Stock stock;
  char trading_state;  // 'H' halted, 'P' paused, 'Q' quotation only, 'T' trading
  std::array<char, 4> reason;
};

struct RegShoRestriction {
  Header h;
  Stock stock;
  char action;
};

struct MarketParticipantPosition {
  Header h;
  std::array<char, 4> mpid;
  Stock stock;
  char primary_mm;
  char mode;
  char state;
};

struct MwcbDeclineLevel {
  Header h;
  std::uint64_t level1, level2, level3;  // Price(8)
};

struct MwcbStatus {
  Header h;
  char breached_level;
};

struct IpoQuotingPeriod {
  Header h;
  Stock stock;
  std::uint32_t release_time;
  char qualifier;
  Price price;
};

struct LuldAuctionCollar {
  Header h;
  Stock stock;
  Price ref_price, upper, lower;
  std::uint32_t extension;
};

struct OperationalHalt {
  Header h;
  Stock stock;
  char market_code;
  char action;
};

struct Noii {
  Header h;
  std::uint64_t paired_shares;
  std::uint64_t imbalance_shares;
  char imbalance_direction;
  Stock stock;
  Price far_price, near_price, ref_price;
  char cross_type;
  char price_variation;
};

struct Rpii {
  Header h;
  Stock stock;
  char interest_flag;
};

struct DirectListingPriceDiscovery {
  Header h;
  Stock stock;
  char open_eligible;
  Price min_price, max_price, near_price;
  std::uint64_t near_time;
  Price lower_collar, upper_collar;
};

}  // namespace ib::itch
