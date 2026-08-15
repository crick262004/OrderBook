#pragma once

#include <cstdint>

// Hot-path error codes for order state transitions: 1 byte, no message, no allocation.
enum class OrderError : std::uint8_t
{
    Overfill,       // Fill() requested more than the remaining quantity
    NotMarketOrder, // ToGoodTillCancel() on an order that is not a market order
};
