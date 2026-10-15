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
 * @file
 * @brief Reconciliation: turning a strategy's intents and targets into the orders and
 *   cancellations to send.
 *
 * The `reconcile` functions are pure: no I/O and no state. A target is compared with the position
 * **plus the orders already working**, so restating it does not order twice. A buy above the day's
 * band is clamped down to it and a sell below it up (a less aggressive price is left as it is), prices
 * are rounded onto the tick grid, and quantities rounded down onto the lot; each
 * adjustment is flagged on the `PlannedOrder`.
 */

/**
 * @brief Why an instruction was not placed.
 *
 * Most reasons mean reconciliation could not build a valid instruction. `BROKER_REFUSED` and
 * `BINDING_REFUSED` mean it was built and then refused.
 */
enum class RejectReason {
  /** @brief No reference data for the instrument, so neither the tick grid nor the lot is known. */
  NO_REFERENCE_DATA,
  /** @brief The instrument's tick grid is unknown, so a limit price cannot be made valid. */
  UNKNOWN_PRICE_GRID,
  /** @brief A price was needed to size the order and none was available. */
  NO_PRICE_AVAILABLE,
  /** @brief A weight needed the portfolio's value, and none (or a non-positive one, or one in another currency) was supplied. */
  NO_PORTFOLIO_VALUE,
  /** @brief The quantity rounded down to zero on the lot. */
  QUANTITY_ROUNDS_TO_ZERO,
  /** @brief The decimal arithmetic did not fit, or the portfolio's rows did not add up. */
  NOT_REPRESENTABLE,
  /**
   * @brief Rounding the limit onto the grid, in the cautious direction, would move it out of the
   *   day's band: no tick lies between the band's bound and the limit (clamped into the band).
   *
   * Only a band narrower than one tick, at a price of a few won on KRX, does this. A limit the
   * strategy itself set beyond the band (a buy below the lower bound, a sell above the upper) is
   * left alone, as without a band.
   */
  NO_PERMITTED_PRICE,
  /** @brief The broker refused the instruction (a request was sent). */
  BROKER_REFUSED,
  /**
   * @brief The binding refused to send the instruction, so no request was made.
   *
   * For example, an idempotency key at a broker that accepts none, or a cancellation of an order the
   * broker has no reference for.
   */
  BINDING_REFUSED,
  /**
   * @brief A stop-limit order's trigger price is not on the instrument's tick grid.
   *
   * Unlike a limit price, a trigger is never rounded, since rounding would change when the order
   * is released.
   */
  TRIGGER_OFF_GRID,
  /**
   * @brief A target needs a trade while a stop order rests on the instrument; the stop is cancelled
   *   first and nothing is placed this pass.
   *
   * `runner::Runner` keeps the target and settles it again once no stop is listed on the
   * instrument (see `Runner::deferred_targets`); a newer instruction for the instrument replaces it.
   * A target the holding already meets is not refused, and the stop stays.
   */
  STOP_ORDER_RESTING,
  /**
   * @brief A weight-sized intent with something a holding cannot express: a sell, a negative
   *   weight, a trigger, a validity other than `DAY`, or an idempotency key.
   */
  UNSUPPORTED_WEIGHT,
  /**
   * @brief An intent's size is negative: a quantity or a notional amount below zero.
   *
   * An intent carries its direction in its side, so a negative size is a sign error, and an order of -5
   * shares is not a sell of 5. With the inputs it needs available, a zero quantity or notional is
   * `QUANTITY_ROUNDS_TO_ZERO`; a negative weight is `UNSUPPORTED_WEIGHT`.
   */
  NEGATIVE_SIZE,
};

/**
 * @brief Returns a lowercase name for a reason, such as `"no_reference_data"`, for logs.
 * @param reason The reason.
 * @return The name.
 */
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
    case RejectReason::BROKER_REFUSED:
      return "broker_refused";
    case RejectReason::BINDING_REFUSED:
      return "binding_refused";
    case RejectReason::TRIGGER_OFF_GRID:
      return "trigger_off_grid";
    case RejectReason::STOP_ORDER_RESTING:
      return "stop_order_resting";
    case RejectReason::UNSUPPORTED_WEIGHT:
      return "unsupported_weight";
    case RejectReason::NEGATIVE_SIZE:
      return "negative_size";
  }
  return "unknown";
}

/** @brief One instruction that was not placed, and why. */
struct Rejection {
  /** @brief Why it was not placed. */
  RejectReason reason_{RejectReason::NO_REFERENCE_DATA};
  /** @brief The instrument it was for. */
  InstrumentId instrument_;
  /** @brief The tag of the intent or target it came from. */
  std::string tag_;
};

/**
 * @brief One order the runner is to place.
 *
 * The `price_rounded_`, `price_clamped_` and `quantity_rounded_` flags say what reconciliation
 * changed from what the strategy asked for.
 */
struct PlannedOrder {
  /** @brief The instrument to trade. */
  InstrumentId instrument_;
  /** @brief The side. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The order type. */
  OrderType type_{OrderType::LIMIT};
  /** @brief How long the order stays valid. */
  TimeInForce time_in_force_{TimeInForce::DAY};
  /** @brief The trading session. */
  OrderSession session_{OrderSession::REGULAR};
  /**
   * @brief Where to route the order; see `OrderRequest::venue_`.
   *
   * Strategies do not set it; a live runner fills in its binding's route before sending.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** @brief The quantity, after rounding onto the lot. */
  Decimal64 quantity_;
  /** @brief The limit price, after clamping and rounding; empty for a market order. */
  std::optional<Decimal64> price_;
  /** @brief The trigger price of a `STOP_LIMIT` order, exactly as stated; empty otherwise. */
  std::optional<Decimal64> trigger_price_;
  /** @brief The tag of the intent or target the order came from. */
  std::string tag_;
  /** @brief The idempotency key, if the intent carried one. */
  std::optional<std::string> client_order_id_;

  /** @brief Whether the price was rounded onto the tick grid. */
  bool price_rounded_{false};
  /** @brief Whether the price was clamped to the day's band (an aggressive limit beyond it). */
  bool price_clamped_{false};
  /**
   * @brief Whether a price band was known but not applied (too old), and would have clamped this
   *   price.
   *
   * Set by `runner::Runner`, not by reconciliation; see `Runner::set_maximum_price_limits_age`.
   */
  bool price_would_have_been_clamped_{false};
  /** @brief Whether the quantity was rounded down onto the lot. */
  bool quantity_rounded_{false};
};

/**
 * @brief What to send: first every cancellation, then every placement.
 *
 * Send the cancellations first; placing a replacement before its predecessor is withdrawn would
 * briefly double the position.
 *
 * @tparam Broker The broker tag of the order references.
 */
template <typename Broker>
struct Plan {
  /** @brief The orders to cancel, sent first. */
  std::vector<OrderRef<Broker>> cancel_;
  /** @brief The orders to place, after the cancellations. */
  std::vector<PlannedOrder> place_;
  /** @brief The instructions not placed, with reasons. */
  std::vector<Rejection> rejected_;

  /**
   * @brief Tests whether the plan is empty.
   * @return `true` if there is nothing to cancel, place or report.
   */
  bool empty() const noexcept {
    return cancel_.empty() && place_.empty() && rejected_.empty();
  }
};

/**
 * @brief How small a difference from a target is not worth an order.
 *
 * Every floor is optional and absent by default, which trades every share of drift. If any stated
 * floor is met, no order is sent:
 *
 * | Floor | Compares | Use it for |
 * | --- | --- | --- |
 * | `minimum_quantity_` | the drift in shares | lot economics, a per-share fee |
 * | `minimum_notional_` | the drift's value at the reference price | a fixed or minimum fee per order |
 * | `minimum_fraction_of_working_` | the drift over the quantity already working | not giving up queue priority |
 *
 * A floor that cannot be evaluated (a notional floor without a price) does not suppress. A drift is
 * never suppressed when an order on the wrong side has to be cancelled.
 */
struct TradeTolerance {
  /** @brief The smallest drift worth trading, in shares. */
  std::optional<Decimal64> minimum_quantity_;
  /** @brief The smallest drift worth trading, valued at the reference price. */
  std::optional<Decimal64> minimum_notional_;
  /**
   * @brief The smallest drift worth trading, as a fraction of the quantity already working
   *   (`0.02` is 2%). Ignored when nothing is working.
   */
  std::optional<Decimal64> minimum_fraction_of_working_;

  /**
   * @brief Tests whether any floor is set.
   * @return `true` if at least one floor is present.
   */
  bool stated() const noexcept {
    return minimum_quantity_.has_value() || minimum_notional_.has_value() ||
      minimum_fraction_of_working_.has_value();
  }
};

/**
 * @brief What reconciliation needs besides the intent and the portfolio; the runner fills it in.
 *
 * A missing input refuses the instructions that need it, with a `RejectReason` that names it.
 */
struct ReconcileInputs {
  /** @brief The instrument's reference data (tick grid, lot size, currency); required. */
  const Instrument* instrument_{nullptr};
  /** @brief A price to size a notional or weight against, such as the last trade. */
  std::optional<Decimal64> reference_price_;
  /** @brief The day's price band, if the broker publishes one. */
  std::optional<PriceLimits> limits_;
  /** @brief The total portfolio value, for weight sizing. */
  std::optional<Decimal64> portfolio_value_;

  /** @brief How small a drift from a target is not worth an order. */
  TradeTolerance tolerance_;
  /**
   * @brief The currency of `portfolio_value_`; absent means the instrument's currency.
   *
   * A weight on an instrument in another currency is refused (`NO_PORTFOLIO_VALUE`).
   */
  std::optional<Currency> portfolio_value_currency_;
  /**
   * @brief Whether the portfolio's reserved rows (`WorkingOrder::reserved_`) are orders this runner
   *   sent that the holdings may not reflect yet, and which may already have filled.
   *
   * When set, a target that finds too much working cancels that side and sizes its replacement net
   * of those rows, so a filled reservation is not traded twice.
   */
  bool reserved_rows_are_unconfirmed_{false};
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
 * need not be a tick multiple — whereas rounding moves a price away from the bound it was clamped
 * to. It can still cross the band's other bound when no tick lies inside the band on that side;
 * `rounded_out_of_band` finds that case.
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

/**
 * Returns whether rounding, not the strategy, put a limit price outside the day's band.
 *
 * Rounding moves a buy down and a sell up, so the price can leave the band only below its lower
 * bound (a buy) or above its upper (a sell). That is rounding's doing only if the strategy's own
 * price, `wanted`, was not already beyond that bound.
 */
inline bool rounded_out_of_band(
  const Decimal64& wanted,
  const Decimal64& priced,
  OrderSide side,
  const std::optional<PriceLimits>& limits
) {
  if (!limits) {
    return false;
  }
  if (side == OrderSide::BUY) {
    return limits->lower_ && decimal_less(priced, *limits->lower_) && !decimal_less(wanted, *limits->lower_);
  }
  return limits->upper_ && decimal_less(*limits->upper_, priced) && !decimal_less(*limits->upper_, wanted);
}

/**
 * Returns whether `drift` is small enough that no instruction is worth issuing.
 *
 * Any stated floor being met is sufficient, because each expresses an independent reason not to trade.
 * A floor that cannot be evaluated -- a notional one with no price -- does not suppress: failing to
 * trade leaves an invisible shortfall, which is the worse of the two errors here.
 */
inline bool below_tolerance(
  const Decimal64& drift,
  const Decimal64& already_working,
  const ReconcileInputs& inputs
) {
  const auto& tolerance = inputs.tolerance_;

  if (tolerance.minimum_quantity_ && decimal_less(drift, *tolerance.minimum_quantity_)) {
    return true;
  }

  if (tolerance.minimum_notional_ && inputs.reference_price_) {
    const auto value = decimal_multiply(drift, *inputs.reference_price_);
    if (value && decimal_less(*value, *tolerance.minimum_notional_)) {
      return true;
    }
  }

  // Only meaningful against something resting: with nothing working there is no queue position to
  // give up, so a drift is simply a new order and this floor has nothing to say about it.
  if (tolerance.minimum_fraction_of_working_ && already_working.coefficient() > 0) {
    // Six places, which resolves a fraction far finer than any threshold an operator would state.
    const auto fraction = decimal_divide(drift, already_working, 6);
    if (fraction && decimal_less(*fraction, *tolerance.minimum_fraction_of_working_)) {
      return true;
    }
  }

  return false;
}

} // namespace reconcile_detail

/**
 * @brief Tests whether a working order is a stop order: a trigger-price type, or one carrying a
 *   trigger price.
 * @param order The order.
 * @return `true` for a stop order.
 */
inline bool is_stop_order(const OrderSnapshot& order) {
  return carries_trigger_price(order.type_) || order.trigger_price_.has_value();
}

/**
 * @brief Plans the cancellations and the order that move a holding to a target position.
 *
 * 1. The need is the target minus the net quantity held (every row).
 * 2. Open orders working against the need are cancelled; if the target is already met, every open
 *    order on the instrument is.
 * 3. What is already working on the needed side is subtracted from the need.
 * 4. If more is working than needed, that whole side is cancelled and the remainder placed fresh.
 * 5. The order is rounded down onto the lot, and its limit (if the target has one) clamped where it
 *    is more aggressive than the band allows, and rounded; without a limit it is a market order.
 *
 * When a stop order rests on the instrument, the steps above do not apply:
 * - if a trade is needed, the plan cancels the stop (unless its cancellation is already pending)
 *   and also carries a `RejectReason::STOP_ORDER_RESTING` rejection, with nothing placed;
 * - if the target is already met, the stop is kept and only the other open orders are cancelled.
 *
 * A drift below `ReconcileInputs::tolerance_` yields an empty plan, but only when nothing has to be
 * cancelled; an order working against the target is still cancelled.
 *
 * @tparam Broker The broker tag.
 * @param target The target position.
 * @param portfolio The positions and working orders.
 * @param inputs Reference data, prices, band and tolerance.
 * @return The plan. A refusal for any reason other than `STOP_ORDER_RESTING` clears the
 *   cancellations and placements and carries one `Rejection`; with `STOP_ORDER_RESTING` the
 *   cancellations stay, and the caller should send them.
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
  // Net across every position row, as the guards read it: a holding can be split across lots.
  const auto net = portfolio.net_quantity_of(target.instrument_);
  if (!net) {
    // A snapshot whose own rows will not add up cannot say what reaching a target would take, and
    // `value_or(zero)` here would read a broken listing as flat and order the whole target.
    return reject(RejectReason::NOT_REPRESENTABLE);
  }
  const auto held = *net;
  const auto needed = decimal_subtract(target.target_quantity_, held);
  if (!needed) {
    return reject(RejectReason::NOT_REPRESENTABLE);
  }

  const bool buying = decimal_less(zero, *needed);
  const bool selling = decimal_less(*needed, zero);

  // **Stops first; see `RejectReason::STOP_ORDER_RESTING`.** A row is a stop when its basis says so
  // or it carries a trigger -- an adapter records a positive trigger even on a row whose basis reads
  // otherwise, and that row may be a released stop.
  const auto is_stop = [](const OrderSnapshot& order) { return is_stop_order(order); };
  bool stop_resting = false;
  for (const auto& working : portfolio.working_) {
    if (working.order_.instrument_ == target.instrument_ &&
        order_is_open(working.order_.status_) && is_stop(working.order_)) {
      stop_resting = true;
    }
  }
  if (stop_resting) {
    const bool met = !buying && !selling;
    for (const auto& working : portfolio.working_) {
      if (!(working.order_.instrument_ == target.instrument_) ||
          !order_is_open(working.order_.status_)) {
        continue;
      }
      if (is_stop(working.order_)) {
        // Withdrawn only when a trade is needed, and not again once its withdrawal is pending: a
        // second request for an order already on its way out can only fail.
        if (!met && working.order_.status_ != OrderStatus::PENDING_CANCEL) {
          plan.cancel_.push_back(working.ref_);
        }
      } else if (met) {
        // The holding meets the target, so every *other* working order is unwanted, as before.
        plan.cancel_.push_back(working.ref_);
      }
    }
    if (!met) {
      plan.rejected_.push_back(
        Rejection{RejectReason::STOP_ORDER_RESTING, target.instrument_, target.tag_}
      );
    }
    return plan;
  }

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
  if (inputs.tolerance_.stated() && plan.cancel_.empty()) {
    const auto drift = decimal_less(*remaining, zero) ?
      decimal_subtract(zero, *remaining) :
      std::optional<Decimal64>(*remaining);
    if (drift && reconcile_detail::below_tolerance(*drift, already, inputs)) {
      return Plan<Broker>{};
    }
  }

  // Step 4: more working than wanted, so withdraw the side and send the whole remainder -- net of what
  // this runner reserved for its own orders not yet shown by holdings, when those may have filled
  // (`ReconcileInputs::reserved_rows_are_unconfirmed_`): their withdrawal is refused if they did, and a
  // whole remainder beside them traded twice. One genuinely resting is withdrawn all the same;
  // its history settles what it filled, and a later pass places the rest.
  if (decimal_less(*remaining, zero)) {
    Decimal64 reserved = zero;
    for (const auto& working : portfolio.working_) {
      if (working.order_.instrument_ == target.instrument_ &&
          working.order_.side_ == side && order_is_open(working.order_.status_)) {
        plan.cancel_.push_back(working.ref_);
        if (inputs.reserved_rows_are_unconfirmed_ && working.reserved_) {
          // A sum that will not compute refuses: undercounted, it would size the replacement larger.
          const auto left = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_);
          if (!left) {
            return reject(RejectReason::NOT_REPRESENTABLE);
          }
          if (decimal_less(zero, *left)) {
            const auto sum = decimal_add(reserved, *left);
            if (!sum) {
              return reject(RejectReason::NOT_REPRESENTABLE);
            }
            reserved = *sum;
          }
        }
      }
    }
    remaining = decimal_subtract(magnitude, reserved);
    if (!remaining) {
      return reject(RejectReason::NOT_REPRESENTABLE);
    }
    if (!decimal_less(zero, *remaining)) {
      return plan;
    }
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
    if (reconcile_detail::rounded_out_of_band(target.limit_->value(), *priced, side, inputs.limits_)) {
      return reject(RejectReason::NO_PERMITTED_PRICE);
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
 * @brief Plans the order for one intent, without comparing against the portfolio.
 *
 * An intent says "trade this much", so the portfolio is not consulted. A notional size is divided
 * by the limit price (or the reference price); the quantity is rounded down onto the lot; a limit
 * more aggressive than the band is clamped to it, and rounded onto the grid; a trigger must already be on the grid.
 *
 * @note The runner plans a weight-sized intent with `weight_target` instead; called directly, this
 *   sizes the whole allocation as a trade.
 *
 * @tparam Broker The broker tag.
 * @param intent The order intent.
 * @param portfolio Not used.
 * @param inputs Reference data, prices and band.
 * @return A plan with one placement, or one `Rejection`.
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
      if (intent.size().as_quantity().coefficient() < 0) {
        return reject(RejectReason::NEGATIVE_SIZE);
      }
      wanted = intent.size().as_quantity();
      break;
    case SizeKind::NOTIONAL: {
      if (intent.size().as_notional().amount_.coefficient() < 0) {
        return reject(RejectReason::NEGATIVE_SIZE);
      }
      // Not positive is no price to size by: divided by a negative one, a buy became a negative quantity.
      if (!sizing_price || sizing_price->coefficient() <= 0) {
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
      // The guards `weight_target` applies: a weight states a holding, which cannot be negative, and is
      // a fraction of a value and sized by a price that must both be positive.
      if (intent.size().as_target_weight().coefficient() < 0) {
        return reject(RejectReason::UNSUPPORTED_WEIGHT);
      }
      if (!inputs.portfolio_value_ || inputs.portfolio_value_->coefficient() <= 0) {
        return reject(RejectReason::NO_PORTFOLIO_VALUE);
      }
      if (!sizing_price || sizing_price->coefficient() <= 0) {
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
    if (reconcile_detail::rounded_out_of_band(intent.limit()->value(), *priced, intent.side(), inputs.limits_)) {
      return reject(RejectReason::NO_PERMITTED_PRICE);
    }
    order.type_ = OrderType::LIMIT;
    order.price_ = priced;
  } else {
    order.type_ = OrderType::MARKET;
  }

  if (intent.trigger()) {
    // Checked, never moved; see `RejectReason::TRIGGER_OFF_GRID`. An unknown grid answers neither
    // way and is refused as it is for a limit.
    const auto aligned = inputs.instrument_->price_rule_.on_tick(intent.trigger()->value());
    if (!aligned) {
      return reject(RejectReason::UNKNOWN_PRICE_GRID);
    }
    if (!*aligned) {
      return reject(RejectReason::TRIGGER_OFF_GRID);
    }
    order.type_ = OrderType::STOP_LIMIT;
    order.trigger_price_ = intent.trigger()->value();
  }

  plan.place_.push_back(std::move(order));
  return plan;
}

/** @brief The result of `weight_target`: a target position or a refusal, exactly one of them. */
struct WeightTarget {
  /** @brief The target position, if the weight could be sized. */
  std::optional<TargetPosition> target_;
  /** @brief Why it could not be, otherwise. */
  std::optional<RejectReason> refused_;
};

/**
 * @brief Converts a weight-sized intent into the target position it states.
 *
 * The target is `weight * portfolio value / price` shares, rounded down onto the lot, where the
 * price is the intent's limit if it has one and the reference price otherwise. The target keeps the
 * intent's limit and tag. Reconciled as a target, its limit applies to whatever trade the
 * difference needs, so above the weight a buy limit becomes the limit of a sell.
 *
 * @param intent A weight-sized intent (`SizeKind::TARGET_WEIGHT`).
 * @param inputs Reference data, reference price, and portfolio value.
 * @return The target, or a refusal: `UNSUPPORTED_WEIGHT` (sell, negative weight, trigger, non-`DAY`,
 *   idempotency key), `NO_REFERENCE_DATA`, `NO_PORTFOLIO_VALUE` (absent, not positive, or in
 *   another currency), `NO_PRICE_AVAILABLE`, or `NOT_REPRESENTABLE`.
 * @throws std::logic_error if `intent` is not weight-sized.
 */
inline WeightTarget weight_target(const OrderIntent& intent, const ReconcileInputs& inputs) {
  WeightTarget out;
  const auto refuse = [&](RejectReason reason) {
    out.refused_ = reason;
    return out;
  };
  const auto& weight = intent.size().as_target_weight();
  if (intent.side() != OrderSide::BUY || decimal_less(weight, Decimal64::parse("0")) ||
      intent.trigger() || intent.time_in_force() != TimeInForce::DAY || intent.client_order_id()) {
    return refuse(RejectReason::UNSUPPORTED_WEIGHT);
  }
  if (inputs.instrument_ == nullptr) {
    return refuse(RejectReason::NO_REFERENCE_DATA);
  }
  // A value of zero or less is not one a holding can be a fraction of: sized against it, a weight
  // would become a liquidation (zero) or a short (negative) -- an account whose read failed and came
  // back empty, or a replay that borrowed below zero equity, must not sell on that.
  if (!inputs.portfolio_value_ || inputs.portfolio_value_->coefficient() <= 0 ||
      (inputs.portfolio_value_currency_ &&
       !(*inputs.portfolio_value_currency_ == inputs.instrument_->currency_))) {
    return refuse(RejectReason::NO_PORTFOLIO_VALUE);
  }
  const auto price = intent.limit() ?
    std::optional<Decimal64>(intent.limit()->value()) :
    inputs.reference_price_;
  if (!price || price->coefficient() <= 0) {
    return refuse(RejectReason::NO_PRICE_AVAILABLE);
  }
  const auto allocation = decimal_multiply(*inputs.portfolio_value_, weight);
  const auto shares = allocation ? decimal_divide(*allocation, *price, 0) : std::nullopt;
  bool adjusted = false;
  const auto wanted = shares ?
    reconcile_detail::round_to_lot(*shares, reconcile_detail::lot_of(*inputs.instrument_), adjusted) :
    std::nullopt;
  if (!wanted) {
    return refuse(RejectReason::NOT_REPRESENTABLE);
  }
  TargetPosition target;
  target.instrument_ = intent.instrument();
  target.target_quantity_ = *wanted;
  target.limit_ = intent.limit();
  target.tag_ = intent.tag();
  out.target_ = std::move(target);
  return out;
}

} // namespace strategy
} // namespace algotradelib
