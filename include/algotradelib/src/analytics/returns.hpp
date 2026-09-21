#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>

namespace algotradelib {
namespace analytics {

/**
 * Returns computed from a price series.
 *
 * Two conventions that every function in these layers keeps, because together they are
 * what stops an indicator from quietly looking ahead:
 *
 * 1. **The result has the same length as the input.** Row `i` of the output describes row
 *    `i` of the input, so a result binds to the same `TimeIndex` with no shifting. A
 *    function that returned a shorter series would make the caller do the alignment, and
 *    an off-by-one there is a lookahead bug that no test of the function itself can catch.
 * 2. **A value that cannot be computed is `missing()`, never zero and never carried
 *    forward.** The first row of a return series has no prior price, so it is unobserved.
 *    Zero would be a claim the price did not move.
 */

/**
 * Returns the simple period-over-period return, `p[i] / p[i - 1] - 1`.
 *
 * Row 0 is unobserved, as is any row whose own price or prior price is unobserved.
 *
 * A prior price of exactly zero yields an unobserved return rather than an infinity: the
 * return is undefined there, and propagating an infinity would survive every later
 * reduction that masks `NaN` but not `inf`.
 */
inline math::Vector returns(math::SeriesView prices) {
  const auto rows = prices.size();
  math::Vector result = math::unobserved(static_cast<std::size_t>(rows));
  if (rows < 2) {
    return result;
  }
  // One vectorized division over the whole series, then the guards. An unobserved price on either
  // side makes the quotient NaN by itself, which is already the answer; a zero prior price would
  // make it an infinity, so that row is replaced explicitly.
  const auto current = prices.tail(rows - 1).array();
  const auto previous = prices.head(rows - 1).array();
  result.tail(rows - 1) =
    (previous == Scalar{0}).select(missing(), current / previous - Scalar{1});
  return result;
}

/**
 * Returns the continuously compounded return, `log(p[i] / p[i - 1])`.
 *
 * Preferred over `returns` wherever returns are summed — over time, or across a
 * portfolio's rebalancing periods — because log returns add where simple returns compound.
 * The cost is that they do not aggregate across *instruments* at a point in time.
 *
 * A non-positive price on either side yields an unobserved return. A price series should
 * not contain one, but an adjusted series with a data error can, and a `log` of it would
 * produce a `NaN` from the standard library with no indication of why.
 */
inline math::Vector log_returns(math::SeriesView prices) {
  const auto rows = prices.size();
  math::Vector result = math::unobserved(static_cast<std::size_t>(rows));
  if (rows < 2) {
    return result;
  }
  const auto current = prices.tail(rows - 1).array();
  const auto previous = prices.head(rows - 1).array();
  // A non-positive price on either side is refused before the logarithm rather than left to
  // produce a bare NaN with nothing to say which row or which side caused it.
  const auto usable = (previous > Scalar{0}) && (current > Scalar{0});
  result.tail(rows - 1) = usable.select((current / previous).log(), missing());
  return result;
}

/**
 * Returns the cumulative simple return implied by a series of period returns.
 *
 * Row `i` is `prod(1 + r[0..i]) - 1`, the total return an investment held from the start
 * of the series through row `i` would have earned.
 *
 * An unobserved period return is **skipped** — treated as a period the series does not
 * describe, not as a period of zero return — which is the masking convention `missing()`
 * documents. A row before the first observed return is itself unobserved, because no
 * capital has been at risk yet.
 *
 * Takes simple returns, not log returns. Feeding it log returns compounds them twice; the
 * cumulative log return is `sum`, which needs no function of its own.
 */
inline math::Vector cumulative_returns(math::SeriesView period_returns) {
  math::Vector result = math::unobserved(static_cast<std::size_t>(period_returns.size()));
  Scalar growth{1};
  bool started = false;
  for (math::Index row = 0; row < period_returns.size(); ++row) {
    const Scalar value = period_returns[row];
    if (observed(value)) {
      growth *= Scalar{1} + value;
      started = true;
    }
    if (started) {
      result[row] = growth - Scalar{1};
    }
  }
  return result;
}

} // namespace analytics
} // namespace algotradelib
