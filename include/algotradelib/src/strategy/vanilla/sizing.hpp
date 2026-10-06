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
 * @brief Holds a long position sized so its daily value volatility matches a risk budget.
 *
 * The target is `q = budget / (price * s)`, where `s` is the sample standard deviation of the last
 * `window_` bar returns, capped at `max_quantity_`. Nothing is stated while the window is
 * incomplete or flat. The budget is in currency, not a fraction of equity.
 */
struct VolatilityTargetSizing {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instrument to hold; bars for any other instrument are ignored. */
    InstrumentId instrument_;
    /** @brief The number of returns the volatility is measured over; below 2, nothing is traded. */
    std::size_t window_{20};
    /** @brief The value volatility per bar the position should have, in the instrument's currency. */
    Decimal64 risk_budget_;
    /** @brief The largest position to hold, whatever the arithmetic gives. */
    Decimal64 max_quantity_;
    /** @brief The tag on the targets. */
    std::string tag_{"volatility-target"};
  };

  /** @brief The settings. */
  Params params_;

  /**
   * @brief Returns how many bars to load before the first live bar.
   * @return `window_ + 2`.
   */
  std::size_t warmup_bars() const { return params_.window_ + 2; }

  /**
   * @brief States the volatility-sized target position on each bar of the instrument.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param bar The new bar.
   */
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
    // A flat window has no volatility to size against; do not fall back to the maximum.
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
 * @brief Holds an equal fraction of portfolio value in each instrument of a universe.
 *
 * Rebalances only when the market enters `rebalance_at_` (via `on_phase`), not on every bar. Each
 * weight is `1 / n` truncated at `weight_scale_` decimal places (three names get `0.3333`); the
 * residual stays unallocated. Each weight is a holding (`SizeSpec::target_weight`), so a rebalance
 * trades only the drift; set a notional `TradeTolerance` to skip one-share drifts.
 *
 * @note Live, the runner refuses weights (`no_portfolio_value`) until you call
 *   `Runner::set_portfolio_value`.
 * @note In a backtest on **daily** bars, `on_phase` fires about once in the whole run, because the
 *   replay clock visits only bar ends; replay intraday bars to rebalance each session. In a
 *   multi-instrument replay the other instruments are sized at their previous close; on a
 *   mixed-currency replay the portfolio value counts one currency only; and quotes move prices while
 *   the value stays at the last bar's marks.
 */
struct EqualWeightRebalance {
  /** @brief The strategy's settings. */
  struct Params {
    /** @brief The instruments to hold; an empty universe does nothing. */
    std::vector<InstrumentId> universe_;
    /** @brief Decimal places of each weight; 4 is a hundredth of a percent. */
    std::uint8_t weight_scale_{4};
    /** @brief The phase whose start triggers a rebalance. */
    MarketPhase rebalance_at_{MarketPhase::OPEN};
    /** @brief The tag on the orders. */
    std::string tag_{"equal-weight"};
  };

  /** @brief The settings. */
  Params params_;

  /** @brief No history is needed before the first live bar. */
  static constexpr std::size_t warmup_bars = 0;

  /**
   * @brief Submits an equal-weight buy intent for every instrument when `phase` is `rebalance_at_`.
   * @tparam Ctx The runner's context type.
   * @param ctx The context.
   * @param phase The new market phase.
   */
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
