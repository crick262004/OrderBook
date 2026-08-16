#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "Order.h"
#include "OrderModify.h"
#include "OrderbookLevelInfos.h"
#include "Trade.h"
#include "Usings.h"

// Single-threaded by design: no internal locking. GoodForDay currently rests like
// GoodTillCancel
class Orderbook
{
public:
    Orderbook() = default;
    Orderbook(const Orderbook &) = delete;
    Orderbook &operator=(const Orderbook &) = delete;
    Orderbook(Orderbook &&) = delete;
    Orderbook &operator=(Orderbook &&) = delete;
    ~Orderbook() = default;

    Trades AddOrder(OrderPointer order);
    void CancelOrder(OrderId orderId);
    Trades ModifyOrder(OrderModify order);

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] OrderbookLevelInfos GetOrderInfos() const;

private:
    struct OrderEntry
    {
        OrderPointer order_{nullptr};
        OrderPointers::iterator location_;
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

    void OnOrderCancelled(const OrderPointer &order);
    void OnOrderAdded(const OrderPointer &order);
    void OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled);
    void UpdateLevelData(Price price, Quantity quantity, LevelData::Action action);

    [[nodiscard]] bool CanFullyFill(Side side, Price price, Quantity quantity) const;
    [[nodiscard]] bool CanMatch(Side side, Price price) const;
    Trades MatchOrders();

    std::unordered_map<Price, LevelData> data_;
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderEntry> orders_;
};
