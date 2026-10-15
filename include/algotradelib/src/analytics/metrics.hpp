#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Performance metrics: maximum drawdown, Sharpe and Sortino ratios, annualization, turnover.
 *
 * Each function states the convention it implements. Ratios are per period; annualize them
 * explicitly with `annualize_ratio`.
 */

namespace metric_detail {

/** The observed count, mean, and sum of squared deviations of a whole series. */
struct Summary {
  std::size_t count_{0};
  Scalar mean_{0};
  Scalar squared_{0};
  // Whether at least two observations are present and all equal (`constant_detail::constant`).
  bool constant_{false};
};

/**
 * Summarizes `values`, masking absent entries once and reducing vectorized.
 *
 * Two passes, for the reason `rolling_stdev` takes two: the one-pass `E[x²] - E[x]²` form loses
 * almost all its significant digits when the mean is large relative to the dispersion. The second
 * pass is a single expression, because subtracting `mean * present_` leaves an absent row
 * contributing exactly zero.
 */
inline Summary summarize(math::SeriesView values) {
  const auto parts = math::observed_parts(values);
  Summary result;
  const Scalar count = parts.present_.sum();
  result.count_ = static_cast<std::size_t>(count);
  if (result.count_ == 0) {
    return result;
  }
  result.mean_ = parts.values_.sum() / count;
  result.squared_ = (parts.values_ - result.mean_ * parts.present_).squaredNorm();
  if (result.count_ >= 2) {
    const Scalar deviation = std::sqrt(result.squared_ / static_cast<Scalar>(result.count_ - 1));
    result.constant_ = constant_detail::constant(
      deviation, result.mean_, result.count_, parts.values_, parts.present_, 0, parts.values_.size()
    );
  }
  return result;
}

} // namespace metric_detail

/** @brief The worst peak-to-trough decline of a level series, and where it happened. */
struct MaxDrawdown {
  /** @brief The decline as a non-negative fraction of the peak (`0.2` is 20% below it). */
  Scalar magnitude_{0};
  /** @brief The row of the peak the decline fell from, or `-1` if there is none. */
  math::Index peak_row_{-1};
  /** @brief The row of the low the decline reached, or `-1` if there is none. */
  math::Index trough_row_{-1};
};

/**
 * @brief Finds the worst peak-to-trough decline of a level series.
 *
 * Rows where the running peak is at or below zero are not measured, since no percentage
 * decline exists there.
 *
 * @param levels A **level** series (an equity curve, a growth factor, a price), not returns.
 *   `NaN` entries are skipped.
 * @return The decline and its rows. With no observed level, `magnitude_` is `missing()` and both
 *   rows are `-1`; if levels exist but every running peak is at or below zero, `magnitude_` is
 *   `math::undefined()`. A series that never declines gives `0`.
 */
inline MaxDrawdown max_drawdown(math::SeriesView levels) {
  MaxDrawdown result;
  result.magnitude_ = missing();
  Scalar peak{0};
  math::Index peak_row = -1;
  std::size_t observations = 0;
  for (math::Index row = 0; row < levels.size(); ++row) {
    if (!observed(levels[row])) {
      continue;
    }
    ++observations;
    if (peak_row < 0 || peak < levels[row]) {
      peak = levels[row];
      peak_row = row;
    }
    if (peak <= Scalar{0}) {
      continue;
    }
    const Scalar decline = (peak - levels[row]) / peak;
    if (!observed(result.magnitude_) || result.magnitude_ < decline) {
      result.magnitude_ = decline;
      result.peak_row_ = peak_row;
      result.trough_row_ = row;
    }
  }
  // Levels were present but no ratio could be formed from them -- every running peak was at or
  // below zero. That is "undefined", not "no data".
  if (observations > 0 && !observed(result.magnitude_)) {
    result.magnitude_ = math::undefined();
  }
  return result;
}

/**
 * @brief Computes the per-period Sharpe ratio, `(mean(returns) - risk_free) / stdev(returns)`.
 *
 * Uses the sample standard deviation. The result is not annualized; use `annualize_ratio`.
 *
 * @param period_returns The period returns; `NaN` entries are skipped.
 * @param risk_free The risk-free rate **per period**, in the units of the returns (a daily rate
 *   for daily returns).
 * @return The ratio; `missing()` with fewer than two observations, and `math::undefined()` if the
 *   returns never vary.
 */
inline Scalar sharpe(math::SeriesView period_returns, Scalar risk_free = Scalar{0}) {
  const auto summary = metric_detail::summarize(period_returns);
  if (summary.count_ < 2) {
    return missing();
  }
  const Scalar deviation =
    std::sqrt(summary.squared_ / static_cast<Scalar>(summary.count_ - 1));
  if (summary.constant_) {
    // Returns were present and never varied. `undefined()` rather than `missing()`, because a
    // caller ranking strategies should be able to tell this from "not enough history".
    return math::undefined();
  }
  return (summary.mean_ - risk_free) / deviation;
}

/**
 * @brief Computes the per-period Sortino ratio, `(mean(returns) - target) / downside_deviation`.
 *
 * The downside deviation is `sqrt(sum(min(r - target, 0)^2) / n)`, where `n` counts **every**
 * observation, not only those below `target` (Sortino's definition). Not annualized.
 *
 * @param period_returns The period returns; `NaN` entries are skipped.
 * @param target The minimum acceptable return per period.
 * @return The ratio; `missing()` with fewer than two observations, and `math::undefined()` if no
 *   return falls below `target`.
 */
inline Scalar sortino(math::SeriesView period_returns, Scalar target = Scalar{0}) {
  std::size_t count = 0;
  std::size_t below = 0;
  Scalar sum{0};
  Scalar shortfall{0};
  for (math::Index row = 0; row < period_returns.size(); ++row) {
    const Scalar value = period_returns[row];
    if (!observed(value)) {
      continue;
    }
    ++count;
    sum += value;
    if (value < target) {
      ++below;
      const Scalar gap = target - value;
      shortfall += gap * gap;
    }
  }
  if (count < 2) {
    return missing();
  }
  if (below == 0) {
    // Nothing fell short of the target. There is no downside risk to divide by, and an infinity
    // would survive every later comparison as the best strategy ever measured. `undefined()`
    // says "computed, and does not exist" rather than "no data", so a caller ranking strategies
    // can treat the two differently.
    return math::undefined();
  }
  // Divided by the whole count, not by `below`. That is the definition, and the difference
  // is a systematic flattery of strategies with rare losses.
  const Scalar downside = std::sqrt(shortfall / static_cast<Scalar>(count));
  if (downside == Scalar{0}) {
    return math::undefined();
  }
  return (sum / static_cast<Scalar>(count) - target) / downside;
}

/**
 * @brief Annualizes a per-period ratio by multiplying it by `sqrt(periods_per_year)`.
 *
 * The square-root rule assumes serially uncorrelated returns.
 *
 * @param ratio A per-period ratio from `sharpe` or `sortino`.
 * @param periods_per_year Periods in a year, such as 252 for trading days or 12 for months.
 * @return The annualized ratio; `ratio` unchanged if it is not observed.
 * @throws std::invalid_argument if `periods_per_year` is not positive.
 */
inline Scalar annualize_ratio(Scalar ratio, Scalar periods_per_year) {
  if (!(periods_per_year > Scalar{0})) {
    throw std::invalid_argument("periods per year must be positive");
  }
  return observed(ratio) ? ratio * std::sqrt(periods_per_year) : ratio;
}

/** @brief How turnover treats the first period. */
enum class TurnoverStart {
  /** @brief Row 0 measures the first positions against an all-cash start. Use it to cost one strategy. */
  FROM_CASH,
  /** @brief Row 0 is `missing()`. Use it to compare strategies whose histories start on different dates. */
  SKIP,
};

/**
 * @brief Computes the one-way turnover of each period, `0.5 * sum(|w[i] - w[i - 1]|)`.
 *
 * One-way: moving the whole portfolio from one instrument to another is `1`.
 *
 * @param weights Portfolio weights, one row per period and one column per instrument. A `NaN`
 *   weight counts as no position.
 * @param start How row 0 is treated.
 * @return The turnover, one value per row.
 * @see two_way_turnover for the convention that counts both sides (twice this).
 */
inline math::Vector one_way_turnover(
  math::MatrixView weights,
  TurnoverStart start = TurnoverStart::FROM_CASH
) {
  // Masked once for the whole panel, so each period is a plain vectorized row difference.
  const math::Matrix held =
    (weights.array() == weights.array()).select(weights, Scalar{0});
  math::Vector result = math::unobserved(static_cast<std::size_t>(weights.rows()));
  for (math::Index row = 0; row < held.rows(); ++row) {
    if (row == 0) {
      if (start == TurnoverStart::SKIP) {
        continue;
      }
      result[row] = Scalar{0.5} * held.row(0).cwiseAbs().sum();
      continue;
    }
    result[row] = Scalar{0.5} * (held.row(row) - held.row(row - 1)).cwiseAbs().sum();
  }
  return result;
}

/**
 * @brief Computes the two-way turnover of each period, `sum(|w[i] - w[i - 1]|)`.
 *
 * Exactly twice `one_way_turnover`: moving the whole portfolio from one instrument to another is `2`.
 *
 * @param weights Portfolio weights, one row per period and one column per instrument. A `NaN`
 *   weight counts as no position.
 * @param start How row 0 is treated.
 * @return The turnover, one value per row.
 */
inline math::Vector two_way_turnover(
  math::MatrixView weights,
  TurnoverStart start = TurnoverStart::FROM_CASH
) {
  return Scalar{2} * one_way_turnover(weights, start);
}

} // namespace analytics
} // namespace algotradelib
