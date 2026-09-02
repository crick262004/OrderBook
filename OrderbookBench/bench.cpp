#include <cstdint>

#include <benchmark/benchmark.h>

#include "Order.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "Side.h"
#include "Usings.h"

// Steady-state hot-path baselines. Every measured iteration leaves the
// book at its starting depth, so growth of the std::map/std::list containers never
// pollutes the per-iteration average. Order construction stays inside the timed
// loop: since 1.1 it is a stack value (the shared_ptr control block and refcounts
// are gone), but the list-node and map allocations remain until Phases 1.2/2.

namespace
{

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

    for (auto _ : state)
    {
        auto trades = book.AddOrder(Order{OrderType::GoodTillCancel, orderId, Side::Buy, price, RestingQuantity});
        benchmark::DoNotOptimize(trades);
        book.CancelOrder(orderId);
    }
}

// Crossing sell fully fills the best bid (one trade), then the bid is replenished:
// measures the full match pipeline at constant depth.
void BM_AddMatch(benchmark::State &state)
{
    const auto depth = state.range(0);
    Orderbook book;
    FillBids(book, depth);

    const auto topPrice = static_cast<Price>(depth);
    const auto bidId = static_cast<OrderId>(depth);
    const auto askId = static_cast<OrderId>(depth + 1);

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
