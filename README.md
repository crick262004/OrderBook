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
| `TBD` | Trade sink: fills go to a 16-byte `function_ref` callback instead of a returned `std::vector<Trade>`; the last allocation is gone | 13 / 15 / 23 | 23 / 28 / 34 |

The bench replaces global `operator new` and reports heap allocations per iteration as an
`allocs` counter. Since the trade sink it reads `allocs=0` on every benchmark: the book
allocates only at construction.

### Threading

The same commit adds `MatchingEngine`: the book runs on its own thread and the rest of the
program talks to it through two single-producer/single-consumer rings — commands in, trades
out. The book gained no thread-aware code at all (single-writer principle); the boundary is
where concurrency lives. Instruments for the upcoming pinning and cache-line-alignment
commits (unpinned, ring counters sharing a cache line):

| Benchmark | What it measures | ns |
|---|---|---|
| `BM_SpscPushPop` | one thread: push + front + pop on the ring (the acquire/release floor) | 1.8 |
| `BM_SpscPingPong` | two threads, two rings: one round trip = two cross-core hand-offs | 269 |
| `BM_EngineRoundTrip/1000` | order submitted → trade read back out, through the engine, depth 1,000 | 285 |

## License

MIT — see [LICENSE.txt](LICENSE.txt).
