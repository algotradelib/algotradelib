#pragma once

#include "./numeric.hpp"

#include <algotradelib/core>

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
 * The conversion is exact when the coefficient fits in 53 bits (every KRW price and order
 * quantity does) and rounds silently beyond that.
 *
 * @param value The decimal to convert.
 * @return The nearest floating-point value.
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
 * The value is converted with `to_decimal` and then rounded onto the exchange's tick grid
 * in the direction that never crosses the order's intent: a buy rounds down, a sell rounds up.
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
  const auto exact = to_decimal(value, scale);
  if (!exact) {
    return std::nullopt;
  }
  return rule.round_to_tick(*exact, side);
}

} // namespace analytics
} // namespace algotradelib
