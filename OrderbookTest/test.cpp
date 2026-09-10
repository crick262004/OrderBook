#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Constants.h"
#include "Order.h"
#include "OrderModify.h"
#include "OrderPool.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "PriceLevels.h"
#include "Side.h"
#include "Trade.h"
#include "Usings.h"

// File-driven scenarios: each TestFiles/*.txt is a script of actions replayed
// against a fresh Orderbook, then checked against expected trades and book state.
//
//   A <B|S> <OrderType> <price> <quantity> <orderId>   add an order
//   M <orderId> <B|S> <price> <quantity>               modify (cancel + re-add)
//   C <orderId>                                        cancel
//   P                                                  prune GoodForDay orders (close of day)
//   T <bidId> <bidPrice> <askId> <askPrice> <quantity> expected trade, in order
//   R <orderCount> <bidLevelCount> <askLevelCount>     expected final book (last line)
//
// The parser throws on malformed input: this is cold, test-only code, and a broken
// scenario file should abort the run loudly (KB rule 5 governs the engine, not tests).

namespace
{

enum class ActionType : std::uint8_t
{
    Add,
    Cancel,
    Modify,
    Prune,
};

struct Action
{
    ActionType type_{};
    OrderType orderType_{};
    Side side_{};
    Price price_{};
    Quantity quantity_{};
    OrderId orderId_{};
};

struct ExpectedTrade
{
    OrderId bidOrderId_;
    Price bidPrice_;
    OrderId askOrderId_;
    Price askPrice_;
    Quantity quantity_;
};

struct ExpectedResult
{
    std::size_t orderCount_;
    std::size_t bidLevelCount_;
    std::size_t askLevelCount_;
};

struct Scenario
{
    std::vector<Action> actions_;
    std::vector<ExpectedTrade> trades_;
    ExpectedResult result_{};
};

std::vector<std::string_view> Split(std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t start = 0;

    while (start < line.size())
    {
        const auto end = line.find(' ', start);
        if (end == std::string_view::npos)
        {
            tokens.push_back(line.substr(start));
            break;
        }
        if (end != start)
        {
            tokens.push_back(line.substr(start, end - start));
        }
        start = end + 1;
    }

    return tokens;
}

std::uint64_t ToNumber(std::string_view token)
{
    std::uint64_t value{};
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size())
    {
        throw std::logic_error{"Malformed number: " + std::string{token}};
    }
    return value;
}

Side ParseSide(std::string_view token)
{
    if (token == "B")
    {
        return Side::Buy;
    }
    if (token == "S")
    {
        return Side::Sell;
    }
    throw std::logic_error{"Unknown side: " + std::string{token}};
}

OrderType ParseOrderType(std::string_view token)
{
    if (token == "GoodTillCancel")
    {
        return OrderType::GoodTillCancel;
    }
    if (token == "GoodForDay")
    {
        return OrderType::GoodForDay;
    }
    if (token == "FillAndKill")
    {
        return OrderType::FillAndKill;
    }
    if (token == "FillOrKill")
    {
        return OrderType::FillOrKill;
    }
    if (token == "Market")
    {
        return OrderType::Market;
    }
    throw std::logic_error{"Unknown order type: " + std::string{token}};
}

void RequireTokens(const std::vector<std::string_view> &tokens, std::size_t expected, const std::string &line)
{
    if (tokens.size() != expected)
    {
        throw std::logic_error{"Malformed line: " + line};
    }
}

Scenario ParseScenario(const std::filesystem::path &path)
{
    std::ifstream file{path};
    if (!file)
    {
        throw std::logic_error{"Cannot open scenario file: " + path.string()};
    }

    Scenario scenario;
    bool haveResult = false;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (haveResult)
        {
            throw std::logic_error{"R must be the last line: " + path.string()};
        }

        const auto tokens = Split(line);

        switch (line.front())
        {
        case 'A':
        {
            RequireTokens(tokens, 6, line);
            Action action;
            action.type_ = ActionType::Add;
            action.side_ = ParseSide(tokens[1]);
            action.orderType_ = ParseOrderType(tokens[2]);
            action.price_ = static_cast<Price>(ToNumber(tokens[3]));
            action.quantity_ = static_cast<Quantity>(ToNumber(tokens[4]));
            action.orderId_ = ToNumber(tokens[5]);
            scenario.actions_.push_back(action);
            break;
        }
        case 'M':
        {
            RequireTokens(tokens, 5, line);
            Action action;
            action.type_ = ActionType::Modify;
            action.orderId_ = ToNumber(tokens[1]);
            action.side_ = ParseSide(tokens[2]);
            action.price_ = static_cast<Price>(ToNumber(tokens[3]));
            action.quantity_ = static_cast<Quantity>(ToNumber(tokens[4]));
            scenario.actions_.push_back(action);
            break;
        }
        case 'C':
        {
            RequireTokens(tokens, 2, line);
            Action action;
            action.type_ = ActionType::Cancel;
            action.orderId_ = ToNumber(tokens[1]);
            scenario.actions_.push_back(action);
            break;
        }
        case 'P':
        {
            RequireTokens(tokens, 1, line);
            Action action;
            action.type_ = ActionType::Prune;
            scenario.actions_.push_back(action);
            break;
        }
        case 'T':
        {
            RequireTokens(tokens, 6, line);
            scenario.trades_.push_back(ExpectedTrade{ToNumber(tokens[1]), static_cast<Price>(ToNumber(tokens[2])),
                                                     ToNumber(tokens[3]), static_cast<Price>(ToNumber(tokens[4])),
                                                     static_cast<Quantity>(ToNumber(tokens[5]))});
            break;
        }
        case 'R':
        {
            RequireTokens(tokens, 4, line);
            scenario.result_ = ExpectedResult{ToNumber(tokens[1]), ToNumber(tokens[2]), ToNumber(tokens[3])};
            haveResult = true;
            break;
        }
        default:
            throw std::logic_error{"Unknown line: " + line};
        }
    }

    if (!haveResult)
    {
        throw std::logic_error{"No R line in scenario file: " + path.string()};
    }

    return scenario;
}

class OrderbookScenarioTest : public testing::TestWithParam<const char *>
{
};

TEST_P(OrderbookScenarioTest, ReplaysFileScenario)
{
    // Arrange
    const auto file = std::filesystem::path{TEST_FILES_DIR} / GetParam();
    const auto scenario = ParseScenario(file);

    // Act
    Orderbook orderbook;
    Trades trades;

    for (const auto &action : scenario.actions_)
    {
        switch (action.type_)
        {
        case ActionType::Add:
        {
            const auto newTrades = orderbook.AddOrder(
                Order{action.orderType_, action.orderId_, action.side_, action.price_, action.quantity_});
            trades.insert(trades.end(), newTrades.begin(), newTrades.end());
            break;
        }
        case ActionType::Modify:
        {
            const auto newTrades =
                orderbook.ModifyOrder(OrderModify{action.orderId_, action.side_, action.price_, action.quantity_});
            trades.insert(trades.end(), newTrades.begin(), newTrades.end());
            break;
        }
        case ActionType::Cancel:
            orderbook.CancelOrder(action.orderId_);
            break;
        case ActionType::Prune:
            orderbook.PruneGoodForDayOrders();
            break;
        }
    }

    // Assert trade contents, not just final counts (legacy bug #6).
    ASSERT_EQ(trades.size(), scenario.trades_.size());
    for (std::size_t i = 0; i < trades.size(); ++i)
    {
        const auto &expected = scenario.trades_[i];
        const auto &bid = trades[i].GetBidTrade();
        const auto &ask = trades[i].GetAskTrade();
        EXPECT_EQ(bid.orderId_, expected.bidOrderId_) << "trade " << i;
        EXPECT_EQ(bid.price_, expected.bidPrice_) << "trade " << i;
        EXPECT_EQ(bid.quantity_, expected.quantity_) << "trade " << i;
        EXPECT_EQ(ask.orderId_, expected.askOrderId_) << "trade " << i;
        EXPECT_EQ(ask.price_, expected.askPrice_) << "trade " << i;
        EXPECT_EQ(ask.quantity_, expected.quantity_) << "trade " << i;
    }

    const auto levels = orderbook.GetOrderInfos();
    EXPECT_EQ(orderbook.Size(), scenario.result_.orderCount_);
    EXPECT_EQ(levels.GetBids().size(), scenario.result_.bidLevelCount_);
    EXPECT_EQ(levels.GetAsks().size(), scenario.result_.askLevelCount_);
}

constexpr const char *ScenarioFiles[] = {
    "Match_GoodTillCancel.txt", "Match_FillAndKill.txt",     "Match_FillAndKill_Partial.txt",
    "Match_FillOrKill_Hit.txt", "Match_FillOrKill_Miss.txt", "Match_Market.txt",
    "Match_PriceImprovement.txt", "Cancel_Success.txt",      "Modify_Side.txt",
    "Prune_GoodForDay.txt",     "Prune_NoGoodForDay.txt",
};

INSTANTIATE_TEST_SUITE_P(Scenarios, OrderbookScenarioTest, testing::ValuesIn(ScenarioFiles));

// Arena mechanics, tested against the pool directly: slot identity, LIFO
// recycling, and full-pool rejection (unreachable via Orderbook, whose id
// contract caps live orders at pool capacity).
TEST(OrderPoolTest, AllocatesUntilFullThenRejects)
{
    OrderPool pool{2};

    const auto first = pool.Alloc(Order{OrderType::GoodTillCancel, 0, Side::Buy, 100, 10});
    const auto second = pool.Alloc(Order{OrderType::GoodTillCancel, 1, Side::Buy, 101, 10});
    EXPECT_NE(first, Constants::InvalidIndex);
    EXPECT_NE(second, Constants::InvalidIndex);
    EXPECT_EQ(pool.Size(), 2u);

    EXPECT_EQ(pool.Alloc(Order{OrderType::GoodTillCancel, 2, Side::Buy, 102, 10}), Constants::InvalidIndex);
    EXPECT_EQ(pool.Size(), 2u);
}

TEST(OrderPoolTest, ReusesFreedSlotLifo)
{
    OrderPool pool{3};

    const auto first = pool.Alloc(Order{OrderType::GoodTillCancel, 0, Side::Buy, 100, 10});
    const auto second = pool.Alloc(Order{OrderType::GoodTillCancel, 1, Side::Sell, 101, 10});

    pool.Free(first);
    EXPECT_EQ(pool.Size(), 1u);

    // LIFO: the most recently freed slot is handed out first, and the survivor
    // is untouched by the recycling.
    const auto third = pool.Alloc(Order{OrderType::GoodTillCancel, 2, Side::Buy, 102, 7});
    EXPECT_EQ(third, first);
    EXPECT_EQ(pool[third].GetOrderId(), 2u);
    EXPECT_EQ(pool[third].GetRemainingQuantity(), 7u);
    EXPECT_EQ(pool[second].GetOrderId(), 1u);
}

// The capacity contract at the book's boundary: ids are dense in [0, capacity),
// duplicates and out-of-range ids are rejected with the book untouched.
TEST(OrderbookCapacityTest, RejectsIdAtOrBeyondCapacity)
{
    Orderbook orderbook{4};

    EXPECT_TRUE(orderbook.AddOrder(Order{OrderType::GoodTillCancel, 4, Side::Buy, 100, 10}).empty());
    EXPECT_EQ(orderbook.Size(), 0u);

    EXPECT_TRUE(orderbook.AddOrder(Order{OrderType::GoodTillCancel, 3, Side::Buy, 100, 10}).empty());
    EXPECT_EQ(orderbook.Size(), 1u);
}

TEST(OrderbookCapacityTest, RejectsDuplicateIdWhileLive)
{
    Orderbook orderbook{4};

    orderbook.AddOrder(Order{OrderType::GoodTillCancel, 1, Side::Buy, 100, 10});
    orderbook.AddOrder(Order{OrderType::GoodTillCancel, 1, Side::Buy, 105, 5});
    EXPECT_EQ(orderbook.Size(), 1u);

    const auto levels = orderbook.GetOrderInfos();
    ASSERT_EQ(levels.GetBids().size(), 1u);
    EXPECT_EQ(levels.GetBids().front().price_, 100);
}

TEST(OrderbookCapacityTest, IdReusableAfterCancel)
{
    Orderbook orderbook{2};

    orderbook.AddOrder(Order{OrderType::GoodTillCancel, 0, Side::Buy, 100, 10});
    orderbook.CancelOrder(0);
    EXPECT_EQ(orderbook.Size(), 0u);

    orderbook.AddOrder(Order{OrderType::GoodTillCancel, 0, Side::Sell, 105, 5});
    EXPECT_EQ(orderbook.Size(), 1u);

    const auto levels = orderbook.GetOrderInfos();
    EXPECT_TRUE(levels.GetBids().empty());
    ASSERT_EQ(levels.GetAsks().size(), 1u);
    EXPECT_EQ(levels.GetAsks().front().price_, 105);
}

// Flat level mechanics, tested against PriceLevels directly: sort direction per
// side, the touch at the back, mid-array birth/death, and growth past the reserve.
std::vector<Price> BestFirstPrices(const auto &levels)
{
    std::vector<Price> prices;
    for (const auto &[price, level] : levels.BestFirst())
    {
        prices.push_back(price);
    }
    return prices;
}

TEST(PriceLevelsTest, SortsTowardTheTouchOnBothSides)
{
    PriceLevels<std::greater<Price>> bids{4};
    PriceLevels<std::less<Price>> asks{4};

    for (const Price price : {100, 105, 95})
    {
        [[maybe_unused]] auto &bidLevel = bids.FindOrCreate(price);
        [[maybe_unused]] auto &askLevel = asks.FindOrCreate(price);
    }

    EXPECT_EQ(bids.BestPrice(), 105);
    EXPECT_EQ(bids.WorstPrice(), 95);
    EXPECT_EQ(BestFirstPrices(bids), (std::vector<Price>{105, 100, 95}));

    EXPECT_EQ(asks.BestPrice(), 95);
    EXPECT_EQ(asks.WorstPrice(), 105);
    EXPECT_EQ(BestFirstPrices(asks), (std::vector<Price>{95, 100, 105}));
}

TEST(PriceLevelsTest, FindOrCreateReturnsTheExistingLevel)
{
    PriceLevels<std::greater<Price>> bids{4};

    auto &created = bids.FindOrCreate(100);
    created.count_ = 1;
    created.quantity_ = 10;

    const auto &found = bids.FindOrCreate(100);
    EXPECT_EQ(bids.Size(), 1u);
    EXPECT_EQ(found.count_, 1u);
    EXPECT_EQ(found.quantity_, 10u);

    EXPECT_EQ(bids.Find(101), nullptr);
    ASSERT_NE(bids.Find(100), nullptr);
    EXPECT_EQ(bids.Find(100)->count_, 1u);
}

TEST(PriceLevelsTest, EraseMidArrayKeepsOrderAndGrowsPastReserve)
{
    PriceLevels<std::less<Price>> asks{2};

    for (const Price price : {103, 101, 105, 102, 104})
    {
        [[maybe_unused]] auto &level = asks.FindOrCreate(price);
    }
    EXPECT_EQ(asks.Size(), 5u);
    EXPECT_EQ(BestFirstPrices(asks), (std::vector<Price>{101, 102, 103, 104, 105}));

    asks.Erase(*asks.Find(103));
    EXPECT_EQ(asks.Find(103), nullptr);
    EXPECT_EQ(BestFirstPrices(asks), (std::vector<Price>{101, 102, 104, 105}));

    asks.PopBest();
    EXPECT_EQ(asks.BestPrice(), 102);
    EXPECT_EQ(asks.WorstPrice(), 105);
    EXPECT_EQ(asks.Size(), 3u);
}

// The intrusive FIFO: resting orders are their own queue nodes, linked through
// their pool slots. Tested against a pool and a bare Level.
std::vector<OrderIndex> QueueFrontToBack(const OrderPool &pool, const Level &level)
{
    std::vector<OrderIndex> slots;
    for (auto slot = level.head_; slot != Constants::InvalidIndex; slot = pool.Next(slot))
    {
        slots.push_back(slot);
    }
    return slots;
}

OrderIndex AllocResting(OrderPool &pool, OrderId orderId)
{
    return pool.Alloc(Order{OrderType::GoodTillCancel, orderId, Side::Buy, 100, 1});
}

TEST(LevelQueueTest, PushBackKeepsTimePriority)
{
    OrderPool pool{4};
    Level level;
    EXPECT_TRUE(level.Empty());

    const auto first = AllocResting(pool, 0);
    const auto second = AllocResting(pool, 1);
    const auto third = AllocResting(pool, 2);
    level.PushBack(pool, first);
    level.PushBack(pool, second);
    level.PushBack(pool, third);

    EXPECT_FALSE(level.Empty());
    EXPECT_EQ(level.head_, first);
    EXPECT_EQ(level.tail_, third);
    EXPECT_EQ(QueueFrontToBack(pool, level), (std::vector<OrderIndex>{first, second, third}));
}

TEST(LevelQueueTest, UnlinksHeadMiddleAndTailInPlace)
{
    OrderPool pool{4};
    Level level;

    const auto a = AllocResting(pool, 0);
    const auto b = AllocResting(pool, 1);
    const auto c = AllocResting(pool, 2);
    const auto d = AllocResting(pool, 3);
    for (const auto slot : {a, b, c, d})
    {
        level.PushBack(pool, slot);
    }

    // Middle: neighbours are re-stitched, the removed slot's links are reset.
    level.Unlink(pool, b);
    EXPECT_EQ(QueueFrontToBack(pool, level), (std::vector<OrderIndex>{a, c, d}));
    EXPECT_EQ(pool.Prev(b), Constants::InvalidIndex);
    EXPECT_EQ(pool.Next(b), Constants::InvalidIndex);

    level.Unlink(pool, a); // head
    EXPECT_EQ(level.head_, c);
    EXPECT_EQ(QueueFrontToBack(pool, level), (std::vector<OrderIndex>{c, d}));

    level.Unlink(pool, d); // tail
    EXPECT_EQ(level.tail_, c);
    EXPECT_EQ(QueueFrontToBack(pool, level), (std::vector<OrderIndex>{c}));

    level.Unlink(pool, c); // last one out
    EXPECT_TRUE(level.Empty());
    EXPECT_EQ(level.tail_, Constants::InvalidIndex);
}

TEST(LevelQueueTest, UnlinkedSlotRecyclesWithoutDisturbingTheQueue)
{
    OrderPool pool{4};
    Level level;

    const auto a = AllocResting(pool, 0);
    const auto b = AllocResting(pool, 1);
    const auto c = AllocResting(pool, 2);
    for (const auto slot : {a, b, c})
    {
        level.PushBack(pool, slot);
    }

    // Unlink, then Free: next_ becomes the free-list link only once the queue
    // no longer needs it. LIFO reuse hands the same slot back.
    level.Unlink(pool, b);
    pool.Free(b);
    const auto recycled = AllocResting(pool, 3);
    EXPECT_EQ(recycled, b);

    level.PushBack(pool, recycled);
    EXPECT_EQ(QueueFrontToBack(pool, level), (std::vector<OrderIndex>{a, c, recycled}));
    EXPECT_EQ(pool[recycled].GetOrderId(), 3u);
}

// Aggregates ride on the level: a snapshot must report the sum of remaining
// quantity after a partial fill, and a level must die with its last order.
TEST(OrderbookLevelTest, LevelQuantityTracksRemainingAfterPartialFill)
{
    Orderbook orderbook{4};

    orderbook.AddOrder(Order{OrderType::GoodTillCancel, 0, Side::Sell, 100, 10});
    const auto trades = orderbook.AddOrder(Order{OrderType::GoodTillCancel, 1, Side::Buy, 100, 30});
    ASSERT_EQ(trades.size(), 1u);

    auto levels = orderbook.GetOrderInfos();
    EXPECT_TRUE(levels.GetAsks().empty());
    ASSERT_EQ(levels.GetBids().size(), 1u);
    EXPECT_EQ(levels.GetBids().front().price_, 100);
    EXPECT_EQ(levels.GetBids().front().quantity_, 20u);

    orderbook.CancelOrder(1);
    levels = orderbook.GetOrderInfos();
    EXPECT_TRUE(levels.GetBids().empty());
    EXPECT_EQ(orderbook.Size(), 0u);
}

} // namespace
