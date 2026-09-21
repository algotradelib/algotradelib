#pragma once

#include "../event.hpp"
#include "../intent.hpp"

#include <algotradelib/core>

#include <cstddef>
#include <string>
#include <vector>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * Flattens every position when the session leaves the phase it is allowed to hold through.
 *
 * Composable with any signal strategy: run both, and this one overrides at the boundary because a
 * target of zero is a target like any other and reconciliation takes the last one stated.
 *
 * Written against `on_phase` rather than a clock reading, which is the point of the phase existing.
 * A strategy comparing `ctx.now()` to a hard-coded 15:20 would be wrong on a half-day, wrong in a
 * backtest replaying a different market, and silently wrong the day an exchange changes its hours.
 *
 * It flattens on `UNKNOWN` as well as after the close. An uncovered calendar date is precisely when
 * a position should not be carried on an assumption.
 */
struct TimedLiquidation {
  struct Params {
    /** Phases in which holding is acceptable. Anything else flattens. */
    std::vector<MarketPhase> hold_through_{MarketPhase::OPEN, MarketPhase::PRE_OPEN};
    std::string tag_{"timed-liquidation"};
  };

  Params params_;

  /** No warm-up: the decision needs no history. */
  static constexpr std::size_t warmup_bars = 0;

  template <class Ctx>
  void on_phase(Ctx& ctx, MarketPhase phase) {
    for (const auto allowed : params_.hold_through_) {
      if (allowed == phase) {
        return;
      }
    }
    flatten(ctx);
  }

private:
  /**
   * Targets zero in everything currently held.
   *
   * Positions only, not working orders: reconciliation withdraws anything working against a target
   * of zero by itself, so cancelling here as well would issue the same instruction twice.
   */
  template <class Ctx>
  void flatten(Ctx& ctx) {
    for (const auto& position : ctx.portfolio().positions_) {
      if (position.quantity_.coefficient() == 0) {
        continue;
      }
      TargetPosition target;
      target.instrument_ = position.instrument_;
      target.target_quantity_ = Decimal64::parse("0");
      target.tag_ = params_.tag_;
      ctx.submit(target);
    }
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
