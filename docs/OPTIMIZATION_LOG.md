# Optimization log

Each entry gives the hypothesis, the change, and before/after numbers measured on real data.
Negative results stay in, because they decided what *not* to build.

## Methodology

- **Corpus.** The first 40M messages of NASDAQ TotalView-ITCH 5.0, 2019-12-30. That's 1.26 GB,
  running 04:00–10:00:25, so it includes the open, and it reaches ~1.7M live orders, close to the
  day's 1.92M peak. `ib_bench --load --mlock` copies it into a wired buffer, so runs never touch
  the page cache.
- **Host.** Apple M5 (Mac17,2), 16 GB, macOS 26.5, Apple clang 21, `-O3 -mcpu=apple-m4`.
  The machine was shared with other heavy workloads during every measurement (an 8 GB VM,
  multi-GB Python jobs, swap at 20+ GB). Run-to-run drift of ±20–30% was normal.
- **Interleaving.** Because of that drift, variants are never compared across separate sessions.
  `scripts/run_benchmarks.sh` runs every variant round-robin for R rounds. A/B tests run
  interleaved too, against saved binaries in `build/ab/`. Tables report **best** (closest to
  uncontended capability) and **median**.
- **Timing.** Throughput comes from `steady_clock` around the whole replay. Per-operation
  latency uses `CycleClock`: `rdtsc` on x86, `mach_absolute_time` on Apple silicon. The latter
  ticks at 24 MHz, so p50 values on Apple silicon are quantized to 41.67 ns.
- **Raw results.** Everything is in `bench/results/*.jsonl`; `scripts/summarize_bench.py`
  rebuilds each table.

## Summary (final code, 40M-message corpus, 5 interleaved rounds)

| variant | what changed | best (M msg/s) | median | ns/msg | vs naive |
|---|---|---:|---:|---:|---:|
| `parse` | decode only (no book): upper bound | 160.12 | 137.26 | 6.2 | — |
| `naive` | std::map levels + std::list FIFO + std::unordered_map | 2.44 | 2.07 | 410.2 | 1.0x |
| `std-index` | pooled 32-B orders, intrusive FIFO, SoA ladder; std::unordered_map index | 5.37 | 4.57 | 186.1 | 2.2x |
| `flat-murmur-linear` | + open-addressing index (murmur, linear probing, 4M slots) | 8.61 | 8.47 | 116.1 | 3.5x |
| `flat-ident-linear` | identity hash, 16M slots, linear probing | 10.95 | 10.30 | 91.3 | 4.5x |
| `flat-murmur` | murmur, Robin Hood, 4M slots | 8.65 | 8.19 | 115.6 | 3.5x |
| `flat-ident-4M` | identity, Robin Hood, 4M slots (clustering pathology) | 3.04 | 2.82 | 328.8 | 1.2x |
| `flat-ident` | identity, Robin Hood, 16M slots (default) | 11.01 | 10.91 | 90.8 | 4.5x |
| `batched-ident` | + decode-ahead batches, staged prefetch (index, order, level, book, ladder) | 16.74 | 14.82 | 59.8 | 6.9x |
| `pipeline-ident` | + parser thread → SPSC ring → book thread | 18.44 | 16.39 | 54.2 | 7.6x |

Section numbers below come from the A/B session in which each change was made. Absolute
throughput drifts between sessions on this shared host, so compare ratios within a table, not
across tables.

---

## 0. The build was producing x86-64 binaries under Rosetta

**Symptom.** The first flamegraph attributed 51% of samples to `Rosetta Runtime Routines`.

**Cause.** CMake and Ninja came from Intel Homebrew (`/usr/local`), so they run under Rosetta,
and the `clang` they spawn inherits the x86-64 default. Every binary, plus the Homebrew
GoogleTest and Benchmark libraries, was x86-64 and silently translated. Two more traps sat
behind that one:

- `-mcpu=native` resolved by a translated clang reports `westmere` for an arm64 target.
- Apple clang searches `/usr/local/include` **before** `-isystem` directories, so the x86
  Homebrew gtest 1.18 headers shadowed the fetched 1.15 ones.

**Fix.** CMake now detects `hw.optional.arm64` and forces `CMAKE_OSX_ARCHITECTURES=arm64`. It
resolves the native CPU by running clang under `arch -arm64`; that gives `apple-m4`, clang 21's
newest Apple target and ISA-compatible with the M5. GoogleTest and Benchmark are fetched and
built for the target architecture, with their include directories exported as plain `-I`.

**Effect.** Decoding went from ~100–135M to 150–175M msg/s. Book replay barely moved, which was
the first hint that the book is bound by memory latency rather than instructions.

## 1. Baseline → pooled, intrusive, cache-sized book

The baseline `NaiveMarket` is `std::map<Price, Level>` per side, `std::list<Order>` per level,
and `std::unordered_map` from order id to list iterator: three heap nodes per order and pointer
chasing on every operation.

The replacement:

- `Order` and `Level` are 32-byte PODs in `IndexPool`s: one `mmap` reservation each, LIFO free
  list, zero `malloc` on the hot path.
- FIFO queues are intrusive, linked with 32-bit pool handles instead of pointers. Halving the
  link size is what keeps an `Order` at 32 bytes, two per 64-byte line.
- Price levels live in a sorted structure-of-arrays ladder with the **best price at the back**.
  The touch is always the last element, so levels created or destroyed near it move almost
  nothing.
- Cancel, delete, and execute are O(1): order handle → level handle, with no search.

| | best (M msg/s) | median |
|---|---:|---:|
| `naive` | 2.80 | 2.64 |
| pooled intrusive book, still `std::unordered_map` index | **6.60** (2.4x) | 6.56 |

## 2. Open-addressing order-id index

`std::unordered_map` costs a node allocation per insert and a dependent pointer load per lookup.
`FlatHashMap` stores 16-byte slots inline in one flat array, with linear probing, a 0.5 max load
factor, and no tombstones.

| index churn microbenchmark (insert + lookup + erase per op) | 65K live | 2M live |
|---|---:|---:|
| `std::unordered_map` | 31 ns | 237–273 ns |
| `FlatHashMap<Murmur3Mix>` | 34 ns | 57 ns |

| real-data replay | best | median |
|---|---:|---:|
| `std::unordered_map` index | 6.60 | 6.56 |
| `FlatHashMap`, murmur3 finalizer | **10.8** (1.6x) | 10.0 |

## 3. Identity hashing, and the clustering trap

**Hypothesis.** NASDAQ assigns order refs from one increasing sequence, and most orders die
within seconds of being added. A ref therefore carries temporal locality: deletes hit refs near
recent adds. `slot = ref & mask` turns that temporal locality into spatial locality.

**Result.** It works only at a low load factor:

| index | slots | load at 1.7M live | best | median | diagnostics |
|---|---:|---:|---:|---:|---|
| murmur | 4M | 0.40 | 10.8 | 10.0 | longest cluster 33 |
| identity | 4M | 0.40 | **3.36** | 3.31 | longest cluster **24,008**, mean probe 6.7 |
| identity | 16M | 0.10 | **14.3** | 13.8 | longest cluster 101, mean probe 1.02 |

At load 0.4, the advancing window of new sequential refs collides with long-lived survivor
orders from earlier laps around the table. Each collision shifts every following key by one
slot, so displacements accumulate into clusters tens of thousands of slots long. Linear probing
then has to scan them on inserts, misses, and deletes. The `longest_cluster()` diagnostic in
`FlatHashMap` made this visible.

**Decision.** Identity hashing over 16M slots (256 MB) is the default for feed replay, at +32%
over murmur. The matching engine, whose client ids carry no ordering guarantee, uses murmur.
`XorFoldHash` (`ref ^ ref >> bits`) was also tried; it is identical to identity at both sizes,
because the problem is density, not aliasing.

## 4. Robin Hood probing

A synthetic sliding window of sequential keys exposed the worst case of plain linear probing.
Backward-shift deletion must scan to the end of the cluster, because any later entry might
belong in the hole, and a dense run of sequential keys is one long cluster.

Robin Hood insertion keeps probe distances non-decreasing along every cluster. Deletion can then
stop at the first entry already sitting at its home slot, and a miss can stop at the first
entry closer to home than itself. The probe distance is stored in what was padding, so neither
operation rehashes the keys it walks past.

| churn microbenchmark | linear probing | Robin Hood |
|---|---:|---:|
| identity, 4K live | 3.3 µs | — |
| identity, 65K live | **52 µs** | **5.4 ns** (~10,000x) |
| identity, 2M live | 1.65 ms | 27 ns |
| murmur, 2M live | 54 ns | 57 ns |

On the real feed, at the operating points from §3, the two are at parity. A/B best of 5: identity
11.87 vs 12.42M (linear vs Robin Hood); murmur 7.95 vs 9.16M. Robin Hood was kept because it
removes a pathological case at no real-data cost.

## 5. Decode-ahead batches with two-stage software prefetch

The remaining cost is dependent DRAM misses (index slot → order → level) on orders added minutes
or hours earlier. The whole feed is known in advance, so `batched_replay` decodes 256 messages
into 32-byte `BookEvent`s first, then applies them while prefetching ahead:

- **Stage 1**, event i+16: prefetch the index slot.
- **Stage 2**, event i+8: that slot is now cached, so look up the order handle and prefetch the
  `Order` and its `Level`.

This turns serialized misses into overlapping ones (memory-level parallelism) without changing
results.

| | best | median |
|---|---:|---:|
| flat, identity | 14.3 | 13.8 |
| batched + two-stage prefetch | **17.8** (+25%) | 16.3 |

A phase-split timer (`ib_bench --split`) shows decode at 7–8 ns/msg and apply at ~70 ns/msg.
The book is ~90% of the cost.

## 6. Price-ladder search: vectorizing was neutral, prefetching was not

With `--mlock`, the profile showed `PriceLadder::upper`/`find`/`erase` at 27–36% inclusive. A
measurement program (`ladder_dist`) over the 40M corpus gave the access pattern:

- 81% of adds and 66% of level removals land within 8 levels of the touch.
- The tail is long: mean side depth 157, max 3,694.
- 35% of adds create a level; 39% of removals empty one.

The first attempt replaced the "8 scalar steps, then binary search" scan with a branch-free count
over 16-key blocks from the touch, which the compiler vectorizes, falling back to a branchless
binary search after 64 levels. It is verified against `std::upper_bound` for every size 0–300.

The second hypothesis was that the cost is cache misses on the ladder arrays themselves, since
each message touches a random symbol's book. Every event carries its stock locate, so stage 1
also prefetches the `OrderBook` and stage 2 prefetches the touch end of both ladders.

Four-way interleaved A/B on batched replay, 6 rounds:

| variant | best | median |
|---|---:|---:|
| scalar scan (before) | 15.6 | 13.1 |
| vectorized block scan | 15.9 | 14.3 |
| + O(1) touch fast path | 14.6 | 14.0 |
| **+ book/ladder prefetch stage** | **17.8** | **16.7** (+19% over the previous row) |

The search algorithm was never the bottleneck; waiting for the ladder's cache lines was. The
vectorized scan is kept because it is strictly better for deep books (64+ levels: 52 → 49 ns,
190 levels: 97 → 88 ns in the microbenchmark), but the win came from prefetching.

## 7. Two-thread pipeline and ring sizing

The parser thread decodes into a lock-free SPSC ring of 32-byte events. It writes directly into
reserved slots and publishes a batch with a single release-store. The book thread applies them
with the same staged prefetch. Cached peer indices and 128-byte padding keep cross-core traffic
on the indices near zero.

SPSC microbenchmark: 28M items/s pushing one at a time, 172M with batches of 16, **323M** with
batches of 256. Cross-core ping-pong round trip: 184 ns.

The parser is ~8x faster than the book, so in throughput mode the ring sits full, and the
producer→consumer latency is simply queue depth × book service time. Shrinking the ring cuts
latency by orders of magnitude at almost no throughput cost:

| ring slots | throughput | p50 latency | p99 | p99.9 |
|---:|---:|---:|---:|---:|
| 65,536 | 16.8M msg/s | 3.58 ms | 12.6 ms | 28.3 ms |
| 4,096 | 18.6M | 208 µs | 475 µs | 821 µs |
| **256** | 17.8M | **13.5 µs** | 35 µs | 67 µs |
| 64 | 15.9M | 3.6 µs | 12 µs | 29 µs |

## 8. LTO and PGO: no gain (negative result)

`scripts/pgo.sh` builds an instrumented binary, trains it on the first 10M messages in every
replay mode, merges the profile, and rebuilds with `-fprofile-instr-use` and LTO.

| 5 interleaved rounds | flat best / median | batched best / median |
|---|---:|---:|
| release | 11.7 / 11.0 | 15.7 / 13.4 |
| release + LTO | 10.7 / 8.5 | 14.2 / 11.1 |
| release + PGO + LTO | 11.0 / 9.2 | 14.9 / 12.8 |

Neither helps, which is consistent with everything above. The hot path is header-only templates,
already fully inlined into one translation unit, so LTO has nothing to cross. The bottleneck is
memory latency, which block layout and branch hints don't address.

## 9. Measurement pitfalls found along the way

1. **`sample(1)` cannot capture running threads.** On macOS 26 without elevated privileges it
   only returns stacks while the target is blocked, so a compute loop produces an empty call
   graph. `SamplingProfiler` replaces it: a `SIGPROF` interval timer, an async-signal-safe
   frame-pointer walk from the `ucontext`, and offline symbolization with `atos -i` to recover
   inlined frames. `scripts/flamegraph_macos.sh` drives it end to end.
2. **`cntvct_el0` is unusable from userspace on this M5 / macOS 26.** It reported a 1 GHz
   frequency but returned the same value for 999,999 of 1,000,000 consecutive reads, then jumped,
   sometimes backwards. Latency histograms built on it were garbage. `CycleClock` uses
   `mach_absolute_time` on Apple silicon: monotonic, ~4 ns per read, 41.67 ns resolution.
3. **Profiling an un-`mlock`ed buffer on a memory-starved host misattributes cost.** Without
   `--mlock`, ~40% of samples landed on big-endian loads in the decoders: the 1.26 GB input was
   being compressed and faulted back in by the OS. Phase timers (§5) and a `--mlock`ed
   re-profile put decode back at ~10%.
4. **Prefaulting the whole 8.25 GB file** on a 16 GB host thrashed the page cache and halved
   throughput. `--load` copies exactly the benchmarked prefix instead.
5. **Single-threaded runs need a QoS class.** Without `QOS_CLASS_USER_INTERACTIVE`, macOS may
   schedule a busy benchmark thread on an efficiency core under load. All tools now request it
   (`pin_current_thread`; on Linux it pins to a CPU).
6. **Unfair baselines flatter results.** The first reference matching engine scanned a level's
   list to cancel and ran at 113K ops/s, which would have made a "64x speedup". With O(1)
   cancels through stored list iterators it runs at 21M ops/s; the honest gap is 2.3x (§10).

## 10. Matching engine

Synthetic flow: 10M operations around a random-walking mid, including passive and aggressive
limits, market, IOC, FOK, post-only, cancels, and amends. Five interleaved runs, without
per-operation timing:

| engine | median ops/s | |
|---|---:|---|
| `ReferenceEngine` (`std::map` + `std::list` + `unordered_map`, O(1) cancel) | 21.0M | |
| `MatchingEngine` (pooled book, Robin Hood murmur index) | **48.2M** | 2.3x |

Sizing matters here as in §3. An index sized for 4M live orders (128 MB) against a flow that
keeps ~114K live dropped the engine to the reference's speed. Sized to the workload (256K), its
table is cache-resident.
