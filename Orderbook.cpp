#include "Orderbook.h"

#include <iterator>
#include <numeric>

Trades Orderbook::AddOrder(OrderPointer order)
{
    if (orders_.contains(order->GetOrderId()))
    {
        return {};
    }

    if (order->GetOrderType() == OrderType::Market)
    {
        // A market order pegs itself to the worst resting price on the opposite
        // side, so it sweeps every level the book can offer.
        if (order->GetSide() == Side::Buy && !asks_.empty())
        {
            const auto &[worstAsk, _] = *asks_.rbegin();
            if (!order->ToGoodTillCancel(worstAsk))
            {
                return {};
            }
        }
        else if (order->GetSide() == Side::Sell && !bids_.empty())
        {
            const auto &[worstBid, _] = *bids_.rbegin();
            if (!order->ToGoodTillCancel(worstBid))
            {
                return {};
            }
        }
        else
        {
            return {};
        }
    }

    if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
    {
        return {};
    }

    if (order->GetOrderType() == OrderType::FillOrKill &&
        !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity()))
    {
        return {};
    }

    auto &levelOrders = order->GetSide() == Side::Buy ? bids_[order->GetPrice()] : asks_[order->GetPrice()];
    levelOrders.push_back(order);
    const auto iterator = std::prev(levelOrders.end());

    orders_.insert({order->GetOrderId(), OrderEntry{order, iterator}});

    OnOrderAdded(order);

    return MatchOrders();
}

void Orderbook::CancelOrder(OrderId orderId)
{
    const auto entryIt = orders_.find(orderId);
    if (entryIt == orders_.end())
    {
        return;
    }

    const auto [order, iterator] = entryIt->second;
    orders_.erase(entryIt);

    const auto price = order->GetPrice();

    if (order->GetSide() == Side::Sell)
    {
        auto &levelOrders = asks_.at(price);
        levelOrders.erase(iterator);
        if (levelOrders.empty())
        {
            asks_.erase(price);
        }
    }
    else
    {
        auto &levelOrders = bids_.at(price);
        levelOrders.erase(iterator);
        if (levelOrders.empty())
        {
            bids_.erase(price);
        }
    }

    OnOrderCancelled(order);
}

Trades Orderbook::ModifyOrder(OrderModify order)
{
    const auto entryIt = orders_.find(order.GetOrderId());
    if (entryIt == orders_.end())
    {
        return {};
    }

    const auto orderType = entryIt->second.order_->GetOrderType();

    CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrderPointer(orderType));
}

std::size_t Orderbook::Size() const noexcept
{
    return orders_.size();
}

OrderbookLevelInfos Orderbook::GetOrderInfos() const
{
    LevelInfos bidInfos;
    LevelInfos askInfos;
    bidInfos.reserve(bids_.size());
    askInfos.reserve(asks_.size());

    const auto createLevelInfo = [](Price price, const OrderPointers &levelOrders)
    {
        return LevelInfo{price, std::accumulate(levelOrders.begin(), levelOrders.end(), Quantity{0},
                                                [](Quantity runningSum, const OrderPointer &order)
                                                { return runningSum + order->GetRemainingQuantity(); })};
    };

    for (const auto &[price, levelOrders] : bids_)
    {
        bidInfos.push_back(createLevelInfo(price, levelOrders));
    }

    for (const auto &[price, levelOrders] : asks_)
    {
        askInfos.push_back(createLevelInfo(price, levelOrders));
    }

    return OrderbookLevelInfos{bidInfos, askInfos};
}

void Orderbook::OnOrderCancelled(const OrderPointer &order)
{
    UpdateLevelData(order->GetPrice(), order->GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(const OrderPointer &order)
{
    UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}

void Orderbook::OnOrderMatched(Price price, Quantity quantity, bool isFullyFilled)
{
    UpdateLevelData(price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}

void Orderbook::UpdateLevelData(Price price, Quantity quantity, LevelData::Action action)
{
    auto &data = data_[price];

    switch (action)
    {
    case LevelData::Action::Add:
        ++data.count_;
        data.quantity_ += quantity;
        break;
    case LevelData::Action::Remove:
        --data.count_;
        data.quantity_ -= quantity;
        break;
    case LevelData::Action::Match:
        data.quantity_ -= quantity;
        break;
    }

    if (data.count_ == 0)
    {
        data_.erase(price);
    }
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    if (!CanMatch(side, price))
    {
        return false;
    }

    // CanMatch guarantees the opposite side is non-empty, so its best price exists.
    const Price threshold = side == Side::Buy ? asks_.begin()->first : bids_.begin()->first;

    for (const auto &[levelPrice, levelData] : data_)
    {
        // Skip levels on the wrong side of the opposing best price — those belong
        // to the incoming order's own side, not to liquidity it can hit.
        if ((side == Side::Buy && levelPrice < threshold) || (side == Side::Sell && levelPrice > threshold))
        {
            continue;
        }

        // Skip levels beyond the incoming order's limit price.
        if ((side == Side::Buy && levelPrice > price) || (side == Side::Sell && levelPrice < price))
        {
            continue;
        }

        if (quantity <= levelData.quantity_)
        {
            return true;
        }

        quantity -= levelData.quantity_;
    }

    return false;
}

bool Orderbook::CanMatch(Side side, Price price) const
{
    if (side == Side::Buy)
    {
        if (asks_.empty())
        {
            return false;
        }

        const auto &[bestAsk, _] = *asks_.begin();
        return price >= bestAsk;
    }

    if (bids_.empty())
    {
        return false;
    }

    const auto &[bestBid, _] = *bids_.begin();
    return price <= bestBid;
}

Trades Orderbook::MatchOrders()
{
    // Matching engine lands in 0.5: price-time priority, FAK/FOK/Market semantics.
    return {};
}
