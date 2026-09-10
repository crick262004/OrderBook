#pragma once

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "Constants.h"
#include "Order.h"
#include "Usings.h"

// Fixed-capacity arena that owns every live Order. The slab is allocated once
// at construction and never grows: a full pool rejects (InvalidIndex), it never
// resizes — a grow would be an unbounded pause mid-matching. Alloc/Free are a
// head pop/push on the free list: O(1), a few stores, no heap traffic. LIFO
// reuse also hands back the most recently freed — likely still cache-warm —
// slot first.
//
// Every slot carries two intrusive links, so a resting order is its own FIFO
// node (a Level holds the queue's head and tail). next_ does double duty: the
// queue link while the slot is live, the free-list link while it is dead —
// disjoint phases, one field, so the free list costs no extra memory.
class OrderPool
{
public:
    explicit OrderPool(OrderIndex capacity) : slots_(capacity)
    {
        // Thread the virgin free list in slab order: 0 -> 1 -> ... -> end.
        for (OrderIndex i = 0; i + 1 < capacity; ++i)
        {
            slots_[i].next_ = i + 1;
        }
        freeHead_ = capacity > 0 ? 0 : Constants::InvalidIndex;
    }

    // The pool controls the Order's lifetime inside each slot, so slots must not
    // be copied wholesale behind its back.
    OrderPool(const OrderPool &) = delete;
    OrderPool &operator=(const OrderPool &) = delete;
    OrderPool(OrderPool &&) = delete;
    OrderPool &operator=(OrderPool &&) = delete;
    ~OrderPool() = default;

    // Returns the slot now homing the order — links reset, queued nowhere — or
    // InvalidIndex when full.
    [[nodiscard]] OrderIndex Alloc(Order order) noexcept
    {
        if (freeHead_ == Constants::InvalidIndex)
        {
            return Constants::InvalidIndex;
        }

        const OrderIndex index = freeHead_;
        Slot &slot = slots_[index];
        freeHead_ = slot.next_;

        // Placement new: start the Order's lifetime in storage the slab already
        // owns — no allocation happens here.
        ::new (&slot.order_) Order{std::move(order)};
        slot.prev_ = Constants::InvalidIndex;
        slot.next_ = Constants::InvalidIndex;

        ++size_;
        return index;
    }

    // Precondition: the slot is no longer queued in any level. Free repurposes
    // next_ as the free-list link, so Level::Unlink must come first.
    void Free(OrderIndex index) noexcept
    {
        assert(index < slots_.size() && size_ > 0);
        Slot &slot = slots_[index];
        assert(slot.prev_ == Constants::InvalidIndex && slot.next_ == Constants::InvalidIndex);

        slot.order_.~Order();
        slot.next_ = freeHead_;
        freeHead_ = index;
        --size_;
    }

    [[nodiscard]] Order &operator[](OrderIndex index) noexcept
    {
        assert(index < slots_.size());
        return slots_[index].order_;
    }

    [[nodiscard]] const Order &operator[](OrderIndex index) const noexcept
    {
        assert(index < slots_.size());
        return slots_[index].order_;
    }

    // Queue links of a live slot, owned by the Level the order rests in.
    [[nodiscard]] OrderIndex &Prev(OrderIndex index) noexcept
    {
        assert(index < slots_.size());
        return slots_[index].prev_;
    }

    [[nodiscard]] OrderIndex &Next(OrderIndex index) noexcept
    {
        assert(index < slots_.size());
        return slots_[index].next_;
    }

    [[nodiscard]] OrderIndex Prev(OrderIndex index) const noexcept
    {
        assert(index < slots_.size());
        return slots_[index].prev_;
    }

    [[nodiscard]] OrderIndex Next(OrderIndex index) const noexcept
    {
        assert(index < slots_.size());
        return slots_[index].next_;
    }

    [[nodiscard]] OrderIndex Size() const noexcept { return size_; }
    [[nodiscard]] OrderIndex Capacity() const noexcept { return static_cast<OrderIndex>(slots_.size()); }

private:
    struct Slot
    {
        // Raw storage for the Order: alive between Alloc and Free, nothing
        // otherwise. The single-member union keeps the compiler from
        // constructing or copying an Order behind the pool's back; the pool
        // alone starts and ends its lifetime (placement new / explicit destroy).
        union
        {
            Order order_;
        };
        OrderIndex prev_{Constants::InvalidIndex};
        OrderIndex next_{Constants::InvalidIndex}; // FIFO link while live, free-list link while dead

        Slot() noexcept {}
        Slot(const Slot &) = delete;
        Slot &operator=(const Slot &) = delete;
        Slot(Slot &&) = delete;
        Slot &operator=(Slot &&) = delete;
    };

    // Teardown frees the slab wholesale without visiting live slots; that is
    // only sound while Order has nothing to release.
    static_assert(std::is_trivially_destructible_v<Order>);
    static_assert(sizeof(Slot) == 32, "two slots per 64-byte cache line");

    std::vector<Slot> slots_;
    OrderIndex freeHead_{Constants::InvalidIndex};
    OrderIndex size_{0};
};
