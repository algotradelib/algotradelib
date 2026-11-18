#pragma once

#include <algotradelib/core>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief The prices of a bootstrap path, rebuilt from drawn blocks joined to the path and put on the instrument's
 *   price grid.
 */

namespace bootstrap_detail {

// A refusal naming the function.
inline std::invalid_argument refusal(const char* who, const char* why) {
  return std::invalid_argument(std::string(who) + ": " + why);
}

// The checks before the identity draw: two prices or more (`few`), the indices, and without `any_sign` every price
// above zero (`sign`). Returns whether `indices` is the identity draw.
inline bool identity_draw(
  std::size_t count, const std::vector<std::size_t>& indices, const std::vector<Decimal64>& prices, bool any_sign,
  const char* who, const char* few, const char* sign
) {
  if (count < 2 || indices.size() > count - 1) {
    throw refusal(who, few);
  }
  bool identity = indices.size() == count - 1;
  for (std::size_t k = 0; k < indices.size(); ++k) {
    identity = identity && indices[k] == k;
    if (indices[k] >= count - 1) {
      throw refusal(who, "an index names no return");
    }
  }
  for (const auto& price : prices) {
    if (!any_sign && price.coefficient() <= 0) {
      throw refusal(who, sign);
    }
  }
  return identity;
}

// A price grid in whole units of `10^-scale_`, checked aligned, and the rounding of a path's level onto it.
class Grid {
public:
  // A value in grid units is refused at or beyond 2^53, where a double would no longer hold every integer.
  static constexpr std::int64_t LIMIT = std::int64_t{1} << 53;

  // The bands at their own scale; `rule` must outlive the grid.
  Grid(const PriceRule& rule, bool any_sign, const char* who) : rule_(rule), any_sign_(any_sign), who_(who) {
    if (!rule.grid_known()) {
      throw refusal(who_, "the price grid is unknown");
    }
    build();
  }

  // The bands again, at a scale covering the values'.
  void widen(const std::vector<Decimal64>& values) {
    for (const auto& value : values) {
      scale_ = std::max(scale_, value.scale());
    }
    build();
  }

  std::int64_t units(const Decimal64& value) const {
    std::int64_t out = value.coefficient();
    for (auto step = value.scale();; ++step) {
      if (out >= LIMIT || out <= -LIMIT) {
        throw refusal(who_, "a value at or beyond 2^53 grid units");
      }
      if (step >= scale_) {
        return out;
      }
      out *= 10;
    }
  }

  // The nearest grid price, a tie going up; below the first band only unconstrained with `any_sign`.
  Decimal64 round(double level, const char* zero) const {
    constexpr auto limit = static_cast<double>(LIMIT);
    std::size_t band = 0;
    while (band + 1 < grid_.size() && static_cast<double>(grid_[band + 1].first) <= level) {
      ++band;
    }
    const bool floored = !any_sign_ || rule_.kind_ != PriceRule::Kind::UNCONSTRAINED;
    if (
      !(level < limit) || !(level > -limit) || grid_.empty() ||
      (floored && static_cast<double>(grid_[band].first) > level)
    ) {
      throw refusal(who_, "a level at or beyond 2^53 grid units, or below the first band");
    }
    const auto tick = grid_[band].second;
    auto lower = static_cast<std::int64_t>(std::floor(level / static_cast<double>(tick)));
    for (;;) {
      const std::int64_t lo = lower * tick;
      const std::int64_t hi = lo + tick;
      if (hi >= LIMIT || lo <= -LIMIT) {
        throw refusal(who_, "a grid neighbour at or beyond 2^53 grid units");
      }
      if (static_cast<double>(lo) > level) {
        --lower;
      } else if (static_cast<double>(hi) <= level) {
        ++lower;
      } else {
        const auto nearest = level - static_cast<double>(lo) < static_cast<double>(hi) - level ? lo : hi;
        if (nearest == 0 && !any_sign_) {
          throw refusal(who_, zero);
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
        throw refusal(who_, "a band's start is not a multiple of its tick and of the previous band's");
      }
      grid_.emplace_back(start, tick);
    }
  }

  const PriceRule& rule_;
  bool any_sign_;
  const char* who_;
  std::uint8_t scale_{0};
  std::vector<std::pair<std::int64_t, std::int64_t>> grid_;
};

// Whether a join declares `static constexpr bool any_sign = true`; `false` when it declares nothing.
template <typename Join, typename = void>
struct has_any_sign : std::false_type {};

template <typename Join>
struct has_any_sign<Join, std::void_t<decltype(Join::any_sign)>> : std::bool_constant<Join::any_sign> {};

} // namespace bootstrap_detail

/** @brief Chains the drawn returns: a path price is the path's previous close times `value / from`. */
struct Returns {
  /**
   * @brief Returns the path's price for a drawn price.
   * @param level The path's previous close, unrounded, in grid units.
   * @param from The drawn day's previous close in the history, in grid units; positive.
   * @param value The drawn day's price (open, high, low or close), in grid units; positive.
   * @return `level * (value / from)`, the ratio first.
   */
  double join(double level, double from, double value) const {
    return level * (value / from);
  }
};

/** @brief Chains the drawn changes (Paparoditis and Politis): the path's previous close plus `value - from`. */
struct Differences {
  /** @brief Prices at or below zero are joined, not refused. */
  static constexpr bool any_sign = true;

  /**
   * @brief Returns the path's price for a drawn price.
   * @param level The path's previous close, in grid units.
   * @param from The drawn day's previous close in the history, in grid units.
   * @param value The drawn day's price, in grid units.
   * @return `level + (value - from)`.
   * @throws std::invalid_argument if `value - from` is at or beyond 2^53 units in magnitude.
   */
  double join(double level, double from, double value) const {
    constexpr auto limit = static_cast<double>(bootstrap_detail::Grid::LIMIT);
    const double step = value - from;
    if (!(step < limit && step > -limit)) {
      throw std::invalid_argument("Differences: a step at or beyond 2^53 grid units");
    }
    return level + step;
  }
};

/**
 * @brief Joins the drawn prices as they are (Künsch; Politis and Romano): for a stationary series, not trending
 *   prices, where each block boundary is a jump.
 */
struct Levels {
  /** @brief Prices at or below zero are joined, not refused. */
  static constexpr bool any_sign = true;

  /**
   * @brief Returns the drawn price.
   * @param level Unused.
   * @param from Unused.
   * @param value The drawn day's price, in grid units.
   * @return `value`.
   */
  double join([[maybe_unused]] double level, [[maybe_unused]] double from, double value) const {
    return value;
  }
};

/**
 * @brief Whether `Join` can join drawn blocks: `true` when a `const Join` has a member
 *   `join(double, double, double)` returning something convertible to `double`.
 * @tparam Join The join type.
 */
template <typename Join, typename = void>
struct is_price_join : std::false_type {};

/** @brief A join with the member `join` that `resample_closes` and `resample_bars` call. */
template <typename Join>
struct is_price_join<
  Join,
  std::enable_if_t<
    std::is_convertible_v<decltype(std::declval<const Join&>().join(double{}, double{}, double{})), double>
  >
> : std::true_type {};

/**
 * @brief Rebuilds one instrument's closes along a bootstrap path (`analytics::bootstrap_indices`), joining the drawn
 *   blocks by `Join`, and rounds each to its nearest grid price, a tie going up.
 *
 * Call it with the same `indices` for every instrument of a path, so they move on the same drawn days. The identity
 * draw (`0, 1, ..., n - 2`) returns `closes` as they are. Otherwise close 0 is the first close, and close `k` is
 * `join.join(level, closes[j], closes[j + 1])` with `j = indices[k - 1]` and `level` close `k - 1`, accumulated
 * unrounded and rounded once each; with `Returns`, close `k - 1` times `closes[j + 1] / closes[j]`. The arithmetic is
 * in grid units (`10^-s`, `s` the largest scale among the closes and the rule's band starts and ticks): converting a
 * price to units is exact below 2^53, and `Differences` and `Levels` stay in whole units. `Returns` divides and
 * multiplies in rounded binary64 doubles in a fixed order, with no multiply-add to fuse, so its result is the same
 * wherever doubles are IEEE binary64 rounding to nearest (not x87 extended precision).
 *
 * `Levels` keeps close 0 and draws closes 1 to `n - 1`, so a circular block wraps from close `n - 1` to close 1. With
 * `Differences` or `Levels` a close can be at or below zero; whether a replay may trade at it is the caller's choice.
 *
 * @tparam Join `Returns`, `Differences`, `Levels`, or one of your own with the member
 *   `double join(double level, double from, double value) const`. A join without
 *   `static constexpr bool any_sign = true` takes and returns positive closes only.
 * @param closes The closes, in order.
 * @param rule The price grid: `UNCONSTRAINED` rounds to one unit, from zero (at every sign with `any_sign`); a stated
 *   grid must be aligned (each band's start a multiple of its own tick and of the previous band's).
 * @param indices The drawn returns, each below `closes.size() - 1`.
 * @param join The join.
 * @return `indices.size() + 1` closes.
 * @throws std::invalid_argument for fewer than two closes, too many indices or one out of range, a close at or below
 *   zero (without `any_sign`), an `UNKNOWN` or unaligned rule, or a band or tick at or beyond 2^53 units at the rule's
 *   own scale (the identity draw included); for a drawn path, a close, band, tick, level or grid neighbour at or
 *   beyond 2^53 units in magnitude, a level below a stated grid's first band (or below zero without `any_sign`), a
 *   close rounded to zero (without `any_sign`), or what `join` throws.
 */
template <typename Join = Returns>
std::vector<Decimal64> resample_closes(
  const std::vector<Decimal64>& closes, const PriceRule& rule, const std::vector<std::size_t>& indices,
  const Join& join = Join{}
) {
  static_assert(
    is_price_join<Join>::value,
    "a bootstrap join needs the member double join(double level, double from, double value) const"
  );
  constexpr bool any_sign = bootstrap_detail::has_any_sign<Join>::value;
  const bool identity = bootstrap_detail::identity_draw(
    closes.size(), indices, closes, any_sign, "resample_closes",
    "needs two closes or more, and at most one index per return", "a close at or below zero"
  );
  // The bands checked aligned at their own scale, then at the closes'.
  bootstrap_detail::Grid grid(rule, any_sign, "resample_closes");
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
      level = join.join(level, in[indices[k - 1]], in[indices[k - 1] + 1]);
    }
    out.push_back(grid.round(level, "a close rounded to zero"));
  }
  return out;
}

/**
 * @brief Rebuilds one instrument's bars along a bootstrap path (`analytics::bootstrap_indices`): each drawn day's
 *   prices joined to the path's previous close and rounded to their nearest grid prices, a tie going up.
 *
 * Call it with the same `indices` for every instrument of a path. The identity draw (`0, 1, ..., n - 2`) returns
 * `bars` as they are. Otherwise bar 0 is the first bar with its prices rounded. Bar `k` draws day `j + 1` with
 * `j = indices[k - 1]`: each of its prices `value` (open, high, low, close) becomes
 * `join.join(level, bars[j].close_, value)`, `level` the path's close `k - 1` unrounded, so the closes are those of
 * `resample_closes` when the scale is the same (here it is read over every price). Bar `k` takes `bars[k]`'s
 * timestamp and the drawn day's volume; an empty open stays empty. Bar 0 keeps its own `traded_value_`; a drawn bar
 * (`k >= 1`) has none.
 *
 * An ordered bar (low at most open and close, high at least both) stays ordered, since every built-in join and the
 * rounding are non-decreasing in the price; the input's order is not checked. With `Differences` or `Levels` a price
 * can be at or below zero; whether a replay may trade at it is the caller's choice.
 *
 * @tparam Join `Returns`, `Differences`, `Levels`, or one of your own with the member
 *   `double join(double level, double from, double value) const`, non-decreasing in `value`. A join without
 *   `static constexpr bool any_sign = true` takes and returns positive prices only.
 * @param bars The bars, in order, one per instant.
 * @param rule The price grid, as `resample_closes`.
 * @param indices The drawn returns, each below `bars.size() - 1`.
 * @param join The join.
 * @return `indices.size() + 1` bars, bar `k` stamped with `bars[k].timestamp_`.
 * @throws std::invalid_argument for what `resample_closes` refuses, read over every price of every bar (an empty
 *   open is skipped).
 */
template <typename Join = Returns>
std::vector<Bar> resample_bars(
  const std::vector<Bar>& bars, const PriceRule& rule, const std::vector<std::size_t>& indices,
  const Join& join = Join{}
) {
  static_assert(
    is_price_join<Join>::value,
    "a bootstrap join needs the member double join(double level, double from, double value) const"
  );
  constexpr bool any_sign = bootstrap_detail::has_any_sign<Join>::value;
  std::vector<Decimal64> prices;
  for (const auto& bar : bars) {
    if (bar.open_) {
      prices.push_back(*bar.open_);
    }
    prices.insert(prices.end(), {bar.high_, bar.low_, bar.close_});
  }
  const bool identity = bootstrap_detail::identity_draw(
    bars.size(), indices, prices, any_sign, "resample_bars", "needs two bars or more, and at most one index per return",
    "a price at or below zero"
  );
  bootstrap_detail::Grid grid(rule, any_sign, "resample_bars");
  if (identity) {
    return bars;
  }
  grid.widen(prices);
  // Every price in bounds, drawn or not, as `resample_closes` reads every close.
  for (const auto& price : prices) {
    grid.units(price);
  }
  const auto units = [&grid](const Decimal64& value) {
    return static_cast<double>(grid.units(value));
  };
  std::vector<Bar> out;
  double level = units(bars.front().close_);
  for (std::size_t k = 0; k <= indices.size(); ++k) {
    // Bar 0 is the first bar on the grid; bar `k` is day `j + 1` joined from close `j`.
    const auto& drawn = k == 0 ? bars.front() : bars[indices[k - 1] + 1];
    const double from = k == 0 ? 0.0 : units(bars[indices[k - 1]].close_);
    const auto price = [&](const Decimal64& value) {
      return grid.round(k == 0 ? units(value) : join.join(level, from, units(value)), "a price rounded to zero");
    };
    Bar bar = drawn;
    bar.timestamp_ = bars[k].timestamp_;
    if (drawn.open_) {
      bar.open_ = price(*drawn.open_);
    }
    bar.high_ = price(drawn.high_);
    bar.low_ = price(drawn.low_);
    bar.close_ = price(drawn.close_);
    if (k > 0) {
      level = join.join(level, from, units(drawn.close_));
      bar.traded_value_.reset();
    }
    out.push_back(bar);
  }
  return out;
}

} // namespace backtest
} // namespace algotradelib
