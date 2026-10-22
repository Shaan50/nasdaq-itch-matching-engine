#pragma once

// Normalized, fixed-size book event passed from the parser thread to the book thread.
//
// ITCH messages are 12-50 bytes, big-endian, and variable-length. The producer decodes
// and byte-swaps them into this 32-byte host-endian record (4 per 128 B Apple cache
// line, 2 per 64 B x86 line), so the consumer does no decoding at all and the parsing
// cost is moved entirely onto the other core.

#include <cstdint>
#include <cstring>

#include "ironbook/common/compiler.hpp"
#include "ironbook/itch/messages.hpp"

namespace ib {

enum class EventType : std::uint8_t {
  Add,
  Execute,
  ExecutePrice,
  Cancel,
  Delete,
  Replace,
  Trade,
  Cross,
  System,
  TradingAction,
  Directory,
  End,  // sentinel: producer finished
};

struct alignas(32) BookEvent {
  std::uint64_t ref;  // order ref | Cross: shares | Directory: 8 symbol bytes
  std::uint64_t aux;  // Replace: new ref | otherwise: timestamp
  Price price;
  Qty qty;
  Locate locate;
  EventType type;
  std::uint8_t flags;  // Add/Trade: side | ExecutePrice: printable | System/Action: code
};
static_assert(sizeof(BookEvent) == 32);

namespace detail {
IB_ALWAYS_INLINE std::uint64_t pack_stock(const itch::Stock& s) noexcept {
  std::uint64_t v;
  std::memcpy(&v, s.data(), 8);
  return v;
}
IB_ALWAYS_INLINE itch::Stock unpack_stock(std::uint64_t v) noexcept {
  itch::Stock s;
  std::memcpy(s.data(), &v, 8);
  return s;
}
}  // namespace detail

// ITCH handler that converts messages into BookEvents and hands them to `Sink`, which
// must provide `BookEvent& next()` returning a slot to fill.
template <class Sink>
class Normalizer {
 public:
  explicit Normalizer(Sink& sink) : sink_(sink) {}

  IB_ALWAYS_INLINE void on(const itch::AddOrder& m) {
    emit(EventType::Add, m.h, m.ref, m.h.timestamp, m.price, m.shares,
         static_cast<std::uint8_t>(m.side));
  }
  IB_ALWAYS_INLINE void on(const itch::OrderExecuted& m) {
    emit(EventType::Execute, m.h, m.ref, m.h.timestamp, 0, m.shares, 0);
  }
  IB_ALWAYS_INLINE void on(const itch::OrderExecutedWithPrice& m) {
    emit(EventType::ExecutePrice, m.h, m.ref, m.h.timestamp, m.price, m.shares, m.printable);
  }
  IB_ALWAYS_INLINE void on(const itch::OrderCancel& m) {
    emit(EventType::Cancel, m.h, m.ref, m.h.timestamp, 0, m.shares, 0);
  }
  IB_ALWAYS_INLINE void on(const itch::OrderDelete& m) {
    emit(EventType::Delete, m.h, m.ref, m.h.timestamp, 0, 0, 0);
  }
  IB_ALWAYS_INLINE void on(const itch::OrderReplace& m) {
    emit(EventType::Replace, m.h, m.orig_ref, m.new_ref, m.price, m.shares, 0);
  }
  IB_ALWAYS_INLINE void on(const itch::Trade& m) {
    emit(EventType::Trade, m.h, m.ref, m.h.timestamp, m.price, m.shares,
         static_cast<std::uint8_t>(m.side));
  }
  IB_ALWAYS_INLINE void on(const itch::CrossTrade& m) {
    emit(EventType::Cross, m.h, m.shares, m.h.timestamp, m.price, 0,
         static_cast<std::uint8_t>(m.cross_type));
  }
  IB_ALWAYS_INLINE void on(const itch::SystemEvent& m) {
    emit(EventType::System, m.h, 0, m.h.timestamp, 0, 0, static_cast<std::uint8_t>(m.event_code));
  }
  IB_ALWAYS_INLINE void on(const itch::StockTradingAction& m) {
    emit(EventType::TradingAction, m.h, detail::pack_stock(m.stock), m.h.timestamp, 0, 0,
         static_cast<std::uint8_t>(m.trading_state));
  }
  IB_ALWAYS_INLINE void on(const itch::StockDirectory& m) {
    emit(EventType::Directory, m.h, detail::pack_stock(m.stock), m.h.timestamp, 0,
         m.round_lot_size, 0);
  }

 private:
  IB_ALWAYS_INLINE void emit(EventType t, const itch::Header& h, std::uint64_t ref,
                             std::uint64_t aux, Price px, Qty qty, std::uint8_t flags) {
    BookEvent& e = sink_.next();
    e.ref = ref;
    e.aux = aux;
    e.price = px;
    e.qty = qty;
    e.locate = h.locate;
    e.type = t;
    e.flags = flags;
  }
  Sink& sink_;
};

// Re-materializes the ITCH message for an event and delivers it to handler `h` (normally
// a FeedHandler), so the single-threaded and pipelined paths share one implementation.
template <class H>
IB_ALWAYS_INLINE void apply_event(const BookEvent& e, H& h) {
  const itch::Header hdr{itch::MsgType::AddOrder, e.locate, 0, e.aux};
  switch (e.type) {
    case EventType::Add:
      h.on(itch::AddOrder{hdr, e.ref, static_cast<Side>(e.flags), e.qty, {}, e.price, {}, false});
      break;
    case EventType::Execute: h.on(itch::OrderExecuted{hdr, e.ref, e.qty, 0}); break;
    case EventType::ExecutePrice:
      h.on(itch::OrderExecutedWithPrice{hdr, e.ref, e.qty, 0, e.flags != 0, e.price});
      break;
    case EventType::Cancel: h.on(itch::OrderCancel{hdr, e.ref, e.qty}); break;
    case EventType::Delete: h.on(itch::OrderDelete{hdr, e.ref}); break;
    case EventType::Replace:
      h.on(itch::OrderReplace{{itch::MsgType::OrderReplace, e.locate, 0, 0}, e.ref, e.aux, e.qty,
                              e.price});
      break;
    case EventType::Trade:
      h.on(itch::Trade{hdr, e.ref, static_cast<Side>(e.flags), e.qty, {}, e.price, 0});
      break;
    case EventType::Cross:
      h.on(itch::CrossTrade{hdr, e.ref, {}, e.price, 0, static_cast<char>(e.flags)});
      break;
    case EventType::System: h.on(itch::SystemEvent{hdr, static_cast<char>(e.flags)}); break;
    case EventType::TradingAction:
      h.on(itch::StockTradingAction{hdr, detail::unpack_stock(e.ref), static_cast<char>(e.flags), {}});
      break;
    case EventType::Directory: {
      itch::StockDirectory d{};
      d.h = hdr;
      d.stock = detail::unpack_stock(e.ref);
      d.round_lot_size = e.qty;
      h.on(d);
      break;
    }
    case EventType::End: break;
  }
}

}  // namespace ib
