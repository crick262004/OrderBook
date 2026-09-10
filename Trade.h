#pragma once

#include <vector>

#include "FunctionRef.h"
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

// How the book reports a fill: invoked once per trade, the instant it happens,
// with no container in between. Reporting is the caller's concern — a test
// collects into a vector, the engine pushes onto its outbound ring, a benchmark
// discards — so the book neither allocates for it nor bounds it.
using TradeSink = FunctionRef<void(const Trade &)>;
