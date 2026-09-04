#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "Order.h"
#include "OrderModify.h"
#include "OrderPool.h"
#include "OrderbookLevelInfos.h"
#include "Trade.h"
#include "Usings.h"

// The arena owns every live Order; an order's single home is its pool slot,
// addressed by a 4-byte OrderIndex that never changes for the order's lifetime.
// The level FIFO is a queue of those indices (its nodes still heap-allocate
// until the intrusive links of 2.2), and everything else holds non-owning
// views whose validity ends when the slot is freed.
using OrderList = std::list<OrderIndex>;

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
    struct OrderEntry
    {
        OrderIndex slot_{Constants::InvalidIndex}; // InvalidIndex <=> this id is not live
        OrderList::iterator location_;             // queue position, for O(1) cancel
    };

    struct LevelData
    {
        Quantity quantity_{};
        Quantity count_{};

        enum class Action : std::uint8_t
        {
            Add,
            Remove,
            Match,
        };
    };

    void OnOrderCancelled(const Order &order);
    void OnOrderAdded(const Order &order);
    void OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled);
    void UpdateLevelData(Price price, Quantity quantity, LevelData::Action action);

    [[nodiscard]] bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    [[nodiscard]] bool CanMatch(Side side, Price price) const;
    Trades MatchOrders(Side takerSide);

    std::unordered_map<Price, LevelData> data_;
    std::map<Price, OrderList, std::greater<Price>> bids_;
    std::map<Price, OrderList, std::less<Price>> asks_;
    OrderPool pool_;
    // Flat order index: the OrderId IS the array position (see the capacity
    // contract above) — one array access, no hashing, no per-insert node.
    std::vector<OrderEntry> orders_;
};
