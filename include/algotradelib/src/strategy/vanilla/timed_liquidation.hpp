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
 * @brief Flattens every position when the market enters a phase it should not hold through.
 *
 * On each phase change that is not in `hold_through_` (including `UNKNOWN`, an uncovered calendar
 * date), it states a target of zero for every instrument held. Working orders against those targets
 * are cancelled by reconciliation.
 *
 * @warning To combine it with a signal strategy, the signal must stop stating its target once this
 *   has fired. `on_bar` runs after `on_phase` in the same pass, so a signal that restates its target
 *   on every bar (such as `BuyAndHold`) replaces the flatten. See `BuyThenLiquidate` in
 *   `tests/test-backtest.cpp` for a gated composition.
 */
struct TimedLiquidation {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The phases in which positions may be held; entering any other phase flattens. */
    std::vector<MarketPhase> hold_through_{MarketPhase::OPEN, MarketPhase::PRE_OPEN};
    /** @brief The tag on the targets. */
    std::string tag_{"timed-liquidation"};
  };

  /** @brief The settings. */
  Params params_;

  /** @brief No history is needed. */
  static constexpr std::size_t warmup_bars = 0;

  /**
   * @brief Targets zero in every held instrument if `phase` is not in `hold_through_`.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param phase The new market phase.
   */
  template <class Ctx>
  void on_phase(Ctx& ctx, MarketPhase phase) {
    if (!holds_through(phase)) {
      flatten(ctx);
    }
  }

private:
  // Whether `phase` is one this strategy may hold through.
  bool holds_through(MarketPhase phase) const {
    for (const auto allowed : params_.hold_through_) {
      if (allowed == phase) {
        return true;
      }
    }
    return false;
  }

  // Targets zero in every instrument held, once per instrument (a holding may span several rows,
  // and duplicate targets would each plan a sell). Working orders are left to reconciliation.
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
