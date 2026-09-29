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
  /**
   * Whether this bar released a dormant stop-limit order.
   *
   * Set whether or not it also filled, so the engine can mark a stop that released and still rests;
   * the next bar evaluates it as the limit it has become. See `NextBarFillModel::decide`.
   */
  bool released_{false};
  /**
   * Whether the fill depended on a release **inside** the bar -- the case a bar with no path between
   * its open and its extremes could have ordered differently. Counted by the report.
   */
  bool intrabar_release_fill_{false};
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
    // A stop rests while it is dormant and, once released, as the limit it has become.
    return order.type_ == OrderType::LIMIT || order.type_ == OrderType::STOP_LIMIT;
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
   * @param open_date the market-local day the bar's **open** belongs to, where the caller knows it;
   * used instead of `date` when the fill is priced at the open -- a market order, or a limit under
   * `AT_OPEN_IF_BETTER` whose open was better. `Backtester` supplies it for an `OPEN`-stamped bar,
   * whose stamp is its period's start: `date` there comes from a *nominal* period end, one day late
   * for a daily bar and up to five days into the next month for a monthly one, and a rate that
   * changed in between was charged a fill it did not apply to. Absent, every fill uses `date`, as
   * before. **A partial correction**: a fill priced inside a multi-day bar has no known day and still
   * uses `date` (`plan/todo/nominal-period-end-dates-a-fill.md`).
   * @param released whether an earlier bar already released this order, when it is a stop-limit
   * order; the engine records it on the queued order. A released stop is priced as the limit it has
   * become. Ignored for every other basis.
   */
  FillDecision decide(
    const strategy::PlannedOrder& order,
    const strategy::BarEvent& next,
    const PriceRule& rule,
    const Date& date,
    const std::optional<Date>& open_date = std::nullopt,
    bool released = false
  ) const {
    FillDecision decision;
    bool at_open = false;
    std::optional<Decimal64> reference;
    if (order.type_ == OrderType::STOP_LIMIT && !released) {
      if (!order.trigger_price_ || !order.price_) {
        // Not a stop this model can release; `ReplayExecutor::can_model` refuses one before it is
        // queued, so this is a caller driving the model directly. It does not trade and does not rest.
        return decision;
      }
      switch (release_of(order, next)) {
        case Release::NONE:
          // Dormant: the bar never reached the trigger.
          decision.still_working_ = true;
          return decision;
        case Release::AT_OPEN:
          // The whole bar is after the release, so it is priced as the limit it now is.
          decision.released_ = true;
          reference = reference_price(order, next, at_open);
          // **Dated from the open whatever the price policy credits.** A stop released at the open
          // and marketable there traded at the open; `AT_LIMIT` crediting the limit rather than the
          // open is a statement about price, not about when, and dating it from the period end
          // charged a multi-day bar's fill on a day it did not happen.
          if (reference && limit_marketable_at(order, *next.bar_.open_)) {
            at_open = true;
          }
          break;
        case Release::INTRABAR:
          decision.released_ = true;
          reference = intrabar_release_price(order, next);
          if (!reference) {
            // Whether the price reached the limit *after* the release is what the bar cannot say --
            // its extreme on the other side may have come first. Released; rests from the next bar,
            // which can only defer a fill.
            decision.still_working_ = true;
            return decision;
          }
          decision.intrabar_release_fill_ = true;
          break;
      }
    } else {
      reference = reference_price(order, next, at_open);
    }
    if (!reference) {
      // A limit the bar never reached. It stays queued: a resting order does not expire because one
      // bar passed it by, and discarding it would silently turn every limit into a fill-or-kill.
      decision.still_working_ = rests_unfilled(order);
      return decision;
    }

    // **A stop fill is credited a price its bar traded, and never one through its limit.** The
    // bound is the worst such price: a sell the higher of the bar's low and its limit, a buy the lower
    // of the bar's high and its limit. Applied to the reference before slippage -- a plain limit's
    // `AT_LIMIT` credit can sit outside the bar when the bar opened through it -- and to the slipped
    // price after, since slippage can carry an in-range price out of the bar or through the limit.
    // A plain limit is not bounded yet (`plan/todo/limit-slippage-breach.md`).
    std::optional<Decimal64> bound;
    if (order.type_ == OrderType::STOP_LIMIT && order.price_) {
      const auto& limit = *order.price_;
      if (order.side_ == OrderSide::SELL) {
        bound = decimal_less(limit, next.bar_.low_) ? next.bar_.low_ : limit;
        if (decimal_less(*reference, *bound)) {
          reference = bound;
        }
      } else {
        bound = decimal_less(next.bar_.high_, limit) ? next.bar_.high_ : limit;
        if (decimal_less(*bound, *reference)) {
          reference = bound;
        }
      }
    }
    auto slipped = slippage_.apply(*reference, order.side_, rule);
    if (slipped && bound) {
      // **The other side too.** `SlippageModel` accepts a negative figure, which moves a fill in the
      // order's favour -- and past the bar's opposite extreme it is a price the bar never showed, the
      // same as one pushed past the limit. Held at that extreme, with the move recorded as it was.
      const auto& ceiling = order.side_ == OrderSide::SELL ? next.bar_.high_ : next.bar_.low_;
      const bool past = order.side_ == OrderSide::SELL ?
        decimal_less(ceiling, slipped->first) :
        decimal_less(slipped->first, ceiling);
      if (past) {
        const auto moved = order.side_ == OrderSide::SELL ?
          decimal_subtract(*reference, ceiling) :
          decimal_subtract(ceiling, *reference);
        if (!moved) {
          slipped.reset();
        } else {
          slipped->first = ceiling;
          slipped->second = *moved;
        }
      }
    }
    if (slipped && bound) {
      const bool beyond = order.side_ == OrderSide::SELL ?
        decimal_less(slipped->first, *bound) :
        decimal_less(*bound, slipped->first);
      if (beyond) {
        const auto moved = order.side_ == OrderSide::SELL ?
          decimal_subtract(*reference, *bound) :
          decimal_subtract(*bound, *reference);
        if (!moved) {
          slipped.reset();
        } else {
          slipped->first = *bound;
          slipped->second = *moved;
        }
      }
    }
    if (!slipped) {
      decision.still_working_ = rests_unfilled(order);
      return decision;
    }
    const auto& charged_on = at_open && open_date ? *open_date : date;
    auto charged =
      commission_.charge(order.quantity_, slipped->first, charged_on, order.instrument_.country_);
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
  /** Where in a bar a dormant stop released, if it did. */
  enum class Release {
    /** The bar never reached the trigger. */
    NONE,
    /** The open was already through the trigger, so the whole bar followed the release. */
    AT_OPEN,
    /** The bar's extreme reached the trigger and its open did not. */
    INTRABAR,
  };

  /**
   * Returns whether, and where, `next` released a dormant stop.
   *
   * **A sell stop releases when the price falls to its trigger and a buy stop when it rises to it.**
   * That is an assumption about the broker -- neither KIS nor Kiwoom documents the direction -- and
   * it is the universal convention. `ReportCaveats::stop_releases_` counts how often it was used.
   *
   * A bar with no open cannot establish a release *at* the open, so a release there is intrabar,
   * which is the answer that credits less.
   */
  static Release release_of(const strategy::PlannedOrder& order, const strategy::BarEvent& next) {
    const auto& trigger = *order.trigger_price_;
    const bool sell = order.side_ == OrderSide::SELL;
    const bool reached = sell ?
      !decimal_less(trigger, next.bar_.low_) :
      !decimal_less(next.bar_.high_, trigger);
    if (!reached) {
      return Release::NONE;
    }
    if (next.bar_.open_) {
      const auto& open = *next.bar_.open_;
      const bool through = sell ? !decimal_less(trigger, open) : !decimal_less(open, trigger);
      if (through) {
        return Release::AT_OPEN;
      }
    }
    return Release::INTRABAR;
  }

  /** Returns whether the order's limit would trade at `price` -- a sell at or above it, a buy at or below. */
  static bool limit_marketable_at(const strategy::PlannedOrder& order, const Decimal64& price) {
    const auto& limit = *order.price_;
    return order.side_ == OrderSide::SELL ? !decimal_less(price, limit) : !decimal_less(limit, price);
  }

  /**
   * Returns what a stop released inside `next` is credited with, or nothing when the bar cannot show
   * it traded after the release.
   *
   * At the release the price *was* the trigger. Two cases, using only prices the bar proves came after
   * it -- the trigger itself and the close, which is always last:
   *
   * - **Marketable at the trigger** (a sell limited at or below it, a buy at or above): it trades. It is
   *   credited the **worst price the bar traded that the limit allows** -- a sell the higher of its
   *   limit and the bar's low, a buy the lower of its limit and the bar's high -- so the price is inside
   *   the bar and never through the limit. Crediting the limit itself could name a price the bar never
   *   reached; crediting the trigger would assume the book was there.
   * - **Not marketable at the trigger** (a sell limited above it): it trades only if the price came back
   *   to the limit after the release. The close proves that when it is at or through the limit; then it
   *   is credited the limit. Otherwise the bar's other extreme may have come first, and nothing is.
   */
  static std::optional<Decimal64> intrabar_release_price(
    const strategy::PlannedOrder& order,
    const strategy::BarEvent& next
  ) {
    const auto& trigger = *order.trigger_price_;
    const auto& limit = *order.price_;
    const bool sell = order.side_ == OrderSide::SELL;
    if (limit_marketable_at(order, trigger)) {
      if (sell) {
        return decimal_less(limit, next.bar_.low_) ? next.bar_.low_ : limit;
      }
      return decimal_less(next.bar_.high_, limit) ? next.bar_.high_ : limit;
    }
    if (limit_marketable_at(order, next.bar_.close_)) {
      return limit;
    }
    return std::nullopt;
  }

  /**
   * Returns the price before slippage, or nothing when the order does not trade against this bar.
   *
   * A market order takes the open. A limit trades only if the bar's range reached it — a buy needs the
   * low at or below the limit, a sell needs the high at or above it — because a bar that never touched
   * the price is a bar in which the order sat unfilled, however close it came.
   *
   * Sets `at_open` when the price is the bar's stated open -- a market order, or a limit under
   * `AT_OPEN_IF_BETTER` whose open was better -- which is what lets `decide` date the charge from the
   * open's day. False for a bar with no open, whose close stands in for it.
   */
  std::optional<Decimal64> reference_price(
    const strategy::PlannedOrder& order,
    const strategy::BarEvent& next,
    bool& at_open
  ) const {
    at_open = false;
    // A bar with no open prices a market order at its close, which is not the open's moment.
    const bool has_open = next.bar_.open_.has_value();
    const auto open = has_open ? *next.bar_.open_ : next.bar_.close_;

    // A released stop is priced as the limit it has become.
    const bool limit_basis = order.type_ == OrderType::LIMIT || order.type_ == OrderType::STOP_LIMIT;
    if (!limit_basis || !order.price_) {
      at_open = has_open;
      return open;
    }
    const auto& limit = *order.price_;

    if (order.side_ == OrderSide::BUY) {
      if (decimal_less(limit, next.bar_.low_)) {
        return std::nullopt;
      }
      if (limit_price_ == LimitFillPrice::AT_OPEN_IF_BETTER && decimal_less(open, limit)) {
        at_open = has_open;
        return open;
      }
      return limit;
    }

    if (decimal_less(next.bar_.high_, limit)) {
      return std::nullopt;
    }
    if (limit_price_ == LimitFillPrice::AT_OPEN_IF_BETTER && decimal_less(limit, open)) {
      at_open = has_open;
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
