#pragma once

#include "./signal.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <string>
#include <utility>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * Holds when a fast moving average is above a slow one, and is flat otherwise.
 *
 * Stated as a *level* rather than as a pair of crossing events, which is the more robust of the two
 * formulations and the one this library's target positions make natural. A crossing-driven version
 * has to remember whether it is currently long, and after a restart it does not know — so it either
 * misses the exit or re-enters a position it already holds. Asking for "long while above, flat while
 * below" is the same strategy with no state to lose.
 *
 * `AverageKind` selects the average rather than there being two near-identical strategies. The two
 * differ only in a decay convention, which `analytics` already documents.
 */
enum class AverageKind {
  /** A simple mean of the window. */
  SIMPLE,
  /** An exponential average with `alpha = 2 / (span + 1)`. */
  EXPONENTIAL,
};

/** Holds while a fast average leads a slow one. */
struct MovingAverageCrossover {
  struct Params {
    InstrumentId instrument_;
    std::size_t fast_{20};
    std::size_t slow_{50};
    AverageKind kind_{AverageKind::SIMPLE};
    /** How many shares to hold while the signal is on. */
    Decimal64 quantity_;
    std::string tag_{"ma-crossover"};
  };

  Params params_;

  /**
   * Bars needed before the strategy will act.
   *
   * The slow window plus one, because a level comparison still reads two values so that the same
   * code answers "is it above" and "did it just cross".
   */
  std::size_t warmup_bars() const { return params_.slow_ + 1; }

  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.fast_ == 0 || params_.slow_ <= params_.fast_) {
      // A fast window that is not faster inverts the signal. Refusing to act is better than
      // trading the opposite of what was configured, and a runner's log will show it doing nothing.
      return;
    }
    const auto* series = ctx.bars(bar.instrument_, bar.interval_);
    if (series == nullptr) {
      return;
    }
    const auto closes = series->column(analytics::BarField::CLOSE);
    const auto window = trailing(closes, params_.slow_);
    if (!window) {
      return;
    }

    const auto fast = line(*window, params_.fast_);
    const auto slow = line(*window, params_.slow_);
    const auto fast_pair = last_two(fast);
    const auto slow_pair = last_two(slow);
    if (!fast_pair || !slow_pair) {
      return;
    }

    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.target_quantity_ = slow_pair->current_ < fast_pair->current_ ?
      params_.quantity_ :
      Decimal64::parse("0");
    target.tag_ = params_.tag_;
    ctx.submit(target);
  }

private:
  /** Returns the chosen average over `window`. */
  math::Vector line(math::SeriesView window, std::size_t span) const {
    if (params_.kind_ == AverageKind::EXPONENTIAL) {
      return analytics::ema(window, span, span);
    }
    return analytics::sma(window, span);
  }
};

/** A simple-average crossover, the conventional spelling. */
struct SmaCrossover {
  MovingAverageCrossover inner_;

  explicit SmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::SIMPLE;
    inner_.params_ = std::move(params);
  }

  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

/** An exponential-average crossover. */
struct EmaCrossover {
  MovingAverageCrossover inner_;

  explicit EmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::EXPONENTIAL;
    inner_.params_ = std::move(params);
  }

  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
