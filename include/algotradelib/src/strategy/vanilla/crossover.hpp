#pragma once

#include "./signal.hpp"

#include "../event.hpp"
#include "../intent.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <string>
#include <utility>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * @brief Which moving average `MovingAverageCrossover` uses.
 *
 * - `SIMPLE` reads the last `slow_ + 1` closes.
 * - `EXPONENTIAL` reads every cached close (up to the runner's `series_capacity`). A line with at
 *   least three spans of observed closes uses `analytics::ema` seeded at the oldest close; a line
 *   with fewer uses `adjusted_ema`, which does not overweight the first close (right for a new
 *   listing). The choice is made per line.
 *
 * @note With `EXPONENTIAL`, the signal can change on an unchanged close when one line switches from
 *   the adjusted to the plain average. A span above a third of `series_capacity` (170 at the default
 *   512) always uses the adjusted average. A backtest, whose cache keeps growing, can differ from a
 *   live run that loaded fewer bars until the live cache catches up.
 */
enum class AverageKind {
  /** @brief The simple mean of the window. */
  SIMPLE,
  /** @brief An exponential average with `alpha = 2 / (span + 1)`, over every cached bar. */
  EXPONENTIAL,
};

/**
 * @brief Holds `quantity_` shares while the fast moving average is above the slow one, and is flat
 *   otherwise.
 *
 * On every bar it states a target position (the level, not a crossing event), so it keeps no state
 * and behaves the same after a restart.
 */
struct MovingAverageCrossover {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to trade; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The fast span; must be nonzero and less than `slow_`, or the strategy does nothing. */
    std::size_t fast_{20};
    /** @brief The slow span. */
    std::size_t slow_{50};
    /** @brief Which average to use; see `AverageKind`. */
    AverageKind kind_{AverageKind::SIMPLE};
    /** @brief The shares to hold while the fast average is above the slow one. */
    Decimal64 quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"ma-crossover"};
  };

  /** @brief The settings. */
  Params params_;

  /**
   * @brief Returns how many bars to load before the first live bar.
   * @return `slow_ + 1`; the strategy acts only once that many closes are cached.
   */
  std::size_t warmup_bars() const { return params_.slow_ + 1; }

  /**
   * @brief States the target position for the instrument on each of its bars.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.fast_ == 0 || params_.slow_ <= params_.fast_) {
      // A fast window that is not faster would invert the signal; do nothing instead.
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
  // The chosen average of `closes`, the whole cached series; see `AverageKind`.
  math::Vector line(math::SeriesView closes, std::size_t span) const {
    if (params_.kind_ == AverageKind::EXPONENTIAL) {
      return exponential_line(closes, span);
    }
    // Only the window: slicing keeps the per-bar cost from growing with the cache. The slice is the
    // slow window's for both lines, as it always was, so these numbers do not move.
    return analytics::sma(closes.tail(static_cast<math::Index>(params_.slow_) + 1), span);
  }
};

/** @brief `MovingAverageCrossover` with simple averages. */
struct SmaCrossover {
  /** @brief The general crossover, with `kind_` set to `SIMPLE`. */
  MovingAverageCrossover inner_;

  /**
   * @brief Constructs the strategy.
   * @param params The settings; `kind_` is overridden with `SIMPLE`.
   */
  explicit SmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::SIMPLE;
    inner_.params_ = std::move(params);
  }

  /** @copydoc MovingAverageCrossover::warmup_bars() */
  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  /**
   * @brief Calls the general crossover's `on_bar`.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

/** @brief `MovingAverageCrossover` with exponential averages. */
struct EmaCrossover {
  /** @brief The general crossover, with `kind_` set to `EXPONENTIAL`. */
  MovingAverageCrossover inner_;

  /**
   * @brief Constructs the strategy.
   * @param params The settings; `kind_` is overridden with `EXPONENTIAL`.
   */
  explicit EmaCrossover(MovingAverageCrossover::Params params = {}) {
    params.kind_ = AverageKind::EXPONENTIAL;
    inner_.params_ = std::move(params);
  }

  /** @copydoc MovingAverageCrossover::warmup_bars() */
  std::size_t warmup_bars() const { return inner_.warmup_bars(); }

  /**
   * @brief Calls the general crossover's `on_bar`.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    inner_.on_bar(ctx, bar);
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
