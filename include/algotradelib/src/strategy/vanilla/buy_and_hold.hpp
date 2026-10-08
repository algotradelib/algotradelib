#pragma once

#include "../event.hpp"
#include "../intent.hpp"

#include <algotradelib/core>

#include <cstddef>
#include <string>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * @brief Holds a fixed quantity of one instrument.
 *
 * States the same target position on every bar, so it buys once and then sends nothing while the
 * holding matches, including after a restart. Useful as a benchmark.
 */
struct BuyAndHold {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to hold; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The shares to hold. */
    Decimal64 quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"buy-and-hold"};
  };

  /** @brief The settings. */
  Params params_;

  /** @brief One bar of history is enough. */
  static constexpr std::size_t warmup_bars = 1;

  /**
   * @brief States the target position on each bar of the instrument.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
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
