#pragma once

#include "./signal.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <string>
#include <vector>

namespace algotradelib {
namespace strategy {
namespace vanilla {

/**
 * Holds a position sized so its recent volatility contributes a stated risk budget.
 *
 * A *sizing policy*, not a signal: it decides how much, and something else decides whether. Compose
 * it by running both — the signal states a direction and this states the size — or use it alone, as
 * here, where the direction is simply "long".
 *
 * The arithmetic is the standard one. A position of `q` shares at price `p` whose daily return
 * deviation is `s` has a daily value deviation of `q * p * s`, so a budget of `b` currency units per
 * day gives `q = b / (p * s)`. Stated in currency rather than as a fraction of equity because a
 * fraction needs a portfolio total, which spans currencies and would need an exchange rate this
 * library does not carry.
 *
 * **Volatility is measured on returns, not on prices.** A deviation of prices is in currency and
 * scales with the price level, so the same formula would size a 700,000-won instrument and a
 * 7,000-won one identically wrongly. That mistake produces plausible numbers, which is why it is
 * worth stating rather than only doing.
 */
struct VolatilityTargetSizing {
  /** How this strategy is configured. */
  struct Params {
    /** Which instrument to size. Bars for anything else are ignored. */
    InstrumentId instrument_;
    /** Window the return deviation is measured over. */
    std::size_t window_{20};
    /** Currency of value deviation per bar the position should contribute. */
    Decimal64 risk_budget_;
    /** Largest position to hold whatever the arithmetic says, as a last guard against a tiny
     *  measured volatility producing an enormous size. */
    Decimal64 max_quantity_;
    /** Label carried onto the orders, for attribution. */
    std::string tag_{"volatility-target"};
  };

  /** The configuration this instance runs under. */
  Params params_;

  /** The window plus two: one row is lost to the first return, and one to reading a previous value. */
  std::size_t warmup_bars() const { return params_.window_ + 2; }

  /** Reads the latest bar and, when the signal says so, states a target position. */
  template <class Ctx>
  void on_bar(Ctx& ctx, const BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    if (params_.window_ < 2) {
      return;
    }
    const auto* series = ctx.bars(bar.instrument_, bar.interval_);
    if (series == nullptr) {
      return;
    }
    const auto closes = series->column(analytics::BarField::CLOSE);
    // One extra row, because a return consumes one: a deviation over `window_` returns needs
    // `window_ + 1` closes.
    const auto window = trailing(closes, params_.window_ + 1);
    if (!window) {
      return;
    }

    const auto period_returns = analytics::returns(*window);
    const auto deviations = analytics::rolling_stdev(period_returns, params_.window_);
    const auto deviation = deviations[deviations.size() - 1];
    const auto close = (*window)[window->size() - 1];
    if (!math::observed(deviation) || !math::observed(close)) {
      return;
    }
    // A flat window has no volatility to budget against. Sizing from it would divide by zero, and
    // clamping to the maximum would take the largest possible position on the least information.
    if (deviation <= math::Scalar{0} || close <= math::Scalar{0}) {
      return;
    }

    const auto value_deviation = analytics::to_decimal(close * deviation, 4);
    if (!value_deviation || value_deviation->coefficient() == 0) {
      return;
    }
    const auto sized = decimal_divide(params_.risk_budget_, *value_deviation, 0);
    if (!sized) {
      return;
    }

    TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.target_quantity_ =
      decimal_less(params_.max_quantity_, *sized) ? params_.max_quantity_ : *sized;
    target.tag_ = params_.tag_;
    ctx.submit(target);
  }
};

/**
 * Holds an equal fraction of the portfolio in each instrument of a universe.
 *
 * The cross-sectional one, and the reason `Panel` exists: it acts on many instruments from one
 * decision rather than on each in isolation.
 *
 * It rebalances only when told to by `on_phase`, not on every bar. A weight-based strategy asked to
 * rebalance per bar would trade every instrument every bar, because a price move changes every
 * weight — and the turnover of chasing that costs more than the drift it corrects.
 *
 * Weights are exact decimals at a stated scale, so an equal split of three names is `0.3333` rather
 * than a double nobody chose the precision of. The residual is left unallocated: distributing it
 * would make one arbitrary name larger than the others.
 */
struct EqualWeightRebalance {
  /** How this strategy is configured. */
  struct Params {
    /** The instruments to hold. An empty universe holds nothing. */
    std::vector<InstrumentId> universe_;
    /** Scale the weight is expressed at; four places is a hundredth of a percent. */
    std::uint8_t weight_scale_{4};
    /** Phase that triggers a rebalance. */
    MarketPhase rebalance_at_{MarketPhase::OPEN};
    /** Label carried onto the orders, for attribution. */
    std::string tag_{"equal-weight"};
  };

  /** The configuration this instance runs under. */
  Params params_;

  /** No warm-up: a weight needs no history, only a price to size against. */
  static constexpr std::size_t warmup_bars = 0;

  /** Restates an equal-weight target for the universe when `rebalance_at_` arrives. */
  template <class Ctx>
  void on_phase(Ctx& ctx, MarketPhase phase) {
    if (phase != params_.rebalance_at_ || params_.universe_.empty()) {
      return;
    }
    // One divided by the count, truncated at the stated scale, so the arithmetic is exact and the
    // sum is at most one rather than possibly over it.
    const auto share = decimal_divide(
      Decimal64::parse("1"),
      Decimal64(static_cast<std::int64_t>(params_.universe_.size()), 0),
      params_.weight_scale_
    );
    if (!share || share->coefficient() == 0) {
      return;
    }

    for (const auto& instrument : params_.universe_) {
      ctx.submit(
        OrderIntent::market(instrument, OrderSide::BUY, SizeSpec::target_weight(*share))
          .with_tag(params_.tag_)
      );
    }
  }
};

} // namespace vanilla
} // namespace strategy
} // namespace algotradelib
