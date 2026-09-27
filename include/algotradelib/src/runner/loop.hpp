#pragma once

#include "./clock.hpp"
#include "./stop.hpp"

#include <algotradelib/core>
#include <algotradelib/strategy>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/** What a loop polls for, and the key its cadence is configured under. */
enum class PollKind {
  /** Completed bars, which is what most strategies trade on. */
  BARS,
  /** Current prices, for strategies that act between bars. */
  QUOTES,
  /** Positions and cash, which sizing is reconciled against. */
  HOLDINGS,
  /** Resting orders, which reconciliation and withdrawal both need. */
  WORKING_ORDERS,
  /**
   * What the broker says may be spent, which sizing a buy is checked against.
   *
   * On the holdings cadence rather than per order, deliberately. The figure changes on every fill,
   * so reading it immediately before each order would cost a round trip on the latency-sensitive
   * path and still race a resting order filling a moment later. Polled beside holdings it is
   * exactly as fresh as the positions it is reconciled against, which is the staleness the loop
   * already reasons about.
   */
  BUYING_POWER,
  /**
   * Which dates the market trades, which is half of what the market phase is derived from.
   *
   * **The only poll whose answer the loop itself reads.** Every other kind feeds the runner and is
   * done; this one lands in `Context::set_calendar` and the loop then asks it whether today is a
   * trading day, because `strategy::RiskLimits::permitted_phases_` decides whether anything may be
   * sent at all. It is therefore **first in `all_poll_kinds`**, and the one cadence that is **on by
   * default** -- see `PollSchedule::calendar_` for that argument, which is not the argument for the
   * others.
   *
   * Declared **last** even so, because these values are array indices and an enumerator inserted at
   * the front renumbers every existing one. Where a kind is polled and where it is declared are two
   * different questions with two different answers.
   *
   * ## The one obligation this puts on a source
   *
   * **A source writes the calendar during this poll, nowhere else, and does not feed the runner from
   * it.** Two halves, and the second is not redundant: Codex round 3 found that a source obeying only
   * the first -- writing the calendar in this poll and then calling `Runner::feed` before returning --
   * sends an order against the phase as it stood when the poll began, which is the whole hazard the
   * phase exists to prevent. The loop derives the phase before every call it makes into a source, so a
   * calendar arriving in one call is in force for the next; but nothing of its runs between a source's
   * own `set_calendar` and its own `Runner::feed`.
   *
   * Stated as "this poll installs a calendar and feeds nothing", which is the version with no edge:
   * `live::SessionSource::poll_calendar` is exactly that. A source that breaks either half is
   * reported rather than silently tolerated -- see `PollingLoop::note_revised_phase`, which counts a
   * call that both sent an order and left the phase changed, whichever poll it was.
   */
  CALENDAR,
};

/**
 * How many `PollKind` enumerators there are, for the fixed-size counters keyed by one.
 *
 * Several arrays are indexed by `static_cast<int>(kind)`. Sized by a literal, adding an enumerator
 * writes one past the end of every one of them -- silently, since the index is in range for the
 * enum and out of range for the array. Naming the count is what makes the next enumerator a
 * one-line change rather than a hunt.
 */
inline constexpr std::size_t poll_kind_count = 6;

/** Returns a stable lowercase name for `kind`. */
inline constexpr std::string_view poll_kind_name(PollKind kind) noexcept {
  switch (kind) {
    case PollKind::CALENDAR:
      return "calendar";
    case PollKind::BARS:
      return "bars";
    case PollKind::QUOTES:
      return "quotes";
    case PollKind::HOLDINGS:
      return "holdings";
    case PollKind::WORKING_ORDERS:
      return "working_orders";
    case PollKind::BUYING_POWER:
      return "buying_power";
  }
  return "unknown";
}

/** Every kind, for iterating without repeating the list. */
inline constexpr PollKind all_poll_kinds[] = {
  // First, because it is the cheapest and because the phase derived from it decides whether anything
  // else in this iteration may be sent -- and being first, its answer is in force for every other poll
  // in the same iteration, since the phase is re-derived before each of them. It used to be in force
  // only from the next iteration, which was a defect rather than a design: see `PollingLoop::step`.
  PollKind::CALENDAR,
  PollKind::BARS,
  PollKind::QUOTES,
  PollKind::HOLDINGS,
  // After holdings, so an iteration that refreshes both leaves the money and the positions equally
  // fresh, and the buy sized against them is sized against one moment rather than two.
  PollKind::BUYING_POWER,
  PollKind::WORKING_ORDERS,
};

static_assert(
  sizeof(all_poll_kinds) / sizeof(all_poll_kinds[0]) == poll_kind_count,
  "every PollKind must appear in all_poll_kinds: a kind left out is never polled, and the loop "
  "would silently answer with whatever the previous poll left behind"
);

/**
 * What became of one poll.
 *
 * Four values rather than a bool, because the loop owes a different response to each and two of
 * them are easy to conflate:
 *
 * - **`UNCHANGED` is not a failure.** Nothing new since the last poll is the normal case between
 *   bars, and counting it as a failure would escalate a quiet market into a shutdown.
 * - **`RATE_LIMITED` is not a failure either.** It is the limiter working as designed. The
 *   controller already handles the wait, and counting a refusal as an outage would make a loop
 *   polling slightly too fast quiesce itself.
 */
enum class PollOutcome {
  /** New data arrived and was fed to the runner. */
  UPDATED,
  /** The poll succeeded and there was nothing new. */
  UNCHANGED,
  /** The poll failed. Counted toward the failure budget. */
  FAILED,
  /** The poll was refused by the rate limiter. Not counted as a failure. */
  RATE_LIMITED,
};

/** Returns a stable lowercase name for `outcome`. */
inline constexpr std::string_view poll_outcome_name(PollOutcome outcome) noexcept {
  switch (outcome) {
    case PollOutcome::UPDATED:
      return "updated";
    case PollOutcome::UNCHANGED:
      return "unchanged";
    case PollOutcome::FAILED:
      return "failed";
    case PollOutcome::RATE_LIMITED:
      return "rate_limited";
  }
  return "unknown";
}

/**
 * How often each kind is polled.
 *
 * An unset interval disables that poll entirely, which is the honest default: a loop should not
 * invent a cadence for data the caller never said they wanted, and a broker without working-order
 * inquiry has no cadence to set.
 *
 * **`tick_` is the loop's own granularity**, and the floor on how quickly a stop is noticed through
 * the control file. It is not a poll: an iteration with nothing due still checks for a stop, which is
 * why a loop polling bars once a day still responds to an operator within a second.
 */
struct PollSchedule {
  /** How often to fetch bars; absent disables the poll. */
  std::optional<std::chrono::nanoseconds> bars_;
  /** How often to fetch quotes; absent disables the poll. */
  std::optional<std::chrono::nanoseconds> quotes_;
  /** How often to re-read holdings; absent disables the poll and leaves the portfolio stale. */
  std::optional<std::chrono::nanoseconds> holdings_;
  /** How often to re-read the broker's working orders; absent disables the poll. */
  std::optional<std::chrono::nanoseconds> working_orders_;
  /**
   * How often to ask what may be spent; absent disables the poll.
   *
   * Absent by default, like every other cadence here, and a caller enforcing
   * `RiskLimits::enforce_buying_power_` without setting it gets a runner that holds rather than one
   * that guesses -- which is the safe direction and is visible in the report.
   */
  std::optional<std::chrono::nanoseconds> buying_power_;
  /** The loop's own granularity, and the floor on how quickly a stop is noticed. Not a poll. */
  std::chrono::nanoseconds tick_{std::chrono::seconds(1)};
  /**
   * How often to ask which dates the market trades. **The one cadence that is on by default.**
   *
   * Every other member here is absent by default, because the library will not spend a request on
   * data the caller never asked for. This one is different in kind: the loop derives the market phase
   * from it, `RiskLimits::permitted_phases_` defaults to `{OPEN}`, and an uncovered date is `UNKNOWN`
   * -- so a loop with no calendar refuses every order that opens or increases a position. It still
   * permits one that closes a held position, which is the only thing a run in an unknown phase
   * should be doing. A cadence absent by default would make the whole mechanism one nobody switches
   * on, which is exactly the shape the phase decision closed.
   *
   * **Six hours, and the number has three constraints behind it.** KIS asks that its holiday endpoint
   * be called at most once a day, and its client caches -- a covered date is answered without a
   * request -- so four polls a day cost one request a day, on the first new date. Toss answers at
   * reference-data priority, the lowest there is, and states no daily limit. And a calendar stops
   * covering the local date at local midnight, so any cadence longer than the nine hours between
   * midnight and a 09:00 open can leave the phase `UNKNOWN` through an open.
   *
   * Absent disables it, which is then a visible act rather than a default nobody noticed. A broker
   * that publishes no calendar compiles no request whatever this says.
   *
   * **Declared last rather than beside the other cadences, and that is not tidiness.** This is an
   * aggregate, so `PollSchedule{std::chrono::seconds(5)}` initializes whichever member comes first.
   * Adding this one at the front silently turned that expression from "poll bars every five seconds"
   * into "poll the calendar every five seconds, and never poll bars" -- a caller that compiles cleanly
   * and stops receiving data. Appending preserves every existing positional mapping. The **poll**
   * order is `all_poll_kinds`, where the calendar is deliberately first; declaration order here is
   * only layout.
   */
  std::optional<std::chrono::nanoseconds> calendar_{std::chrono::hours(6)};
  /** Returns the interval configured for `kind`, or nothing when it is disabled. */
  std::optional<std::chrono::nanoseconds> interval_for(PollKind kind) const noexcept {
    switch (kind) {
      case PollKind::CALENDAR:
        return calendar_;
      case PollKind::BARS:
        return bars_;
      case PollKind::QUOTES:
        return quotes_;
      case PollKind::HOLDINGS:
        return holdings_;
      case PollKind::WORKING_ORDERS:
        return working_orders_;
      case PollKind::BUYING_POWER:
        return buying_power_;
    }
    return std::nullopt;
  }
};

/**
 * Whether `Source` leaves the broker's outstanding orders in the portfolio after an order-list read.
 *
 * **Defaults to false**, which is the safe direction and a deliberate one: a source that does not
 * say it publishes working orders cannot be assumed to, and the cost of being wrong is a `WITHDRAW`
 * that reports a complete stop while an order is still resting. A source declares
 * `static constexpr bool provides_working_orders = true;` to say otherwise.
 */
template <typename Source, typename = void>
struct source_provides_working_orders : std::false_type {};

template <typename Source>
struct source_provides_working_orders<
  Source, std::void_t<decltype(Source::provides_working_orders)>
> : std::bool_constant<Source::provides_working_orders> {};

/**
 * Cost a broker stated against its own orders, as observed on the snapshots a loop happened to poll.
 *
 * **This is a subtotal of unknown coverage, and every name here is chosen to keep saying so.** A
 * running loop polls *working* orders; an order that fills and leaves that listing between two polls
 * is never seen carrying its final commission, and an order placed and filled between two polls is
 * never seen at all. Neither is detectable from the listing, so nothing here can be called a total
 * and no counter here certifies completeness. A figure with known coverage needs a deliberate
 * order-history poll, which this loop does not make.
 *
 * What it is good for is the thing that did not exist before it: Toss states the actual commission
 * and tax it charged on every order, the library read it into `OrderExecution` and then discarded
 * it, and a replay's modelled cost had nothing on the live side to be compared against at all.
 */
struct ChargedCosts {
  /**
   * Broker-stated commission observed, per currency.
   *
   * Per currency because `Money` refuses to add unlike currencies and a run can poll a universe
   * spanning markets: one scalar would either be wrong or would have to refuse to exist. A currency
   * nobody stated a cost in has **no entry**, which is not the same as an entry of zero.
   */
  std::vector<Money> commission_;
  /** Broker-stated tax observed, per currency, on the same terms. */
  std::vector<Money> tax_;
  /** How many distinct orders were seen stating a cost, including a stated zero. */
  std::size_t orders_stating_cost_{0};
  /**
   * How many distinct orders were seen stating no cost at all.
   *
   * Every KIS order is one of these: its order snapshots carry no commission field, so absence here
   * is the broker's silence rather than a charge of nothing.
   */
  std::size_t orders_stating_none_{0};
  /**
   * How many orders left the working-order listing between two polls.
   *
   * **This is not a completeness measure and must not be read as one.** It counts what it says: a
   * row that was there and then was not. That happens on a fill, and equally on a cancellation or an
   * expiry, and it cannot see an order that was placed and filled without ever appearing. It is a
   * diagnostic for how much of the picture moved while the loop was not looking, nothing more.
   */
  std::size_t working_list_disappearances_{0};
  /**
   * How many times an order restated a *different* cost at the same filled quantity.
   *
   * Nothing in an order snapshot carries a broker sequence number or a fee revision time --
   * `ordered_at_` and `canceled_at_` are lifecycle instants and `filled_at_` timestamps the latest
   * fill -- so when two readings disagree at the same filled quantity there is no authority that
   * says which is later. The first is kept and the disagreement is counted here rather than being
   * resolved by arrival order, which would let a delayed stale payload silently reduce a charge.
   */
  std::size_t conflicting_restatements_{0};

  /** Returns the commission observed in `currency`, or nothing when none was stated in it. */
  std::optional<Decimal64> commission_in(const Currency& currency) const {
    for (const auto& amount : commission_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  }

  /** Returns the tax observed in `currency`, or nothing when none was stated in it. */
  std::optional<Decimal64> tax_in(const Currency& currency) const {
    for (const auto& amount : tax_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  }

  /** Adds `amount` to the running figure for its own currency, adding the currency if it is new. */
  void add(std::vector<Money>& totals, const Money& amount) {
    for (auto& held : totals) {
      if (held.currency_ == amount.currency_) {
        if (const auto sum = decimal_add(held.amount_, amount.amount_)) {
          held.amount_ = *sum;
        }
        return;
      }
    }
    totals.push_back(amount);
  }

  /**
   * Returns what was observed between `before` and this, per currency.
   *
   * A `SessionSource` outlives the runs made against it, and `Loop::run` builds a fresh report each
   * call -- so an absolute accumulator copied at the end would put the first run's charges in the
   * second run's report. Differenced around the run, exactly as `sends_outside_permitted_phase` is.
   * The totals only ever rise, which is what makes the subtraction meaningful.
   */
  ChargedCosts since(const ChargedCosts& before) const {
    ChargedCosts delta;
    const auto difference = [](const std::vector<Money>& now, const std::vector<Money>& then) {
      std::vector<Money> out;
      for (const auto& amount : now) {
        const auto earlier = [&]() -> Decimal64 {
          for (const auto& was : then) {
            if (was.currency_ == amount.currency_) {
              return was.amount_;
            }
          }
          return Decimal64::parse("0");
        }();
        if (const auto moved = decimal_subtract(amount.amount_, earlier)) {
          if (moved->coefficient() != 0) {
            out.push_back(Money{*moved, amount.currency_});
          }
        }
      }
      return out;
    };
    delta.commission_ = difference(commission_, before.commission_);
    delta.tax_ = difference(tax_, before.tax_);
    delta.orders_stating_cost_ = orders_stating_cost_ - before.orders_stating_cost_;
    delta.orders_stating_none_ = orders_stating_none_ - before.orders_stating_none_;
    delta.working_list_disappearances_ =
      working_list_disappearances_ - before.working_list_disappearances_;
    delta.conflicting_restatements_ = conflicting_restatements_ - before.conflicting_restatements_;
    return delta;
  }
};

/**
 * Whether `Source` states the broker-reported cost it observed.
 *
 * **Defaults to false**, so a source that stays silent reports nothing rather than failing to
 * compile. A source declares `const ChargedCosts& charged_costs() const` to have its observations
 * reach `LoopReport`.
 */
template <typename Source, typename = void>
struct source_states_charged_costs : std::false_type {};

template <typename Source>
struct source_states_charged_costs<
  Source, std::void_t<decltype(std::declval<const Source&>().charged_costs())>
> : std::true_type {};

/**
 * Whether `Source` can say which currencies it was configured to ask about.
 *
 * **Defaults to false**, so a source that stays silent gets the weaker gate it had before rather
 * than a compile error. A source declares `const std::vector<Currency>& buying_power_currencies()
 * const` to let the gate hold for a configured budget that never arrived, instead of settling for
 * "some currency has a reading".
 */
template <typename Source, typename = void>
struct source_names_buying_power_currencies : std::false_type {};

template <typename Source>
struct source_names_buying_power_currencies<
  Source, std::void_t<decltype(std::declval<const Source&>().buying_power_currencies())>
> : std::true_type {};

/** How the loop behaves when polls keep failing. */
struct FailurePolicy {
  /**
   * Consecutive failures of one kind before the loop quiesces.
   *
   * Bounded rather than infinite because a loop spinning against a broker that is refusing it is
   * both useless and rude. Counted per kind, so a broken working-order inquiry does not mask a
   * healthy bar feed.
   */
  std::size_t consecutive_limit_{5};
};

/** What one iteration did. */
struct IterationReport {
  /** When the iteration ran. */
  std::chrono::system_clock::time_point at_{};
  /** What each poll returned, in the order they were attempted. */
  std::vector<std::pair<PollKind, PollOutcome>> polls_;
  /** Which hold blocked sending, or nothing when nothing did. */
  std::optional<strategy::HaltReason> gate_;
  /** Whether anything was fed to the runner. */
  bool fed_{false};
  /**
   * The phase the loop computed for this iteration.
   *
   * Reported because it decides most of what could be sent: `UNKNOWN` here and a default
   * `RiskLimits::permitted_phases_` means every placement that would open or increase a position was
   * refused, and a reader should not have to infer that from an empty placement count. An order
   * closing a held position is permitted in any phase, so this is not on its own a claim that
   * nothing went out -- `LoopReport::sends_outside_permitted_phase_` is what says whether any did.
   */
  strategy::MarketPhase phase_{strategy::MarketPhase::UNKNOWN};
  /** Whether this iteration announced a phase change to the strategy. */
  bool phase_changed_{false};

  /** Returns whether any poll brought new data. */
  bool updated() const noexcept {
    for (const auto& entry : polls_) {
      if (entry.second == PollOutcome::UPDATED) {
        return true;
      }
    }
    return false;
  }
};

/** What a whole run did, and why it ended. */
struct LoopReport {
  /** How many iterations ran. */
  std::size_t iterations_{0};
  /**
   * What the broker said it charged, on the orders this run happened to see.
   *
   * **Read `ChargedCosts`' own documentation before using it**: it is a subtotal whose coverage is
   * unknown on every run, not a total. It exists because a replay reports a modelled commission and
   * a live run reported nothing at all, so the two could not be compared in any direction.
   */
  ChargedCosts charged_costs_;
  /** How many polls of each kind were attempted, indexed by `PollKind`. */
  std::size_t attempted_[poll_kind_count] = {};
  /** How many polls of each kind failed. */
  std::size_t failed_[poll_kind_count] = {};
  /** How many iterations were blocked from sending, by reason. */
  std::size_t blocked_{0};
  /** The intent that ended the run, or nothing when it ended for another reason. */
  std::optional<StopIntent> stopped_by_;
  /** Which hold stopped the loop sending when it did, for an operator reading the tail of a log. */
  std::optional<strategy::HaltReason> final_gate_;
  /** The phase the last iteration computed. */
  strategy::MarketPhase final_phase_{strategy::MarketPhase::UNKNOWN};
  /**
   * How many calls into user code both sent an order and left the market phase changed behind them.
   *
   * **What it measures, exactly**, because an earlier name and note overstated it: around every call the
   * loop makes **that could send** -- a source's `poll` or `read_order_list`, a strategy handler, a
   * closing pass -- the loop records the phase and the guard's total sends and compares both afterwards.
   * This counts the calls where the phase moved **and** something went out. A call that cannot send is
   * not watched: `Source::buying_power_currencies` is a const accessor handed no runner, and
   * `Runner::observe_sellable_convention` reads the portfolio and dispatches nothing.
   *
   * It deliberately does **not** count a calendar revision that sent nothing, because nothing was
   * harmed, nor one that leaves today's answer unchanged. It cannot see a revision that is undone before
   * the call returns. And it is **co-occurrence, not causation**: a call that sent before revising, or one
   * that only cancelled -- cancellations never consult the phase -- is counted too, because both break
   * the writer contract even though neither placed an order against a stale answer. Always zero against `live::SessionSource` and any strategy that does not write the
   * calendar; non-zero names a contract in `PollKind::CALENDAR` or `runner::Context::set_calendar` that
   * somebody's code is breaking.
   */
  std::size_t sends_under_a_revised_phase_{0};
  /**
   * How many phase changes were announced to the strategy.
   *
   * The counter that distinguishes a mechanism that ran from one that was merely configured. A run
   * spanning an open and a close sees transitions; a run of ten seconds sees the first one only.
   */
  std::size_t phase_transitions_{0};
  /**
   * Orders sent during this run under a phase `RiskLimits::permitted_phases_` did not permit.
   *
   * `permitted_phases_` governs what an order *opens*, not what it closes: an order reducing a held
   * position toward flat is permitted whatever the phase, which is what lets an operator's flatten
   * go out after the close instead of being refused by their own limits. This counts how often that
   * happened, because it is the library sending into a session the operator said not to trade in and
   * an empty report would not say so.
   *
   * **A send, not a decision.** An order the exception admitted and a later limit refused is not
   * here; a broker's rejection is, because the request went out. Outside a session a rejection is
   * the likeliest answer, so a counter that excluded it would read zero in the case it exists for.
   *
   * Non-zero is not a fault. A nightly flatten produces one; a strategy restating a close on every
   * pass produces one per pass, and `RiskLimits::max_orders_per_window_` is the lever for that.
   */
  std::size_t sends_outside_permitted_phase_{0};
  /** Whether the loop gave up because a poll kept failing. */
  bool abandoned_{false};
  /** Which kind exhausted its failure budget, when one did. */
  std::optional<PollKind> abandoned_kind_;
  /** Whether the order list was successfully read at least once. */
  bool order_list_read_{false};
  /** Cancellations queued while honouring a `WITHDRAW` or `FLATTEN`. */
  std::size_t withdrawn_{0};
  /** Positions closed while honouring a `FLATTEN`. */
  std::size_t flattened_{0};
  /**
   * What the closing pass actually managed to send.
   *
   * Separate from `withdrawn_` and `flattened_`, which count what was *asked for*. The difference is
   * the whole point: a flatten that queues a closing order the guard then refuses has
   * `flattened_ == 1` and `closing_placed_ == 0`, and an operator who only saw the first number
   * would believe they were out of the market while still holding the position.
   */
  std::size_t closing_placed_{0};
  /**
   * Closing instructions that were rejected: reconciliation could not build them, the broker
   * refused them, or the binding refused to send them.
   *
   * All three are answers rather than doubts -- nothing needs reconciling any of them -- and all
   * three leave the position exactly where it was, which is why they share a count. The report's
   * `rejected_` and `rejected_cancel_` entries say which each one was.
   */
  std::size_t closing_rejected_{0};
  /** Closing instructions a guard refused. */
  std::size_t closing_refused_{0};
  /** Closing instructions withheld because the instrument's state is unresolved. */
  std::size_t closing_withheld_{0};
  /**
   * Closing instructions whose send failed in transit, leaving their outcome unknown.
   *
   * Cancellations included. A cancellation that failed in transit may have withdrawn the order or
   * may not, which is not the same as one that went.
   */
  std::size_t closing_indeterminate_{0};
  /**
   * Closing instructions local machinery refused to send, so no broker was asked.
   *
   * The journal could not be written. Different from every count above in what an operator does
   * about it: fix the storage, `resume_after_local_failure`, and stop again.
   */
  std::size_t closing_unavailable_{0};
  /**
   * Closing cancellations naming an order the portfolio snapshot does not hold.
   *
   * The snapshot is stale rather than the instruction wrong, and the answer is to read the broker's
   * own list rather than to retry blind.
   */
  std::size_t closing_unresolved_{0};

  /**
   * Positions whose closing order was sized down, or not sent at all, because another order could
   * close them too.
   *
   * A flatten cancels every resting order and then closes the position -- but a cancellation is a
   * request, not an outcome, so a same-side order can fill while its withdrawal is still pending.
   * Closing the *whole* holding alongside it could sell the position twice and leave the account
   * short. Two causes, one consequence:
   *
   * - **A same-side order in the listing.** The close is sized to the holding minus what is working
   *   on that side, which is the most that cannot overshoot.
   * - **A listing that cannot be trusted** -- `closing_source_unverified_`. Then no closing order is
   *   sent for that position at all, because the deduction above can only subtract what it can see.
   *
   * Which way it resolves is not knowable in this pass: if the cancellations complete, part or all
   * of the position is still held; if one fails because the order filled, that much is closed.
   * Either is acceptable and neither is complete, which is why this makes `closing_complete()`
   * false. The cancellations are sent in both cases, because withdrawing only reduces exposure.
   */
  std::size_t closing_contended_{0};

  /**
   * Positions whose closing order could not be *sized*, because the snapshot's own arithmetic
   * failed.
   *
   * Separate from `closing_contended_`, and the separation is the point. Contention is a statement
   * about competing **orders** -- something is working on the closing side, or the listing cannot be
   * trusted -- and its note sends an operator to the order list. This is a statement about the
   * **holding**: rows that will not add up, or a net whose closing size is not representable. An
   * operator told "contended" would go and read the order list, which is not where the problem is.
   *
   * Reachable at the boundary rather than in ordinary trading: two rows that sum to the most
   * negative `Decimal64` have no representable positive magnitude, so the size of the buy that would
   * close them cannot be stated. Rare, and the reason it is counted rather than skipped is that
   * `closing_complete()` promises the pass knew what to ask for. A silent `continue` here let a stop
   * report completion over a position it had never sized.
   */
  std::size_t closing_unsizable_{0};

  /**
   * Whether the closing pass could not establish what it needed to send.
   *
   * A stop builds its instructions from the holdings snapshot -- working orders for a `WITHDRAW`,
   * positions for a `FLATTEN` -- so a pass that runs before that snapshot exists, or before the
   * broker's own order list has been read once, is working from a picture that may be missing an
   * order a *predecessor* left resting. A stop requested before the loop's first iteration does
   * exactly that: `gate_for` reports `STOPPING` and never reaches the startup read.
   *
   * The pass still sends what it does know about, because withdrawing the orders it can see beats
   * withdrawing none. But it cannot claim to have closed everything, which is why this makes
   * `closing_complete()` false on its own.
   */
  bool closing_source_unverified_{false};

  /**
   * Returns whether every closing instruction the loop asked for actually went out.
   *
   * **Every way a closing instruction can fail to go counts here**, which is the whole value of the
   * call: an operator must not get `true` from a stop where the journal was broken, where the
   * broker rejected a cancellation, or where a cancellation named an order a stale snapshot did not
   * list. Those failed silently before, because they were reported on the runner's report and never
   * carried onto this one.
   *
   * **It is not a confirmation of anything at the exchange, and must not be read as one.** It says
   * every instruction the loop asked for was *accepted by the broker*, and acceptance is where this
   * library's knowledge stops:
   *
   * - A closing placement that was accepted may not have **filled**. Only a later holdings snapshot
   *   shows that, and a `WITHDRAW` deliberately leaves positions held anyway.
   * - A cancellation that was accepted may not have **completed**. Cancellation is asynchronous at
   *   both brokers -- Toss publishes `PENDING_CANCEL` and `CANCEL_REJECTED`, which returns the order
   *   to its previous state -- so an accepted withdrawal can still be working.
   *
   * `closing_contended_` covers a third thing: a flatten that deliberately sent *less* than the
   * position -- or nothing for it -- because closing the whole of it beside an order that could
   * also close it risks selling it twice. `closing_unsizable_` covers a fourth: a holding whose own
   * arithmetic failed, so no size could be stated at all. And `closing_source_unverified_` covers
   * the case before all of them: a pass that never established *which* instructions it owed cannot
   * be complete no matter how many it sent.
   *
   * `true` therefore means "every instruction I asked for was taken, and I knew what to ask for",
   * never "I am flat" and never "nothing is resting". Read the broker's own order list and holdings
   * to learn the state; this call tells you only whether anything failed to go.
   */
  bool closing_complete() const noexcept {
    return closing_rejected_ == 0 && closing_refused_ == 0 && closing_withheld_ == 0 &&
      closing_indeterminate_ == 0 && closing_unavailable_ == 0 && closing_unresolved_ == 0 &&
      closing_contended_ == 0 && closing_unsizable_ == 0 && !closing_source_unverified_;
  }
  /**
   * Instructions the strategy's `on_stop` asked for and the guard refused.
   *
   * Refusing them is the intended outcome rather than a failure, so this does not affect
   * `closing_complete()`: during a stop the operator's instructions are the ones that go, and a
   * strategy's parting order would be sized against nothing the closing pass reasoned about. It is
   * counted because a strategy quietly losing its shutdown behaviour should be visible rather than
   * discovered.
   */
  std::size_t stop_handler_refused_{0};
  /**
   * Instructions the strategy's `on_stop` got out, which should always be zero.
   *
   * Kept as an assertion in the report rather than in a comment: the hold is applied before
   * `on_stop` is dispatched, so a non-zero value here means that ordering has been broken.
   */
  std::size_t stop_handler_sent_{0};

  /** Notes an operator should read, such as an unrecognized control-file token. */
  std::vector<std::string> notes_;

  /** Returns how many polls of `kind` were attempted. */
  std::size_t attempted(PollKind kind) const noexcept {
    return attempted_[static_cast<int>(kind)];
  }

  /** Returns how many polls of `kind` failed. */
  std::size_t failed(PollKind kind) const noexcept { return failed_[static_cast<int>(kind)]; }
};

/**
 * A source that answers nothing, for driving a loop in a test.
 *
 * The counterpart to `RecordingExecutor`, and the reason every loop behaviour in this header is
 * testable with no session, no credentials and no network. A test queues what each poll should
 * return and then asserts what the loop did with it.
 *
 * Deliberately not a mock of a broker. It is a mock of *arrival*: the question the loop answers is
 * "what do I do when data shows up, or does not, or the list read fails", and none of that needs a
 * broker to be interesting.
 *
 * **It also demonstrates the source contract worth copying**: `set_calendar` makes this source install
 * a calendar during `PollKind::CALENDAR` and during no other poll, which is the rule
 * `PollKind::CALENDAR` states and the one the loop cannot enforce.
 */
template <typename Runner>
class ManualSource {
public:
  /** Its queued outcomes stand for a real listing, so a closing pass may rely on them. */
  static constexpr bool provides_working_orders = true;

  /** Queues what the next poll of `kind` returns. */
  void queue(PollKind kind, PollOutcome outcome) {
    queued_[static_cast<int>(kind)].push_back(outcome);
  }

  /** Sets what every unqueued poll of `kind` returns. Defaults to `UNCHANGED`. */
  void set_default(PollKind kind, PollOutcome outcome) {
    defaults_[static_cast<int>(kind)] = outcome;
  }

  /** Sets what the order-list read returns. */
  void set_order_list(PollOutcome outcome) { order_list_ = outcome; }

  /** Sets which currencies this source stands for having been configured to ask about. */
  void set_buying_power_currencies(std::vector<Currency> currencies) {
    currencies_ = std::move(currencies);
  }

  /**
   * Sets the calendar a successful calendar poll installs, and makes that poll succeed.
   *
   * Supplying one is how a test says "the broker answers this", so it also sets the default outcome
   * for `PollKind::CALENDAR` to `UPDATED` -- a test that supplies a calendar and then has to remember
   * a second call to make it arrive would be a test that silently asserts nothing about the phase. A
   * test that wants the poll to fail queues or defaults that outcome afterwards, and then nothing is
   * installed, which is what `live::SessionSource` does too.
   */
  void set_calendar(TradingCalendar calendar) {
    calendar_ = std::move(calendar);
    defaults_[static_cast<int>(PollKind::CALENDAR)] = PollOutcome::UPDATED;
  }

  /** Returns those currencies, which is what lets the gate hold for one that never answered. */
  const std::vector<Currency>& buying_power_currencies() const noexcept { return currencies_; }

  /** Returns how many times the order list was read. */
  std::size_t order_list_reads() const noexcept { return order_list_reads_; }

  /** Returns how many times `kind` was polled. */
  std::size_t polls(PollKind kind) const noexcept { return polls_[static_cast<int>(kind)]; }

  /** Returns the queued outcome for `kind`, or its default, installing a calendar that arrived. */
  PollOutcome poll(PollKind kind, Runner& runner_instance) {
    const int at = static_cast<int>(kind);
    ++polls_[at];
    auto outcome = defaults_[at];
    if (!queued_[at].empty()) {
      outcome = queued_[at].front();
      queued_[at].erase(queued_[at].begin());
    }
    // The one kind whose answer goes somewhere: the loop reads the calendar back out of the context,
    // so a source that did not put it there would leave every phase assertion in this header testing
    // an empty calendar.
    if (kind == PollKind::CALENDAR && calendar_ && outcome == PollOutcome::UPDATED) {
      runner_instance.context().set_calendar(*calendar_);
    }
    return outcome;
  }

  /** Returns the configured order-list outcome. */
  PollOutcome read_order_list(Runner&) {
    ++order_list_reads_;
    return order_list_;
  }

private:
  /**
   * Every kind's default outcome, filled rather than listed.
   *
   * It used to be a list with one entry per `PollKind`, and `PollOutcome::UPDATED` is the zero value
   * -- so adding an enumerator silently defaulted the new kind to "new data arrived" in every test
   * that had not set it. Filling cannot be one short.
   */
  static std::array<PollOutcome, poll_kind_count> unchanged_defaults() {
    std::array<PollOutcome, poll_kind_count> filled{};
    filled.fill(PollOutcome::UNCHANGED);
    return filled;
  }

  std::vector<PollOutcome> queued_[poll_kind_count];
  std::array<PollOutcome, poll_kind_count> defaults_{unchanged_defaults()};
  PollOutcome order_list_{PollOutcome::UPDATED};
  std::vector<Currency> currencies_;
  std::optional<TradingCalendar> calendar_;
  std::size_t polls_[poll_kind_count] = {};
  std::size_t order_list_reads_{0};
};

/**
 * Drives a runner for as long as a program lives, and stops on purpose.
 *
 * `Runner::feed` is one pass and caller-driven, which is right for a backtest and right for a single
 * live decision. What was missing is everything a program needs *around* it, and the hazard is
 * specific: nothing stopped a caller writing `while (true) { runner.feed(next()); }`, at which point
 * they owned a cadence, a rate-limit interaction, a reconciliation obligation and a stop control,
 * with documentation as the only thing between them and a doubled position.
 *
 * **Parameterized on a source, for the same reason the runner is parameterized on an executor.** A
 * polling loop looks like a broker concern and almost none of it is: when to poll next, noticing a
 * stop, refusing to send before the first reconciliation, and deciding a snapshot is too stale to
 * size against are all broker-free. Only the fetching is not. So `SessionSource` does the fetching
 * in `<algotradelib/live>`, `ManualSource` stands in for it here, and every behaviour worth testing
 * is reachable without a network.
 *
 * ## How sending is blocked
 *
 * Through `RiskGuard::halt()`, which is already the library's one documented primitive for "stop
 * sending without withdrawing or flattening" -- exactly what a gate needs, and exactly what
 * `QUIESCE` means. Reusing it means no change to `Runner::settle()` and no second parallel
 * mechanism that could disagree with the first.
 *
 * ## The market phase, which this loop owns
 *
 * Each iteration derives one from `set_session_window`'s window and whatever `PollKind::CALENDAR` has
 * put on the context, installs it **before** the polls, and announces a change **after** them. Before
 * this existed, nothing outside `backtest/` ever set a phase: `on_phase` never fired live, a shipped
 * vanilla built on it did nothing, and a caller who set nothing was refused every order by the
 * default `permitted_phases_`. The two halves of the ordering are separate because a poll can settle
 * an order -- see `step`, which carries the argument.
 *
 * A phase the loop cannot establish is **reported and not held**: `final_phase_` names it and a note
 * says why. See `note_unestablished_phase` for why a hold would be worse than nothing.
 *
 * **If you are writing a source, this is the one rule the loop cannot enforce for you**: write the
 * trading calendar during `PollKind::CALENDAR`, nowhere else, and do not feed the runner from that
 * poll. The loop re-derives the phase before
 * every call it makes into a source, but nothing of its runs between your own `set_calendar` and your
 * own `Runner::feed`, so doing both inside one poll settles an order against the phase as it stood
 * when that poll began. `PollKind::CALENDAR` carries the full argument, and
 * `LoopReport::sends_under_a_revised_phase_` reports a source that breaks it, and the rule has a
 * second half: the calendar poll must not feed either, because a source may not do both in one call.
 *
 * ## The trap a stop must not hide
 *
 * An operator who asked to be flat and was told nothing would believe they were. That is why
 * `LoopReport` counts what the broker *accepted* separately from what the loop *asked for*, and why
 * a refusal produces a note saying the position is still held. `closing_complete()` is the one call
 * that answers "did everything I asked for go out" -- which is not the same question as "am I
 * flat", and deliberately so: acceptance is the last thing this library observes, and the broker's
 * own order list and holdings are the authority on what is actually left.
 *
 * **The phase is no longer what springs this trap, and it was.** `RiskLimits::permitted_phases_`
 * defaults to `{MarketPhase::OPEN}`, and a flatten issued outside market hours used to be refused by
 * it -- so the one instruction an operator most needs to succeed failed because of their own risk
 * limit. `ai/phase-limit-exits` settled that the phase governs what an order *opens* rather than
 * what it closes, so a closing pass now goes out whatever the session is doing, and
 * `LoopReport::sends_outside_permitted_phase_` says when that happened.
 *
 * **The counts stay load-bearing, because every other limit still applies to a close.** A sellable
 * check with no broker reading, a per-order ceiling, a position ceiling, a halt: each of them
 * refuses an exit and each produces the same silence. The trap was never really about the phase.
 *
 * **The loop holds the guard, and it names what it holds.** Its gates are
 * `strategy::HaltReason` values it takes and releases itself -- `ORDER_LIST_UNREAD`,
 * `PORTFOLIO_STALE`, `STOPPING`, `POLL_FAILING`. It never takes `OPERATOR` or `LOCAL_FAILURE`,
 * which belong to a person and to the runner, so an operator halting by hand while the loop's gate
 * is closed is not lost when that gate opens. The loop still *reports* those holds: `final_gate_`
 * names the most fundamental hold in force, whoever owns it, so a run that sent nothing because
 * somebody halted it says so instead of showing an open gate.
 */
template <typename Runner, typename Source, typename Clock = SteadyClock>
class PollingLoop {
public:
  /** Constructs a loop driving `runner` from `source`. Both are borrowed, not owned. */
  PollingLoop(Runner& runner, Source& source, PollSchedule schedule = {}, Clock clock = {})
  : runner_(&runner), source_(&source), schedule_(std::move(schedule)), clock_(std::move(clock)) {}

  /** Returns the clock, so a test can read how long the loop believed it waited. */
  const Clock& clock() const noexcept { return clock_; }
  /** @copydoc PollingLoop::clock() const */
  Clock& clock() noexcept { return clock_; }

  /** Returns the schedule. */
  const PollSchedule& schedule() const noexcept { return schedule_; }

  /** Sets how stale a holdings snapshot may be before sending is blocked. */
  void set_maximum_portfolio_age(std::chrono::nanoseconds age) { maximum_portfolio_age_ = age; }

  /**
   * Sets the session the market phase is derived against.
   *
   * `strategy::SessionWindow::krx()` by default, which is the same default `backtest::ReplayConfig`
   * carries -- so a strategy replayed and then run live derives its phases against the same window
   * unless a caller changes one of them, and a divergence between the two worlds has to be asked for.
   *
   * Operator-stated rather than read from a broker, which `SessionWindow` itself argues: only Toss
   * publishes intraday hours, so deriving them from a broker would give one answer for one broker and
   * nothing for the other.
   */
  void set_session_window(strategy::SessionWindow window) {
    session_window_ = std::move(window);
  }

  /** Returns that session window. */
  const strategy::SessionWindow& session_window() const noexcept { return session_window_; }

  /** Sets the failure policy. */
  void set_failure_policy(FailurePolicy policy) { failure_policy_ = std::move(policy); }

  /** Attaches a control file an operator can write an intent into. */
  void set_control_file(FileStopChannel channel) { control_ = std::move(channel); }

  /**
   * Runs until `stop` is requested, a poll exhausts its failure budget, or `max_iterations` pass.
   *
   * `max_iterations` exists for tests and for a bounded operational run; zero means unbounded. A
   * caller who wants "forever" passes nothing, and a caller who wants one pass has always had
   * `Runner::feed`.
   */
  LoopReport run(StopSource& stop, std::size_t max_iterations = 0) {
    LoopReport report;
    // Carried in rather than left false: the field says whether the order list has been read at
    // least once, and the loop remembers that across runs. A second `run` on the same loop had been
    // reporting it as unread, which reads as a startup gate that never opened.
    report.order_list_read_ = order_list_read_;
    // **Differenced around the run, not accumulated from the passes.** An order can leave the
    // process from inside a source's `poll`, which calls `Runner::feed` itself and discards the
    // report it gets back, so a per-pass figure would only ever see what a stop did. The runner's
    // counter is monotonic for exactly this reason, and reading it at both ends is how
    // `RiskGuard::total_sent` is already used around a callback. Taken per `run` so a second run on
    // the same loop reports its own sends rather than the first one's as well.
    const auto outside_phase_before = runner_->sends_outside_permitted_phase();
    // Differenced around the run for the same reason, and the reason is sharper here: the source
    // survives `run` returning, so an absolute figure would report the previous run's charges again.
    const auto charged_before = [&]() {
      if constexpr (source_states_charged_costs<Source>::value) {
        return source_->charged_costs();
      } else {
        return ChargedCosts{};
      }
    }();
    for (auto& due : next_due_) {
      due.reset();
    }
    for (auto& count : consecutive_failures_) {
      count = 0;
    }

    while (max_iterations == 0 || report.iterations_ < max_iterations) {
      const auto iteration = step(stop, report);
      ++report.iterations_;
      if (iteration.gate_) {
        ++report.blocked_;
      }
      report.final_gate_ = iteration.gate_;
      report.final_phase_ = iteration.phase_;
      if (iteration.phase_changed_) {
        ++report.phase_transitions_;
      }
      last_ = iteration;

      if (report.abandoned_) {
        break;
      }
      if (const auto intent = stop.intent()) {
        honour(*intent, report);
        report.stopped_by_ = intent;
        break;
      }
      clock_.wait_for(until_next(iteration.at_));
    }

    // Deliberately **not** released here. A run that ended because an operator asked it to, or
    // because a poll kept failing, must leave the runner unable to send: releasing the halt on the
    // way out would reopen the gate at exactly the moment the loop stopped watching it. A caller who
    // genuinely wants to trade again resumes the guard themselves, which is a deliberate act.
    //
    // A healthy run that simply exhausted `max_iterations` never halted anything, so there is
    // nothing to leave behind in that case.
    report.sends_outside_permitted_phase_ =
      runner_->sends_outside_permitted_phase() - outside_phase_before;
    if constexpr (source_states_charged_costs<Source>::value) {
      report.charged_costs_ = source_->charged_costs().since(charged_before);
    }
    note_sellable_convention(report);
    note_unestablished_phase(report);
    return report;
  }

  /** Returns what the last iteration did. */
  const IterationReport& last() const noexcept { return last_; }

private:
  /**
   * Puts the sellable-convention probe's verdict in the report, when it reached one.
   *
   * The probe answers a question neither broker's documentation does -- whether the sellable figure
   * already excludes resting sells -- and it answers it as a by-product of ordinary polling. An
   * operator should not have to write code to read that, so a run that learned something says so,
   * with the counts behind it because one observation is evidence rather than proof.
   *
   * Silent when nothing was observed, which is most runs: a note saying "undetermined" on every run
   * that never sold anything is noise, and noise is how a report stops being read.
   */
  void note_sellable_convention(LoopReport& report) const {
    const auto evidence = runner_->sellable_convention().overall();
    if (evidence.total() == 0) {
      return;
    }
    report.notes_.push_back(
      "broker's sellable figure appears " +
      std::string(strategy::sellable_convention_name(evidence.verdict())) + " (" +
      std::to_string(evidence.net_) + " net, " + std::to_string(evidence.gross_) + " gross)"
    );
  }

  /** Runs one iteration: poll what is due, then let the runner act if the gate is open. */
  IterationReport step(StopSource& stop, LoopReport& report) {
    IterationReport iteration;
    iteration.at_ = clock_.now();

    // Read before polling, so an operator's stop is honoured without first spending requests on a
    // broker the loop is about to walk away from.
    check_control_file(stop, report);

    // **Installed before anything can send, and announced only after the polls.** The two halves are
    // separate on purpose and neither is optional.
    //
    // A source's `poll` calls `Runner::feed`, which settles, so an order can leave the process from
    // inside a poll -- the same fact the gate below is computed early for. If the phase were installed
    // after the polls, the first bar arriving after the close would be settled while the context still
    // said `OPEN`, and an opening order would pass a phase limit that exists to refuse it. Once per
    // session, on the close.
    //
    // Announcing the change is the opposite case: `on_phase` may place orders, and it should see this
    // iteration's polls rather than the previous one's -- fresh prices, a fresh portfolio, and a gate
    // re-evaluated against them. That is what `backtest/engine.hpp:283-306` does for the replay, with
    // the same argument: a handler deciding on a mark from one moment beside a price from another is
    // not looking at a fresher picture, it is looking at an incoherent one.
    install_phase(iteration);

    // **Established before any poll runs, because a poll is what feeds the runner.** A source's
    // `poll` calls `Runner::feed` itself -- `live::SessionSource::poll_bars` does, per instrument --
    // and `feed` settles, so an order can leave the process from inside a poll. Computing the gate
    // afterwards would mean the startup gate and the staleness gate were installed one poll too
    // late, and on the very first iteration they would not be installed at all: a bar arriving
    // before the first order-list read could place an order against a position a predecessor may
    // have left, which is the exact hazard the startup gate exists for.
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/true));

    for (const auto kind : all_poll_kinds) {
      if (!is_due(kind, iteration.at_)) {
        continue;
      }
      // **Re-established immediately before every poll**, because a poll is a point at which an order
      // can leave the process and anything that ran before it may have replaced the calendar the phase
      // was derived from. Three things can have: a previous poll in this same loop, the calendar poll
      // in particular since it is polled first, and `gate_for`'s out-of-band `read_order_list` above.
      //
      // Before rather than after, which is the correction: "after every poll" closed the first two and
      // left the third, because the pre-loop install runs *before* `gate_for` and nothing re-derived
      // the phase between that call and the first poll. Placing it here makes the rule positional
      // instead of enumerated -- every send site is preceded by an install, and no list of which polls
      // can change what has to stay correct.
      //
      // The cost is a date comparison per due poll.
      install_phase(iteration);
      const auto watch = watch_phase(iteration);
      const auto outcome = source_->poll(kind, *runner_);
      note_revised_phase(std::string(poll_kind_name(kind)) + " poll", watch, report);
      // `UNCHANGED` counts, because a listing that is identical to the last one is still a listing
      // -- but only on a source that publishes them at all, which `note_working_orders_read`
      // enforces: a broker without the inquiry answers `UNCHANGED` without asking anybody.
      if (kind == PollKind::WORKING_ORDERS &&
          (outcome == PollOutcome::UPDATED || outcome == PollOutcome::UNCHANGED)) {
        note_working_orders_read(iteration.at_);
      }
      ++report.attempted_[static_cast<int>(kind)];
      iteration.polls_.emplace_back(kind, outcome);
      schedule_next(kind, iteration.at_);
      note_outcome(kind, outcome, report);
      if (outcome == PollOutcome::UPDATED) {
        iteration.fed_ = true;
      }
      if (report.abandoned_) {
        // Recorded before returning, so the report does not claim sending was permitted while the
        // guard `note_outcome` just halted says otherwise.
        iteration.gate_ = runner_->guard().blocking_reason();
        return iteration;
      }
    }

    // Observed after the polls, not before: a poll can refresh the listing without a bar arriving,
    // and that is precisely when an order of ours begins showing as resting. Observing first would
    // read every iteration's portfolio one iteration late. A repeat observation of unchanged state
    // concludes nothing, so a quiet iteration costs a scan and teaches the probe nothing.
    runner_->observe_sellable_convention();

    // Re-evaluated after the polls so the report and the next iteration reflect what those polls
    // changed -- a holdings refresh should lift the staleness hold now, not one iteration later.
    // The order list is deliberately not attempted a second time: one attempt per iteration, and
    // the gate simply stands until the next one.
    // The last send site: the announcement below settles what `on_phase` asks for, and the final poll
    // may have replaced the calendar after its own pre-poll install.
    install_phase(iteration);
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/false));

    // Announced last, so the handler runs on everything this iteration established. A change is
    // announced once, not every iteration it persists for.
    if (announced_ != iteration.phase_) {
      // **The clock moves with the phase, and only forwards.** `Runner::feed(MarketPhase)` does not
      // advance `Context::now_`; only a bar or a quote does, from a broker's stamp. So a daily-bar run
      // would hand `on_phase` an instant hours old, and `RiskGuard` files a send at
      // `GuardInputs::now_` and counts sends within `rate_window_` of it -- the phase order and the
      // next bar's order would be filed at what the guard thinks are two unrelated moments, and two
      // orders would pass a one-order window. `backtest/engine.hpp:296-302` documents the same hazard
      // for the replay.
      //
      // Forwards only: a broker stamp ahead of this machine's clock must not be walked backwards,
      // because a handler deciding at an instant *earlier* than the bar it has already been shown is
      // the same incoherence with the sign flipped. The skew is the broker's and is not this loop's
      // to quietly correct.
      //
      // **What this does not fix**, stated because it is easy to read more into it: a bar fed earlier
      // in this same iteration set `now_` to its own period end, which on a daily bar is yesterday's
      // close. So that order is filed at the bar's instant and this one at the wall clock, and a rate
      // window narrower than the gap sees them as unrelated. That is inherent to a bar's decision
      // instant being its period end -- which the replay depends on -- and is not introduced here:
      // before this, the phase order did not exist to be filed at all.
      if (runner_->context().now() < iteration.at_) {
        runner_->context().set_now(iteration.at_);
      }
      announced_ = iteration.phase_;
      iteration.phase_changed_ = true;
      // Watched for the same reason a poll is: `Context::set_calendar` is public, so an `on_phase`
      // handler can revise the calendar and submit in one call, and `settle` runs before this returns.
      const auto watch = watch_phase(iteration);
      runner_->feed(iteration.phase_);
      note_revised_phase("on_phase handler", watch, report);
    }

    // Reported from the guard rather than from what the loop just computed, so the report names the
    // most fundamental hold in force whoever owns it -- an operator's halt, or a journal that
    // failed, outranks anything the loop decided.
    iteration.gate_ = runner_->guard().blocking_reason();
    return iteration;
  }

  /** What the phase and the send count were before the loop handed control to somebody else. */
  struct PhaseWatch {
    std::chrono::system_clock::time_point at_{};
    strategy::MarketPhase phase_{strategy::MarketPhase::UNKNOWN};
    std::size_t sends_{0};
  };

  /** Records the phase and the send count, for comparison after a call into user code. */
  PhaseWatch watch_phase(const IterationReport& iteration) const {
    return PhaseWatch{iteration.at_, iteration.phase_, runner_->guard().total_sent()};
  }

  /**
   * Counts and names a call that sent an order and left the phase changed behind it.
   *
   * **The one hazard here the loop cannot prevent and can only witness.** It re-derives the phase before
   * every call it makes that could send, but nothing of its runs *inside* such a call: a
   * source that replaces the calendar and then feeds, or an `on_phase` handler that does the same before
   * submitting, settles an order against the phase as it stood when the call began. So the loop looks
   * afterwards, which turns an invisible violation into a line in the report -- the same choice
   * `closing_source_unverified_` makes about a listing it could not trust.
   *
   * **Both halves are required, and each one removes a wrong answer.** The phase must have moved, or an
   * ordinary send would be reported. Something must have been sent, or an ordinary calendar poll would be
   * reported on every iteration -- which is why this no longer excludes the calendar poll by kind. That
   * exclusion was a false-positive workaround and it hid the case Codex round 3 found: a source obeying
   * the letter of the contract, writing the calendar in the calendar poll and *then* feeding.
   *
   * `total_sent()` rather than the guard's rate-window deque, because that one prunes and can fall, so
   * it cannot be differenced across a call.
   */
  void note_revised_phase(const std::string& what, const PhaseWatch& before, LoopReport& report) {
    if (runner_->guard().total_sent() == before.sends_) {
      return;
    }
    if (phase_at(before.at_) == before.phase_) {
      return;
    }
    ++report.sends_under_a_revised_phase_;
    if (report.sends_under_a_revised_phase_ == 1) {
      // Once per run, not once per call: a strategy doing this on every bar would otherwise bury every
      // other note under thousands of copies of this one.
      // **Worded to exactly what was observed**, which is co-occurrence and not causation. Two cases
      // the stronger claim would have got wrong: a call that sent *first* and revised the calendar
      // afterwards, where no order used the stale answer; and a call that only cancelled, since
      // `Runner::send_cancel` never consults the phase at all. Both still break the writer contract and
      // are worth reporting, so the note states the facts and names the check rather than the verdict.
      report.notes_.push_back(
        "the " + what + " changed the market phase and sent a request in the same call; check whether "
        "that request used the earlier phase. Only the calendar poll may write the trading calendar, "
        "and it must not send");
    }
  }

  /**
   * Computes the phase for this iteration and installs it, so nothing can send under a stale one.
   *
   * **Called immediately before every point at which this loop hands control to the source**: once
   * before `gate_for`, which performs an out-of-band order-list read; once before each due poll; and
   * once before the phase announcement. So the invariant is exactly this, and no stronger:
   * **whenever the loop calls into the source, `Context::phase_` is derived from the calendar as it
   * stood at that call.**
   *
   * **What that deliberately does not promise**, because it cannot: a source is free to replace the
   * calendar and then feed the runner inside a single `poll`, and no loop code runs in between, so
   * that order would settle against the phase as it stood when the poll began. `PollKind::CALENDAR`
   * documents the contract that rules it out, `SessionSource` satisfies it -- it writes the calendar in
   * the calendar poll and nowhere else -- and `note_revised_phase` reports a source that
   * breaks it, because a violation nobody is told about is one nobody fixes. An earlier version of this
   * comment claimed the stronger at-send-site invariant; Codex round 2 falsified it with a probe.
   *
   * The instant is the iteration's own `at_` throughout, never re-read from the clock, so a poll that
   * took a second does not move the session boundary mid-iteration.
   */
  void install_phase(IterationReport& iteration) {
    iteration.phase_ = phase_at(iteration.at_);
    // Recorded here rather than derived later, and it outlives a `run` for the same reason `announced_`
    // does: "this loop has never known the phase" is a claim about the loop, not about one call.
    if (iteration.phase_ != strategy::MarketPhase::UNKNOWN) {
      phase_ever_known_ = true;
    }
    runner_->context().set_phase(iteration.phase_);
  }

  /**
   * Returns the phase at `at`, from the configured window and whatever calendar has arrived.
   *
   * The composition `ReplayClock` already performs, with a polled calendar in place of a replay's
   * per-bar answer. An uncovered date is `UNKNOWN` and never "closed": `TradingCalendar` draws that
   * distinction because assuming closed stops a strategy on a day that is in fact open, and assuming
   * open lets one order into a holiday.
   *
   * The date is the *market's*, not this process's. `SessionWindow` owns the offset and derives both
   * answers from it, so the day the calendar is asked about and the day the session boundaries are
   * computed for cannot disagree.
   */
  strategy::MarketPhase phase_at(std::chrono::system_clock::time_point at) const {
    const auto& calendar = runner_->context().calendar();
    return session_window_.phase(at, calendar.is_trading_day(session_window_.local_date(at)));
  }

  /**
   * Says so when a run never established the phase, and names which of the three reasons applies.
   *
   * Silent otherwise, like `note_sellable_convention`: a line on every healthy run is how a report
   * stops being read. But a run that placed nothing because the phase was never known must not look
   * like a quiet market -- and this is **not** a hold on the guard, deliberately. A hold would also
   * stop cancellations, which `Runner::send_cancel` refuses on any blocking reason while never
   * consulting the phase at all; taking one because a reference-data poll failed would make an
   * unknown phase more restrictive than a closed one, and a withdrawal only ever reduces exposure.
   * `RiskGuard` already refuses every placement that opens or increases a position with
   * `PHASE_NOT_PERMITTED`, and permits one that closes a held position -- which is exactly what a
   * hold would wrongly stop, for the same reason it would wrongly stop a cancellation.
   */
  void note_unestablished_phase(LoopReport& report) const {
    if (report.iterations_ == 0 || report.final_phase_ != strategy::MarketPhase::UNKNOWN) {
      return;
    }
    // **"Never" is a claim, so it is tracked rather than derived.** A loop can establish the phase and
    // then lose it -- the local date rolls past what the calendar covers, or the broker revises it --
    // and telling an operator it was never established would send them looking for a configuration
    // fault that is not there.
    //
    // This used to read `report.phase_transitions_ > 0`, which Codex round 2 falsified: `announced_`
    // outlives a `run` while the report's counter restarts at zero, so a run with nothing left to
    // announce reports no transitions and would claim the phase had never been known -- of a loop that
    // spent its first run open. The verdict was right in the case that was tested and the reasoning was
    // wrong, which is the worse of the two.
    const bool ever_known = phase_ever_known_;
    // Three reasons, not four. "The poll never came due" was here and is unreachable: `run` resets
    // `next_due_`, so a configured cadence makes the calendar due on the first iteration of every
    // run. A branch describing a state that cannot happen is worse than no branch, because a reader
    // who sees it believes it can.
    std::string reason;
    if (!schedule_.calendar_) {
      reason = "no calendar cadence is configured";
    } else if (report.failed(PollKind::CALENDAR) == report.attempted(PollKind::CALENDAR)) {
      reason = "every calendar poll failed";
    } else if (runner_->context().calendar().days_.empty()) {
      // What the loop can actually tell: a poll reported success and no calendar arrived. It does
      // **not** claim to know why, because it cannot distinguish a broker that publishes none -- where
      // the request is not even compiled -- from a source that fetched one and installed nothing.
      reason = "a calendar poll succeeded but no calendar arrived (this broker may publish none)";
    } else {
      // Named, not merely called uncovered. "The calendar does not cover today" sends a reader to
      // check a span they cannot see; the date is the one thing that turns this note into an action.
      const auto date = session_window_.local_date(last_.at_);
      reason = "the calendar does not cover " + std::to_string(date.year_) + "-" +
        std::to_string(date.month_) + "-" + std::to_string(date.day_);
    }
    report.notes_.push_back(
      std::string(ever_known ?
        "the market phase stopped being established (" :
        "the market phase was never established (") +
      reason + "); a guard permitting only known phases refused every placement opening a position" +
      (ever_known ? " after that" : ""));
  }

  /**
   * Returns which of the loop's *own* conditions blocks sending, reading the order list if due.
   *
   * Only conditions the loop computes, because a hold is released by whoever took it. `OPERATOR`
   * belongs to a person and `LOCAL_FAILURE` to the runner; a loop that took either would be a
   * second holder of someone else's hold -- the defect `HaltReason` exists to prevent,
   * reintroduced one layer up.
   *
   * **Always evaluated, even while someone else's hold stands.** Short-circuiting on a foreign hold
   * would leave the loop's own hold un-maintained underneath it, so the moment an operator resumed,
   * the guard would be wide open against a portfolio nobody had rechecked.
   *
   * Called twice per iteration -- before the polls, which is what actually protects anything, and
   * after them so the report is current. `may_read` is false on the second call so the order-list
   * read is attempted at most once per iteration.
   */
  std::optional<strategy::HaltReason> gate_for(
    const StopSource& stop,
    std::chrono::system_clock::time_point at,
    LoopReport& report,
    bool may_read
  ) {
    if (stop.requested()) {
      return strategy::HaltReason::STOPPING;
    }
    if (!order_list_read_) {
      // Not attempted when the caller says not to, and not attempted at all while the runner's
      // journal is broken: a runner that cannot record what it does must not be sent looking for
      // more work to fail at. Either way the gate stands, which is the safe answer.
      if (!may_read || runner_->guard().halted(strategy::HaltReason::LOCAL_FAILURE)) {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
      // Watched like a poll, because it is the same kind of thing: a call into the source, from which a
      // source that revised the calendar and sent would reach the identical hazard through a different
      // door. The phase in force here is the one `step` installed immediately before calling this.
      const auto watch = PhaseWatch{at, phase_at(at), runner_->guard().total_sent()};
      const auto outcome = source_->read_order_list(*runner_);
      note_revised_phase("order-list read", watch, report);
      if (outcome == PollOutcome::UPDATED || outcome == PollOutcome::UNCHANGED) {
        order_list_read_ = true;
        report.order_list_read_ = true;
        // The startup read is a working-order read *on a source that has the inquiry*. On one that
        // does not, it reads order history for the journal and never populates `working_`, so
        // `note_working_orders_read` deliberately does nothing and a closing pass stays unverified.
        note_working_orders_read(at);
      } else {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
    }
    if (maximum_portfolio_age_) {
      // Measured from the snapshot's own `as_of_` rather than from when the poll returned, because
      // that is when the broker says the positions were true. A poll that succeeded at noon carrying
      // a snapshot the broker took at 11:00 is an hour stale, and the poll's own timestamp would call
      // it fresh.
      const auto as_of = runner_->context().portfolio().as_of_;
      if (as_of == std::chrono::system_clock::time_point{}) {
        // A default-constructed `Portfolio` carries the epoch, and no broker ever reports a snapshot
        // taken in 1970, so the epoch means "never populated". Treated as stale rather than as
        // fresh-but-empty, which is the safe direction: an empty portfolio read as current would let
        // a strategy size as though it held nothing.
        return strategy::HaltReason::PORTFOLIO_STALE;
      }
      if (at - as_of > *maximum_portfolio_age_) {
        return strategy::HaltReason::PORTFOLIO_STALE;
      }
    }
    if (runner_->guard().limits().enforce_buying_power_) {
      // Checked only when the caller asked for the limit. A loop with no buying-power cadence and
      // no check configured is an ordinary loop, and must not be held for a reading nobody wants.
      if (const auto unread = buying_power_unread(at)) {
        return *unread;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns whether some budget the run needs is missing or too old to size against.
   *
   * **Every configured currency must have its own fresh reading**, not merely one of them. A source
   * that names its currencies is asked, because an account trading two and answering for one is a
   * partial answer: the per-order guard would still refuse the unread currency, but the loop would
   * go on reporting an open gate while a budget it was told to watch never arrived at all.
   *
   * A source that names none falls back to "any reading at all", which is the most the loop can
   * check without knowing what was asked for.
   */
  std::optional<strategy::HaltReason> buying_power_unread(
    std::chrono::system_clock::time_point at
  ) const {
    const auto& readings = runner_->context().portfolio().buying_power_;
    // Aged against the reading's own stamp, for the same reason the portfolio is: a figure that
    // arrived an hour ago is an hour old however recently the poll succeeded.
    const auto usable = [&](const BuyingPower& reading) {
      return !maximum_buying_power_age_ ||
        at - reading.received_at_ <= *maximum_buying_power_age_;
    };
    if constexpr (source_names_buying_power_currencies<Source>::value) {
      const auto& wanted = source_->buying_power_currencies();
      if (!wanted.empty()) {
        for (const auto& currency : wanted) {
          bool found = false;
          for (const auto& reading : readings) {
            if (reading.cash_.currency_ == currency && usable(reading)) {
              found = true;
              break;
            }
          }
          if (!found) {
            return strategy::HaltReason::BUYING_POWER_UNREAD;
          }
        }
        return std::nullopt;
      }
    }
    if (readings.empty()) {
      return strategy::HaltReason::BUYING_POWER_UNREAD;
    }
    for (const auto& reading : readings) {
      if (!usable(reading)) {
        return strategy::HaltReason::BUYING_POWER_UNREAD;
      }
    }
    return std::nullopt;
  }

  /** Sets how old a buying-power reading may be before the gate closes on it. */
public:
  void set_maximum_buying_power_age(std::chrono::nanoseconds age) {
    maximum_buying_power_age_ = age;
  }

private:
  /** Reads the control file, escalating `stop` when it names an intent. */
  void check_control_file(StopSource& stop, LoopReport& report) {
    if (!control_.enabled()) {
      return;
    }
    if (const auto intent = control_.poll()) {
      stop.request(*intent);
      return;
    }
    if (!control_.unrecognized().empty()) {
      // Reported rather than guessed at, because picking the nearest intent would decide how much of
      // an operator's position to close on their behalf.
      report.notes_.push_back(
        "control file names no intent: '" + control_.unrecognized() + "'");
    }
  }

  /** Counts an outcome, and abandons the run when one kind keeps failing. */
  void note_outcome(PollKind kind, PollOutcome outcome, LoopReport& report) {
    const int at = static_cast<int>(kind);
    if (outcome == PollOutcome::FAILED) {
      ++report.failed_[at];
      ++consecutive_failures_[at];
      if (consecutive_failures_[at] >= failure_policy_.consecutive_limit_) {
        report.abandoned_ = true;
        report.abandoned_kind_ = kind;
        report.notes_.push_back(
          std::string("gave up after repeated failures polling ") + std::string(poll_kind_name(kind)));
        hold(strategy::HaltReason::POLL_FAILING);
      }
      return;
    }
    // A rate-limit refusal is the limiter working, and nothing new is the normal case between bars.
    // Neither resets the counter either: a kind alternating failure and refusal is still broken.
    if (outcome != PollOutcome::RATE_LIMITED) {
      consecutive_failures_[at] = 0;
    }
  }

  /** Carries out `intent`, which is where the three stops actually differ. */
  void honour(StopIntent intent, LoopReport& report) {
    hold(strategy::HaltReason::STOPPING);

    // **The strategy is told first, and cannot trade on the way out.** `on_stop` is dispatched here,
    // while the hold stands, so every instruction it asks for is refused by the guard and counted.
    // It must not be dispatched later: from the moment the loop queues its own closing orders, a
    // strategy's order would be planned against the same snapshot and settled in the same window,
    // going through none of the sizing the closing pass does -- a second full-size close beside the
    // first, or a placement during a `WITHDRAW`, which promises none.
    // **Not watched, and it does not need to be.** `on_stop` runs with a mutable context and can revise
    // the calendar like any other handler, but `hold(STOPPING)` above is already in force, so every
    // instruction it asks for is refused before anything is sent -- which `stop_handler_sent_` asserts as
    // a report field precisely so that ordering cannot be broken silently. The revision it may have made
    // *is* answered, one call later: the closing derive below picks it up.
    //
    // For the record, the complete list of calls this loop makes out of itself is: `source_->poll`,
    // `source_->read_order_list`, `source_->buying_power_currencies` (a const accessor that is handed no
    // runner and so can neither feed nor write a calendar), `runner_->observe_sellable_convention` (which
    // reads the portfolio and dispatches nothing), this one, and `settle_closing`. The two that can send
    // under a phase are watched.
    const auto parting = runner_->notify_stop();
    report.stop_handler_refused_ = parting.refused_.size() + parting.refused_cancel_.size() +
      parting.rejected_.size() + parting.withheld_.size() + parting.unavailable_.size();
    report.stop_handler_sent_ = parting.would_place_.size() + parting.would_cancel_.size();

    if (intent == StopIntent::QUIESCE) {
      return;
    }

    // **Both intents state their instructions outright rather than expressing them as a target.**
    //
    // A withdraw always had to: there is no target to reconcile against, so the working orders are
    // named one by one. A flatten used to state a zero `TargetPosition` and let reconciliation do
    // the cancelling, which was wrong twice over -- reconciliation keeps a resting order that points
    // the same way as the target, so the flatten could send nothing at all, and it skipped orders on
    // instruments with no position to target. Stating both halves here means one withdrawal per
    // resting order, no instruction derived from another, and no case where the two disagree.
    //
    // Recorded before anything is queued, because it is a statement about the picture the pass is
    // built from rather than about any instruction in it.
    report.closing_source_unverified_ = !closing_source_verified(clock_.now());

    const auto& portfolio = runner_->context().portfolio();
    if (intent == StopIntent::WITHDRAW) {
      for (const auto& working : portfolio.working_) {
        runner_->context().cancel(working.ref_, "stop-withdraw");
        ++report.withdrawn_;
      }
    } else {
      // **A flatten withdraws everything, then closes only what it cannot double-sell.**
      //
      // Two ways to get this wrong, and the library has now had both. Expressing the flatten as a
      // zero `TargetPosition` lets reconciliation treat a resting same-side order as progress and
      // send nothing at all -- a limit sell that may never fill is not being out of the market.
      // Sending the *whole* holding instead ignores that order, and since a cancellation is a
      // request rather than an outcome, the resting sell can fill while its cancellation is still
      // pending: ten long becomes ten short. The second error is much worse than the first, because
      // it manufactures a position in the opposite direction rather than leaving the one that was
      // already there.
      //
      // So the close is sized to what cannot double up: the holding minus whatever is still working
      // on the same side. Every working order is withdrawn either way, so a cancellation that
      // completes leaves part of the position open rather than a resting order nobody is watching,
      // and a cancellation that fails because the order filled leaves the account flat. Neither
      // outcome can overshoot, and the pass says it could not prove which one happened.
      for (const auto& working : portfolio.working_) {
        runner_->context().cancel(working.ref_, "stop-flatten");
        ++report.withdrawn_;
      }
      // **One close per instrument, sized from the net of its rows.** This looped over rows, and a
      // broker reporting one instrument as several lots is ordinary -- so it went wrong twice at
      // once. Rows of +100 and -40 are a holding of +60, and it submitted SELL 100 *and* BUY 40:
      // outside the session the phase rule refuses both, so the operator's flatten sent nothing
      // although SELL 60 was permitted all along. And the deduction below subtracts what is working
      // on the closing side, which is a figure for the whole instrument -- subtracted once per row,
      // it was charged twice, so rows of +60 and +40 against a working SELL 20 sent 40 and 20 and
      // left 20 of the position behind. Neither is reachable once the instrument is the unit.
      std::vector<InstrumentId> closed;
      for (const auto& position : portfolio.positions_) {
        bool seen = false;
        for (const auto& already : closed) {
          if (already == position.instrument_) {
            seen = true;
            break;
          }
        }
        if (seen) {
          continue;
        }
        closed.push_back(position.instrument_);

        // The net, not this row. A snapshot whose rows will not add up cannot say what flattening
        // would take, and guessing is the one thing a closing pass must not do.
        const auto net = portfolio.net_quantity_of(position.instrument_);
        if (!net) {
          // **Not `closing_contended_`.** That one means a competing order, and its note sends an
          // operator to the order list. What failed here is the holding's own arithmetic.
          ++report.closing_unsizable_;
          continue;
        }
        if (net->coefficient() == 0) {
          // Flat on the net, however many rows say so. A closed position can sit in a snapshot at
          // zero, and two rows can cancel; an order for none of it is one the broker would reject,
          // for nothing.
          continue;
        }
        if (report.closing_source_unverified_) {
          // **Nothing is placed against a listing that cannot be trusted.** The deduction below
          // subtracts what is *known* to be working on the closing side, and an unverified picture
          // is precisely one where that is not all of it: an order this runner sent after the last
          // listing, or a source that cannot enumerate orders at all. Sending a full-size close
          // beside an order nobody can see is the same double sell, reached by a different door --
          // and reporting it afterwards does not recall the instruction.
          //
          // The withdrawals above still went, because cancelling only ever reduces exposure. What
          // is left is the position, held and reported, which is the safe end of every branch here.
          ++report.closing_contended_;
          continue;
        }
        const bool held_long = decimal_less(Decimal64{}, *net);
        const auto magnitude = held_long ?
          std::optional<Decimal64>(*net) :
          decimal_subtract(Decimal64{}, *net);
        if (!magnitude) {
          // **A representable net can have no representable magnitude**: two rows summing to the
          // most negative `Decimal64` have no positive counterpart, so the size of the buy that
          // would close them cannot be stated. This used to `continue` in silence, which moved no
          // count -- so `closing_complete()` returned true over a position the pass had never
          // sized, which is the one thing that call promises it will not do.
          ++report.closing_unsizable_;
          continue;
        }
        const OrderSide closing_side = held_long ? OrderSide::SELL : OrderSide::BUY;
        const auto contending = portfolio.working_quantity(position.instrument_, closing_side);
        const auto sendable = decimal_subtract(*magnitude, contending);
        if (!sendable) {
          // The same silence for the same reason: a size that cannot be computed is not a size of
          // zero, and a pass that skipped it did not knowingly send nothing. **Unlike the two cases
          // above, this one can fail with both inputs representable** -- a held magnitude and a
          // working quantity whose scales cannot be aligned -- which is why the note names the
          // working orders as well as the holdings.
          ++report.closing_unsizable_;
          continue;
        }
        if (contending.coefficient() != 0) {
          // Sized down, and the reduction is a fact about what this pass could not establish rather
          // than about what the operator asked for. Counted so `closing_complete()` cannot claim a
          // flatten that may have left part of the position open.
          ++report.closing_contended_;
        }
        if (decimal_less(*sendable, Decimal64{}) || sendable->coefficient() == 0) {
          // Everything held is already working on the closing side. Sending anything at all here is
          // the over-close, so the withdrawal above is the whole of this instrument's flatten.
          continue;
        }
        runner_->context().submit(
          strategy::OrderIntent::market(
            position.instrument_,
            closing_side,
            strategy::SizeSpec::quantity(*sendable)
          ).with_tag("stop-flatten")
        );
        ++report.flattened_;
      }
    }

    // Lifted so the closing instructions can actually go out -- a withdraw that cannot send is not a
    // withdraw -- and closed again immediately afterwards. The window is exactly one `stop()` call
    // wide, and it is the only point at which a stop in progress can send anything.
    //
    // Only the loop's *own* hold lifts. An operator who halted by hand, or a runner whose journal
    // failed, still refuses every closing instruction -- placements through `RiskGuard::check` and
    // cancellations through the same holds in `Runner::send_cancel` -- and the closing counts say
    // so, so `closing_complete()` is false and the notes say the position is still held. A stop
    // arriving by signal or control file therefore cannot send anything through a halt somebody had
    // already set.
    hold(std::nullopt);
    // **Derived once more, immediately before the only call that can send during a stop.** `on_stop` ran
    // a few lines above with a mutable context, so it may have revised the calendar -- and unlike a
    // strategy's write inside `on_phase`, this one the loop *can* answer, because `settle_closing` is a
    // separate call it makes itself. Codex round 3 reproduced the version without this: a handler
    // revising the calendar to closed, and the flatten going out under the `OPEN` it replaced.
    //
    // At `clock_.now()` rather than the iteration's instant, because a stop is honoured after the
    // iteration and a closing pass should be checked against the session as it is now.
    const auto closing_at = clock_.now();
    runner_->context().set_phase(phase_at(closing_at));
    const auto watch = PhaseWatch{closing_at, phase_at(closing_at), runner_->guard().total_sent()};
    const auto closing = runner_->settle_closing();
    note_revised_phase("closing pass", watch, report);
    hold(strategy::HaltReason::STOPPING);

    report.closing_placed_ = closing.would_place_.size();
    // **Both halves of every bucket.** A closing instruction is a placement or a cancellation, and
    // each can fail in the same five ways; counting only the placement half is how a stop that sent
    // nothing reported itself complete.
    report.closing_rejected_ = closing.rejected_.size() + closing.rejected_cancel_.size();
    report.closing_refused_ = closing.refused_.size() + closing.refused_cancel_.size();
    report.closing_withheld_ = closing.withheld_.size();
    report.closing_indeterminate_ =
      closing.indeterminate_.size() + closing.indeterminate_cancel_.size();
    report.closing_unavailable_ = closing.unavailable_.size() + closing.unavailable_cancel_.size();
    report.closing_unresolved_ = closing.unresolved_cancel_.size();

    // Named individually rather than summarized, because "your flatten did not go out" is the one
    // note in this whole report an operator must not have to infer.
    if (!closing.refused_.empty() || !closing.refused_cancel_.empty()) {
      report.notes_.push_back(
        "a guard refused " + std::to_string(report.closing_refused_) +
        " closing instruction(s); the position is still held");
    }
    if (!closing.rejected_.empty()) {
      // Deliberately does not name who decided. The bucket holds three things -- reconciliation
      // could not build the instruction, the broker refused it, or the binding refused to send it
      // -- and an operator reading a stop report needs the consequence, which is the same for all
      // three. The report's own entries carry which it was, on `RejectReason` for a placement and
      // on `by_broker_` for a cancellation.
      report.notes_.push_back(
        std::to_string(closing.rejected_.size()) +
        " closing instruction(s) were not placed; the position is still held");
    }
    if (!closing.rejected_cancel_.empty()) {
      report.notes_.push_back(
        std::to_string(closing.rejected_cancel_.size()) +
        " closing cancellation(s) were refused; read the broker's order list for their state");
    }
    if (report.closing_withheld_ != 0) {
      report.notes_.push_back(
        "withheld " + std::to_string(report.closing_withheld_) +
        " closing instruction(s) pending reconciliation; read the broker's order list");
    }
    if (report.closing_indeterminate_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_indeterminate_) +
        " closing instruction(s) failed in transit; their outcome is unknown");
    }
    if (report.closing_unavailable_ != 0) {
      report.notes_.push_back(
        "local machinery could not send " + std::to_string(report.closing_unavailable_) +
        " closing instruction(s); no broker was asked and the position is still held");
    }
    if (report.stop_handler_refused_ != 0) {
      report.notes_.push_back(
        "the strategy's on_stop asked for " + std::to_string(report.stop_handler_refused_) +
        " instruction(s) during the stop; none were sent, because an operator's stop is not a"
        " moment for a strategy to trade");
    }
    if (report.closing_contended_ != 0) {
      report.notes_.push_back(
        report.closing_source_unverified_ ?
          "no closing order was sent for " + std::to_string(report.closing_contended_) +
            " position(s), because an order this loop cannot see could close them too; they are"
            " still held and the withdrawals above are all that went" :
          std::to_string(report.closing_contended_) +
            " position(s) were closed only for the part no working order was already selling or"
            " buying; if those withdrawals succeed, part of the position is still held");
    }
    if (report.closing_unsizable_ != 0) {
      // **Names both inputs, because the three causes do not share one.** Two of them are the
      // holdings rows alone -- they will not add up, or the net has no representable closing
      // magnitude. The third subtracts what is already working on the closing side, and that
      // subtraction can fail with both inputs perfectly representable: a held quantity of
      // 1,000,000,000,000,000,000 and a working close of 0.1 cannot be aligned. An earlier note
      // blamed the holdings snapshot alone and would have sent an operator to check one of two
      // inputs. It still does not say "contended", which means a competing order reduced the close;
      // here nothing could be sized at all.
      report.notes_.push_back(
        "no closing order could be sized for " + std::to_string(report.closing_unsizable_) +
        " position(s): the holdings quantities would not add up or fit, or could not be combined"
        " with what is already working on the closing side; nothing was sent for them and they are"
        " still held");
    }
    if (report.closing_source_unverified_) {
      report.notes_.push_back(
        std::string("the stop could not verify what was outstanding: ") + unverified_source_reason() +
        "; orders or positions a previous process left may remain");
    }
    if (report.closing_unresolved_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_unresolved_) +
        " closing cancellation(s) named an order the portfolio does not hold; read the broker's"
        " order list");
    }
  }

  /**
   * Takes `gate`'s hold on the guard and releases the one this loop took before it.
   *
   * There is no "did I halt it" flag any more, and that is the point: the loop names the hold it
   * takes and releases only that one, so an operator halting by hand into a loop's closed gate is
   * no longer invisible, and the loop reopening its gate no longer lifts their halt with it. The
   * hold is released and retaken rather than diffed, because taking a hold is idempotent.
   *
   * Only the loop's *own* conditions are held. `OPERATOR` belongs to a person and `LOCAL_FAILURE`
   * to the runner; both outlive any gate the loop computes, so they are reported and never taken or
   * released here.
   */
  void hold(std::optional<strategy::HaltReason> gate) {
    if (gate && !owns(*gate)) {
      gate.reset();
    }
    if (held_ == gate) {
      return;
    }
    if (held_) {
      runner_->guard().resume(*held_);
    }
    if (gate) {
      runner_->guard().halt(*gate);
    }
    held_ = gate;
  }

  /**
   * Returns whether the loop knows what a closing pass at `at` would have to send.
   *
   * Two sources, both required. The broker's own order list must have been read once, which is what
   * the startup gate exists for and what a stop requested before the first iteration skips. And the
   * holdings snapshot -- which is where a `WITHDRAW`'s working orders and a `FLATTEN`'s positions
   * actually come from -- must exist and be within the configured age.
   */
  bool closing_source_verified(std::chrono::system_clock::time_point at) const {
    if (!order_list_read_) {
      return false;
    }
    // The working-order picture is its own source with its own freshness, and `as_of_` is not its
    // timestamp: `SessionSource::poll_holdings` refreshes positions and `as_of_` while *keeping* the
    // working orders it already had, and only a working-order poll replaces those. So a stop can
    // have a snapshot stamped a second ago whose resting orders were last read an hour ago.
    if (!working_orders_at_) {
      return false;
    }
    // And the decisive one, which no timestamp catches: an order this runner sent after that read
    // cannot be in it. A listing is out of date the moment the next placement is accepted, and an
    // indeterminate send counts too, because the order may exist.
    if (runner_->placements_at_broker() != placements_at_working_read_) {
      return false;
    }
    if (maximum_portfolio_age_ && at - *working_orders_at_ > *maximum_portfolio_age_) {
      return false;
    }
    const auto as_of = runner_->context().portfolio().as_of_;
    if (as_of == std::chrono::system_clock::time_point{}) {
      return false;
    }
    return !maximum_portfolio_age_ || at - as_of <= *maximum_portfolio_age_;
  }

  /**
   * Records that the working-order picture was just refreshed, with what it could not contain.
   *
   * Does nothing when the source does not publish working orders, so a stop on such a broker stays
   * unverified rather than being told a history read or a no-op poll refreshed a listing it never
   * built.
   */
  void note_working_orders_read(std::chrono::system_clock::time_point at) {
    if constexpr (source_provides_working_orders<Source>::value) {
      working_orders_at_ = at;
      placements_at_working_read_ = runner_->placements_at_broker();
    } else {
      (void)at;
    }
  }

  /** Returns which half of the closing source is missing, for the operator's note. */
  std::string unverified_source_reason() const {
    if constexpr (!source_provides_working_orders<Source>::value) {
      return "this source does not publish the broker's outstanding orders";
    }
    if (!order_list_read_ || !working_orders_at_) {
      return "the broker's order list has not been read";
    }
    if (runner_->placements_at_broker() != placements_at_working_read_) {
      return "an order was sent after the working orders were last read";
    }
    if (runner_->context().portfolio().as_of_ == std::chrono::system_clock::time_point{}) {
      return "no holdings snapshot has arrived";
    }
    if (maximum_portfolio_age_ &&
        clock_.now() - *working_orders_at_ > *maximum_portfolio_age_) {
      return "the working orders were read longer ago than the configured maximum";
    }
    return "the holdings snapshot is older than the configured maximum";
  }

  /** Returns whether `kind` is due at `at`. */
  bool is_due(PollKind kind, std::chrono::system_clock::time_point at) const {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return false;
    }
    const auto& due = next_due_[static_cast<int>(kind)];
    return !due || at >= *due;
  }

  /** Records when `kind` is next due. */
  void schedule_next(PollKind kind, std::chrono::system_clock::time_point at) {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return;
    }
    next_due_[static_cast<int>(kind)] =
      at + std::chrono::duration_cast<std::chrono::system_clock::duration>(*interval);
  }

  /**
   * Returns how long to wait before the next iteration.
   *
   * The soonest due poll, capped at the tick so a stop through the control file is noticed within
   * one tick however sparse the polling is. Never negative: a loop that has fallen behind catches up
   * rather than pausing once per overdue poll.
   */
  std::chrono::nanoseconds until_next(std::chrono::system_clock::time_point at) const {
    auto wait = schedule_.tick_;
    for (const auto kind : all_poll_kinds) {
      const auto& due = next_due_[static_cast<int>(kind)];
      if (!due) {
        continue;
      }
      const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(*due - at);
      if (remaining < wait) {
        wait = remaining;
      }
    }
    return wait > std::chrono::nanoseconds::zero() ? wait : std::chrono::nanoseconds::zero();
  }

  Runner* runner_;
  Source* source_;
  PollSchedule schedule_;
  Clock clock_;
  FileStopChannel control_;
  FailurePolicy failure_policy_;
  strategy::SessionWindow session_window_{strategy::SessionWindow::krx()};
  /**
   * The phase most recently announced, which is not the same as the phase in force.
   *
   * `UNKNOWN` to begin with rather than "nothing announced yet", because that is genuinely what the
   * loop knows before its first calendar arrives -- and it means a run that starts knowing nothing and
   * then learns the market is open announces that as a change, which is exactly right.
   */
  strategy::MarketPhase announced_{strategy::MarketPhase::UNKNOWN};
  /** Whether this loop has ever derived a phase that was not `UNKNOWN`, across every `run`. */
  bool phase_ever_known_{false};
  std::optional<std::chrono::nanoseconds> maximum_portfolio_age_;
  std::optional<std::chrono::nanoseconds> maximum_buying_power_age_;
  std::optional<std::chrono::system_clock::time_point> next_due_[poll_kind_count];
  std::size_t consecutive_failures_[poll_kind_count] = {};
  bool order_list_read_{false};
  /** Returns whether `reason` is a condition this loop computes, and may therefore hold. */
  static constexpr bool owns(strategy::HaltReason reason) noexcept {
    return reason != strategy::HaltReason::OPERATOR &&
      reason != strategy::HaltReason::LOCAL_FAILURE;
  }

  /** When the working-order picture was last refreshed, or nothing while it never has been. */
  std::optional<std::chrono::system_clock::time_point> working_orders_at_;
  /** `Runner::placements_at_broker()` as of that read, so a later send invalidates it. */
  std::size_t placements_at_working_read_{0};

  /** The hold this loop currently owns, and the only one it will ever release. */
  std::optional<strategy::HaltReason> held_;
  IterationReport last_;
};

} // namespace runner
} // namespace algotradelib
