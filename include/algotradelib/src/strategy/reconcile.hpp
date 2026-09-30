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

/**
 * Why an instruction was not placed.
 *
 * Most values are reconciliation's own: it could not produce a valid instruction from what it was
 * given. The last two are not -- the instruction was built correctly and refused afterwards, by the
 * broker or by the binding that would have sent it -- and they live here because a report's
 * rejection bucket holds all of them, and a caller reading one entry needs its own answer rather
 * than a guess from executor state that a later instruction may already have overwritten.
 */
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
  /**
   * The broker refused the instruction, which reconciliation had built correctly.
   *
   * Unlike every reason above it, this is not a decision this library made. It is here because a
   * report's rejection bucket holds both, and a caller reading one needs to know which -- a broker
   * refusal spent a request and says something about the account or the instrument, while the
   * reasons above say the library could not build a valid order at all. Before this existed, a
   * broker refusal was recorded as `NOT_REPRESENTABLE`, which claims the arithmetic overflowed.
   */
  BROKER_REFUSED,
  /**
   * The binding refused to send it, so no broker was asked.
   *
   * An idempotency key at a broker that accepts none, or a withdrawal for an order the broker has
   * no reference for. Distinct from `BROKER_REFUSED` because no request went out and none was
   * counted, and distinct from the reasons above because reconciliation built the instruction
   * correctly -- what it asked for is something this binding cannot express.
   */
  BINDING_REFUSED,
  /**
   * A stop-limit order's trigger is not on the instrument's tick grid.
   *
   * Refused rather than rounded, unlike a limit. Rounding a limit toward the passive side is safe
   * whichever way the market moves; rounding a trigger moves *when the order exists*, and which
   * direction is the cautious one depends on whether the stop releases on a rise or a fall -- which
   * neither KIS nor Kiwoom documents. `Price` is on-grid by construction for the rule it was built
   * against, so this fires only when that rule was not the instrument's own.
   */
  TRIGGER_OFF_GRID,
  /**
   * A target that needs a trade was stated while a stop order rests on its instrument, so the stop
   * is being withdrawn first and nothing is sent this pass.
   *
   * A stop is a *conditional* order, and target arithmetic is about orders that will trade. Counted
   * as working, a resting sell stop satisfies a target of zero and "flatten now" sends nothing;
   * ignored, an immediate exit goes out beside it and both can sell. So a target that needs a trade
   * withdraws the stop and waits. **`runner::Runner` keeps the target** and settles it again, with no
   * callback from the strategy, once a listing no longer reports a stop on the instrument -- see
   * `Runner::deferred_targets`. A strategy stating anything newer for the instrument replaces it.
   *
   * A target the holding already meets needs no trade and is not refused: the stop stays, since
   * protecting a position the strategy means to hold is what it is for, and only other working orders
   * are withdrawn as before.
   */
  STOP_ORDER_RESTING,
  /**
   * A weight-sized intent that states something a holding cannot: a sell, a negative weight, a
   * trigger, a validity other than a day order, or an idempotency key. A weight is planned as the
   * position it states (`weight_target`), and none of those survive the conversion, so each is
   * refused by name rather than dropped.
   */
  UNSUPPORTED_WEIGHT,
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
  }
  return "unknown";
}

/** One instruction that was refused, and why. */
struct Rejection {
  /** Why reconciliation refused the instruction. */
  RejectReason reason_{RejectReason::NO_REFERENCE_DATA};
  /** The instrument the refused instruction was for. */
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
  /** What to trade. Identity, not routing. */
  InstrumentId instrument_;
  /** Which way. */
  OrderSide side_{OrderSide::BUY};
  /** The price basis. */
  OrderType type_{OrderType::LIMIT};
  /** The execution condition. */
  TimeInForce time_in_force_{TimeInForce::DAY};
  /** The session to trade in. */
  OrderSession session_{OrderSession::REGULAR};
  /**
   * Where to route it; see `OrderRequest::venue_`. Not the instrument's venue.
   *
   * Nothing a strategy says sets it. A live runner writes its binding's route in before sending
   * (`live::SessionExecutor::default_route`), so the journal, the reserved working row and the report
   * name where the order went.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** How much, after any rounding onto the lot that `quantity_rounded_` records. */
  Decimal64 quantity_;
  /** The limit price, after any rounding or clamping the flags below record. */
  std::optional<Decimal64> price_;
  /**
   * The trigger price of a `STOP_LIMIT` order, exactly as the strategy stated it. Never rounded
   * and never clamped -- see `RejectReason::TRIGGER_OFF_GRID` -- and empty on every other basis.
   */
  std::optional<Decimal64> trigger_price_;
  /** The strategy's own label on the intent this order came from. */
  std::string tag_;
  /** An idempotency key, only where the bound broker supports one. */
  std::optional<std::string> client_order_id_;

  /** Whether the price was moved onto the tick grid. */
  bool price_rounded_{false};
  /** Whether the price was moved into today's permitted band. */
  bool price_clamped_{false};
  /**
   * Whether a band was held for this instrument, **not applied**, and would have moved this price.
   *
   * Never set by reconciliation, which is handed one band or none and cannot know a second existed.
   * `runner::Runner` sets it after the fact when its own age policy withheld a band -- see
   * `Runner::set_maximum_price_limits_age` for why withholding is the safe direction and
   * `Runner::unclamped_orders_rejected` for what it costs when it is not.
   *
   * A statement about this price, not a prediction about the wire: the price lies outside the
   * withheld band on the side the clamp corrects. Clamping runs before tick rounding, so a bound
   * that binds here could in principle round back to the same value.
   */
  bool price_would_have_been_clamped_{false};
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
  /** Orders to withdraw. These go first; see the note above on why the order is not optional. */
  std::vector<OrderRef<Broker>> cancel_;
  /** Orders to send, after every cancellation has gone. */
  std::vector<PlannedOrder> place_;
  /** Instructions reconciliation refused, with the reason on each. */
  std::vector<Rejection> rejected_;

  /** Returns whether the plan asks for nothing at all. */
  bool empty() const noexcept {
    return cancel_.empty() && place_.empty() && rejected_.empty();
  }
};

/**
 * How small a drift is not worth an instruction.
 *
 * An execution-cost policy, which is why it arrives from the runner rather than from a strategy: a
 * strategy states the position it wants, and how finely that is chased depends on commission, on
 * whether the venue charges per instruction, and on what queue priority is worth -- none of which a
 * signal knows.
 *
 * **Three units, because the right one depends on which cost dominates, and one number in shares is
 * the weakest of them.** A single share count means wildly different things across a portfolio: with
 * `minimum_quantity_` set to five, the implied value threshold is 1,000,000 won on a 200,000-won share
 * and 15,000 won on a 3,000-won one, a factor of sixty-seven. There is no share count that is right
 * for a portfolio, so the other two exist.
 *
 * | Floor | Keys on | The cost it answers |
 * | --- | --- | --- |
 * | `minimum_quantity_` | the drift in shares | lot economics, or a per-share fee |
 * | `minimum_notional_` | the drift's value at the reference price | a fixed or minimum fee per order |
 * | `minimum_fraction_of_working_` | the drift over what is already working | queue priority given up |
 *
 * The third is the subtle one and often the largest cost. Replacing a working order for 100 to change
 * it by one share risks that order's whole place in the book to adjust one percent of it -- and the
 * loss is proportional to the 100, not to the 1. It therefore applies **only when something is
 * working**: with nothing resting there is no priority to give up, and a drift is simply a new order.
 *
 * **Every floor is absent by default, which is exactly the behaviour before any of this existed.** No
 * number is invented here, because each depends on a broker's own fee schedule. A tolerance guessed at
 * would silently hold back orders a strategy asked for, and an invisible shortfall is worse than
 * visible churn.
 *
 * **Any stated floor being met is sufficient to suppress.** Each expresses an independent reason the
 * trade is not worth making, so one of them triggering is enough; requiring all of them would mean a
 * drift worth less than a fixed fee still traded because it happened to be a large fraction of a tiny
 * working order.
 *
 * **A floor that cannot be evaluated does not suppress.** Without a reference price the notional floor
 * is unknowable, and the honest response is to trade -- the opposite of `RiskGuard`, where a limit that
 * cannot be evaluated refuses. The reasoning is the same and the consequences are mirrored: there,
 * failing to refuse risks an oversized position; here, failing to trade leaves an invisible shortfall.
 */
struct TradeTolerance {
  /** Smallest drift worth trading, in shares. */
  std::optional<Decimal64> minimum_quantity_;
  /** Smallest drift worth trading, valued at the reference price. */
  std::optional<Decimal64> minimum_notional_;
  /**
   * Smallest drift worth trading, as a fraction of the quantity already working.
   *
   * `0.02` is two percent. Ignored when nothing is working, since there is then no queue position to
   * give up.
   */
  std::optional<Decimal64> minimum_fraction_of_working_;

  /** Returns whether any floor is stated. */
  bool stated() const noexcept {
    return minimum_quantity_.has_value() || minimum_notional_.has_value() ||
      minimum_fraction_of_working_.has_value();
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

  /** How small a drift is not worth an instruction. Every floor absent means chase every share. */
  TradeTolerance tolerance_;
  /**
   * The currency `portfolio_value_` is stated in, where the caller said. A weight on an instrument in
   * another currency is refused rather than sized across currencies at a rate of one; absent, the
   * value is taken to be in the instrument's currency, as before. Appended last.
   */
  std::optional<Currency> portfolio_value_currency_;
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
/**
 * Returns whether a working row is a stop order: its basis says so, **or** it carries a trigger.
 *
 * The second half is not redundant. An adapter records a positive trigger even on a row whose basis
 * reads otherwise, and that row may be a stop that has released. Target reconciliation and the
 * runner's deferred targets both ask this, so they cannot disagree about which rows hold a target up.
 */
inline bool is_stop_order(const OrderSnapshot& order) {
  return carries_trigger_price(order.type_) || order.trigger_price_.has_value();
}

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
  // **Net across every position row, not the first one.** This read `quantity_of`, which returns the
  // first match, and a broker reporting one instrument as two lots is ordinary -- so a target of
  // zero against rows of 60 and 40 planned a sell of 60 and left 40 held, while
  // `vanilla::TimedLiquidation` submitted one such target *per row* and planned that same sell
  // twice. Together those sold 120 of a holding of 100 wherever the phase permitted it. Sizing
  // against the net holding is the same rule `projected_gross_exposure` and `closes_toward_flat`
  // already apply, and having reconciliation disagree with the guards about how much is held is the
  // partial-coherence failure this repository has paid for before.
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
 * consulted: a strategy that emits intents has taken responsibility for its own sizing. **Except a
 * weight**, which says "hold this much": the runner plans a weight-sized intent as the target it
 * states (`weight_target`) and does not call this for one. Called with one directly, this still
 * sizes the whole allocation as a trade. What still
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

/** What a weight-sized intent turns into: the target position it states, or the reason it cannot. */
struct WeightTarget {
  std::optional<TargetPosition> target_;
  std::optional<RejectReason> refused_;
};

/**
 * Turns a weight-sized intent into the target position it states.
 *
 * **A weight is a holding**, which is what `SizeKind::TARGET_WEIGHT` promises: planned as an order it
 * would buy the whole allocation again at every rebalance. So the runner plans it as a
 * `TargetPosition` of `weight x portfolio value / price` shares, rounded down onto the lot, where the
 * price is the intent's limit if it has one and the reference price otherwise, carrying the limit and
 * the tag; target reconciliation then diffs it against the position and the working orders.
 *
 * Refused by name, and never converted, when the intent states what a holding cannot
 * (`UNSUPPORTED_WEIGHT`): a sell or a negative weight (the side would be discarded, turning a sell
 * into a buy), a trigger (a target carries none, so a stop would become an immediate limit), a
 * validity other than a day order, or an idempotency key (a target carries neither, and a key
 * silently dropped is a retry that looked safe). Refused `NO_PORTFOLIO_VALUE` with no value, a value
 * of zero or less, or a value stated in another currency than the instrument's; `NO_PRICE_AVAILABLE`
 * with no price.
 *
 * As a target it behaves as a target does: other working orders on the instrument are withdrawn when
 * the weight is met, and orders opposing the trade it needs are withdrawn before it; and a buy
 * intent's limit is the limit of whatever trade the difference needs, so when the holding is above
 * the weight a passive buy limit becomes the limit of a sell.
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
