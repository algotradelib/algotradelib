#pragma once

#include "./cost.hpp"
#include "./ledger.hpp"

#include "../strategy/event.hpp"
#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <optional>
#include <stdexcept>
#include <utility>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief The backtest fill model: whether a queued order trades against the next bar, and at what
 *   price and cost.
 *
 * An order is always filled against the bar **after** the one that produced it, so a backtest
 * cannot fill from a bar the strategy has just seen.
 */

/** @brief What `NextBarFillModel::decide` concluded about one order and one bar. */
struct FillDecision {
  /** @brief The execution, if the order traded against this bar. */
  std::optional<Execution> execution_;
  /** @brief Whether the order stays queued for a later bar (it did not trade but may still). */
  bool still_working_{false};
  /**
   * @brief Why a fill could not be costed (no commission rate, or a tax that does not fit).
   *
   * Set instead of an execution; the backtest halts rather than dropping the trade.
   */
  std::optional<BrokerError> costing_error_;
  /** @brief Whether this bar released a dormant stop-limit order (whether or not it also filled). */
  bool released_{false};
  /**
   * @brief Whether the fill depended on a stop released inside the bar, where the bar cannot show
   *   the order of prices; counted by the report.
   */
  bool intrabar_release_fill_{false};
  /**
   * @brief Whether a limit-type fill price was moved to stay within the bar's range and the limit;
   *   counted by the report.
   */
  bool bounded_{false};
};

/** @brief A tax on sales, stated by the caller; see `NextBarFillModel::with_sell_tax`. */
struct SellTax {
  /** @brief The fraction of a sale's value charged, such as `0.0020` for 0.20%. */
  Decimal64 rate_;
  /** @brief The market whose sales pay it. */
  Country country_{Country::KR};
};

/**
 * @brief The fill model: market orders at the next bar's open, limit orders when the next bar's
 *   range reaches them, stop-limit orders once released.
 *
 * Works the same for any bar interval. Volume is not consulted: a fill is never partial or capped
 * by what traded (the report states this).
 */
class NextBarFillModel {
public:
  /**
   * @brief Constructs a model with a flat commission.
   * @param commission The commission model, charged on every execution.
   * @param slippage The slippage model.
   * @param limit_price Which price a filled limit order is credited.
   */
  explicit NextBarFillModel(
    CommissionModel commission = CommissionModel::none(),
    SlippageModel slippage = SlippageModel::none(),
    LimitFillPrice limit_price = LimitFillPrice::AT_LIMIT
  )
  : commission_(CommissionPolicy::stated(std::move(commission))),
    slippage_(std::move(slippage)),
    limit_price_(limit_price) {}

  /**
   * @brief Constructs a model with a commission policy, which may resolve a schedule per execution.
   * @param commission The commission policy.
   * @param slippage The slippage model.
   * @param limit_price Which price a filled limit order is credited.
   */
  explicit NextBarFillModel(
    CommissionPolicy commission,
    SlippageModel slippage = SlippageModel::none(),
    LimitFillPrice limit_price = LimitFillPrice::AT_LIMIT
  )
  : commission_(std::move(commission)),
    slippage_(std::move(slippage)),
    limit_price_(limit_price) {}

  /**
   * @brief Returns the commission policy.
   * @return The policy in force.
   */
  const CommissionPolicy& commission() const noexcept { return commission_; }

  /**
   * @brief Returns the slippage model.
   * @return The model in force.
   */
  const SlippageModel& slippage() const noexcept { return slippage_; }

  /**
   * @brief Returns which price a filled limit order is credited.
   * @return The choice.
   */
  LimitFillPrice limit_price() const noexcept { return limit_price_; }

  /**
   * @brief Returns a copy of this model that charges a tax on sales.
   *
   * The library has no tax rate of its own (Korean securities transaction tax changes yearly); one
   * rate applies to the whole replay. The tax is truncated to whole currency units and reported apart
   * from commission (`Execution::tax_`, `Report::tax_paid_`). Only sales of instruments in
   * `tax.country_` pay it; exempt instruments (Korean ETFs and ETNs) need a replay without it.
   *
   * @param tax The rate and market.
   * @return The new model.
   * @throws std::invalid_argument if the rate is below 0 or not below 1.
   */
  NextBarFillModel with_sell_tax(SellTax tax) const {
    if (decimal_less(tax.rate_, Decimal64::parse("0")) || !decimal_less(tax.rate_, Decimal64::parse("1"))) {
      throw std::invalid_argument("a sell tax rate must be at least 0 and below 1");
    }
    auto model = *this;
    model.sell_tax_ = std::move(tax);
    return model;
  }

  /**
   * @brief Returns the tax on sales.
   * @return The tax, or `std::nullopt` if none is modelled.
   */
  const std::optional<SellTax>& sell_tax() const noexcept { return sell_tax_; }

  /**
   * @brief Tests whether an order that did not fill stays queued for a later bar of its session.
   *
   * Only `DAY` limit and stop-limit orders rest; immediate-or-cancel, fill-or-kill and market orders
   * do not. `Backtester` withdraws a resting `DAY` order when its session ends (on daily bars, after
   * one bar).
   *
   * @param order The order.
   * @return `true` if it rests.
   */
  static bool rests_unfilled(const strategy::PlannedOrder& order) noexcept {
    if (order.time_in_force_ != TimeInForce::DAY) {
      return false;
    }
    // A stop rests while dormant and, once released, as the limit it has become.
    return order.type_ == OrderType::LIMIT || order.type_ == OrderType::STOP_LIMIT;
  }

  /**
   * @brief Decides whether an order trades against a bar, and at what price and cost.
   *
   * - **Market**: the bar's open (its close if it has no open).
   * - **Limit**: only if the bar reached the limit (low at or below a buy limit, high at or above a
   *   sell limit). Credited the limit, or under `AT_OPEN_IF_BETTER` the open if better; the credit is
   *   kept within the bar's range and never through the limit, even after slippage.
   * - **Stop-limit**: dormant until the bar reaches the trigger (a sell stop when the low reaches it,
   *   a buy stop when the high does). Released at the open, it is priced as a limit. Released inside
   *   the bar, it fills only when the bar proves a price after the release: at the worst bar price the
   *   limit allows if the limit was marketable at the trigger, at the limit if the close is through
   *   the limit; otherwise it rests. No favourable slippage applies after an intrabar release.
   *
   * Slippage, then commission and any sale tax, are applied to a fill. The execution is stamped at
   * `next.period_end_`.
   *
   * @param order The queued order.
   * @param next The bar after the one the order was decided on.
   * @param rule The instrument's price grid, for tick slippage.
   * @param date The market-local trading date of the fill, for the commission rate (see
   *   `SessionWindow::local_date`).
   * @param open_date The market-local date of the bar's open, if known; used instead of `date` for a
   *   fill priced at the open. For a fill inside a multi-day bar `date` is used, which can be a later
   *   day than the fill.
   * @param released Whether an earlier bar already released this stop-limit order.
   * @return The decision. A stop-limit order without both a trigger and a limit price neither trades
   *   nor rests.
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
        // Not a stop this model can release (a caller driving the model directly).
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
          // Dated from the open whatever the price policy credits: it traded at the open.
          if (reference && limit_marketable_at(order, *next.bar_.open_)) {
            at_open = true;
          }
          break;
        case Release::INTRABAR:
          decision.released_ = true;
          reference = intrabar_release_price(order, next);
          if (!reference) {
            // The bar cannot show the price reached the limit after the release. Rests for the rest of
            // its session (on daily bars, it expires unfilled).
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
      // A limit the bar never reached stays queued for the rest of its session.
      decision.still_working_ = rests_unfilled(order);
      return decision;
    }

    // A limit fill is credited a price its bar traded and never one through the limit: the bound is a
    // sell's max(low, limit) or a buy's min(high, limit), applied before and after slippage.
    std::optional<Decimal64> bound;
    const bool limit_basis = order.type_ == OrderType::LIMIT || order.type_ == OrderType::STOP_LIMIT;
    if (limit_basis && order.price_) {
      const auto& limit = *order.price_;
      if (order.side_ == OrderSide::SELL) {
        bound = decimal_less(limit, next.bar_.low_) ? next.bar_.low_ : limit;
        if (decimal_less(*reference, *bound)) {
          reference = bound;
          decision.bounded_ = true;
        }
      } else {
        bound = decimal_less(next.bar_.high_, limit) ? next.bar_.high_ : limit;
        if (decimal_less(*bound, *reference)) {
          reference = bound;
          decision.bounded_ = true;
        }
      }
    }
    auto slipped = slippage_.apply(*reference, order.side_, rule);
    if (slipped && decision.intrabar_release_fill_) {
      // No favourable slippage after an intrabar release: the bar's other extreme may have come first.
      const bool favourable = order.side_ == OrderSide::SELL ?
        decimal_less(*reference, slipped->first) :
        decimal_less(slipped->first, *reference);
      if (favourable) {
        slipped->first = *reference;
        slipped->second = Decimal64::parse("0");
      }
    }
    if (slipped && bound) {
      // Negative slippage cannot carry a fill past the bar's opposite extreme either.
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
          decision.bounded_ = true;
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
          decision.bounded_ = true;
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
      // A cost that cannot be resolved halts the engine; it is neither a non-fill nor silent.
      decision.costing_error_ = std::move(charged.error());
      return decision;
    }

    // The tax on a sale, where a rate was stated: of the sale's value, truncated to whole units.
    std::optional<Decimal64> tax;
    if (sell_tax_ && order.side_ == OrderSide::SELL && order.instrument_.country_ == sell_tax_->country_) {
      const auto value = decimal_multiply(order.quantity_, slipped->first);
      const auto charged = value ? decimal_multiply(*value, sell_tax_->rate_) : std::nullopt;
      tax = charged ? decimal_floor_to_multiple(*charged, Decimal64::parse("1")) : std::nullopt;
      if (!tax) {
        // Not a tax of zero: the cost is unknown, so the engine halts.
        BrokerError error;
        error.category_ = ErrorCategory::INVALID_ARGUMENT;
        error.reached_broker_ = false;
        error.broker_code_ = "tax_not_representable";
        error.message_ = "the tax on that sale does not fit an exact decimal";
        decision.costing_error_ = std::move(error);
        return decision;
      }
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
    if (tax) {
      execution.tax_ = *tax;
    }
    decision.execution_ = std::move(execution);
    return decision;
  }

private:
  // Where in a bar a dormant stop released, if it did.
  enum class Release {
    // The bar never reached the trigger.
    NONE,
    // The open was already through the trigger, so the whole bar followed the release.
    AT_OPEN,
    // The bar's extreme reached the trigger and its open did not.
    INTRABAR,
  };

  // Returns whether, and where, `next` released a dormant stop.
  //
  // **A sell stop releases when the price falls to its trigger and a buy stop when it rises to it.**
  // That is an assumption about the broker -- neither KIS nor Kiwoom documents the direction -- and
  // it is the universal convention. `ReportCaveats::stop_releases_` counts how often it was used.
  //
  // A bar with no open cannot establish a release *at* the open, so a release there is intrabar,
  // which is the answer that credits less.
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

  // Returns whether the order's limit would trade at `price` -- a sell at or above it, a buy at or below.
  static bool limit_marketable_at(const strategy::PlannedOrder& order, const Decimal64& price) {
    const auto& limit = *order.price_;
    return order.side_ == OrderSide::SELL ? !decimal_less(price, limit) : !decimal_less(limit, price);
  }

  // Returns what a stop released inside `next` is credited with, or nothing when the bar cannot show
  // it traded after the release.
  //
  // At the release the price *was* the trigger. Two cases, using only prices the bar proves came after
  // it -- the trigger itself and the close, which is always last:
  //
  // - **Marketable at the trigger** (a sell limited at or below it, a buy at or above): it trades. It is
  //   credited the **worst price the bar traded that the limit allows** -- a sell the higher of its
  //   limit and the bar's low, a buy the lower of its limit and the bar's high -- so the price is inside
  //   the bar and never through the limit. Crediting the limit itself could name a price the bar never
  //   reached; crediting the trigger would assume the book was there.
  // - **Not marketable at the trigger** (a sell limited above it): it trades only if the price came back
  //   to the limit after the release. The close proves that when it is at or through the limit; then it
  //   is credited the limit. Otherwise the bar's other extreme may have come first, and nothing is.
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

  // Returns the price before slippage, or nothing when the order does not trade against this bar.
  //
  // A market order takes the open. A limit trades only if the bar's range reached it — a buy needs the
  // low at or below the limit, a sell needs the high at or above it — because a bar that never touched
  // the price is a bar in which the order sat unfilled, however close it came.
  //
  // Sets `at_open` when the price is the bar's stated open -- a market order, or a limit under
  // `AT_OPEN_IF_BETTER` whose open was better -- which is what lets `decide` date the charge from the
  // open's day. False for a bar with no open, whose close stands in for it.
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
  std::optional<SellTax> sell_tax_;
};

} // namespace backtest
} // namespace algotradelib
