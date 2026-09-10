# OrderBook

[![CI](https://github.com/crick262004/OrderBook/actions/workflows/ci.yml/badge.svg)](https://github.com/crick262004/OrderBook/actions/workflows/ci.yml)

A limit order book / matching engine in modern C++23, built incrementally with a focus on
low-latency engineering: cache-friendly data structures, zero-allocation hot paths, and
lock-free concurrency.

## Features (growing commit by commit)

- Price-time (FIFO) priority matching
- Order types: GoodTillCancel, FillAndKill, FillOrKill, GoodForDay, Market
- Zero heap allocation on every hot path, measured per benchmark iteration (`allocs=0`)
- Dedicated matching thread behind two lock-free SPSC rings (commands in, trades out);
  the book itself is single-owner and has no locks or atomics

## Build

Requires CMake ≥ 3.28 and a C++23 compiler (Apple Clang 17+, GCC 14+, Clang 18+).

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/orderbook_app
```

Use the `debug` preset for development (also exports `compile_commands.json`).

## Tests

GoogleTest (fetched automatically by CMake) drives file-based scenarios: each
`OrderbookTest/TestFiles/*.txt` script replays add/modify/cancel actions against a fresh
book and asserts the exact trades produced and the final book state.

## Performance

Steady-state hot-path timings from `OrderbookBench/` (Google Benchmark, Release `-O3`,
Apple M5 Pro). Each cell is ns per operation pair at book depths 100 / 1,000 / 10,000;
one row per optimization commit, all measured on the same machine (±10% laptop tolerance).

| Commit | Change | add + cancel | match + replenish |
|---|---|---|---|
| `5dd83af` | Baseline: `shared_ptr` orders, `std::map` levels, hash-map order index | 67 / 69 / 72 | 199 / 208 / 246 |
| `5b18318` | Single ownership: orders by value in level nodes, no `shared_ptr` | 45 / 43 / 44 | 150 / 152 / 204 |
| `f6bf84b` | Arena: pooled orders, index handles, flat id→slot lookup | 23 / 25 / 26 | 134 / 136 / 148 |
| `927f6c4` | Flat levels: sorted price arrays with the touch at the back, aggregates on the level, no `std::map`/hash | 22 / 25 / 29 | 57 / 60 / 61 |
| `3a981ec` | Intrusive FIFO: orders are their own queue nodes, linked through pool slots; no `std::list`, zero per-order allocation | 14 / 15 / 21 | 34 / 37 / 40 |
| `a24ee1f` | Trade sink: fills go to a 16-byte `function_ref` callback instead of a returned `std::vector<Trade>`; the last allocation is gone | 13 / 15 / 23 | 23 / 28 / 34 |
| `563be41` | Padded ring counters: one cache line per SPSC counter plus a cached copy of the peer's, no false sharing (book untouched; see the threading table) | 12 / 15 / 19 | 23 / 30 / 32 |

The bench replaces global `operator new` and reports heap allocations per iteration as an
`allocs` counter. Since the trade sink it reads `allocs=0` on every benchmark: the book
allocates only at construction.

### Threading

`MatchingEngine` runs the book on its own thread; the rest of the program talks to it through
two single-producer/single-consumer rings — commands in, trades out. The book gained no
thread-aware code at all (single-writer principle); the boundary is where concurrency lives.

| Benchmark | What it measures | `a24ee1f` | padded counters |
|---|---|---|---|
| `BM_SpscPushPop` | one thread: push + front + pop on the ring (the acquire/release floor) | 1.8 | 2.0 |
| `BM_SpscPingPong` | two threads, two rings: one round trip = two cross-core hand-offs | 269 | **121** |
| `BM_SpscPingPong/packed` | the same code with all four ring counters on one cache line (A/B control) | — | 231 |
| `BM_SpscPingPongTail` | per-iteration timing of the round trip: p50 / p99 / max | — | 167 / 208 / ~20,000 |
| `BM_EngineRoundTrip/1000` | order submitted → trade read back out, through the engine, depth 1,000 | 285 | **161** |

The padded/packed pair is the false-sharing measurement: same code, same run, only the
layout differs, so the 110 ns gap is the tax of two writers sharing a cache line. The
~20 µs max in both layouts is OS preemption of an unpinned spinning thread — the number
thread pinning targets.

### Cache misses

Every number above is wall-clock; the claim behind them is "fewer cache misses". The
[`Cache misses`](.github/workflows/cache-miss.yml) workflow measures that directly:
`tools/cachestat.sh` rebuilds each optimization commit and runs the add+cancel and match
benchmarks under valgrind's cache simulator (`callgrind --cache-sim=yes`, a fixed 32 KB L1D /
8 MB LL geometry, misses counted only inside the benchmark function), reporting data-cache
misses per operation. A simulation is deterministic — the same binary gives the same count
to the digit — and runs on a CI VM whose hypervisor hides the hardware counters; what it
cannot say is how long a miss took, so it proves *fewer* while the ns table proves
*faster*. Results land in the workflow's job summary; the per-commit table is copied here
after each run.

| Commit | L1D misses / add+cancel | L1D misses / match | LL misses / match |
|---|---|---|---|
| _pending first Linux run_ | | | |

Pinning (`ScopedPin`, `MatchingEngine{capacity, core}`) is hard affinity on Linux, verified in
CI by asking the kernel which core the pinned thread runs on. macOS offers no hard affinity, so
on this machine the `/pinned` benchmark variants report `pinned=0` and match the unpinned
numbers; pinned latencies await a Linux run. A spin-loop hint (`isb`) was tried and measured at
+40 ns per round trip, so the wait loops stay tight.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
