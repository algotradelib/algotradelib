#pragma once

#include <algotradelib/strategy>

#include <vector>

/**
 * Strategies that act on a phase change, for the tests of what the runner and the replay do with what they
 * submit there. Outside the library, as a user's own strategies would be.
 */
namespace fixture {

/**
 * On each phase change outside `hold_through_`, states a target of zero once for every instrument held, however
 * many rows the holding spans: one target per row would plan a sell for each.
 */
struct FlattensOnPhase {
  /** The phases in which positions may be held. */
  std::vector<algotradelib::strategy::MarketPhase> hold_through_{
    algotradelib::strategy::MarketPhase::OPEN,
    algotradelib::strategy::MarketPhase::PRE_OPEN,
  };

  /** Whether `phase` is one positions may be held through. */
  bool holds_through(algotradelib::strategy::MarketPhase phase) const {
    for (const auto allowed : hold_through_) {
      if (allowed == phase) {
        return true;
      }
    }
    return false;
  }

  template <class Ctx>
  void on_phase(Ctx& ctx, algotradelib::strategy::MarketPhase phase) {
    if (holds_through(phase)) {
      return;
    }
    std::vector<algotradelib::InstrumentId> targeted;
    for (const auto& position : ctx.portfolio().positions_) {
      if (position.quantity_.coefficient() == 0) {
        continue;
      }
      bool seen = false;
      for (const auto& already : targeted) {
        seen = seen || already == position.instrument_;
      }
      if (seen) {
        continue;
      }
      targeted.push_back(position.instrument_);
      algotradelib::strategy::TargetPosition target;
      target.instrument_ = position.instrument_;
      target.target_quantity_ = algotradelib::Decimal64::parse("0");
      target.tag_ = "flatten";
      ctx.submit(target);
    }
  }
};

/**
 * When the market enters `OPEN`, states a target weight of `weight_` for every instrument of `universe_`, as a
 * market buy sized by `SizeSpec::target_weight`: the runner plans the holding the weight states.
 */
struct HoldsEqualWeights {
  /** The instruments to hold. */
  std::vector<algotradelib::InstrumentId> universe_;
  /** Each instrument's fraction of the portfolio value. */
  algotradelib::Decimal64 weight_;

  template <class Ctx>
  void on_phase(Ctx& ctx, algotradelib::strategy::MarketPhase phase) {
    if (phase != algotradelib::strategy::MarketPhase::OPEN) {
      return;
    }
    for (const auto& instrument : universe_) {
      ctx.submit(
        algotradelib::strategy::OrderIntent::market(
          instrument, algotradelib::OrderSide::BUY, algotradelib::strategy::SizeSpec::target_weight(weight_)
        )
          .with_tag("equal-weight")
      );
    }
  }
};

} // namespace fixture
