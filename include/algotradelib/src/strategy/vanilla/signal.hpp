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
 * @file
 * @brief Helpers for the vanilla strategies that compute indicators.
 *
 * Needs a linear algebra backend (`ATL_USE_EIGEN=ON`). `BuyAndHold` and `TimedLiquidation` do not.
 */

/** @brief The last two values of an indicator line, for testing a crossing. */
struct Crossing {
  /** @brief The value on the previous bar. */
  math::Scalar previous_{0};
  /** @brief The value on the latest bar. */
  math::Scalar current_{0};

  /**
   * @brief Tests whether this line crossed above another on the latest bar.
   * @param other The other line's last two values.
   * @return `true` if this line was at or below `other` before and is above it now.
   */
  bool rose_above(const Crossing& other) const {
    return previous_ <= other.previous_ && other.current_ < current_;
  }

  /**
   * @brief Tests whether this line crossed below another on the latest bar.
   * @param other The other line's last two values.
   * @return `true` if this line was at or above `other` before and is below it now.
   */
  bool fell_below(const Crossing& other) const {
    return other.previous_ <= previous_ && current_ < other.current_;
  }
};

/**
 * @brief Returns the last two values of a line.
 * @param line The indicator line.
 * @return The two values, or `std::nullopt` if the line has fewer than two rows or either value is
 *   unobserved (a line still warming up cannot have crossed).
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
 * @brief Computes the bias-adjusted exponential moving average.
 *
 * At each row, the exponentially weighted mean of the observations so far, with weights `1, q, q^2,
 * ...` (newest first, `q = 1 - alpha`) normalized to sum to one. On finite observed rows it equals
 * pandas' `ewm(alpha=alpha, adjust=True, ignore_na=True).mean()`. Unlike `analytics::ema`, the first
 * observation never weighs more than its exponential share, so use it on a short series with no
 * history before its first row. A constant series gives exactly that constant.
 *
 * @param values The series; `NaN` entries are skipped and their rows left `missing()`.
 * @param alpha The smoothing factor, in `(0, 1]`.
 * @param min_observed How many observations must be seen before a value is reported.
 * @return The averages, one per row.
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
 * @brief Computes the exponential moving average `MovingAverageCrossover` reads.
 *
 * With at least `3 * span` observed closes, `analytics::ema` seeded at the oldest; with fewer,
 * `adjusted_ema`, since the seed would still weigh too much. Reported from the `span`-th
 * observation.
 *
 * @param closes The whole close series.
 * @param span The EMA span.
 * @return The line, one value per row.
 * @throws std::invalid_argument if `span` is zero.
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
 * @brief Returns the last `count + 1` rows of a series.
 *
 * Slice before computing a windowed indicator when only its latest values are needed, so the cost
 * per bar does not grow with the cached history. Not for a recursive indicator such as an EMA, whose
 * value depends on every earlier row.
 *
 * @param values The series.
 * @param count The indicator's window; one more row is kept for the previous value.
 * @return A view of the last `count + 1` rows, or `std::nullopt` if there are fewer.
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
