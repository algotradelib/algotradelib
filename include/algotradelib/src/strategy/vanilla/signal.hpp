#pragma once

#include "../event.hpp"
#include "../intent.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * Shared machinery for the strategies that read a series.
 *
 * Available only where the library was built with a linear-algebra backend, because these compute
 * indicators. `BuyAndHold` and `TimedLiquidation` need none and are available either way.
 */

/** The last two values of an indicator, which is what a crossing test needs. */
struct Crossing {
  /** The value on the previous bar. */
  math::Scalar previous_{0};
  /** The value on the latest bar. */
  math::Scalar current_{0};

  /** Returns whether the line rose through `other` on the latest bar. */
  bool rose_above(const Crossing& other) const {
    return previous_ <= other.previous_ && other.current_ < current_;
  }

  /** Returns whether the line fell through `other` on the latest bar. */
  bool fell_below(const Crossing& other) const {
    return other.previous_ <= previous_ && current_ < other.current_;
  }
};

/**
 * Returns the last two values of `line`, or nothing unless both are observed.
 *
 * Both, deliberately. A crossing is a statement about a change, so a line whose previous value is
 * still in its warm-up region cannot have crossed anything — and treating an absent previous value
 * as zero would fire a crossing on the first bar that produced a number, every time.
 */
inline std::optional<Crossing> last_two(const math::Vector& line) {
  if (line.size() < 2) {
    return std::nullopt;
  }
  const Crossing pair{line[line.size() - 2], line[line.size() - 1]};
  if (!math::observed(pair.previous_) || !math::observed(pair.current_)) {
    return std::nullopt;
  }
  return pair;
}

/**
 * Returns the compensated exponential average of `values`: at each row, the exponentially weighted mean
 * of the observations so far, its weights normalized to sum to one.
 *
 * With `q = 1 - alpha`, the newest observation weighs `1`, the one before `q`, then `q^2`, and so on,
 * divided by their sum `N`. For finite observations that is pandas'
 * `ewm(alpha=alpha, adjust=True, ignore_na=True)` on observed rows. Unlike `analytics::ema`, which seeds at the first observation and keeps `q^{k-1}` of its value
 * after `k` observations, no observation weighs more than its exponential share -- the reason to use it
 * on a series with no history before its first row; see `MovingAverageCrossover` for when.
 *
 * Computed as a running weighted mean, `N_t = 1 + q * N_{t-1}` and `C_t = C_{t-1} + (y_t - C_{t-1}) / N_t`.
 * That form stays exact on a finite constant series (a crossover comparing two lines with `<` must not
 * see a flat market as a crossing), and stays accurate for an `alpha` too small for `1 - q^k` to be
 * formed: it becomes the plain mean. Same masking as `analytics::ema`: unobserved inputs are skipped and
 * their rows left unobserved, and nothing is reported before `min_observed` observations. An infinity
 * counts as an observation, as it does there: the mean becomes infinite, then `NaN` at the next finite
 * value, and stays `NaN` -- except at `alpha = 1`, which keeps only the newest value.
 *
 * @throws std::invalid_argument unless `0 < alpha <= 1`, or if `min_observed` is zero.
 */
inline math::Vector adjusted_ema(math::SeriesView values, math::Scalar alpha, std::size_t min_observed = 1) {
  if (!(math::Scalar{0} < alpha && alpha <= math::Scalar{1})) {
    throw std::invalid_argument("an ema smoothing factor must be in (0, 1]");
  }
  if (min_observed == 0) {
    throw std::invalid_argument("ema must require at least one observation");
  }
  math::Vector result = math::unobserved(static_cast<std::size_t>(values.size()));
  const math::Scalar keep = math::Scalar{1} - alpha;
  math::Scalar mean{0};
  math::Scalar total{0};
  std::size_t count = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    const math::Scalar value = values[row];
    if (!math::observed(value)) {
      continue;
    }
    total = math::Scalar{1} + keep * total;
    // `alpha = 1` keeps only the newest value, exactly: `mean + (value - mean)` need not round back to it.
    mean = count == 0 || keep == math::Scalar{0} ? value : mean + (value - mean) / total;
    ++count;
    if (count >= min_observed) {
      result[row] = mean;
    }
  }
  return result;
}

/**
 * Returns the exponential average a crossover reads: of the whole of `closes`, spanning `span`.
 *
 * Reported from the `span`-th observation. With three spans of *observed* closes or more, the plain
 * `analytics::ema`, seeded at the oldest; with fewer, `adjusted_ema`: the seed would still weigh more than
 * about `e^-6`, and the series may have no earlier history to stand for. Observations, not rows, because a cache with
 * gaps holds fewer closes than rows. `MovingAverageCrossover`'s class comment has the reasons and the
 * measurements.
 */
inline math::Vector exponential_line(math::SeriesView closes, std::size_t span) {
  const auto alpha = analytics::alpha_from_span(span);
  std::size_t observations = 0;
  for (math::Index row = 0; row < closes.size(); ++row) {
    observations += math::observed(closes[row]) ? 1 : 0;
  }
  if (observations < 3 * span) {
    return adjusted_ema(closes, alpha, span);
  }
  return analytics::ema(closes, alpha, span);
}

/**
 * Returns the trailing `count + 1` rows of `values`, or nothing when there are too few.
 *
 * Why a strategy reading a windowed indicator passes a slice rather than the whole series: an indicator
 * over a cached window is `O(rows x window)`, and a strategy needs only the last two values. Slicing
 * first turns a per-bar cost that grows with the cache into one that does not. Not for a recursive
 * indicator, whose value depends on every row before the slice (`exponential_line`).
 *
 * `count + 1` because a crossing needs the value before the latest one as well.
 */
inline std::optional<math::SeriesView> trailing(math::SeriesView values, std::size_t count) {
  const auto needed = static_cast<math::Index>(count) + 1;
  if (values.size() < needed) {
    return std::nullopt;
  }
  return values.tail(needed);
}

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
