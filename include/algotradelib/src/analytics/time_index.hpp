#pragma once

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
 * @brief The instants a frame's rows correspond to, strictly ascending.
 *
 * Maps an instant to a row position. Lookups (`find`, `asof`) are binary searches, meant
 * for asking about one instant at a time.
 */
class TimeIndex {
public:
  /** @brief The instant type of the index. */
  using Instant = std::chrono::system_clock::time_point;

  /** @brief Constructs an empty index. */
  TimeIndex() = default;

  /**
   * @brief Constructs an index over a sequence of instants.
   *
   * @param stamps The instants, one per row, strictly ascending.
   * @throws std::invalid_argument if `stamps` is not strictly ascending.
   */
  explicit TimeIndex(std::vector<Instant> stamps) : stamps_(std::move(stamps)) {
    for (std::size_t position = 1; position < stamps_.size(); ++position) {
      if (!(stamps_[position - 1] < stamps_[position])) {
        throw std::invalid_argument("time index must be strictly ascending");
      }
    }
  }

  /**
   * @brief Returns the number of instants.
   * @return The number of rows the index covers.
   */
  std::size_t size() const noexcept { return stamps_.size(); }

  /**
   * @brief Tests whether the index is empty.
   * @return `true` if it holds no instants.
   */
  bool empty() const noexcept { return stamps_.empty(); }

  /**
   * @brief Returns the instant of one row.
   *
   * @param position The row position.
   * @return The instant at `position`.
   * @throws std::out_of_range if `position` is past the end.
   */
  const Instant& at(std::size_t position) const {
    if (position >= stamps_.size()) {
      throw std::out_of_range("time index position is past the end");
    }
    return stamps_[position];
  }

  /**
   * @brief Returns all instants.
   * @return The instants, ascending.
   */
  const std::vector<Instant>& stamps() const noexcept { return stamps_; }

  /**
   * @brief Finds the row of an exact instant.
   *
   * @param when The instant to find.
   * @return Its position, or `std::nullopt` if `when` is not in the index.
   */
  std::optional<std::size_t> find(const Instant& when) const {
    const auto found = std::lower_bound(stamps_.begin(), stamps_.end(), when);
    if (found == stamps_.end() || *found != when) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(found - stamps_.begin());
  }

  /**
   * @brief Finds the latest row at or before an instant ("as of" lookup).
   *
   * Never looks forward, so it is safe to use in a backtest.
   *
   * @param when The instant to look up.
   * @return The position of the latest instant not after `when`, or `std::nullopt` if
   *   `when` precedes every instant in the index.
   */
  std::optional<std::size_t> asof(const Instant& when) const {
    const auto above = std::upper_bound(stamps_.begin(), stamps_.end(), when);
    if (above == stamps_.begin()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>((above - 1) - stamps_.begin());
  }

  /**
   * @brief Compares two indices instant by instant.
   * @param lhs The first index.
   * @param rhs The second index.
   * @return `true` if both hold the same instants.
   */
  friend bool operator==(const TimeIndex& lhs, const TimeIndex& rhs) {
    return lhs.stamps_ == rhs.stamps_;
  }

  /**
   * @brief Compares two indices for inequality.
   * @param lhs The first index.
   * @param rhs The second index.
   * @return `true` if they differ in any instant.
   */
  friend bool operator!=(const TimeIndex& lhs, const TimeIndex& rhs) { return !(lhs == rhs); }

private:
  std::vector<Instant> stamps_;
};

} // namespace analytics
} // namespace algotradelib
