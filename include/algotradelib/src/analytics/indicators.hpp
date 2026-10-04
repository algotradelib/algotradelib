#pragma once

#include "./rolling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * The named indicators a strategy signals on.
 *
 * Built on `rolling.hpp` and keeping its three conventions: the result is as long as the
 * input, a value that cannot be computed is `missing()`, and nothing is reported before its
 * window holds enough observations.
 *
 * Each indicator that has a *conventional* parameterization says which one it follows, in
 * its own documentation, next to the arithmetic. This is not decoration. An RSI smoothed
 * with `2 / (n + 1)` rather than Wilder's `1 / n` produces different numbers from every
 * charting package, and a strategy tuned against a chart and traded against this library
 * would be trading a different indicator than the one it was fitted on.
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
 * Returns how many trailing standard deviations each value sits from its trailing mean.
 *
 * A window with no dispersion -- every observation identical -- is unobserved rather than
 * zero or an infinity. The z-score is genuinely undefined there, and a flat window is
 * common in real data: a halted instrument, or a tick-constrained price that did not move.
 *
 * Both the mean and the deviation come from a single pass over each window, which is why
 * this is not written as `(values - rolling_mean(...)) / rolling_stdev(...)`.
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

/** Returns the trailing z-score, requiring every row of the window to be observed. */
inline math::Vector zscore(math::SeriesView values, std::size_t window) {
  return zscore(values, window, window);
}

/** The three lines of a Bollinger band, all on the same rows as the input. */
struct BollingerBands {
  /** The trailing mean. */
  math::Vector middle_;
  /** `middle_` plus `multiplier` sample deviations. */
  math::Vector upper_;
  /** `middle_` minus `multiplier` sample deviations. */
  math::Vector lower_;
};

/**
 * Returns the trailing mean and the bands `multiplier` sample deviations either side of it.
 *
 * `window = 20` and `multiplier = 2` are Bollinger's own parameters and the ones every
 * charting package defaults to. The deviation is the sample one, matching `rolling_stdev`;
 * some packages use the population divisor, which makes their bands narrower on short
 * windows by a factor of `sqrt(n / (n - 1))` -- about 2.6% at 20 rows.
 *
 * A window with no dispersion collapses all three lines onto the mean, and that is the
 * right answer rather than an omission: a flat window's band genuinely has zero width, and
 * nothing here divides by the deviation. That is the difference from `zscore`, which is
 * unobserved on the same window because it does divide by it.
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

/** Returns Bollinger bands, requiring every row of the window to be observed. */
inline BollingerBands bollinger(
  math::SeriesView values,
  std::size_t window = 20,
  Scalar multiplier = Scalar{2}
) {
  return bollinger(values, window, multiplier, window);
}

/**
 * Returns Wilder's relative strength index over `period` price changes.
 *
 * `period = 14` is Wilder's own. The average gain and average loss are smoothed by
 * `indicator_detail::wilder`, and the index is `100 - 100 / (1 + gain / loss)`.
 *
 * The three degenerate cases are decided rather than left to the arithmetic. A zero average
 * loss with a non-zero average gain -- an unbroken run of up moves -- gives exactly 100,
 * which is the limit the ratio approaches rather than a division by zero; a zero average
 * gain against a non-zero average loss gives 0 by the same reasoning; and both zero, a price
 * that has not moved at all, gives 50, because a flat series shows neither strength nor
 * weakness and any other answer would read as a signal.
 *
 * The first `period` rows are unobserved: row 0 has no price change at all, and the
 * smoothing needs `period` changes before it has a seed.
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
 * Returns the true range of each bar: the widest of the three spans that can gap.
 *
 * `max(high - low, |high - previous close|, |low - previous close|)`. The two terms
 * involving the previous close are what make this a *true* range rather than a bar's own
 * height: an instrument that gaps overnight and then trades in a narrow band moved further
 * than its bar is tall, and a volatility measure that missed that would understate the risk
 * of holding it.
 *
 * Row 0 has no previous close, so it is the bar's own height.
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
 * Returns the average true range over `period` bars, smoothed Wilder's way.
 *
 * `period = 14` is Wilder's own, and the smoothing is his: the seed is the simple mean of
 * the first `period` true ranges and each later value is `(previous * 13 + range) / 14`.
 *
 * In the same units as the price, not a ratio, which is what makes it usable directly as a
 * stop distance or a position-size divisor.
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

/** The three series of a MACD, all on the same rows as the input. */
struct MacdLines {
  /** The fast average less the slow one. */
  math::Vector macd_;
  /** The exponential average of `macd_`. */
  math::Vector signal_;
  /** `macd_` less `signal_`. */
  math::Vector histogram_;
};

/**
 * Returns the moving-average convergence/divergence lines.
 *
 * `macd = ema(fast) - ema(slow)`, `signal = ema(macd, alpha_from_span(signal_span))`, and
 * the histogram is their difference. `12, 26, 9` are the conventional spans; each is turned into a smoothing
 * factor by `alpha_from_span`, the convention MACD is quoted in.
 *
 * The signal line is an exponential average *of an exponential average*, so the caution in
 * `ema` applies doubly here: a MACD recomputed over a trailing window is not the MACD a
 * live loop would have produced from the same prices.
 *
 * @throws std::invalid_argument if the fast span is not shorter than the slow one. A MACD
 * with the two reversed is the negation of the intended one, which is a sign error that
 * survives every test of magnitude and shows up only as a strategy that trades backwards.
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
 * Returns the trailing **sample** covariance of two series, over their paired rows.
 *
 * Divides by `count - 1`, matching `rolling_stdev`, so a correlation formed from these
 * pieces is consistent. Only rows on which both series are observed take part; see
 * `indicator_detail::pair` for why that is not negotiable.
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

/** Returns the trailing sample covariance, requiring every row of the window to be paired. */
inline math::Vector rolling_cov(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window
) {
  return rolling_cov(left, right, window, window);
}

/**
 * Returns the trailing Pearson correlation of two series, over their paired rows.
 *
 * Computed from one pass over each window so that the covariance and both deviations
 * describe the same sample. That is what bounds the result to `[-1, 1]`; the same three
 * numbers taken from independent calls are not so bounded when the two series have
 * different gaps.
 *
 * A window in which either series has no dispersion is unobserved. Correlation with a
 * constant is undefined, not zero.
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

/** Returns the trailing correlation, requiring every row of the window to be paired. */
inline math::Vector rolling_corr(
  math::SeriesView left,
  math::SeriesView right,
  std::size_t window
) {
  return rolling_corr(left, right, window, window);
}

/**
 * Returns the trailing beta of `asset` against `market`: `cov(asset, market) / var(market)`.
 *
 * Takes **returns**, not prices. A beta computed on price levels measures the ratio of two
 * trends and is close to meaningless; the mistake is easy to make because it produces a
 * plausible-looking number rather than an error, so it is worth saying here.
 *
 * A window in which the market has no variance is unobserved: with no market movement to
 * attribute the asset's movement to, beta is undefined rather than zero or infinite.
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

/** Returns the trailing beta, requiring every row of the window to be paired. */
inline math::Vector beta(
  math::SeriesView asset,
  math::SeriesView market,
  std::size_t window
) {
  return beta(asset, market, window, window);
}

} // namespace analytics
} // namespace algotradelib
