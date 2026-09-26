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
 * Buys a stated quantity once and never trades again.
 *
 * The simplest thing that is still a strategy, and it earns its place twice: as the benchmark every
 * other strategy is measured against, and as the minimum test of the interface. It reaches for
 * nothing but a target and an instrument, so if *this* needs something the context does not offer,
 * the context is wrong.
 *
 * Emits a target rather than an order, which is why it "never trades again" without tracking
 * anything: once the position matches, reconciliation produces nothing. A version that emitted an
 * order would have to remember whether it had already bought, and would buy twice after a restart.
 */
struct BuyAndHold {
  /** How this strategy is configured. */
  struct Params {
    /** Which instrument to hold. Bars for anything else are ignored. */
    InstrumentId instrument_;
    /** How many shares to hold. */
    Decimal64 quantity_;
    /** Label carried onto the orders, for attribution. */
    std::string tag_{"buy-and-hold"};
  };

  /** The configuration this instance runs under. */
  Params params_;

  /** No warm-up: one bar is enough to know the instrument trades. */
  static constexpr std::size_t warmup_bars = 1;

  /** States the configured position on every bar for the configured instrument. */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.target_quantity_ = params_.quantity_;
    target.tag_ = params_.tag_;
    ctx.submit(target);
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
