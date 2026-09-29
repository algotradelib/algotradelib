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
 * **Composable with a signal strategy only if that strategy stops stating its target once this one
 * has fired.** A target of zero is a target like any other and reconciliation takes the last one
 * stated -- which is the whole mechanism, and also the trap. `on_bar` runs after `on_phase` within
 * a single pass, so a signal that restates its target unconditionally restates it on the very bar
 * that liquidated, and reconciliation correctly withdraws the flatten as an order opposing the new
 * target. `BuyAndHold` is exactly such a strategy.
 *
 * The composing type therefore has to gate its signal, which is what
 * `tests/test-backtest.cpp`'s `BuyThenLiquidate` does and why it holds a flag. An earlier version
 * of this comment claimed composability with *any* signal strategy and it was wrong; the replay
 * appeared to confirm it only because `backtest::ReplayLedger::snapshot` reported no working
 * orders, so reconciliation could not see the flatten it would have withdrawn.
 *
 * Written against `on_phase` rather than a clock reading, which is the point of the phase existing.
 * A strategy comparing `ctx.now()` to a hard-coded 15:20 would be wrong on a half-day, wrong in a
 * backtest replaying a different market, and silently wrong the day an exchange changes its hours.
 *
 * It flattens on `UNKNOWN` as well as after the close. An uncovered calendar date is precisely when
 * a position should not be carried on an assumption.
 */
struct TimedLiquidation {
  /** How this strategy is configured. */
  struct Params {
    /** Phases in which holding is acceptable. Anything else flattens. */
    std::vector<MarketPhase> hold_through_{MarketPhase::OPEN, MarketPhase::PRE_OPEN};
    /** Label carried onto the orders, for attribution. */
    std::string tag_{"timed-liquidation"};
  };

  /** The configuration this instance runs under. */
  Params params_;

  /** No warm-up: the decision needs no history. */
  static constexpr std::size_t warmup_bars = 0;

  /** Targets flat as soon as the phase is not one of `hold_through_`, `UNKNOWN` included. */
  template <class Ctx>
  void on_phase(Ctx& ctx, MarketPhase phase) {
    if (!holds_through(phase)) {
      flatten(ctx);
    }
  }

  /**
   * Restates the flatten on every bar outside `hold_through_`.
   *
   * **A target stated once is not a target kept.** A flatten can be refused for a pass --
   * `RejectReason::STOP_ORDER_RESTING` withdraws a resting stop and sends nothing until the listing
   * stops reporting it -- and nothing retries a refused target on the strategy's behalf. Stated only
   * from `on_phase`, such a flatten was never stated again and the position was carried past the
   * phase this strategy exists to leave. Restating it is harmless otherwise: a flat instrument
   * submits nothing, and a sell already working toward zero plans nothing new.
   */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent&) {
    if (!holds_through(ctx.phase())) {
      flatten(ctx);
    }
  }

private:
  /** Returns whether `phase` is one this strategy may hold through. */
  bool holds_through(MarketPhase phase) const {
    for (const auto allowed : params_.hold_through_) {
      if (allowed == phase) {
        return true;
      }
    }
    return false;
  }

  /**
   * Targets zero in everything currently held.
   *
   * Positions only, not working orders: reconciliation withdraws anything working against a target
   * of zero by itself, so cancelling here as well would issue the same instruction twice.
   *
   * **One target per instrument, not per row.** A broker reporting one instrument as two lots is
   * ordinary, and this loop once submitted a target for each of them. Two identical targets are not
   * harmless: reconciliation plans each against the portfolio as it stood at the start of the pass,
   * so one holding split across two rows produced two sells rather than one. With
   * `reconcile(TargetPosition)` reading only the first row as well, the pair came to 120 against a
   * holding of 100 -- an over-sell into a short, from a strategy whose entire purpose is to end the
   * day flat. Both halves are fixed; this one is here because a duplicate target is wrong even with
   * the netting in place, where it plans one correct sell and one that a guard must refuse.
   */
  template <class Ctx>
  void flatten(Ctx& ctx) {
    std::vector<InstrumentId> targeted;
    for (const auto& position : ctx.portfolio().positions_) {
      if (position.quantity_.coefficient() == 0) {
        continue;
      }
      bool seen = false;
      for (const auto& already : targeted) {
        if (already == position.instrument_) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue;
      }
      targeted.push_back(position.instrument_);

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
