#pragma once

#include "./signal.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <string>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * @brief Long-only mean reversion on the z-score of the close.
 *
 * Buys `quantity_` when flat and the z-score over `window_` closes is at or below `-entry_`; goes
 * flat when holding and the z-score has risen to `-exit_` or above. Exiting before the mean
 * (`exit_ > 0`) keeps it from re-entering on the next small dip.
 */
struct ZScoreReversion {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to trade; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The number of closes the z-score is measured over; below 2, nothing is traded. */
    std::size_t window_{20};
    /** @brief How many standard deviations below the mean to buy at; positive. */
    math::Scalar entry_{2.0};
    /** @brief The z-score `-exit_` at which to sell; positive and less than `entry_`, or nothing is traded. */
    math::Scalar exit_{0.5};
    /** @brief The shares to hold while in a position. */
    Decimal64 quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"zscore-reversion"};
  };

  /** @brief The settings. */
  Params params_;

  /**
   * @brief Returns how many bars to load before the first live bar.
   * @return `window_ + 1`.
   */
  std::size_t warmup_bars() const { return params_.window_ + 1; }

  /**
   * @brief States a target position for the instrument when its signal enters or exits.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.window_ < 2 || !(params_.exit_ < params_.entry_)) {
      // An exit at or beyond the entry is contradictory; do nothing.
      return;
    }
    const auto* series = ctx.bars(bar.instrument_, bar.interval_);
    if (series == nullptr) {
      return;
    }
    const auto closes = series->column(analytics::BarField::CLOSE);
    const auto window = trailing(closes, params_.window_);
    if (!window) {
      return;
    }
    const auto scores = analytics::zscore(*window, params_.window_);
    const auto score = scores[scores.size() - 1];
    // Unobserved covers both the warm-up and a window with no dispersion, where a score does not
    // exist. Either way there is nothing to act on.
    if (!math::observed(score)) {
      return;
    }

    // Net across every position row: a spent lot of 0 beside a live one is not flat.
    const auto held = ctx.portfolio().net_quantity_of(params_.instrument_);
    if (!held) {
      // Rows that do not add up say nothing about what is held; reading them as flat would enter.
      return;
    }
    const bool holding = held->coefficient() != 0;

    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.tag_ = params_.tag_;
    if (!holding && score <= -params_.entry_) {
      target.target_quantity_ = params_.quantity_;
    } else if (holding && -params_.exit_ <= score) {
      target.target_quantity_ = Decimal64::parse("0");
    } else {
      return;
    }
    ctx.submit(target);
  }
};

/**
 * @brief Long-only RSI strategy: buys when the index is oversold, sells when it is overbought.
 *
 * Buys `quantity_` when flat and Wilder's RSI (`analytics::rsi`) is at or below `oversold_`; goes
 * flat when holding and it is at or above `overbought_`. The defaults 30 and 70 are Wilder's.
 *
 * The index is computed over every cached close (up to the runner's `series_capacity`), because
 * Wilder's smoothing depends on its seed. With about 99 closes the seed's weight falls below 0.2% for
 * period 14.
 *
 * @note A backtest, whose cache keeps growing, can differ from a live run that loaded fewer bars,
 *   until the live cache catches up. Once the cache is full, evicting old closes can still move the
 *   index slightly.
 */
struct RsiThreshold {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to trade; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The RSI period; zero disables the strategy. */
    std::size_t period_{14};
    /** @brief Buy at or below this level. */
    math::Scalar oversold_{30.0};
    /** @brief Sell at or above this level; must exceed `oversold_`, or nothing is traded. */
    math::Scalar overbought_{70.0};
    /** @brief The shares to hold while in a position. */
    Decimal64 quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"rsi-threshold"};
  };

  /** @brief The settings. */
  Params params_;

  /**
   * @brief Returns how many bars to load before the first live bar.
   * @return `period_ + 1`, the first row at which the index exists.
   */
  std::size_t warmup_bars() const { return params_.period_ + 1; }

  /**
   * @brief States a target position for the instrument when its signal enters or exits.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.period_ == 0 || !(params_.oversold_ < params_.overbought_)) {
      return;
    }
    const auto* series = ctx.bars(bar.instrument_, bar.interval_);
    if (series == nullptr) {
      return;
    }
    const auto closes = series->column(analytics::BarField::CLOSE);
    if (static_cast<std::size_t>(closes.size()) < warmup_bars()) {
      return;
    }
    // The whole cache, not a slice: Wilder's smoothing depends on its seed.
    const auto index = analytics::rsi(closes, params_.period_);
    const auto level = index[index.size() - 1];
    if (!math::observed(level)) {
      return;
    }

    // Net across every position row: a spent lot of 0 beside a live one is not flat.
    const auto held = ctx.portfolio().net_quantity_of(params_.instrument_);
    if (!held) {
      // Rows that do not add up say nothing about what is held; reading them as flat would enter.
      return;
    }
    const bool holding = held->coefficient() != 0;

    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.tag_ = params_.tag_;
    if (!holding && level <= params_.oversold_) {
      target.target_quantity_ = params_.quantity_;
    } else if (holding && params_.overbought_ <= level) {
      target.target_quantity_ = Decimal64::parse("0");
    } else {
      return;
    }
    ctx.submit(target);
  }
};

/**
 * @brief Donchian channel breakout: holds after the close breaks above the channel, exits after it
 *   breaks below.
 *
 * The channel is the highest and lowest close of the `lookback_` bars **before** the latest one.
 * A close above the channel states a target of `quantity_`; a close below it, a target of zero.
 */
struct DonchianBreakout {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to trade; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The number of prior closes the channel spans; zero disables the strategy. */
    std::size_t lookback_{20};
    /** @brief The shares to hold while in a position. */
    Decimal64 quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"donchian-breakout"};
  };

  /** @brief The settings. */
  Params params_;

  /**
   * @brief Returns how many bars to load before the first live bar.
   * @return `lookback_ + 1`.
   */
  std::size_t warmup_bars() const { return params_.lookback_ + 1; }

  /**
   * @brief States a target position for the instrument when its signal enters or exits.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.lookback_ == 0) {
      return;
    }
    const auto* series = ctx.bars(bar.instrument_, bar.interval_);
    if (series == nullptr) {
      return;
    }
    const auto closes = series->column(analytics::BarField::CLOSE);
    const auto window = trailing(closes, params_.lookback_);
    if (!window) {
      return;
    }

    // The channel excludes the latest row, so a break is measurable at all.
    const auto prior = window->head(window->size() - 1);
    const auto highest = analytics::rolling_max(prior, params_.lookback_);
    const auto lowest = analytics::rolling_min(prior, params_.lookback_);
    const auto ceiling = highest[highest.size() - 1];
    const auto floor = lowest[lowest.size() - 1];
    const auto close = (*window)[window->size() - 1];
    if (!math::observed(ceiling) || !math::observed(floor) || !math::observed(close)) {
      return;
    }

    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.tag_ = params_.tag_;
    if (ceiling < close) {
      target.target_quantity_ = params_.quantity_;
    } else if (close < floor) {
      target.target_quantity_ = Decimal64::parse("0");
    } else {
      return;
    }
    ctx.submit(target);
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
