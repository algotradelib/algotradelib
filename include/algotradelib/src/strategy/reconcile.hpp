#pragma once

#include "./intent.hpp"
#include "./portfolio.hpp"

#include <algotradelib/core>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * Turning what a strategy asked for into what the broker will be told.
 *
 * A pure function of the intent, the portfolio, and the instrument's reference data. It performs
 * no I/O and holds no state, which is what lets the same arithmetic serve a dry run, a live
 * session, and later a backtest — and what makes every case below testable without a broker.
 *
 * **The problem it exists to solve is the projected position.** A strategy that says "hold 100"
 * must not produce a second order for 100 when 40 are filled and 60 are still working. So the
 * comparison is never against the position alone; it is against the position *plus everything
 * already in the market*, which is why `Portfolio::working_quantity` exists and why
 * `TargetPosition` is the form most strategies should emit.
 *
 * Four adjustments happen here rather than in a strategy, because each needs data a strategy
 * should not have to collect: rounding a price onto the tick grid, rounding a quantity onto the
 * lot, clamping a price into today's permitted band, and dropping an instruction that would
 * change nothing. None of them is silent — every adjustment is recorded on the planned order.
 */

/** Why an instruction could not be produced. */
enum class RejectReason {
  /** No reference data, so neither the tick grid nor the lot size is known. */
  NO_REFERENCE_DATA,
  /** The instrument's tick grid is unknown, so a limit price cannot be made valid. */
  UNKNOWN_PRICE_GRID,
  /** A price was needed to size the order and none was available. */
  NO_PRICE_AVAILABLE,
  /** Sizing needed the portfolio's value and none was supplied. */
  NO_PORTFOLIO_VALUE,
  /** The quantity rounded to zero on the lot, so there is nothing to send. */
  QUANTITY_ROUNDS_TO_ZERO,
  /** The arithmetic overflowed the exact decimal range. */
  NOT_REPRESENTABLE,
  /** A limit price fell outside today's band and the band leaves no valid price. */
  NO_PERMITTED_PRICE,
};

/** Returns a stable lowercase name for `reason`, for logs and structured reporting. */
inline constexpr std::string_view reject_reason_name(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::NO_REFERENCE_DATA:
      return "no_reference_data";
    case RejectReason::UNKNOWN_PRICE_GRID:
      return "unknown_price_grid";
    case RejectReason::NO_PRICE_AVAILABLE:
      return "no_price_available";
    case RejectReason::NO_PORTFOLIO_VALUE:
      return "no_portfolio_value";
    case RejectReason::QUANTITY_ROUNDS_TO_ZERO:
      return "quantity_rounds_to_zero";
    case RejectReason::NOT_REPRESENTABLE:
      return "not_representable";
    case RejectReason::NO_PERMITTED_PRICE:
      return "no_permitted_price";
  }
  return "unknown";
}

/** One instruction that was refused, and why. */
struct Rejection {
  RejectReason reason_{RejectReason::NO_REFERENCE_DATA};
  InstrumentId instrument_;
  /** The strategy's own label on the refused instruction, for attribution. */
  std::string tag_;
};

/**
 * One order the runner is to place.
 *
 * The three `*_adjusted_` flags record what reconciliation changed relative to what the strategy
 * asked for. They exist because an adjustment a caller cannot see is indistinguishable from a bug:
 * a strategy whose limit was clamped on a limit-up day traded at a price it did not choose, and it
 * is entitled to know.
 */
struct PlannedOrder {
  InstrumentId instrument_;
  OrderSide side_{OrderSide::BUY};
  OrderType type_{OrderType::LIMIT};
  TimeInForce time_in_force_{TimeInForce::DAY};
  Decimal64 quantity_;
  std::optional<Decimal64> price_;
  std::string tag_;
  std::optional<std::string> client_order_id_;

  /** Whether the price was moved onto the tick grid. */
  bool price_rounded_{false};
  /** Whether the price was moved into today's permitted band. */
  bool price_clamped_{false};
  /** Whether the quantity was moved onto the lot. */
  bool quantity_rounded_{false};
};

/**
 * What the runner should do, in this order: cancel first, then place.
 *
 * The order matters and is the caller's obligation. A replacement placed before its predecessor
 * is withdrawn is briefly a double position; a cancellation issued first leaves the instrument
 * unrepresented for a moment instead, which is the lesser exposure and the one a strategy can
 * reason about.
 */
template <typename Broker>
struct Plan {
  std::vector<OrderRef<Broker>> cancel_;
  std::vector<PlannedOrder> place_;
  std::vector<Rejection> rejected_;

  /** Returns whether the plan asks for nothing at all. */
  bool empty() const noexcept {
    return cancel_.empty() && place_.empty() && rejected_.empty();
  }
};

/**
 * Everything reconciliation needs that is not the intent or the portfolio.
 *
 * Gathered by the runner, which has the caches; passed in so the arithmetic stays a pure
 * function. Each member is optional because a broker may not have published it, and the
 * rejections above say which absence stopped which instruction rather than letting a default
 * stand in for a fact.
 */
struct ReconcileInputs {
  /** Reference data, carrying the tick grid and the lot size. */
  const Instrument* instrument_{nullptr};
  /** A price to size a notional or a weight against; the last trade or a mid. */
  std::optional<Decimal64> reference_price_;
  /** Today's permitted band, where the broker publishes one. */
  std::optional<PriceLimits> limits_;
  /** Total portfolio value in the instrument's currency, for weight sizing. */
  std::optional<Decimal64> portfolio_value_;

  /**
   * Smallest shortfall worth an instruction, in shares. Absent means chase every share.
   *
   * An execution-cost policy, which is why it arrives from the runner rather than from the strategy:
   * a strategy states the position it wants, and how finely that is chased depends on commission,
   * on whether the venue charges per instruction, and on what queue priority is worth -- none of
   * which a signal knows.
   *
   * **Absent by default, which is exactly the behaviour before this existed.** No number is invented
   * here. A tolerance guessed at would silently hold back orders a strategy asked for, and an
   * invisible shortfall is worse than visible churn.
   *
   * What it prevents when set: a target drifting from 100 to 99 cancels a working order for 100 and
   * places a new one for 99, spending two instructions and whatever queue priority the first had, to
   * move the position by one share.
   */
  std::optional<Decimal64> minimum_trade_;
};

namespace reconcile_detail {

/** Returns the lot size to round quantities onto, or nothing when none is known. */
inline std::optional<Decimal64> lot_of(const Instrument& instrument) {
  if (!instrument.lot_size_) {
    return std::nullopt;
  }
  if (instrument.lot_size_->coefficient() <= 0) {
    return std::nullopt;
  }
  return instrument.lot_size_;
}

/**
 * Rounds `quantity` **down** onto `lot`.
 *
 * Down, always. Rounding a quantity up would trade more than the strategy asked for, which is a
 * larger position than it sized and a larger loss than it budgeted. Rounding down trades less,
 * which is a shortfall the next reconciliation sees and can act on.
 *
 * An absent lot applies no rounding, which is the honest answer rather than assuming one. The
 * asymmetry with `PriceRule::UNKNOWN` — which refuses outright — is deliberate: an off-grid price
 * is a certain rejection by the exchange, whereas a quantity of one share is valid under every
 * lot size there is, so refusing all trading for want of a lot would forbid orders that are fine.
 */
inline std::optional<Decimal64> round_to_lot(
  const Decimal64& quantity,
  const std::optional<Decimal64>& lot,
  bool& adjusted
) {
  adjusted = false;
  if (!lot) {
    return quantity;
  }
  const auto rounded = decimal_floor_to_multiple(quantity, *lot);
  if (!rounded) {
    return std::nullopt;
  }
  adjusted = !(*rounded == quantity);
  return rounded;
}

/**
 * Moves `price` into today's permitted band, in the direction that reduces aggression.
 *
 * A buy above the upper bound is clamped down to it and a sell below the lower bound is clamped up
 * to it. Both make the order less willing than instructed, which is the same rule tick rounding
 * follows, and both turn an order the exchange would certainly reject into one it will accept.
 *
 * A buy *below* the lower bound and a sell *above* the upper are left alone: they are valid
 * prices that simply will not fill today, and moving them would be inventing a willingness the
 * strategy never expressed.
 */
inline std::optional<Decimal64> clamp_to_limits(
  const Decimal64& price,
  OrderSide side,
  const std::optional<PriceLimits>& limits,
  bool& adjusted
) {
  adjusted = false;
  if (!limits) {
    return price;
  }
  if (side == OrderSide::BUY && limits->upper_ && decimal_less(*limits->upper_, price)) {
    adjusted = true;
    return limits->upper_;
  }
  if (side == OrderSide::SELL && limits->lower_ && decimal_less(price, *limits->lower_)) {
    adjusted = true;
    return limits->lower_;
  }
  return price;
}

/**
 * Prepares a limit price: clamped into the band, then rounded onto the grid.
 *
 * In that order, because clamping can land on a price the grid does not carry — a published bound
 * need not be a tick multiple — whereas rounding a price that is already inside the band keeps it
 * inside, since rounding moves it away from the bound it was clamped to.
 */
inline std::optional<Decimal64> prepare_price(
  const Decimal64& wanted,
  OrderSide side,
  const PriceRule& rule,
  const std::optional<PriceLimits>& limits,
  bool& clamped,
  bool& rounded
) {
  const auto within = clamp_to_limits(wanted, side, limits, clamped);
  if (!within) {
    return std::nullopt;
  }
  const auto on_grid = rule.round_to_tick(*within, side);
  if (!on_grid) {
    return std::nullopt;
  }
  rounded = !(*on_grid == *within);
  return on_grid;
}

} // namespace reconcile_detail

/**
 * Reconciles one target position into the smallest set of instructions that reaches it.
 *
 * The arithmetic, in order:
 *
 * 1. **What is still needed** is the target minus what is held. Working orders are not subtracted
 *    yet, because their side matters.
 * 2. **Orders working the wrong way are cancelled.** Buying toward a target while a sell is
 *    resting on the same instrument works against itself, and a target of exactly the held
 *    quantity means every working order is now unwanted.
 * 3. **What is left to send** is the need minus what is already working on the right side. This is
 *    the step that stops a partial fill producing a second full-size order.
 * 4. **Over-ordering cancels and re-places.** When more is working than is now wanted, the whole
 *    side is withdrawn and the remainder sent fresh, rather than partially cancelled — partial
 *    cancellation is a capability only one of the two brokers has, and cancel-then-replace is
 *    correct on both.
 *
 * A target already met produces an empty plan, which is the common case and must cost nothing.
 */
template <typename Broker>
Plan<Broker> reconcile(
  const TargetPosition& target,
  const Portfolio<Broker>& portfolio,
  const ReconcileInputs& inputs
) {
  Plan<Broker> plan;
  const auto reject = [&](RejectReason reason) {
    plan.cancel_.clear();
    plan.place_.clear();
    plan.rejected_.push_back(Rejection{reason, target.instrument_, target.tag_});
    return plan;
  };

  if (inputs.instrument_ == nullptr) {
    return reject(RejectReason::NO_REFERENCE_DATA);
  }

  const auto zero = Decimal64::parse("0");
  const auto held = portfolio.quantity_of(target.instrument_).value_or(zero);
  const auto needed = decimal_subtract(target.target_quantity_, held);
  if (!needed) {
    return reject(RejectReason::NOT_REPRESENTABLE);
  }

  const bool buying = decimal_less(zero, *needed);
  const bool selling = decimal_less(*needed, zero);

  // Step 2: withdraw what works against the target. With nothing needed, that is everything.
  //
  // Only orders that can still trade are withdrawn. A snapshot may carry an order that has since
  // filled or been cancelled, and asking a broker to cancel one of those is a request that can
  // only fail -- noise on a path where a real cancellation failure has to be noticed.
  for (const auto& working : portfolio.working_) {
    if (!(working.order_.instrument_ == target.instrument_) ||
        !order_is_open(working.order_.status_)) {
      continue;
    }
    const bool opposes = (buying && working.order_.side_ == OrderSide::SELL) ||
      (selling && working.order_.side_ == OrderSide::BUY) || (!buying && !selling);
    if (opposes) {
      plan.cancel_.push_back(working.ref_);
    }
  }
  if (!buying && !selling) {
    return plan;
  }

  const OrderSide side = buying ? OrderSide::BUY : OrderSide::SELL;
  const auto magnitude = buying ? *needed : decimal_subtract(zero, *needed).value_or(zero);
  const auto already = portfolio.working_quantity(target.instrument_, side);

  // Step 3: only the part not already in the market.
  auto remaining = decimal_subtract(magnitude, already);
  if (!remaining) {
    return reject(RejectReason::NOT_REPRESENTABLE);
  }

  // Step 3a: a drift smaller than the tolerance is not worth an instruction.
  //
  // Measured on the *drift* -- how far the in-flight position is from the target -- and not on what
  // would be sent. Those differ exactly in the case the tolerance exists for: 100 working against a
  // target of 99 would withdraw the side and re-place 99, so the quantity sent is 99 while the drift
  // is one share.
  //
  // Suppressed only when nothing has already been queued for cancellation. An order working the
  // *wrong way* still has to be withdrawn however small the drift, because leaving it resting works
  // against the target rather than merely failing to reach it. And returning an empty plan rather
  // than the accumulated one matters for the same reason: withdrawing a working order and then
  // declining to replace it would leave the position further from the target than doing nothing.
  if (inputs.minimum_trade_ && plan.cancel_.empty()) {
    const auto drift = decimal_less(*remaining, zero) ?
      decimal_subtract(zero, *remaining) :
      std::optional<Decimal64>(*remaining);
    if (drift && decimal_less(*drift, *inputs.minimum_trade_)) {
      return Plan<Broker>{};
    }
  }

  // Step 4: more working than wanted, so withdraw the side and send the whole remainder.
  if (decimal_less(*remaining, zero)) {
    for (const auto& working : portfolio.working_) {
      if (working.order_.instrument_ == target.instrument_ &&
          working.order_.side_ == side && order_is_open(working.order_.status_)) {
        plan.cancel_.push_back(working.ref_);
      }
    }
    remaining = magnitude;
  }
  if (remaining->coefficient() == 0) {
    return plan;
  }

  PlannedOrder order;
  order.instrument_ = target.instrument_;
  order.side_ = side;
  order.time_in_force_ = TimeInForce::DAY;
  order.tag_ = target.tag_;

  const auto lot = reconcile_detail::lot_of(*inputs.instrument_);
  const auto sized = reconcile_detail::round_to_lot(*remaining, lot, order.quantity_rounded_);
  if (!sized) {
    return reject(RejectReason::NOT_REPRESENTABLE);
  }
  if (sized->coefficient() == 0) {
    // Rounded away entirely. A rejection rather than an empty plan, because the strategy asked
    // for something and is getting nothing; silence here would look like a met target.
    return reject(RejectReason::QUANTITY_ROUNDS_TO_ZERO);
  }
  order.quantity_ = *sized;

  if (target.limit_) {
    const auto priced = reconcile_detail::prepare_price(
      target.limit_->value(),
      side,
      inputs.instrument_->price_rule_,
      inputs.limits_,
      order.price_clamped_,
      order.price_rounded_
    );
    if (!priced) {
      return reject(RejectReason::UNKNOWN_PRICE_GRID);
    }
    order.type_ = OrderType::LIMIT;
    order.price_ = priced;
  } else {
    order.type_ = OrderType::MARKET;
  }

  plan.place_.push_back(std::move(order));
  return plan;
}

/**
 * Reconciles one order intent into an instruction, without diffing against the portfolio.
 *
 * An intent says "trade this much", not "hold this much", so the portfolio is deliberately not
 * consulted: a strategy that emits intents has taken responsibility for its own sizing. What still
 * happens here is the part that needs reference data — converting a notional or a weight into a
 * quantity, rounding onto the lot and the tick, and clamping into today's band.
 *
 * A strategy that wants the double-sizing protection emits a `TargetPosition` instead. That choice
 * is the whole reason both forms exist.
 */
template <typename Broker>
Plan<Broker> reconcile(
  const OrderIntent& intent,
  const Portfolio<Broker>& portfolio,
  const ReconcileInputs& inputs
) {
  (void)portfolio;
  Plan<Broker> plan;
  const auto reject = [&](RejectReason reason) {
    plan.place_.clear();
    plan.rejected_.push_back(Rejection{reason, intent.instrument(), intent.tag()});
    return plan;
  };

  if (inputs.instrument_ == nullptr) {
    return reject(RejectReason::NO_REFERENCE_DATA);
  }

  // A price to size against: the limit if there is one, otherwise whatever the runner supplied.
  const auto sizing_price = intent.limit() ?
    std::optional<Decimal64>(intent.limit()->value()) :
    inputs.reference_price_;

  Decimal64 wanted;
  switch (intent.size().kind()) {
    case SizeKind::QUANTITY:
      wanted = intent.size().as_quantity();
      break;
    case SizeKind::NOTIONAL: {
      if (!sizing_price || sizing_price->coefficient() == 0) {
        return reject(RejectReason::NO_PRICE_AVAILABLE);
      }
      const auto shares =
        decimal_divide(intent.size().as_notional().amount_, *sizing_price, 0);
      if (!shares) {
        return reject(RejectReason::NOT_REPRESENTABLE);
      }
      wanted = *shares;
      break;
    }
    case SizeKind::TARGET_WEIGHT: {
      if (!inputs.portfolio_value_) {
        return reject(RejectReason::NO_PORTFOLIO_VALUE);
      }
      if (!sizing_price || sizing_price->coefficient() == 0) {
        return reject(RejectReason::NO_PRICE_AVAILABLE);
      }
      const auto allocation = decimal_multiply(
        inputs.portfolio_value_.value(), intent.size().as_target_weight()
      );
      if (!allocation) {
        return reject(RejectReason::NOT_REPRESENTABLE);
      }
      const auto shares = decimal_divide(*allocation, *sizing_price, 0);
      if (!shares) {
        return reject(RejectReason::NOT_REPRESENTABLE);
      }
      wanted = *shares;
      break;
    }
  }

  PlannedOrder order;
  order.instrument_ = intent.instrument();
  order.side_ = intent.side();
  order.time_in_force_ = intent.time_in_force();
  order.tag_ = intent.tag();
  order.client_order_id_ = intent.client_order_id();

  const auto lot = reconcile_detail::lot_of(*inputs.instrument_);
  const auto sized = reconcile_detail::round_to_lot(wanted, lot, order.quantity_rounded_);
  if (!sized) {
    return reject(RejectReason::NOT_REPRESENTABLE);
  }
  if (sized->coefficient() == 0) {
    return reject(RejectReason::QUANTITY_ROUNDS_TO_ZERO);
  }
  order.quantity_ = *sized;

  if (intent.limit()) {
    const auto priced = reconcile_detail::prepare_price(
      intent.limit()->value(),
      intent.side(),
      inputs.instrument_->price_rule_,
      inputs.limits_,
      order.price_clamped_,
      order.price_rounded_
    );
    if (!priced) {
      return reject(RejectReason::UNKNOWN_PRICE_GRID);
    }
    order.type_ = OrderType::LIMIT;
    order.price_ = priced;
  } else {
    order.type_ = OrderType::MARKET;
  }

  plan.place_.push_back(std::move(order));
  return plan;
}

} // namespace strategy
} // namespace algotradelib
