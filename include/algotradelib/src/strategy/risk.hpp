#pragma once

#include "./event.hpp"
#include "./portfolio.hpp"
#include "./reconcile.hpp"

#include <algotradelib/core>

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
 * Who or what is holding trading stopped.
 *
 * A halt used to be one `bool`, which was exactly right when the only caller was a person deciding
 * to stop trading: one holder, one bool. It stopped being right once machinery held it too — a
 * polling loop's gate, a journal that cannot be written — because two holders of one bool cannot
 * tell each other apart, and the one that clears it clears the other's intent with it.
 *
 * So a hold names itself, and trading resumes only when every hold is released. The enumerators are
 * ordered **most fundamental first**, and `RiskGuard::blocking_reason` relies on that order: an
 * operator reading a refusal sees the reason that most needs their attention rather than whichever
 * condition happens to be checked first. A new reason goes where its gravity puts it, not at the end.
 */
enum class HaltReason {
  /** A person stopped trading, through `halt()` with no argument. Cleared only by a person. */
  OPERATOR,
  /**
   * Local machinery cannot be trusted, so nothing may be sent through it.
   *
   * The order journal is the motivating case: a runner that cannot record what it is about to do
   * must not do it, because every later order would fail the same way and produce a refusal per bar
   * that reads like the broker declining -- when in fact nothing left the process. Sticky, carries
   * the reason as its detail, and released only by `runner::Runner::resume_after_local_failure`.
   */
  LOCAL_FAILURE,
  /** A stop intent is being honoured and the run is on its way out. */
  STOPPING,
  /**
   * The broker's order list has not been read since this process started.
   *
   * The cheapest and most valuable half of order-journal reconciliation, and it needs no
   * persistence at all. A runner that has just started cannot know whether a *previous* process
   * died mid-POST, and an empty `runner::IndeterminacyLedger` does not mean nothing is owed -- it
   * means nothing is remembered. Until the list has been read once, sending could double a position
   * left by a predecessor.
   */
  ORDER_LIST_UNREAD,
  /**
   * The holdings snapshot is older than the configured maximum.
   *
   * Reconciling a target against a stale portfolio is how a position gets double-sized by a route
   * that `working_quantity` does not close: the runner subtracts a position it no longer holds, or
   * fails to subtract one it acquired since.
   */
  PORTFOLIO_STALE,
  /**
   * The broker has not said what may be spent, and a check needs it.
   *
   * Held only by a loop that was asked to enforce a buying-power limit and has no reading to
   * enforce it against -- because the poll is not configured, or has not returned yet, or its
   * answer has aged out. Sizing a buy against an unknown budget is the thing that check exists to
   * stop, so the hold stands rather than the order going out on an estimate.
   */
  BUYING_POWER_UNREAD,
  /**
   * A poll kept failing, so the loop gave up rather than spinning.
   *
   * Distinct from `STOPPING` because nobody asked for it: the loop decided, and an operator reading
   * the report needs to know which of the two happened.
   */
  POLL_FAILING,
};

/** How many `HaltReason` enumerators there are, for the guard's fixed-capacity storage. */
inline constexpr std::size_t halt_reason_count = 7;

/** Returns a stable lowercase name for `reason`, for logs and structured reporting. */
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
    case HaltReason::PORTFOLIO_STALE:
      return "portfolio_stale";
    case HaltReason::BUYING_POWER_UNREAD:
      return "buying_power_unread";
    case HaltReason::POLL_FAILING:
      return "poll_failing";
  }
  return "unknown";
}

/**
 * The limits an operator imposes, checked before anything is sent.
 *
 * Separate from reconciliation, and the separation is the point. Reconciliation answers "what
 * instruction expresses this decision"; a guard answers "is this instruction permitted". The
 * failures read differently in a log — `RejectReason` means the order was not placed, most often
 * because the library could not build one, `GuardReason` means the operator forbade one — and
 * conflating them would make a configuration mistake look like a data problem.
 *
 * **A guard rejects; it never resizes.** Halving an order that exceeded a notional limit would
 * place a position the strategy never sized, against a signal it never gave, and the strategy
 * would then reconcile against that position and compound the error. Refusing leaves the strategy
 * to decide, which is where the decision belongs.
 */

/** Which limit refused an order. */
enum class GuardReason {
  /** The kill switch is on. */
  TRADING_HALTED,
  /** The instrument is outside the permitted universe. */
  INSTRUMENT_NOT_PERMITTED,
  /** The market is not in a phase the operator permits trading in. */
  PHASE_NOT_PERMITTED,
  /** The order's value exceeds the per-order ceiling. */
  ORDER_NOTIONAL_TOO_LARGE,
  /** The resulting position would exceed the per-instrument ceiling. */
  POSITION_LIMIT_EXCEEDED,
  /** The resulting gross exposure would exceed the portfolio ceiling. */
  GROSS_EXPOSURE_EXCEEDED,
  /** Too many orders have been sent in the current window. */
  ORDER_RATE_EXCEEDED,
  /**
   * A limit needed a value and could not be given one.
   *
   * Three ways: no price to value the order with, no resolved currency for a limit that is
   * per-currency, or a `GuardInputs::exposure_basis_` that does not describe every instrument the
   * portfolio holds or has working. All three are Decision 6 -- a limit the caller stated is
   * honored or refused, never evaluated against a figure the library made up.
   */
  VALUE_NOT_KNOWN,
  /** The sell exceeds what the broker says is sellable, or the broker did not say. */
  QUANTITY_NOT_SELLABLE,
  /** The buy costs more than the broker says may be spent, or the broker did not say. */
  INSUFFICIENT_BUYING_POWER,
};

/** Returns a stable lowercase name for `reason`, for logs and structured reporting. */
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
    case GuardReason::INSUFFICIENT_BUYING_POWER:
      return "insufficient_buying_power";
  }
  return "unknown";
}

/**
 * One order a guard refused, with enough to act on rather than only to read.
 *
 * `limit_` and `attempted_` are carried so an operator can see how far over the order was without
 * reconstructing the arithmetic, and so a strategy can adapt rather than retry the same size.
 */
struct GuardRejection {
  /** Which limit refused the order. */
  GuardReason reason_{GuardReason::TRADING_HALTED};
  /** The instrument the refused order was for. */
  InstrumentId instrument_;
  /** The strategy's own label on the intent it came from. */
  std::string tag_;
  /**
   * Which hold stopped the order, when `reason_` is `TRADING_HALTED`.
   *
   * Present for that reason and absent for every other, because a halt is the one refusal whose
   * *cause* lives outside the limits — an operator, a loop's gate, a journal that failed — and
   * "trading is halted" without saying by what is the diagnostic this type exists to avoid.
   */
  std::optional<HaltReason> halt_reason_;
  /** The ceiling that was exceeded, where the reason has one. */
  std::optional<Decimal64> limit_;
  /** What the order would have reached, where the reason has one. */
  std::optional<Decimal64> attempted_;
};

/**
 * What the operator permits.
 *
 * Every limit is optional, and absent means unlimited — except the two where an unlimited default
 * would be unsafe rather than merely permissive, which is why they are shaped differently:
 *
 * - `permitted_phases_` defaults to trading only while the market is `OPEN`. An absent value
 *   explicitly means any phase, which an operator has to ask for. `MarketPhase::UNKNOWN` is
 *   therefore refused by default, which is the point: an uncovered calendar date must not become
 *   an order placed into a holiday. It refuses what an order *opens*, never what it closes; see the
 *   member.
 * - Nothing is halted to begin with, but `halt()` is one call and takes effect before every other
 *   check. A halt is not a limit and does not live here: it is held on the guard itself, by
 *   `HaltReason`, so that the operator's hold and the machinery's cannot clear one another.
 */
struct RiskLimits {
  /** Largest value a single order may have, in the currency it is priced in. */
  std::optional<Decimal64> max_order_notional_;
  /**
   * Largest absolute position, in shares, any one instrument may reach.
   *
   * Measured against the position that would exist if the order **and everything already working
   * on its side** filled, not against the last poll's holding: a ceiling that only looked at what
   * was held would pass two orders that each fit and together do not.
   *
   * **Held is the net of every position row**, via `Portfolio::net_quantity_of`. It used to be the
   * first row, which understated a holding a broker reported as two lots -- so the ceiling passed
   * orders that breached it, and, once reconciliation began sizing against the net, refused an exit
   * that reached flat because it measured the order against one lot of the position it was closing.
   */
  std::optional<Decimal64> max_position_quantity_;
  /**
   * Largest total exposure the portfolio may reach, **in the currency the order is priced in**.
   *
   * Measured as the exposure that would exist if this order and everything already outstanding
   * filled, valued at the most expensive price that could apply -- see `projected_gross_exposure`,
   * which does the arithmetic and is testable on its own.
   *
   * **A ceiling per currency, not an account-wide one.** The same number is applied independently to
   * each currency, which is `max_order_notional_`'s reading and `holdings-core`'s refusal to add
   * currencies without a rate the caller can see. So an account holding ten million of exposure in
   * each of two currencies passes a fifteen-million ceiling. `plan/todo/exchange-rates.md` is where
   * that changes; until then a converted account-wide cap is not something this expresses.
   *
   * **This limit did nothing before `ai/guard-inputs`.** Nothing in the library ever supplied the
   * current exposure, so it was compared against the value of the single order in front of it -- a
   * second `max_order_notional_` under another name, silently permitting the accumulation it exists
   * to stop. Two consequences of fixing it, both worth reading before turning it on:
   *
   * - It now **refuses** orders it used to pass, which is the point.
   * - It now **permits** one shape it used to refuse. An order that does not raise reachable
   *   exposure is allowed at any size, however far over the ceiling the account already is -- which
   *   is what makes an exit expressible. A sell against a long **up to flat** is the ordinary case.
   *   It is not every order opposing a position: selling 300 against a long of 100 reaches a short
   *   of 200, which is more reachable exposure than the 100 it started from, and is refused on the
   *   same rule. A caller who was relying on this limit to cap a single order wants
   *   `max_order_notional_`, which is unchanged.
   *
   * An order that cannot be valued, or a portfolio `GuardInputs::exposure_basis_` does not fully
   * describe, is refused with `GuardReason::VALUE_NOT_KNOWN`.
   */
  std::optional<Decimal64> max_gross_exposure_;
  /** Most orders that may be sent within `rate_window_`. */
  std::optional<std::size_t> max_orders_per_window_;
  /** The window `max_orders_per_window_` counts over. */
  std::chrono::seconds rate_window_{60};
  /**
   * Whether a sell is checked against what the broker says is sellable.
   *
   * Off by default, because it changes what an existing program does: a broker that does not
   * publish the figure leaves it absent, and this check refuses rather than assuming the held
   * quantity is sellable. A caller that has always sized from positions keeps doing so until it
   * asks for this.
   *
   * Unlike every other member here it is not a number the operator chose. It is a switch, because
   * the *limit* is the broker's answer rather than the operator's policy — which is the whole point
   * of asking the broker.
   */
  bool enforce_sellable_quantity_{false};
  /**
   * Whether a buy is checked against what the broker says may be spent.
   *
   * Off by default, and a switch rather than a number for the same reason as the sellable check: the
   * ceiling is the broker's answer, not the operator's policy.
   *
   * **It is a reading, with a reading's freshness.** The figure was true when the broker answered,
   * and a fill against a resting order moves it without telling anyone. Passing this check is
   * therefore not a guarantee the broker will accept the order; it is the difference between sizing
   * against the broker's last answer and sizing against nothing, which is what the live path did
   * before.
   */
  bool enforce_buying_power_{false};
  /** Instruments that may be traded; absent means any. */
  std::optional<std::vector<InstrumentId>> permitted_universe_;
  /**
   * Phases in which a position may be **opened or increased**; absent means any.
   *
   * **It governs what an order opens, not what it closes.** An order reducing a held position toward
   * flat is permitted whatever the phase. That is the answer `max_gross_exposure_` already gives one
   * paragraph up -- an order that does not raise reachable exposure is allowed however far over the
   * ceiling the account is -- and it applies here with more force, because a phase restriction that
   * cannot express an exit turns "the market is shut" into "you may not reduce risk". Without it the
   * default refuses an operator's flatten after the close, and `vanilla::TimedLiquidation` cannot
   * place the order it exists to place without the limit being switched off in all but spelling.
   *
   * **The exception is `closes_toward_flat`, and not the exposure rule.** "Does not raise the
   * projection" is broader: with nothing held and a buy resting, a sell of the same size leaves the
   * projection unchanged while opening a short if it alone fills. This admits an order only when it
   * opposes the instrument's *net* held quantity and cannot carry it through flat, counting what is
   * already working on the closing side. Anything it cannot establish is refused.
   *
   * **Everything else still applies to that exit.** The phase is checked third; the sellable check,
   * a buying-power limit, a position ceiling and the exposure ceiling all come after it and can each
   * refuse an order this admitted. Permission here is not permission to send.
   *
   * **What it costs, decided rather than discovered.** The order goes to a broker that will very
   * likely reject it outside its session, so a nightly flatten becomes a rejection rather than a
   * refusal -- the venue's answer instead of this library's guess, and journalled. A strategy that
   * restates a close on every pass will therefore send one per pass; `max_orders_per_window_` is the
   * lever, and `runner::Runner::sends_outside_permitted_phase` is how an operator sees it happening.
   *
   * A cancellation is not checked against this at all. `Runner::send_cancel` consults no limit but a
   * halt, because withdrawing only ever reduces exposure.
   */
  std::optional<std::vector<MarketPhase>> permitted_phases_{
    std::vector<MarketPhase>{MarketPhase::OPEN}
  };
};

/**
 * What one instrument is denominated in, and the price to value it at.
 *
 * The two facts a portfolio does not carry for itself. `HoldingPosition` carries both, but a working
 * order on an instrument with no position carries neither, and an exposure ceiling has to value
 * exactly those -- an opening buy is the ordinary case.
 */
struct InstrumentBasis {
  /** The instrument this basis is for. */
  InstrumentId instrument_;
  /** What it is denominated in. A ceiling is per currency and never converts. */
  Currency currency_;
  /**
   * The mark to value the position at.
   *
   * A starting point rather than the final say: `projected_gross_exposure` raises it to any limit
   * price that could apply to the same instrument, because a limit order can fill above the last
   * quote and a ceiling valued at the quote would under-report.
   */
  Decimal64 price_;
};

/** What a guard needs about the world that is not the order or the portfolio. */
struct GuardInputs {
  /** The instant the guard should judge against, which is what `rate_window_` counts from. */
  std::chrono::system_clock::time_point now_{};
  /** The market phase the order would go out in, which `permitted_phases_` is checked against. */
  MarketPhase phase_{MarketPhase::UNKNOWN};
  /** A price to value a market order with, since it carries none of its own. */
  std::optional<Decimal64> reference_price_;
  /**
   * What every instrument in the portfolio is denominated in and marked at.
   *
   * **Replaces a `gross_exposure_` total that nothing ever supplied.** That field was an
   * `std::optional<Decimal64>` the caller was expected to fill, and no path in the library filled
   * it, so `max_gross_exposure_` defaulted its current exposure to zero and compared the ceiling
   * against the single order in front of it. Accepting a total from outside would leave that silent
   * zero reachable; the guard computes the exposure itself now, and this is the only thing it cannot
   * work out on its own.
   *
   * A position states its own currency and mark, but a working order on an instrument with no
   * position states neither, which is why resolution is the caller's job -- the runner reads it from
   * the reference data it already consults for `order_currency_`.
   *
   * **Incomplete is refused, never skipped.** One entry per instrument the portfolio holds or has
   * working, plus the order being checked; a missing entry, or two entries for one instrument,
   * refuses with `GuardReason::VALUE_NOT_KNOWN`. An instrument omitted from the sum could have
   * belonged to the currency being checked, and under-reporting is the one direction a ceiling must
   * not err in.
   */
  std::vector<InstrumentBasis> exposure_basis_;
  /**
   * The currency the order is priced in, which a `PlannedOrder` does not carry.
   *
   * Needed only by the buying-power check, because that limit is per currency and the portfolio may
   * hold several. It comes from the instrument's reference data, which the runner already reads to
   * plan the order. Absent means the runner could not resolve the instrument, and an unresolvable
   * currency refuses rather than picking whichever reading happens to be first.
   */
  std::optional<Currency> order_currency_;
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

} // namespace risk_detail

/**
 * What a candidate order contributes to a projection.
 *
 * Exists so that "the exposure with this order" and "the exposure without it" can be compared
 * **like for like**. The valuation takes the most expensive price that could apply to an instrument,
 * and the candidate's own limit is one of those prices — so a version of this that simply omitted
 * the candidate entirely would value the two cases at different prices and report a difference that
 * is the *price*, not the order. A sell of 10 limited above the mark then looks like it adds
 * exposure to a position it can only reduce, and the ceiling refuses the exit.
 */
enum class CandidateContribution {
  /** The candidate's quantity and its price, which is the exposure if the order goes. */
  QUANTITY_AND_PRICE,
  /**
   * The candidate's price only, which is the exposure if it does not.
   *
   * Not "the portfolio as it stands": it is the portfolio valued on the same envelope the other
   * case uses, so that comparing the two isolates what the order's *quantity* does.
   */
  PRICE_ONLY,
};

/**
 * Returns the gross exposure in `currency` that would exist if `candidate` and everything already
 * outstanding filled, or nothing when it cannot be computed honestly.
 *
 * **Pure, so it is testable without a runner**, which is why the resolution it needs arrives as
 * `basis` rather than being looked up here. Nothing about a broker, a session or a clock enters.
 *
 * ## What it measures
 *
 * Per instrument, the largest absolute position reachable from here:
 *
 * ```
 * projected = max( |held + working_buys| , |held - working_sells| )
 * ```
 *
 * No relief is assumed from the opposing side. A resting sell does not offset a resting buy, because
 * treating it as relief assumes a fill that may never come, and a ceiling exists for what happens
 * when everything outstanding *does* fill. This is `max_position_quantity_`'s rule
 * (`RiskLimits::max_position_quantity_`), valued and summed instead of compared per instrument, and
 * it reduces to `|held|` when nothing is working.
 *
 * **`candidate` joins its own instrument's projection on its own side; it is not added to the
 * total.** Adding it would report a sell against a long position as *increasing* exposure, so the
 * ceiling would refuse the order that reduces it -- a limit that blocks de-risking is worse than one
 * that does nothing.
 *
 * Folding it in does **not** make a sell reduce the figure, and that is not an oversight. Hold 100
 * with a sell of 100 resting and `max(|100|, |0|)` is 100: the position is 100 until that sell fills,
 * and a ceiling that assumed the fill would be assuming exactly what a ceiling exists to survive
 * being wrong about. What folding buys is that a sell does not *raise* the figure, which is what
 * makes an exit expressible at all. `RiskGuard::check` is where the remaining half lives -- it
 * compares the projection with and without the candidate, so an order that adds nothing is permitted
 * however far over the ceiling the account already is.
 *
 * ## The price is the conservative one
 *
 * An instrument is valued at the highest price that could apply to it: its mark, and the limit price
 * of every open order on it, `candidate` included. A basis carrying only the last quote values a
 * limit buy of 100 at 100 against a cached mark of 70 as 7,000, so it passes an 8,000 ceiling and
 * may fill at 10,000. Taking the maximum over-states exposure for a limit order that fills better
 * than its limit -- refusing an order that would have been allowed -- and never the reverse. A market
 * order contributes the mark, which is an estimate and is one whatever this does.
 *
 * ## Nothing is silently omitted
 *
 * Nothing is returned when the basis does not describe the portfolio, rather than a total computed
 * over the part of it that could be resolved: a missing entry for an instrument held or working, two
 * entries for one instrument, or any decimal arithmetic that will not fit. `Portfolio::working_quantity`
 * is deliberately not used -- it skips a row whose arithmetic fails, which is right for a figure a
 * strategy reads and wrong for a limit, since a skipped row under-reports.
 *
 * Instruments in another currency contribute nothing and are not an error. `max_gross_exposure_` is
 * a ceiling *per currency*; see its documentation.
 */
template <typename Broker>
std::optional<Decimal64> projected_gross_exposure(
  const Portfolio<Broker>& portfolio,
  const std::vector<InstrumentBasis>& basis,
  const Currency& currency,
  const PlannedOrder* candidate,
  const std::optional<Decimal64>& candidate_price,
  CandidateContribution contribution
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
    // **A flat row is not exposure and must not be a reason to refuse.** `ReplayLedger` keeps a lot
    // after it returns to flat, and a broker may list a closed position, so requiring a basis for
    // one would have an instrument nobody holds any more refuse orders in an unrelated instrument.
    // Zero quantity valued at any price is zero. An instrument that is flat *and* has a working
    // order is still collected, by the loop below.
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
    // **A mark has to be positive to value anything.** Zero values a position at nothing, which is
    // the silent omission this function refuses elsewhere; a negative one is worse, because it makes
    // an instrument's exposure *negative* and lets a malformed quote offset real holdings until the
    // ceiling passes anything. `Context::set_last_price` stores whatever a bar or quote carried, so
    // this is reachable from bad market data rather than only from a caller mistake.
    if (!decimal_less(zero, resolved->price_)) {
      return std::nullopt;
    }

    // **Summed over every row, not read from the first.** `Portfolio::quantity_of` returns the first
    // matching position, and nothing dedupes what an adapter appends -- a broker reporting one
    // instrument as two lots is ordinary. Reading the first would under-report the ceiling by
    // whatever the other rows hold, which is the direction a risk limit must not err in.
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
      const auto remaining = decimal_subtract(
        working.order_.quantity_, working.order_.execution_.filled_quantity_
      );
      if (!remaining) {
        return std::nullopt;
      }
      // **A row filled beyond what it ordered refuses, where `working_quantity` skips it.** That
      // skip is right for a figure a strategy reads -- it declines to amplify a broker's
      // contradiction -- and wrong for a limit, because dropping the row takes its limit price out
      // of the valuation envelope with it while the row is still `PENDING`. The snapshot is
      // inconsistent, and a ceiling cannot infer from it that nothing is outstanding.
      if (decimal_less(*remaining, zero)) {
        return std::nullopt;
      }
      const auto sum = decimal_add(
        working.order_.side_ == OrderSide::BUY ? buys : sells, *remaining
      );
      if (!sum) {
        return std::nullopt;
      }
      (working.order_.side_ == OrderSide::BUY ? buys : sells) = *sum;
      if (working.order_.price_) {
        // A present price that is not a price. `max` would simply not pick it, which is the quiet
        // outcome: the row's own valuation would vanish into the mark and nothing would say the
        // listing was malformed.
        if (!decimal_less(zero, *working.order_.price_)) {
          return std::nullopt;
        }
        price = risk_detail::decimal_max(price, *working.order_.price_);
      }
    }

    if (candidate != nullptr && candidate->instrument_ == id) {
      // **The price applies under both contributions, the quantity only under one.** That asymmetry
      // is the whole reason `CandidateContribution` exists: raising the envelope in one case and not
      // the other would make the comparison report the price difference as though the order had
      // caused it.
      const auto stated = candidate->price_ ? candidate->price_ : candidate_price;
      if (stated) {
        // **The candidate's own price is validated, not merely offered to `max`.** A negative one
        // loses the maximum to the positive basis and disappears, so the ceiling would value the
        // order at a mark nobody quoted while `max_order_notional_` valued it at a negative number
        // that no ceiling exceeds. `Price::checked` will build a negative limit under an
        // unconstrained rule, and `Context::set_last_price` stores whatever a bar carried, so
        // neither source rules this out upstream.
        if (!decimal_less(zero, *stated)) {
          return std::nullopt;
        }
        price = risk_detail::decimal_max(price, *stated);
      }
      if (contribution == CandidateContribution::QUANTITY_AND_PRICE) {
        const auto sum = decimal_add(
          candidate->side_ == OrderSide::BUY ? buys : sells, candidate->quantity_
        );
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
 * Returns whether `order` reduces a held position toward flat without carrying it past flat, or
 * nothing when that cannot be established.
 *
 * **Pure, so it is testable without a runner or a guard**, for the reason `projected_gross_exposure`
 * is: the rule is arithmetic over a portfolio snapshot and an order, and nothing about a broker, a
 * session or a clock belongs in it.
 *
 * ## What it answers, exactly
 *
 * ```
 * held      = the sum of every position row for the order's instrument
 * closing   = SELL when held > 0, BUY when held < 0
 * resting   = the remaining quantity of every open working order on `closing`
 * committed = resting + order.quantity_
 *
 * true  ⟺  held ≠ 0  ∧  order.side_ == closing  ∧  committed ≤ |held|
 * ```
 *
 * `std::nullopt` means the arithmetic could not be done honestly -- a sum that will not fit, a
 * working row reporting more filled than it ordered. A caller must treat it as "no", because every
 * caller of this is deciding whether to *relax* a restriction.
 *
 * ## Why it is not `max_gross_exposure_`'s rule
 *
 * `RiskGuard::check` permits an order that does not raise `projected_gross_exposure`, and it is
 * tempting to reuse that here. It is **broader than this question and would admit an order that
 * opens a position**: hold nothing with a buy of 100 resting, and the projection is
 * `max(|0+100|, |0-0|) = 100`; add a candidate sell of 100 and it is `max(|0+100|, |0-100|) = 100`,
 * unchanged. So that rule reads the sell as costless while it opens a short of 100 if it alone
 * fills. The ceiling can afford this because it is a *ceiling*: the projection already counts the
 * resting buy, so the account cannot exceed the limit either way. A phase restriction cannot,
 * because it is asking a different question -- not "how big can this get" but "is this order
 * closing something". `held != 0` is what separates them.
 *
 * ## Why the sums are written out rather than taken from `Portfolio`
 *
 * Both of the obvious accessors are wrong here, in opposite directions, and both would fail
 * quietly:
 *
 * - `Portfolio::quantity_of` returns the **first** matching position row and nothing dedupes what an
 *   adapter appends -- a broker reporting one instrument as two lots is ordinary. It under-reports
 *   `held`, which refuses an exit the operator chose to allow, for a reason no report would show.
 * - `Portfolio::working_quantity` **skips** a row whose remaining quantity will not compute or comes
 *   out negative. That is right for a figure a strategy reads and wrong for a limit: it
 *   under-reports `resting`, which *admits* an order that can carry the position through flat and
 *   open the opposite one. This refuses such a snapshot instead, which is the rule
 *   `projected_gross_exposure` already states for the same reason.
 *
 * A flat position row contributes zero and is not an error, matching the projection's treatment of
 * one: a broker may list a closed position, and `ReplayLedger` keeps a lot after it returns to flat.
 * Rows of opposing signs for one instrument net, because the question is about the net holding.
 */
template <typename Broker>
std::optional<bool> closes_toward_flat(
  const Portfolio<Broker>& portfolio, const PlannedOrder& order
) {
  const auto zero = Decimal64::parse("0");

  // **Net across every row, which is what `net_quantity_of` exists for.** `quantity_of` returns the
  // first, and reading it here would understate a split holding and refuse an exit the operator
  // chose to allow -- silently, since no report carries a figure this rule never computed.
  const auto net = portfolio.net_quantity_of(order.instrument_);
  if (!net) {
    return std::nullopt;
  }
  const auto held = *net;
  // **No row at all and a row of zero are different claims, and this rule gives them the same
  // answer.** `Portfolio::quantity_of` keeps them apart -- nothing versus zero -- because a broker
  // omitting a closed position is saying something. Here neither is a holding to close, so both are
  // `false`, and distinguishing them would produce two paths to one answer.
  if (held.coefficient() == 0) {
    return false;
  }

  // **A quantity that is not positive cannot close anything, and must not be arithmetic here.**
  // `PlannedOrder::quantity_` is a public field, and a negative one is constructible through the
  // ordinary path: `SizeSpec::quantity` accepts any decimal and `reconcile(OrderIntent)` rejects a
  // size only once it *rounds to zero*. Without this, a sell of -1 against a long of 100 satisfies
  // `committed <= |held|` and would be granted the exception -- an inconsistent order relaxing a
  // restriction, which is the one direction this rule must never fail in. Zero is excluded with it:
  // an order that closes nothing is not a close.
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
    const auto remaining = decimal_subtract(
      working.order_.quantity_, working.order_.execution_.filled_quantity_
    );
    // A row filled beyond what it ordered refuses rather than counting zero. The snapshot is
    // inconsistent about an order on the very side this rule is measuring, and a rule that relaxes a
    // restriction must not infer "nothing outstanding" from a contradiction.
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
  // `<=`, so an order that reaches flat exactly is a close. That is the ordinary case and the one
  // `vanilla::TimedLiquidation` produces: reconciliation targets zero, so the order is the whole
  // holding.
  return !decimal_less(*magnitude, *committed);
}

/**
 * What a guard decided about one order, and on what terms.
 *
 * `check` returns the first member and is what almost every caller wants. This exists for the one
 * caller that has to *count* what it sent: `admitted_outside_phase_` is the only fact about a
 * permitted order that cannot be recovered afterwards from the order, the portfolio or the report.
 *
 * **`admitted_outside_phase_` is never true beside a rejection**, so it means what its name says:
 * this order got past every limit, and the phase limit is one it would not have got past without
 * the closing exception. The phase check is third in the ordering and the sellable check, a
 * buying-power limit and both ceilings come after it, so an order can use the exception and still
 * be refused -- in which case nothing was admitted and nothing is flagged.
 *
 * **It still does not mean the order was sent.** A permitted order can be rejected by the broker,
 * refused by the binding, or fail in transit. A caller counting what actually left the process must
 * therefore read this at the *send*, which is why `Runner` increments where it learns the outcome
 * rather than here. Counting at the admission would report a proxy for the harm instead of the harm.
 */
struct GuardDecision {
  /** The reason the order was refused, or nothing when it was permitted. */
  std::optional<GuardRejection> rejection_;
  /**
   * Whether `permitted_phases_` did not admit this order's phase and only its closing exception did.
   *
   * False whenever the phase was permitted outright, whenever the limit is absent, and on every
   * refusal.
   */
  bool admitted_outside_phase_{false};
};

/**
 * Checks planned orders against the operator's limits, and remembers what was sent.
 *
 * Stateful, unlike reconciliation, because a rate limit is a statement about history. The state is
 * only the timestamps of orders actually sent, which is why `note_sent` is separate from `check`:
 * an order that was checked and then refused by a later guard, or never reached the broker, must
 * not consume rate budget. Recording at the point of sending rather than of checking is what keeps
 * the count honest.
 *
 * Owned by one runner and not synchronized, the model every stateful type in this library follows.
 */
class RiskGuard {
public:
  /** Constructs a guard enforcing `limits`. */
  explicit RiskGuard(RiskLimits limits = {}) : limits_(std::move(limits)) {}

  /** Returns the limits in force. */
  const RiskLimits& limits() const noexcept { return limits_; }

  /**
   * Stops every order until `resume()`, on the operator's behalf. Takes effect before any other
   * check.
   *
   * **The no-argument call is the operator's hold**, `HaltReason::OPERATOR`, and that is not merely
   * source compatibility: a bare `halt()` in a program or at a prompt is a person deciding to stop,
   * and a person's intent is exactly the one that must not be cleared by machinery that happens to
   * be holding the same switch. Every other holder names itself.
   *
   * **This is *quiesce*, and only that**: it stops this runner from *sending*. Three things an
   * operator might mean by "stop" are worth keeping apart, because each asks the broker for
   * something different:
   *
   * - *quiesce* -- send nothing further. Positions and working orders are left exactly as they are,
   *   and this is the only one of the three whose effect is entirely local and therefore certain.
   *   This.
   * - *withdraw* -- quiesce, then *ask* the broker to cancel the working orders.
   * - *flatten* -- withdraw, then *ask* it to close the positions with market orders.
   *
   * The second and third are requests, not outcomes. A cancellation the broker accepts can still be
   * rejected afterwards and leave the order working, and a closing order that is accepted has not
   * necessarily filled. Only the broker's own order list and holdings say what is true;
   * `runner::LoopReport::closing_complete()` says only whether every request was accepted.
   *
   * Flattening is a strategy's decision, which is what `vanilla::TimedLiquidation` exists for, or an
   * operator's through the broker's own tools. It is deliberately not something a risk limit does.
   * `runner::StopIntent` names all three for a long-running session, and `runner::PollingLoop`
   * honours them; this switch is the primitive underneath the first of them.
   *
   * In-process and in-memory: nothing reaches this from outside the program, and it does not survive
   * the process. `runner::FileStopChannel` and `runner::SignalStop` are the out-of-band channels.
   */
  void halt() noexcept { held_[index_of(HaltReason::OPERATOR)] = true; }

  /**
   * Takes the hold named by `reason`, with an operator-readable `detail`.
   *
   * **Idempotent, and a set rather than a count.** Taking a hold that is already held replaces its
   * detail and nothing else, and one `resume(reason)` releases it however many times it was taken.
   * That is deliberate: the holders are distinct *conditions*, not distinct callers, so a loop that
   * recomputes `PORTFOLIO_STALE` on every iteration must not build a hold that as many refreshes are
   * needed to clear. A counter would get exactly that wrong, in the opposite direction from the
   * single bool this replaces.
   */
  void halt(HaltReason reason, std::string detail) {
    held_[index_of(reason)] = true;
    details_[index_of(reason)] = std::move(detail);
  }

  /** Takes the hold named by `reason`, with no detail. */
  void halt(HaltReason reason) { halt(reason, std::string()); }

  /** Releases the operator's hold. Other holds, if any, still stop trading. */
  void resume() noexcept { resume(HaltReason::OPERATOR); }

  /** Releases the hold named by `reason`. Releasing one that is not held does nothing. */
  void resume(HaltReason reason) noexcept {
    held_[index_of(reason)] = false;
    details_[index_of(reason)].clear();
  }

  /** Returns whether anything at all is stopping trading. */
  bool halted() const noexcept {
    for (const auto held : held_) {
      if (held) {
        return true;
      }
    }
    return false;
  }

  /** Returns whether `reason` in particular is holding. */
  bool halted(HaltReason reason) const noexcept { return held_[index_of(reason)]; }

  /**
   * Returns the most fundamental hold in force, or nothing when trading is permitted.
   *
   * "Most fundamental" is the declaration order of `HaltReason`, which is why that order is part of
   * the enum's contract. This is what a refusal reports, and it follows the rule `check` already
   * applies to limits: say the reason that most needs attention, not an incidental one.
   */
  std::optional<HaltReason> blocking_reason() const noexcept {
    for (std::size_t index = 0; index < halt_reason_count; ++index) {
      if (held_[index]) {
        return static_cast<HaltReason>(index);
      }
    }
    return std::nullopt;
  }

  /** Returns the detail recorded with `reason`'s hold, or an empty string when there is none. */
  const std::string& hold_detail(HaltReason reason) const noexcept {
    return details_[index_of(reason)];
  }

  /**
   * Returns every hold in force, most fundamental first.
   *
   * For diagnosis — an operator asking why trading is stopped when more than one thing is stopping
   * it, which is precisely the case the single bool could not answer. `blocking_reason` is what the
   * send path uses.
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
   * Records that an order was sent at `when`, for the rate limit.
   *
   * Called by the runner after the broker accepted the instruction, not when it was checked.
   */
  void note_sent(std::chrono::system_clock::time_point when) {
    sent_.push_back(when);
    ++total_sent_;
  }

  /**
   * Returns how many orders this guard has ever seen sent.
   *
   * Distinct from `recent_orders`, which is the rate limit's question and prunes: `forget_before` drops
   * timestamps that can no longer matter, so `sent_.size()` can fall and cannot be differenced across a
   * call. This only ever rises, which is what a caller asking "did anything go out while I was not
   * looking" needs -- `runner::PollingLoop` asks exactly that around every call it makes into a source
   * or a strategy handler, to tell a poll that revised the trading calendar harmlessly from one that
   * revised it and then sent an order against the answer it had just replaced.
   *
   * An observable on the component that performs the accounting rather than a seam added for a test,
   * which is the rule `ai/wall-clock-test-assertions` settled.
   */
  std::size_t total_sent() const noexcept { return total_sent_; }

  /** Returns how many orders were sent within `rate_window_` of `now`. */
  std::size_t recent_orders(std::chrono::system_clock::time_point now) const {
    std::size_t count = 0;
    for (const auto when : sent_) {
      if (now - when < limits_.rate_window_) {
        ++count;
      }
    }
    return count;
  }

  /** Drops recorded sends that can no longer affect the rate limit. */
  void forget_before(std::chrono::system_clock::time_point cutoff) {
    while (!sent_.empty() && sent_.front() < cutoff) {
      sent_.pop_front();
    }
  }

  /**
   * Returns the reason `order` is not permitted, or nothing when it is.
   *
   * The narrow answer, and the one almost every caller wants. `evaluate` is the same check with the
   * one extra fact a caller has to know if it is going to *count* what it sends; see
   * `GuardDecision`.
   */
  template <typename Broker>
  std::optional<GuardRejection> check(
    const PlannedOrder& order,
    const Portfolio<Broker>& portfolio,
    const GuardInputs& inputs
  ) const {
    return evaluate(order, portfolio, inputs).rejection_;
  }

  /**
   * Returns whether `order` is permitted, and on what terms.
   *
   * Checked in order of cost and of bluntness: the kill switch, then what the operator has
   * forbidden outright, then the arithmetic ones. That order means an operator reading a log sees
   * the most fundamental reason rather than an incidental one — a halted system reports the halt,
   * not a notional ceiling it also happened to exceed.
   */
  template <typename Broker>
  GuardDecision evaluate(
    const PlannedOrder& order,
    const Portfolio<Broker>& portfolio,
    const GuardInputs& inputs
  ) const {
    const auto refuse = [&order](GuardReason reason) {
      GuardRejection rejection;
      rejection.reason_ = reason;
      rejection.instrument_ = order.instrument_;
      rejection.tag_ = order.tag_;
      return GuardDecision{std::optional<GuardRejection>(rejection), false};
    };
    const auto refuse_with =
      [&order](GuardReason reason, Decimal64 limit, Decimal64 attempted) {
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
        // **The phase governs what an order opens, not what it closes.** A restriction that cannot
        // express an exit turns "the market is shut" into "you may not reduce risk", and this
        // library already answered that question once: `max_gross_exposure_` permits an order that
        // does not raise reachable exposure however far over the ceiling the account is, because a
        // limit that cannot be de-risked past is worse than one that does nothing. The same
        // argument applies here with more force, since the account did not choose to be holding
        // when the session ended.
        //
        // **A different predicate, though, and deliberately so.** `closes_toward_flat` carries why:
        // "does not raise the projection" admits a sell against a flat book with a buy resting,
        // which opens a short. See its documentation before reusing either rule for the other.
        //
        // What this costs, and it was chosen with the cost known: the order goes to a broker that
        // will very likely reject it outside its session, so a nightly flatten becomes a rejection
        // rather than a refusal. That is the broker's own answer rather than this library's guess,
        // it is journalled, and `max_orders_per_window_` is the lever for a strategy that restates
        // a close on every pass.
        const auto closing = closes_toward_flat(portfolio, order);
        // Unknown refuses, because this branch only ever *relaxes* a restriction the operator
        // stated. A snapshot whose arithmetic does not work cannot establish that an order closes
        // anything, and Decision 6 does not permit approximating it.
        if (!closing || !*closing) {
          return refuse(GuardReason::PHASE_NOT_PERMITTED);
        }
        admitted_outside_phase = true;
      }
    }

    if (limits_.max_orders_per_window_ &&
        recent_orders(inputs.now_) >= *limits_.max_orders_per_window_) {
      return refuse_with(
        GuardReason::ORDER_RATE_EXCEEDED,
        Decimal64(static_cast<std::int64_t>(*limits_.max_orders_per_window_), 0),
        Decimal64(static_cast<std::int64_t>(recent_orders(inputs.now_) + 1), 0)
      );
    }

    // A market order carries no price, so its value has to come from the runner's last quote.
    // Without one, a notional or exposure limit cannot be evaluated, and a limit that cannot be
    // evaluated must refuse rather than wave the order through.
    const auto price = order.price_ ? order.price_ : inputs.reference_price_;
    const bool needs_value =
      limits_.max_order_notional_.has_value() || limits_.max_gross_exposure_.has_value() ||
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

    if (limits_.max_order_notional_ && value &&
        decimal_less(*limits_.max_order_notional_, *value)) {
      return refuse_with(
        GuardReason::ORDER_NOTIONAL_TOO_LARGE, *limits_.max_order_notional_, *value
      );
    }

    if (limits_.enforce_sellable_quantity_ && order.side_ == OrderSide::SELL) {
      const auto sellable = portfolio.sellable_quantity_of(order.instrument_);
      if (!sellable) {
        // Absent is unknown, and unknown refuses. The broker did not publish the figure, or there
        // is no such position; neither is permission to sell, and substituting the held quantity
        // here would answer a question nobody asked. Decision 6: a condition the caller stated is
        // honored or refused, never quietly approximated.
        return refuse(GuardReason::QUANTITY_NOT_SELLABLE);
      }
      // Resting sells count against the same figure. Whether a broker's number is already net of
      // them is not stated by either specification, so this subtracts them: over-counting can only
      // refuse an order that would have been accepted, while under-counting sells a position
      // twice. `ai/halt-provenance` spent two review rounds on exactly that asymmetry, and
      // reconciliation's same-side offset exists for the same reason.
      const auto resting = portfolio.working_quantity(order.instrument_, OrderSide::SELL);
      const auto committed = decimal_add(resting, order.quantity_);
      if (!committed) {
        return refuse(GuardReason::QUANTITY_NOT_SELLABLE);
      }
      if (decimal_less(*sellable, *committed)) {
        return refuse_with(GuardReason::QUANTITY_NOT_SELLABLE, *sellable, *committed);
      }
    }

    if (limits_.enforce_buying_power_ && order.side_ == OrderSide::BUY) {
      if (!value) {
        // A buy whose cost cannot be computed cannot be checked against a budget. `needs_value`
        // already forces a price when this limit is on, so reaching here means the multiplication
        // itself failed.
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
      if (!inputs.order_currency_) {
        return refuse(GuardReason::INSUFFICIENT_BUYING_POWER);
      }
      const auto available = portfolio.buying_power_in(*inputs.order_currency_);
      if (!available) {
        // Absent is unknown, and unknown refuses -- the same rule as the sellable check. Falling
        // back to `cash_in` would be worse than useless here: live it is empty, so the fallback
        // would refuse anyway while looking like an answer.
        return refuse(GuardReason::INSUFFICIENT_BUYING_POWER);
      }
      if (decimal_less(*available, *value)) {
        return refuse_with(GuardReason::INSUFFICIENT_BUYING_POWER, *available, *value);
      }
    }

    if (limits_.max_position_quantity_) {
      const auto zero = Decimal64::parse("0");
      // **Net across every position row, the same reading reconciliation sizes against.** This read
      // `quantity_of` -- the first row -- and the two disagreeing is worse than either being wrong
      // alone: reconciliation plans a sell of the whole net holding, and a ceiling measuring from
      // one lot of it computes a resulting position that never existed. Rows of 60 and 40 with a
      // ceiling of 30 refused the sell of 100 that reaches flat, because it read 60 - 100 = -40 and
      // called that a short of 40. The exit was refused for breaching a ceiling it was on its way
      // *under*, and the account kept the position.
      //
      // So this moves in both directions, deliberately. It refuses a buy that two lots together put
      // over the ceiling, which is the limit doing its job and what it silently permitted before.
      // And it permits an exit that reaches flat from a split holding, which no operator setting a
      // ceiling meant to forbid.
      const auto net = portfolio.net_quantity_of(order.instrument_);
      if (!net) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto held = *net;
      // **Orders already working on this side count, because the ceiling is on the position this
      // order could produce, not on the one the last poll reported.** Without this, two buys of 60
      // against a ceiling of 100 both pass -- each is checked against an unchanged holding -- and
      // the pair builds 120. Same-side only: a resting sell is not counted as relief for a buy,
      // because treating it as relief assumes a fill that may never come, and the whole point of a
      // ceiling is what happens when everything outstanding does fill. This is the same asymmetry
      // the sellable check above resolves the same way.
      const auto pending = portfolio.working_quantity(order.instrument_, order.side_);
      const auto committed = decimal_add(pending, order.quantity_);
      if (!committed) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto signed_quantity = order.side_ == OrderSide::BUY ?
        std::optional<Decimal64>(*committed) :
        decimal_subtract(zero, *committed);
      if (!signed_quantity) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto resulting = decimal_add(held, *signed_quantity);
      if (!resulting) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      // The ceiling is on magnitude, so a short of the same size is limited as a long is.
      const auto magnitude = decimal_less(*resulting, zero) ?
        decimal_subtract(zero, *resulting) :
        std::optional<Decimal64>(*resulting);
      if (!magnitude) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      if (decimal_less(*limits_.max_position_quantity_, *magnitude)) {
        return refuse_with(
          GuardReason::POSITION_LIMIT_EXCEEDED, *limits_.max_position_quantity_, *magnitude
        );
      }
    }

    if (limits_.max_gross_exposure_) {
      // Per currency, so which one the order is priced in decides what this compares. An
      // unresolved instrument leaves it absent, and a ceiling that cannot tell which budget it is
      // spending must refuse rather than pick the first.
      if (!inputs.order_currency_) {
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
      const auto resulting = projected_gross_exposure(
        portfolio, inputs.exposure_basis_, *inputs.order_currency_, &order, price,
        CandidateContribution::QUANTITY_AND_PRICE
      );
      if (!resulting) {
        // The basis does not describe the portfolio, or the arithmetic did not fit. Refusing is the
        // point: the previous code defaulted the current exposure to zero and compared the ceiling
        // against this order alone, which is how a portfolio limit came to permit everything it
        // exists to refuse.
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
      if (decimal_less(*limits_.max_gross_exposure_, *resulting)) {
        // **Over the ceiling is not on its own a reason to refuse: taking it further is.**
        //
        // The projection is the largest position still reachable, so a resting sell does not lower
        // it -- hold 100 with a sell of 100 resting and the exposure is 100 until that sell fills.
        // Correct, and on its own it would trap an account that is *already* over the ceiling: a
        // ceiling lowered by its operator, or exposure carried up by a mark, would refuse every
        // order including the ones selling out of it. A limit that cannot be de-risked past is the
        // failure this branch's own issue document set out to avoid, one step further along than
        // the version it argued against.
        //
        // So the ceiling governs what an order *adds*. An order that leaves the projection where it
        // was -- any sell against a long, any buy covering a short -- is permitted however far over
        // the account already is, and an order that moves it up while over is refused.
        // **`PRICE_ONLY`, not `nullptr`.** Dropping the candidate entirely would value this case at
        // the mark while the other used the candidate's limit, so the two would differ by a price
        // rather than by the order: a sell of ten limited above the mark would read as *adding*
        // exposure to a position it can only reduce, and the exit this branch exists to protect
        // would be refused. Same envelope, different quantity, so the comparison isolates the order.
        const auto without = projected_gross_exposure(
          portfolio, inputs.exposure_basis_, *inputs.order_currency_, &order, price,
          CandidateContribution::PRICE_ONLY
        );
        if (!without) {
          return refuse(GuardReason::VALUE_NOT_KNOWN);
        }
        if (decimal_less(*without, *resulting)) {
          return refuse_with(
            GuardReason::GROSS_EXPOSURE_EXCEEDED, *limits_.max_gross_exposure_, *resulting
          );
        }
      }
    }

    return GuardDecision{std::nullopt, admitted_outside_phase};
  }

private:
  /** Returns `reason`'s slot in the fixed-capacity hold arrays. */
  static constexpr std::size_t index_of(HaltReason reason) noexcept {
    return static_cast<std::size_t>(reason);
  }

  RiskLimits limits_;
  /**
   * Which holds are in force, indexed by `HaltReason`.
   *
   * A fixed array rather than a set or a vector because the reasons are a closed enum and there are
   * six of them: no allocation, no ordering to maintain, and declaration order is the priority order
   * `blocking_reason` needs for free.
   */
  std::array<bool, halt_reason_count> held_{};
  /** Each hold's operator-readable detail, empty where a hold carries none or is not held. */
  std::array<std::string, halt_reason_count> details_;
  /**
   * Timestamps of orders actually sent, oldest first.
   *
   * A deque rather than a counter because the window slides: a counter reset on a boundary would
   * let twice the limit through across one, which is the classic way a rate limit is wrong.
   */
  std::deque<std::chrono::system_clock::time_point> sent_;
  std::size_t total_sent_{0};
};

} // namespace strategy
} // namespace algotradelib
