#include "Orderbook.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <numeric>

Orderbook::Orderbook(OrderIndex capacity) : pool_{capacity}, orders_(capacity)
{
}

Trades Orderbook::AddOrder(Order order)
{
    // Capacity contract: the id is the position in the flat index, so an id at
    // or beyond capacity is rejected exactly like a duplicate.
    if (order.GetOrderId() >= orders_.size() || orders_[order.GetOrderId()].slot_ != Constants::InvalidIndex)
    {
        return {};
    }

    if (order.GetOrderType() == OrderType::Market)
    {
        // A market order pegs itself to the worst resting price on the opposite
        // side, so it sweeps every level the book can offer.
        if (order.GetSide() == Side::Buy && !asks_.empty())
        {
            const auto &[worstAsk, _] = *asks_.rbegin();
            if (!order.ToGoodTillCancel(worstAsk))
            {
                return {};
            }
        }
        else if (order.GetSide() == Side::Sell && !bids_.empty())
        {
            const auto &[worstBid, _] = *bids_.rbegin();
            if (!order.ToGoodTillCancel(worstBid))
            {
                return {};
            }
        }
        else
        {
            return {};
        }
    }

    if (order.GetOrderType() == OrderType::FillAndKill && !CanMatch(order.GetSide(), order.GetPrice()))
    {
        return {};
    }

    if (order.GetOrderType() == OrderType::FillOrKill &&
        !CanFullyFill(order.GetSide(), order.GetPrice(), order.GetInitialQuantity()))
    {
        return {};
    }

    const auto slot = pool_.Alloc(std::move(order));
    if (slot == Constants::InvalidIndex)
    {
        // Pool full: backpressure, the order is rejected with the book untouched.
        return {};
    }

    // The slot is the order's single home from here on; read via the pool,
    // never the moved-from parameter.
    const Order &resting = pool_[slot];

    auto &levelOrders = resting.GetSide() == Side::Buy ? bids_[resting.GetPrice()] : asks_[resting.GetPrice()];
    levelOrders.push_back(slot);

    orders_[resting.GetOrderId()] = OrderEntry{slot, std::prev(levelOrders.end())};

    OnOrderAdded(resting);

    return MatchOrders(resting.GetSide());
}

void Orderbook::CancelOrder(OrderId orderId)
{
    if (orderId >= orders_.size() || orders_[orderId].slot_ == Constants::InvalidIndex)
    {
        return;
    }

    // Copy out, then destroy: the Order dies when its slot is freed, so
    // everything bookkeeping needs is read before Free.
    const auto [slot, iterator] = orders_[orderId];
    const auto price = pool_[slot].GetPrice();
    const auto side = pool_[slot].GetSide();

    OnOrderCancelled(pool_[slot]);

    // Reset discipline: a removed id must read as not-live immediately, or a
    // stale id would later reach a recycled slot holding a stranger's order.
    orders_[orderId].slot_ = Constants::InvalidIndex;
    pool_.Free(slot);

    if (side == Side::Sell)
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
}

void Orderbook::PruneGoodForDayOrders()
{
    // Collect first, cancel second: CancelOrder erases from orders_, which would
    // invalidate the iterator mid-walk. Cold path (once per trading day), so the
    // vector allocation is acceptable.
    OrderIds goodForDayIds;

    // Walks the whole id space, not just live orders — O(capacity) is fine on
    // the once-per-day path, and it needs no auxiliary live-order structure.
    for (OrderId orderId = 0; orderId < orders_.size(); ++orderId)
    {
        const auto slot = orders_[orderId].slot_;
        if (slot != Constants::InvalidIndex && pool_[slot].GetOrderType() == OrderType::GoodForDay)
        {
            goodForDayIds.push_back(orderId);
        }
    }

    // Internal call into public CancelOrder
    // small trivial scalars → copy; anything with an expensive copy constructor (shared_ptr, string, vector) or big footprint → const&
    for (const auto orderId : goodForDayIds)
    {
        CancelOrder(orderId);
    }
}

Trades Orderbook::ModifyOrder(OrderModify order)
{
    if (order.GetOrderId() >= orders_.size() || orders_[order.GetOrderId()].slot_ == Constants::InvalidIndex)
    {
        return {};
    }

    const auto orderType = pool_[orders_[order.GetOrderId()].slot_].GetOrderType();

    CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrder(orderType));
}

std::size_t Orderbook::Size() const noexcept
{
    return pool_.Size();
}

OrderbookLevelInfos Orderbook::GetOrderInfos() const
{
    LevelInfos bidInfos;
    LevelInfos askInfos;
    bidInfos.reserve(bids_.size());
    askInfos.reserve(asks_.size());

    const auto createLevelInfo = [this](Price price, const OrderList &levelOrders)
    {
        return LevelInfo{price, std::accumulate(levelOrders.begin(), levelOrders.end(), Quantity{0},
                                                [this](Quantity runningSum, OrderIndex slot)
                                                { return runningSum + pool_[slot].GetRemainingQuantity(); })};
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

void Orderbook::OnOrderCancelled(const Order &order)
{
    UpdateLevelData(order.GetPrice(), order.GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(const Order &order)
{
    UpdateLevelData(order.GetPrice(), order.GetInitialQuantity(), LevelData::Action::Add);
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

Trades Orderbook::MatchOrders(Side takerSide)
{
    // Most adds don't cross the book: bail out before Trades allocates anything.
    if (bids_.empty() || asks_.empty() || bids_.begin()->first < asks_.begin()->first)
    {
        return {};
    }

    Trades trades;
    // Upper bound: every trade fully fills at least one resting order.
    trades.reserve(orders_.size());

    while (!bids_.empty() && !asks_.empty())
    {
        const auto bidsIt = bids_.begin();
        const auto asksIt = asks_.begin();

        if (bidsIt->first < asksIt->first)
        {
            break;
        }

        auto &bidOrders = bidsIt->second;
        auto &askOrders = asksIt->second;

        while (!bidOrders.empty() && !askOrders.empty())
        {
            const OrderIndex bidSlot = bidOrders.front();
            const OrderIndex askSlot = askOrders.front();

            // References, never copies: Fill must shrink the order resting in the
            // pool, not a stack duplicate the loop would then re-read forever.
            Order &bid = pool_[bidSlot];
            Order &ask = pool_[askSlot];

            const Quantity quantity = std::min(bid.GetRemainingQuantity(), ask.GetRemainingQuantity());

            // quantity is the min of both remainders, so neither Fill can overfill.
            [[maybe_unused]] const auto bidFilled = bid.Fill(quantity);
            [[maybe_unused]] const auto askFilled = ask.Fill(quantity);
            assert(bidFilled.has_value() && askFilled.has_value());

            // Trades execute at the maker's (resting order's) price: the book is never
            // crossed at rest, so the resting side is always opposite the incoming taker.
            const Price executionPrice = takerSide == Side::Buy ? ask.GetPrice() : bid.GetPrice();

            trades.push_back(Trade{TradeInfo{bid.GetOrderId(), executionPrice, quantity},
                                   TradeInfo{ask.GetOrderId(), executionPrice, quantity}});

            // Level bookkeeping keeps each order's own price: executionPrice is a
            // reporting concept, but the quantity left the level the order rests at.
            OnOrderMatched(bid.GetPrice(), quantity, bid.IsFilled());
            OnOrderMatched(ask.GetPrice(), quantity, ask.IsFilled());

            // Free last: bid/ask reference the slots being released — nothing may
            // read them after Free. Same reset discipline as CancelOrder: the id
            // must stop reading as live before its slot can be recycled.
            if (bid.IsFilled())
            {
                orders_[bid.GetOrderId()].slot_ = Constants::InvalidIndex;
                pool_.Free(bidSlot);
                bidOrders.pop_front();
            }

            if (ask.IsFilled())
            {
                orders_[ask.GetOrderId()].slot_ = Constants::InvalidIndex;
                pool_.Free(askSlot);
                askOrders.pop_front();
            }
        }

        // Level bookkeeping in data_ is owned by UpdateLevelData alone: the last
        // fill's Action::Remove already erased an emptied level's entry.
        if (bidOrders.empty())
        {
            bids_.erase(bidsIt);
        }

        if (askOrders.empty())
        {
            asks_.erase(asksIt);
        }
    }

    // A partially filled FillAndKill never rests: it is the incoming order, still at
    // the front of its side's best level. 
    // CancelOrder call self-deadlocked on the held non-recursive mutex; it is plainly
    // safe in the single-threaded book.
    if (!bids_.empty())
    {
        const Order &order = pool_[bids_.begin()->second.front()];
        if (order.GetOrderType() == OrderType::FillAndKill)
        {
            CancelOrder(order.GetOrderId());
        }
    }

    if (!asks_.empty())
    {
        const Order &order = pool_[asks_.begin()->second.front()];
        if (order.GetOrderType() == OrderType::FillAndKill)
        {
            CancelOrder(order.GetOrderId());
        }
    }

    return trades;
}
