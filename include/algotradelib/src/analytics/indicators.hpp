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
 * @brief Named technical indicators: z-score, Bollinger bands, RSI, ATR, MACD, and trailing
 *   covariance, correlation and beta.
 *
 * They follow the conventions of `rolling.hpp`: the result is as long as the input, a value that
 * cannot be computed is `missing()`, and nothing is reported before the window holds enough
 * observations. Where an indicator has a conventional parameterization (Wilder's smoothing for
 * RSI and ATR, spans for MACD), its documentation says which one it follows.
 */

namespace indicator_detail {

/**
 * Applies Wilder's smoothing to `values`, seeding on the first `period` observed values.
 *
 * Wilder's average is `(previous * (period - 1) + value) / period`, equivalently an
 * exponential average with `alpha = 1 / period`, seeded with the simple mean of the first
 * `period` values rather than with the first value alone. Both halves of that are part of
 * the convention: RSI and ATR as every charting package draws them use this smoothing and
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

/**
 * Two series split so a co-moment over their shared rows is a pair of vectorized reductions.
 *
 * `present_` is `1` only where **both** series carry a value, and each `left_`/`right_` is zeroed
 * wherever that is false. Pairing matters, and getting it wrong is subtle: a covariance taken
 * over the rows where both are present, divided by deviations each taken over its own series'
 * rows, is not a correlation — the three numbers describe different samples, and the quotient can
 * leave `[-1, 1]` entirely. So every co-moment here reduces over one row set, and this is it.
 */
struct Paired {
  math::Vector left_;
  math::Vector right_;
  math::Vector present_;
};

/** Splits two series onto their shared observed rows, in one pass each. */
inline Paired pair_series(math::SeriesView left, math::SeriesView right) {
  const auto left_parts = math::observed_parts(left);
  const auto right_parts = math::observed_parts(right);
  Paired paired;
  paired.present_ = math::both_present(left_parts.present_, right_parts.present_);
  paired.left_ = left_parts.values_.cwiseProduct(paired.present_);
  paired.right_ = right_parts.values_.cwiseProduct(paired.present_);
  return paired;
}

/** One window of a `Paired` series, reduced to the means a co-moment is taken about. */
struct Pairing {
  bool reported_{false};
  std::size_t count_{0};
  Scalar mean_left_{0};
  Scalar mean_right_{0};
};

/** Reduces one window of a paired series. */
inline Pairing reduce_pair(
  const Paired& paired,
  math::Index first,
  math::Index length,
  std::size_t needed
) {
  Pairing result;
  const Scalar count = paired.present_.segment(first, length).sum();
  result.count_ = static_cast<std::size_t>(count);
  if (result.count_ < needed || result.count_ < 2) {
    return result;
  }
  result.mean_left_ = paired.left_.segment(first, length).sum() / count;
  result.mean_right_ = paired.right_.segment(first, length).sum() / count;
  result.reported_ = true;
  return result;
}

/** Returns one window's values centred on its own mean, absent rows contributing zero. */
inline math::Vector centred(
  const math::Vector& values,
  const math::Vector& present,
  math::Index first,
  math::Index length,
  Scalar mean
) {
  return values.segment(first, length) - mean * present.segment(first, length);
}

/**
 * Throws unless two series describe the same rows.
 *
 * Two series of different lengths cannot be paired by row at all, and quietly reducing to
 * the shorter one would silently align the wrong instants.
 */
inline void require_same_rows(math::SeriesView left, math::SeriesView right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("paired series must have the same number of rows");
  }
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
    if (spread.deviation_ == Scalar{0}) {
      // The data was there and the statistic does not exist for it, which is a different
      // answer from "no data yet" and now says so.
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

/** @brief The three lines of Bollinger bands, each as long as the input. */
struct BollingerBands {
  /** @brief The trailing mean. */
  math::Vector middle_;
  /** @brief `middle_` plus `multiplier` trailing sample standard deviations. */
  math::Vector upper_;
  /** @brief `middle_` minus `multiplier` trailing sample standard deviations. */
  math::Vector lower_;
};

/**
 * @brief Computes Bollinger bands: the trailing mean and bands `multiplier` sample standard
 *   deviations either side of it.
 *
 * Uses the sample deviation (as `rolling_stdev`); packages that use the population deviation draw
 * bands narrower by a factor of `sqrt((n - 1) / n)` (about 2.5% at 20 rows). A window with no dispersion gives
 * three equal lines.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported.
 * @param multiplier The band width in standard deviations.
 * @return The three lines; each row is `missing()` where fewer than `max(min_observed, 2)` rows
 *   are observed.
 * @throws std::invalid_argument if `window` or `min_observed` is zero, or `min_observed > window`.
 */
inline BollingerBands bollinger(
  math::SeriesView values,
  std::size_t window,
  Scalar multiplier,
  std::size_t min_observed
) {
  rolling_detail::check(window, min_observed);
  const auto parts = math::observed_parts(values);
  const auto rows = static_cast<std::size_t>(values.size());
  BollingerBands bands{math::unobserved(rows), math::unobserved(rows), math::unobserved(rows)};
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const auto spread = rolling_detail::dispersion(parts, first, row - first + 1, min_observed);
    if (!spread.reported_) {
      continue;
    }
    bands.middle_[row] = spread.mean_;
    bands.upper_[row] = spread.mean_ + multiplier * spread.deviation_;
    bands.lower_[row] = spread.mean_ - multiplier * spread.deviation_;
  }
  return bands;
}

/**
 * @brief Computes Bollinger bands, requiring every row of the window.
 *
 * The defaults, 20 rows and 2 deviations, are Bollinger's own.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param window The number of trailing rows; all must be observed.
 * @param multiplier The band width in standard deviations.
 * @return The three lines; see the overload with `min_observed`.
 * @throws std::invalid_argument if `window` is zero.
 */
inline BollingerBands bollinger(
  math::SeriesView values,
  std::size_t window = 20,
  Scalar multiplier = Scalar{2}
) {
  return bollinger(values, window, multiplier, window);
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

/**
 * @brief Computes each bar's true range, `max(high - low, |high - prev close|, |low - prev close|)`.
 *
 * Where the previous close is unavailable (row 0, or an unobserved close), the range is
 * `high - low`.
 *
 * @param highs The bar highs.
 * @param lows The bar lows, as many rows as `highs`.
 * @param closes The bar closes, as many rows as `highs`.
 * @return The true ranges, one per row; `missing()` where the high or low is unobserved.
 * @throws std::invalid_argument if the three series differ in length.
 */
inline math::Vector true_range(
  math::SeriesView highs,
  math::SeriesView lows,
  math::SeriesView closes
) {
  indicator_detail::require_same_rows(highs, lows);
  indicator_detail::require_same_rows(highs, closes);

  math::Vector result = math::unobserved(static_cast<std::size_t>(highs.size()));
  for (math::Index row = 0; row < highs.size(); ++row) {
    if (!observed(highs[row]) || !observed(lows[row])) {
      continue;
    }
    Scalar range = highs[row] - lows[row];
    if (row > 0 && observed(closes[row - 1])) {
      const Scalar previous = closes[row - 1];
      range = std::max(range, std::abs(highs[row] - previous));
      range = std::max(range, std::abs(lows[row] - previous));
    }
    result[row] = range;
  }
  return result;
}

/**
 * @brief Computes the average true range with Wilder's smoothing.
 *
 * The first value is the simple mean of the first `period` true ranges; each later value is
 * `(previous * (period - 1) + range) / period`. The result is in price units.
 *
 * @param highs The bar highs.
 * @param lows The bar lows, as many rows as `highs`.
 * @param closes The bar closes, as many rows as `highs`.
 * @param period The number of bars averaged; 14 is Wilder's.
 * @return The average true range, one per row; `missing()` until `period` true ranges are observed.
 * @throws std::invalid_argument if `period` is zero or the series differ in length.
 */
inline math::Vector atr(
  math::SeriesView highs,
  math::SeriesView lows,
  math::SeriesView closes,
  std::size_t period = 14
) {
  if (period == 0) {
    throw std::invalid_argument("atr period must be at least one bar");
  }
  return indicator_detail::wilder(true_range(highs, lows, closes), period);
}

/** @brief The three lines of a MACD, each as long as the input. */
struct MacdLines {
  /** @brief The fast EMA minus the slow EMA. */
  math::Vector macd_;
  /** @brief The EMA of `macd_`. */
  math::Vector signal_;
  /** @brief `macd_` minus `signal_`. */
  math::Vector histogram_;
};

/**
 * @brief Computes the MACD (moving-average convergence/divergence) lines.
 *
 * `macd = ema(fast) - ema(slow)`, `signal = ema(macd)`, and `histogram = macd - signal`, each EMA
 * with the smoothing factor `alpha_from_span(span)`.
 *
 * @param values The series; `NaN` entries are unobserved.
 * @param fast The fast EMA span.
 * @param slow The slow EMA span; must be greater than `fast`.
 * @param signal_span The signal EMA span.
 * @return The three lines.
 * @throws std::invalid_argument if `fast >= slow`, or a span is zero.
 * @warning As with `ema`, recomputing a MACD over a trailing window gives different values from
 *   one kept bar by bar over the whole history.
 */
inline MacdLines macd(
  math::SeriesView values,
  std::size_t fast = 12,
  std::size_t slow = 26,
  std::size_t signal_span = 9
) {
  if (fast >= slow) {
    throw std::invalid_argument("macd fast span must be shorter than the slow span");
  }
  const math::Vector quick = ema(values, alpha_from_span(fast));
  const math::Vector slower = ema(values, alpha_from_span(slow));

  const auto rows = static_cast<std::size_t>(values.size());
  MacdLines lines{math::unobserved(rows), math::unobserved(rows), math::unobserved(rows)};
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(quick[row]) && observed(slower[row])) {
      lines.macd_[row] = quick[row] - slower[row];
    }
  }
  lines.signal_ = ema(lines.macd_, alpha_from_span(signal_span));
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(lines.macd_[row]) && observed(lines.signal_[row])) {
      lines.histogram_[row] = lines.macd_[row] - lines.signal_[row];
    }
  }
  return lines;
}

/**
 * @brief Computes the trailing sample covariance of two series (divisor `count - 1`).
 *
 * Only rows on which both series are observed take part.
 *
 * @param left The first series.
 * @param right The second series, with as many rows as `left`.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported. Counts rows where both are observed.
 * @return The covariances, one per row; `missing()` where fewer than `max(min_observed, 2)` rows
 *   are paired.
 * @throws std::invalid_argument if the series differ in length, `window` or `min_observed` is
 *   zero, or `min_observed > window`.
 */
inline math::Vector rolling_cov(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window,
  std::size_t min_observed
) {
  indicator_detail::require_same_rows(left, right);
  rolling_detail::check(window, min_observed);
  const auto paired = indicator_detail::pair_series(left, right);
  math::Vector result = math::unobserved(static_cast<std::size_t>(left.size()));
  for (math::Index row = 0; row < left.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    const auto reduced = indicator_detail::reduce_pair(paired, first, length, min_observed);
    if (!reduced.reported_) {
      continue;
    }
    const math::Vector left_centred = indicator_detail::centred(
      paired.left_, paired.present_, first, length, reduced.mean_left_
    );
    const math::Vector right_centred = indicator_detail::centred(
      paired.right_, paired.present_, first, length, reduced.mean_right_
    );
    result[row] = left_centred.dot(right_centred) / static_cast<Scalar>(reduced.count_ - 1);
  }
  return result;
}

/**
 * @brief Computes the trailing sample covariance, requiring both series on every row of the window.
 *
 * @param left The first series.
 * @param right The second series, with as many rows as `left`.
 * @param window The number of trailing rows.
 * @return The covariances, one per row.
 * @throws std::invalid_argument if the series differ in length or `window` is zero.
 */
inline math::Vector rolling_cov(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window
) {
  return rolling_cov(left, right, window, window);
}

/**
 * @brief Computes the trailing Pearson correlation of two series.
 *
 * Only rows on which both series are observed take part, so the result stays in `[-1, 1]`
 * when the series have different gaps.
 *
 * @param left The first series.
 * @param right The second series, with as many rows as `left`.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported. Counts rows where both are observed.
 * @return The correlations, one per row; `missing()` where fewer than `max(min_observed, 2)` rows
 *   are paired, and `math::undefined()` where either series is constant over the window.
 * @throws std::invalid_argument if the series differ in length, `window` or `min_observed` is
 *   zero, or `min_observed > window`.
 */
inline math::Vector rolling_corr(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window,
  std::size_t min_observed
) {
  indicator_detail::require_same_rows(left, right);
  rolling_detail::check(window, min_observed);
  const auto paired = indicator_detail::pair_series(left, right);
  math::Vector result = math::unobserved(static_cast<std::size_t>(left.size()));
  for (math::Index row = 0; row < left.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    const auto reduced = indicator_detail::reduce_pair(paired, first, length, min_observed);
    if (!reduced.reported_) {
      continue;
    }
    const math::Vector left_centred = indicator_detail::centred(
      paired.left_, paired.present_, first, length, reduced.mean_left_
    );
    const math::Vector right_centred = indicator_detail::centred(
      paired.right_, paired.present_, first, length, reduced.mean_right_
    );
    const Scalar squared_left = left_centred.squaredNorm();
    const Scalar squared_right = right_centred.squaredNorm();
    if (squared_left == Scalar{0} || squared_right == Scalar{0}) {
      // A constant series. Correlation with it is undefined, not zero.
      result[row] = math::undefined();
      continue;
    }
    result[row] = left_centred.dot(right_centred) / std::sqrt(squared_left * squared_right);
  }
  return result;
}

/**
 * @brief Computes the trailing correlation, requiring both series on every row of the window.
 *
 * @param left The first series.
 * @param right The second series, with as many rows as `left`.
 * @param window The number of trailing rows.
 * @return The correlations, one per row.
 * @throws std::invalid_argument if the series differ in length or `window` is zero.
 */
inline math::Vector rolling_corr(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window
) {
  return rolling_corr(left, right, window, window);
}

/**
 * @brief Computes the trailing beta of an asset against a market, `cov(asset, market) / var(market)`.
 *
 * Only rows on which both series are observed take part.
 *
 * @param asset The asset's **returns** (not prices; a beta of price levels is meaningless).
 * @param market The market's returns, as many rows as `asset`.
 * @param window The number of trailing rows each result looks at, observed or not.
 * @param min_observed How many of those rows must be observed for a result to be reported. Counts rows where both are observed.
 * @return The betas, one per row; `missing()` where fewer than `max(min_observed, 2)` rows are
 *   paired, and `math::undefined()` where the market does not move over the window.
 * @throws std::invalid_argument if the series differ in length, `window` or `min_observed` is
 *   zero, or `min_observed > window`.
 */
inline math::Vector beta(
  math::SeriesView asset,
  math::SeriesView market,
  std::size_t window,
  std::size_t min_observed
) {
  indicator_detail::require_same_rows(asset, market);
  rolling_detail::check(window, min_observed);
  const auto paired = indicator_detail::pair_series(asset, market);
  math::Vector result = math::unobserved(static_cast<std::size_t>(asset.size()));
  for (math::Index row = 0; row < asset.size(); ++row) {
    const math::Index first = rolling_detail::window_start(row, window);
    const math::Index length = row - first + 1;
    const auto reduced = indicator_detail::reduce_pair(paired, first, length, min_observed);
    if (!reduced.reported_) {
      continue;
    }
    const math::Vector market_centred = indicator_detail::centred(
      paired.right_, paired.present_, first, length, reduced.mean_right_
    );
    const Scalar squared_market = market_centred.squaredNorm();
    if (squared_market == Scalar{0}) {
      // No market movement to attribute the asset's movement to.
      result[row] = math::undefined();
      continue;
    }
    const math::Vector asset_centred = indicator_detail::centred(
      paired.left_, paired.present_, first, length, reduced.mean_left_
    );
    result[row] = asset_centred.dot(market_centred) / squared_market;
  }
  return result;
}

/**
 * @brief Computes the trailing beta, requiring both series on every row of the window.
 *
 * @param asset The asset's returns.
 * @param market The market's returns, as many rows as `asset`.
 * @param window The number of trailing rows.
 * @return The betas, one per row.
 * @throws std::invalid_argument if the series differ in length or `window` is zero.
 */
inline math::Vector beta(
  math::SeriesView asset,
  math::SeriesView market,
  std::size_t window
) {
  return beta(asset, market, window, window);
}

} // namespace analytics
} // namespace algotradelib
