#pragma once

#include "./numeric.hpp"

#include <cstddef>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Simple returns of a price series.
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

} // namespace analytics
} // namespace algotradelib
