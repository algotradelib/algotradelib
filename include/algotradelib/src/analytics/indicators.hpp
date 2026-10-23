#pragma once

#include "./rolling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Named technical indicators: the trailing z-score and the relative strength index.
 *
 * They follow the conventions of `rolling.hpp`: the result is as long as the input, a value that
 * cannot be computed is `missing()`, and nothing is reported before the window holds enough
 * observations. Where an indicator has a conventional parameterization (Wilder's smoothing for
 * RSI), its documentation says which one it follows.
 */

namespace indicator_detail {

/**
 * Applies Wilder's smoothing to `values`, seeding on the first `period` observed values.
 *
 * Wilder's average is `(previous * (period - 1) + value) / period`, equivalently an
 * exponential average with `alpha = 1 / period`, seeded with the simple mean of the first
 * `period` values rather than with the first value alone. Both halves of that are part of
 * the convention: RSI as every charting package draws it use this smoothing and
 * this seed, and changing either shifts the whole series.
 *
 * Unobserved inputs are skipped -- they do not advance the average and they do not seed it
 * -- and the corresponding output row is unobserved.
 */
inline math::Vector wilder(math::SeriesView values, std::size_t period) {
  const auto span = static_cast<Scalar>(period);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  Scalar average{0};
  Scalar seed{0};
  std::size_t count = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    const Scalar value = values[row];
    if (!observed(value)) {
      continue;
    }
    ++count;
    if (count < period) {
      seed += value;
      continue;
    }
    if (count == period) {
      average = (seed + value) / span;
    } else {
      average = (average * (span - Scalar{1}) + value) / span;
    }
    result[row] = average;
  }
  return result;
}

} // namespace indicator_detail

/**
 * @brief Computes how many trailing sample standard deviations each value lies from its
 *   trailing mean.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The z-scores, one per row. A row is `missing()` if its own value is unobserved or the
 *   window has fewer than `max(min_observed, 2)` observations, and `math::undefined()` if every
 *   observation in the window is equal (no dispersion; common for a halted instrument).
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
inline math::Vector zscore(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    if (!observed(values[row])) {
      continue;
    }
    const math::Index first = rolling_detail::window_start(row, window);
    const auto spread = rolling_detail::dispersion(parts, first, row - first + 1, min_observed);
    if (!spread.reported_) {
      continue;
    }
    if (spread.deviation_ == Scalar{0} ||
        constant_detail::rounded_constant(
          spread.deviation_, spread.mean_, spread.count_, parts.values_, parts.present_, first, row - first + 1
        )) {
      // The data was there and the statistic does not exist for it, which is a different
      // answer from "no data yet" and now says so. Equal values that are not binary fractions
      // leave a deviation of a few ulps rather than zero; dividing by it would not be a z-score.
      result[row] = math::undefined();
      continue;
    }
    result[row] = (values[row] - spread.mean_) / spread.deviation_;
  }
  return result;
}

/**
 * @brief Computes the trailing z-score, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows; all must be observed.
 * @return The z-scores, one per row; see the overload with `min_observed`.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector zscore(math::SeriesView values, std::size_t window) {
  return zscore(values, window, window);
}

/**
 * @brief Computes Wilder's relative strength index.
 *
 * Average gains and losses use Wilder's smoothing (`alpha = 1 / period`, seeded with the simple
 * mean of the first `period` changes), and the index is `100 - 100 / (1 + gain / loss)`. Only
 * gains gives 100, only losses gives 0, and no movement at all gives 50.
 *
 * @param prices The price series; `NaN` entries are unobserved, and a change needs both prices.
 * @param period The number of price changes averaged; 14 is Wilder's.
 * @return The index, in `[0, 100]`, one per row; the first `period` rows (and more after gaps)
 *   are `missing()`.
 * @throws std::invalid_argument if `period` is zero.
 */
inline math::Vector rsi(math::SeriesView prices, std::size_t period = 14) {
  if (period == 0) {
    throw std::invalid_argument("rsi period must be at least one row");
  }
  const auto rows = static_cast<std::size_t>(prices.size());
  math::Vector gains = math::unobserved(rows);
  math::Vector losses = math::unobserved(rows);
  for (math::Index row = 1; row < prices.size(); ++row) {
    if (!observed(prices[row - 1]) || !observed(prices[row])) {
      continue;
    }
    const Scalar change = prices[row] - prices[row - 1];
    gains[row] = std::max(change, Scalar{0});
    losses[row] = std::max(-change, Scalar{0});
  }

  const math::Vector average_gain = indicator_detail::wilder(gains, period);
  const math::Vector average_loss = indicator_detail::wilder(losses, period);

  math::Vector result = math::unobserved(rows);
  for (math::Index row = 0; row < prices.size(); ++row) {
    if (!observed(average_gain[row]) || !observed(average_loss[row])) {
      continue;
    }
    if (average_loss[row] == Scalar{0}) {
      result[row] = average_gain[row] == Scalar{0} ? Scalar{50} : Scalar{100};
      continue;
    }
    const Scalar strength = average_gain[row] / average_loss[row];
    result[row] = Scalar{100} - Scalar{100} / (Scalar{1} + strength);
  }
  return result;
}

} // namespace analytics
} // namespace algotradelib
