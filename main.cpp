#include <memory>
#include <print>

#include "Orderbook.h"

int main()
{
    std::println("OrderBook matching engine!");

    Orderbook orderbook;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10));
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 2, Side::Sell, 105, 5));

    // Crosses the resting bid at 100: partial fill, order 3 fully consumed.
    const auto trades = orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 3, Side::Sell, 100, 4));

    for (const auto &trade : trades)
    {
        const auto &bid = trade.GetBidTrade();
        const auto &ask = trade.GetAskTrade();
        std::println("Trade: {} filled — bid #{} @ {} x ask #{} @ {}", bid.quantity_, bid.orderId_, bid.price_,
                     ask.orderId_, ask.price_);
    }

    const auto levels = orderbook.GetOrderInfos();
    std::println("Resting orders: {} (bid levels: {}, ask levels: {})", orderbook.Size(), levels.GetBids().size(),
                 levels.GetAsks().size());

    return 0;
}
