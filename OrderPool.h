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
// resizes — a grow would be an unbounded pause mid-matching. Dead slots store
// the free list through their own bytes (no side structure), so Alloc/Free are
// a head pop/push: O(1), two stores, no heap traffic. LIFO reuse also hands
// back the most recently freed — likely still cache-warm — slot first.
class OrderPool
{
public:
    explicit OrderPool(OrderIndex capacity) : slots_(capacity)
    {
        // Thread the virgin free list in slab order: 0 -> 1 -> ... -> end.
        for (OrderIndex i = 0; i + 1 < capacity; ++i)
        {
            slots_[i].nextFree_ = i + 1;
        }
        freeHead_ = capacity > 0 ? 0 : Constants::InvalidIndex;
    }

    // The pool controls which union member is alive, so slots must not be
    // copied wholesale behind its back.
    OrderPool(const OrderPool &) = delete;
    OrderPool &operator=(const OrderPool &) = delete;
    OrderPool(OrderPool &&) = delete;
    OrderPool &operator=(OrderPool &&) = delete;
    ~OrderPool() = default;

    // Returns the slot now homing the order, or InvalidIndex when full.
    [[nodiscard]] OrderIndex Alloc(Order order) noexcept
    {
        if (freeHead_ == Constants::InvalidIndex)
        {
            return Constants::InvalidIndex;
        }

        const OrderIndex index = freeHead_;
        freeHead_ = slots_[index].nextFree_;

        // Placement new: start the Order's lifetime in storage the slab already
        // owns — no allocation happens here.
        ::new (&slots_[index].order_) Order{std::move(order)};

        ++size_;
        return index;
    }

    void Free(OrderIndex index) noexcept
    {
        assert(index < slots_.size() && size_ > 0);

        slots_[index].order_.~Order();
        slots_[index].nextFree_ = freeHead_;
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

    [[nodiscard]] OrderIndex Size() const noexcept { return size_; }
    [[nodiscard]] OrderIndex Capacity() const noexcept { return static_cast<OrderIndex>(slots_.size()); }

private:
    // A slot is either a live Order or a free-list link overlaid on the same
    // bytes — never both, so the link costs zero extra memory. The compiler
    // cannot manage a union with a non-trivial member; the pool alone decides
    // which member is alive (placement new / explicit destroy above).
    union Slot
    {
        OrderIndex nextFree_;
        Order order_;

        Slot() noexcept : nextFree_{Constants::InvalidIndex} {}
        Slot(const Slot &) = delete;
        Slot &operator=(const Slot &) = delete;
        Slot(Slot &&) = delete;
        Slot &operator=(Slot &&) = delete;
        ~Slot() {}
    };

    // Teardown frees the slab wholesale without visiting live slots; that is
    // only sound while Order has nothing to release.
    static_assert(std::is_trivially_destructible_v<Order>);

    std::vector<Slot> slots_;
    OrderIndex freeHead_{Constants::InvalidIndex};
    OrderIndex size_{0};
};
