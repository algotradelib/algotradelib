#pragma once

#include "./scalar.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * The instants a frame's rows correspond to, strictly ascending.
 *
 * Strictly ascending is checked on construction rather than assumed. Two frames are
 * only alignable if their indices mean the same thing, and a repeated or out-of-order
 * instant makes every lookup below ambiguous — which is exactly the failure that
 * produces plausible, wrong cross-sectional numbers. `BarSeries::ascending()` checks
 * the same invariant one layer down, so a series that passed it converts here without
 * incident.
 */
class TimeIndex {
public:
  /** Instant type the index is expressed in. */
  using Instant = std::chrono::system_clock::time_point;

  /** Constructs an empty index. */
  TimeIndex() = default;

  /**
   * Constructs an index over `stamps`.
   *
   * @throws std::invalid_argument if `stamps` is not strictly ascending.
   */
  explicit TimeIndex(std::vector<Instant> stamps) : stamps_(std::move(stamps)) {
    for (std::size_t position = 1; position < stamps_.size(); ++position) {
      if (!(stamps_[position - 1] < stamps_[position])) {
        throw std::invalid_argument("time index must be strictly ascending");
      }
    }
  }

  /** Returns how many instants the index holds. */
  std::size_t size() const noexcept { return stamps_.size(); }

  /** Returns whether the index holds no instants. */
  bool empty() const noexcept { return stamps_.empty(); }

  /**
   * Returns the instant at `position`.
   *
   * @throws std::out_of_range if `position` is past the end.
   */
  const Instant& at(std::size_t position) const {
    if (position >= stamps_.size()) {
      throw std::out_of_range("time index position is past the end");
    }
    return stamps_[position];
  }

  /** Returns the instants, for a caller that needs the whole sequence. */
  const std::vector<Instant>& stamps() const noexcept { return stamps_; }

  /** Returns the position of `when`, or nothing when it is not in the index. */
  std::optional<std::size_t> find(const Instant& when) const {
    const auto found = std::lower_bound(stamps_.begin(), stamps_.end(), when);
    if (found == stamps_.end() || *found != when) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(found - stamps_.begin());
  }

  /**
   * Returns the latest position at or before `when`, or nothing if none exists.
   *
   * This is the lookup a strategy needs: "what did I know as of this instant". It
   * never looks forward, which is the property that makes it safe to use in a
   * backtest, and it returns nothing rather than position zero when `when` precedes
   * the whole index — there is no observation to report, and reporting the first one
   * would be a lookahead of the worst kind.
   */
  std::optional<std::size_t> asof(const Instant& when) const {
    const auto above = std::upper_bound(stamps_.begin(), stamps_.end(), when);
    if (above == stamps_.begin()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>((above - 1) - stamps_.begin());
  }

  /** Compares two indices instant by instant. */
  friend bool operator==(const TimeIndex& lhs, const TimeIndex& rhs) {
    return lhs.stamps_ == rhs.stamps_;
  }

  /** Compares two indices for inequality. */
  friend bool operator!=(const TimeIndex& lhs, const TimeIndex& rhs) { return !(lhs == rhs); }

private:
  std::vector<Instant> stamps_;
};

} // namespace analytics
} // namespace algotradelib
