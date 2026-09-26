#pragma once

#include "../event.hpp"
#include "../intent.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <optional>

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
 * Returns the trailing `count + 1` rows of `values`, or nothing when there are too few.
 *
 * The reason every strategy here passes a slice rather than the whole series: an indicator over a
 * cached window is `O(rows x window)`, and a strategy needs only the last two values. Slicing first
 * turns a per-bar cost that grows with the cache into one that does not.
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
