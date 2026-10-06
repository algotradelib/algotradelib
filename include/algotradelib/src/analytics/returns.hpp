#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Simple, log and cumulative returns of a price series.
 *
 * Like every series function in `analytics`, each result is as long as its input (row `i`
 * describes row `i`, so it shares the input's `TimeIndex`), and a value that cannot be computed
 * is `missing()`, never zero.
 */

/**
 * @brief Computes simple period returns, `p[i] / p[i - 1] - 1`.
 *
 * @param prices The price series; `NaN` entries are unobserved.
 * @return The returns, one per row. Row 0 is `missing()`, as is any row whose price or previous
 *   price is unobserved, or whose previous price is zero.
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
 * @brief Computes log (continuously compounded) returns, `log(p[i] / p[i - 1])`.
 *
 * Log returns add over time; simple returns, unlike log returns, add across instruments at one
 * instant.
 *
 * @param prices The price series; `NaN` entries are unobserved.
 * @return The returns, one per row. Row 0 is `missing()`, as is any row where either price is
 *   unobserved or not positive.
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
 * @brief Compounds simple period returns into the cumulative return, `prod(1 + r[0..i]) - 1`.
 *
 * @param period_returns **Simple** returns (as from `returns`), not log returns; the cumulative
 *   log return is their plain sum. `NaN` entries are skipped.
 * @return The cumulative return through each row. Rows before the first observed return are
 *   `missing()`; a later unobserved row repeats the running total.
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
