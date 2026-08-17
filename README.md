# OrderBook

[![CI](https://github.com/crick262004/OrderBook/actions/workflows/ci.yml/badge.svg)](https://github.com/crick262004/OrderBook/actions/workflows/ci.yml)

A limit order book / matching engine in modern C++23, built incrementally with a focus on
low-latency engineering: cache-friendly data structures, zero-allocation hot paths, and
lock-free concurrency.

## Features (growing commit by commit)

- Price-time (FIFO) priority matching
- Order types: GoodTillCancel, FillAndKill, FillOrKill, GoodForDay, Market

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

## License

MIT — see [LICENSE.txt](LICENSE.txt).
