#pragma once

#include "./cost.hpp"
#include "./ledger.hpp"

#include "../strategy/event.hpp"
#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <optional>
#include <utility>

namespace algotradelib {
namespace backtest {

/**
 * Deciding whether a queued order trades, and at what price.
 *
 * **The lookahead discipline is structural here rather than a rule to remember.** A fill is always
 * computed against the bar *after* the one that produced the order, because the function has no way to
 * see any other: it takes the next bar and nothing else. An engine that passed the signal's own bar
 * would have to do so deliberately, and the test suite asserts that it does not.
 *
 * That is why `Runner`'s executor accepts an order without filling it. Filling at the moment of
 * submission would mean filling from a bar the strategy has just been shown, which is reading the
 * future — the most common defect in backtesting code and the reason `BarStamp` and
 * `BarEvent::period_end_` exist at all.
 */

/** What a fill model decided, and what it assumed. */
struct FillDecision {
  /** The execution, absent when the order did not trade against this bar. */
  std::optional<Execution> execution_;
  /** Whether the order should stay queued for a later bar rather than being discarded. */
  bool still_working_{false};
  /**
   * Why the fill could not be **costed**, as distinct from not having traded.
   *
   * The two were one answer until this field existed, and conflating them is the failure it exists
   * to prevent: a date the schedule does not cover produced a decision with no execution, which the
   * engine read as an order that simply did not fill, and the replay continued with a trade silently
   * missing from a path every performance figure is derived from. Present here, the engine halts.
   */
  std::optional<BrokerError> costing_error_;
};

/**
 * Fills market orders at the next bar's open and limit orders when the bar's range reaches them.
 *
 * The plain model, and the one whose assumptions are easiest to state — which matters more than
 * sophistication in a component whose output somebody will act on.
 *
 * **Interval-agnostic.** "The next bar's open" is the same sentence for a minute bar and a daily one,
 * which is the whole reason the rule is expressed that way rather than as "the next day's open".
 *
 * **Volume is not consulted**, so a fill is never partial and never capped by what traded. A bar's
 * volume says how much changed hands in total, not how much would have been available to *this* order
 * at *its* price, and inventing a participation rate would be inventing a number with no data behind
 * it. The report states this rather than leaving a reader to assume otherwise.
 */
class NextBarFillModel {
public:
  /**
   * Constructs a model with the given costs and limit-price assumption.
   *
   * The `CommissionModel` spelling is preserved so every existing caller keeps compiling and keeps
   * charging exactly what it charged; it becomes a `CommissionPolicy` that resolves the same number
   * on every date and at every venue.
   */
  explicit NextBarFillModel(
    CommissionModel commission = CommissionModel::none(),
    SlippageModel slippage = SlippageModel::none(),
    LimitFillPrice limit_price = LimitFillPrice::AT_LIMIT
  )
  : commission_(CommissionPolicy::stated(std::move(commission))),
    slippage_(std::move(slippage)),
    limit_price_(limit_price) {}

  /** Constructs a model costing each execution through `commission`, which may resolve a schedule. */
  explicit NextBarFillModel(
    CommissionPolicy commission,
    SlippageModel slippage = SlippageModel::none(),
    LimitFillPrice limit_price = LimitFillPrice::AT_LIMIT
  )
  : commission_(std::move(commission)),
    slippage_(std::move(slippage)),
    limit_price_(limit_price) {}

  /** Returns the cost policy in force, so a report can state what it applied. */
  const CommissionPolicy& commission() const noexcept { return commission_; }

  /** Returns the slippage model in force. */
  const SlippageModel& slippage() const noexcept { return slippage_; }

  /** Returns what a limit order is credited with. */
  LimitFillPrice limit_price() const noexcept { return limit_price_; }

  /**
   * Returns whether an unfilled order stays queued for a later bar.
   *
   * Resting is a property of the *condition*, not of the basis: an
   * immediate-or-cancel order that this bar did not fill is gone, and a
   * best-limit day order that this bar did not fill is still there. This read
   * `type_ == OrderType::LIMIT`, which was the same answer while every order
   * was a day order and is not now.
   *
   * A market order still does not rest, because `reference_price` always prices
   * one -- an unfilled market order is a fill the model refused for another
   * reason, and queueing it would retry it on the next bar against a price it
   * was never offered.
   *
   * Fill-or-kill and immediate-or-cancel behave identically here, and that is
   * the model rather than a simplification: this model never fills partially,
   * so "cancel the remainder" and "cancel the lot" describe the same outcome.
   */
  static bool rests_unfilled(const strategy::PlannedOrder& order) noexcept {
    if (order.time_in_force_ != TimeInForce::DAY) {
      return false;
    }
    return order.type_ == OrderType::LIMIT;
  }

  /**
   * Decides `order` against `next`, the bar after the one that produced it.
   *
   * @param order the queued instruction
   * @param next the bar to fill against; never the bar the order was decided on
   * @param rule the instrument's price grid, for the tick component of slippage
   * @param date the **market-local** calendar day the fill happens on, which is what a published
   * rate window is written in. `SessionWindow::local_date` converts the replay clock's instant, and
   * a bar either side of UTC midnight is what distinguishes the two.
   */
  FillDecision decide(
    const strategy::PlannedOrder& order,
    const strategy::BarEvent& next,
    const PriceRule& rule,
    const Date& date
  ) const {
    FillDecision decision;

    const auto reference = reference_price(order, next);
    if (!reference) {
      // A limit the bar never reached. It stays queued: a resting order does not expire because one
      // bar passed it by, and discarding it would silently turn every limit into a fill-or-kill.
      decision.still_working_ = rests_unfilled(order);
      return decision;
    }

    const auto slipped = slippage_.apply(*reference, order.side_, rule);
    if (!slipped) {
      decision.still_working_ = rests_unfilled(order);
      return decision;
    }
    auto charged =
      commission_.charge(order.quantity_, slipped->first, date, order.instrument_.country_);
    if (!charged) {
      // **Not `still_working_`, and not silence.** A cost that cannot be resolved is not a bar that
      // did not reach a limit: leaving the order queued would retry it on the next bar and leaving
      // it quiet would drop a trade out of the path the report describes. The engine halts on this.
      decision.costing_error_ = std::move(charged.error());
      return decision;
    }

    Execution execution;
    execution.fill_.instrument_ = order.instrument_;
    execution.fill_.side_ = order.side_;
    execution.fill_.quantity_ = order.quantity_;
    execution.fill_.price_ = slipped->first;
    execution.fill_.tag_ = order.tag_;
    execution.fill_.at_ = next.period_end_;
    execution.commission_ = charged.value().broker_;
    execution.institution_fee_ = charged.value().institution_;
    execution.applied_ = std::move(charged.value().applied_);
    execution.slippage_ = slipped->second;
    decision.execution_ = std::move(execution);
    return decision;
  }

private:
  /**
   * Returns the price before slippage, or nothing when the order does not trade against this bar.
   *
   * A market order takes the open. A limit trades only if the bar's range reached it — a buy needs the
   * low at or below the limit, a sell needs the high at or above it — because a bar that never touched
   * the price is a bar in which the order sat unfilled, however close it came.
   */
  std::optional<Decimal64> reference_price(
    const strategy::PlannedOrder& order,
    const strategy::BarEvent& next
  ) const {
    const auto open = next.bar_.open_ ? *next.bar_.open_ : next.bar_.close_;

    if (order.type_ != OrderType::LIMIT || !order.price_) {
      return open;
    }
    const auto& limit = *order.price_;

    if (order.side_ == OrderSide::BUY) {
      if (decimal_less(limit, next.bar_.low_)) {
        return std::nullopt;
      }
      if (limit_price_ == LimitFillPrice::AT_OPEN_IF_BETTER && decimal_less(open, limit)) {
        return open;
      }
      return limit;
    }

    if (decimal_less(next.bar_.high_, limit)) {
      return std::nullopt;
    }
    if (limit_price_ == LimitFillPrice::AT_OPEN_IF_BETTER && decimal_less(limit, open)) {
      return open;
    }
    return limit;
  }

  CommissionPolicy commission_;
  SlippageModel slippage_;
  LimitFillPrice limit_price_;
};

} // namespace backtest
} // namespace algotradelib
