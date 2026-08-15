#pragma once

#include <cstdint>

enum class OrderType : std::uint8_t
{
    GoodTillCancel,
    FillAndKill,
    FillOrKill,
    GoodForDay,
    Market,
};
