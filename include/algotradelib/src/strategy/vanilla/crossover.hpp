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
 * `AverageKind` selects the average rather than there being two near-identical strategies.
 *
 * **The two kinds read different amounts of history.** A simple average is defined on its window,
 * so the simple kind reads the last `slow + 1` closes. An exponential average is a recursion: its
 * value depends on every bar since it was seeded, `ema_t = a * sum_{k<N} (1-a)^k y_{t-k} +
 * (1-a)^N ema_{t-N}`, and an average recomputed over only the last `N + 1` bars puts the weight
 * `(1-a)^N` -- 13.5% after one span -- on a single raw close instead of on the history before it. So
 * the exponential kind reads every bar the runner has cached (up to `series_capacity`):
 *
 * - **At least three spans of observed closes cached:** the plain average seeded at the oldest cached
 *   close, which then carries `(1-a)^{k-1}` of the line after `k` closes: about `e^-6`, 0.25%, at three
 *   spans. That bounds the seed's weight on complete data, not the price error, and a chart that seeds
 *   differently converges to the same line only as its own seed decays. On 005930 daily bars, 20/50,
 *   three spans left 3 bars in 1,829 whose crossing state differed from the full-history averages, each
 *   within 0.06% of a crossing.
 * - **Fewer:** the compensated average (`adjusted_ema`, pandas' `adjust=True`): the observed rows'
 *   exponential weights, normalized. For a new listing there is no earlier history to stand in for,
 *   and a plain seed would put 13.5% of the line, after one span, on the listing day's often extreme
 *   first print; compensation gives that bar its ordinary weight, about 0.6%. The cost: when the
 *   instrument does have history and the broker served only a little of it, the plain seed is the
 *   closer estimate (about 0.09% against 0.29% mean error with 100 bars for span 50, 005930), and this
 *   rule cannot tell the two cases apart. A vanilla strategy has to run on both.
 *
 * The rule is per line: with 100 cached bars and 20/50, the fast line is plain and the slow line
 * compensated. With `S` the plain line, `y0` the first close and `d = (1-a)^k` after `k` closes, the
 * compensated line is `S + d (S - y0) / (1 - d)`: it leans away from the first close rather than
 * averaging out, so one line switching while the other does not can change the signal on an unchanged
 * close. Spans 3/5 over `95, 100, 105, 100, 100, 95, 105, 100` are long, and one more close of `100`
 * makes them flat, because the fast line turned plain at nine closes. A switch can also move a line by
 * the difference of the two estimates (about 0.04% of price at three spans on 005930). Spans above a
 * third of `series_capacity` (170 at the default 512) are always compensated. And a replay, whose cache keeps
 * growing, can differ from a live start that read fewer bars, until the live cache catches up.
 */
enum class AverageKind {
  /** A simple mean of the window. */
  SIMPLE,
  /** An exponential average with `alpha = 2 / (span + 1)`, over every cached bar. */
  EXPONENTIAL,
};

/** Holds while a fast average leads a slow one. */
struct MovingAverageCrossover {
  /** How this strategy is configured. */
  struct Params {
    /** Which instrument to trade. Bars for anything else are ignored. */
    InstrumentId instrument_;
    /** The leading window. Must be non-zero and shorter than `slow_`, or the strategy does nothing. */
    std::size_t fast_{20};
    /** The lagging window. */
    std::size_t slow_{50};
    /** Which average to use, and so how much history each line reads; see the class comment. */
    AverageKind kind_{AverageKind::SIMPLE};
    /** How many shares to hold while the signal is on. */
    Decimal64 quantity_;
    /** Label carried onto the orders, for attribution. */
    std::string tag_{"ma-crossover"};
  };

  /** The configuration this instance runs under. */
  Params params_;

  /**
   * Bars needed before the strategy will act.
   *
   * The slow window plus one, because a level comparison still reads two values so that the same
   * code answers "is it above" and "did it just cross".
   */
  std::size_t warmup_bars() const { return params_.slow_ + 1; }

  /** Reads the latest bar and, when the signal says so, states a target position. */
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
    if (static_cast<std::size_t>(closes.size()) < warmup_bars()) {
      return;
    }

    const auto fast = line(closes, params_.fast_);
    const auto slow = line(closes, params_.slow_);
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
  /** Returns the chosen average of `closes`, the whole cached series; see the class comment. */
  math::Vector line(math::SeriesView closes, std::size_t span) const {
    if (params_.kind_ == AverageKind::EXPONENTIAL) {
      return exponential_line(closes, span);
    }
    // Only the window: slicing keeps the per-bar cost from growing with the cache. The slice is the
    // slow window's for both lines, as it always was, so these numbers do not move.
    return analytics::sma(closes.tail(static_cast<math::Index>(params_.slow_) + 1), span);
  }
};

/** A simple-average crossover, the conventional spelling. */
struct SmaCrossover {
  /** The general crossover, with `kind_` pinned to `SIMPLE`. */
  MovingAverageCrossover inner_;

  /** Constructs one from `params`, overriding `kind_`. */
  explicit SmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::SIMPLE;
    inner_.params_ = std::move(params);
  }

  /** @copydoc MovingAverageCrossover::warmup_bars() */
  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  /** Forwards to the general crossover. */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

/** An exponential-average crossover. */
struct EmaCrossover {
  /** The general crossover, with `kind_` pinned to `EXPONENTIAL`. */
  MovingAverageCrossover inner_;

  /** Constructs one from `params`, overriding `kind_`. */
  explicit EmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::EXPONENTIAL;
    inner_.params_ = std::move(params);
  }

  /** @copydoc MovingAverageCrossover::warmup_bars() */
  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  /** Forwards to the general crossover. */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
