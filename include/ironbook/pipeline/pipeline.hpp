#pragma once

// Two-thread replay pipeline:
//
//   [parser thread]  mmap'd ITCH bytes -> parse_frames -> Normalizer -> ring slots
//                                                                         │ SpscQueue<BookEvent>
//   [book thread]    peek() batch -> (prefetch index slots) -> FeedHandler -> Market
//
// The producer writes events straight into reserved ring slots and publishes them in
// batches (one release-store per batch). Optionally every K-th event is timestamped so
// the consumer can build a producer->consumer latency histogram; the stamp array is
// written before the batch's release-store, so the ring's acquire/release pairing also
// orders the stamp accesses (no extra synchronization, TSan-clean).

#include <atomic>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "ironbook/common/histogram.hpp"
#include "ironbook/common/timing.hpp"
#include "ironbook/concurrency/spsc_queue.hpp"
#include "ironbook/concurrency/thread_utils.hpp"
#include "ironbook/itch/parser.hpp"
#include "ironbook/pipeline/batched_replay.hpp"
#include "ironbook/pipeline/events.hpp"

namespace ib {

struct PipelineConfig {
  std::size_t queue_capacity = 1u << 16;  // events (32 B each -> 2 MiB ring)
  std::size_t batch = 256;                // events per publish
  std::uint64_t latency_sample_every = 0; // 0 = no latency sampling (power of two)
  std::size_t prefetch_d1 = 16;           // staged prefetch distances (0 = off),
  std::size_t prefetch_d2 = 8;            // see batched_replay.hpp
  bool pin_threads = true;
  int producer_cpu = 2;
  int consumer_cpu = 4;
};

struct PipelineStats {
  itch::ParseStats parse;
  std::uint64_t events = 0;
  std::uint64_t producer_full_spins = 0;
  std::uint64_t consumer_empty_spins = 0;
  double seconds = 0;
  LatencyHistogram latency;  // CycleClock ticks, producer emit -> consumer apply
};

namespace detail {

class RingWriter {
 public:
  RingWriter(SpscQueue<BookEvent>& q, std::size_t batch, std::uint64_t sample_every,
             std::uint64_t* stamps)
      : q_(q), batch_(batch), sample_mask_(sample_every ? sample_every - 1 : ~0ull),
        stamps_(stamps) {}

  IB_ALWAYS_INLINE BookEvent& next() {
    if (IB_UNLIKELY(avail_ == 0)) refill();
    --avail_;
    ++pending_;
    if (stamps_ && (seq_ & sample_mask_) == 0) stamps_[seq_ >> sample_shift()] = CycleClock::now();
    ++seq_;
    return *slot_++;
  }

  void finish() {
    BookEvent& e = next();
    e = BookEvent{};
    e.type = EventType::End;
    flush();
  }

  std::uint64_t full_spins() const noexcept { return full_spins_; }
  std::uint64_t emitted() const noexcept { return seq_; }

 private:
  unsigned sample_shift() const noexcept {
    return static_cast<unsigned>(std::countr_one(sample_mask_));
  }
  IB_ALWAYS_INLINE void flush() {
    if (pending_) {
      q_.commit(pending_);
      pending_ = 0;
    }
  }
  IB_NOINLINE void refill() {
    flush();
    std::size_t n;
    while ((n = q_.reserve(slot_)) == 0) {
      ++full_spins_;
      cpu_relax();
    }
    avail_ = n < batch_ ? n : batch_;
  }

  SpscQueue<BookEvent>& q_;
  std::size_t batch_;
  std::uint64_t sample_mask_;
  std::uint64_t* stamps_;
  BookEvent* slot_ = nullptr;
  std::size_t avail_ = 0;
  std::size_t pending_ = 0;
  std::uint64_t seq_ = 0;
  std::uint64_t full_spins_ = 0;
};

}  // namespace detail

// Runs the pipeline to completion over `data`, applying every event to handler `fh`
// (typically FeedHandler<Market, ...>) on a dedicated consumer thread.
template <class Handler>
PipelineStats run_pipeline(std::span<const std::uint8_t> data, Handler& fh,
                           const PipelineConfig& cfg = {},
                           std::uint64_t max_messages = ~std::uint64_t{0}) {
  PipelineStats st;
  SpscQueue<BookEvent> q(cfg.queue_capacity);

  const std::uint64_t sample_every = cfg.latency_sample_every;
  std::vector<std::uint64_t> stamps;
  if (sample_every) stamps.resize(data.size() / 12 / sample_every + 2);  // >= max messages
  std::uint64_t* stamp_ptr = sample_every ? stamps.data() : nullptr;
  const unsigned sample_shift = sample_every ? static_cast<unsigned>(std::countr_zero(sample_every)) : 0;

  std::atomic<bool> go{false};
  Stopwatch sw;

  std::thread producer([&] {
    set_thread_name("ib-parser");
    if (cfg.pin_threads) pin_current_thread(cfg.producer_cpu);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    detail::RingWriter writer(q, cfg.batch, sample_every, stamp_ptr);
    Normalizer<detail::RingWriter> norm(writer);
    itch::parse_frames(data, norm, st.parse, max_messages);
    writer.finish();
    st.producer_full_spins = writer.full_spins();
  });

  std::thread consumer([&] {
    set_thread_name("ib-book");
    if (cfg.pin_threads) pin_current_thread(cfg.consumer_cpu);
    go.store(true, std::memory_order_release);
    sw.reset();
    std::uint64_t seq = 0;
    const std::uint64_t mask = sample_every ? sample_every - 1 : ~0ull;
    const BatchConfig bc{0, cfg.prefetch_d1, cfg.prefetch_d2};
    for (;;) {
      const BookEvent* p;
      const std::size_t n = q.peek(p);
      if (n == 0) {
        ++st.consumer_empty_spins;
        cpu_relax();
        continue;
      }
      // End is the last event ever published, so it can only be the last in a chunk.
      const bool end = p[n - 1].type == EventType::End;
      const std::size_t m = end ? n - 1 : n;
      apply_batch(p, m, fh, bc);
      if (stamp_ptr) {
        const std::uint64_t now = CycleClock::now();
        for (std::uint64_t s = (seq + mask) & ~mask; s < seq + m; s += sample_every)
          st.latency.record(now - stamp_ptr[s >> sample_shift]);
      }
      seq += m;
      q.release(n);
      if (end) {
        st.events = seq;
        st.seconds = sw.elapsed_sec();
        return;
      }
    }
  });

  producer.join();
  consumer.join();
  return st;
}

}  // namespace ib
