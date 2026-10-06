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
 * @brief Converts an exact decimal to a floating-point approximation.
 *
 * The coefficient is cast to `Scalar` (exact up to 53 bits, which covers every KRW price and order
 * quantity) and then divided by ten once per decimal place; each division can round, so a value
 * with a fractional part may not be the nearest `double` (`Decimal64(7, 2)` gives
 * `0.069999999999999993`). A scale-zero value within 53 bits converts exactly.
 *
 * @param value The decimal to convert.
 * @return A floating-point approximation of `value`.
 * @warning Do not send the result back to a broker as is; use `round_scalar_to_tick`.
 */
inline Scalar to_scalar(const Decimal64& value) {
  Scalar result = static_cast<Scalar>(value.coefficient());
  for (std::uint8_t step = 0; step < value.scale(); ++step) {
    result /= Scalar(10);
  }
  return result;
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
  const Scalar rounded = scaled < 0 ? std::ceil(scaled - Scalar(0.5)) : std::floor(scaled + Scalar(0.5));
  if (
    rounded > static_cast<Scalar>(std::numeric_limits<std::int64_t>::max()) ||
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
 * onto the tick grid the same way, so a buy is never above the computed value and a sell never below
 * it: with the KRX grid and `scale = 0`, a buy at `1999.8` gives `1999` and a sell at `2000.2` gives
 * `2005`. A value within a few units of floating-point
 * precision of a decimal counts as that decimal, so a price converted from a `Decimal64` comes back
 * unchanged.
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
  const Scalar noise =
    std::max(Scalar(1e-9), std::abs(scaled) * Scalar(4) * std::numeric_limits<Scalar>::epsilon());
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
