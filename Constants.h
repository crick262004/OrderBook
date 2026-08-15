#pragma once

#include <limits>

#include "Usings.h"

namespace Constants
{
// Sentinel for "no price" (e.g. market orders). Must be unrepresentable as a
// real price: quiet_NaN() is meaningless for integer types (yields 0, a valid
// price), so the maximum representable Price is used instead.
inline constexpr Price InvalidPrice = std::numeric_limits<Price>::max();
} // namespace Constants
