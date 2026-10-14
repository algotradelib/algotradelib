#pragma once

#include "./numeric.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Conversions between exact `Decimal64` values and the analytics `Scalar`.
 *
 * Use `to_scalar` to analyze a price or quantity, and `round_scalar_to_tick` to turn a
 * computed value back into an order price.
 */

/**
 * @brief Converts an exact decimal to the nearest `Scalar`.
 *
 * The coefficient is divided once by `10^scale`. Within 53 bits of coefficient (every price and
 * quantity the bundled brokers quote) and 22 decimal places, both operands are exact, so in IEEE
 * double arithmetic's default round-to-nearest mode the result is the nearest `double`: the same
 * value `std::strtod` or Python's `float` gives for the decimal's text (`Decimal64(7, 2)` gives
 * `0.07`). Beyond either bound it is an approximation.
 *
 * @param value The decimal to convert.
 * @return The nearest `Scalar` to `value`, within the bounds above.
 * @warning Do not send the result back to a broker as is; use `round_scalar_to_tick`.
 */
inline Scalar to_scalar(const Decimal64& value) {
  // Every power of ten up to 10^22 is a double, so this product is exact over that range.
  Scalar divisor{1};
  for (std::uint8_t step = 0; step < value.scale(); ++step) {
    divisor *= Scalar(10);
  }
  return static_cast<Scalar>(value.coefficient()) / divisor;
}

/**
 * @brief Converts a `Scalar` to an exact decimal with a given number of decimal places.
 *
 * Rounds half away from zero. The result keeps exactly `scale` decimal places, trailing zeros
 * included: `to_decimal(1.5, 3)` is `1.500`, which is numerically equal to
 * `Decimal64::parse("1.5")` but not `==` to it, because `==` also compares the scale.
 *
 * @param value The value to convert.
 * @param scale The number of decimal places of the result.
 * @return The decimal, or `std::nullopt` if `value` is not finite (for example `missing()`)
 *   or too large for `Decimal64`.
 * @note For an order price use `round_scalar_to_tick`, which also puts the price on the
 *   exchange's tick grid.
 */
inline std::optional<Decimal64> to_decimal(Scalar value, std::uint8_t scale) {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  Scalar scaled = value;
  for (std::uint8_t step = 0; step < scale; ++step) {
    scaled *= Scalar(10);
  }
  // `std::round` is half away from zero by definition. Adding 0.5 first rounds the sum: 0.49999999999999994
  // became 1, and an odd integer near 2^52 moved up by one.
  const Scalar rounded = std::round(scaled);
  // `INT64_MAX` is not a double: it converts to 2^63, which does not fit, so that bound is exclusive.
  if (
    rounded >= static_cast<Scalar>(std::numeric_limits<std::int64_t>::max()) ||
    rounded < static_cast<Scalar>(std::numeric_limits<std::int64_t>::min())
  ) {
    return std::nullopt;
  }
  return Decimal64(static_cast<std::int64_t>(rounded), scale);
}

/**
 * @brief Converts a computed value into a price an order can carry.
 *
 * The value is rounded to a decimal at `scale` in the order's favour (a buy down, a sell up), then
 * onto the tick grid the same way, so a buy is never above the computed value (by more than
 * floating-point noise) and a sell never below it: with the KRX grid and `scale = 0`, a buy at
 * `1999.8` gives `1999` and a sell at `2000.2` gives `2005`. A value within floating-point noise of a
 * decimal counts as that decimal, so a market price converted from a `Decimal64` comes back
 * unchanged (a coefficient beyond 2^52 may not).
 *
 * @param value The computed price.
 * @param rule The instrument's tick rule.
 * @param side The side of the order the price is for.
 * @param scale The number of decimal places the market quotes in (`0` for KRW).
 * @return The price, or `std::nullopt` if `value` is not finite (such as an indicator's
 *   warm-up `missing()`), does not fit a `Decimal64`, or `rule` has no grid
 *   (`PriceRule::Kind::UNKNOWN`).
 */
inline std::optional<Decimal64> round_scalar_to_tick(
  Scalar value,
  const PriceRule& rule,
  OrderSide side,
  std::uint8_t scale = 0
) {
  const auto nearest = to_decimal(value, scale);
  if (!nearest) {
    return std::nullopt;
  }
  // `to_decimal` rounds to nearest, which can cross the computed value against the order. Step one
  // unit back when it did by more than floating-point noise (a few units in the last place).
  Scalar scaled = value;
  for (std::uint8_t step = 0; step < scale; ++step) {
    scaled *= Scalar(10);
  }
  // Each of the `scale` multiplications here can add half a unit in the last place, and a value that
  // came through `to_scalar` within its bounds carries one more half unit from its division. The
  // tolerance is wider than that sum: it also absorbs values computed from such inputs.
  const Scalar noise = std::max(
    Scalar(1e-9),
    std::abs(scaled) * Scalar(2 * scale + 4) * std::numeric_limits<Scalar>::epsilon()
  );
  const auto coefficient = nearest->coefficient();
  const Scalar crossed = static_cast<Scalar>(coefficient) - scaled;
  auto exact = *nearest;
  if (side == OrderSide::BUY && crossed > noise) {
    if (coefficient == std::numeric_limits<std::int64_t>::min()) {
      return std::nullopt;
    }
    exact = Decimal64(coefficient - 1, scale);
  } else if (side == OrderSide::SELL && -crossed > noise) {
    if (coefficient == std::numeric_limits<std::int64_t>::max()) {
      return std::nullopt;
    }
    exact = Decimal64(coefficient + 1, scale);
  }
  return rule.round_to_tick(exact, side);
}

} // namespace analytics
} // namespace algotradelib
