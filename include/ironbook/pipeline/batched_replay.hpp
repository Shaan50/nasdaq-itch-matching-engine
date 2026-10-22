#pragma once

// Single-threaded replay with software pipelining.
//
// Book updates are dominated by dependent DRAM misses (order-id index slot -> order ->
// level) for orders added minutes or hours earlier. Because the whole feed is known in
// advance, we can decode a batch of messages first and then, while applying event i,
// issue prefetches for the events ahead of it:
//
//   event i + d1 : prefetch the index slot for its order ref           (stage 1)
//   event i + d2 : index slot is now cached -> look up the order handle
//                  and prefetch the Order and its Level                (stage 2)
//   event i      : apply; its lines are (ideally) already in L1/L2
//
// This converts a chain of serialized misses into many overlapping ones (memory-level
// parallelism), without changing any results.

#include <cstdint>
#include <span>
#include <vector>

#include "ironbook/common/timing.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/pipeline/events.hpp"

namespace ib {

// Also prefetch the symbol's OrderBook and the touch end of its price ladders (every
// event carries its stock locate, so the book is known before the order is read).
#ifndef IRONBOOK_PREFETCH_LADDERS
#define IRONBOOK_PREFETCH_LADDERS 1
#endif
inline constexpr bool kPrefetchLadders = IRONBOOK_PREFETCH_LADDERS;

struct BatchConfig {
  std::size_t batch = 256;
  std::size_t d1 = 16;  // 0 disables prefetching entirely
  std::size_t d2 = 8;
  // Optional phase split (CycleClock ticks): time decoding vs. applying batches.
  std::uint64_t* decode_ticks = nullptr;
  std::uint64_t* apply_ticks = nullptr;
};

template <class MarketT>
IB_ALWAYS_INLINE void prefetch_stage1(const MarketT& m, const BookEvent& e) noexcept {
  if constexpr (requires { m.prefetch(e.ref); }) {
    if (e.type <= EventType::Replace) {
      m.prefetch(e.ref);
      if (e.type == EventType::Replace) m.prefetch(e.aux);
      if (kPrefetchLadders) m.prefetch_book(e.locate);
    }
  }
}

template <class MarketT>
IB_ALWAYS_INLINE void prefetch_stage2(const MarketT& m, const BookEvent& e) noexcept {
  if constexpr (requires { m.prefetch_order(e.ref); }) {
    if (e.type <= EventType::Replace) {
      if (e.type != EventType::Add) m.prefetch_order(e.ref);
      if (kPrefetchLadders) m.prefetch_ladders(e.locate);
    }
  }
}

// Applies a decoded batch with staged prefetching.
template <class FH>
IB_ALWAYS_INLINE void apply_batch(const BookEvent* ev, std::size_t n, FH& fh, const BatchConfig& cfg) {
  auto& m = fh.market();
  const std::size_t d1 = cfg.d1, d2 = cfg.d2;
  if (d1 == 0) {
    for (std::size_t i = 0; i < n; ++i) apply_event(ev[i], fh);
    return;
  }
  // Prime the pipeline for the head of the batch.
  for (std::size_t i = 0; i < d1 && i < n; ++i) prefetch_stage1(m, ev[i]);
  for (std::size_t i = 0; i < d2 && i < n; ++i) prefetch_stage2(m, ev[i]);
  for (std::size_t i = 0; i < n; ++i) {
    if (i + d1 < n) prefetch_stage1(m, ev[i + d1]);
    if (d2 && i + d2 < n) prefetch_stage2(m, ev[i + d2]);
    apply_event(ev[i], fh);
  }
}

namespace detail {
struct ArraySink {
  BookEvent* buf;
  std::size_t n = 0;
  IB_ALWAYS_INLINE BookEvent& next() noexcept { return buf[n++]; }
};
}  // namespace detail

template <class FH>
itch::ParseStats batched_replay(std::span<const std::uint8_t> data, FH& fh, const BatchConfig& cfg = {},
                                std::uint64_t max_messages = ~std::uint64_t{0}) {
  itch::ParseStats st;
  std::vector<BookEvent> buf(cfg.batch);
  detail::ArraySink sink{buf.data()};
  Normalizer<detail::ArraySink> norm(sink);
  std::size_t off = 0;
  while (off < data.size() && st.messages < max_messages) {
    sink.n = 0;
    const std::uint64_t want = std::min<std::uint64_t>(cfg.batch, max_messages - st.messages);
    const std::uint64_t t0 = cfg.decode_ticks ? CycleClock::now() : 0;
    const std::size_t used = itch::parse_frames(data.subspan(off), norm, st, want);
    if (used == 0) break;
    off += used;
    const std::uint64_t t1 = cfg.decode_ticks ? CycleClock::now() : 0;
    apply_batch(buf.data(), sink.n, fh, cfg);
    if (cfg.decode_ticks) {
      *cfg.decode_ticks += t1 - t0;
      *cfg.apply_ticks += CycleClock::now() - t1;
    }
  }
  return st;
}

}  // namespace ib
