#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace algotradelib {
namespace analytics {

/**
 * Trailing statistics over a series.
 *
 * Every function here keeps the two conventions `returns.hpp` states — the result is as
 * long as the input, and a value that cannot be computed is `missing()` — and adds a
 * third that is specific to windows:
 *
 * **A window reports only when it has enough observations, and the caller decides how
 * many.** `min_observations` defaults to `0`, which means the whole window: row `i` is
 * unobserved until `window` observed values have accumulated at or before it. Passing a
 * smaller number relaxes that, which is what makes a partial warm-up an explicit request
 * rather than an accident. Silently reporting a 20-day average computed from 3 days is the
 * second most common source of lookahead bias after bar stamping, so it cannot happen here
 * without being asked for.
 *
 * Unobserved values inside a window are **skipped**, not propagated, which is the masking
 * convention `math::missing()` documents: one absent print does not blank a whole column.
 * A window's divisor is therefore the count of values it actually saw, which is also why
 * `min_observations` is a count of observations rather than a count of rows.
 */

namespace rolling_detail {

/** The observed values of one window, reduced to what every statistic over it needs. */
struct Reduction {
  std::size_t count{0};
  Scalar sum{0};
};

/**
 * Reduces the observed values of `values[first, first + length)`.
 *
 * A plain loop rather than a vectorized `segment().sum()` because a single unobserved
 * value would poison the whole vectorized reduction, and masking it is the documented
 * convention.
 */
inline Reduction reduce(math::SeriesView values, math::Index first, math::Index length) {
  Reduction result;
  for (math::Index row = first; row < first + length; ++row) {
    const Scalar value = values[row];
    if (observed(value)) {
      ++result.count;
      result.sum += value;
    }
  }
  return result;
}

/**
 * Returns how many observations a window must hold before it reports.
 *
 * @throws std::invalid_argument if `window` is zero, or if more observations are demanded
 * than the window can ever hold — both are programming errors that would otherwise show up
 * as an all-unobserved result with no explanation.
 */
inline std::size_t required(std::size_t window, std::size_t min_observations) {
  if (window == 0) {
    throw std::invalid_argument("rolling window must be at least one row");
  }
  if (min_observations > window) {
    throw std::invalid_argument(
      "rolling window of " + std::to_string(window) + " rows cannot hold " +
      std::to_string(min_observations) + " observations"
    );
  }
  return min_observations == 0 ? window : min_observations;
}

/** Returns the first row of the window ending at `row`, clamped to the start of series. */
inline math::Index window_start(math::Index row, std::size_t window) {
  const auto span = static_cast<math::Index>(window);
  return row + 1 >= span ? row + 1 - span : math::Index{0};
}

} // namespace rolling_detail

/** Returns the trailing sum over `window` rows ending at each row. */
inline math::Vector rolling_sum(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto reduced = rolling_detail::reduce(values, first, row - first + 1);
    if (reduced.count >= needed) {
      result[row] = reduced.sum;
    }
  }
  return result;
}

/** Returns the trailing arithmetic mean over `window` rows ending at each row. */
inline math::Vector rolling_mean(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto reduced = rolling_detail::reduce(values, first, row - first + 1);
    if (reduced.count >= needed) {
      result[row] = reduced.sum / static_cast<Scalar>(reduced.count);
    }
  }
  return result;
}

/**
 * Returns the trailing **sample** standard deviation, dividing by `count - 1`.
 *
 * The sample divisor rather than the population one, because a trailing window is a sample
 * of a process that continues, not the whole population. A window holding fewer than two
 * observations is unobserved regardless of `min_observations`: one point has no dispersion
 * to report, and `count - 1` would be a division by zero.
 *
 * Computed in two passes over each window — the mean, then the squared deviations from it.
 * The one-pass `E[x²] - E[x]²` form is deliberately not used: on price levels, where the
 * mean is large relative to the dispersion, the two terms are nearly equal and their
 * difference loses most of its significant digits to cancellation. It can even come out
 * negative, which turns into a `NaN` at the square root with nothing to indicate that
 * arithmetic, not data, produced it.
 */
inline math::Vector rolling_stdev(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    const auto reduced = rolling_detail::reduce(values, first, length);
    if (reduced.count < needed || reduced.count < 2) {
      continue;
    }
    const Scalar mean = reduced.sum / static_cast<Scalar>(reduced.count);
    Scalar squared{0};
    for (math::Index at = first; at < first + length; ++at) {
      const Scalar value = values[at];
      if (observed(value)) {
        const Scalar deviation = value - mean;
        squared += deviation * deviation;
      }
    }
    result[row] = std::sqrt(squared / static_cast<Scalar>(reduced.count - 1));
  }
  return result;
}

/** Returns the smallest observed value in the trailing window ending at each row. */
inline math::Vector rolling_min(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    std::size_t count = 0;
    Scalar smallest{0};
    for (math::Index at = first; at <= row; ++at) {
      const Scalar value = values[at];
      if (!observed(value)) {
        continue;
      }
      if (count == 0 || value < smallest) {
        smallest = value;
      }
      ++count;
    }
    if (count >= needed) {
      result[row] = smallest;
    }
  }
  return result;
}

/** Returns the largest observed value in the trailing window ending at each row. */
inline math::Vector rolling_max(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    std::size_t count = 0;
    Scalar largest{0};
    for (math::Index at = first; at <= row; ++at) {
      const Scalar value = values[at];
      if (!observed(value)) {
        continue;
      }
      if (count == 0 || largest < value) {
        largest = value;
      }
      ++count;
    }
    if (count >= needed) {
      result[row] = largest;
    }
  }
  return result;
}

/**
 * Returns the simple moving average over `window` rows.
 *
 * Exactly `rolling_mean` with the same arguments, kept as a separate name because the
 * trading vocabulary has one and a reader should not have to wonder whether the two differ.
 * They do not, and this is the one line that guarantees it.
 */
inline math::Vector sma(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  return rolling_mean(values, window, min_observations);
}

/**
 * Returns the linearly weighted moving average over `window` rows.
 *
 * Weights ascend `1, 2, ... window` with the most recent row heaviest, and the result is
 * divided by the sum of the weights **actually used**. Skipping an unobserved row without
 * also removing its weight would bias every affected window toward zero, which is the kind
 * of error that shows up as a strategy that looks slightly worse than it is and is never
 * traced back to here.
 */
inline math::Vector wma(
  math::SeriesView values,
  std::size_t window,
  std::size_t min_observations = 0
) {
  const std::size_t needed = rolling_detail::required(window, min_observations);
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    std::size_t count = 0;
    Scalar weighted{0};
    Scalar weights{0};
    for (math::Index at = first; at <= row; ++at) {
      const Scalar value = values[at];
      if (!observed(value)) {
        continue;
      }
      const auto weight = static_cast<Scalar>(at - first + 1);
      weighted += weight * value;
      weights += weight;
      ++count;
    }
    if (count >= needed && weights > Scalar{0}) {
      result[row] = weighted / weights;
    }
  }
  return result;
}

/**
 * Returns the exponentially weighted moving average with a decay set by `span`.
 *
 * The smoothing factor is `alpha = 2 / (span + 1)` — the *span* convention, the one every
 * charting package and `pandas.ewm(span=...)` use, so an `ema(closes, 12)` here matches a
 * 12-period EMA drawn elsewhere. The alternative conventions (a half-life, a raw alpha,
 * Wilder's `1 / n`) are all reachable by choosing `span` accordingly; Wilder's smoothing is
 * `span = 2n - 1`, and `rsi` and `atr` use it internally rather than making a caller
 * remember that.
 *
 * The recursion is seeded with the first observed value rather than with an average of the
 * first `span` rows. That makes the result depend only on rows at or before each row, which
 * is the property that matters, at the cost of the early values leaning on the seed — pass
 * `min_observations = span` to suppress the region where it still dominates.
 *
 * An unobserved input is skipped: the level carries across it, and that row's output is
 * unobserved, because a row with no observation is one the indicator has nothing to say
 * about.
 *
 * **A caution that applies to this function and not to the others here.** An EMA's value
 * depends on the entire history feeding it, so running it over a trailing `RollingWindow`
 * view does *not* reproduce the value the same series would give a live loop — the window's
 * oldest row becomes a fresh seed every time it advances. For a live strategy, keep an
 * accumulator that updates once per bar. `sma` and the `rolling_*` functions have no such
 * problem: their windows are finite, so window and full series agree exactly.
 */
inline math::Vector ema(
  math::SeriesView values,
  std::size_t span,
  std::size_t min_observations = 0
) {
  if (span == 0) {
    throw std::invalid_argument("ema span must be at least one row");
  }
  const Scalar alpha = Scalar{2} / (static_cast<Scalar>(span) + Scalar{1});
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  Scalar level{0};
  std::size_t count = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    const Scalar value = values[row];
    if (!observed(value)) {
      continue;
    }
    level = count == 0 ? value : level + alpha * (value - level);
    ++count;
    if (count >= min_observations) {
      result[row] = level;
    }
  }
  return result;
}

} // namespace analytics
} // namespace algotradelib
