#pragma once

#include <algotradelib/core>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief The closes of a bootstrap path, rebuilt from drawn returns and put on the instrument's price grid.
 */

namespace bootstrap_detail {

/** @brief A price grid in whole units of `10^-scale`, and the rounding of a path's level onto it. */
class Grid {
public:
  /** @brief A value in grid units is refused at or beyond 2^53, where a double would no longer hold every integer. */
  static constexpr std::int64_t LIMIT = std::int64_t{1} << 53;

  /**
   * @brief Reads the rule's bands at their own scale.
   * @param rule The price grid; it must outlive the grid.
   * @param who The caller's name, put before each refusal.
   * @throws std::invalid_argument for an `UNKNOWN` or unaligned rule, or a band or tick at or beyond 2^53 units.
   */
  Grid(const PriceRule& rule, const char* who) : rule_(rule), who_(who) {
    if (!rule.grid_known()) {
      throw refuse("the price grid is unknown");
    }
    build();
  }

  /**
   * @brief Returns a refusal naming the caller.
   * @param why The reason.
   * @return The exception.
   */
  std::invalid_argument refuse(const char* why) const {
    return std::invalid_argument(std::string(who_) + ": " + why);
  }

  /**
   * @brief Raises the scale to cover a value's, and reads the bands again at it.
   * @param values The values.
   * @throws std::invalid_argument for a band or tick at or beyond 2^53 units at the new scale.
   */
  void widen(const std::vector<Decimal64>& values) {
    for (const auto& value : values) {
      scale_ = std::max(scale_, value.scale());
    }
    build();
  }

  /**
   * @brief Returns a value in grid units.
   * @param value The value.
   * @return The value times `10^scale`.
   * @throws std::invalid_argument if it is at or beyond 2^53 units in magnitude.
   */
  std::int64_t units(const Decimal64& value) const {
    std::int64_t out = value.coefficient();
    for (auto step = value.scale();; ++step) {
      if (out >= LIMIT || out <= -LIMIT) {
        throw refuse("a value at or beyond 2^53 grid units");
      }
      if (step >= scale_) {
        return out;
      }
      out *= 10;
    }
  }

  /**
   * @brief Rounds a level to its nearest grid price, a tie going up.
   * @param level The level, in grid units.
   * @param zero The refusal for a price rounded to zero.
   * @return The price, at the grid's scale.
   * @throws std::invalid_argument for a level or neighbour at or beyond 2^53 units, a level below the first band, or
   *   a price rounded to zero.
   */
  Decimal64 round(double level, const char* zero) const {
    std::size_t band = 0;
    while (band + 1 < grid_.size() && static_cast<double>(grid_[band + 1].first) <= level) {
      ++band;
    }
    if (!(level < static_cast<double>(LIMIT)) || grid_.empty() || static_cast<double>(grid_[band].first) > level) {
      throw refuse("a level at or beyond 2^53 grid units, or below the first band");
    }
    const auto tick = grid_[band].second;
    auto lower = static_cast<std::int64_t>(std::floor(level / static_cast<double>(tick)));
    for (;;) {
      const std::int64_t lo = lower * tick;
      const std::int64_t hi = lo + tick;
      if (hi >= LIMIT) {
        throw refuse("a grid neighbour at or beyond 2^53 grid units");
      }
      if (static_cast<double>(lo) > level) {
        --lower;
      } else if (static_cast<double>(hi) <= level) {
        ++lower;
      } else {
        const auto nearest = level - static_cast<double>(lo) < static_cast<double>(hi) - level ? lo : hi;
        if (nearest == 0) {
          throw refuse(zero);
        }
        return Decimal64(nearest, scale_);
      }
    }
  }

private:
  // The bands in units, checked aligned; one unit unconstrained.
  void build() {
    grid_.clear();
    if (rule_.kind_ == PriceRule::Kind::UNCONSTRAINED) {
      grid_.emplace_back(0, 1);
      return;
    }
    for (const auto& band : rule_.bands_) {
      scale_ = std::max({scale_, band.from_.scale(), band.tick_.scale()});
    }
    for (const auto& band : rule_.bands_) {
      const auto start = units(band.from_);
      const auto tick = units(band.tick_);
      if (tick <= 0 || start % tick != 0 || (!grid_.empty() && start % grid_.back().second != 0)) {
        throw refuse("a band's start is not a multiple of its tick and of the previous band's");
      }
      grid_.emplace_back(start, tick);
    }
  }

  const PriceRule& rule_;
  const char* who_;
  std::uint8_t scale_{0};
  std::vector<std::pair<std::int64_t, std::int64_t>> grid_;
};

} // namespace bootstrap_detail

/**
 * @brief Rebuilds one instrument's closes along a bootstrap path (`analytics::bootstrap_indices`) and rounds each to
 *   its nearest grid price, a tie going up.
 *
 * Call it with the same `indices` for every instrument of a path, so they move on the same drawn days. The identity
 * draw (`0, 1, ..., n - 2`) returns `closes` as they are. Otherwise close 0 is the first close, and close `k` is
 * close `k - 1` times `closes[j + 1] / closes[j]` with `j = indices[k - 1]`, accumulated unrounded and rounded once
 * each. The arithmetic is in grid units (`10^-s`, `s` the largest scale among the closes and the rule's band
 * starts and ticks), exact below 2^53 units, so the result is the same on every platform.
 *
 * @param closes The closes, in order.
 * @param rule The price grid: `UNCONSTRAINED` rounds to one unit; a stated grid must be aligned (each band's start
 *   a multiple of its own tick and of the previous band's).
 * @param indices The drawn returns, each below `closes.size() - 1`.
 * @return `indices.size() + 1` closes.
 * @throws std::invalid_argument for fewer than two closes, a close at or below zero, too many indices or one out
 *   of range, an `UNKNOWN` or unaligned rule, or a band or tick at or beyond 2^53 units at the rule's own scale
 *   (the identity draw included); for a drawn path, a close, band, tick, level or grid neighbour at or beyond 2^53
 *   units, a level below the first band, or a close rounded to zero.
 */
inline std::vector<Decimal64> resample_closes(
  const std::vector<Decimal64>& closes, const PriceRule& rule, const std::vector<std::size_t>& indices
) {
  const auto refuse = [](const char* why) {
    return std::invalid_argument(std::string("resample_closes: ") + why);
  };
  if (closes.size() < 2 || indices.size() > closes.size() - 1) {
    throw refuse("needs two closes or more, and at most one index per return");
  }
  bool identity = indices.size() == closes.size() - 1;
  for (std::size_t k = 0; k < indices.size(); ++k) {
    identity = identity && indices[k] == k;
    if (indices[k] >= closes.size() - 1) {
      throw refuse("an index names no return");
    }
  }
  for (const auto& close : closes) {
    if (close.coefficient() <= 0) {
      throw refuse("a close at or below zero");
    }
  }
  // The bands checked aligned at their own scale, then at the closes'.
  bootstrap_detail::Grid grid(rule, "resample_closes");
  if (identity) {
    return closes;
  }
  grid.widen(closes);
  std::vector<double> in;
  for (const auto& close : closes) {
    in.push_back(static_cast<double>(grid.units(close)));
  }
  std::vector<Decimal64> out;
  double level = in.front();
  for (std::size_t k = 0; k <= indices.size(); ++k) {
    if (k > 0) {
      const double ratio = in[indices[k - 1] + 1] / in[indices[k - 1]];
      level = level * ratio;
    }
    out.push_back(grid.round(level, "a close rounded to zero"));
  }
  return out;
}

} // namespace backtest
} // namespace algotradelib
