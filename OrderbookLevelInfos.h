#pragma once

#include "LevelInfo.h"

// Read-only snapshot of the book's aggregate state, one LevelInfo per price level.
class OrderbookLevelInfos
{
public:
    OrderbookLevelInfos(const LevelInfos &bids, const LevelInfos &asks) : bids_{bids}, asks_{asks} {}

    [[nodiscard]] constexpr const LevelInfos &GetBids() const noexcept { return bids_; }
    [[nodiscard]] constexpr const LevelInfos &GetAsks() const noexcept { return asks_; }

private:
    LevelInfos bids_;
    LevelInfos asks_;
};
