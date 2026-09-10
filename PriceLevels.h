#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <ranges>
#include <type_traits>
#include <vector>

#include "Constants.h"
#include "OrderPool.h"
#include "Usings.h"

// One price level: its time-priority queue as head/tail slot indices — the
// resting orders are the queue's nodes, linked through their own pool slots, so
// queueing an order allocates nothing — plus the aggregates FOK checks, level
// death and snapshots need. 16 bytes of scalars: relocating a level when the
// array shifts is a memmove. quantity_ is the sum of remaining quantity over the
// queue: every fill shrinks the order and the level by the same amount.
struct Level
{
    Quantity quantity_{};
    Quantity count_{};
    OrderIndex head_{Constants::InvalidIndex}; // oldest order: the next to match
    OrderIndex tail_{Constants::InvalidIndex}; // youngest order

    [[nodiscard]] bool Empty() const noexcept { return head_ == Constants::InvalidIndex; }

    // Queue a slot at the back (time priority): O(1), touching only the old tail.
    void PushBack(OrderPool &pool, OrderIndex slot) noexcept
    {
        pool.Prev(slot) = tail_;
        pool.Next(slot) = Constants::InvalidIndex;

        if (tail_ == Constants::InvalidIndex)
        {
            head_ = slot;
        }
        else
        {
            pool.Next(tail_) = slot;
        }

        tail_ = slot;
    }

    // Remove a slot from anywhere in the queue: O(1), no search, no iterator —
    // the slot's own links say where it sits. Leaves the slot's links reset,
    // the state OrderPool::Free requires.
    void Unlink(OrderPool &pool, OrderIndex slot) noexcept
    {
        const OrderIndex prev = pool.Prev(slot);
        const OrderIndex next = pool.Next(slot);

        if (prev == Constants::InvalidIndex)
        {
            head_ = next;
        }
        else
        {
            pool.Next(prev) = next;
        }

        if (next == Constants::InvalidIndex)
        {
            tail_ = prev;
        }
        else
        {
            pool.Prev(next) = prev;
        }

        pool.Prev(slot) = Constants::InvalidIndex;
        pool.Next(slot) = Constants::InvalidIndex;
    }
};

static_assert(std::is_trivially_copyable_v<Level>, "level shifts in PriceLevels must be memmoves");
static_assert(sizeof(Level) == 16, "four levels per 64-byte cache line");

// The live levels of one side, contiguous and sorted worst -> best under Better
// (std::greater<Price> for bids, std::less<Price> for asks), so the touch is
// back(): read, pop and push in O(1) with no heap traffic — the hot end of the
// book is the cheap end of the vector. Keys are stored apart from payload
// (std::flat_map's layout): a binary search touches only the 4-byte price array
// — 16 keys per cache line, L1-resident at tens of thousands of levels — and the
// Level exactly once. A level born or dying away from the touch shifts every
// better level (one memmove), a cost paid by that deep order alone.
//
// A Level reference stays valid until this side creates or erases a level.
template <typename Better>
class PriceLevels
{
public:
    explicit PriceLevels(std::size_t reserveLevels)
    {
        prices_.reserve(reserveLevels);
        levels_.reserve(reserveLevels);
    }

    // True when a is the more aggressive price on this side.
    [[nodiscard]] static constexpr bool IsBetter(Price a, Price b) noexcept { return Better{}(a, b); }

    [[nodiscard]] bool Empty() const noexcept { return prices_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return prices_.size(); }

    [[nodiscard]] Price BestPrice() const noexcept
    {
        assert(!Empty());
        return prices_.back();
    }

    [[nodiscard]] Price WorstPrice() const noexcept
    {
        assert(!Empty());
        return prices_.front();
    }

    [[nodiscard]] Level &Best() noexcept
    {
        assert(!Empty());
        return levels_.back();
    }

    [[nodiscard]] const Level &Best() const noexcept
    {
        assert(!Empty());
        return levels_.back();
    }

    // The level resting at price, or nullptr.
    [[nodiscard]] Level *Find(Price price) noexcept
    {
        const auto it = LowerBound(price);
        return it != prices_.end() && *it == price ? &levels_[Index(it)] : nullptr;
    }

    // The level at price, born in sorted position if absent. The one call that
    // moves levels (and, past the reserve, allocates).
    [[nodiscard]] Level &FindOrCreate(Price price)
    {
        const auto it = LowerBound(price);
        const auto index = Index(it);
        if (it == prices_.end() || *it != price)
        {
            prices_.insert(it, price);
            levels_.emplace(levels_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        return levels_[index];
    }

    // Erase a level obtained from this side. No re-search: a level's index is its
    // position in the array.
    void Erase(const Level &level) noexcept
    {
        const auto index = static_cast<std::size_t>(&level - levels_.data());
        assert(index < levels_.size());
        levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(index));
        prices_.erase(prices_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void PopBest() noexcept
    {
        assert(!Empty());
        levels_.pop_back();
        prices_.pop_back();
    }

    // (price, level) pairs from the touch outward.
    [[nodiscard]] auto BestFirst() const { return std::views::zip(prices_, levels_) | std::views::reverse; }

private:
    [[nodiscard]] std::vector<Price>::const_iterator LowerBound(Price price) const noexcept
    {
        // Sorted worst -> best: a precedes b when b is the better price.
        return std::ranges::lower_bound(prices_, price, [](Price a, Price b) noexcept { return IsBetter(b, a); });
    }

    [[nodiscard]] std::size_t Index(std::vector<Price>::const_iterator it) const noexcept
    {
        return static_cast<std::size_t>(it - prices_.cbegin());
    }

    std::vector<Price> prices_;
    std::vector<Level> levels_;
};
