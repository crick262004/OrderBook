#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Order.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "Orderbook.h"
#include "Side.h"
#include "Trade.h"
#include "Usings.h"

// File-driven scenarios: each TestFiles/*.txt is a script of actions replayed
// against a fresh Orderbook, then checked against expected trades and book state.
//
//   A <B|S> <OrderType> <price> <quantity> <orderId>   add an order
//   M <orderId> <B|S> <price> <quantity>               modify (cancel + re-add)
//   C <orderId>                                        cancel
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
            const auto newTrades = orderbook.AddOrder(std::make_shared<Order>(
                action.orderType_, action.orderId_, action.side_, action.price_, action.quantity_));
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
    "Match_GoodTillCancel.txt", "Match_FillAndKill.txt",   "Match_FillAndKill_Partial.txt",
    "Match_FillOrKill_Hit.txt", "Match_FillOrKill_Miss.txt", "Match_Market.txt",
    "Cancel_Success.txt",       "Modify_Side.txt",
};

INSTANTIATE_TEST_SUITE_P(Scenarios, OrderbookScenarioTest, testing::ValuesIn(ScenarioFiles));

} // namespace
