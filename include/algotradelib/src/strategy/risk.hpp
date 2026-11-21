#pragma once

#include "./event.hpp"
#include "./portfolio.hpp"
#include "./reconcile.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * @brief Who or what is holding trading stopped.
 *
 * Several holds can be in force at once; trading resumes only when every one is released. The
 * enumerators are ordered most fundamental first, and `RiskGuard::blocking_reason` reports the
 * first one held. Whether a hold also stops cancellations is `halt_blocks`.
 */
enum class HaltReason {
  /** @brief A person stopped trading with `RiskGuard::halt()`; only `RiskGuard::resume()` clears it. */
  OPERATOR,
  /**
   * @brief Local machinery failed, such as the order journal not being writable; nothing is sent.
   *
   * Sticky; release it with `runner::Runner::resume_after_local_failure`, which also acknowledges the
   * journal failures (`RiskGuard::resume(LOCAL_FAILURE)` lifts the hold without that). The detail names
   * the failure.
   */
  LOCAL_FAILURE,
  /** @brief A stop was requested and the run is shutting down. */
  STOPPING,
  /**
   * @brief The broker's order list has not been read since this process started.
   *
   * Until it is, the runner cannot know whether a previous process left orders working, so it
   * places nothing.
   */
  ORDER_LIST_UNREAD,
  /**
   * @brief No holdings snapshot has been read yet, so what the account holds is unknown.
   *
   * An empty portfolio would pass every check as if the account were flat, so placements wait.
   */
  PORTFOLIO_ABSENT,
  /**
   * @brief The holdings snapshot is older than the configured maximum age.
   *
   * Sizing against old holdings can double a position or miss one acquired since.
   */
  PORTFOLIO_STALE,
  /**
   * @brief A buying-power limit is enforced, but there is no current reading to check it against.
   *
   * The reading was never polled, has not returned yet, or has aged out. `runner::PollingLoop` also
   * holds this over a source whose broker reports no buying power, or that does not say
   * (`runner::source_reports_buying_power`): without `RiskLimits::run_budget_`, since nothing would
   * then bound a buy, and with one while `RiskLimits::enforce_buying_power_` is on, since no reading
   * will ever arrive.
   */
  BUYING_POWER_UNREAD,
  /** @brief A poll kept failing and the loop gave up, as opposed to `STOPPING` on request. */
  POLL_FAILING,
};

/** @brief Every `HaltReason`, in declaration order. */
inline constexpr std::array<HaltReason, 8> all_halt_reasons{
  HaltReason::OPERATOR,
  HaltReason::LOCAL_FAILURE,
  HaltReason::STOPPING,
  HaltReason::ORDER_LIST_UNREAD,
  HaltReason::PORTFOLIO_ABSENT,
  HaltReason::PORTFOLIO_STALE,
  HaltReason::BUYING_POWER_UNREAD,
  HaltReason::POLL_FAILING,
};

/** @brief The number of `HaltReason` values. */
inline constexpr std::size_t halt_reason_count = all_halt_reasons.size();

/**
 * @brief What an instruction can do to a position, which decides which holds stop it.
 *
 * A replacement is sent as a withdrawal plus a placement, and the placement is `OPENING`.
 */
enum class InstructionClass {
  /** @brief A placement, which can open or enlarge a position. Every hold stops it. */
  OPENING,
  /**
   * @brief A cancellation of an order the caller named.
   *
   * Not a cancellation the runner derived from the portfolio; `runner::Runner` holds those to the
   * same holds as a placement.
   */
  WITHDRAWAL,
};

/**
 * @brief Tests whether a hold stops an instruction of a given class.
 *
 * Every hold stops a placement. A named cancellation is stopped only by holds that mean someone
 * stopped the runner (`OPERATOR`, `LOCAL_FAILURE`, `STOPPING`), not by holds that mean data is
 * missing or stale, so a resting order can still be withdrawn when polling fails.
 *
 * @param reason The hold.
 * @param klass The instruction class.
 * @return `true` if `reason` stops `klass`.
 */
inline constexpr bool halt_blocks(HaltReason reason, InstructionClass klass) noexcept {
  switch (klass) {
    case InstructionClass::OPENING:
      return true;
    case InstructionClass::WITHDRAWAL:
      break;
  }
  switch (reason) {
    case HaltReason::OPERATOR:
    case HaltReason::LOCAL_FAILURE:
    case HaltReason::STOPPING:
      return true;
    // Guards against an opening order duplicating a predecessor's; a named withdrawal duplicates nothing.
    case HaltReason::ORDER_LIST_UNREAD:
    // None of these says anything about a row the caller already named.
    case HaltReason::PORTFOLIO_ABSENT:
    case HaltReason::PORTFOLIO_STALE:
    case HaltReason::BUYING_POWER_UNREAD:
    // A known resting order is still safe to withdraw when polling is abandoned.
    case HaltReason::POLL_FAILING:
      return false;
  }
  return true;
}

/**
 * @brief Returns a lowercase name for a hold, such as `"portfolio_stale"`, for logs.
 * @param reason The hold.
 * @return The name; empty for a value that is not a `HaltReason`.
 */
inline constexpr std::string_view halt_reason_name(HaltReason reason) noexcept {
  switch (reason) {
    case HaltReason::OPERATOR:
      return "operator";
    case HaltReason::LOCAL_FAILURE:
      return "local_failure";
    case HaltReason::STOPPING:
      return "stopping";
    case HaltReason::ORDER_LIST_UNREAD:
      return "order_list_unread";
    case HaltReason::PORTFOLIO_ABSENT:
      return "portfolio_absent";
    case HaltReason::PORTFOLIO_STALE:
      return "portfolio_stale";
    case HaltReason::BUYING_POWER_UNREAD:
      return "buying_power_unread";
    case HaltReason::POLL_FAILING:
      return "poll_failing";
  }
  return "";
}

static_assert(
  lists_every_value(
    all_halt_reasons,
    [](HaltReason reason) {
      return !halt_reason_name(reason).empty();
    }
  ),
  "all_halt_reasons must list every HaltReason"
);

/*
 * Operator limits, checked before anything is sent. A guard refuses an order; it never resizes one.
 */

/** @brief Which check refused an order. */
enum class GuardReason {
  /** @brief A hold is in force; `GuardRejection::halt_reason_` names it. */
  TRADING_HALTED,
  /** @brief The instrument is not in `RiskLimits::permitted_universe_`. */
  INSTRUMENT_NOT_PERMITTED,
  /** @brief The market phase is not in `RiskLimits::permitted_phases_`. */
  PHASE_NOT_PERMITTED,
  /** @brief The order's value exceeds `RiskLimits::max_order_notional_`. */
  ORDER_NOTIONAL_TOO_LARGE,
  /** @brief The position could exceed `RiskLimits::max_position_quantity_`. */
  POSITION_LIMIT_EXCEEDED,
  /** @brief The gross exposure could exceed `RiskLimits::max_gross_exposure_`. */
  GROSS_EXPOSURE_EXCEEDED,
  /** @brief `RiskLimits::max_orders_per_window_` orders were already sent in the window. */
  ORDER_RATE_EXCEEDED,
  /**
   * @brief A limit needed a value that is not known.
   *
   * No price to value the order, no currency for a per-currency limit, or a
   * `GuardInputs::exposure_basis_` that does not cover every instrument held or working.
   */
  VALUE_NOT_KNOWN,
  /** @brief The sell exceeds the broker's sellable quantity, or the broker gave none. */
  QUANTITY_NOT_SELLABLE,
  /**
   * @brief A sellable quantity is present, but what it covers is not established.
   *
   * Unlike `QUANTITY_NOT_SELLABLE`, the fix is to declare a `SellableBasis` for the figure, not to
   * resize. A `SellableBasis::Coverage::PARTIAL` basis reports here too.
   */
  SELLABLE_BASIS_UNESTABLISHED,
  /** @brief The buy costs more than the broker's buying power, or the broker gave none. */
  INSUFFICIENT_BUYING_POWER,
  /**
   * @brief The buy's value is more than the run's room (`RiskLimits::run_budget_`).
   *
   * The room is the run's cash less what its open buys reserve less this buy's estimated fee
   * (`RiskLimits::run_fees_`); the value is its quantity times its limit price. `GuardRejection::limit_` is the
   * room, below zero when the run is, and `GuardRejection::attempted_` the value. Also a buy priced in another
   * currency than the run's: then `limit_` is the run's cash, in its own currency, and `attempted_` is absent.
   */
  RUN_BUDGET_EXCEEDED,
  /**
   * @brief A budgeted run's buy carries no positive limit price of its own, so there is no cap to reserve
   *   against.
   *
   * Only a limit or a stop-limit with a positive price passes: every other type (market, best, primary, mid,
   * closing, unknown, and a conditional limit, which becomes a market order at the close) is refused.
   */
  RUN_BUY_NEEDS_LIMIT,
  /**
   * @brief The sell is more than the run's own shares of the instrument less its open sells.
   *
   * Shares the account held before the run are not the run's. `GuardRejection::limit_` is never below zero.
   */
  RUN_HOLDING_EXCEEDED,
  /**
   * @brief The run's cash, or its shares of the instrument, is not known.
   *
   * `GuardInputs::run_cash_` (for a buy) or `GuardInputs::run_holding_` (for a sell) is absent, the run's cash is
   * in another currency than the budget, or `RiskLimits::run_fees_` is absent. A live run leaves them absent
   * while it is not open, its journal writes have stopped, or an order of the run is unnamed, held, or released
   * without a booking (`runner::Runner::run_unsettled`).
   */
  RUN_LEDGER_UNSETTLED,
};

/**
 * @brief Returns a lowercase name for a refusal reason, such as `"order_rate_exceeded"`, for logs.
 * @param reason The reason.
 * @return The name.
 */
inline constexpr std::string_view guard_reason_name(GuardReason reason) noexcept {
  switch (reason) {
    case GuardReason::TRADING_HALTED:
      return "trading_halted";
    case GuardReason::INSTRUMENT_NOT_PERMITTED:
      return "instrument_not_permitted";
    case GuardReason::PHASE_NOT_PERMITTED:
      return "phase_not_permitted";
    case GuardReason::ORDER_NOTIONAL_TOO_LARGE:
      return "order_notional_too_large";
    case GuardReason::POSITION_LIMIT_EXCEEDED:
      return "position_limit_exceeded";
    case GuardReason::GROSS_EXPOSURE_EXCEEDED:
      return "gross_exposure_exceeded";
    case GuardReason::ORDER_RATE_EXCEEDED:
      return "order_rate_exceeded";
    case GuardReason::VALUE_NOT_KNOWN:
      return "value_not_known";
    case GuardReason::QUANTITY_NOT_SELLABLE:
      return "quantity_not_sellable";
    case GuardReason::SELLABLE_BASIS_UNESTABLISHED:
      return "sellable_basis_unestablished";
    case GuardReason::INSUFFICIENT_BUYING_POWER:
      return "insufficient_buying_power";
    case GuardReason::RUN_BUDGET_EXCEEDED:
      return "run_budget_exceeded";
    case GuardReason::RUN_BUY_NEEDS_LIMIT:
      return "run_buy_needs_limit";
    case GuardReason::RUN_HOLDING_EXCEEDED:
      return "run_holding_exceeded";
    case GuardReason::RUN_LEDGER_UNSETTLED:
      return "run_ledger_unsettled";
  }
  return "unknown";
}

/** @brief One order a guard refused, with the limit and the value that exceeded it. */
struct GuardRejection {
  /** @brief Which check refused the order. */
  GuardReason reason_{GuardReason::TRADING_HALTED};
  /** @brief The instrument of the refused order. */
  InstrumentId instrument_;
  /** @brief The tag of the intent the order came from. */
  std::string tag_;
  /** @brief The hold that stopped the order, when `reason_` is `TRADING_HALTED`; otherwise empty. */
  std::optional<HaltReason> halt_reason_;
  /** @brief The limit that was exceeded, where the reason has one. */
  std::optional<Decimal64> limit_;
  /** @brief The value the order would have reached, where the reason has one. */
  std::optional<Decimal64> attempted_;
};

/**
 * @brief The fees a run budget estimates for an order (`RiskLimits::run_fees_`).
 *
 * The same shapes as the backtest's `CommissionModel`, institution rate and `SellTax`, stated here because a
 * budget needs them before any fill. They apply per order: the minimum and the rounding apply once to the
 * order's traded value, however many fills make it up. Each charge rounds up to a whole currency unit (an exact
 * zero stays zero), so it is never below a charge truncated to whole units; for a currency with cents the
 * commission and the institution fee, each rounded up, can over-reserve up to two units an order. Per-share fees
 * have no shape here. Default-constructed, it states no fees at all.
 */
struct RunFees {
  /** @brief The fraction of traded value charged on buys and sells (`0.00015`, not `0.015`). */
  Decimal64 commission_rate_{Decimal64::parse("0")};
  /** @brief The least commission on one order. */
  Decimal64 commission_minimum_{Decimal64::parse("0")};
  /** @brief A fraction of traded value charged on top of the commission (an institution fee), if any. */
  std::optional<Decimal64> institution_rate_;
  /** @brief The fraction of a sell's value charged as tax, if any. */
  std::optional<Decimal64> sell_tax_rate_;

  /**
   * @brief Estimates the commission on one order.
   * @param value The order's traded value.
   * @return `max(ceil(value x commission_rate_), commission_minimum_) + ceil(value x institution_rate_)`, each
   *   `ceil` to a whole unit and the minimum as stated; `std::nullopt` if the value, a rate or the minimum is
   *   negative, or the arithmetic does not fit.
   */
  std::optional<Decimal64> estimate_commission(const Decimal64& value) const {
    // A decimal's sign is its coefficient's.
    if (
      value.coefficient() < 0 || commission_rate_.coefficient() < 0 || commission_minimum_.coefficient() < 0 ||
      (institution_rate_ && institution_rate_->coefficient() < 0)
    ) {
      return std::nullopt;
    }
    auto commission = charged_on(value, commission_rate_);
    if (!commission) {
      return std::nullopt;
    }
    if (decimal_less(*commission, commission_minimum_)) {
      commission = commission_minimum_;
    }
    if (!institution_rate_) {
      return commission;
    }
    const auto institution = charged_on(value, *institution_rate_);
    if (!institution) {
      return std::nullopt;
    }
    return decimal_add(*commission, *institution);
  }

  /**
   * @brief Estimates the tax on one order.
   * @param value The order's traded value.
   * @param side The order's side.
   * @return `ceil(value x sell_tax_rate_)` on a sell, in whole units, and zero on a buy or without a rate;
   *   `std::nullopt` if the value or the rate is negative, or the arithmetic does not fit.
   */
  std::optional<Decimal64> estimate_tax(const Decimal64& value, OrderSide side) const {
    if (value.coefficient() < 0 || (sell_tax_rate_ && sell_tax_rate_->coefficient() < 0)) {
      return std::nullopt;
    }
    if (side != OrderSide::SELL || !sell_tax_rate_) {
      return Decimal64(0, 0);
    }
    return charged_on(value, *sell_tax_rate_);
  }

private:
  // `value x rate`, rounded up to a whole unit; both are not negative.
  static std::optional<Decimal64> charged_on(const Decimal64& value, const Decimal64& rate) {
    if (rate.coefficient() == 0) {
      return Decimal64(0, 0);
    }
    const auto exact = decimal_multiply(value, rate);
    if (!exact) {
      return std::nullopt;
    }
    // Rounded up on the coefficient: `coefficient / 10^scale`, plus one for any remainder. A scale of 19 or more
    // has no `int64` power of ten, and gives no estimate.
    const auto scale = exact->scale();
    if (scale > 18) {
      return std::nullopt;
    }
    std::int64_t unit = 1;
    for (std::uint8_t step = 0; step < scale; ++step) {
      unit *= 10;
    }
    const auto coefficient = exact->coefficient();
    return Decimal64(coefficient / unit + (coefficient % unit != 0 ? 1 : 0), 0);
  }
};

/**
 * @brief The limits the operator sets for `RiskGuard`.
 *
 * Every limit is optional and absent means unlimited, except `permitted_phases_`, which by
 * default permits opening positions only while the market is `OPEN`, and
 * `enforce_sellable_quantity_`, which is on by default. Holds (halts) are not limits;
 * they are taken on the guard itself.
 */
struct RiskLimits {
  /** @brief The largest value one order may have, in its own currency. */
  std::optional<Decimal64> max_order_notional_;
  /**
   * @brief The largest absolute position, in shares, any instrument may reach.
   *
   * Checked against the net held quantity (all rows) plus everything working on the order's side
   * plus the order, as if all of it filled.
   */
  std::optional<Decimal64> max_position_quantity_;
  /**
   * @brief The largest gross exposure the portfolio may reach, in the order's currency.
   *
   * Checked against the exposure if this order and every outstanding order filled, each valued at
   * the highest price that could apply (see `projected_gross_exposure`). The ceiling applies to each
   * currency separately; there is no account-wide converted cap.
   *
   * An order that does not raise this projection is permitted at any size, so an exit (a sell up
   * to flat against a long) is allowed even when the account is over the ceiling. A sell that goes
   * through flat into a short raises it and can be refused. To cap one order's size, use
   * `max_order_notional_`. An order that cannot be valued, or an incomplete
   * `GuardInputs::exposure_basis_`, is refused with `GuardReason::VALUE_NOT_KNOWN`.
   */
  std::optional<Decimal64> max_gross_exposure_;
  /** @brief The most orders that may be sent within `rate_window_`. */
  std::optional<std::size_t> max_orders_per_window_;
  /** @brief The window `max_orders_per_window_` counts over. */
  std::chrono::seconds rate_window_{60};
  /**
   * @brief Whether to refuse a sell larger than the broker's sellable quantity.
   *
   * On by default: a sell is refused if the broker gave no sellable figure (the held quantity is not
   * assumed sellable) or the figure's basis is not established. So an account that cannot sell what
   * it does not hold never sends a sale that would open a short: every shipped adapter places cash
   * orders, and a target below zero is refused here before it is sent.
   *
   * Turn it off, where the replay or the dry run is configured, to simulate short positions; that
   * models no borrow. Turning it off for a live account borrows nothing either: it only lets the sale
   * reach the broker.
   */
  bool enforce_sellable_quantity_{true};
  /**
   * @brief Whether to refuse a buy that costs more than the broker's buying power.
   *
   * Off by default. The figure is a reading: a fill since the broker answered can change it, so
   * passing this check does not guarantee the broker accepts the order.
   */
  bool enforce_buying_power_{false};
  /** @brief The instruments that may be traded; absent means any. */
  std::optional<std::vector<InstrumentId>> permitted_universe_;
  /**
   * @brief The market phases in which a position may be opened or increased; absent means any.
   *
   * The default permits only `OPEN`, so `UNKNOWN` (an uncovered calendar date) is refused.
   *
   * An order that only closes toward flat (it opposes the net held quantity and, with what is
   * already working on the closing side, cannot carry it through flat) is permitted in any phase,
   * so a flatten after the close can still be sent. The later checks (sellable, buying power,
   * position and exposure ceilings) still apply to it, and the broker will likely reject it outside
   * its session; `max_orders_per_window_` limits how often such an order is resent, and
   * `runner::Runner::sends_outside_permitted_phase` counts them. Cancellations are never checked
   * against this.
   */
  std::optional<std::vector<MarketPhase>> permitted_phases_{std::vector<MarketPhase>{MarketPhase::OPEN}};
  /**
   * @brief The run's starting cash, in one currency: buys spend it, sells return their proceeds, and the run
   *   never borrows.
   *
   * The run starts flat and owns only the shares it buys. A buy must carry a positive limit price
   * (`GuardReason::RUN_BUY_NEEDS_LIMIT`). The buying-power check then spends the smaller of the broker's buying
   * power (with `enforce_buying_power_`) and the run's room: its cash less what its open buys reserve less this
   * buy's estimated fee (`run_fees_`). A buy whose limit price times its quantity is more is refused, naming the
   * smaller bound: `GuardReason::RUN_BUDGET_EXCEEDED` when the room is smaller, `INSUFFICIENT_BUYING_POWER` when
   * the broker's is smaller or equal; equal passes. The sellable check bounds a sell by the smaller of the
   * broker's sellable quantity less resting sells (with `enforce_sellable_quantity_`) and the run's own shares
   * less its open sells: `GuardReason::RUN_HOLDING_EXCEEDED` when the run's is smaller, `QUANTITY_NOT_SELLABLE`
   * when the broker's is smaller or equal. Always name the currency (`Currency` defaults to KRW); a buy priced in
   * another currency is refused.
   *
   * `runner::Runner` keeps the run's cash and shares, and refuses to construct with a budget but no `run_fees_`,
   * or over an executor that sends to a broker unless the runner keeps its run in the order journal (a serialized
   * `runner::JournalingExecutor` over a broker that lists working orders). Such a runner opens its run at
   * `runner::recover_runner`: it resumes the journal's run, and opens a new one with this amount only when stated
   * fresh (`runner::RunOpening`). It does not turn on `enforce_buying_power_`. See "The run budget" in
   * running-a-strategy-live.md.
   */
  std::optional<Money> run_budget_;
  /** @brief The fees a run budget estimates for each order; required with `run_budget_`, unread without it. */
  std::optional<RunFees> run_fees_;
};

/**
 * @brief An instrument's currency and the price to value it at, for the exposure ceiling.
 */
struct InstrumentBasis {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The currency it trades in. */
  Currency currency_;
  /**
   * @brief The mark to value it at.
   *
   * `projected_gross_exposure` raises it to any higher limit price of an order on the instrument.
   */
  Decimal64 price_;
};

/** @brief What `RiskGuard::check` needs besides the order and the portfolio. */
struct GuardInputs {
  /** @brief The current instant; `RiskLimits::rate_window_` counts back from it. */
  std::chrono::system_clock::time_point now_{};
  /** @brief The market phase the order would be sent in. */
  MarketPhase phase_{MarketPhase::UNKNOWN};
  /** @brief A price to value a market order, which carries none. */
  std::optional<Decimal64> reference_price_;
  /**
   * @brief The currency and mark of every instrument held or working, plus the order's.
   *
   * Needed only by `RiskLimits::max_gross_exposure_`. A missing entry, or two entries for one
   * instrument, refuses with `GuardReason::VALUE_NOT_KNOWN`. The runner fills it from reference
   * data.
   */
  std::vector<InstrumentBasis> exposure_basis_;
  /**
   * @brief The currency the order is priced in, from its reference data.
   *
   * Needed by the buying-power check (absent refuses with `INSUFFICIENT_BUYING_POWER`) and by the
   * gross-exposure ceiling and a run budget's buy (absent refuses with `VALUE_NOT_KNOWN`).
   */
  std::optional<Currency> order_currency_;
  /**
   * @brief The run's cash less what its open buys reserve, read by a budgeted buy (`RiskLimits::run_budget_`).
   *
   * The guard takes the buy's own estimated fee off it. The runner fills it; absent, or in another currency than
   * the budget, refuses a budgeted buy with `GuardReason::RUN_LEDGER_UNSETTLED`.
   */
  std::optional<Money> run_cash_;
  /**
   * @brief The run's own shares of the order's instrument less its open sells, read by a budgeted sell.
   *
   * The runner fills it; absent refuses a budgeted sell with `GuardReason::RUN_LEDGER_UNSETTLED`.
   */
  std::optional<Decimal64> run_holding_;
};

namespace risk_detail {

/** Returns the larger of two decimals, since `Decimal64` orders through `decimal_less`. */
inline Decimal64 decimal_max(const Decimal64& lhs, const Decimal64& rhs) {
  return decimal_less(lhs, rhs) ? rhs : lhs;
}

/** Returns `value`'s distance from zero. */
inline std::optional<Decimal64> decimal_magnitude(const Decimal64& value) {
  const auto zero = Decimal64::parse("0");
  if (!decimal_less(value, zero)) {
    return value;
  }
  return decimal_subtract(zero, value);
}

/** What a run budget reserves for a buy: `quantity x price` plus the commission `fees` estimate on it, or nothing. */
inline std::optional<Decimal64> run_buy_cost(const RunFees& fees, const Decimal64& quantity, const Decimal64& price) {
  const auto value = decimal_multiply(quantity, price);
  const auto commission = value ? fees.estimate_commission(*value) : std::nullopt;
  return commission ? decimal_add(*value, *commission) : std::nullopt;
}

} // namespace risk_detail

/**
 * @brief What a candidate order adds to `projected_gross_exposure`.
 *
 * Compare `QUANTITY_AND_PRICE` with `PRICE_ONLY` to see what the order's quantity alone adds; both
 * value instruments at the same prices.
 */
enum class CandidateContribution {
  /** @brief The candidate's quantity and price: the exposure if the order is sent. */
  QUANTITY_AND_PRICE,
  /** @brief The candidate's price only: the exposure without the order, valued at the same prices. */
  PRICE_ONLY,
};

/**
 * @brief Computes the gross exposure in one currency if every outstanding order (and a candidate)
 *   filled.
 *
 * Per instrument, the exposure is `max(|held + working buys|, |held - working sells|)` times a
 * price, summed over instruments in `currency`. Opposite-side orders do not offset each other.
 * The price is the highest that could apply: the basis mark and the limit price of every open order
 * on the instrument, the candidate's included. The candidate joins its own instrument on its own
 * side, so a sell that exits a long up to flat (counting sells already working) does not raise
 * the figure, though it does not lower it either until it fills. A sell larger than the long
 * reaches a short and can raise it.
 *
 * @tparam Broker The portfolio's broker tag.
 * @param portfolio The positions (summed over every row) and working orders.
 * @param basis The currency and mark of every instrument held, working or ordered.
 * @param currency The currency to total; instruments in other currencies contribute nothing.
 * @param candidate The order being checked, or `nullptr`.
 * @param candidate_price The price to value the candidate at if it has no limit (a market order).
 * @param contribution Whether the candidate's quantity counts, or only its price.
 * @return The exposure, or `std::nullopt` if it cannot be computed exactly: an instrument missing
 *   from `basis` or listed twice, a mark or price that is not positive, a working order filled
 *   beyond its quantity, or arithmetic that does not fit.
 */
template <typename Broker>
std::optional<Decimal64> projected_gross_exposure(
  const Portfolio<Broker>& portfolio, const std::vector<InstrumentBasis>& basis, const Currency& currency,
  const PlannedOrder* candidate, const std::optional<Decimal64>& candidate_price, CandidateContribution contribution
) {
  const auto zero = Decimal64::parse("0");

  // Every instrument the answer depends on: held, working, or about to be ordered. Collected first
  // so each one is valued exactly once however many rows mention it.
  std::vector<InstrumentId> instruments;
  const auto note = [&instruments](const InstrumentId& id) {
    for (const auto& seen : instruments) {
      if (seen == id) {
        return;
      }
    }
    instruments.push_back(id);
  };
  for (const auto& position : portfolio.positions_) {
    // A flat row (a closed position a broker or the replay ledger still lists) is not exposure and
    // needs no basis; if it also has a working order, the loop below collects it.
    if (position.quantity_.coefficient() == 0) {
      continue;
    }
    note(position.instrument_);
  }
  for (const auto& working : portfolio.working_) {
    if (order_is_open(working.order_.status_)) {
      note(working.order_.instrument_);
    }
  }
  if (candidate != nullptr) {
    note(candidate->instrument_);
  }

  auto total = zero;
  for (const auto& id : instruments) {
    const InstrumentBasis* resolved = nullptr;
    for (const auto& entry : basis) {
      if (!(entry.instrument_ == id)) {
        continue;
      }
      if (resolved != nullptr) {
        // Two answers for one instrument. Picking either would be picking one of two currencies or
        // two marks, and the caller resolved it twice for a reason this cannot see.
        return std::nullopt;
      }
      resolved = &entry;
    }
    if (resolved == nullptr) {
      return std::nullopt;
    }
    if (!(resolved->currency_ == currency)) {
      continue;
    }
    // A mark must be positive: zero would omit the position, and a negative one (from bad market
    // data) would offset real holdings.
    if (!decimal_less(zero, resolved->price_)) {
      return std::nullopt;
    }

    // Summed over every row: a holding can be split across lots.
    auto held = zero;
    for (const auto& position : portfolio.positions_) {
      if (!(position.instrument_ == id)) {
        continue;
      }
      const auto sum = decimal_add(held, position.quantity_);
      if (!sum) {
        return std::nullopt;
      }
      held = *sum;
    }
    auto buys = zero;
    auto sells = zero;
    auto price = resolved->price_;
    for (const auto& working : portfolio.working_) {
      if (!(working.order_.instrument_ == id) || !order_is_open(working.order_.status_)) {
        continue;
      }
      const auto remaining = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_);
      if (!remaining) {
        return std::nullopt;
      }
      // A row filled beyond its quantity refuses (`working_quantity` skips it): a limit cannot infer
      // from an inconsistent snapshot that nothing is outstanding.
      if (decimal_less(*remaining, zero)) {
        return std::nullopt;
      }
      const auto sum = decimal_add(working.order_.side_ == OrderSide::BUY ? buys : sells, *remaining);
      if (!sum) {
        return std::nullopt;
      }
      (working.order_.side_ == OrderSide::BUY ? buys : sells) = *sum;
      if (working.order_.price_) {
        // A non-positive price would silently lose to the mark in `max`; refuse instead.
        if (!decimal_less(zero, *working.order_.price_)) {
          return std::nullopt;
        }
        price = risk_detail::decimal_max(price, *working.order_.price_);
      }
    }

    if (candidate != nullptr && candidate->instrument_ == id) {
      // The candidate's price applies under both contributions, its quantity only under one, so the
      // two projections differ only by the quantity.
      const auto stated = candidate->price_ ? candidate->price_ : candidate_price;
      if (stated) {
        // Validated, not just offered to `max`: a negative price would vanish into the mark here and
        // pass `max_order_notional_` as a negative value. Nothing upstream rules it out.
        if (!decimal_less(zero, *stated)) {
          return std::nullopt;
        }
        price = risk_detail::decimal_max(price, *stated);
      }
      bool adds_quantity = false;
      switch (contribution) {
        case CandidateContribution::QUANTITY_AND_PRICE:
          adds_quantity = true;
          break;
        case CandidateContribution::PRICE_ONLY:
          break;
      }
      if (adds_quantity) {
        const auto sum = decimal_add(candidate->side_ == OrderSide::BUY ? buys : sells, candidate->quantity_);
        if (!sum) {
          return std::nullopt;
        }
        (candidate->side_ == OrderSide::BUY ? buys : sells) = *sum;
      }
    }

    const auto if_bought = decimal_add(held, buys);
    const auto if_sold = decimal_subtract(held, sells);
    if (!if_bought || !if_sold) {
      return std::nullopt;
    }
    const auto long_side = risk_detail::decimal_magnitude(*if_bought);
    const auto short_side = risk_detail::decimal_magnitude(*if_sold);
    if (!long_side || !short_side) {
      return std::nullopt;
    }
    const auto projected = risk_detail::decimal_max(*long_side, *short_side);

    const auto value = decimal_multiply(projected, price);
    if (!value) {
      return std::nullopt;
    }
    const auto running = decimal_add(total, *value);
    if (!running) {
      return std::nullopt;
    }
    total = *running;
  }
  return total;
}

/**
 * @brief Tests whether an order only reduces a held position toward flat, without passing through it.
 *
 * With `held` the net quantity over every position row, the closing side SELL for a long and BUY
 * for a short, and `resting` the remaining quantity of open orders on the closing side:
 *
 * ```
 * true  ⟺  held ≠ 0  ∧  order.side_ is the closing side  ∧  order.quantity_ > 0
 *          ∧  resting + order.quantity_ ≤ |held|
 * ```
 *
 * `RiskGuard` uses this to let a closing order through `RiskLimits::permitted_phases_`.
 *
 * @tparam Broker The portfolio's broker tag.
 * @param portfolio The positions and working orders.
 * @param order The planned order.
 * @return Whether the order closes toward flat, or `std::nullopt` if that cannot be established
 *   (arithmetic that does not fit, or a working order filled beyond its quantity); treat
 *   `std::nullopt` as `false`.
 */
template <typename Broker>
std::optional<bool> closes_toward_flat(const Portfolio<Broker>& portfolio, const PlannedOrder& order) {
  const auto zero = Decimal64::parse("0");

  // Net across every row; a split holding must not understate `held`.
  const auto net = portfolio.net_quantity_of(order.instrument_);
  if (!net) {
    return std::nullopt;
  }
  const auto held = *net;
  // No row and a row of zero both mean nothing to close.
  if (held.coefficient() == 0) {
    return false;
  }

  // A non-positive quantity closes nothing; a negative one would otherwise satisfy the bound below.
  if (!decimal_less(zero, order.quantity_)) {
    return false;
  }

  const auto closing = decimal_less(zero, held) ? OrderSide::SELL : OrderSide::BUY;
  if (order.side_ != closing) {
    return false;
  }

  auto resting = zero;
  for (const auto& working : portfolio.working_) {
    if (!(working.order_.instrument_ == order.instrument_) || working.order_.side_ != closing) {
      continue;
    }
    if (!order_is_open(working.order_.status_)) {
      continue;
    }
    const auto remaining = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_);
    // A row filled beyond its quantity is a contradiction; refuse rather than count zero.
    if (!remaining || decimal_less(*remaining, zero)) {
      return std::nullopt;
    }
    const auto sum = decimal_add(resting, *remaining);
    if (!sum) {
      return std::nullopt;
    }
    resting = *sum;
  }

  const auto committed = decimal_add(resting, order.quantity_);
  if (!committed) {
    return std::nullopt;
  }
  const auto magnitude = risk_detail::decimal_magnitude(held);
  if (!magnitude) {
    return std::nullopt;
  }
  // `<=`: an order that reaches flat exactly is a close (the usual liquidation case).
  return !decimal_less(*magnitude, *committed);
}

/**
 * @brief What `RiskGuard::evaluate` concluded about one order.
 *
 * `admitted_outside_phase_` is `true` only for a permitted order that got past
 * `RiskLimits::permitted_phases_` by closing toward flat. It does not mean the order was sent: the
 * broker can still reject it.
 */
struct GuardDecision {
  /** @brief Why the order was refused, or `std::nullopt` if it was permitted. */
  std::optional<GuardRejection> rejection_;
  /**
   * @brief Whether the order was permitted only because it closes toward flat in a phase the
   *   limits do not permit. Always `false` for a refusal.
   */
  bool admitted_outside_phase_{false};
};

/**
 * @brief Checks planned orders against `RiskLimits`, holds trading stopped, and counts orders sent.
 *
 * The rate limit counts what was recorded with `note_sent`. The runner records every placement or
 * cancellation that was sent or may have been sent: accepted, rejected by the broker, or of unknown
 * outcome. A request refused before sending (by this guard, the binding or the adapter) is not
 * recorded, so it uses no budget.
 *
 * @note Not thread-safe; one guard belongs to one runner.
 */
class RiskGuard {
public:
  /**
   * @brief Constructs a guard.
   * @param limits The limits to enforce.
   */
  explicit RiskGuard(RiskLimits limits = {})
  : limits_(std::move(limits)),
    checks_selling_(limits_.enforce_sellable_quantity_ || limits_.run_budget_.has_value()),
    checks_spending_(limits_.enforce_buying_power_ || limits_.run_budget_.has_value()) {}

  /**
   * @brief Copies a guard.
   * @param other The guard to copy.
   */
  RiskGuard(const RiskGuard& other) = default;
  /**
   * @brief Moves a guard.
   * @param other The guard to move from.
   */
  RiskGuard(RiskGuard&& other) = default;
  /**
   * @brief Replaces this guard with a move of another.
   * @param other The guard to move from.
   * @return This guard.
   */
  RiskGuard& operator=(RiskGuard&& other) = default;

  /**
   * @brief Replaces this guard with a copy of another, entirely or not at all.
   * @param other The guard to copy.
   * @return This guard.
   * @throws std::bad_alloc If the copy cannot be made; this guard is then unchanged.
   */
  RiskGuard& operator=(const RiskGuard& other) {
    // Copied whole first: a send list copied in part could be out of time order while still marked in it.
    if (this != &other) {
      RiskGuard copy(other);
      *this = std::move(copy);
    }
    return *this;
  }

  /**
   * @brief Returns the limits.
   * @return The limits in force.
   */
  const RiskLimits& limits() const noexcept {
    return limits_;
  }

  /**
   * @brief Tests whether a phase permits opening or increasing a position.
   * @param phase The market phase.
   * @return `true` if `RiskLimits::permitted_phases_` is absent or contains `phase`.
   */
  bool permits_phase(MarketPhase phase) const noexcept {
    if (!limits_.permitted_phases_) {
      return true;
    }
    for (const auto allowed : *limits_.permitted_phases_) {
      if (allowed == phase) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Takes the operator's hold (`HaltReason::OPERATOR`): no order or cancellation is sent
   *   until `resume()`.
   *
   * This only stops sending ("quiesce"): working orders and positions are left as they are. To
   * also cancel working orders or close positions, use `runner::StopIntent` with
   * `runner::PollingLoop`, or a strategy such as `vanilla::TimedLiquidation`. The hold is in memory;
   * `runner::FileStopChannel` and `runner::SignalStop` stop a running process from outside.
   */
  void halt() noexcept {
    held_[index_of(HaltReason::OPERATOR)] = true;
  }

  /**
   * @brief Takes a hold, with a description.
   *
   * Taking a hold that is already held replaces its detail; one `resume(reason)` releases it however
   * many times it was taken.
   *
   * @param reason The hold.
   * @param detail A description for the operator, returned by `hold_detail`.
   */
  void halt(HaltReason reason, std::string detail) {
    held_[index_of(reason)] = true;
    details_[index_of(reason)] = std::move(detail);
  }

  /**
   * @brief Takes a hold, with no description.
   * @param reason The hold.
   */
  void halt(HaltReason reason) {
    halt(reason, std::string());
  }

  /** @brief Releases the operator's hold; other holds, if any, still stop trading. */
  void resume() noexcept {
    resume(HaltReason::OPERATOR);
  }

  /**
   * @brief Releases a hold; releasing one that is not held does nothing.
   * @param reason The hold.
   */
  void resume(HaltReason reason) noexcept {
    held_[index_of(reason)] = false;
    details_[index_of(reason)].clear();
  }

  /**
   * @brief Tests whether any hold is in force.
   * @return `true` if at least one hold is held.
   */
  bool halted() const noexcept {
    for (const auto held : held_) {
      if (held) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Tests whether one hold is in force.
   * @param reason The hold.
   * @return `true` if it is held.
   */
  bool halted(HaltReason reason) const noexcept {
    return held_[index_of(reason)];
  }

  /**
   * @brief Returns the hold that stops placements, the most fundamental first.
   * @return The first held reason in `HaltReason` order, or `std::nullopt` if none.
   */
  std::optional<HaltReason> blocking_reason() const noexcept {
    return blocking_reason_for(InstructionClass::OPENING);
  }

  /**
   * @brief Returns the hold that stops an instruction class, the most fundamental first.
   * @param klass The instruction class; see `halt_blocks`.
   * @return The first held reason that stops `klass`, or `std::nullopt` if none.
   */
  std::optional<HaltReason> blocking_reason_for(InstructionClass klass) const noexcept {
    for (std::size_t index = 0; index < halt_reason_count; ++index) {
      const auto reason = static_cast<HaltReason>(index);
      if (held_[index] && halt_blocks(reason, klass)) {
        return reason;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Returns the description given with a hold.
   * @param reason The hold.
   * @return The detail, or an empty string if there is none or the hold is not held.
   */
  const std::string& hold_detail(HaltReason reason) const noexcept {
    return details_[index_of(reason)];
  }

  /**
   * @brief Returns every hold in force.
   * @return The held reasons, most fundamental first.
   */
  std::vector<HaltReason> holds() const {
    std::vector<HaltReason> held;
    for (std::size_t index = 0; index < halt_reason_count; ++index) {
      if (held_[index]) {
        held.push_back(static_cast<HaltReason>(index));
      }
    }
    return held;
  }

  /**
   * @brief Records that a request was sent, for the rate limit.
   *
   * The runner calls it for each placement or cancellation the broker accepted, rejected, or may
   * have received (unknown outcome).
   *
   * @param when When the order was sent.
   */
  void note_sent(std::chrono::system_clock::time_point when) {
    // While every send arrives in time order, `recent_orders` can binary-search; a clock that
    // stepped back makes it scan, as it always did.
    if (!sent_.empty() && when < sent_.back()) {
      in_time_order_ = false;
    }
    sent_.push_back(when);
    ++total_sent_;
  }

  /**
   * @brief Returns how many orders have been recorded with `note_sent` over the guard's life.
   *
   * Unlike `recent_orders`, it never decreases, so a caller can difference it across a call to
   * learn whether anything was sent in between.
   *
   * @return The total count.
   */
  std::size_t total_sent() const noexcept {
    return total_sent_;
  }

  /**
   * @brief Returns how many orders were sent within `RiskLimits::rate_window_` before an instant.
   * @param now The instant to count back from.
   * @return The count.
   */
  std::size_t recent_orders(std::chrono::system_clock::time_point now) const {
    const auto in_window = [&](std::chrono::system_clock::time_point when) {
      return now - when < limits_.rate_window_;
    };
    if (!in_time_order_) {
      return static_cast<std::size_t>(std::count_if(sent_.begin(), sent_.end(), in_window));
    }
    // In time order, the sends outside the window come first; the same test finds where they end.
    const auto first =
      std::partition_point(sent_.begin(), sent_.end(), [&](std::chrono::system_clock::time_point when) {
        return !in_window(when);
      });
    return static_cast<std::size_t>(sent_.end() - first);
  }

  /**
   * @brief Forgets sends recorded before an instant, to bound memory.
   * @param cutoff Sends earlier than this are dropped; pass a time at least `rate_window_` ago.
   */
  void forget_before(std::chrono::system_clock::time_point cutoff) {
    while (!sent_.empty() && sent_.front() < cutoff) {
      sent_.pop_front();
    }
  }

  /**
   * @brief Checks one planned order against the limits and holds.
   *
   * @tparam Broker The portfolio's broker tag.
   * @param order The order to check.
   * @param portfolio The current positions and working orders.
   * @param inputs The instant, phase, prices and currencies the checks need.
   * @return Why the order is refused, or `std::nullopt` if it is permitted.
   * @see evaluate, which also reports whether the order was admitted by the closing exception.
   */
  template <typename Broker>
  std::optional<GuardRejection> check(
    const PlannedOrder& order, const Portfolio<Broker>& portfolio, const GuardInputs& inputs
  ) const {
    return evaluate(order, portfolio, inputs).rejection_;
  }

  /**
   * @brief Checks one planned order, and reports whether only the closing exception admitted it.
   *
   * Checks run in this order and the first failure is reported: holds, permitted universe,
   * permitted phases (with the closing-toward-flat exception), order rate, order notional,
   * sellable quantity, buying power, position ceiling, gross exposure ceiling.
   *
   * With `RiskLimits::run_budget_`, the sellable and buying-power checks also apply the run's bounds (the
   * broker's figures still only where enforced), and refuse naming the smaller bound, the broker's on a tie.
   * A budgeted market buy may be refused by an earlier check (`VALUE_NOT_KNOWN` without a reference price
   * where another limit needs a value, or `ORDER_NOTIONAL_TOO_LARGE`) before
   * `GuardReason::RUN_BUY_NEEDS_LIMIT`. A caller that sets a budget but fills no `GuardInputs::run_cash_` or
   * `GuardInputs::run_holding_` has every buy and sell refused, as `RUN_LEDGER_UNSETTLED` unless an earlier
   * check refuses first.
   *
   * @tparam Broker The portfolio's broker tag.
   * @param order The order to check.
   * @param portfolio The current positions and working orders.
   * @param inputs The instant, phase, prices and currencies the checks need.
   * @return The decision.
   */
  template <typename Broker>
  GuardDecision evaluate(
    const PlannedOrder& order, const Portfolio<Broker>& portfolio, const GuardInputs& inputs
  ) const {
    const auto refuse = [&order](GuardReason reason) {
      GuardRejection rejection;
      rejection.reason_ = reason;
      rejection.instrument_ = order.instrument_;
      rejection.tag_ = order.tag_;
      return GuardDecision{std::optional<GuardRejection>(rejection), false};
    };
    const auto refuse_with = [&order](GuardReason reason, Decimal64 limit, Decimal64 attempted) {
      GuardRejection rejection;
      rejection.reason_ = reason;
      rejection.instrument_ = order.instrument_;
      rejection.tag_ = order.tag_;
      rejection.limit_ = std::move(limit);
      rejection.attempted_ = std::move(attempted);
      return GuardDecision{std::optional<GuardRejection>(rejection), false};
    };
    // Set by the phase check and reported only if the order survives every later limit, since
    // `refuse` and `refuse_with` both build a decision with it false. An order the exception
    // admitted and a ceiling then refused was not admitted.
    bool admitted_outside_phase = false;

    if (const auto holding = blocking_reason()) {
      auto decision = refuse(GuardReason::TRADING_HALTED);
      decision.rejection_->halt_reason_ = *holding;
      return decision;
    }

    if (limits_.permitted_universe_) {
      bool permitted = false;
      for (const auto& allowed : *limits_.permitted_universe_) {
        if (allowed == order.instrument_) {
          permitted = true;
          break;
        }
      }
      if (!permitted) {
        return refuse(GuardReason::INSTRUMENT_NOT_PERMITTED);
      }
    }

    if (limits_.permitted_phases_) {
      bool permitted = false;
      for (const auto phase : *limits_.permitted_phases_) {
        if (phase == inputs.phase_) {
          permitted = true;
          break;
        }
      }
      if (!permitted) {
        // The phase restricts what an order opens, not what it closes: an exit must stay expressible
        // after the session. `closes_toward_flat` is narrower than the exposure rule, which would
        // admit a sell that opens a short against a resting buy.
        const auto closing = closes_toward_flat(portfolio, order);
        // Unknown refuses: this branch only relaxes a restriction.
        if (!closing || !*closing) {
          return refuse(GuardReason::PHASE_NOT_PERMITTED);
        }
        admitted_outside_phase = true;
      }
    }

    if (limits_.max_orders_per_window_ && recent_orders(inputs.now_) >= *limits_.max_orders_per_window_) {
      return refuse_with(
        GuardReason::ORDER_RATE_EXCEEDED, Decimal64(static_cast<std::int64_t>(*limits_.max_orders_per_window_), 0),
        Decimal64(static_cast<std::int64_t>(recent_orders(inputs.now_) + 1), 0)
      );
    }

    // A market order carries no price, so its value has to come from the runner's last quote.
    // Without one, a notional or exposure limit cannot be evaluated, and a limit that cannot be
    // evaluated must refuse rather than wave the order through.
    const auto price = order.price_ ? order.price_ : inputs.reference_price_;
    const bool needs_value = limits_.max_order_notional_.has_value() || limits_.max_gross_exposure_.has_value() ||
      (limits_.enforce_buying_power_ && order.side_ == OrderSide::BUY);
    if (needs_value && !price) {
      return refuse(GuardReason::VALUE_NOT_KNOWN);
    }

    std::optional<Decimal64> value;
    if (price) {
      value = decimal_multiply(order.quantity_, *price);
      if (needs_value && !value) {
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
    }

    if (limits_.max_order_notional_ && value && decimal_less(*limits_.max_order_notional_, *value)) {
      return refuse_with(GuardReason::ORDER_NOTIONAL_TOO_LARGE, *limits_.max_order_notional_, *value);
    }

    if (checks_selling_ && order.side_ == OrderSide::SELL) {
      if (limits_.run_budget_) {
        if (auto refused = refuse_run_sell(order, portfolio, inputs)) {
          return *refused;
        }
      } else {
        // One read, so the figure and its basis come from the same row.
        const auto reading = portfolio.sellable_reading_for(order.instrument_);
        const auto& sellable = reading.quantity_;
        if (!sellable) {
          // Absent is unknown, and unknown refuses; the held quantity is not substituted.
          return refuse(GuardReason::QUANTITY_NOT_SELLABLE);
        }
        // A figure whose basis is not established refuses even though present; this fails closed for
        // an adapter the library has never seen.
        if (!reading.basis_.is_enforceable()) {
          return refuse(GuardReason::SELLABLE_BASIS_UNESTABLISHED);
        }
        // Every open sell (the broker's rows and this library's reservations) counts against the
        // figure. That over-refuses where the broker already deducted them and never under-refuses;
        // see `SellableBasis::DEDUCT`.
        const auto resting = portfolio.working_quantity(order.instrument_, OrderSide::SELL);
        const auto committed = decimal_add(resting, order.quantity_);
        if (!committed) {
          return refuse(GuardReason::QUANTITY_NOT_SELLABLE);
        }
        if (decimal_less(*sellable, *committed)) {
          return refuse_with(GuardReason::QUANTITY_NOT_SELLABLE, *sellable, *committed);
        }
      }
    }

    if (checks_spending_ && order.side_ == OrderSide::BUY) {
      if (limits_.run_budget_) {
        if (auto refused = refuse_run_buy(order, portfolio, inputs, value)) {
          return *refused;
        }
      } else {
        if (!value) {
          // A price is present (`needs_value`), so the multiplication itself failed.
          return refuse(GuardReason::VALUE_NOT_KNOWN);
        }
        if (!inputs.order_currency_) {
          return refuse(GuardReason::INSUFFICIENT_BUYING_POWER);
        }
        const auto available = portfolio.buying_power_in(*inputs.order_currency_);
        if (!available) {
          // Absent is unknown, and unknown refuses. (`cash_in` is empty live, so no fallback.)
          return refuse(GuardReason::INSUFFICIENT_BUYING_POWER);
        }
        if (decimal_less(*available, *value)) {
          return refuse_with(GuardReason::INSUFFICIENT_BUYING_POWER, *available, *value);
        }
      }
    }

    if (limits_.max_position_quantity_) {
      const auto zero = Decimal64::parse("0");
      // Net across every position row, as reconciliation sizes: a split holding read from its first
      // row would refuse an exit to flat and pass a buy that both lots together put over the ceiling.
      const auto net = portfolio.net_quantity_of(order.instrument_);
      if (!net) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto held = *net;
      // Orders already working on this side count, so two buys that each fit cannot together exceed
      // the ceiling. A resting opposite-side order is not counted as relief.
      const auto pending = portfolio.working_quantity(order.instrument_, order.side_);
      const auto committed = decimal_add(pending, order.quantity_);
      if (!committed) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto signed_quantity =
        order.side_ == OrderSide::BUY ? std::optional<Decimal64>(*committed) : decimal_subtract(zero, *committed);
      if (!signed_quantity) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto resulting = decimal_add(held, *signed_quantity);
      if (!resulting) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      // The ceiling is on magnitude, so a short of the same size is limited as a long is.
      const auto magnitude =
        decimal_less(*resulting, zero) ? decimal_subtract(zero, *resulting) : std::optional<Decimal64>(*resulting);
      if (!magnitude) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      if (decimal_less(*limits_.max_position_quantity_, *magnitude)) {
        return refuse_with(GuardReason::POSITION_LIMIT_EXCEEDED, *limits_.max_position_quantity_, *magnitude);
      }
    }

    if (limits_.max_gross_exposure_) {
      // Per currency: an unresolved order currency refuses.
      if (!inputs.order_currency_) {
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
      const auto resulting = projected_gross_exposure(
        portfolio, inputs.exposure_basis_, *inputs.order_currency_, &order, price,
        CandidateContribution::QUANTITY_AND_PRICE
      );
      if (!resulting) {
        // The basis does not describe the portfolio, or the arithmetic did not fit.
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
      if (decimal_less(*limits_.max_gross_exposure_, *resulting)) {
        // Over the ceiling, refuse only an order that raises the projection, so an account already
        // over it can still sell out. Compare against `PRICE_ONLY` (same prices, no quantity), not
        // `nullptr`, so the difference is the order's quantity and not its limit price.
        const auto without = projected_gross_exposure(
          portfolio, inputs.exposure_basis_, *inputs.order_currency_, &order, price, CandidateContribution::PRICE_ONLY
        );
        if (!without) {
          return refuse(GuardReason::VALUE_NOT_KNOWN);
        }
        if (decimal_less(*without, *resulting)) {
          return refuse_with(GuardReason::GROSS_EXPOSURE_EXCEEDED, *limits_.max_gross_exposure_, *resulting);
        }
      }
    }

    return GuardDecision{std::nullopt, admitted_outside_phase};
  }

private:
  // A decision refusing `order` for `reason`, with the figures where the reason has them.
  static GuardDecision refusal(
    const PlannedOrder& order, GuardReason reason, std::optional<Decimal64> limit = std::nullopt,
    std::optional<Decimal64> attempted = std::nullopt
  ) {
    GuardRejection rejection;
    rejection.reason_ = reason;
    rejection.instrument_ = order.instrument_;
    rejection.tag_ = order.tag_;
    rejection.limit_ = std::move(limit);
    rejection.attempted_ = std::move(attempted);
    return GuardDecision{std::optional<GuardRejection>(std::move(rejection)), false};
  }

  // The selling check with a run budget: the smaller of the broker's sellable room (where enforced) and the run's
  // own shares less its open sells, the broker's named on a tie.
  template <typename Broker>
  std::optional<GuardDecision> refuse_run_sell(
    const PlannedOrder& order, const Portfolio<Broker>& portfolio, const GuardInputs& inputs
  ) const {
    const auto zero = Decimal64(0, 0);
    if (!decimal_less(zero, order.quantity_)) {
      return refusal(order, GuardReason::VALUE_NOT_KNOWN);
    }
    // The broker's figures as main's block reads them.
    std::optional<Decimal64> sellable;
    std::optional<Decimal64> resting;
    std::optional<Decimal64> committed;
    if (limits_.enforce_sellable_quantity_) {
      const auto reading = portfolio.sellable_reading_for(order.instrument_);
      sellable = reading.quantity_;
      if (!sellable) {
        return refusal(order, GuardReason::QUANTITY_NOT_SELLABLE);
      }
      if (!reading.basis_.is_enforceable()) {
        return refusal(order, GuardReason::SELLABLE_BASIS_UNESTABLISHED);
      }
      resting = portfolio.working_quantity(order.instrument_, OrderSide::SELL);
      committed = decimal_add(*resting, order.quantity_);
      if (!committed) {
        return refusal(order, GuardReason::QUANTITY_NOT_SELLABLE);
      }
    }
    if (!inputs.run_holding_) {
      return refusal(order, GuardReason::RUN_LEDGER_UNSETTLED);
    }
    const bool broker_over = sellable && decimal_less(*sellable, *committed);
    const bool run_over = decimal_less(*inputs.run_holding_, order.quantity_);
    if (!broker_over && !run_over) {
      return std::nullopt;
    }
    bool names_run = !broker_over;
    if (broker_over && run_over) {
      // The broker's room is formed only here; a subtraction that fails names the broker.
      const auto room = decimal_subtract(*sellable, *resting);
      names_run = room && decimal_less(*inputs.run_holding_, *room);
    }
    if (names_run) {
      return refusal(
        order, GuardReason::RUN_HOLDING_EXCEEDED, risk_detail::decimal_max(*inputs.run_holding_, zero), order.quantity_
      );
    }
    return refusal(order, GuardReason::QUANTITY_NOT_SELLABLE, *sellable, *committed);
  }

  // The spending check with a run budget: the smaller of the broker's buying power (where enforced) and the run's
  // room, its cash less this buy's estimated fee, the broker's named on a tie. `value` is quantity x limit price.
  template <typename Broker>
  std::optional<GuardDecision> refuse_run_buy(
    const PlannedOrder& order, const Portfolio<Broker>& portfolio, const GuardInputs& inputs,
    const std::optional<Decimal64>& value
  ) const {
    const auto zero = Decimal64(0, 0);
    if (!decimal_less(zero, order.quantity_)) {
      return refusal(order, GuardReason::VALUE_NOT_KNOWN);
    }
    // Not a conditional limit, which carries a price but becomes a market order at the close.
    if (
      (order.type_ != OrderType::LIMIT && order.type_ != OrderType::STOP_LIMIT) || !order.price_ ||
      !decimal_less(zero, *order.price_)
    ) {
      return refusal(order, GuardReason::RUN_BUY_NEEDS_LIMIT);
    }
    if (!value) {
      return refusal(order, GuardReason::VALUE_NOT_KNOWN);
    }
    std::optional<Decimal64> available;
    if (limits_.enforce_buying_power_) {
      if (!inputs.order_currency_) {
        return refusal(order, GuardReason::INSUFFICIENT_BUYING_POWER);
      }
      available = portfolio.buying_power_in(*inputs.order_currency_);
      if (!available) {
        return refusal(order, GuardReason::INSUFFICIENT_BUYING_POWER);
      }
    }
    // A direct caller bypasses the runner's validation, so each run figure is checked before any arithmetic.
    if (!limits_.run_fees_ || !inputs.run_cash_ || !(inputs.run_cash_->currency_ == limits_.run_budget_->currency_)) {
      return refusal(order, GuardReason::RUN_LEDGER_UNSETTLED);
    }
    if (!inputs.order_currency_) {
      return refusal(order, GuardReason::VALUE_NOT_KNOWN);
    }
    if (!(*inputs.order_currency_ == inputs.run_cash_->currency_)) {
      // Amounts in two currencies are never compared, so there is no attempted figure.
      return refusal(order, GuardReason::RUN_BUDGET_EXCEEDED, inputs.run_cash_->amount_);
    }
    const auto fee = limits_.run_fees_->estimate_commission(*value);
    const auto room = fee ? decimal_subtract(inputs.run_cash_->amount_, *fee) : std::nullopt;
    if (!room) {
      return refusal(order, GuardReason::VALUE_NOT_KNOWN);
    }
    const bool broker_over = available && decimal_less(*available, *value);
    const bool run_over = decimal_less(*room, *value);
    if (broker_over && (!run_over || !decimal_less(*room, *available))) {
      return refusal(order, GuardReason::INSUFFICIENT_BUYING_POWER, *available, *value);
    }
    if (run_over) {
      return refusal(order, GuardReason::RUN_BUDGET_EXCEEDED, *room, *value);
    }
    return std::nullopt;
  }

  // `reason`'s slot in the hold arrays.
  static constexpr std::size_t index_of(HaltReason reason) noexcept {
    return static_cast<std::size_t>(reason);
  }

  RiskLimits limits_;
  // Which holds are in force, indexed by `HaltReason`; declaration order is priority order.
  std::array<bool, halt_reason_count> held_{};
  // Each hold's detail, empty where none or not held.
  std::array<std::string, halt_reason_count> details_;
  // Timestamps of orders sent, oldest first; a sliding window, not a counter reset on a boundary.
  std::deque<std::chrono::system_clock::time_point> sent_;
  // Whether `sent_` has never had a send earlier than the one before it.
  bool in_time_order_{true};
  // Whether `evaluate` enters the sellable and the buying-power blocks; fixed with `limits_`.
  bool checks_selling_{true};
  bool checks_spending_{false};
  std::size_t total_sent_{0};
};

} // namespace strategy
} // namespace algotradelib
