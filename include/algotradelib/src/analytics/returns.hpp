#pragma once

#include "./numeric.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

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
 *   price is unobserved, or whose previous price is zero or negative (from a negative base the ratio's
 *   sign is reversed, so a further fall would read as a gain).
 */
inline math::Vector returns(math::SeriesView prices) {
  const auto rows = prices.size();
  math::Vector result = math::unobserved(static_cast<std::size_t>(rows));
  if (rows < 2) {
    return result;
  }
  // One vectorized division over the whole series, then the guards. An unobserved price on either
  // side makes the quotient NaN by itself, which is already the answer; a zero prior price would
  // make it an infinity and a negative one reverses its sign, so those rows are replaced explicitly.
  const auto current = prices.tail(rows - 1).array();
  const auto previous = prices.head(rows - 1).array();
  result.tail(rows - 1) =
    (previous <= Scalar{0}).select(missing(), current / previous - Scalar{1});
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

/**
 * @brief The period returns of a portfolio held at target weights, for a whole history at once:
 *   `r[t] = sum_i w[t - lag][i] * R[t][i]`.
 *
 * Research over a universe without orders: weights times instrument returns, as a vectorized backtest computes
 * them. A replay (`backtest::Backtester`) simulates the orders the same weights would send, with lots, costs and
 * fills; this does not. A product with a missing (`NaN`) weight or return is left out of its period's sum; a period
 * with no product observed, and each of the first `lag` periods, is `missing()`, not zero (pandas' row sum would
 * give zero there; a caller reproducing it replaces the missing values itself).
 *
 * @param returns Each instrument's simple return per period (as from `returns`): one row per period, one column
 *   per instrument.
 * @param weights The portfolio's weights in the same shape; row `t` is decided by the end of period `t` (by its
 *   start, for `lag` 0).
 * @param lag How many periods after a weight is decided it is held: `1` (the default) earns the next period's
 *   return, as a weight set at a close does; `0` earns the same period's, for a weight decided before that
 *   period's return begins (at its open, say). A lag of the whole history or more leaves every period missing.
 * @return One return per period (row).
 * @throws std::invalid_argument If the two matrices differ in shape.
 */
inline math::Vector weighted_returns(math::MatrixView returns, math::MatrixView weights, std::size_t lag = 1) {
  if (returns.rows() != weights.rows() || returns.cols() != weights.cols()) {
    throw std::invalid_argument("weighted_returns needs returns and weights of the same shape");
  }
  const auto rows = returns.rows();
  math::Vector result = math::unobserved(static_cast<std::size_t>(rows));
  // Compared before it becomes a signed index, so no lag can reach before the first row.
  if (lag >= static_cast<std::size_t>(rows)) {
    return result;
  }
  const auto shift = static_cast<math::Index>(lag);
  // Summed from zero, with whether a row saw any product kept apart from its sum: a sum can itself become `NaN`
  // (opposite infinities), which must stay so rather than read as a row not yet started.
  math::Vector sums = math::Vector::Zero(rows);
  std::vector<bool> seen(static_cast<std::size_t>(rows), false);
  // Column by column, so both matrices are read in the order they are stored.
  for (math::Index column = 0; column < returns.cols(); ++column) {
    for (math::Index row = shift; row < rows; ++row) {
      const Scalar product = weights(row - shift, column) * returns(row, column);
      if (observed(product)) {
        sums[row] += product;
        seen[static_cast<std::size_t>(row)] = true;
      }
    }
  }
  for (math::Index row = shift; row < rows; ++row) {
    if (seen[static_cast<std::size_t>(row)]) {
      result[row] = sums[row];
    }
  }
  return result;
}

} // namespace analytics
} // namespace algotradelib
