#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * The figures a backtest or a live review is judged on.
 *
 * Every one of these has more than one convention in circulation, and the differences are
 * large enough to change a decision: a Sortino ratio computed over the downside count rather
 * than the whole sample can be half again as flattering, and a turnover figure can be off by
 * exactly a factor of two. So each function here states which convention it implements, and
 * none of them takes a parameter whose default silently picks one -- annualization in
 * particular is a separate, named step rather than an argument with a plausible default.
 */

namespace metric_detail {

/** The observed count, mean, and sum of squared deviations of a whole series. */
struct Summary {
  std::size_t count{0};
  Scalar mean{0};
  Scalar squared{0};
};

/** Summarizes `values` in two passes, for the reason `rolling_stdev` takes two. */
inline Summary summarize(math::SeriesView values) {
  Summary result;
  Scalar sum{0};
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(values[row])) {
      ++result.count;
      sum += values[row];
    }
  }
  if (result.count == 0) {
    return result;
  }
  result.mean = sum / static_cast<Scalar>(result.count);
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(values[row])) {
      const Scalar deviation = values[row] - result.mean;
      result.squared += deviation * deviation;
    }
  }
  return result;
}

} // namespace metric_detail

/** The worst peak-to-trough decline of a level series, and where it happened. */
struct MaxDrawdown {
  /** The decline as a non-negative fraction of the peak: `0.2` is a fifth below it. */
  Scalar magnitude_{0};
  /** Row of the high-water mark the decline fell from, or `-1` if there is none. */
  math::Index peak_row_{-1};
  /** Row of the low the decline reached, or `-1` if there is none. */
  math::Index trough_row_{-1};
};

/**
 * Returns the worst peak-to-trough decline of a level series.
 *
 * Takes a **level** -- an equity curve, a cumulative growth factor, a price -- not a series
 * of period returns, the same as `RunningDrawdown`. Feeding it returns measures the drawdown
 * of the return series, which is a different quantity and rarely the one wanted.
 *
 * Reported as a non-negative fraction of the peak, matching `RunningDrawdown`, and the rows
 * come back so a report can say *when* rather than only how much. A series whose running peak
 * is at or below zero contributes nothing: a curve that reached zero has not declined by a
 * percentage, and a ratio there would be a fabrication.
 *
 * Unobserved levels are skipped, and a series with none leaves the magnitude unobserved and
 * both rows at `-1`.
 */
inline MaxDrawdown max_drawdown(math::SeriesView levels) {
  MaxDrawdown result;
  result.magnitude_ = missing();
  Scalar peak{0};
  math::Index peak_row = -1;
  for (math::Index row = 0; row < levels.size(); ++row) {
    if (!observed(levels[row])) {
      continue;
    }
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
  return result;
}

/**
 * Returns the **per-period** Sharpe ratio of a return series.
 *
 * `(mean(returns) - risk_free) / stdev(returns)`, with the sample deviation, matching
 * `rolling_stdev` and `RunningMoments`. `risk_free` is per period, in the same units as the
 * returns: a daily series wants a daily rate, and passing an annual one is a mistake that
 * produces a plausible number rather than an error.
 *
 * **Not annualized.** Annualizing is `annualize_ratio`, a separate call, deliberately.
 * The factor is `sqrt(periods_per_year)` rather than `periods_per_year`, and the number of
 * periods is a calendar question -- 252 or 245 trading days, 12 months, 52 weeks -- that a
 * default here would answer wrongly for someone. Two named steps make both choices visible.
 *
 * A series with fewer than two observations, or with no dispersion, leaves the ratio
 * unobserved rather than dividing by zero. A strategy whose returns never varied has no
 * risk-adjusted return to report.
 */
inline Scalar sharpe(math::SeriesView period_returns, Scalar risk_free = Scalar{0}) {
  const auto summary = metric_detail::summarize(period_returns);
  if (summary.count < 2) {
    return missing();
  }
  const Scalar deviation =
    std::sqrt(summary.squared / static_cast<Scalar>(summary.count - 1));
  if (deviation == Scalar{0}) {
    return missing();
  }
  return (summary.mean - risk_free) / deviation;
}

/**
 * Returns the **per-period** Sortino ratio of a return series.
 *
 * `(mean(returns) - target) / downside_deviation`, where the downside deviation is the root
 * mean squared shortfall below `target`.
 *
 * **Two details decide the number, and both are the ones usually got wrong.** The shortfalls
 * are measured from `target`, not from the mean of the series. And the mean is taken over
 * *every* observation, not only the ones that fell short -- that is Sortino's own definition,
 * and dividing by the downside count instead inflates the ratio for a strategy whose losses
 * are rare, which is exactly the strategy an investor most wants a truthful number for.
 *
 * A series with no observation below `target` leaves the ratio unobserved rather than
 * infinite. There is no downside risk to divide by, and an infinity would survive every
 * later comparison as the best strategy ever measured.
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
  if (count < 2 || below == 0) {
    return missing();
  }
  // Divided by the whole count, not by `below`. That is the definition, and the difference
  // is a systematic flattery of strategies with rare losses.
  const Scalar downside = std::sqrt(shortfall / static_cast<Scalar>(count));
  if (downside == Scalar{0}) {
    return missing();
  }
  return (sum / static_cast<Scalar>(count) - target) / downside;
}

/**
 * Scales a per-period ratio to an annual one.
 *
 * Multiplies by `sqrt(periods_per_year)`, not by `periods_per_year`. The mean scales with the
 * number of periods and the deviation with its square root, so their ratio scales with the
 * square root -- which is why annualizing by the period count overstates a Sharpe ratio by a
 * factor of about sixteen on daily data.
 *
 * This assumes the returns are serially uncorrelated, which is the assumption the square-root
 * rule rests on and which trending strategies violate. It is stated because the number is
 * reported as though it were a fact.
 *
 * @throws std::invalid_argument if `periods_per_year` is not positive.
 */
inline Scalar annualize_ratio(Scalar ratio, Scalar periods_per_year) {
  if (!(periods_per_year > Scalar{0})) {
    throw std::invalid_argument("periods per year must be positive");
  }
  return observed(ratio) ? ratio * std::sqrt(periods_per_year) : ratio;
}

/**
 * Returns the fraction of the portfolio traded in each period.
 *
 * `weights` has rows for periods and columns for instruments, matching every other frame in
 * this layer. Row `i` is `0.5 * sum(|w[i] - w[i - 1]|)`, and row 0 is measured from an
 * all-cash portfolio, because building the first position is real trading with real cost.
 *
 * **The halving is the one-way convention:** the result is the fraction of portfolio value
 * that changed hands, so moving a portfolio entirely from one instrument to another is a
 * turnover of 1, not 2. Doubling these numbers gives the two-way convention that some
 * reports use. Which one is in force matters because turnover is usually multiplied by a
 * cost per unit traded, and a factor of two there is the difference between a profitable
 * strategy and a losing one.
 *
 * An unobserved weight is treated as no position in that instrument for that period, since a
 * weight is a decision and an absent decision is an absent position -- unlike a price, where
 * absence means the value is unknown.
 */
inline math::Vector turnover(math::MatrixView weights) {
  math::Vector result = math::unobserved(static_cast<std::size_t>(weights.rows()));
  for (math::Index row = 0; row < weights.rows(); ++row) {
    Scalar traded{0};
    for (math::Index column = 0; column < weights.cols(); ++column) {
      const Scalar now = observed(weights(row, column)) ? weights(row, column) : Scalar{0};
      const Scalar before = row == 0 || !observed(weights(row - 1, column)) ?
        Scalar{0} :
        weights(row - 1, column);
      traded += std::abs(now - before);
    }
    result[row] = Scalar{0.5} * traded;
  }
  return result;
}

} // namespace analytics
} // namespace algotradelib
