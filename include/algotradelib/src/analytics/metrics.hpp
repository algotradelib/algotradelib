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
  std::size_t count_{0};
  Scalar mean_{0};
  Scalar squared_{0};
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
 * of period returns. Feeding it returns measures the drawdown
 * of the return series, which is a different quantity and rarely the one wanted.
 *
 * Reported as a non-negative fraction of the peak, and the rows
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
 * Returns the **per-period** Sharpe ratio of a return series.
 *
 * `(mean(returns) - risk_free) / stdev(returns)`, with the sample deviation, matching
 * `rolling_stdev`. `risk_free` is per period, in the same units as the
 * returns: a daily series wants a daily rate, and passing an annual one is a mistake that
 * produces a plausible number rather than an error.
 *
 * **Not annualized.** Annualizing is `annualize_ratio`, a separate call, deliberately.
 * The factor is `sqrt(periods_per_year)` rather than `periods_per_year`, and the number of
 * periods is a calendar question -- 252 or 245 trading days, 12 months, 52 weeks -- that a
 * default here would answer wrongly for someone. Two named steps make both choices visible.
 *
 * Fewer than two observations gives `missing()`. Returns that never varied give `undefined()` --
 * the data was there and the ratio does not exist for it, which `is_undefined()` distinguishes.
 */
inline Scalar sharpe(math::SeriesView period_returns, Scalar risk_free = Scalar{0}) {
  const auto summary = metric_detail::summarize(period_returns);
  if (summary.count_ < 2) {
    return missing();
  }
  const Scalar deviation =
    std::sqrt(summary.squared_ / static_cast<Scalar>(summary.count_ - 1));
  if (deviation == Scalar{0}) {
    // Returns were present and never varied. `undefined()` rather than `missing()`, because a
    // caller ranking strategies should be able to tell this from "not enough history".
    return math::undefined();
  }
  return (summary.mean_ - risk_free) / deviation;
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
 * A series with no observation below `target` gives `undefined()` rather than an infinity: an
 * infinity would survive every later comparison as the best strategy ever measured. It is
 * distinguishable from `missing()` through `is_undefined()`, because "no losses" and "not enough
 * history" are answers a caller ranking strategies should be allowed to treat differently.
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

/** Whether the first period's trading counts, and against what. */
enum class TurnoverStart {
  /**
   * Row 0 is measured against an all-cash portfolio.
   *
   * Building the first position is real trading with real cost, so it counts. The right choice
   * for costing a single strategy end to end.
   */
  FROM_CASH,
  /**
   * Row 0 is unobserved.
   *
   * The right choice when comparing strategies whose histories begin on different dates, where
   * one initial build would otherwise show up as a difference in trading behaviour.
   */
  SKIP,
};

/**
 * Returns the **one-way** fraction of the portfolio traded in each period.
 *
 * `weights` has rows for periods and columns for instruments, matching every other frame in this
 * layer. Row `i` is `0.5 * sum(|w[i] - w[i - 1]|)`.
 *
 * One-way means the result is the fraction of portfolio *value* that changed hands: moving a
 * portfolio entirely from one instrument to another is `1`, not `2`. That is what the halving is
 * for, and it is in the function's name rather than only in this paragraph because turnover is
 * usually multiplied by a cost per unit traded, and a silent factor of two there separates a
 * profitable strategy from a losing one. `two_way_turnover` is the other convention.
 *
 * An unobserved weight is treated as no position in that instrument for that period. A weight is
 * a decision, and an absent decision is an absent position — unlike a price, where absence means
 * the value is unknown.
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
 * Returns the **two-way** fraction traded in each period: exactly twice `one_way_turnover`.
 *
 * `sum(|w[i] - w[i - 1]|)`, counting each side of a rotation separately, so moving a portfolio
 * entirely from one instrument to another is `2`. Both conventions are in use and neither is
 * wrong; what is wrong is not knowing which one a number came from, which is why both are named
 * rather than selected by a flag.
 */
inline math::Vector two_way_turnover(
  math::MatrixView weights,
  TurnoverStart start = TurnoverStart::FROM_CASH
) {
  return Scalar{2} * one_way_turnover(weights, start);
}

} // namespace analytics
} // namespace algotradelib
