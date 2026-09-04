#pragma once

#include <limits>

#include "Usings.h"

namespace Constants
{
// Sentinel for "no price" (e.g. market orders). Must be unrepresentable as a
// real price: quiet_NaN() is meaningless for integer types (yields 0, a valid
// price), so the maximum representable Price is used instead.
inline constexpr Price InvalidPrice = std::numeric_limits<Price>::max();

// Sentinel for "no slot": returned by a full pool, stored in the order index
// for ids that aren't live. Unreachable as a real slot (capacities are far
// below 2^32 - 1).
inline constexpr OrderIndex InvalidIndex = std::numeric_limits<OrderIndex>::max();
} // namespace Constants
