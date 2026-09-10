#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <benchmark/benchmark.h>

#include "Order.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "Side.h"
#include "Usings.h"

// Steady-state hot-path baselines. Every measured iteration leaves the
// book at its starting depth, so growth of the containers never pollutes the
// per-iteration average. Order construction stays inside the timed loop: since
// 1.1 it is a stack value, since 1.2 it lands in the arena, since 2.1 the price
// levels are contiguous, and since 2.2 the level FIFO is intrusive — no per-order
// heap allocation remains; the Trades vector of a crossing call is the only one.
// The `allocs` counter below proves that claim per iteration instead of asserting it.

namespace
{

// Heap allocations observed through the replaced global operator new (this
// binary only). Atomic so the replacement stays correct if the harness ever
// allocates from another thread; the timed loops themselves are single-threaded.
std::atomic<std::uint64_t> allocationCount{0};

} // namespace

void *operator new(std::size_t size)
{
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept
{
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{

// Report heap allocations per iteration alongside the timings: 0 means the
// path is allocation-free.
class AllocationScope
{
public:
    explicit AllocationScope(benchmark::State &state)
        : state_{state}, before_{allocationCount.load(std::memory_order_relaxed)}
    {
    }

    ~AllocationScope()
    {
        const auto count = allocationCount.load(std::memory_order_relaxed) - before_;
        state_.counters["allocs"] = benchmark::Counter(static_cast<double>(count), benchmark::Counter::kAvgIterations);
    }

    AllocationScope(const AllocationScope &) = delete;
    AllocationScope &operator=(const AllocationScope &) = delete;

private:
    benchmark::State &state_;
    std::uint64_t before_;
};

constexpr Quantity RestingQuantity = 10;

// N resting bids at distinct prices 1..N, id == price. No asks, so nothing matches.
void FillBids(Orderbook &book, std::int64_t depth)
{
    for (std::int64_t i = 1; i <= depth; ++i)
    {
        book.AddOrder(Order{OrderType::GoodTillCancel, static_cast<OrderId>(i), Side::Buy, static_cast<Price>(i),
                            RestingQuantity});
    }
}

// Non-crossing add + cancel at a mid-book level: book size oscillates N <-> N+1.
void BM_AddCancel(benchmark::State &state)
{
    const auto depth = state.range(0);
    Orderbook book;
    FillBids(book, depth);

    const auto price = static_cast<Price>(depth / 2);
    const auto orderId = static_cast<OrderId>(depth + 1);

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        auto trades = book.AddOrder(Order{OrderType::GoodTillCancel, orderId, Side::Buy, price, RestingQuantity});
        benchmark::DoNotOptimize(trades);
        book.CancelOrder(orderId);
    }
}

// Crossing sell fully fills the best bid (one trade), then the bid is replenished:
// measures the full match pipeline at constant depth. Expect allocs == 1: the
// returned Trades vector (roadmap 2.3 moves trade output off the hot path).
void BM_AddMatch(benchmark::State &state)
{
    const auto depth = state.range(0);
    Orderbook book;
    FillBids(book, depth);

    const auto topPrice = static_cast<Price>(depth);
    const auto bidId = static_cast<OrderId>(depth);
    const auto askId = static_cast<OrderId>(depth + 1);

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        auto trades = book.AddOrder(Order{OrderType::GoodTillCancel, askId, Side::Sell, topPrice, RestingQuantity});
        benchmark::DoNotOptimize(trades);
        book.AddOrder(Order{OrderType::GoodTillCancel, bidId, Side::Buy, topPrice, RestingQuantity});
    }
}

BENCHMARK(BM_AddCancel)->RangeMultiplier(10)->Range(100, 10'000);
BENCHMARK(BM_AddMatch)->RangeMultiplier(10)->Range(100, 10'000);

} // namespace
