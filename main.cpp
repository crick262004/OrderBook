#include <print>

#include "Command.h"
#include "MatchingEngine.h"
#include "Order.h"
#include "OrderType.h"
#include "Side.h"

int main()
{
    std::println("OrderBook matching engine!");

    // The engine runs the book on its own thread; this thread only feeds
    // commands in and reads trades out through the two rings.
    MatchingEngine engine;
    engine.Submit(Command::Add(Order{OrderType::GoodTillCancel, 1, Side::Buy, 100, 10}));
    engine.Submit(Command::Add(Order{OrderType::GoodTillCancel, 2, Side::Sell, 105, 5}));

    // Crosses the resting bid at 100: partial fill, order 3 fully consumed.
    engine.Submit(Command::Add(Order{OrderType::GoodTillCancel, 3, Side::Sell, 100, 4}));

    // Everything submitted above is applied before the poison pill; the book is
    // ours to inspect once the matching thread has joined.
    engine.Stop();

    while (const Trade *trade = engine.NextTrade())
    {
        const auto &bid = trade->GetBidTrade();
        const auto &ask = trade->GetAskTrade();
        std::println("Trade: {} filled — bid #{} @ {} x ask #{} @ {}", bid.quantity_, bid.orderId_, bid.price_,
                     ask.orderId_, ask.price_);
        engine.PopTrade();
    }

    const auto &orderbook = engine.Book();
    const auto levels = orderbook.GetOrderInfos();
    std::println("Resting orders: {} (bid levels: {}, ask levels: {})", orderbook.Size(), levels.GetBids().size(),
                 levels.GetAsks().size());

    return 0;
}
