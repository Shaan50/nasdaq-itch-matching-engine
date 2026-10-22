#pragma once

// Zero-copy ITCH 5.0 parser.
//
// The parser walks length-prefixed frames directly over a byte buffer (normally an
// mmap of the file), decodes each payload into a host-endian struct on the stack, and
// hands it to the handler. Handlers are bound statically (template parameter), so the
// switch + decode + handler body inline into one loop with no virtual calls. A handler
// only implements on(const X&) for the messages it cares about; the rest compile away.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "ironbook/common/compiler.hpp"
#include "ironbook/common/endian.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib::itch {

namespace detail {

IB_ALWAYS_INLINE Header header(const std::uint8_t* p) noexcept {
  return {static_cast<MsgType>(p[0]), be::load16(p + 1), be::load16(p + 3), be::load48(p + 5)};
}

IB_ALWAYS_INLINE Stock stock(const std::uint8_t* p) noexcept {
  Stock s;
  std::memcpy(s.data(), p, 8);
  return s;
}

template <std::size_t N>
IB_ALWAYS_INLINE std::array<char, N> chars(const std::uint8_t* p) noexcept {
  std::array<char, N> a;
  std::memcpy(a.data(), p, N);
  return a;
}

IB_ALWAYS_INLINE char ch(const std::uint8_t* p) noexcept { return static_cast<char>(*p); }
IB_ALWAYS_INLINE Side side(const std::uint8_t* p) noexcept {
  return *p == 'S' ? Side::Sell : Side::Buy;
}

}  // namespace detail

// --- Per-message decoders (p points at the message type byte) ---------------------

IB_ALWAYS_INLINE AddOrder decode_add(const std::uint8_t* p, bool mpid) noexcept {
  using namespace detail;
  AddOrder m{header(p), be::load64(p + 11), side(p + 19), be::load32(p + 20), stock(p + 24),
             be::load32(p + 32), {}, mpid};
  if (mpid) m.mpid = chars<4>(p + 36);
  return m;
}
IB_ALWAYS_INLINE OrderExecuted decode_executed(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11), be::load32(p + 19), be::load64(p + 23)};
}
IB_ALWAYS_INLINE OrderExecutedWithPrice decode_executed_price(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11), be::load32(p + 19), be::load64(p + 23),
          p[31] == 'Y', be::load32(p + 32)};
}
IB_ALWAYS_INLINE OrderCancel decode_cancel(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11), be::load32(p + 19)};
}
IB_ALWAYS_INLINE OrderDelete decode_delete(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11)};
}
IB_ALWAYS_INLINE OrderReplace decode_replace(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11), be::load64(p + 19), be::load32(p + 27),
          be::load32(p + 31)};
}
IB_ALWAYS_INLINE Trade decode_trade(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p),     be::load64(p + 11), side(p + 19),      be::load32(p + 20),
          stock(p + 24), be::load32(p + 32), be::load64(p + 36)};
}
IB_ALWAYS_INLINE CrossTrade decode_cross(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p), be::load64(p + 11), stock(p + 19), be::load32(p + 27), be::load64(p + 31),
          ch(p + 39)};
}
IB_ALWAYS_INLINE BrokenTrade decode_broken(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11)};
}
IB_ALWAYS_INLINE SystemEvent decode_system(const std::uint8_t* p) noexcept {
  return {detail::header(p), detail::ch(p + 11)};
}
IB_ALWAYS_INLINE StockDirectory decode_directory(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p),          stock(p + 11), ch(p + 19),        ch(p + 20),
          be::load32(p + 21), ch(p + 25),    ch(p + 26),        chars<2>(p + 27),
          ch(p + 29),         ch(p + 30),    ch(p + 31),        ch(p + 32),
          ch(p + 33),         be::load32(p + 34), ch(p + 38)};
}
IB_ALWAYS_INLINE StockTradingAction decode_trading_action(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p), stock(p + 11), ch(p + 19), chars<4>(p + 21)};
}
IB_ALWAYS_INLINE RegShoRestriction decode_reg_sho(const std::uint8_t* p) noexcept {
  return {detail::header(p), detail::stock(p + 11), detail::ch(p + 19)};
}
IB_ALWAYS_INLINE MarketParticipantPosition decode_mpp(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p), chars<4>(p + 11), stock(p + 15), ch(p + 23), ch(p + 24), ch(p + 25)};
}
IB_ALWAYS_INLINE MwcbDeclineLevel decode_mwcb_decline(const std::uint8_t* p) noexcept {
  return {detail::header(p), be::load64(p + 11), be::load64(p + 19), be::load64(p + 27)};
}
IB_ALWAYS_INLINE MwcbStatus decode_mwcb_status(const std::uint8_t* p) noexcept {
  return {detail::header(p), detail::ch(p + 11)};
}
IB_ALWAYS_INLINE IpoQuotingPeriod decode_ipo(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p), stock(p + 11), be::load32(p + 19), ch(p + 23), be::load32(p + 24)};
}
IB_ALWAYS_INLINE LuldAuctionCollar decode_luld(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p),          stock(p + 11),      be::load32(p + 19),
          be::load32(p + 23), be::load32(p + 27), be::load32(p + 31)};
}
IB_ALWAYS_INLINE OperationalHalt decode_op_halt(const std::uint8_t* p) noexcept {
  return {detail::header(p), detail::stock(p + 11), detail::ch(p + 19), detail::ch(p + 20)};
}
IB_ALWAYS_INLINE Noii decode_noii(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p),          be::load64(p + 11), be::load64(p + 19), ch(p + 27),
          stock(p + 28),      be::load32(p + 36), be::load32(p + 40), be::load32(p + 44),
          ch(p + 48),         ch(p + 49)};
}
IB_ALWAYS_INLINE Rpii decode_rpii(const std::uint8_t* p) noexcept {
  return {detail::header(p), detail::stock(p + 11), detail::ch(p + 19)};
}
IB_ALWAYS_INLINE DirectListingPriceDiscovery decode_dlcr(const std::uint8_t* p) noexcept {
  using namespace detail;
  return {header(p),          stock(p + 11),      ch(p + 19),         be::load32(p + 20),
          be::load32(p + 24), be::load32(p + 28), be::load64(p + 32), be::load32(p + 40),
          be::load32(p + 44)};
}

// --- Dispatch ------------------------------------------------------------------------

template <class H, class M>
IB_ALWAYS_INLINE void deliver(H& h, const M& m) {
  if constexpr (requires { h.on(m); }) h.on(m);
}

enum class DispatchResult : std::uint8_t { Ok, UnknownType, BadLength };

// Decode one payload of `len` bytes and invoke the handler.
template <class H>
IB_ALWAYS_INLINE DispatchResult dispatch(const std::uint8_t* p, std::size_t len, H& h) {
  const auto t = static_cast<unsigned char>(p[0]);
  const std::size_t expect = t < 128 ? kMessageLength[t] : 0;
  if (IB_UNLIKELY(expect == 0)) return DispatchResult::UnknownType;
  if (IB_UNLIKELY(len < expect)) return DispatchResult::BadLength;

  // Ordered by frequency in a typical trading day (adds, deletes, replaces, executes).
  switch (static_cast<MsgType>(t)) {
    case MsgType::AddOrder: deliver(h, decode_add(p, false)); break;
    case MsgType::OrderDelete: deliver(h, decode_delete(p)); break;
    case MsgType::OrderReplace: deliver(h, decode_replace(p)); break;
    case MsgType::OrderExecuted: deliver(h, decode_executed(p)); break;
    case MsgType::OrderCancel: deliver(h, decode_cancel(p)); break;
    case MsgType::AddOrderMpid: deliver(h, decode_add(p, true)); break;
    case MsgType::OrderExecutedWithPrice: deliver(h, decode_executed_price(p)); break;
    case MsgType::Trade: deliver(h, decode_trade(p)); break;
    case MsgType::Noii: deliver(h, decode_noii(p)); break;
    case MsgType::CrossTrade: deliver(h, decode_cross(p)); break;
    case MsgType::BrokenTrade: deliver(h, decode_broken(p)); break;
    case MsgType::SystemEvent: deliver(h, decode_system(p)); break;
    case MsgType::StockDirectory: deliver(h, decode_directory(p)); break;
    case MsgType::StockTradingAction: deliver(h, decode_trading_action(p)); break;
    case MsgType::RegShoRestriction: deliver(h, decode_reg_sho(p)); break;
    case MsgType::MarketParticipantPosition: deliver(h, decode_mpp(p)); break;
    case MsgType::MwcbDeclineLevel: deliver(h, decode_mwcb_decline(p)); break;
    case MsgType::MwcbStatus: deliver(h, decode_mwcb_status(p)); break;
    case MsgType::IpoQuotingPeriod: deliver(h, decode_ipo(p)); break;
    case MsgType::LuldAuctionCollar: deliver(h, decode_luld(p)); break;
    case MsgType::OperationalHalt: deliver(h, decode_op_halt(p)); break;
    case MsgType::Rpii: deliver(h, decode_rpii(p)); break;
    case MsgType::DirectListingPriceDiscovery: deliver(h, decode_dlcr(p)); break;
  }
  return DispatchResult::Ok;
}

struct ParseStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t unknown = 0;
  std::uint64_t bad_length = 0;
  std::array<std::uint64_t, 128> by_type{};

  void merge(const ParseStats& o) noexcept {
    messages += o.messages;
    bytes += o.bytes;
    unknown += o.unknown;
    bad_length += o.bad_length;
    for (std::size_t i = 0; i < by_type.size(); ++i) by_type[i] += o.by_type[i];
  }
};

// Parse every complete frame in `buf`. Returns the number of bytes consumed; a trailing
// partial frame is left for the caller (streaming readers carry it into the next chunk).
template <class H>
std::size_t parse_frames(std::span<const std::uint8_t> buf, H& h, ParseStats& stats,
                         std::uint64_t max_messages = ~std::uint64_t{0}) {
  const std::uint8_t* p = buf.data();
  const std::uint8_t* const end = p + buf.size();
  std::uint64_t n = 0;
  while (end - p >= 2 && n < max_messages) {
    const std::size_t len = be::load16(p);
    if (IB_UNLIKELY(static_cast<std::size_t>(end - p) < len + 2)) break;
    const std::uint8_t* msg = p + 2;
    if (len > 0) {
      ++stats.by_type[msg[0] & 0x7F];
      const DispatchResult r = dispatch(msg, len, h);
      if (IB_UNLIKELY(r != DispatchResult::Ok)) {
        if (r == DispatchResult::UnknownType) ++stats.unknown;
        else ++stats.bad_length;
      }
    }
    p += len + 2;
    ++n;
  }
  const auto consumed = static_cast<std::size_t>(p - buf.data());
  stats.messages += n;
  stats.bytes += consumed;
  return consumed;
}

// Lightweight frame iterator for tools that need raw message bytes (slicing, filtering).
class FrameCursor {
 public:
  explicit FrameCursor(std::span<const std::uint8_t> buf) : p_(buf.data()), end_(p_ + buf.size()) {}

  // Returns the payload of the next frame or an empty span at end of buffer.
  std::span<const std::uint8_t> next() noexcept {
    if (end_ - p_ < 2) return {};
    const std::size_t len = be::load16(p_);
    if (static_cast<std::size_t>(end_ - p_) < len + 2) return {};
    std::span<const std::uint8_t> out{p_ + 2, len};
    p_ += len + 2;
    return out;
  }
  // The full frame (length prefix + payload) most recently returned by next().
  const std::uint8_t* position() const noexcept { return p_; }

 private:
  const std::uint8_t* p_;
  const std::uint8_t* end_;
};

}  // namespace ib::itch
