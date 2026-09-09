#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "Order.h"
#include "OrderModify.h"
#include "OrderPool.h"
#include "OrderbookLevelInfos.h"
#include "PriceLevels.h"
#include "Trade.h"
#include "Usings.h"

// The arena owns every live Order; an order's single home is its pool slot,
// addressed by a 4-byte OrderIndex that never changes for the order's lifetime.
// Each side's levels are contiguous and sorted with the touch at the back
// (PriceLevels); a level holds its FIFO of slots and its aggregates. Everything
// else is a non-owning view whose validity ends when the slot is freed or when
// the level's side creates or erases a level.
//
// Single-threaded by design: no internal locking. The book never reads a clock;
// deciding when the trading day ends is the caller's policy — GoodForDay orders
// rest like GoodTillCancel until PruneGoodForDayOrders() is called at close.
//
// Capacity contract: `capacity` bounds both the number of resting orders and
// the order-id space — ids are venue-assigned dense integers in [0, capacity).
// An id outside that range, a duplicate id, or a full pool rejects the order
// (empty Trades, book untouched) — backpressure, never a throw, never a resize.
class Orderbook
{
public:
    static constexpr OrderIndex DefaultCapacity = 1u << 20;

    explicit Orderbook(OrderIndex capacity = DefaultCapacity);
    Orderbook(const Orderbook &) = delete;
    Orderbook &operator=(const Orderbook &) = delete;
    Orderbook(Orderbook &&) = delete;
    Orderbook &operator=(Orderbook &&) = delete;
    ~Orderbook() = default;

    Trades AddOrder(Order order);
    void CancelOrder(OrderId orderId);
    Trades ModifyOrder(OrderModify order);
    void PruneGoodForDayOrders();

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] OrderbookLevelInfos GetOrderInfos() const;

private:
    // Levels per side reserved up front, so a level born in steady state never
    // allocates. A deeper book grows the arrays: a cold, one-off cost.
    static constexpr std::size_t LevelReserve = 1024;

    using Bids = PriceLevels<std::greater<Price>>; // best bid: highest price
    using Asks = PriceLevels<std::less<Price>>;    // best ask: lowest price

    struct OrderEntry
    {
        OrderIndex slot_{Constants::InvalidIndex}; // InvalidIndex <=> this id is not live
        OrderList::iterator location_;             // queue position, for O(1) cancel
    };

    template <typename Levels>
    void Rest(Levels &levels, OrderIndex slot);

    template <typename Levels>
    void Unrest(Levels &levels, Price price, Quantity remaining, OrderList::iterator location);

    template <typename Levels>
    [[nodiscard]] static bool Covers(const Levels &opposite, Price limit, Quantity quantity);

    [[nodiscard]] bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    [[nodiscard]] bool CanMatch(Side side, Price price) const;
    Trades MatchOrders(Side takerSide);

    Bids bids_;
    Asks asks_;
    OrderPool pool_;
    // Flat order index: the OrderId IS the array position (see the capacity
    // contract above) — one array access, no hashing, no per-insert node.
    std::vector<OrderEntry> orders_;
};
