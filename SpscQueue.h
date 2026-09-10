#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

// Bounded lock-free single-producer / single-consumer ring. Exactly one thread
// calls TryPush; exactly one (other) thread calls Front/Pop. Each counter has
// one writer, so neither side ever needs a CAS or a lock — plain loads and
// stores with two release/acquire edges: tail_ publishes filled slots to the
// consumer, head_ hands drained slots back to the producer. Both sides are
// wait-free (a bounded number of steps per call). Storage is allocated once at
// construction; a full ring reports back-pressure, it never grows.
//
// head_ and tail_ are monotonically increasing 64-bit counters (a wrap would
// take centuries at 1 G ops/s): the slot is the counter masked by Capacity - 1
// and occupancy is tail_ - head_, so full and empty are unambiguous and every
// slot is usable.
//
// Layout (3.3): four counters on four cache lines. A write to a line invalidates
// every other core's copy of it, so two writers sharing a line — the producer's
// tail_ next to the consumer's head_ — made each side miss on its *own* counter
// after every operation of the other (false sharing). With one writer per line,
// tail_'s line travels producer -> consumer once per publish and never comes
// back. Each side also keeps a private copy of the peer's counter and re-reads
// the shared line only when that copy says full/empty, so a burst of N pushes
// touches the consumer's line once, not N times. The private copies get their
// own lines too: a note scribbled next to tail_ would shred the consumer's copy
// of tail_ for nothing. CounterAlignment is a template parameter so the packed
// layout stays available as the A/B control in the benchmarks.
template <typename T, std::size_t Capacity, std::size_t CounterAlignment = std::hardware_destructive_interference_size>
class SpscQueue
{
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two: slot = counter & mask");
    static_assert(std::is_trivially_copyable_v<T>, "items are copied into and read out of raw slots by value");
    static_assert(std::has_single_bit(CounterAlignment) && CounterAlignment >= alignof(std::atomic<std::uint64_t>),
                  "counter alignment must be a power of two no smaller than the counter itself");

public:
    // The layout's counter alignment, for code that must agree with it (tests, the
    // packed A/B control). GCC warns on every direct use of
    // hardware_destructive_interference_size outside a template because its value
    // is tuning-dependent; naming it once here keeps that decision in one place.
    static constexpr std::size_t Alignment = CounterAlignment;

    SpscQueue() : cells_{std::make_unique<Cell[]>(Capacity)} {}

    // A ring with a live peer cannot be relocated behind its back.
    SpscQueue(const SpscQueue &) = delete;
    SpscQueue &operator=(const SpscQueue &) = delete;
    SpscQueue(SpscQueue &&) = delete;
    SpscQueue &operator=(SpscQueue &&) = delete;
    ~SpscQueue() = default;

    // Producer side. False when the ring is full: the caller decides whether to
    // spin, drop, or count — the ring itself never blocks and never allocates.
    [[nodiscard]] bool TryPush(const T &item) noexcept
    {
        // Own counter: nobody else writes it, so no ordering is needed to read it.
        const auto tail = tail_.load(std::memory_order_relaxed);

        // The private copy of head_ is a lower bound (head_ only grows), so
        // "not full" by the copy is safe. Only when it says full do we touch the
        // consumer's line. Acquire pairs with the consumer's release in Pop: its
        // read of the slot we are about to overwrite is complete before we do.
        if (tail - cachedHead_ == Capacity)
        {
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (tail - cachedHead_ == Capacity)
            {
                return false;
            }
        }

        std::construct_at(std::addressof(cells_[tail & Mask].value_), item);

        // Release: the item is visible to whoever acquires this counter.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side, zero-copy: Front exposes the oldest item in place (nullptr
    // when empty), Pop releases its slot. Use the item, *then* Pop — after Pop
    // the producer may overwrite it. Non-const: it refreshes the consumer's
    // private copy of tail_.
    [[nodiscard]] const T *Front() noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);

        // Mirror of TryPush: the copy of tail_ is a lower bound, so anything it
        // says is published really is (the acquire that fetched it ordered those
        // items before us). Only an apparently empty ring re-reads the shared line.
        if (cachedTail_ == head)
        {
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (cachedTail_ == head)
            {
                return nullptr;
            }
        }

        return std::addressof(cells_[head & Mask].value_);
    }

    // Precondition: Front() returned non-null since the last Pop.
    void Pop() noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        assert(head != cachedTail_);

        // Release: our read of the slot happens-before the producer's next write to it.
        head_.store(head + 1, std::memory_order_release);
    }

    // Exact from the consumer, a snapshot from anywhere else (the peer may be mid-call).
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool Empty() const noexcept { return Size() == 0; }
    [[nodiscard]] static constexpr std::size_t MaxSize() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t Mask = Capacity - 1;

    // Raw storage for one item: the single-member union defers T's lifetime to
    // the producer's construct_at (T need not be default-constructible), and a
    // trivially copyable T needs no destroy on the way out.
    struct Cell
    {
        union
        {
            T value_;
        };

        Cell() noexcept {}
        Cell(const Cell &) = delete;
        Cell &operator=(const Cell &) = delete;
        Cell(Cell &&) = delete;
        Cell &operator=(Cell &&) = delete;
    };

    // Read by both sides, written by neither after construction: sharing is free.
    std::unique_ptr<Cell[]> cells_;

    // Producer's lines: its counter (read remotely by the consumer) and its
    // private copy of the consumer's counter.
    alignas(CounterAlignment) std::atomic<std::uint64_t> tail_{0};
    alignas(CounterAlignment) std::uint64_t cachedHead_{0};

    // Consumer's lines: mirror image.
    alignas(CounterAlignment) std::atomic<std::uint64_t> head_{0};
    alignas(CounterAlignment) std::uint64_t cachedTail_{0};
};
