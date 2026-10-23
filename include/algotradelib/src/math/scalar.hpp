#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace algotradelib {
namespace math {

/**
 * @brief The floating-point type the analytics and math layers compute in (`double`).
 *
 * Prices, quantities and money in orders and broker data stay `Decimal64`; convert between
 * the two with the functions in `analytics/conversion.hpp`.
 */
using Scalar = double;

/**
 * @brief Returns the value that stands for "no observation" (a quiet `NaN`).
 *
 * Used for an indicator's warm-up rows, a gap in a panel, or a field a broker did not
 * report. Every reduction in the analytics layer skips it.
 *
 * @return A quiet `NaN`.
 */
inline Scalar missing() { return std::numeric_limits<Scalar>::quiet_NaN(); }

/**
 * @brief Tests whether a value is an observation.
 *
 * @param value The value to test.
 * @return `false` for `missing()`, `undefined()` or any other `NaN`; `true` otherwise.
 */
inline bool observed(Scalar value) { return !std::isnan(value); }

/**
 * @brief Returns the value that stands for "the data was there, but the statistic does not exist".
 *
 * Examples: a z-score over a window with no dispersion, a Sortino ratio over a series that never
 * fell below its target. It differs from `missing()`,
 * which means there was no data. Both are quiet `NaN`s, so `observed()` is `false` for both and
 * reductions skip both; only `is_undefined()` tells them apart.
 *
 * @return A quiet `NaN` with a payload that `is_undefined()` recognizes.
 *
 * @warning Test `is_undefined()` on a value a function returned (or one copied from a matrix).
 *   Arithmetic on it, such as `undefined() + 1.0`, yields a `NaN` that may no longer be recognized.
 */
inline Scalar undefined() {
  // A quiet NaN with a distinguishing payload: exponent all ones, quiet bit set, and a
  // payload no library produces by accident.
  static_assert(sizeof(Scalar) == sizeof(std::uint64_t), "undefined() assumes a 64-bit Scalar");
  const std::uint64_t bits = 0x7FF80000000F1A7EULL;
  Scalar value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * @brief Tests whether a value is the `undefined()` marker.
 *
 * @param value The value to test.
 * @return `true` only for `undefined()` (including a negated copy); `false` for `missing()`,
 *   any other `NaN` and every number.
 * @see undefined() for when the marker survives.
 */
inline bool is_undefined(Scalar value) {
  if (!std::isnan(value)) {
    return false;
  }
  std::uint64_t bits{0};
  std::memcpy(&bits, &value, sizeof(bits));
  // Compares the payload only, masking off the sign, the exponent, and the quiet bit, so a
  // negated or re-quieted copy still reads as the same marker.
  return (bits & 0x0007FFFFFFFFFFFFULL) == 0x000F1A7EULL;
}

} // namespace math
} // namespace algotradelib
