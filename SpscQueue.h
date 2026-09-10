#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
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
// slot is usable. The two counters deliberately share a cache line for now —
// roadmap 3.3 splits them (false sharing) and measures the difference.
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two: slot = counter & mask");
    static_assert(std::is_trivially_copyable_v<T>, "items are copied into and read out of raw slots by value");

public:
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

        // Acquire pairs with the consumer's release in Pop: the consumer's read
        // of the slot we are about to overwrite is complete before we touch it.
        // A stale head_ only errs toward "full" — it never increases.
        if (tail - head_.load(std::memory_order_acquire) == Capacity)
        {
            return false;
        }

        std::construct_at(std::addressof(cells_[tail & Mask].value_), item);

        // Release: the item is visible to whoever acquires this counter.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side, zero-copy: Front exposes the oldest item in place (nullptr
    // when empty), Pop releases its slot. Use the item, *then* Pop — after Pop
    // the producer may overwrite it.
    [[nodiscard]] const T *Front() const noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);

        // Acquire pairs with the producer's release in TryPush: the item's bytes
        // are visible before we read them.
        if (tail_.load(std::memory_order_acquire) == head)
        {
            return nullptr;
        }

        return std::addressof(cells_[head & Mask].value_);
    }

    // Precondition: Front() returned non-null since the last Pop.
    void Pop() noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        assert(head != tail_.load(std::memory_order_relaxed));

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

    std::unique_ptr<Cell[]> cells_;
    std::atomic<std::uint64_t> head_{0}; // next slot to read; written by the consumer only
    std::atomic<std::uint64_t> tail_{0}; // next slot to write; written by the producer only
};
