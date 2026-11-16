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
 *   of range, an `UNKNOWN` or unaligned rule; for a drawn path, a close, band, tick, level or grid neighbour at or
 *   beyond 2^53 units, a level below the first band, or a close rounded to zero.
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
  if (!rule.grid_known()) {
    throw refuse("the price grid is unknown");
  }
  std::uint8_t scale = 0;
  // A value in grid units, refused at or beyond 2^53, where a double would no longer hold every integer.
  constexpr std::int64_t limit = std::int64_t{1} << 53;
  const auto units = [&](const Decimal64& value) {
    std::int64_t out = value.coefficient();
    for (auto step = value.scale();; ++step) {
      if (out >= limit || out <= -limit) {
        throw refuse("a value at or beyond 2^53 grid units");
      }
      if (step >= scale) {
        return out;
      }
      out *= 10;
    }
  };
  // The bands in units, checked aligned, at the bands' own scale and again at the closes'; one unit unconstrained.
  std::vector<std::pair<std::int64_t, std::int64_t>> grid;
  const auto grid_in_units = [&]() {
    grid.clear();
    if (rule.kind_ == PriceRule::Kind::UNCONSTRAINED) {
      grid.emplace_back(0, 1);
      return;
    }
    for (const auto& band : rule.bands_) {
      scale = std::max({scale, band.from_.scale(), band.tick_.scale()});
    }
    for (const auto& band : rule.bands_) {
      const auto start = units(band.from_);
      const auto tick = units(band.tick_);
      if (tick <= 0 || start % tick != 0 || (!grid.empty() && start % grid.back().second != 0)) {
        throw refuse("a band's start is not a multiple of its tick and of the previous band's");
      }
      grid.emplace_back(start, tick);
    }
  };
  grid_in_units();
  if (identity) {
    return closes;
  }
  for (const auto& close : closes) {
    scale = std::max(scale, close.scale());
  }
  grid_in_units();
  std::vector<double> in;
  for (const auto& close : closes) {
    in.push_back(static_cast<double>(units(close)));
  }
  std::vector<Decimal64> out;
  double level = in.front();
  for (std::size_t k = 0; k <= indices.size(); ++k) {
    if (k > 0) {
      const double ratio = in[indices[k - 1] + 1] / in[indices[k - 1]];
      level = level * ratio;
    }
    std::size_t band = 0;
    while (band + 1 < grid.size() && static_cast<double>(grid[band + 1].first) <= level) {
      ++band;
    }
    if (!(level < static_cast<double>(limit)) || grid.empty() || static_cast<double>(grid[band].first) > level) {
      throw refuse("a level at or beyond 2^53 grid units, or below the first band");
    }
    const auto tick = grid[band].second;
    auto lower = static_cast<std::int64_t>(std::floor(level / static_cast<double>(tick)));
    for (;;) {
      const std::int64_t lo = lower * tick;
      const std::int64_t hi = lo + tick;
      if (hi >= limit) {
        throw refuse("a grid neighbour at or beyond 2^53 grid units");
      }
      if (static_cast<double>(lo) > level) {
        --lower;
      } else if (static_cast<double>(hi) <= level) {
        ++lower;
      } else {
        const auto nearest = level - static_cast<double>(lo) < static_cast<double>(hi) - level ? lo : hi;
        if (nearest == 0) {
          throw refuse("a close rounded to zero");
        }
        out.emplace_back(nearest, scale);
        break;
      }
    }
  }
  return out;
}

} // namespace backtest
} // namespace algotradelib
