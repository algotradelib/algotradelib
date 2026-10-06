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
 * @file
 * @brief Trailing-window statistics: sums, means, deviations, extremes and moving averages.
 *
 * Every function returns a series as long as its input: row `i` of the result describes the
 * window ending at row `i` of the input. A row that cannot be computed is `missing()`.
 *
 * Two numbers describe a window:
 * - `window` is how many trailing rows are looked at, whether or not they carry a value;
 * - `min_observed` is how many of those rows must carry a value before a result is reported.
 *
 * Unobserved values inside a window are skipped, and the statistic is computed over the values
 * that are there. `rolling_mean(closes, 20, 15)` reports as soon as 15 of the 20 rows are
 * observed. The overloads without `min_observed` require every row of the window.
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

/**
 * @brief Computes the trailing sum of the observed values.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The sums, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
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

/**
 * @brief Computes the trailing sum, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The sums, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector rolling_sum(math::SeriesView values, std::size_t window) {
  return rolling_sum(values, window, window);
}

/**
 * @brief Computes the trailing mean of the observed values.
 *
 * Divides by the number of observed values in the window, not by `window`.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The means, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
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

/**
 * @brief Computes the trailing mean, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The means, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector rolling_mean(math::SeriesView values, std::size_t window) {
  return rolling_mean(values, window, window);
}

/**
 * @brief Computes the trailing sample standard deviation (divisor `count - 1`).
 *
 * A window with fewer than two observed values reports `missing()`, whatever `min_observed` says.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The deviations, one per row; `missing()` where fewer than `max(min_observed, 2)` rows
 *   are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
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

/**
 * @brief Computes the trailing sample standard deviation, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The deviations, one per row; `missing()` where any row of the window is unobserved,
 *   and everywhere if `window` is `1`.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector rolling_stdev(math::SeriesView values, std::size_t window) {
  return rolling_stdev(values, window, window);
}

/**
 * @brief Computes the trailing minimum of the observed values.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The minima, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
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

/**
 * @brief Computes the trailing minimum, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The minima, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector rolling_min(math::SeriesView values, std::size_t window) {
  return rolling_min(values, window, window);
}

/**
 * @brief Computes the trailing maximum of the observed values.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The maxima, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
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

/**
 * @brief Computes the trailing maximum, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The maxima, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector rolling_max(math::SeriesView values, std::size_t window) {
  return rolling_max(values, window, window);
}

/**
 * @brief Computes the simple moving average; identical to `rolling_mean`.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The averages, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
inline math::Vector sma(math::SeriesView values, std::size_t window, std::size_t min_observed) {
  return rolling_mean(values, window, min_observed);
}

/**
 * @brief Computes the simple moving average, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The averages, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector sma(math::SeriesView values, std::size_t window) {
  return rolling_mean(values, window, window);
}

/**
 * @brief Computes the linearly weighted moving average.
 *
 * Weights rise `1, 2, ..., n` across the window, the most recent row heaviest. The result is
 * divided by the sum of the weights of the observed rows only.
 *
 * @param values The series; `NaN` entries are unobserved and skipped.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @return The averages, one per row; `missing()` where fewer than `min_observed` rows are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
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

/**
 * @brief Computes the linearly weighted moving average, requiring every row of the window.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at; all must be observed.
 * @return The averages, one per row; `missing()` where any row of the window is unobserved.
 * @throws std::invalid_argument if `window` is zero.
 */
inline math::Vector wma(math::SeriesView values, std::size_t window) {
  return wma(values, window, window);
}

/**
 * @brief Converts an EMA span to its smoothing factor, `2 / (span + 1)`.
 *
 * This is the convention charting packages and `pandas.ewm(span=...)` use:
 * `ema(closes, alpha_from_span(12))` is a "12-period EMA". Wilder's smoothing (`alpha = 1 / n`)
 * is span `2n - 1`.
 *
 * @param span The EMA period, at least `1`.
 * @return The smoothing factor, in `(0, 1]`.
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
 * @brief Advances an exponential moving average by one value.
 *
 * Use it in a live strategy that keeps its own level from bar to bar; it computes the same levels
 * as `ema` over the same bars:
 *
 *     Scalar level = missing();
 *     level = ema_step(level, close, alpha);   // once per bar
 *
 * @param level The previous level, or `missing()` before the first observation.
 * @param value The new value; if unobserved, `level` is returned unchanged.
 * @param alpha The smoothing factor, in `(0, 1]`; see `alpha_from_span`.
 * @return `alpha * value + (1 - alpha) * level`, or `value` itself if `level` is unobserved.
 * @throws std::invalid_argument unless `0 < alpha <= 1`.
 * @note If the arithmetic itself overflows to `NaN` (infinities of opposite sign), the next
 *   observed value reseeds the level, whereas `ema` keeps the `NaN`.
 */
inline Scalar ema_step(Scalar level, Scalar value, Scalar alpha) {
  rolling_detail::check_alpha(alpha);
  if (!observed(value)) {
    return level;
  }
  return observed(level) ? level + alpha * (value - level) : value;
}

/**
 * @brief Deleted: an integer here would be a span. Pass `alpha_from_span(span)` instead.
 *
 * A `long double` smoothing factor is ambiguous between the overloads; cast it to `Scalar`.
 */
Scalar ema_step(Scalar level, Scalar value, std::size_t span) = delete;

/**
 * @brief Computes the exponentially weighted moving average of a series.
 *
 * The level is seeded with the first observed value and then updated as
 * `alpha * value + (1 - alpha) * level` (the update `ema_step` makes). An unobserved value is
 * skipped: the level carries over it and that row is `missing()`. On observed rows the result
 * equals `pandas.Series.ewm(alpha=alpha, adjust=False, ignore_na=True).mean()` up to rounding.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param alpha The smoothing factor, in `(0, 1]`; for a period, pass `alpha_from_span(span)`.
 * @param min_observed How many values must have been observed before a level is reported; raise it
 *   to hide the rows where the seed still dominates.
 * @return The levels, one per row.
 * @throws std::invalid_argument unless `0 < alpha <= 1`, or if `min_observed` is zero (even for an
 *   empty series).
 * @warning In a live loop, keep the level yourself with `ema_step`. Running `ema` over a trailing
 *   `SeriesBuffer` view reseeds from the oldest row each time and gives different levels.
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
 * @brief Deleted: an integer here would be a span. Write `ema(values, alpha_from_span(span))`.
 *
 * A `long double` smoothing factor is ambiguous between the overloads; cast it to `Scalar`.
 */
math::Vector ema(math::SeriesView values, std::size_t span, std::size_t min_observed = 1) = delete;

} // namespace analytics
} // namespace algotradelib
