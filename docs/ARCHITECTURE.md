# Architecture

```
                         ┌──────────────────────── parser thread ────────────────────────┐
  12302019.NASDAQ_ITCH50 │                                                               │
  (8.25 GB, mmap,        │  FrameCursor ──► dispatch<H>() ──► decode_*() ──► Normalizer   │
   MADV_SEQUENTIAL)  ───►│  2-byte BE len   switch on type    bswap loads    32-B event  │
   or .gz (zlib stream)  │  zero-copy       (static bind)     on the stack   in ring slot│
                         └───────────────────────────────────────────┬───────────────────┘
                                                                     │ SpscQueue<BookEvent>
                                                                     │ reserve()/commit() batches,
                                                                     │ cached head/tail, 128-B padded
                         ┌──────────────────────── book thread ──────▼───────────────────┐
                         │  peek() batch ─► staged prefetch (slot i+16, order i+8)        │
                         │      └► apply_event ─► FeedHandler<Market, Observer>          │
                         │                         │                │                    │
                         │              ┌──────────┘                └─► FeedValidator     │
                         │              ▼                                (optional, static)│
                         │   Market (all 8,906 symbols)                                  │
                         │   ├─ FlatHashMap<order ref → u32 handle>  Robin Hood, 16-B slot│
                         │   ├─ IndexPool<Order> 32 B  ─ intrusive FIFO via u32 prev/next │
                         │   ├─ IndexPool<Level> 32 B  ─ qty/count/head/tail             │
                         │   └─ OrderBook[65536 by locate]                               │
                         │        └─ PriceLadder ×2: sorted SoA {key[], level[]},        │
                         │           best price at the back (key = px | ~px)             │
                         └───────────────────────────────────────────────────────────────┘

  MatchingEngine<Listener> (synthetic orders) reuses OrderBook / pools / FlatHashMap:
    submit(Limit|Market|IOC|FOK|PostOnly) ─► match against opposite ladder (price-time)
      ─► Fill events ─► rest remainder (Limit/PostOnly) or cancel (IOC/Market)
    cancel / modify (reduce keeps priority; reprice = cancel-replace, may trade)
    self-trade prevention: CancelResting | CancelIncoming
```

## Hot-path memory layout

| Structure | Size | Notes |
|---|---|---|
| `Order` | 32 B | id, qty, level handle, prev/next handles, owner, locate, side. Two per 64-B line; price lives in the `Level` |
| `Level` | 32 B | total qty, price, count, FIFO head/tail |
| index slot | 16 B | key, value handle, Robin Hood probe distance (in what would be padding) |
| `BookEvent` | 32 B | normalized message passed between threads; four per 128-B Apple cache line |
| `PriceLadder` | 2 × `vector<u32>` per side | keys searched linearly from the touch for 8 steps, then binary search |

All handles are 32-bit pool indices (0 = nil). Halving link size relative to pointers keeps an
`Order` at 32 bytes. Pools live in one `mmap` reservation each: they never reallocate or move,
pages are committed on first touch (or prefaulted / `mlock`ed on request), and freed slots are
reused LIFO so the next allocation is cache-hot.

## Correctness strategy

1. **Unit tests** for every component (GoogleTest, 100+ tests).
2. **Differential testing.** `Market` against `NaiveMarket` (std::map/list) on synthetic feeds and
   real NASDAQ slices: every touched symbol's top of book after every message, plus an
   order-sensitive digest of every full book. `MatchingEngine` against `ReferenceEngine`: identical
   event streams over 150K random operations for each STP mode.
3. **Feed-consistency validation** (`FeedValidator`). The day's own execution messages are the
   ground truth: zero unknown refs, zero overfills, every continuous-session `E` execution at the
   best price level, FIFO-head rate, crossed-book checks.
4. **Sanitizers.** ASan+UBSan (`debug` preset) and TSan (`tsan` preset, SPSC and pipeline stress).
5. **Pipeline equivalence.** The two-thread pipeline must produce byte-identical book digests to
   the single-threaded replay, including with 16-slot queues that force constant wrap-around.
