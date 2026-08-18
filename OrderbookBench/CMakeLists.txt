# Google Benchmark via FetchContent, mirroring the gtest setup in OrderbookTest.
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(orderbook_bench bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook orderbook_warnings benchmark::benchmark_main)
