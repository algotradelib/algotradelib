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
 * Buys when the price is unusually low against its own recent history, and exits at the mean.
 *
 * Three thresholds rather than two, and the third is what stops it thrashing. Entering at `-entry_`
 * and exiting at zero would re-enter the moment the score dipped again; exiting at `exit_`, a level
 * nearer the mean than the entry, gives the position somewhere to go. Mean reversion that enters and
 * exits on the same line trades constantly and pays the spread every time.
 *
 * Long only. A short leg needs borrow availability, a different margin model, and a hard stop —
 * none of which this library models yet, and a short mean-reversion position without a stop is the
 * classic way to lose more than the account holds.
 */
struct ZScoreReversion {
  struct Params {
    InstrumentId instrument_;
    /** Window the score is measured over. */
    std::size_t window_{20};
    /** How far below the mean, in deviations, to buy. Positive. */
    math::Scalar entry_{2.0};
    /** How far below the mean to hold down to before flattening. Positive, below `entry_`. */
    math::Scalar exit_{0.5};
    Decimal64 quantity_;
    std::string tag_{"zscore-reversion"};
  };

  Params params_;

  std::size_t warmup_bars() const { return params_.window_ + 1; }

  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.window_ < 2 || !(params_.exit_ < params_.entry_)) {
      // An exit at or beyond the entry has no band to hold in, so the position would open and close
      // on the same bar forever. Doing nothing is the honest response to a contradictory setting.
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

    const auto held = ctx.portfolio().quantity_of(params_.instrument_);
    const bool holding = held && held->coefficient() != 0;

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
 * Buys an oversold instrument and sells an overbought one, on Wilder's index.
 *
 * `30` and `70` are Wilder's own thresholds and the ones every chart draws. The index itself uses
 * his smoothing and his seed, which `analytics::rsi` documents — an index smoothed the other
 * conventional way produces different numbers from the chart a threshold was chosen against.
 *
 * Long only, for the reason `ZScoreReversion` gives.
 */
struct RsiThreshold {
  struct Params {
    InstrumentId instrument_;
    std::size_t period_{14};
    /** Buy at or below this level. */
    math::Scalar oversold_{30.0};
    /** Flatten at or above this level. */
    math::Scalar overbought_{70.0};
    Decimal64 quantity_;
    std::string tag_{"rsi-threshold"};
  };

  Params params_;

  /**
   * Bars needed before the index reports.
   *
   * Row zero has no price change at all and the smoothing needs `period_` changes before it has a
   * seed, so the first value appears on row `period_` — hence one more than the period.
   */
  std::size_t warmup_bars() const { return params_.period_ + 1; }

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
    const auto window = trailing(closes, params_.period_);
    if (!window) {
      return;
    }
    const auto index = analytics::rsi(*window, params_.period_);
    const auto level = index[index.size() - 1];
    if (!math::observed(level)) {
      return;
    }

    const auto held = ctx.portfolio().quantity_of(params_.instrument_);
    const bool holding = held && held->coefficient() != 0;

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
 * Holds while the price is at a new high of its lookback, and exits at a new low.
 *
 * Donchian's breakout, and it reads the extremes of the *prior* window rather than one including
 * today. A channel that included the current bar could never be broken: today's high is always
 * within a range that contains today's high, so the signal would never fire. Getting that wrong
 * produces a strategy that silently does nothing, which is why the window is sliced to exclude the
 * latest row.
 */
struct DonchianBreakout {
  struct Params {
    InstrumentId instrument_;
    std::size_t lookback_{20};
    Decimal64 quantity_;
    std::string tag_{"donchian-breakout"};
  };

  Params params_;

  std::size_t warmup_bars() const { return params_.lookback_ + 1; }

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
