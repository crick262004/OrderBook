#include <memory>
#include <print>

#include "Orderbook.h"

int main()
{
    std::println("OrderBook matching engine!");

    Orderbook orderbook;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10));
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 2, Side::Sell, 105, 5));

    const auto levels = orderbook.GetOrderInfos();
    std::println("Resting orders: {} (bid levels: {}, ask levels: {})", orderbook.Size(), levels.GetBids().size(),
                 levels.GetAsks().size());

    return 0;
}
