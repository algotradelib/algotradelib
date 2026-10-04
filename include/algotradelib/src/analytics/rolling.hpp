#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace algotradelib {
namespace analytics {

/**
 * Trailing statistics over a series.
 *
 * Every function here keeps the two conventions `returns.hpp` states — the result is as long as
 * the input, and a value that cannot be computed is `missing()` — and adds a third, specific to
 * windows.
 *
 * **Two numbers describe a window, and they answer different questions.**
 *
 * - `window` is **how many rows are looked at**: the trailing span, in rows of the input,
 *   whether or not those rows carry observations.
 * - `min_observed` is **how many of those rows must actually carry a value** before a result is
 *   reported. It is a count of observations, never a span.
 *
 * They differ exactly when a series has gaps. `rolling_mean(closes, 20, 15)` looks at twenty
 * rows and reports as soon as fifteen of them are observed, dividing by however many it
 * actually saw. The two-argument overload demands all twenty, which is the safe default and
 * therefore the one you get without asking.
 *
 * The check exists because masking makes it necessary. Unobserved values inside a window are
 * skipped rather than propagated — one absent print must not blank a whole column — so a
 * window's divisor is the count of values it saw. Without `min_observed`, a twenty-row mean
 * computed from three observations would be reported as though it were a twenty-observation
 * mean, and nothing in the result would say otherwise. That is the second most common source of
 * lookahead bias after bar stamping, so relaxing it is an explicit request at the call site
 * rather than a default.
 *
 * **On speed.** Each function pays for the `NaN` test once per series, through
 * `math::observed_parts`, and every window is then a plain contiguous reduction. That is around
 * seven times faster than testing inside each window, and identical to the last bit apart from
 * summation order. The remaining `O(rows x window)` structure is deliberate: a prefix-sum would
 * make it linear and would lose the significant digits `rolling_stdev` takes two passes to
 * protect.
 */

namespace rolling_detail {

/** The observed values of one window, reduced to what every statistic over it needs. */
struct Reduction {
  std::size_t count_{0};
  Scalar sum_{0};
};

/** Reduces one window of a pre-split series to a count and a sum. */
inline Reduction reduce(const math::Observed& parts, math::Index first, math::Index length) {
  Reduction result;
  result.count_ = static_cast<std::size_t>(parts.present_.segment(first, length).sum());
  result.sum_ = parts.values_.segment(first, length).sum();
  return result;
}

/** The mean and sample deviation of one window, shared by every statistic that needs both. */
struct Dispersion {
  bool reported_{false};
  std::size_t count_{0};
  Scalar mean_{0};
  Scalar deviation_{0};
};

/**
 * Computes the mean and sample deviation of one window.
 *
 * `reported_` is false when the window holds fewer than `needed` observations, or fewer than
 * two — one point has no dispersion, and `count - 1` would be a division by zero.
 *
 * Two passes over the window rather than the one-pass `E[x²] - E[x]²` form. On price levels the
 * two terms of that form agree to more digits than a `Scalar` has, so their difference keeps
 * almost none: three values one apart around `1e8` come out with a variance of 2 where the
 * answer is 1. It can also go negative and become a `NaN` at the square root, with nothing to
 * say that arithmetic rather than data produced it.
 *
 * The second pass is still one vectorized expression. Subtracting `mean * present_` leaves an
 * absent row contributing exactly zero, because both its value and its mask entry are zero.
 */
inline Dispersion dispersion(
  const math::Observed& parts,
  math::Index first,
  math::Index length,
  std::size_t needed
) {
  Dispersion result;
  const auto reduced = reduce(parts, first, length);
  result.count_ = reduced.count_;
  if (reduced.count_ < needed || reduced.count_ < 2) {
    return result;
  }
  result.mean_ = reduced.sum_ / static_cast<Scalar>(reduced.count_);
  const Scalar squared = (parts.values_.segment(first, length) -
    result.mean_ * parts.present_.segment(first, length)).squaredNorm();
  result.deviation_ = std::sqrt(squared / static_cast<Scalar>(reduced.count_ - 1));
  result.reported_ = true;
  return result;
}

/**
 * Validates a window and the observation count demanded of it.
 *
 * @throws std::invalid_argument if `window` is zero, or if more observations are demanded than
 * the window can ever hold. Both are programming errors that would otherwise surface as an
 * all-unobserved column, indistinguishable from a series that simply had no data.
 */
inline void check(std::size_t window, std::size_t min_observed) {
  if (window == 0) {
    throw std::invalid_argument("rolling window must be at least one row");
  }
  if (min_observed == 0) {
    throw std::invalid_argument("a window must require at least one observation");
  }
  if (min_observed > window) {
    throw std::invalid_argument(
      "rolling window of " + std::to_string(window) + " rows cannot hold " +
      std::to_string(min_observed) + " observations"
    );
  }
}

/** Returns the first row of the window ending at `row`, clamped to the start of the series. */
inline math::Index window_start(math::Index row, std::size_t window) {
  const auto span = static_cast<math::Index>(window);
  return row + 1 >= span ? row + 1 - span : math::Index{0};
}

} // namespace rolling_detail

/** Returns the trailing sum over `window` rows, reporting once `min_observed` are present. */
inline math::Vector rolling_sum(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto reduced = rolling_detail::reduce(parts, first, row - first + 1);
    if (reduced.count_ >= min_observed) {
      result[row] = reduced.sum_;
    }
  }
  return result;
}

/** Returns the trailing sum over `window` rows, requiring every row to be observed. */
inline math::Vector rolling_sum(math::SeriesView values, std::size_t window) {
  return rolling_sum(values, window, window);
}

/** Returns the trailing mean over `window` rows, reporting once `min_observed` are present. */
inline math::Vector rolling_mean(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto reduced = rolling_detail::reduce(parts, first, row - first + 1);
    if (reduced.count_ >= min_observed) {
      result[row] = reduced.sum_ / static_cast<Scalar>(reduced.count_);
    }
  }
  return result;
}

/** Returns the trailing mean over `window` rows, requiring every row to be observed. */
inline math::Vector rolling_mean(math::SeriesView values, std::size_t window) {
  return rolling_mean(values, window, window);
}

/**
 * Returns the trailing **sample** standard deviation, dividing by `count - 1`.
 *
 * The sample divisor rather than the population one, because a trailing window is a sample of a
 * process that continues. A window holding fewer than two observations is unobserved whatever
 * `min_observed` says: one point has no dispersion to report.
 *
 * See `rolling_detail::dispersion` for why each window is computed in two passes.
 */
inline math::Vector rolling_stdev(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto spread =
      rolling_detail::dispersion(parts, first, row - first + 1, min_observed);
    if (spread.reported_) {
      result[row] = spread.deviation_;
    }
  }
  return result;
}

/** Returns the trailing sample deviation, requiring every row to be observed. */
inline math::Vector rolling_stdev(math::SeriesView values, std::size_t window) {
  return rolling_stdev(values, window, window);
}

/** Returns the smallest observed value in each trailing window of `window` rows. */
inline math::Vector rolling_min(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  // Absent rows become `+inf` so they lose every comparison without needing a mask inside the
  // window. A window with no observation at all is rejected by the count, never read from here.
  const math::Vector ceiling =
    math::observed_or(values, std::numeric_limits<Scalar>::infinity());
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    if (parts.present_.segment(first, length).sum() >= static_cast<Scalar>(min_observed)) {
      result[row] = ceiling.segment(first, length).minCoeff();
    }
  }
  return result;
}

/** Returns the smallest observed value, requiring every row to be observed. */
inline math::Vector rolling_min(math::SeriesView values, std::size_t window) {
  return rolling_min(values, window, window);
}

/** Returns the largest observed value in each trailing window of `window` rows. */
inline math::Vector rolling_max(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  const math::Vector floor =
    math::observed_or(values, -std::numeric_limits<Scalar>::infinity());
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    if (parts.present_.segment(first, length).sum() >= static_cast<Scalar>(min_observed)) {
      result[row] = floor.segment(first, length).maxCoeff();
    }
  }
  return result;
}

/** Returns the largest observed value, requiring every row to be observed. */
inline math::Vector rolling_max(math::SeriesView values, std::size_t window) {
  return rolling_max(values, window, window);
}

/**
 * Returns the simple moving average over `window` rows.
 *
 * Exactly `rolling_mean` with the same arguments, kept as a separate name because the trading
 * vocabulary has one and a reader should not have to wonder whether the two differ. They do not,
 * and this is the line that guarantees it.
 */
inline math::Vector sma(math::SeriesView values, std::size_t window, std::size_t min_observed) {
  return rolling_mean(values, window, min_observed);
}

/** Returns the simple moving average, requiring every row to be observed. */
inline math::Vector sma(math::SeriesView values, std::size_t window) {
  return rolling_mean(values, window, window);
}

/**
 * Returns the linearly weighted moving average over `window` rows.
 *
 * Weights ascend `1, 2, ... n` across the window with the most recent row heaviest, and the
 * result is divided by the sum of the weights **actually used**. Dropping an unobserved row
 * without also dropping its weight would bias every affected window toward zero — the kind of
 * error that shows up as a strategy performing slightly worse than it should and is never traced
 * back here.
 *
 * Both sums are dot products against the same precomputed weight vector, so the masking costs
 * nothing per window.
 */
inline math::Vector wma(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  math::Vector weights(static_cast<math::Index>(window));
  for (math::Index at = 0; at < weights.size(); ++at) {
    weights[at] = static_cast<Scalar>(at + 1);
  }

  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    if (parts.present_.segment(first, length).sum() < static_cast<Scalar>(min_observed)) {
      continue;
    }
    const auto used = weights.head(length);
    const Scalar divisor = parts.present_.segment(first, length).dot(used);
    if (divisor > Scalar{0}) {
      result[row] = parts.values_.segment(first, length).dot(used) / divisor;
    }
  }
  return result;
}

/** Returns the linearly weighted moving average, requiring every row to be observed. */
inline math::Vector wma(math::SeriesView values, std::size_t window) {
  return wma(values, window, window);
}

/**
 * Returns the smoothing factor `2 / (span + 1)` of a `span`-period exponential average.
 *
 * The *span* convention, the one charting packages and `pandas.ewm(span=...)` quote: it gives the
 * exponential average the same average lag as a `span`-period simple one, `(1 - alpha) / alpha =
 * (span - 1) / 2`. So `ema(closes, alpha_from_span(12))` is what a "12-period EMA" means. Wilder's
 * smoothing, `alpha = 1 / n`, is span `2n - 1`; `rsi` and `atr` apply it themselves.
 *
 * @throws std::invalid_argument if `span` is zero.
 */
inline Scalar alpha_from_span(std::size_t span) {
  if (span == 0) {
    throw std::invalid_argument("an ema span must be at least one row");
  }
  return Scalar{2} / (static_cast<Scalar>(span) + Scalar{1});
}

namespace rolling_detail {

/** Refuses a smoothing factor outside `(0, 1]`, including `NaN`. */
inline void check_alpha(Scalar alpha) {
  if (!(Scalar{0} < alpha && alpha <= Scalar{1})) {
    throw std::invalid_argument("an ema smoothing factor must be in (0, 1]");
  }
}

} // namespace rolling_detail

/**
 * Returns one exponential-average update: `alpha * value + (1 - alpha) * level`.
 *
 * `level` is the previous result, and unobserved before the first observation: the first observed
 * `value` then seeds it. An unobserved `value` leaves `level` as it was. The arithmetic is `ema`'s,
 * written in the same order, so a live strategy that keeps `level` from bar to bar computes exactly
 * what a replay over the same bars does:
 *
 *     Scalar level = missing();
 *     level = ema_step(level, close, alpha);   // once per bar
 *
 * One difference, once the arithmetic itself produces `NaN` -- an infinity met by its opposite, or an
 * overflow such as `DBL_MAX` followed by `-DBL_MAX`: `ema` keeps that `NaN` for the rest of the series,
 * while a level kept with this function counts as "no level yet" and is reseeded by the next
 * observation. A `NaN` level cannot say which it is.
 *
 * @throws std::invalid_argument unless `0 < alpha <= 1`.
 */
inline Scalar ema_step(Scalar level, Scalar value, Scalar alpha) {
  rolling_detail::check_alpha(alpha);
  if (!observed(value)) {
    return level;
  }
  return observed(level) ? level + alpha * (value - level) : value;
}

/**
 * Refused: an integer here is a span. Write `alpha_from_span(span)`. A `long double` smoothing
 * factor is ambiguous between the two overloads; cast it to `Scalar`.
 */
Scalar ema_step(Scalar level, Scalar value, std::size_t span) = delete;

/**
 * Returns the exponentially weighted moving average with smoothing factor `alpha`.
 *
 * Seeded with the first observed value; an unobserved value is skipped (the level carries across
 * it and that row's output is unobserved); otherwise `alpha * value + (1 - alpha) * level`, the
 * update `ema_step` makes. For a span, pass `alpha_from_span(span)`. On observed rows the levels are
 * those of `pandas.Series.ewm(alpha=alpha, adjust=False, ignore_na=True).mean()`, to rounding (with
 * `min_periods` for `min_observed`); pandas also reports the carried level on a missing row, which
 * this leaves unobserved. pandas' default `adjust=True` weighs the first rows differently, and a
 * package that seeds with the simple mean of the first rows differs until that seed has decayed. Pass `min_observed` to suppress the region where
 * the seed still dominates.
 *
 * **Not vectorized, and it cannot be.** Each level depends on the one before it, so there is no
 * reduction to hand a backend — unlike every other function here. The same dependence is why
 * running it over a trailing `SeriesBuffer` view does *not* reproduce what a live loop computes:
 * the buffer's oldest row becomes a fresh seed each time it advances. A live strategy keeps the level
 * itself and calls `ema_step` once per bar. The finite windows above have no such problem — window and
 * full series agree exactly.
 *
 * @throws std::invalid_argument unless `0 < alpha <= 1`, or if `min_observed` is zero. Checked
 * before anything is read, so an empty series refuses too.
 */
inline math::Vector ema(math::SeriesView values, Scalar alpha, std::size_t min_observed = 1) {
  rolling_detail::check_alpha(alpha);
  if (min_observed == 0) {
    throw std::invalid_argument("ema must require at least one observation");
  }
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  Scalar level{0};
  std::size_t count = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    const Scalar value = values[row];
    if (!observed(value)) {
      continue;
    }
    // Seeded by count rather than by an unobserved level, so a level an infinity turned into `NaN`
    // stays `NaN` instead of being mistaken for "no level yet".
    level = count == 0 ? value : level + alpha * (value - level);
    ++count;
    if (count >= min_observed) {
      result[row] = level;
    }
  }
  return result;
}

/**
 * Refused: an integer here is a span. Write `ema(values, alpha_from_span(span))`. A `long double`
 * smoothing factor is ambiguous between the two overloads; cast it to `Scalar`.
 */
math::Vector ema(math::SeriesView values, std::size_t span, std::size_t min_observed = 1) = delete;

} // namespace analytics
} // namespace algotradelib
