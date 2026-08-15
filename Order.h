#pragma once

#include <expected>
#include <list>
#include <memory>

#include "Constants.h"
#include "OrderError.h"
#include "OrderType.h"
#include "Side.h"
#include "Usings.h"

class Order
{
public:
    Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
        : orderId_{orderId}, price_{price}, initialQuantity_{quantity}, remainingQuantity_{quantity},
          orderType_{orderType}, side_{side}
    {
    }

    Order(OrderId orderId, Side side, Quantity quantity)
        : Order(OrderType::Market, orderId, side, Constants::InvalidPrice, quantity)
    {
    }

    [[nodiscard]] constexpr OrderId GetOrderId() const noexcept { return orderId_; }
    [[nodiscard]] constexpr Side GetSide() const noexcept { return side_; }
    [[nodiscard]] constexpr Price GetPrice() const noexcept { return price_; }
    [[nodiscard]] constexpr OrderType GetOrderType() const noexcept { return orderType_; }
    [[nodiscard]] constexpr Quantity GetInitialQuantity() const noexcept { return initialQuantity_; }
    [[nodiscard]] constexpr Quantity GetRemainingQuantity() const noexcept { return remainingQuantity_; }
    [[nodiscard]] constexpr Quantity GetFilledQuantity() const noexcept
    {
        return GetInitialQuantity() - GetRemainingQuantity();
    }
    [[nodiscard]] constexpr bool IsFilled() const noexcept { return GetRemainingQuantity() == 0; }

    [[nodiscard]] std::expected<void, OrderError> Fill(Quantity quantity) noexcept
    {
        if (quantity > GetRemainingQuantity())
        {
            return std::unexpected{OrderError::Overfill};
        }

        remainingQuantity_ -= quantity;
        return {};
    }

    [[nodiscard]] std::expected<void, OrderError> ToGoodTillCancel(Price price) noexcept
    {
        if (GetOrderType() != OrderType::Market)
        {
            return std::unexpected{OrderError::NotMarketOrder};
        }

        price_ = price;
        orderType_ = OrderType::GoodTillCancel;
        return {};
    }

private:
    // Largest-first member order: 24 bytes vs 32 with the declaration-order layout.
    OrderId orderId_;
    Price price_;
    Quantity initialQuantity_;
    Quantity remainingQuantity_;
    OrderType orderType_;
    Side side_;
};

using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;
