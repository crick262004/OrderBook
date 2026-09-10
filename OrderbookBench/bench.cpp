#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "Affinity.h"
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

// The A/B pair for false sharing: the default layout gives each ring counter its
// own cache line; the packed control puts all four on one line, as 3.1 had them.
// Same code, same run, only the layout differs, so the delta is the false-sharing tax.
using PaddedRing = SpscQueue<std::uint64_t, 1024>;
using PackedRing = SpscQueue<std::uint64_t, 1024, alignof(std::atomic<std::uint64_t>)>;

constexpr std::uint64_t Poison = 0;

// Cores for the pinned variants: the last two the OS reports, the least likely
// to be hosting something else on a busy machine. On macOS pinning reports
// Unsupported and the pinned variants measure the same thing as the unpinned.
unsigned PingCore()
{
    const auto cores = std::thread::hardware_concurrency();
    return cores >= 2 ? cores - 2 : 0;
}

unsigned PongCore()
{
    const auto cores = std::thread::hardware_concurrency();
    return cores >= 2 ? cores - 1 : 0;
}

// Echo thread: pops each request and pushes it straight back; exits on Poison.
template <typename Ring>
std::thread StartEcho(Ring &request, Ring &response)
{
    return std::thread{[&request, &response]
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
}

template <typename Ring>
void RoundTrip(Ring &request, Ring &response)
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

template <typename Ring>
void StopEcho(Ring &request, std::thread &echo)
{
    while (!request.TryPush(Poison))
    {
    }
    echo.join();
}

// Pins this thread and the echo thread to two fixed cores for the benchmark's
// duration (the main thread's mask is restored after). Records whether the OS
// honoured both pins as the `pinned` counter — before the allocation scope
// opens, since inserting a counter allocates.
template <bool Pinned>
void PinPair(benchmark::State &state, std::optional<ScopedPin> &pin, std::thread &echo)
{
    bool pinned = false;
    if constexpr (Pinned)
    {
        pin.emplace(PingCore());
        pinned = pin->Result().has_value() && PinToCore(echo, PongCore()).has_value();
    }
    state.counters["pinned"] = pinned ? 1.0 : 0.0;
}

// Two threads, two rings: this thread pushes a request, the echo thread pops it
// and pushes it back, this thread waits for the reply. One iteration is one
// round trip = two cross-core hand-offs, i.e. twice the cache-line transfer
// latency between the two cores — whichever the OS picked, or the pinned pair —
// plus, for the packed control, the counters' false-sharing tax.
template <typename Ring, bool Pinned>
void BM_SpscPingPong(benchmark::State &state)
{
    Ring request;
    Ring response;
    std::thread echo = StartEcho(request, response);
    std::optional<ScopedPin> pin;
    PinPair<Pinned>(state, pin, echo);

    const AllocationScope allocations{state};
    for (auto _ : state)
    {
        RoundTrip(request, response);
    }

    StopEcho(request, echo);
}

// The same round trip with every iteration timed individually, so the tail is
// visible: pinning and layout move p99 and max far more than the mean. The two
// clock reads add ~20 ns per iteration, so compare p50 here with the mean above.
template <typename Ring, bool Pinned>
void BM_SpscPingPongTail(benchmark::State &state)
{
    Ring request;
    Ring response;
    // Sized before the allocation scope: the samples are harness, not ring.
    std::vector<std::int64_t> samples(static_cast<std::size_t>(state.max_iterations));
    std::thread echo = StartEcho(request, response);
    std::optional<ScopedPin> pin;
    PinPair<Pinned>(state, pin, echo);

    std::size_t count = 0;
    {
        // Scoped to the timed loop: the percentile counters set below are
        // harness bookkeeping that allocates map nodes, not ring traffic.
        const AllocationScope allocations{state};
        for (auto _ : state)
        {
            const auto start = std::chrono::steady_clock::now();
            RoundTrip(request, response);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            samples[count++] = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        }
    }

    StopEcho(request, echo);

    std::sort(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(count));
    const auto percentile = [&](std::size_t hundredths)
    { return static_cast<double>(samples[std::min(count - 1, count * hundredths / 100)]); };
    state.counters["p50_ns"] = percentile(50);
    state.counters["p99_ns"] = percentile(99);
    state.counters["max_ns"] = static_cast<double>(samples[count - 1]);
}

// End to end through the engine: submit a crossing sell, wait for its trade to
// come back out, replenish the bid. One iteration is order-in to trade-out
// latency: two ring hops + the add + the match + the sink's push, on a book of
// the given depth. Compare with BM_AddMatch to see what the thread boundary costs.
// The pinned variant puts the matching thread and this feed thread on two fixed cores.
template <bool Pinned>
void BM_EngineRoundTrip(benchmark::State &state)
{
    const auto depth = state.range(0);
    MatchingEngine engine{Orderbook::DefaultCapacity, Pinned ? std::optional<unsigned>{PongCore()} : std::nullopt};
    std::optional<ScopedPin> pin;
    bool pinned = false;
    if constexpr (Pinned)
    {
        pin.emplace(PingCore());
        pinned = pin->Result().has_value() && engine.Affinity().has_value();
    }
    state.counters["pinned"] = pinned ? 1.0 : 0.0;

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
BENCHMARK_TEMPLATE(BM_SpscPingPong, PaddedRing, false)->Name("BM_SpscPingPong/padded")->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscPingPong, PackedRing, false)->Name("BM_SpscPingPong/packed")->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscPingPong, PaddedRing, true)->Name("BM_SpscPingPong/padded/pinned")->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscPingPongTail, PaddedRing, false)->Name("BM_SpscPingPongTail/padded")->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscPingPongTail, PackedRing, false)->Name("BM_SpscPingPongTail/packed")->UseRealTime();
BENCHMARK_TEMPLATE(BM_SpscPingPongTail, PaddedRing, true)->Name("BM_SpscPingPongTail/padded/pinned")->UseRealTime();
BENCHMARK_TEMPLATE(BM_EngineRoundTrip, false)->Name("BM_EngineRoundTrip")->Arg(1'000)->UseRealTime();
BENCHMARK_TEMPLATE(BM_EngineRoundTrip, true)->Name("BM_EngineRoundTrip/pinned")->Arg(1'000)->UseRealTime();

} // namespace
