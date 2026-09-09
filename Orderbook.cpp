#include "Orderbook.h"

#include <algorithm>
#include <cassert>
#include <iterator>

Orderbook::Orderbook(OrderIndex capacity)
    : bids_{LevelReserve}, asks_{LevelReserve}, pool_{capacity}, orders_(capacity)
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
        if (order.GetSide() == Side::Buy && !asks_.Empty())
        {
            if (!order.ToGoodTillCancel(asks_.WorstPrice()))
            {
                return {};
            }
        }
        else if (order.GetSide() == Side::Sell && !bids_.Empty())
        {
            if (!order.ToGoodTillCancel(bids_.WorstPrice()))
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
    const Side side = pool_[slot].GetSide();
    if (side == Side::Buy)
    {
        Rest(bids_, slot);
    }
    else
    {
        Rest(asks_, slot);
    }

    return MatchOrders(side);
}

template <typename Levels>
void Orderbook::Rest(Levels &levels, OrderIndex slot)
{
    const Order &order = pool_[slot];

    // FindOrCreate may shift this side's levels: take the reference after it and
    // hold none across it.
    Level &level = levels.FindOrCreate(order.GetPrice());
    level.orders_.push_back(slot);
    level.quantity_ += order.GetRemainingQuantity();
    ++level.count_;

    orders_[order.GetOrderId()] = OrderEntry{slot, std::prev(level.orders_.end())};
}

void Orderbook::CancelOrder(OrderId orderId)
{
    if (orderId >= orders_.size() || orders_[orderId].slot_ == Constants::InvalidIndex)
    {
        return;
    }

    // Copy out, then destroy: the Order dies when its slot is freed, so
    // everything bookkeeping needs is read before Free.
    const auto [slot, location] = orders_[orderId];
    const Order &order = pool_[slot];
    const auto price = order.GetPrice();
    const auto side = order.GetSide();
    const auto remaining = order.GetRemainingQuantity();

    // Reset discipline: a removed id must read as not-live immediately, or a
    // stale id would later reach a recycled slot holding a stranger's order.
    orders_[orderId].slot_ = Constants::InvalidIndex;
    pool_.Free(slot);

    if (side == Side::Buy)
    {
        Unrest(bids_, price, remaining, location);
    }
    else
    {
        Unrest(asks_, price, remaining, location);
    }
}

template <typename Levels>
void Orderbook::Unrest(Levels &levels, Price price, Quantity remaining, OrderList::iterator location)
{
    Level *const level = levels.Find(price);
    assert(level != nullptr);

    level->orders_.erase(location);
    level->quantity_ -= remaining;
    if (--level->count_ == 0)
    {
        levels.Erase(*level);
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
    bidInfos.reserve(bids_.Size());
    askInfos.reserve(asks_.Size());

    // Aggregates are kept per level, so a snapshot is O(levels), not O(orders).
    for (const auto &[price, level] : bids_.BestFirst())
    {
        bidInfos.push_back(LevelInfo{price, level.quantity_});
    }

    for (const auto &[price, level] : asks_.BestFirst())
    {
        askInfos.push_back(LevelInfo{price, level.quantity_});
    }

    return OrderbookLevelInfos{bidInfos, askInfos};
}

template <typename Levels>
bool Orderbook::Covers(const Levels &opposite, Price limit, Quantity quantity)
{
    // Sorted levels: walk out from the touch and stop at the first level the
    // limit doesn't reach — every level past it is worse still.
    for (const auto &[levelPrice, level] : opposite.BestFirst())
    {
        if (Levels::IsBetter(limit, levelPrice))
        {
            break;
        }

        if (quantity <= level.quantity_)
        {
            return true;
        }

        quantity -= level.quantity_;
    }

    return false;
}

bool Orderbook::CanFullyFill(Side side, Price price, Quantity quantity) const
{
    return side == Side::Buy ? Covers(asks_, price, quantity) : Covers(bids_, price, quantity);
}

bool Orderbook::CanMatch(Side side, Price price) const
{
    if (side == Side::Buy)
    {
        return !asks_.Empty() && price >= asks_.BestPrice();
    }

    return !bids_.Empty() && price <= bids_.BestPrice();
}

Trades Orderbook::MatchOrders(Side takerSide)
{
    // Most adds don't cross the book: bail out before Trades allocates anything.
    if (bids_.Empty() || asks_.Empty() || bids_.BestPrice() < asks_.BestPrice())
    {
        return {};
    }

    // The book is never crossed at rest, so the crossing order is the taker, and
    // it is alone at its level: the front of its side's best level.
    const Level &takerLevel = takerSide == Side::Buy ? bids_.Best() : asks_.Best();
    const Level &makerLevel = takerSide == Side::Buy ? asks_.Best() : bids_.Best();
    const Quantity takerQuantity = pool_[takerLevel.orders_.front()].GetRemainingQuantity();

    Trades trades;
    // Common case: the taker is consumed within the best opposite level, so
    // trades <= min(taker quantity, orders resting there). A deeper sweep grows
    // the vector geometrically — rare, and never a capacity-sized reservation.
    trades.reserve(std::min<std::size_t>(takerQuantity, makerLevel.count_));

    while (!bids_.Empty() && !asks_.Empty() && bids_.BestPrice() >= asks_.BestPrice())
    {
        // Valid until this side creates or erases a level: nothing in the inner
        // loop does, and PopBest below comes after the last use.
        Level &bidLevel = bids_.Best();
        Level &askLevel = asks_.Best();

        while (!bidLevel.orders_.empty() && !askLevel.orders_.empty())
        {
            const OrderIndex bidSlot = bidLevel.orders_.front();
            const OrderIndex askSlot = askLevel.orders_.front();

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

            // Aggregates live on the level: no lookup, and the Level is already hot.
            // executionPrice is a reporting concept; the quantity left each order's
            // own level.
            bidLevel.quantity_ -= quantity;
            askLevel.quantity_ -= quantity;

            // Free last: bid/ask reference the slots being released — nothing may
            // read them after Free. Same reset discipline as CancelOrder: the id
            // must stop reading as live before its slot can be recycled.
            if (bid.IsFilled())
            {
                orders_[bid.GetOrderId()].slot_ = Constants::InvalidIndex;
                pool_.Free(bidSlot);
                bidLevel.orders_.pop_front();
                --bidLevel.count_;
            }

            if (ask.IsFilled())
            {
                orders_[ask.GetOrderId()].slot_ = Constants::InvalidIndex;
                pool_.Free(askSlot);
                askLevel.orders_.pop_front();
                --askLevel.count_;
            }
        }

        // An emptied level dies at the touch: one pop_back per side, no heap.
        if (bidLevel.orders_.empty())
        {
            bids_.PopBest();
        }

        if (askLevel.orders_.empty())
        {
            asks_.PopBest();
        }
    }

    // A partially filled FillAndKill never rests: it is the taker, still alone at
    // the front of its side's best level. (In the legacy code this public
    // CancelOrder call self-deadlocked on the held non-recursive mutex; it is
    // plainly safe in the single-threaded book.)
    const bool takerSideEmpty = takerSide == Side::Buy ? bids_.Empty() : asks_.Empty();
    if (!takerSideEmpty)
    {
        const Level &best = takerSide == Side::Buy ? bids_.Best() : asks_.Best();
        const Order &remainder = pool_[best.orders_.front()];
        if (remainder.GetOrderType() == OrderType::FillAndKill)
        {
            CancelOrder(remainder.GetOrderId());
        }
    }

    return trades;
}
