#pragma once

#include <cmath>
#include <cstddef>
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

} // namespace math
} // namespace algotradelib
