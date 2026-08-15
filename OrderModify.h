#pragma once

#include "Order.h"

class OrderModify
{
public:
    OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
        : orderId_{orderId}, side_{side}, price_{price}, quantity_{quantity}
    {
    }

    [[nodiscard]] constexpr OrderId GetOrderId() const noexcept { return orderId_; }
    [[nodiscard]] constexpr Side GetSide() const noexcept { return side_; }
    [[nodiscard]] constexpr Price GetPrice() const noexcept { return price_; }
    [[nodiscard]] constexpr Quantity GetQuantity() const noexcept { return quantity_; }

    // Modify is cancel + re-add: build the replacement order. 
    [[nodiscard]] OrderPointer ToOrderPointer(OrderType type) const
    {
        return std::make_shared<Order>(type, GetOrderId(), GetSide(), GetPrice(), GetQuantity());
    }

private:
    OrderId orderId_;
    Side side_;
    Price price_;
    Quantity quantity_;
};
