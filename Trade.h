#pragma once

#include <vector>

#include "TradeInfo.h"

class Trade
{
public:
    Trade(const TradeInfo &bidTrade, const TradeInfo &askTrade) : bidTrade_{bidTrade}, askTrade_{askTrade} {}

    [[nodiscard]] constexpr const TradeInfo &GetBidTrade() const noexcept { return bidTrade_; }
    [[nodiscard]] constexpr const TradeInfo &GetAskTrade() const noexcept { return askTrade_; }

private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;
};

using Trades = std::vector<Trade>;
