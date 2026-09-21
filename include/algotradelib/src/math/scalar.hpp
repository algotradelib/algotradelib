#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace algotradelib {
namespace math {

/**
 * Numeric type this library computes in.
 *
 * `double` by default. It is an alias rather than a hard-coded type so a consumer can
 * build the numeric layers in `float` or `long double` without touching any code above
 * them; `double` is the default because covariance and regression on price levels lose
 * too much in `float`.
 *
 * This is deliberately *not* the type the trading domain uses. Prices, quantities, and
 * money stay `Decimal64` everywhere they cross a wire or are reconciled. The two meet
 * only at the conversions in `analytics/conversion.hpp`, which is the one place that
 * knows about both.
 */
using Scalar = double;

/**
 * The value standing for "no observation".
 *
 * One representation, used everywhere: an indicator's warm-up region, a gap in a panel,
 * a field a broker did not report. Every reduction in these layers masks it, so one
 * missing observation cannot poison a whole column.
 */
inline Scalar missing() { return std::numeric_limits<Scalar>::quiet_NaN(); }

/** Returns whether `value` carries an observation. */
inline bool observed(Scalar value) { return !std::isnan(value); }

/**
 * The value standing for "computed, and undefined here".
 *
 * A second kind of absence, distinct from `missing()`. `missing()` means no observation was
 * available — a warm-up row, a gap, a field a broker omitted. `undefined()` means the data
 * was there and the statistic does not exist for it: a z-score over a window with no
 * dispersion, a correlation against a constant, a Sortino ratio over a series that never fell
 * below its target, a beta against a market that did not move.
 *
 * Both are quiet `NaN`s, so `observed()` is false for both and every reduction masks both. The
 * difference is only readable through `is_undefined()`, and it matters where a caller would act
 * differently: ranking strategies, "not enough history yet" and "no downside risk at all" are
 * very different answers, and collapsing them loses the distinction exactly where it counts.
 *
 * **Reliable on a value a function returned, and not through arithmetic.** The distinction is
 * carried in the `NaN` payload, and IEEE-754 leaves the payload of a propagated `NaN`
 * implementation-defined: `undefined() + 1.0` is still a `NaN`, but need not still be *this*
 * one. So test it on what a function handed back, and treat anything computed from it as merely
 * absent. Storing it in a matrix and reading it out is a bit copy and preserves it.
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
 * Returns whether `value` is specifically the "undefined" absence rather than a plain gap.
 *
 * False for `missing()`, for any other `NaN`, and for every real number. See `undefined()` for
 * why this is meaningful only on a value returned directly by one of these functions.
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
