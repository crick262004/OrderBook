#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "Order.h"
#include "OrderModify.h"
#include "OrderbookLevelInfos.h"
#include "Trade.h"
#include "Usings.h"

// The level FIFO owns its Orders by value: an order's single home is its list
// node, and everything else (orders_, locals during matching) holds non-owning
// views — iterators and references whose validity ends when the node is erased.
using OrderList = std::list<Order>;

// Single-threaded by design: no internal locking. The book never reads a clock;
// deciding when the trading day ends is the caller's policy — GoodForDay orders
// rest like GoodTillCancel until PruneGoodForDayOrders() is called at close.
class Orderbook
{
public:
    Orderbook() = default;
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
    // List iterators stay valid until their own node is erased, so the index
    // needs nothing else: *iterator reaches the Order for O(1) cancel/lookup.
    std::unordered_map<OrderId, OrderList::iterator> orders_;
};
