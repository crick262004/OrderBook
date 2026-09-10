#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>

#include <benchmark/benchmark.h>

#include "Command.h"
#include "MatchingEngine.h"
#include "Order.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "Side.h"
#include "SpscQueue.h"
#include "Trade.h"
#include "Usings.h"

// Steady-state hot-path baselines. Every measured iteration leaves the
// book at its starting depth, so growth of the containers never pollutes the
// per-iteration average. Order construction stays inside the timed loop: since
// 1.1 it is a stack value, since 1.2 it lands in the arena, since 2.1 the price
// levels are contiguous, since 2.2 the level FIFO is intrusive, and since 3.1
// fills go to a sink instead of a returned vector — no heap allocation remains
// on any path. The `allocs` counter below proves that per iteration instead of
// asserting it. The SPSC benchmarks time the ring itself and the two-core
// round trip through it: the instruments roadmap 3.2 (pinning) and 3.3
// (cache-line alignment) will move.

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

// The sink the timed loops hand to the book: the fill is observed (so the match
// can't be optimised away) and dropped — reporting cost stays out of the number.
constexpr auto DiscardTrade = [](const Trade &trade)
{
    auto quantity = trade.GetBidTrade().quantity_;
    benchmark::DoNotOptimize(quantity);
};

Order RestingBid(std::int64_t priceAndId)
{
    return Order{OrderType::GoodTillCancel, static_cast<OrderId>(priceAndId), Side::Buy, static_cast<Price>(priceAndId),
                 RestingQuantity};
}

// N resting bids at distinct prices 1..N, id == price. No asks, so nothing matches.
void FillBids(Orderbook &book, std::int64_t depth)
{
    for (std::int64_t i = 1; i <= depth; ++i)
    {
        book.AddOrder(RestingBid(i), DiscardTrade);
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
        book.AddOrder(Order{OrderType::GoodTillCancel, orderId, Side::Buy, price, RestingQuantity}, DiscardTrade);
        book.CancelOrder(orderId);
    }
}

// Crossing sell fully fills the best bid (one trade), then the bid is replenished:
// measures the full match pipeline at constant depth. Expect allocs == 0 since
// 3.1: the fill goes to the sink, no vector is returned.
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
        book.AddOrder(Order{OrderType::GoodTillCancel, askId, Side::Sell, topPrice, RestingQuantity}, DiscardTrade);
        book.AddOrder(Order{OrderType::GoodTillCancel, bidId, Side::Buy, topPrice, RestingQuantity}, DiscardTrade);
    }
}

// The ring alone, one thread: the cost of a push + front + pop pair is the
// acquire/release traffic (ldar/stlr on ARM, plain movs on x86) with no
// cross-core transfer — the floor under every inter-thread number below.
void BM_SpscPushPop(benchmark::State &state)
{
    // Heap-allocated and its address leaked through DoNotOptimize: a ring that
    // provably never escapes the function can't be seen by another thread, so
    // the compiler is entitled to keep head_/tail_ in registers and drop every
    // atomic — measured 0.6 ns of pure arithmetic before this line existed.
    auto queue = std::make_unique<SpscQueue<Command, 1024>>();
    benchmark::DoNotOptimize(queue.get());
    const auto command = Command::Cancel(1);

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(queue->TryPush(command));
        const Command *front = queue->Front();
        benchmark::DoNotOptimize(front);
        queue->Pop();
    }
}

// Two threads, two rings: this thread pushes a request, the echo thread pops it
// and pushes it back, this thread waits for the reply. One iteration is one
// round trip = two cross-core hand-offs, i.e. twice the cache-line transfer
// latency between whichever cores the OS picked (3.2 pins them) plus the
// counters' false-sharing tax (3.3 removes it).
void BM_SpscPingPong(benchmark::State &state)
{
    constexpr std::uint64_t Poison = 0;
    SpscQueue<std::uint64_t, 1024> request;
    SpscQueue<std::uint64_t, 1024> response;

    std::jthread echo{[&request, &response]
                      {
                          for (;;)
                          {
                              const std::uint64_t *item = nullptr;
                              while ((item = request.Front()) == nullptr)
                              {
                              }
                              const auto value = *item;
                              request.Pop();
                              if (value == Poison)
                              {
                                  return;
                              }
                              while (!response.TryPush(value))
                              {
                              }
                          }
                      }};

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        while (!request.TryPush(1))
        {
        }
        const std::uint64_t *reply = nullptr;
        while ((reply = response.Front()) == nullptr)
        {
        }
        auto value = *reply;
        benchmark::DoNotOptimize(value);
        response.Pop();
    }

    while (!request.TryPush(Poison))
    {
    }
}

// End to end through the engine: submit a crossing sell, wait for its trade to
// come back out, replenish the bid. One iteration is order-in to trade-out
// latency: two ring hops + the add + the match + the sink's push, on a book of
// the given depth. Compare with BM_AddMatch to see what the thread boundary costs.
void BM_EngineRoundTrip(benchmark::State &state)
{
    const auto depth = state.range(0);
    MatchingEngine engine;
    for (std::int64_t i = 1; i <= depth; ++i)
    {
        engine.Submit(Command::Add(RestingBid(i)));
    }

    const auto topPrice = static_cast<Price>(depth);
    const auto bidId = static_cast<OrderId>(depth);
    const auto askId = static_cast<OrderId>(depth + 1);

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        engine.Submit(Command::Add(Order{OrderType::GoodTillCancel, askId, Side::Sell, topPrice, RestingQuantity}));
        const Trade *trade = nullptr;
        while ((trade = engine.NextTrade()) == nullptr)
        {
        }
        DiscardTrade(*trade);
        engine.PopTrade();
        // Queued behind this iteration's ask and ahead of the next: the book
        // sees add-match-replenish in order without waiting for it here.
        engine.Submit(Command::Add(Order{OrderType::GoodTillCancel, bidId, Side::Buy, topPrice, RestingQuantity}));
    }
}

BENCHMARK(BM_AddCancel)->RangeMultiplier(10)->Range(100, 10'000);
BENCHMARK(BM_AddMatch)->RangeMultiplier(10)->Range(100, 10'000);
BENCHMARK(BM_SpscPushPop);
BENCHMARK(BM_SpscPingPong)->UseRealTime();
BENCHMARK(BM_EngineRoundTrip)->Arg(1'000)->UseRealTime();

} // namespace
