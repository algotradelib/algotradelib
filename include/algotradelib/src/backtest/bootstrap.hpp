#pragma once

#include <algotradelib/core>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/** @brief A price grid in whole units of `10^-scale`, and the rounding of a path's level onto it. */
class Grid {
public:
  /** @brief A value in grid units is refused at or beyond 2^53, where a double would no longer hold every integer. */
  static constexpr std::int64_t LIMIT = std::int64_t{1} << 53;

  /**
   * @brief Reads the rule's bands at their own scale.
   * @param rule The price grid; it must outlive the grid.
   * @param any_sign Whether levels at or below zero are rounded rather than refused.
   * @param who The caller's name, put before each refusal.
   * @throws std::invalid_argument for an `UNKNOWN` or unaligned rule, or a band or tick at or beyond 2^53 units.
   */
  Grid(const PriceRule& rule, bool any_sign, const char* who) : rule_(rule), any_sign_(any_sign), who_(who) {
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
   * @brief Raises the scale to cover the values', and reads the bands again at it.
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
   * @throws std::invalid_argument for a level or neighbour at or beyond 2^53 units in magnitude, a level below the
   *   first band (stated, or unconstrained without `any_sign`), or a price rounded to zero without `any_sign`.
   */
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
      throw refuse("a level at or beyond 2^53 grid units, or below the first band");
    }
    const auto tick = grid_[band].second;
    auto lower = static_cast<std::int64_t>(std::floor(level / static_cast<double>(tick)));
    for (;;) {
      const std::int64_t lo = lower * tick;
      const std::int64_t hi = lo + tick;
      if (hi >= LIMIT || lo <= -LIMIT) {
        throw refuse("a grid neighbour at or beyond 2^53 grid units");
      }
      if (static_cast<double>(lo) > level) {
        --lower;
      } else if (static_cast<double>(hi) <= level) {
        ++lower;
      } else {
        const auto nearest = level - static_cast<double>(lo) < static_cast<double>(hi) - level ? lo : hi;
        if (nearest == 0 && !any_sign_) {
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
  bool any_sign_;
  const char* who_;
  std::uint8_t scale_{0};
  std::vector<std::pair<std::int64_t, std::int64_t>> grid_;
};

/** @brief Whether a join declares `static constexpr bool any_sign = true`; `false` when it declares nothing. */
template <typename Join, typename = void>
struct has_any_sign : std::false_type {};

/** @brief A join that declares `any_sign`. */
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
  double join(double level, double from, double value) const {
    static_cast<void>(level);
    static_cast<void>(from);
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
 * in grid units (`10^-s`, `s` the largest scale among the closes and the rule's band starts and ticks), exact below
 * 2^53 units, so the result is the same on every platform.
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
  if constexpr (!any_sign) {
    for (const auto& close : closes) {
      if (close.coefficient() <= 0) {
        throw refuse("a close at or below zero");
      }
    }
  }
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
 * timestamp and the drawn day's volume; an empty open stays empty, and `traded_value_` is empty.
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
  const auto refuse = [](const char* why) {
    return std::invalid_argument(std::string("resample_bars: ") + why);
  };
  if (bars.size() < 2 || indices.size() > bars.size() - 1) {
    throw refuse("needs two bars or more, and at most one index per return");
  }
  bool identity = indices.size() == bars.size() - 1;
  for (std::size_t k = 0; k < indices.size(); ++k) {
    identity = identity && indices[k] == k;
    if (indices[k] >= bars.size() - 1) {
      throw refuse("an index names no return");
    }
  }
  std::vector<Decimal64> prices;
  for (const auto& bar : bars) {
    if (bar.open_) {
      prices.push_back(*bar.open_);
    }
    prices.insert(prices.end(), {bar.high_, bar.low_, bar.close_});
  }
  if constexpr (!any_sign) {
    for (const auto& price : prices) {
      if (price.coefficient() <= 0) {
        throw refuse("a price at or below zero");
      }
    }
  }
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
  const char* zero = "a price rounded to zero";
  std::vector<Bar> out;
  out.reserve(indices.size() + 1);
  out.push_back(bars.front());
  if (out.front().open_) {
    out.front().open_ = grid.round(units(*out.front().open_), zero);
  }
  out.front().high_ = grid.round(units(out.front().high_), zero);
  out.front().low_ = grid.round(units(out.front().low_), zero);
  double level = units(out.front().close_);
  out.front().close_ = grid.round(level, zero);
  for (std::size_t k = 1; k <= indices.size(); ++k) {
    const auto& drawn = bars[indices[k - 1] + 1];
    const double from = units(bars[indices[k - 1]].close_);
    const auto price = [&](const Decimal64& value) {
      return grid.round(join.join(level, from, units(value)), zero);
    };
    Bar bar;
    bar.timestamp_ = bars[k].timestamp_;
    if (drawn.open_) {
      bar.open_ = price(*drawn.open_);
    }
    bar.high_ = price(drawn.high_);
    bar.low_ = price(drawn.low_);
    level = join.join(level, from, units(drawn.close_));
    bar.close_ = grid.round(level, zero);
    bar.volume_ = drawn.volume_;
    out.push_back(bar);
  }
  return out;
}

} // namespace backtest
} // namespace algotradelib
