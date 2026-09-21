#pragma once

#include <algotradelib/core>

#include "./numeric.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace algotradelib {
namespace analytics {

/**
 * The bridge between the trading domain and the numeric one.
 *
 * `Decimal64` holds what a broker said, exactly. `math::Scalar` holds what a computation
 * needs, approximately. Nothing else in the library converts between them, which is what
 * makes the loss auditable — and why the return direction is narrower than the outward
 * one.
 */

/**
 * Converts an exact decimal to the numeric scalar.
 *
 * **This is lossy and one-directional by intent.** A `Decimal64` holds a value
 * exactly; a `Scalar` holds the nearest representable binary approximation of it. For
 * analysis that is the right trade — you cannot compute a standard deviation in
 * fixed point without inventing a scale — but the result must never travel back to a
 * broker without passing through the gate below.
 *
 * Every price this library has seen converts without meaningful loss: a
 * `Decimal64` whose coefficient fits in 53 bits is exact in `double`, which covers
 * every KRW price and every quantity a caller can place. Beyond that the conversion
 * rounds, silently, which is why the reverse direction is not symmetric.
 */
inline Scalar to_scalar(const Decimal64& value) {
  Scalar result = static_cast<Scalar>(value.coefficient());
  for (std::uint8_t step = 0; step < value.scale(); ++step) {
    result /= Scalar(10);
  }
  return result;
}

/**
 * Converts an analytics scalar back to an exact decimal at `scale`.
 *
 * Rounds half away from zero, which is the rule a reader expects and the one that does
 * not bias a series of conversions toward even values.
 *
 * Returns nothing for a value that is not finite, or one too large for `Decimal64`.
 * An indicator legitimately produces `missing()` in its warm-up region, and that is
 * not a number any decimal can represent.
 *
 * The result carries exactly the `scale` asked for, trailing zeros included, because
 * the parameter is the caller stating the precision the market quotes in. That differs
 * on purpose from the exact arithmetic in `core`, which normalizes: `decimal_add`
 * produces a value to compare against a parsed literal, while this produces a value to
 * *send*. One consequence is worth knowing — `to_decimal(1.5, 3)` is `1.500`, which is
 * numerically equal to `Decimal64::parse("1.5")` but not `==` to it, since equality
 * compares the scale too. Compare with `decimal_subtract` or on `str()` at a known
 * scale rather than with `==` across scales.
 *
 * **This is not the path to an order price.** Use `round_scalar_to_tick`, which
 * additionally puts the result on the exchange's grid. A price that is correct to four
 * decimal places and off the tick ladder is rejected by the exchange just as surely as
 * a wrong one.
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
 * Converts a computed value into a price an order may actually carry.
 *
 * This is the only sanctioned route from the analytics layer back to a broker, and it
 * is deliberately narrow. Three things must hold, and each returns nothing rather than
 * an approximation:
 *
 * 1. The value is finite. An indicator's warm-up region is `missing()`, and a strategy
 *    that has not warmed up must not place an order.
 * 2. It fits an exact decimal at the scale the market quotes in.
 * 3. It sits on the exchange's tick grid, in the direction that cannot cross the
 *    caller's instruction — a buy rounds down, a sell rounds up.
 *
 * `rule` refusing to answer is itself a refusal here: `PriceRule::Kind::UNKNOWN` means
 * the library has no grid for the instrument, and a caller that cannot round cannot
 * safely send a computed price.
 *
 * There is no compile-time barrier stopping someone assigning `to_decimal` straight
 * into an `OrderRequest`; making that ill-formed would mean a distinct price type
 * threaded through the whole merged order API. This is the documented path, the one the
 * runner will use, and the only one in the library that mentions both a `Scalar` and a
 * tick.
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
