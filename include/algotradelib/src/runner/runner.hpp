#pragma once

#include "./context.hpp"
#include "./dispatch.hpp"
#include "./execution.hpp"

#include "../strategy/reconcile.hpp"
#include "../strategy/risk.hpp"
#include "../strategy/sellable_convention.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

namespace runner_detail {

/**
 * Whether an executor can ask the broker what may be sold of one instrument.
 *
 * Detected rather than required, for the same reason the strategy's handlers are: a dry runner, a
 * replay's queueing executor and a test double have no broker to ask, and none of them should have
 * to grow a method that returns nothing. An executor bound to a broker whose holdings already carry
 * the figure declares it too and answers nothing, so the runner's own condition stays one test.
 */
template <typename Executor, typename = void>
struct fills_sellable : std::false_type {};

template <typename Executor>
struct fills_sellable<
  Executor,
  std::void_t<
    decltype(std::declval<Executor&>().sellable_quantity(std::declval<const InstrumentId&>()))
  >
> : std::true_type {};

/**
 * Whether `Executor` states what its sellable *inquiry* answer is evidence of.
 *
 * Detected rather than required, and the default is what matters: an executor that answers a
 * sellable quantity and says nothing about its basis is treated as having established nothing, so
 * the guard refuses rather than measuring a sell against a figure nobody characterized. A consumer's
 * own executor gets that default without being told it exists, which is the point -- the alternative
 * default would silently enforce against whatever its author happened to return.
 */
template <typename Executor, typename = void>
struct states_sellable_basis : std::false_type {};

template <typename Executor>
struct states_sellable_basis<
  Executor,
  std::void_t<decltype(Executor::sellable_inquiry_basis())>
> : std::true_type {};

/** Returns what `Executor`'s inquiry answer is evidence of, or an unestablished basis. */
template <typename Executor>
constexpr SellableBasis sellable_inquiry_basis_of() {
  if constexpr (states_sellable_basis<Executor>::value) {
    return Executor::sellable_inquiry_basis();
  } else {
    return SellableBasis{};
  }
}

/**
 * Whether `Executor` can say which reference the broker issued for the order it just accepted.
 *
 * Detected rather than required, for the same reason as above: a dry runner and a replay's
 * queueing executor have no broker to have been answered by. A reservation written without one is
 * still worth writing -- a limit that ignored the order would be the worse failure -- but it cannot
 * be cancelled by reference until a listing supplies one.
 */
template <typename Executor, typename = void>
struct reports_last_ref : std::false_type {};

template <typename Executor>
struct reports_last_ref<
  Executor, std::void_t<decltype(std::declval<const Executor&>().last_ref())>
> : std::true_type {};

/**
 * Whether `Executor` decides which venue an order that names none is sent to.
 *
 * Detected rather than required: only a broker binding routes, and a dry runner, an observer and a
 * replay's queueing executor send the planned venue as it stands. `live::SessionExecutor` answers
 * with its `default_route()`, and a `JournalingExecutor` forwards its inner executor's answer.
 */
template <typename Executor, typename = void>
struct resolves_route : std::false_type {};

template <typename Executor>
struct resolves_route<
  Executor, std::void_t<decltype(std::declval<const Executor&>().requested_route(Venue::UNSPECIFIED))>
> : std::true_type {};

/**
 * Whether `Executor` can say **why** the last instruction failed.
 *
 * Detected rather than required, on the same terms as the two above: a dry runner refuses nothing
 * and a replay's queueing executor has no broker to be refused by, so neither should have to grow a
 * method that answers nothing. An executor that binds a session declares
 * `const std::optional<BrokerError>& last_error() const` and its refusals then reach
 * `Runner::last_broker_refusal`, where a report can carry the broker's own code and message instead
 * of a count with nothing to chase.
 *
 * **The *shape* is detected, not the name.** An earlier version asked only whether `last_error()`
 * was a valid expression, which would have selected this branch for any executor that happened to
 * have a method of that name -- one returning `std::optional<std::string>`, say -- and then failed
 * to compile on `broker_code_`. A name is not a contract; the return type is.
 */
template <typename Executor, typename = void>
struct states_last_error : std::false_type {};

template <typename Executor>
struct states_last_error<
  Executor,
  std::enable_if_t<
    std::is_same_v<
      std::decay_t<decltype(std::declval<const Executor&>().last_error())>,
      std::optional<BrokerError>
    >
  >
> : std::true_type {};

} // namespace runner_detail

/**
 * Runs a strategy against events it is fed, and sends nothing.
 *
 * The whole pipeline a live session uses, minus the session: handlers fire, intents are queued,
 * reconciliation turns them into instructions, and the guards accept or refuse them. What would
 * have been sent is recorded instead.
 *
 * That makes it three things at once, and the third is the point:
 *
 * - a way to exercise a strategy with no credentials and no network;
 * - the reference implementation of the pipeline, which `LiveRunner` reuses rather than restates;
 * - the proof that a strategy is portable, since a strategy that runs here and live has used
 *   nothing broker-shaped.
 *
 * **No adapter code exists in the binary.** This header reaches no broker and no transport header,
 * so a dry-run program instantiates none of either, and an installed consumer's symbol table
 * confirms it: no `algotradelib::network`, `algotradelib::kis` or `algotradelib::tossinvest` symbol
 * appears at all. There is no instantiated code path from a dry run to a market, which is stronger
 * than a runtime flag that can be set wrong.
 *
 * It does not mean libssl is off the link line: the package exports one interface target carrying
 * the transport dependencies for every consumer. See `plan/todo/offline-only-target.md`.
 *
 * Events are fed in rather than polled, which is also how a backtest will drive it. A live runner
 * polls and calls the same `on_*` methods.
 */

/**
 * One cancellation a hold on the guard refused, and which hold refused it.
 *
 * The cancellation counterpart of `strategy::GuardRejection`, and separate from it because a
 * cancellation is not a `PlannedOrder`: there is no quantity or notional to report, only the order
 * being withdrawn and the reason nothing was sent.
 */
template <typename BrokerTag>
struct RefusedCancel {
  /** The order that was to be withdrawn. */
  OrderRef<BrokerTag> ref_;
  /** Which hold stopped it. Nothing was sent. */
  strategy::HaltReason reason_{strategy::HaltReason::OPERATOR};
};

/**
 * One cancellation that was refused, and by whom.
 *
 * Carried on the entry rather than left to the executor's `last_error()`, for the reason the
 * placement path gives: a pass sends several instructions, and a later accepted one clears that
 * error, so a report would outlive the only record of why this entry is here. The two refusals
 * differ in whether a request went out and was counted, which is the whole reason to tell them
 * apart.
 */
template <typename BrokerTag>
struct RejectedCancel {
  /** The order that was to be withdrawn. */
  OrderRef<BrokerTag> ref_;
  /** False when the binding refused to send it, so no broker was asked. */
  bool by_broker_{true};
};

/** What a dry run decided, in the order it decided. */
template <typename BrokerTag>
struct DryRunReport {
  /** Orders that passed reconciliation and every guard, and would have been sent. */
  std::vector<strategy::PlannedOrder> would_place_;
  /**
   * Working orders the broker **accepted a withdrawal request** for, before anything was placed.
   *
   * **`ACCEPTED` and nothing else.** A cancellation that a hold refused, that the broker rejected,
   * that failed in transit, that local machinery could not send, or that named an order the
   * snapshot does not hold is in its own bucket, so the size of this one is what the broker took.
   *
   * **Taking a request is not the same as the order stopping.** Cancellation is asynchronous at
   * both brokers: Toss publishes `PENDING_CANCEL` -- request received, broker not finished -- and
   * `CANCEL_REJECTED`, which returns the order to its previous state. So an entry here can still be
   * working, and only a fresh order list says otherwise. This bucket answers "did the instruction
   * go", never "is the order gone".
   */
  std::vector<OrderRef<BrokerTag>> would_cancel_;
  /**
   * Instructions that were not placed, each with its own reason.
   *
   * Three things land here and they share a consequence rather than a cause: reconciliation could
   * not build the instruction, the broker refused it (`BROKER_REFUSED`), or the binding refused to
   * send it (`BINDING_REFUSED` -- an idempotency key at a broker that accepts none, say). In every
   * case the order does not exist and nothing needs reconciling; only the broker's refusal was a
   * request the broker counted.
   *
   * **The reason is on the entry, not in `last_error()`.** A pass places several orders, and a
   * later accepted one clears the executor's last error, so a report would outlive the only record
   * of why an earlier entry is here.
   */
  std::vector<strategy::Rejection> rejected_;
  /** Instructions a guard refused, with the reason -- a limit or a hold -- and what was attempted. */
  std::vector<strategy::GuardRejection> refused_;
  /**
   * Orders the runner declined to send because the instrument's state is unresolved.
   *
   * Reported rather than retried. A retry is exactly what must not happen after a transport
   * failure, so these are surfaced for an operator and left alone.
   */
  std::vector<WithheldOrder> withheld_;
  /**
   * Whole reconciliation plans held back before any part of them was sent.
   *
   * Distinct from `withheld_`, which is about an order. A plan's cancellations go out before its
   * placements are guard-checked, so a plan that cannot be acted on has to be stopped as a unit or
   * not at all. **Three reasons put one here** and `WithheldPlan::reason_` says which: an account
   * never read, one older than the stated maximum, or a placement hold in force for a plan that also
   * wanted to withdraw something -- and that last one on a portfolio that is perfectly current.
   */
  std::vector<WithheldPlan<BrokerTag>> withheld_plans_;
  /** Instructions whose send failed in transit, leaving their outcome unknown. */
  std::vector<strategy::PlannedOrder> indeterminate_;
  /**
   * Cancellations that were refused, each saying whether the broker or the binding refused it.
   *
   * A refusal is an answer about the *request*: it was not accepted, so nothing needs reconciling.
   * It says nothing about the order, which may still be working or may have filled a moment
   * earlier -- a fill is one of the ordinary reasons a broker refuses a withdrawal. Either way the
   * request did not go, which is why this is not `would_cancel_`. The placement path carries the
   * same distinction on `rejected_`.
   */
  std::vector<RejectedCancel<BrokerTag>> rejected_cancel_;
  /**
   * Cancellations whose send failed in transit, leaving their outcome unknown.
   *
   * Separate from `would_cancel_`, and the separation is the point: the order may now be withdrawn
   * or may still be resting, and reporting it as a withdrawal that happened would tell an operator
   * the one thing this library will not say about an unknown outcome. The instrument is marked in
   * the ledger for the same reason.
   */
  std::vector<OrderRef<BrokerTag>> indeterminate_cancel_;
  /**
   * Cancellations naming an order the portfolio snapshot does not hold.
   *
   * Reported rather than sent blind. The snapshot is stale, and the order may already be gone; a
   * cancellation the runner cannot describe is one it should not invent.
   */
  std::vector<OrderRef<BrokerTag>> unresolved_cancel_;
  /**
   * Instructions local machinery refused to send, with the reason.
   *
   * Never a broker's answer. **Local machinery was unavailable**, so nothing was attempted -- a
   * pre-send journal write that failed is the usual cause and not the only one, since any executor
   * answering `SendOutcome::UNAVAILABLE` lands here.
   * Reported separately from `rejected_` because the two ask for opposite responses: a rejection is
   * about one instruction, this is about whether the runner should still be trading at all.
   */
  std::vector<WithheldOrder> unavailable_;
  /** Cancellations local machinery refused to send. Distinct from one the snapshot cannot describe. */
  std::vector<OrderRef<BrokerTag>> unavailable_cancel_;
  /**
   * Cancellations a hold on the guard refused, with the hold that refused them.
   *
   * The counterpart of `refused_` on the cancellation path. A halt stops this runner *sending*, and
   * a cancellation is a send: it is an order-writing request that can itself fail indeterminately
   * and wedge the instrument. So a halted runner withdraws nothing, and says so rather than letting
   * a `WITHDRAW` through a kill switch somebody had already pulled.
   */
  std::vector<RefusedCancel<BrokerTag>> refused_cancel_;
  /** Lines the strategy logged. */
  std::vector<std::string> log_;

  /** Returns whether the run decided to do nothing at all. */
  bool quiet() const noexcept {
    // `withheld_plans_` is in this list for the reason the list exists at all: a run that withheld a
    // cancel-only target did *not* decide to do nothing, and a `quiet()` that said so would hide the
    // one result this branch added. A bucket left out of here is a result that reports as silence.
    return would_place_.empty() && would_cancel_.empty() && rejected_.empty() &&
      refused_.empty() && withheld_.empty() && withheld_plans_.empty() && indeterminate_.empty() &&
      unresolved_cancel_.empty() && unavailable_.empty() && unavailable_cancel_.empty() &&
      refused_cancel_.empty() && indeterminate_cancel_.empty() && rejected_cancel_.empty();
  }
};

/**
 * Drives one strategy with no broker attached.
 *
 * Holds the strategy by value so a caller can read its state afterwards — a warm-up count, a
 * position it tracked — which is most of what a strategy test wants to assert.
 */
/** The broker tag a runner with no broker stands in for. References are never sent anywhere. */
struct DryTag {
  /** The tag's normalized name, which is what an order reference prints. */
  static constexpr std::string_view name = "dryrun";
};

/**
 * Drives one strategy through the whole pipeline, sending through `Executor`.
 *
 * One implementation, parameterized on where instructions go. A dry run supplies an executor that
 * records and accepts; a live session supplies one that calls the broker; a test supplies one that
 * fails on purpose. That is what makes the transport-indeterminacy behaviour -- the most important
 * in this branch and the least reachable through a real broker -- exercisable without one.
 *
 * **Everything it owns is held by value; everything it borrows is held by pointer.** That split is
 * worth stating, because a runner looks like the sort of object one would reach for a smart pointer
 * with, and it is not:
 *
 * - The **strategy** is a template parameter, resolved at compile time. There is no base class to
 *   point at and no dynamic dispatch to enable, so an indirection would buy nothing and cost a
 *   dereference in a handler that is otherwise inlined. Holding it by value is also what lets a
 *   caller read the state it kept -- `dry.strategy().bars_seen_` -- which is most of what a strategy
 *   test wants to assert.
 * - The **context, guard, ledger and executor** are parts of the runner, not things shared with it.
 *   A runner is constructed once per program or per test, never per bar, so a heap allocation here
 *   would be a cost with no corresponding benefit.
 * - A **session** is borrowed, and `SessionExecutor` holds it by pointer for exactly that reason: it
 *   owns a connection with executor affinity, and a runner uses it without owning it. The client
 *   underneath is a `shared_ptr` because the transport layer genuinely shares it.
 *
 * So the rule is ownership, not size: by value when the runner's lifetime governs it and the type is
 * known at compile time, by pointer when something else owns it.
 */
template <
  typename Strategy,
  typename CapsType = strategy::NoCaps,
  typename BrokerTag = DryTag,
  typename Executor = RecordingExecutor<DryTag>
>
class Runner {
public:
  /** The tag identifying the broker this runner sends to, or `DryTag` when it sends nowhere. */
  using Tag = BrokerTag;
  /** The context handed to every strategy handler. */
  using Context = runner::Context<BrokerTag, CapsType>;
  /** What one `feed` returns: everything the pass decided, in the order it decided it. */
  using Report = DryRunReport<BrokerTag>;

  ATL_ASSERT_STRATEGY(Strategy, Context);

  /**
   * Whether this runner sends to a broker and its strategy defines `on_fill` -- which then never fires.
   *
   * A replay synthesizes fills and calls `on_fill`; a live session reports none as events, so the same
   * handler is correct in a backtest and inert with real money. That is the one difference between
   * the two modes a correct strategy can lose money over (`issues/live-parity-audit.md`, finding 1).
   * It is legitimate to use `on_fill` for backtest-only bookkeeping, so this refuses nothing: it lets
   * `PollingLoop` put a note in every run's report, and lets a program that drives a runner by hand
   * say the same thing itself.
   *
   * `false` for a dry run, for `live::bind_observer` and for a replay, whose executors state that they
   * reach no broker; `JournalingExecutor` forwards what it wraps.
   *
   * "Never" describes the shipped live path: nothing this library polls produces a `Fill`. A program
   * that calls `feed(const strategy::Fill&)` itself, from a source of its own, does reach `on_fill`,
   * and can ignore the note.
   */
  static constexpr bool on_fill_never_fires =
    runner_detail::sends_to_a_broker<Executor>::value &&
    strategy::strategy_detail::has_on_fill<Strategy, Context>::value;

  /**
   * Whether this runner keeps every order it sends in `working_`, whatever the limits, until the
   * broker shows what became of it (`persist_reservation`, `unconfirmed_orders`).
   *
   * True for a runner that sends to a broker publishing a working-order inquiry -- every shipped
   * binding (`live::SessionExecutor`, and `JournalingExecutor` over it) on KIS, Toss and Kiwoom.
   * False for a dry run, `live::bind_observer` and a replay, which send nowhere. `PollingLoop` reads
   * it: a runner that keeps its orders until the broker accounts for them is held while no listing will
   * (see `PollSchedule::working_orders_`).
   */
  static constexpr bool keeps_sent_orders_until_listed =
    runner_detail::sends_to_a_broker<Executor>::value && CapsType::has_working_orders;

  /** Constructs a runner with the given strategy, limits, and series warm-up window. */
  explicit Runner(
    Strategy strategy = {},
    strategy::RiskLimits limits = {},
    std::size_t series_capacity = 512,
    Executor executor = {}
  )
  : strategy_(std::move(strategy)),
    guard_(std::move(limits)),
    context_(series_capacity),
    executor_(std::move(executor)) {}

  /** Returns the executor, so a caller can read what it was handed. */
  const Executor& executor() const noexcept { return executor_; }
  /** @copydoc Runner::executor() const */
  Executor& executor() noexcept { return executor_; }

  /**
   * Returns the ledger of instruments whose order state is unresolved.
   *
   * Non-const so an operator, having read the broker's order list, can clear an entry. Nothing
   * else clears one.
   */
  const IndeterminacyLedger& ledger() const noexcept { return ledger_; }
  /** @copydoc Runner::ledger() const */
  IndeterminacyLedger& ledger() noexcept { return ledger_; }

  /**
   * Returns why local machinery can no longer send, or nothing while it can.
   *
   * **Sticky by policy, not by prediction.** Whatever failed may well fail on the next order too --
   * but it may equally have recovered, and this hold stays either way. Clearing it on the next quiet
   * bar would turn an outage into a trickle of unexplained refusals, and would decide on its own
   * that a transient failure was transient; leaving it set makes resuming a deliberate act.
   *
   * **It is a hold on the guard**, `strategy::HaltReason::LOCAL_FAILURE`, not a flag beside it.
   * That is the difference between a condition every send path has to remember to check and one
   * checked by the single function every order already passes through. It also means an operator
   * asking the guard why it will not trade is told a local-failure detail -- what the binding said it
   * could not do, or whatever a caller of `halt_locally` supplied, which `live::SessionSource` does
   * when it cannot journal a reconciliation decision. A failed journal write is the usual answer and
   * not the only one. Better than being told only that trading is halted.
   *
   * **Recorded through this runner it is the *first* detail**, kept while the hold stands, so a
   * cascade of later failures cannot bury the one that started it. That promise covers
   * `note_local_failure` and `halt_locally` and stops there: `guard()` hands out a mutable guard, and
   * `RiskGuard::halt(LOCAL_FAILURE, detail)` replaces the detail on a hold already standing. A
   * program that takes this hold itself owns what it reads back.
   *
   * Cleared only by `resume_after_local_failure`, which a program calls when it has fixed whatever
   * was broken.
   */
  std::optional<std::string> local_failure() const {
    if (!guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return std::nullopt;
    }
    return guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE);
  }

  /**
   * Clears the local failure, after an operator has fixed what caused it.
   *
   * **Acknowledges the executor's failure count as well as releasing the hold**, and without that
   * second half the call does nothing useful. `JournalingExecutor::last_journal_error()` keeps its
   * first reason forever -- correct for diagnosis -- so a runner that only released the hold would
   * read that stale reason after the very next send and halt again immediately. Recording the count
   * already seen is what lets the runner tell a *new* failure from the one that has been dealt
   * with.
   *
   * Releases only `HaltReason::LOCAL_FAILURE`. An operator who also halted by hand stays halted,
   * which is the whole point of a hold naming its holder.
   */
  void resume_after_local_failure() {
    guard_.resume(strategy::HaltReason::LOCAL_FAILURE);
    acknowledged_journal_failures_ = executor_journal_failures();
  }

  /**
   * States how old a holdings snapshot may be before a plan derived from it is not acted on.
   *
   * The **policy**, not a verdict: this runner evaluates it against the snapshot in hand every time
   * it settles, so the answer cannot describe a portfolio that has since been replaced. A
   * `runner::PollingLoop` pushes its own configured age down here; a consumer driving a runner
   * directly sets it themselves, and leaving it unset means only that age is not a reason -- a
   * snapshot that was never read, and a hold on placements, still are.
   *
   * It does not hold, refuse or halt anything on its own. `PollingLoop` owns the *hold*; this owns
   * only whether a reconciliation plan may be acted on.
   */
  void set_maximum_portfolio_age(std::optional<std::chrono::nanoseconds> age) {
    maximum_portfolio_age_ = age;
  }

  /**
   * Records the wall-clock instant a driving loop believes it is, for measuring a snapshot's age.
   *
   * **Not the same question as `Context::now()`, and using that one understates age.** `now_` is the
   * *decision* instant, and `feed(BarEvent)` sets it to the bar's period end -- which on a daily bar
   * is yesterday's close. A bar that arrives late would then measure a snapshot taken at the same
   * moment as zero seconds old and act on it, however long ago that actually was.
   *
   * Set by `runner::PollingLoop` at the top of each iteration, before anything it calls can feed, so
   * a plan settled from inside a source callback is measured against the clock rather than against
   * the event that triggered it. A runner with no loop leaves it unset and falls back to `now()`.
   */
  void set_observed_at(std::chrono::system_clock::time_point at) { observed_at_ = at; }

  /**
   * Returns the last instant a driving loop reported through `set_observed_at`, or the epoch when
   * none has.
   *
   * `live::SessionSource` reads it to hold back a bar whose period has not ended yet: a broker's
   * daily query includes the session still trading, and that bar is not one a strategy may be shown.
   */
  std::chrono::system_clock::time_point observed_at() const noexcept { return observed_at_; }

  /**
   * States that this runner may act although it has never read what the account holds.
   *
   * The runner's half of `PollingLoop::allow_absent_portfolio`, which pushes it down. It exists here
   * too because the loop's hold is not installed for the whole of an iteration -- a source callback
   * can settle before it -- so the absence rule has to be enforced where the instruction is decided,
   * and an opt-out that reached only the hold would be an opt-out with no effect on the rule that
   * matters.
   */
  void allow_absent_portfolio() { allow_absent_portfolio_ = true; }

  /**
   * States whether this runner may **place** while it has never read what the account holds.
   *
   * Set by `runner::PollingLoop` at the top of every iteration, before anything it calls can feed,
   * and defaulted from the executor: **on** for one that can reach a broker, off for a dry run and a
   * replay, which have no holdings inquiry that could have failed and, in the replay's case, stamp
   * their own portfolio. `runner_detail::sends_to_a_broker` holds that decision and the argument for
   * which way an unmarked executor should fail.
   *
   * It exists because the loop's own hold is not in force at every point an instruction can be
   * settled -- a source's `read_order_list` runs before the first hold is installed and is handed a
   * mutable runner. A rule enforced only by that hold is a rule with a hole in it on the first
   * iteration, which is exactly the iteration a run has read nothing.
   *
   * Withdrawing is unaffected, and so is a plan that would cancel something: that one is withheld on
   * the snapshot alone, whoever is driving, because a cancellation derived from an account nobody
   * has read can withdraw the order protecting a real position.
   */
  void require_known_portfolio(bool required) { require_known_portfolio_ = required; }

  /**
   * Stops this runner sending, for a reason that has nothing to do with any one instruction.
   *
   * Public because the conditions that warrant it are not all inside the runner. Recovery is the
   * motivating one: a journal whose damaged tail named no instrument says something was in flight
   * and cannot say what, so there is nothing to withhold and the only safe response is to withhold
   * everything. Sticky, like a failure the executor raises, and cleared the same way -- it takes
   * the same `HaltReason::LOCAL_FAILURE` hold, so it stops sending on its own rather than only
   * when a `PollingLoop` notices.
   */
  void halt_locally(std::string detail) { note_local_failure(std::move(detail)); }

  /**
   * Returns how many placements this runner has sent that may have left an order at the broker.
   *
   * Monotonic, and counting acceptances *and* indeterminate sends, because both can leave something
   * resting. Its only purpose is comparison: a caller that read a working-order listing, recorded
   * this number, and then sees it grow knows its listing is out of date without having to ask the
   * broker. `PollingLoop` uses exactly that to decide whether a stop knows what it owes.
   *
   * A rejection does not count -- the broker said the order does not exist -- and neither does a
   * local failure, which never reached a broker at all.
   */
  std::size_t placements_at_broker() const noexcept { return placements_at_broker_; }

  /**
   * Returns how many orders this runner has sent under a phase the operator did not permit.
   *
   * Monotonic, so a caller differences it around whatever span it cares about, the way
   * `PollingLoop` does with `RiskGuard::total_sent`. It is not a per-pass figure and deliberately
   * not one: an order can leave the process from inside a source's poll, whose report nobody keeps,
   * so a count that lived only on a `DryRunReport` would miss every ordinary send and report only
   * what a stop did.
   *
   * **What it counts is a send, not a decision.** `RiskLimits::permitted_phases_` permits an order
   * that closes a held position whatever the phase, which is what lets an operator's flatten go out
   * after the close; this says how often that happened. An order the exception admitted and a later
   * limit then refused is not counted, because nothing was sent. A broker's rejection *is* counted,
   * because the request went out and the rejection is the broker's answer to it -- and outside a
   * session that is the likeliest answer, so a counter excluding it would read zero in the case it
   * exists for.
   *
   * A non-zero value is not by itself a fault. It is the library doing what the limit now says, and
   * the number an operator needs to tell a nightly flatten from a strategy restating a close on
   * every pass -- for which `RiskLimits::max_orders_per_window_` is the lever.
   */
  std::size_t sends_outside_permitted_phase() const noexcept {
    return sends_outside_permitted_phase_;
  }

  /**
   * Starts inferring whether the broker's sellable figure already excludes resting sells.
   *
   * Off by default, because it costs a scan of the positions on every pass and answers a question
   * most callers do not have. Neither broker documents the convention, and `RiskGuard` subtracts
   * resting sells because the safe direction is to over-count; a caller who wants to know whether
   * that deduction is charging twice turns this on and reads `sellable_convention()` after a few
   * sessions of ordinary trading.
   *
   * It observes rather than experiments: nothing extra is sent, nothing extra is requested, and a
   * run with the probe on places exactly the orders it would have placed with it off.
   */
  void probe_sellable_convention(bool enabled = true) { probing_sellable_ = enabled; }

  /**
   * Records one observation of the current portfolio for the convention probe.
   *
   * Called by `settle` on every pass, and by a `PollingLoop` after it polls -- because a poll that
   * refreshes the portfolio without feeding the runner is exactly the moment a sell of ours starts
   * appearing in the listing, and a runner that only observed when fed would miss it.
   *
   * Observing the same state twice teaches nothing by construction: the probe concludes only from a
   * change in the resting quantity, so a repeat is discarded rather than counted twice.
   */
  void observe_sellable_convention() {
    // Skipped, not forgotten, while the listing is newer than the holdings: the resting quantity
    // and the sellable figure would come from two moments, and a sell starting to rest between them
    // reads as a figure that did not move. The baseline stays the last paired reading.
    if (probing_sellable_ && !context_.portfolio().listing_newer_than_holdings()) {
      sellable_probe_.observe(context_.portfolio());
    }
  }

  /** Returns what the probe has inferred, and the counts behind it. */
  const strategy::SellableConventionProbe<Tag>& sellable_convention() const noexcept {
    return sellable_probe_;
  }

  /** Returns the strategy, so a test can read the state it kept. */
  const Strategy& strategy() const noexcept { return strategy_; }
  /** @copydoc Runner::strategy() const */
  Strategy& strategy() noexcept { return strategy_; }

  /** Returns the context, so a caller can arrange the world before feeding an event. */
  const Context& context() const noexcept { return context_; }
  /** @copydoc Runner::context() const */
  Context& context() noexcept { return context_; }

  /** Returns the guard, so a caller can halt trading or read the limits. */
  const strategy::RiskGuard& guard() const noexcept { return guard_; }
  /** @copydoc Runner::guard() const */
  strategy::RiskGuard& guard() noexcept { return guard_; }

  /**
   * Records today's permitted price band for one instrument, as a live runner would have read it.
   *
   * Absent for an instrument means the broker published no band, which reconciliation treats as no
   * clamping rather than as a band of zero width.
   *
   * **Dated on arrival, because a band is a per-trading-day value.** `PriceLimits` is "the highest
   * price allowed today" and for KRX it moves with the previous close, so one registered at startup
   * describes the day it was registered on. The date recorded is the **exchange's own stamp where
   * the broker published one** -- Toss's `timestamp` and Kiwoom's `date`/`tm` both reach
   * `PriceLimits::exchange_time_`, capped so it can only age a band -- and the last instant a
   * driving loop reported, where the broker published none, which is every KIS band. The difference is not cosmetic and `set_maximum_price_limits_age` states it: a
   * stamped band can be asked how old the *exchange's statement* is, an unstamped one only how long
   * ago it was fetched, and a successful fetch of a week-old band would otherwise read as brand new.
   *
   * Keyed on the whole identity. Country and venue participate in `InstrumentId` equality, so a map
   * keyed on the symbol alone lets a KRX band and an NXT band for the same stock overwrite each
   * other -- which nothing reached while nothing in the library filled this map, and
   * `PollKind::PRICE_LIMITS` sweeping a universe does.
   *
   * **The receipt clock is `set_observed_at`'s instant, and deliberately not `freshness_instant()`.**
   * That maximum includes the context's own decision instant, which is a bar's *period end* -- and a
   * broker that publishes a forming bar puts that instant in the future. A band stamped from it would
   * be dated ahead of the clock and stay fresh past the stated maximum in wall time, which is the one
   * direction a staleness rule must not fail in. Stamping from the reported instant while **ageing**
   * against `freshness_instant()` leaves the comparison never earlier than the stamp, so a band can
   * read older than it is and never younger.
   *
   * **A band registered before this runner has been told any instant is stamped at the epoch**, and
   * is then older than any maximum an operator would state. That is a caller driving a runner
   * directly: `PollingLoop` calls `set_observed_at` before anything can feed, so a polled band
   * carries the iteration that read it. A direct caller states the instant itself, accepts any age,
   * or gets a band that is held back -- the fail-closed direction, and visible in
   * `price_bands_withheld`. The library does **not** read the wall clock here to paper over it: no
   * decision path in this runner reads one, and introducing one would make a replay's answer depend
   * on when the replay was run.
   */
  void set_price_limits(const InstrumentId& id, PriceLimits limits) {
    DatedBand dated;
    dated.received_at_ = observed_at_;
    dated.limits_ = std::move(limits);
    price_limits_.insert_or_assign(id, std::move(dated));
  }

  /**
   * States how old a price band may be before it is **not applied** to an order's price.
   *
   * The policy, not a verdict, on the same terms as `set_maximum_portfolio_age`: it is evaluated
   * against whatever band is in hand each time a plan is built.
   *
   * **A band nobody has aged is not applied either**, which is the one behavioural change this
   * carries for a caller who already registers bands. Leaving the policy unset used to mean "apply
   * this band forever"; it now means the same as an expired one. State `allow_unbounded_price_limits_age`
   * to keep the old behaviour deliberately.
   *
   * **What withholding costs, stated without flattery.** It returns the order to the library's
   * ordinary state -- no band, no clamp, the strategy's own price on the wire -- which is what every
   * run that registers no band already does. That is not free, and an earlier draft of this comment
   * claimed it was:
   *
   * - **A fill can be lost.** Where the stated price lies outside *today's real* band, the exchange
   *   refuses the order, while the clamped price would have been accepted and could have traded.
   *   `clamp_to_limits` says so itself: it turns an order the exchange would certainly reject into
   *   one it will accept. A reducing sell is the case that stings -- an exit priced below today's
   *   floor is refused instead of being lifted onto it.
   * - **Something local can refuse it instead, and then no exchange ever sees it.** `RiskGuard`
   *   values an order at `quantity * price_`, so a **buy** left above a withheld ceiling is a larger
   *   notional than the clamped one: the per-order ceiling, buying power and the gross exposure
   *   ceiling each see a bigger number and one of them may refuse an order the band would have
   *   admitted. (A sell left below a withheld floor is the smaller notional, so those limits are not
   *   what stops it.) And the binding itself refuses a price no adapter will send -- a sell at zero
   *   passes `Price::checked`, because zero sits on a stated grid, and every shipped adapter rejects
   *   it before any request goes out.
   *
   * **Both of those are refusals, and both are counted** -- `unclamped_orders_rejected` for the one
   * a broker made and `unclamped_orders_refused` for the ones nothing sent, which is the same
   * division `LoopReport` already makes between a closing instruction the broker rejected and one a
   * guard refused. The case with **no refusal in it** is counted by neither and cannot be: an order
   * accepted at the raw price that then simply never filled leaves nothing to observe.
   *
   * **Why it is still the default.** A band whose age nobody has bounded is not evidence about
   * today's band, and this library does not alter an order's price on the strength of something that
   * is not evidence. The alternative is not "clamp correctly" -- it is "clamp to a number from an
   * unknown day", whose failure is an order resting at a price nobody chose, indistinguishable from
   * a working order and possibly never filling. Between a stated refusal and a silent misprice this
   * library takes the stated one, and the operator overrides it in one line.
   *
   * @param age how old a band may be. `std::nullopt` withdraws a stated maximum and returns this
   *   runner to having stated no policy at all, which withholds every band -- it does not mean
   *   "any age", which is what `allow_unbounded_price_limits_age` is for.
   */
  void set_maximum_price_limits_age(std::optional<std::chrono::nanoseconds> age) {
    maximum_price_limits_age_ = age;
    // **Cleared whether or not an age was given**, because this call *is* the policy now. Clearing
    // it only for a stated age left `set_maximum_price_limits_age(std::nullopt)` after
    // `allow_unbounded_price_limits_age()` still applying a band of any age -- the documented
    // withdrawal doing nothing, under the one call an operator would reach for to stop it.
    allow_unbounded_price_limits_age_ = false;
  }

  /**
   * States that this runner applies a price band of any age, in place of stating a maximum.
   *
   * The deliberate half of the requirement above, and the pre-2026 behaviour named rather than
   * defaulted into. It does not make a stale band safe; it records that the caller considered it.
   */
  void allow_unbounded_price_limits_age() {
    allow_unbounded_price_limits_age_ = true;
    maximum_price_limits_age_.reset();
  }

  /**
   * Returns how many plans were built while a band was held for the instrument and not applied.
   *
   * **Plan evaluations, not orders**: a plan that produced a market order, or no order at all, is
   * counted the same as one that produced a limit. Monotone, so a driving loop differences it.
   * This figure is about the *policy*; what it cost is `unclamped_orders_rejected` and
   * `unclamped_orders_refused`, and neither of those sees an order accepted at the raw price that
   * then simply never filled.
   */
  std::size_t price_bands_withheld() const noexcept { return price_bands_withheld_; }

  /**
   * Returns whether a band-age policy has been stated at all, either way.
   *
   * Published so a driving loop's diagnostics can describe **this runner's** policy rather than
   * remembering which of its own wrappers were called. A loop that kept its own flag could disagree
   * with the runner in both directions -- a maximum set directly on the runner before the loop was
   * built read as "no policy stated", and a withdrawal made directly on the runner after the loop's
   * `allow_unbounded_price_limits_age` suppressed the note that withdrawal is supposed to produce.
   * There is one policy; there should be one place that answers for it.
   *
   * **Both halves are load-bearing as an answer, and only one of them is reachable through a loop
   * today.** A run that accepts any age withholds no band for age, so the note that reads this
   * cannot currently fire while `allow_unbounded_price_limits_age` is in force -- the one state that
   * would combine them, a band dated after the instant it is aged at, needs a caller moving the
   * reported clock backwards, which `PollingLoop` does not do. The term stays because this answers
   * "has a policy been stated", not "is the note due", and an accessor that answered its own
   * question wrongly would be waiting for the first caller who asked it directly.
   */
  bool price_limits_age_policy_stated() const noexcept {
    return maximum_price_limits_age_.has_value() || allow_unbounded_price_limits_age_;
  }

  /**
   * Returns how many **sent** orders were refused by the broker after a withheld band would have
   * moved their price.
   *
   * The half where a **broker was asked**, counted where the outcome is known rather than where the
   * decision was made -- the division `sends_outside_permitted_phase` already makes. Everything that
   * never reached a broker is `unclamped_orders_refused`; an order the broker accepted is in
   * neither.
   *
   * It is **co-occurrence, not causation**, and no arithmetic here can make it otherwise: a broker
   * refuses orders for reasons that have nothing to do with a price band, and a count of one is one
   * order that happened to be carrying an unclamped price when it was refused. Read a count that
   * tracks a run's rejections as a reason to look at the band policy, not as proof about any single
   * order. Zero does **not** mean withholding cost nothing: read it beside
   * `unclamped_orders_refused`, and note that **neither counter can see the case with no refusal at
   * all** -- an order accepted at the raw price that then simply never filled.
   */
  std::size_t unclamped_orders_rejected() const noexcept { return unclamped_orders_rejected_; }

  /**
   * Returns how many orders nothing sent to a broker while carrying a withheld band's unclamped
   * price.
   *
   * Everything that **never reached a broker** while carrying that price, which is two sites and not
   * one:
   *
   * - a **guard**: `RiskGuard` values an order at `quantity * price_`, so a buy left above a
   *   withheld ceiling is the larger notional and the per-order ceiling, the buying-power check or
   *   the gross exposure ceiling can refuse what the band would have admitted;
   * - the **binding**: a limit price no adapter will send, which review found a real instance of --
   *   a sell at zero, which `Price::checked` admits because zero sits on a stated grid, and which
   *   a withheld floor would have lifted into range.
   *
   * `unclamped_orders_rejected` stays at zero through every one of them, which is why this exists
   * separately rather than being folded in. Co-occurrence on the same terms as its sibling: a guard
   * and a binding each refuse orders for their own reasons, and this counts the ones that happened
   * to be carrying an unclamped price. `UNAVAILABLE` is **not** counted -- that is this process's
   * local machinery being unavailable -- usually a pre-send journal write that failed, and not only
   * that -- which no price affects.
   */
  std::size_t unclamped_orders_refused() const noexcept { return unclamped_orders_refused_; }

  /**
   * How many instructions failed to reach a broker, by **origin**, and how many reached one and were
   * refused.
   *
   * **All monotone, so a driving loop differences them**, exactly as it does
   * `sends_outside_permitted_phase`. They exist because a live run threw every one of these away: a
   * `Runner::Report` carries fourteen vectors describing what became of each instruction, and
   * `live::SessionSource` discards the report on every poll -- so a run in which the broker refused
   * everything looked very much like one in which it accepted everything. Not identical: the phase
   * and price-band figures can differ for some refused orders. But nothing said *the broker refused
   * these*.
   *
   * **Six origins for placements**, not one figure, because an operator's next move differs for each
   * and the library already distinguishes them:
   *
   * - `orders_unbuildable` -- **every** reconciliation rejection. Missing reference data, no price,
   *   an unknown grid -- and a quantity that rounded to zero, which is an instruction deliberately
   *   dropped rather than anything being wrong. So a non-zero figure is not by itself a reason to go
   *   looking for missing data; `Report::rejected_` carries the reason per instruction.
   * - `orders_reconcile_withheld` -- the indeterminacy ledger holds this instrument until a listing
   *   settles an unknown outcome. Nothing was wrong with the instruction.
   * - `orders_guard_refused` -- `RiskGuard` refused it. **A hold as well as a limit**: it checks the
   *   active holds first, so a halted run reports every order here, not only one that breached a
   *   number. `Report::refused_` carries the reason per order, which is what a diagnosis needs.
   * - `orders_binding_refused` -- the binding would not send it. No request went out and no broker
   *   counted one.
   * - `orders_broker_refused` -- the broker was asked and said no. A request *was* spent, and
   *   `last_broker_refusal` carries what it said.
   * - `orders_indeterminate` -- the request failed in transit, so the order may or may not exist.
   */
  std::size_t orders_unbuildable() const noexcept { return orders_unbuildable_; }
  /** @copydoc Runner::orders_unbuildable */
  std::size_t orders_reconcile_withheld() const noexcept { return orders_reconcile_withheld_; }
  /** @copydoc Runner::orders_unbuildable */
  std::size_t orders_guard_refused() const noexcept { return orders_guard_refused_; }
  /** @copydoc Runner::orders_unbuildable */
  std::size_t orders_binding_refused() const noexcept { return orders_binding_refused_; }
  /** @copydoc Runner::orders_unbuildable */
  std::size_t orders_broker_refused() const noexcept { return orders_broker_refused_; }
  /** @copydoc Runner::orders_unbuildable */
  std::size_t orders_indeterminate() const noexcept { return orders_indeterminate_; }

  /**
   * The two failures that are not about one instruction's own merits.
   *
   * - `plans_withheld` -- a whole reconciliation plan held back before any of its parts was judged.
   *   Counted apart from the six origins because nothing was wrong with the instructions themselves.
   *   **Four reasons reach it and this figure does not tell them apart**: the portfolio was never
   *   read (`PORTFOLIO_ABSENT`), it is older than the stated maximum (`PORTFOLIO_STALE`), the
   *   working-order listing arrived after it (`PORTFOLIO_SUPERSEDED`), or a hold is refusing
   *   placements while the plan also wanted to withdraw something (`PLACEMENT_HELD`) -- the last two
   *   on a portfolio that is perfectly fresh, so a reader who assumed this figure meant "the account
   *   is unknown" would chase the wrong thing. `Report::withheld_plans_` carries the
   *   reason per plan; this counts how often it happened.
   *
   *   **The loop's own gate does not always describe it**, which is why it is a figure at all: with
   *   `allow_absent_portfolio` the gate is open while this still fires for a plan that would withdraw
   *   something.
   * - `instructions_unavailable` -- **local machinery was unavailable**, so the instruction was not
   *   sent. A pre-send journal write that failed is the usual cause and not the only one: any
   *   executor answering `SendOutcome::UNAVAILABLE` lands here, so a binding with no journal at all
   *   can produce this. Placements and withdrawals together, because the operator's response is about
   *   the process rather than the order. It leaves a sticky `LOCAL_FAILURE` hold, which the loop's
   *   gate *may* report -- a higher-priority hold can be named instead while that one is still in
   *   force -- so this figure is how many instructions it cost rather than evidence about the gate.
   */
  std::size_t plans_withheld() const noexcept { return plans_withheld_; }

  /**
   * Targets held back while a stop on their instrument is withdrawn, at most one per instrument.
   *
   * **Why the runner keeps them rather than the strategy.** A target that needs a trade while a stop
   * rests withdraws the stop and sends nothing (`strategy::RejectReason::STOP_ORDER_RESTING`), because
   * a sell beside a stop that may still release can sell twice. What ends the wait is the stop's own
   * outcome -- and a stop leaving the working-order listing feeds no strategy callback at all
   * (`OrderUpdateWatch` reports what a listing says, never what it stopped saying). A flatten stated
   * once from `on_phase` would therefore never be stated again. So the runner keeps it:
   *
   * - **retained** when a `TargetPosition`'s plan passed the withheld-plan gate and came back refused
   *   for `STOP_ORDER_RESTING`, together with the references of the stops it waits on. A plan withheld
   *   whole retains nothing -- its withdrawal never went out;
   * - **replaced** by any `TargetPosition` or `OrderIntent` the strategy states for the instrument
   *   later, in any pass. It is a fallback for silence, not a goal that outranks what the strategy says;
   * - **released** only by `retry_deferred`, never inside an ordinary pass, and only on **the broker's
   *   own record of each stop**: every stop it waited on is no longer listed, and has a history row --
   *   matched by `OrderSnapshot::order_id_` against the reference's primary part -- that is terminal
   *   with **nothing filled**. A listing alone cannot tell a cancelled stop from a filled one, and a
   *   holdings read is no proof either, since a broker's holdings and order endpoints promise no
   *   read-after-write consistency. Fills are read from `execution_.filled_quantity_`, never from the
   *   status: KIS reports a partly filled order it then cancelled as `CANCELED`;
   * - **dropped without a send** when any stop it waited on shows anything filled. A full fill met the
   *   flatten already; a partial one leaves its remainder **for the strategy to restate** -- the one
   *   case this does not finish by itself, counted by `deferred_dropped_after_fill`. It fails towards
   *   an omission, never a second sell;
   * - **kept** while a stop is still listed, or its history row is missing or not yet terminal: a stale
   *   answer waits, it does not release.
   *
   * It needs only the working-order cadence -- `live::SessionSource` reads the history on demand in
   * the poll that no longer lists the stop -- and no holdings poll. Not journalled: it has produced no
   * instruction a broker could have seen, and a restart loses it as it loses every unstated intention.
   */
  std::vector<strategy::TargetPosition> deferred_targets() const {
    std::vector<strategy::TargetPosition> targets;
    targets.reserve(deferred_targets_.size());
    for (const auto& deferred : deferred_targets_) {
      targets.push_back(deferred.target_);
    }
    return targets;
  }

  /** How many targets have been deferred behind a stop's withdrawal; see `deferred_targets`. */
  std::size_t targets_deferred() const noexcept { return targets_deferred_; }

  /** Deferred targets dropped because a stop they waited on had filled; see `deferred_targets`. */
  std::size_t deferred_dropped_after_fill() const noexcept { return deferred_dropped_after_fill_; }

  /**
   * Returns the earliest instant a target still waiting was deferred, where one's stops are all gone
   * from the listing -- the start of the history a caller must read for it -- or nothing when no
   * deferred target can be decided from history yet.
   */
  std::optional<std::chrono::system_clock::time_point> deferred_awaiting_evidence() const {
    std::optional<std::chrono::system_clock::time_point> earliest;
    for (const auto& deferred : deferred_targets_) {
      if (stop_listed(deferred.target_.instrument_)) {
        continue;
      }
      if (!earliest || deferred.deferred_at_ < *earliest) {
        earliest = deferred.deferred_at_;
      }
    }
    return earliest;
  }

  /**
   * Releases, drops or keeps each deferred target against `evidence`, the broker's history rows, and
   * settles what it released.
   *
   * `live::SessionSource` calls it after a working-order poll's update feeds, with the history it read
   * because `deferred_awaiting_evidence` asked for it. It dispatches no handler, settles only when it
   * released something, and never runs inside an ordinary pass: an order update's pass would otherwise
   * release the fallback before a later update in the same listing let the strategy say something else.
   */
  Report retry_deferred(const std::vector<OrderSnapshot>& evidence) {
    return release_deferred(&evidence);
  }

  /**
   * How many orders this runner sent are still counted until the broker confirms what became of them.
   *
   * **Why a listing alone does not end a reservation** (lab F15). A working-order listing replaces
   * `working_` wholesale, and used to retire every reserved row with it. But a market order that filled
   * at once is in no listing, a resting order may not be listed yet, and a listed order can fill and
   * leave the listing -- and in each case a holdings answer that lags the fill does not show it either.
   * A target then sent the order again: in the lab, holdings 70 s behind turned 47 trades into 295, at
   * up to twice the target.
   *
   * So a runner that keeps sent orders until listed (`keeps_sent_orders_until_listed`) remembers every
   * order the broker accepted with a reference, with the instrument's net holding when it was sent --
   * and every order whose request failed in transit, from the send, with no reference until a sweep
   * names it (`resolve_indeterminate_placement`).
   * Before every handler and every pass the reserved row is rebuilt from it, as the order's quantity
   * that nothing yet explains: less what holdings have moved since in the order's direction, less what
   * a listing still shows for the same reference. Several orders on one instrument are credited in
   * the order they were sent; orders on both sides of one instrument are credited only when holdings
   * account for all of them. The entry ends when **holdings** show the whole quantity, or when the
   * broker's history shows the order ended with less filled (`confirm_from_history`). A trade outside
   * this runner on the same instrument can shrink or delay that, until the timeout withholds the
   * instrument; it cannot make a target send twice.
   *
   * Only orders accepted in this process: a restart starts with none (`running-a-strategy-live.md`).
   */
  std::size_t unconfirmed_orders() const noexcept { return unconfirmed_.size(); }

  /**
   * Rebuilds those orders' reserved rows against the portfolio as it stands now.
   *
   * Every pass and every handler does this first. A caller reading `working_` outside one -- the
   * loop's closing pass sizing a flatten -- calls it before reading.
   */
  void refresh_unconfirmed() { confirm_reservations(); }

  /**
   * Hands a sweep's answer to an order whose request failed in transit (`SendOutcome::INDETERMINATE`).
   *
   * Such an order is counted from its send like an accepted one, with no reference (lab F35): once a
   * sweep finds it in the broker's listing the ledger lets the instrument trade again, and a market
   * order that filled is then in no listing while holdings may not show it yet. With `order_id` (the
   * sweep matched it) the order is counted on as an accepted one is -- a listing covers what it still
   * shows, holdings what filled, its history what it ended with. Without (the sweep concluded it was
   * never placed) it is forgotten, and its reserved row goes with it. An instrument with no such order
   * here -- a predecessor's instruction resolved after a restart, a runner that does not keep its
   * orders -- is left alone.
   *
   * At most one exists per instrument: the ledger withholds every placement on an instrument while one
   * is unknown. `live::SessionSource` calls this for every recorded `MATCHED` or `NOT_PLACED` placement;
   * a source of the caller's own that reconciles by hand should too, or the order waits for holdings.
   */
  void resolve_indeterminate_placement(
    const InstrumentId& instrument, OrderSide side, const std::optional<std::string>& order_id
  ) {
    if constexpr (keeps_sent_orders_until_listed) {
      const auto found = std::find_if(unconfirmed_.begin(), unconfirmed_.end(), [&](const Unconfirmed& entry) {
        return entry.ref_.primary_.empty() && entry.instrument_ == instrument && entry.side_ == side;
      });
      if (found == unconfirmed_.end() || (order_id && order_id->empty())) {
        return;
      }
      // The row first: it is recognized by the reference about to change, and left behind it would be
      // counted beside the rebuilt one.
      auto portfolio = context_.portfolio();
      portfolio.working_.erase(
        std::remove_if(
          portfolio.working_.begin(), portfolio.working_.end(),
          [&](const WorkingOrder<Tag>& working) { return is_reserved_row_of(working, *found); }
        ),
        portfolio.working_.end()
      );
      context_.set_portfolio(std::move(portfolio));
      if (order_id) {
        // The primary part only: a broker's row is matched on it everywhere, and the listing that
        // carries the order supplies the rest of the reference.
        found->ref_.primary_ = *order_id;
        found->row_.ref_.primary_ = *order_id;
      } else {
        unconfirmed_.erase(found);
      }
      confirm_reservations();
    } else {
      (void)instrument;
      (void)side;
      (void)order_id;
    }
  }

  /**
   * Sets how long a sent order's quantity may stay unexplained before its instrument is withheld.
   *
   * Ten minutes by default. Measured from the first pass at which neither holdings nor a listing
   * account for the quantity, on `set_observed_at`'s clock or the decision instant, whichever is later.
   * When it passes, the broker's history is read once more (`unconfirmed_awaiting_evidence`); if that
   * does not settle it -- or if it does, and holdings still have not shown the filled quantity one more
   * timeout later -- the entry is dropped and its instrument **marked in the indeterminacy ledger**,
   * which an operator clears after reading the account (a restart drops it, as it drops every ledger
   * entry, and a sweep resolving another instruction on the instrument lifts it). A timeout too short
   * therefore costs trading on that instrument, never a duplicate order. The mark always waits for a
   * holdings answer read a whole timeout after the quantity became unexplained (for a settled order,
   * after the history settled it), so it does not depend on how often the loop polls. A source of your
   * own that never advances `Portfolio::as_of_` therefore never marks: the order stays reserved for the
   * rest of the run, with no ledger entry and no count.
   */
  void set_confirmation_timeout(std::chrono::nanoseconds timeout) { confirmation_timeout_ = timeout; }

  /** How many sent orders timed out unconfirmed, each withholding its instrument; see `set_confirmation_timeout`. */
  std::size_t orders_unconfirmed_timed_out() const noexcept { return orders_unconfirmed_timed_out_; }

  /**
   * Returns the earliest instant a sent order that only the broker's history can settle was sent, or
   * nothing when none needs it.
   *
   * An order needs it when some of its quantity is unexplained, no listing carries it, and either a
   * listing once did (it was cancelled, expired or filled since), this runner's withdrawal of it was
   * accepted (it may have filled before), or it has passed the confirmation timeout and the history has
   * not yet been read once past it (after that it waits on holdings alone). `live::SessionSource`
   * reads history from the day before this instant after a working-order poll and passes it to
   * `confirm_from_history`.
   */
  std::optional<std::chrono::system_clock::time_point> unconfirmed_awaiting_evidence() {
    // Against the portfolio as it stands: a listing just installed may carry, or no longer carry, an
    // order, and no pass has run since.
    confirm_reservations();
    std::optional<std::chrono::system_clock::time_point> earliest;
    const auto now = freshness_instant();
    for (const auto& entry : unconfirmed_) {
      if (!awaits_evidence(entry, now)) {
        continue;
      }
      if (!earliest || entry.sent_at_ < *earliest) {
        earliest = entry.sent_at_;
      }
    }
    return earliest;
  }

  /**
   * Settles sent orders against `evidence`, the broker's history rows, and withholds what timed out.
   *
   * A terminal row for an order (matched on the reference's primary part, instrument and side) caps
   * what holdings must still show at its `execution_.filled_quantity_` -- never read from the status,
   * since KIS reports a part-filled order it then cancelled as `CANCELED` -- and an order that ended
   * with nothing filled is forgotten at once, and one that filled gets a fresh timeout for holdings to
   * show it. An order past the confirmation timeout that this does not settle is flagged, and
   * `confirm_reservations` withholds its instrument once a holdings read taken a timeout after its
   * quantity became unexplained still lacks it. Sends nothing.
   */
  void confirm_from_history(const std::vector<OrderSnapshot>& evidence) {
    confirm_reservations();
    const auto now = freshness_instant();
    for (auto& entry : unconfirmed_) {
      entry.consulted_ = awaits_evidence(entry, now);
      if (!entry.consulted_) {
        continue;
      }
      for (const auto& row : evidence) {
        if (row.order_id_ == entry.ref_.primary_ && row.instrument_ == entry.instrument_ &&
            row.side_ == entry.side_ && (entry.session_ended_ || !strategy::order_is_open(row.status_))) {
          if (decimal_less(row.execution_.filled_quantity_, entry.expected_)) {
            entry.expected_ = row.execution_.filled_quantity_;
          }
          entry.settled_ = true;
          // Holdings get a full timeout from here to show it (`confirm_reservations`).
          entry.unexplained_since_ = now;
          break;
        }
      }
    }
    // Past the timeout and the history had no answer: noted, and judged by `confirm_reservations` against
    // the next holdings read, as a settled order is. Marking here, on the clock, marked a fill one
    // holdings read early on a loop polling slower than the timeout (the listing, and so this history,
    // is read before holdings in each iteration).
    for (auto& entry : unconfirmed_) {
      if (entry.consulted_ && !entry.settled_ && timed_out(entry, now)) {
        entry.history_missed_ = true;
      }
    }
    // Retires what the history just settled, and withholds what the holdings in hand already condemn.
    confirm_reservations();
  }

  /**
   * Plans again the targets withheld for want of a trustworthy account, once there is one, and the
   * targets the market phase refused, once the phase permits them.
   *
   * A target withheld as `PORTFOLIO_ABSENT`, `PORTFOLIO_STALE` or `PORTFOLIO_SUPERSEDED` -- stated from
   * an `on_order_update` during the listing poll, say, or on a bar after a failed holdings read -- is
   * kept (`targets_retained`) and resubmitted here. `PollingLoop` calls it after each iteration's
   * polls and gate, so the account and the budget it is sized against are that iteration's.
   *
   * **A target the phase refused waits for a permitted phase** (`targets_retained_for_phase`), and so
   * does an order intent (`intents_kept_for_phase`), checked against the book at release. A daily
   * strategy decides on a bar fed after its day has ended -- at 00:00 KST for every shipped adapter's
   * daily bars -- while the market is shut: its opening order is refused `PHASE_NOT_PERMITTED`, and an
   * exit the limit admits outside the phase is typically rejected by a broker that is not taking
   * orders. Either way the target is kept, and planned again in the first iteration whose phase
   * `RiskLimits::permitted_phases_` admits, which is what a replay does by filling at the next bar's
   * open. Only for a runner that sends to a broker: a replay and a dry run never call this, and keep
   * dropping a refused target as before.
   *
   * **An instruction the broker could not be reached for, or turned away for its rate limit, is kept
   * here too** (`broker_unreachable`, `broker_rate_limited`),
   * with the phase-refusal rules, and offered again only once a holdings read newer than the failure
   * has arrived: a retry every iteration into a dead connection would wait out a connect timeout per
   * instruction and hold the loop, and a stop, behind it. The `_for_phase` counts include them. This
   * needs a source that advances `Portfolio::as_of_` on every successful holdings read, as
   * `SessionSource` does; one that does not leaves them waiting until a later statement, a stop or a
   * halt replaces them, sending nothing.
   *
   * **Does nothing, and keeps them, until it can succeed**: while the account was never read or
   * predates the listing, and while any hold is in force. A later target or order intent for the same
   * instrument replaces a retained target; a stop or an abandoned run discards them
   * (`clear_retained_targets`), so retained work lives for one `PollingLoop::run`. `PollingLoop`
   * retries last in an iteration, after `on_phase`. Within a run, a target retained for the account
   * before an operator's halt survives it and is planned, against fresh holdings, once the halt is
   * lifted; **a target retained for the phase does not**: an operator's halt or a local failure
   * discards it, because it may have been decided hours before and the operator stopped trading since.
   * The discard happens here, so a program driving a runner by hand that halts and resumes without a
   * call in between keeps it; `PollingLoop` calls this every iteration.
   *
   * A program driving a `Runner` without `PollingLoop` retains the same way and must call this
   * itself, or `clear_retained_targets`; nothing else replays or discards them. A replay never stamps
   * `Portfolio::working_as_of_`, so a backtest never withholds as superseded.
   */
  Report retry_retained_targets() {
    // Once per iteration, so the context shows what this iteration's holdings confirmed even when no
    // pass runs (`unconfirmed_orders`).
    confirm_reservations();
    if (guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      retained_targets_.erase(
        std::remove_if(
          retained_targets_.begin(), retained_targets_.end(),
          [](const RetainedTarget& kept) { return kept.for_phase_; }
        ),
        retained_targets_.end()
      );
      kept_intents_.clear();
    }
    if ((retained_targets_.empty() && kept_intents_.empty()) ||
        context_.portfolio().as_of_ == std::chrono::system_clock::time_point{} ||
        context_.portfolio().listing_newer_than_holdings() || guard_.blocking_reason()) {
      return Report{};
    }
    const bool phase_permits = guard_.permits_phase(context_.phase());
    std::vector<RetainedTarget> kept;
    bool released = false;
    const auto as_of = context_.portfolio().as_of_;
    // Kept because the broker could not be reached: not offered again until it has answered since.
    const auto awaiting_broker = [&](const auto& entry) { return entry.after_ && !(*entry.after_ < as_of); };
    for (auto& entry : retained_targets_) {
      if ((entry.for_phase_ && !phase_permits) || awaiting_broker(entry)) {
        kept.push_back(std::move(entry));
        continue;
      }
      context_.submit(std::move(entry.target_));
      released = true;
    }
    retained_targets_ = std::move(kept);
    // Kept intents follow, in the order they were stated, and only once the phase permits. **Each is
    // checked against the premise that kept it**: it states a quantity decided against the book at
    // midnight, so it goes out only if the instrument's holding is still what it was then, and a kept
    // stop only if no stop has come to rest meanwhile. Otherwise it is dropped and counted -- another
    // client traded the account, or a corporate action moved it, and a quantity nobody re-decided must
    // not be sent against the new book.
    if (phase_permits && !kept_intents_.empty()) {
      auto intents = std::move(kept_intents_);
      kept_intents_.clear();
      for (auto& entry : intents) {
        if (awaiting_broker(entry)) {
          kept_intents_.push_back(std::move(entry));
          continue;
        }
        const auto held = context_.portfolio().net_quantity_of(entry.intent_.instrument());
        const auto gap = held ? decimal_subtract(*held, entry.held_) : std::nullopt;
        // A weight is planned again as the position it states, against the holding at release, so
        // the book it was decided against does not bind it.
        const bool weight = entry.intent_.size().kind() == strategy::SizeKind::TARGET_WEIGHT;
        const bool same_book = weight || (gap && gap->coefficient() == 0);
        const bool stop_now_rests = entry.intent_.trigger() && stop_listed(entry.intent_.instrument());
        if (!same_book || stop_now_rests) {
          ++kept_intents_dropped_;
          continue;
        }
        context_.submit(std::move(entry.intent_));
        released = true;
      }
    }
    if (!released) {
      return Report{};
    }
    return settle();
  }

  /**
   * Returns how many order intents wait for a permitted market phase, or for a broker that could not be
   * reached or turned them away for its rate limit; see `retry_retained_targets`.
   */
  std::size_t intents_kept_for_phase() const noexcept { return kept_intents_.size(); }

  /**
   * Returns how many kept intents were dropped at release because the book they were decided against
   * had changed; monotone.
   */
  std::size_t kept_intents_dropped() const noexcept { return kept_intents_dropped_; }

  /** Returns how many targets are retained, for either reason; see `retry_retained_targets`. */
  std::size_t targets_retained() const noexcept { return retained_targets_.size(); }

  /**
   * Returns how many of them wait for a permitted market phase, or for a broker that could not be
   * reached or turned them away for its rate limit; see `retry_retained_targets`.
   */
  std::size_t targets_retained_for_phase() const noexcept {
    return static_cast<std::size_t>(std::count_if(
      retained_targets_.begin(), retained_targets_.end(),
      [](const RetainedTarget& kept) { return kept.for_phase_; }
    ));
  }

  /** Discards every retained target. `PollingLoop` calls it once a stop is requested. */
  void clear_retained_targets() noexcept {
    retained_targets_.clear();
    kept_intents_.clear();
  }

  /**
   * Releases, and settles, every deferred target whose stops are gone, for a caller whose working
   * orders and fills are **one ledger**.
   *
   * A replay is that caller: its ledger is the broker, so a stop it has just withdrawn is gone and
   * could not have filled, and there is no history to wait for. A live caller must use
   * `retry_deferred`, because a live stop that left the listing may have filled.
   */
  Report retry_deferred_on_one_snapshot() { return release_deferred(nullptr); }

  /** @copydoc Runner::plans_withheld */
  std::size_t instructions_unavailable() const noexcept { return instructions_unavailable_; }

  /**
   * The same question for **withdrawals**, in four figures rather than one.
   *
   * One figure was the plan and it would have stated a falsehood. An `INDETERMINATE` cancellation may
   * have reached the broker and may have withdrawn the order -- which is why `send_cancel` records it
   * apart from a refusal in the first place -- so calling it "not sent" would claim something nobody
   * knows.
   *
   * - `cancels_rejected` -- refused outright, **by the broker or by the binding**. The two share a
   *   vector, which carries `RejectedCancel::by_broker_` to tell them apart; this figure does not.
   * - `cancels_refused` -- a hold refused it before anything was sent.
   * - `cancels_unresolved` -- the reference named an order the snapshot does not hold, so there was
   *   nothing to withdraw.
   * - `cancels_indeterminate` -- the request failed in transit. It may have withdrawn the order.
   */
  std::size_t cancels_rejected() const noexcept { return cancels_rejected_; }
  /** @copydoc Runner::cancels_rejected */
  std::size_t cancels_refused() const noexcept { return cancels_refused_; }
  /** @copydoc Runner::cancels_rejected */
  std::size_t cancels_unresolved() const noexcept { return cancels_unresolved_; }
  /** @copydoc Runner::cancels_rejected */
  std::size_t cancels_indeterminate() const noexcept { return cancels_indeterminate_; }

  /**
   * What the broker said about the **most recent** order it refused, where the executor can say.
   *
   * Overwritten rather than accumulated: a run refused ten thousand times keeps **at most** one
   * record, which is what makes this affordable at all -- at most, because a refusal that states
   * nothing clears it, so a non-zero count can sit beside an empty optional.
   *
   * Empty on a runner whose broker has refused nothing, and empty
   * on every executor that cannot answer -- a dry runner and a replay's queueing executor have no
   * broker to have been refused by, so `runner_detail::states_last_error` detects the ability rather
   * than requiring it.
   *
   * **A count without this was the half-measure worth avoiding.** `orders_broker_refused` says an
   * operator has a problem; this says which instrument, under which of the strategy's own tags, and
   * in the broker's own words.
   */
  const std::optional<BrokerRefusal>& last_broker_refusal() const noexcept {
    return last_broker_refusal_;
  }

  /**
   * Why reconciliation could not build the **most recent** instruction it refused, and for what.
   *
   * The reason behind `orders_unbuildable`, kept as `last_broker_refusal` keeps the broker's: a live
   * source discards each pass's `Report`, whose `rejected_` carries it per instruction, so without this
   * a run reports only a count. Overwritten, not accumulated. Empty until reconciliation refuses one.
   * A live weight-sized intent, for one, is refused `NO_PORTFOLIO_VALUE` on every pass unless the
   * caller sets `set_portfolio_value`.
   */
  const std::optional<strategy::Rejection>& last_unbuildable() const noexcept {
    return last_unbuildable_;
  }

  /**
   * Sets how small a drift is not worth an instruction.
   *
   * An execution-cost policy and therefore the operator's, alongside `RiskLimits`. Every floor unset
   * by default, which chases every share. See `strategy::TradeTolerance` for the three units, why one
   * share count is the weakest of them, and why no default is invented.
   */
  void set_trade_tolerance(strategy::TradeTolerance tolerance) {
    tolerance_ = std::move(tolerance);
  }

  /**
   * Sets only the share-count floor, for the simple case.
   *
   * Right where a lot size is large or a venue charges per share. For a portfolio spanning price
   * levels, prefer a notional or fractional floor: one share count implies a value threshold that
   * differs by the ratio of the prices involved.
   */
  void set_minimum_trade(Decimal64 quantity) {
    tolerance_.minimum_quantity_ = std::move(quantity);
  }

  /**
   * Records the portfolio's total value, which sizing a target weight needs.
   *
   * Set by the caller rather than summed from the portfolio, because a total spanning currencies
   * needs an exchange rate this library does not yet carry, and inventing one would silently
   * misprice every weighted order. Absent means a weighted intent is refused by name.
   */
  void set_portfolio_value(Decimal64 value) {
    portfolio_value_ = std::move(value);
    portfolio_value_currency_.reset();
  }

  /**
   * Records the portfolio's total value and the currency it is in. A weight on an instrument priced
   * in another currency is then refused rather than sized across currencies at a rate of one.
   * **Never cleared on its own**: a live caller who sets it once sizes every weight against that
   * figure until it sets another or calls `clear_portfolio_value`.
   */
  void set_portfolio_value(Money value) {
    portfolio_value_ = std::move(value.amount_);
    portfolio_value_currency_ = std::move(value.currency_);
  }

  /** Forgets the portfolio value, so a weighted intent is refused rather than sized against an old one. */
  void clear_portfolio_value() noexcept {
    portfolio_value_.reset();
    portfolio_value_currency_.reset();
  }

  /** Calls `on_start` and drains whatever it asked for. */
  Report start() {
    dispatch::start(strategy_, context_);
    return settle();
  }

  /**
   * Calls `on_stop` and drains whatever it asked for.
   *
   * For a caller driving a runner directly. `PollingLoop` deliberately does **not** use this during
   * an operator's stop: it calls `notify_stop` while its hold is still in force, so a strategy's
   * parting instructions are refused rather than settled beside the operator's own, and then
   * `settle_closing` for those. See `notify_stop` for why.
   */
  Report stop() {
    dispatch::stop(strategy_, context_);
    return settle();
  }

  /**
   * Calls `on_stop` and settles what it asked for **now**, before anything else is queued.
   *
   * Exists so that a strategy's shutdown handler cannot ride out alongside an operator's closing
   * instructions. During a `WITHDRAW` or a `FLATTEN` the operator has taken over: the loop sizes
   * its closing orders against what it can prove is outstanding, and an order the strategy adds
   * afterwards goes through none of that reasoning -- it is planned against the same snapshot and
   * can close the same position a second time.
   *
   * Called while the loop's `STOPPING` hold stands, so every instruction here is refused by the
   * guard and reported. The strategy still learns the run is ending, which is what the handler is
   * for; it simply cannot trade on the way out.
   */
  Report notify_stop() {
    dispatch::stop(strategy_, context_);
    return settle();
  }

  /**
   * Settles instructions a caller has already queued, dispatching no handler.
   *
   * The other half of `notify_stop`: `PollingLoop` queues its own cancellations and closing orders
   * and settles exactly those. Using `stop()` here would call `on_stop` inside the window where the
   * loop has lifted its hold so its own instructions can go -- which is the one moment a strategy's
   * order must not.
   */
  Report settle_closing() { return settle(); }

  /**
   * Feeds one bar: records it in the cached series, advances the clock, then calls the handler.
   *
   * The clock advances to the bar's **period end**, not its timestamp, because that is the earliest
   * instant the bar could legitimately have been known. Advancing to a timestamp that marks the
   * period's open would place the runner inside a bar it is being told about, which is the
   * off-by-one that makes a backtest unreproducible.
   */
  Report feed(const strategy::BarEvent& event) {
#if defined(ATL_HAS_EIGEN)
    context_.record_bar(event);
#endif
    context_.set_last_price(event.instrument_, event.bar_.close_);
    context_.set_now(event.period_end_);
    confirm_reservations();
    dispatch::bar(strategy_, context_, event);
    return settle();
  }

  /**
   * Feeds one bar as history: the strategy sees it, and nothing it decides is sent.
   *
   * Exactly `feed`'s bookkeeping -- the bar is recorded in the series, the last price and the decision
   * instant advance, and `on_bar` runs -- and then whatever the handler asked for is **discarded**,
   * counted in `warmup_instructions_discarded`, without planning, sending, retaining or deferring any
   * of it. A strategy's own state therefore evolves over a history exactly as it does in a replay; only
   * the sending is withheld, and nothing a later pass relies on (deferred and retained targets, the
   * reservations, the sellable probe, the guard's record) is touched.
   *
   * **The rule it serves**: an instruction reaches a broker only when it was decided on the newest
   * closed bar a source held for that instrument at that poll. `live::SessionSource` feeds every older
   * bar of a read through here -- a cold start's history, or a backlog after an outage -- so a restart
   * does not act on a signal from months ago at today's price. A source of your own must do the same:
   * feed a read's new bars in period-end order across instruments, and feed live only each
   * instrument's newest bar whose period ended after every other bar fed as history.
   */
  void feed_warmup(const strategy::BarEvent& event) {
#if defined(ATL_HAS_EIGEN)
    context_.record_bar(event);
#endif
    context_.set_last_price(event.instrument_, event.bar_.close_);
    context_.set_now(event.period_end_);
    dispatch::bar(strategy_, context_, event);
    warmup_instructions_discarded_ += context_.drain().size();
  }

  /** Returns how many instructions `feed_warmup` discarded; monotone for the runner's life. */
  std::size_t warmup_instructions_discarded() const noexcept { return warmup_instructions_discarded_; }

  /** Feeds one quote and calls the handler. */
  Report feed(const strategy::QuoteEvent& event) {
    context_.set_last_price(event.quote_.instrument_, event.quote_.last_price_);
    context_.set_now(event.quote_.received_at_);
    confirm_reservations();
    dispatch::quote(strategy_, context_, event);
    return settle();
  }

  /** Feeds one execution and calls the handler. */
  Report feed(const strategy::Fill& event) {
    confirm_reservations();
    dispatch::fill(strategy_, context_, event);
    return settle();
  }

  /** Feeds one order update and calls the handler. */
  Report feed(const OrderSnapshot& event) {
    confirm_reservations();
    dispatch::order_update(strategy_, context_, event);
    return settle();
  }

  /** Announces a phase change, setting it on the context first so a handler reads it. */
  Report feed(strategy::MarketPhase phase) {
    context_.set_phase(phase);
    // A DAY order does not outlive its session, whatever its history row says afterwards: a broker may
    // leave an expired order's remainder on the row (KIS reads it from `rmn_qty`), so the row's filled
    // quantity is taken as final once the session has closed (`confirm_from_history`).
    if (phase == strategy::MarketPhase::CLOSED || phase == strategy::MarketPhase::POST_CLOSE) {
      for (auto& entry : unconfirmed_) {
        if (entry.row_.order_.time_in_force_ == TimeInForce::DAY) {
          entry.session_ended_ = true;
        }
      }
    }
    confirm_reservations();
    dispatch::phase(strategy_, context_, phase);
    return settle();
  }

private:
  /**
   * Drains the queue and turns it into a report.
   *
   * **Cancellations are collected before any order is placed**, across the whole queue, rather than
   * per instruction. That is the sequencing `Plan` documents as the caller's obligation, and here
   * is the caller: a replacement placed before its predecessor is withdrawn is briefly a double
   * position, while cancelling first leaves the instrument unrepresented for a moment instead.
   *
   * A guard refusal drops that instruction and nothing else. One instrument breaching a position
   * ceiling must not silence the rest of a cross-sectional rebalance.
   */
  Report settle() {
    // Before anything is planned, because this pass has not yet added anything of its own: the
    // probe must not read this runner's intentions as the broker's report. The portfolio can still
    // carry a reservation an *earlier* pass persisted, which is why those rows are marked and why
    // the probe skips them rather than relying on when it is called.
    observe_sellable_convention();
    // After the probe, which must read the broker's report rather than this runner's rows, and before
    // anything is planned: whatever replaced `working_` since the last pass, each order this runner
    // sent is counted again at what nothing yet explains (`unconfirmed_orders`).
    confirm_reservations();

    Report report;
    auto instructions = context_.drain();
    kept_this_pass_.clear();
    drop_superseded_deferred(instructions);
    std::vector<strategy::PlannedOrder> pending;
    // The instruction each pending order came from, aligned with `pending`, so a phase refusal can
    // keep the target that produced it (`retain_for_phase`). Points into `instructions`, which lives
    // for the whole pass.
    std::vector<const Instruction<Tag>*> origin;

    // **One withdrawal per order per pass.** A strategy that clears what is resting and then states
    // a target asks twice for the same order: the explicit `CancelIntent` goes out, and then
    // `plan_for` reconciles the target against `context_.portfolio()` -- which does not carry the
    // cancellation just sent -- and plans a withdrawal for the same row. Two requests, and a broker
    // answering the second with "already cancelled" then reads as a rejected withdrawal in the
    // report.
    //
    // **Only the duplicate send is suppressed; the picture planning reads is untouched.** Removing
    // the row from that view is the tempting fix and it is unsafe: `SendOutcome::ACCEPTED` on a
    // withdrawal means the broker took the *request*, and `PENDING_CANCEL` counts as open in
    // `strategy::order_is_open`. Sizing a replacement against an emptied book would place a fresh
    // order of the full target while the original may still fill -- doubling a position to save a
    // request. Reconciliation keeps treating a possibly-working order as working, which is the
    // right answer until a listing says otherwise.
    //
    // Keyed on *attempted*, not on accepted: no outcome here means the order is gone, and a
    // withdrawal that was refused locally will be refused the same way a second time.
    std::vector<OrderRef<Tag>> attempted;
    const auto attempt_cancel = [this, &attempted, &report](const OrderRef<Tag>& ref) {
      // An unaddressed reference is exempt. Two of them compare equal, so deduping would collapse
      // two distinct unsendable withdrawals into one report entry and hide the second.
      if (!ref.primary_.empty() || !ref.secondary_.empty()) {
        for (const auto& seen : attempted) {
          if (seen == ref) {
            return;
          }
        }
        attempted.push_back(ref);
      }
      send_cancel(ref, report);
    };

    for (const auto& instruction : instructions) {
      // **A later target or order replaces a retained target**: the strategy has spoken again about this
      // instrument, and replaying what it said before would override it. A withheld target is retained
      // again below, as the latest. A `CancelIntent` names an order, not an instrument, and leaves it.
      forget_retained(instruction_instrument(instruction));
      if (const auto* cancel = std::get_if<strategy::CancelIntent<Tag>>(&instruction)) {
        attempt_cancel(cancel->ref_);
        continue;
      }

      const auto planned = plan_for(instruction);
      // **Checked before a single cancellation goes out, and on the plan rather than on its parts.**
      //
      // `plan_for` derives both halves from `context_.portfolio()`: which rows oppose the target, and
      // what remains to be placed. So a plan is only as good as the snapshot under it, and the
      // cancellations are sent below -- before the guard has seen the placements at all. Deciding the
      // halves separately is how a reprice becomes a bare withdrawal.
      //
      // Only a plan that would cancel something is stopped. One that only places is left to the guard
      // exactly as before, so a direct `OrderIntent` keeps reporting as a refusal rather than quietly
      // changing bucket.
      const bool would_cancel = !planned.cancel_.empty();
      if (const auto reason = plan_withhold_reason(would_cancel)) {
        // **A missing account stops the whole plan; a hold on placements stops only a plan that
        // would also withdraw something.**
        //
        // The difference is which of the two the *guard* can be relied on to catch. A held placement
        // is refused by `RiskGuard` a few lines below, with the hold named on its own report entry,
        // and that is the better result -- so a plan with nothing to cancel is left to it. What the
        // guard cannot catch is data: a snapshot nobody has read is not a hold, and the loop's hold
        // for it is not yet installed at every point an instruction can be settled. A source's
        // `read_order_list` runs before `PollingLoop` installs any hold and is handed a mutable
        // runner, so a target settled inside that call reaches here with the guard clear. Deciding
        // absence and staleness here, on the snapshot itself, is what makes the rule hold at every
        // send site rather than at most of them.
        // A missing-account reason is only ever returned when it should stop this plan, so it needs
        // no second test here. A hold on placements stops only a plan that would also withdraw
        // something: a placement-only plan is left to `RiskGuard` below, which refuses it with the
        // hold named on its own report entry, and that is the better result.
        const bool data_is_missing = *reason != WithheldPlanReason::PLACEMENT_HELD;
        if (data_is_missing || would_cancel) {
          // The whole plan, before any of its parts were counted separately. **Four reasons reach
        // here** and this counts them together: an account never read, one older than the stated
        // maximum, one read before the listing it would be reconciled with, and a hold refusing
        // placements for a plan that also wanted to withdraw something -- the last two on a perfectly
        // fresh portfolio. Counted at all because the loop's own gate
        // does not always describe it: with `allow_absent_portfolio` the gate can be open while this
        // fires, which is the case Codex round 0 named.
        ++plans_withheld_;
        std::string detail = withhold_detail(*reason);
        if (data_is_missing) {
          if (retain_withheld(instruction)) {
            detail = "retained for the next holdings read";
          } else if (*reason == WithheldPlanReason::PORTFOLIO_SUPERSEDED &&
                     std::holds_alternative<strategy::OrderIntent>(instruction)) {
            detail = "dropped: an order intent states a size against the mis-ordered picture";
          }
        }
        report.withheld_plans_.push_back(WithheldPlan<Tag>{
            *reason,
            instruction_instrument(instruction),
            instruction_tag(instruction),
            planned.cancel_,
            planned.place_.size(),
            std::move(detail),
          });
          continue;
        }
      }
      for (const auto& ref : planned.cancel_) {
        attempt_cancel(ref);
      }
      defer_if_waiting_on_stop(instruction, planned);
      for (const auto& rejection : planned.rejected_) {
        // Reconciliation's own refusal: it could not build a valid instruction at all, which is a
        // different fact from a broker saying no. **Not a diagnosis**: the same bucket holds a
        // quantity that rounded away on the lot, which is a deliberate drop. The reason is on the
        // entry.
        ++orders_unbuildable_;
        last_unbuildable_ = rejection;
        report.rejected_.push_back(rejection);
      }
      for (const auto& order : planned.place_) {
        pending.push_back(order);
        origin.push_back(&instruction);
      }
    }

    strategy::GuardInputs inputs;
    inputs.now_ = context_.now();
    inputs.phase_ = context_.phase();

    // **Every order in this pass is checked against a view that already contains the orders this
    // pass has sent.** Without that, a limit is re-evaluated from the same starting point for every
    // order, and two orders that each fit can breach it together: with a hundred sellable and no
    // resting order, two sells of sixty both pass and twenty shares are sold that were not there.
    //
    // A copy per pass, mutated as orders are accepted, closes that within one `settle`.
    // `persist_reservation` closes the same hazard *across* passes: always for a runner that sends to
    // a broker listing working orders, and otherwise for the limits that read what is outstanding.
    // Nothing has to recognise its own order in a later listing: a working-order poll assigns
    // `working_` wholesale (a holdings poll keeps it), so the broker's own answer replaces the
    // reservation when it arrives. Between a placement and that poll, the context portfolio is
    // therefore the broker's last report **plus** what this runner has sent since -- which is what a
    // target and a limit have to size against, and why the rows are marked so that anything inferring
    // the broker's behaviour from the listing can tell them apart.
    auto view = context_.portfolio();
    // Resolved once for the pass. The currencies and marks do not move as orders are accepted --
    // only quantities do, and the guard reads those from the view -- and resolving per order would
    // repeat a map lookup per instrument for every instrument. Built only when the ceiling is on, so
    // a caller who has not asked for it pays nothing.
    if (guard_.limits().max_gross_exposure_) {
      inputs.exposure_basis_ = exposure_basis_for(view, pending);
    }
    for (std::size_t at = 0; at < pending.size(); ++at) {
      auto& order = pending[at];
      const auto& source_instruction = *origin[at];
      // Checked before the guards, because an unresolved instrument is a safety hold rather than a
      // limit breach: reporting a notional ceiling for an order that was never going to be sent
      // would point an operator at the wrong problem.
      // Checked before the ledger and before the guards, and checked *per order* rather than once
      // at the top. A journal that failed on the first order of a batch will fail on the second, so
      // a batch that carried on would produce one unexplained refusal per instrument. This is also
      // what makes the condition hold for a direct `feed()`, which never reaches `PollingLoop`'s
      // gate at all.
      if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
        ++instructions_unavailable_;
        report.unavailable_.push_back(
          WithheldOrder{
            WithheldReason::LOCAL_FAILURE,
            order,
            guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE),
          }
        );
        continue;
      }
      if (ledger_.requires_reconcile(order.instrument_)) {
        // The indeterminacy ledger, not a guard: this instrument has an outcome nobody knows, so
        // nothing further goes out for it until a listing settles the question. Counted apart from
        // every other origin because the operator's response is to reconcile rather than to relax a
        // limit or fix a price.
        ++orders_reconcile_withheld_;
        report.withheld_.push_back(
          WithheldOrder{
            WithheldReason::RECONCILE_REQUIRED,
            order,
            std::string(ledger_.detail(order.instrument_)),
          }
        );
        continue;
      }

      fill_sellable_if_needed(view, order);

      inputs.reference_price_ = context_.last_price(order.instrument_);
      // The currency the order is priced in, which only the reference data knows. A buying-power
      // limit is per currency, and an unresolved instrument leaves it absent rather than defaulting
      // to one: guessing here would spend one currency's budget on another's order.
      const auto reference = context_.instrument(order.instrument_);
      inputs.order_currency_ =
        reference ? std::optional<Currency>(reference->currency_) : std::nullopt;
      // `evaluate` rather than `check`, for one fact `check` drops: whether the phase limit admitted
      // this order only because it closes a position. Nothing downstream can recover that -- the
      // order, the portfolio and the report all look the same either way -- and it is the one thing
      // an operator needs to see, because it is this library sending into a session the operator's
      // own limits said not to trade in.
      auto decision = guard_.evaluate(order, view, inputs);
      if (decision.rejection_) {
        // **Counted here and not at the send site, because there is no send.** A buy left above a
        // withheld ceiling is the larger notional, so every limit that multiplies quantity by price
        // sees a bigger number than the clamped order would have shown it. (A sell left below a
        // withheld floor is the smaller notional; these limits are not what stops that one.)
        if (order.price_would_have_been_clamped_) {
          ++unclamped_orders_refused_;
        }
        ++orders_guard_refused_;
        if (decision.rejection_->reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED) {
          retain_for_phase(source_instruction);
        }
        report.refused_.push_back(std::move(*decision.rejection_));
        continue;
      }

      // **The route it is sent with, written into the order before anything records it.** A
      // strategy names no venue, and a binding may send one anyway (`SessionExecutor::default_route`).
      // Resolved here rather than only at the wire so the journal line, the row `reserve` keeps --
      // which `cancel_all` and every other instruction built from it restate -- and the report all
      // name the venue the broker was asked for, not the blank the strategy left.
      order.venue_ = requested_route(order.venue_);
      const auto outcome = executor_.place(order);
      // **Counted where the outcome is known, not where the exception was granted.** An admitted
      // order can still be refused by the sellable check, a buying-power limit or either ceiling and
      // never leave the process, so counting at the guard would report a proxy for the harm rather
      // than the harm. `REJECTED` counts: the request went out and the broker answered it, which is
      // exactly the nightly-rejection case this exception was chosen knowing about, and a witness
      // that stayed at zero through every one of them would be silent in the case it exists for.
      // `REFUSED_LOCALLY` and `UNAVAILABLE` do not: nothing was sent. This is the same division
      // `note_sent` already makes, for the same reason.
      if (decision.admitted_outside_phase_ &&
          (outcome == SendOutcome::ACCEPTED || outcome == SendOutcome::REJECTED ||
           outcome == SendOutcome::INDETERMINATE)) {
        ++sends_outside_permitted_phase_;
      }
      // **Split on whether a broker was asked, which is the division `send_outcome_reached_broker`
      // already makes.** `REJECTED` means the exchange saw this price and refused it.
      // `REFUSED_LOCALLY` means the binding did -- and that is a price-dependent refusal too: a
      // limit price no adapter will send is a limit price the withheld band might have moved into
      // range, and review found the concrete case, a sell at zero that `Price::checked` admits
      // because zero sits on a stated grid. Counting only the first left that invisible.
      // `INDETERMINATE` is excluded because it is not a refusal at all -- the request may well be
      // resting -- and `UNAVAILABLE` because it is local machinery being unavailable, which no price
      // affects.
      if (order.price_would_have_been_clamped_) {
        if (outcome == SendOutcome::REJECTED) {
          ++unclamped_orders_rejected_;
        } else if (outcome == SendOutcome::REFUSED_LOCALLY) {
          ++unclamped_orders_refused_;
        }
      }
      // After every send, not only after a refusal. A terminal journal write that fails leaves the
      // broker's own outcome intact -- the send happened, and saying otherwise would be a lie -- but
      // it still means the journal is broken, and the runner must stop.
      note_executor_failure(outcome == SendOutcome::UNAVAILABLE);
      switch (outcome) {
        case SendOutcome::ACCEPTED:
          // Counted against the rate limit only once the instruction actually went, so an order a
          // guard refused or the ledger withheld does not spend budget.
          guard_.note_sent(inputs.now_);
          ++placements_at_broker_;
          // And it is working now, as far as anything later in this pass is concerned, and its
          // money is spent. An `INDETERMINATE` send is counted the same way below, with no reference.
          reserve(
            view, order, inputs.reference_price_, inputs.order_currency_, last_ref(),
            guard_.limits().enforce_buying_power_
          );
          persist_reservation(order, inputs.reference_price_, inputs.order_currency_, last_ref());
          track_unconfirmed(order, last_ref());
          report.would_place_.push_back(std::move(order));
          break;
        case SendOutcome::REJECTED:
          // A refusal is an answer: the order does not exist, so nothing needs reconciling. It
          // still consumed a request, which is what the broker will have counted.
          guard_.note_sent(inputs.now_);
          ++orders_broker_refused_;
          // An exit the phase limit let out of session, refused by a broker not taking orders then:
          // the target waits for the session instead of for its next bar, which may be a day away.
          if (decision.admitted_outside_phase_) {
            retain_for_phase(source_instruction, /*broker_answered=*/true);
          } else if (last_error_is(broker_rate_limited)) {
            // **Turned away for the broker's rate limit, which is about the moment.** Kept as an
            // unreachable send is, and offered again after a newer holdings read: dropped, a daily
            // strategy's opening order or protective stop refused once at a busy open waited a day
            // (lab: one `EGW00201` at 09:00 moved a buy to the next session and left a position
            // without its stop all day). The broker processed nothing, so a retry cannot double it.
            retain_for_phase(source_instruction, /*broker_answered=*/true, context_.portfolio().as_of_);
          }
          note_broker_refusal(order);
          report.rejected_.push_back(
            strategy::Rejection{
              strategy::RejectReason::BROKER_REFUSED, order.instrument_, order.tag_
            }
          );
          break;
        case SendOutcome::INDETERMINATE:
          guard_.note_sent(inputs.now_);
          // Counted like an acceptance for the purpose below: the order may exist, and a listing
          // read before this moment cannot show it either way.
          ++placements_at_broker_;
          ledger_.mark(order.instrument_, "an order request failed in transit");
          ++orders_indeterminate_;
          // **Counted as if accepted, until the sweep says what became of it** (lab F35). The ledger
          // withholds the instrument meanwhile; but once a sweep finds the order in the broker's
          // listing it clears the ledger, and a market order that filled is then in no listing while
          // holdings may not show it yet, so a target sent it again (twice the target, in one
          // process). Reserved and tracked here with the holding at send as the baseline, and handed
          // its reference by `resolve_indeterminate_placement`. **No reference** is passed on purpose:
          // `last_ref()` still names the previous accepted order, and a row carrying it could be
          // withdrawn in that order's place.
          if constexpr (keeps_sent_orders_until_listed && runner_detail::reports_last_ref<Executor>::value) {
            reserve(
              view, order, inputs.reference_price_, inputs.order_currency_, std::nullopt,
              guard_.limits().enforce_buying_power_
            );
            persist_reservation(order, inputs.reference_price_, inputs.order_currency_, std::nullopt);
            track_unconfirmed(order, std::nullopt, /*indeterminate=*/true);
          }
          report.indeterminate_.push_back(std::move(order));
          break;
        case SendOutcome::REFUSED_LOCALLY:
          // **No `note_sent`**: the binding refused it, so no request went out and the broker
          // counted nothing. Spending rate-limit budget here would refuse a later order that would
          // have been allowed, for a request nobody made. Reported as a rejection because that is
          // what it is from the order's point of view -- it does not exist, and nothing needs
          // reconciling -- and the runner keeps trading, because the next instruction may be
          // perfectly sendable. The reason is carried on the entry itself rather than left to
          // `last_error()`: a later accepted order in the same pass clears that, so a report can
          // outlive the only record of which refusal this was.
          ++orders_binding_refused_;
          if (decision.admitted_outside_phase_) {
            retain_for_phase(source_instruction);
          } else if (last_error_is(broker_unreachable)) {
            // **The broker could not be reached, so the decision is kept rather than lost.** Dropped,
            // a daily strategy's target or protective stop released at the open into a dropped
            // connection waited for its next bar, a day away (lab: a 30-minute outage at 09:00 moved a
            // buy to the next session and left a position without its stop all day). Kept as a phase
            // refusal is -- discarded by a stop, an operator's halt or a local failure, replaced by a
            // later statement, an intent checked against the book -- and offered again only after a
            // holdings read newer than this one, which is the evidence the broker answers again: a
            // retry every tick into a dead connection would wait out a connect timeout per instrument
            // per iteration, and hold up a stop behind it.
            retain_for_phase(source_instruction, /*broker_answered=*/false, context_.portfolio().as_of_);
          }
          report.rejected_.push_back(
            strategy::Rejection{
              strategy::RejectReason::BINDING_REFUSED, order.instrument_, order.tag_
            }
          );
          break;
        case SendOutcome::UNAVAILABLE:
          // No `note_sent`: nothing went out, so the broker counted nothing and no rate-limit
          // budget was spent. The ledger is not marked either -- an instruction that was never
          // attempted leaves no doubt about the position, and withholding the instrument would be
          // withholding it for the wrong reason.
          ++instructions_unavailable_;
          report.unavailable_.push_back(
            WithheldOrder{
              WithheldReason::LOCAL_FAILURE,
              order,
              guard_.halted(strategy::HaltReason::LOCAL_FAILURE) ?
                guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE) :
                std::string("local machinery refused to send"),
            }
          );
          break;
      }
    }

    // The pass's instructions die with it; so do the pointers kept to deduplicate them.
    kept_this_pass_.clear();
    report.log_ = context_.log().take();
    return report;
  }

  /**
   * Sends one cancellation, and marks the instrument when its outcome is unknown.
   *
   * A cancellation that fails in transit leaves the same question an order does -- the order may
   * now be withdrawn or may still be resting -- so it obliges the same reconciliation. A
   * cancellation is not gated by the ledger, because withdrawing an order is the one action that
   * cannot make an unresolved position worse.
   */
  /**
   * Asks the broker what may be sold, but only when nothing else can answer.
   *
   * Four conditions, and every one of them is there to avoid a request:
   *
   * - the executor can ask at all -- a dry run and a replay cannot, and compile to nothing here;
   * - the caller enforces the limit, so nobody pays for a figure no check reads;
   * - the order is a sell, since the figure says nothing about buying;
   * - and the snapshot has not already answered, which at a `HOLDINGS` broker it always has.
   *
   * The answer is written to the context's portfolio as well as to this pass's view, so a later
   * pass in the same holdings cycle reuses it rather than asking again. The next holdings poll
   * replaces the positions wholesale and the figure goes with them, which is the right lifetime: it
   * is exactly as fresh as the position it describes.
   */
  void fill_sellable_if_needed(
    strategy::Portfolio<Tag>& view,
    const strategy::PlannedOrder& order
  ) {
    if constexpr (runner_detail::fills_sellable<Executor>::value) {
      if (!guard_.limits().enforce_sellable_quantity_ || order.side_ != OrderSide::SELL) {
        return;
      }
      if (view.sellable_quantity_of(order.instrument_)) {
        return;
      }
      const auto answered = executor_.sellable_quantity(order.instrument_);
      if (!answered) {
        // Left absent, so the guard refuses for the reason it already has. Reporting a failure to
        // establish the figure as a sellable quantity of zero would be a different claim.
        return;
      }
      write_sellable(view, order.instrument_, *answered);
      auto persisted = context_.portfolio();
      write_sellable(persisted, order.instrument_, *answered);
      context_.set_portfolio(std::move(persisted));
    } else {
      static_cast<void>(view);
      static_cast<void>(order);
    }
  }

  /** Returns the venue the executor sends an order asking for `asked` to; `asked` where it does not route. */
  Venue requested_route(Venue asked) const {
    if constexpr (runner_detail::resolves_route<Executor>::value) {
      return executor_.requested_route(asked);
    } else {
      return asked;
    }
  }

  /**
   * Returns the reference the broker issued for the order just accepted, where there is one.
   *
   * Nothing where the executor has no broker to have been answered by, which is every dry run and
   * every replay.
   */
  std::optional<OrderRef<Tag>> last_ref() const {
    if constexpr (runner_detail::reports_last_ref<Executor>::value) {
      return executor_.last_ref();
    } else {
      return std::nullopt;
    }
  }

  /**
   * An order the broker accepted, or one whose request failed in transit, that holdings have not yet
   * shown in full; see `unconfirmed_orders`.
   */
  struct Unconfirmed {
    /** Empty while the order's outcome is unknown; see `resolve_indeterminate_placement`. */
    OrderRef<Tag> ref_;
    InstrumentId instrument_;
    OrderSide side_{OrderSide::BUY};
    /** What holdings must still show: the order's quantity, or less once its history says so. */
    Decimal64 expected_;
    /** The instrument's net holding when it was sent. */
    Decimal64 baseline_;
    /** The reserved row as `reserve` wrote it; rebuilt with the residual quantity. */
    WorkingOrder<Tag> row_;
    std::chrono::system_clock::time_point sent_at_{};
    /** Whether any listing has carried it, and whether the latest one does. */
    bool listed_{false};
    bool listed_now_{false};
    /** Whether this runner's withdrawal of it was accepted. */
    bool withdrawn_{false};
    /** Whether a terminal history row has capped `expected_`. */
    bool settled_{false};
    /** Whether it is a DAY order and the session it was sent in has closed since. */
    bool session_ended_{false};
    /** Whether the current `confirm_from_history` looked for it. */
    bool consulted_{false};
    /** Whether the history, read past the timeout, had no terminal row for it. */
    bool history_missed_{false};
    /** The quantity nothing explains, as of the last rebuild, and since when some has been. */
    Decimal64 residual_;
    std::optional<std::chrono::system_clock::time_point> unexplained_since_;
  };
  /**
   * Remembers an order the broker just accepted, so a listing that lacks it cannot retire its
   * reservation; see `unconfirmed_orders`. Called right after `persist_reservation` wrote its row.
   *
   * Only for a runner that keeps sent orders until listed, an order with a broker reference, and a
   * portfolio that has been read: without a reference no listing or history row can be matched to it,
   * and without a read portfolio there is no baseline. Those keep the old behaviour, where the next
   * listing retires the row.
   *
   * `indeterminate`: an order whose request failed in transit, tracked with no reference until a sweep
   * names it (`resolve_indeterminate_placement`). Until then only holdings can cover it, and it never
   * times out on its own: the indeterminacy ledger already withholds its instrument.
   */
  void track_unconfirmed(
    const strategy::PlannedOrder& order, const std::optional<OrderRef<Tag>>& ref, bool indeterminate = false
  ) {
    if constexpr (keeps_sent_orders_until_listed) {
      const auto& portfolio = context_.portfolio();
      if ((!indeterminate && (!ref || ref->primary_.empty())) ||
          portfolio.as_of_ == std::chrono::system_clock::time_point{} || portfolio.working_.empty()) {
        return;
      }
      const auto held = portfolio.net_quantity_of(order.instrument_);
      if (!held) {
        return;
      }
      Unconfirmed entry;
      if (ref) {
        entry.ref_ = *ref;
      }
      entry.instrument_ = order.instrument_;
      entry.side_ = order.side_;
      entry.expected_ = order.quantity_;
      entry.baseline_ = *held;
      entry.row_ = portfolio.working_.back();
      entry.sent_at_ = freshness_instant();
      entry.residual_ = order.quantity_;
      entry.unexplained_since_ = entry.sent_at_;
      unconfirmed_.push_back(std::move(entry));
    } else {
      (void)order;
      (void)ref;
      (void)indeterminate;
    }
  }

  /** Records that the broker accepted this runner's withdrawal of `ref`, if it is a sent order still counted. */
  void note_withdrawal_accepted(const OrderRef<Tag>& ref) {
    for (auto& entry : unconfirmed_) {
      // The primary part alone, as everywhere a broker's own row is matched to the order.
      if (!entry.ref_.primary_.empty() && entry.ref_.primary_ == ref.primary_) {
        entry.withdrawn_ = true;
      }
    }
  }

  /** Whether some of `entry`'s quantity has been unexplained for longer than the confirmation timeout. */
  bool timed_out(const Unconfirmed& entry, std::chrono::system_clock::time_point now) const {
    return entry.unexplained_since_ && now - *entry.unexplained_since_ > confirmation_timeout_;
  }

  /** Whether only the broker's history can settle `entry` now; see `unconfirmed_awaiting_evidence`. */
  bool awaits_evidence(const Unconfirmed& entry, std::chrono::system_clock::time_point now) const {
    // A settled entry has had its answer: what it filled is known, and only holdings remain to show it.
    // One the history had no answer for past the timeout waits on holdings too, without a read per poll.
    // One with no reference yet has no row to find, and never times out: the ledger holds its
    // instrument until a sweep names the order (`resolve_indeterminate_placement`).
    if (entry.residual_.coefficient() == 0 || entry.settled_ || entry.history_missed_ ||
        entry.ref_.primary_.empty()) {
      return false;
    }
    if (timed_out(entry, now)) {
      return true;
    }
    return !entry.listed_now_ && (entry.listed_ || entry.withdrawn_);
  }

  /** Whether `working` is this runner's reserved row for `entry`. */
  static bool is_reserved_row_of(const WorkingOrder<Tag>& working, const Unconfirmed& entry) {
    return working.reserved_ && working.ref_.primary_ == entry.ref_.primary_ &&
      working.order_.instrument_ == entry.instrument_ && working.order_.side_ == entry.side_;
  }

  /**
   * Rebuilds the reserved row of every sent order still counted, and forgets those holdings confirm.
   *
   * Run before every handler and every pass, so whatever replaced `working_` since -- a listing, or a
   * source of the caller's own -- the strategy and the guard see each order's unexplained quantity:
   *
   * - **covered**: holdings movement since the first unconfirmed order's baseline on the instrument, in
   *   the orders' direction, credited in the order they were sent, each up to what it expects. Orders
   *   on both sides of one instrument are credited only when holdings equal the baseline plus all of
   *   them, since a movement could be either side's;
   * - **listed**: what a listing still shows remaining for the same reference, instrument and side;
   * - the row is put back with `expected - covered - listed`, when that is positive, and **never
   *   spends buying power again** -- the pass that sent the order already did;
   * - the entry ends once `covered` reaches what it expects.
   */
  void confirm_reservations() {
    if constexpr (keeps_sent_orders_until_listed) {
      if (unconfirmed_.empty()) {
        return;
      }
      const auto zero = Decimal64::parse("0");
      const auto now = freshness_instant();
      auto portfolio = context_.portfolio();
      // Every row this list owns comes out first, and the residual goes back in below.
      portfolio.working_.erase(
        std::remove_if(
          portfolio.working_.begin(), portfolio.working_.end(),
          [&](const WorkingOrder<Tag>& working) {
            for (const auto& entry : unconfirmed_) {
              if (is_reserved_row_of(working, entry)) {
                return true;
              }
            }
            return false;
          }
        ),
        portfolio.working_.end()
      );

      std::vector<bool> retire(unconfirmed_.size(), false);
      // Settled by history, or past the timeout with no history answer, and still not in a holdings
      // read taken a full timeout later: withheld, with what holdings did show credited to it.
      std::vector<bool> expired(unconfirmed_.size(), false);
      std::vector<bool> done(unconfirmed_.size(), false);
      for (std::size_t first = 0; first < unconfirmed_.size(); ++first) {
        if (done[first]) {
          continue;
        }
        const auto& instrument = unconfirmed_[first].instrument_;
        const auto direction = unconfirmed_[first].side_;
        std::vector<std::size_t> group;
        bool mixed = false;
        for (std::size_t at = first; at < unconfirmed_.size(); ++at) {
          if (unconfirmed_[at].instrument_ == instrument) {
            group.push_back(at);
            done[at] = true;
            mixed = mixed || unconfirmed_[at].side_ != direction;
          }
        }
        // `a` signed in the group's direction: what a buy adds to a long holding, a sell takes away.
        const auto signed_in = [&](const Unconfirmed& entry, const Decimal64& quantity) {
          return entry.side_ == direction ? quantity : decimal_subtract(zero, quantity).value_or(zero);
        };

        // What a listing still shows of each order: that part cannot have reached holdings.
        std::vector<Decimal64> listed(group.size(), zero);
        std::vector<Decimal64> fillable(group.size(), zero);
        for (std::size_t at = 0; at < group.size(); ++at) {
          auto& entry = unconfirmed_[group[at]];
          entry.listed_now_ = false;
          for (const auto& working : portfolio.working_) {
            if (!working.reserved_ && working.ref_.primary_ == entry.ref_.primary_ &&
                working.order_.instrument_ == entry.instrument_ && working.order_.side_ == entry.side_ &&
                strategy::order_is_open(working.order_.status_)) {
              entry.listed_ = true;
              entry.listed_now_ = true;
              listed[at] = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_)
                             .value_or(zero);
              break;
            }
          }
          const auto left = decimal_subtract(entry.expected_, listed[at]).value_or(zero);
          fillable[at] = decimal_less(zero, left) ? left : zero;
        }

        // The holdings movement since the group's baseline -- the first entry's, which a retirement
        // below advances -- in the group's direction.
        std::optional<Decimal64> moved;
        if (const auto held = portfolio.net_quantity_of(instrument)) {
          moved = direction == OrderSide::BUY ? decimal_subtract(*held, unconfirmed_[first].baseline_) :
                                                decimal_subtract(unconfirmed_[first].baseline_, *held);
        }
        std::vector<Decimal64> covered(group.size(), zero);
        if (moved && !mixed) {
          // Credited in the order they were sent, each up to the part no listing still shows.
          auto left = *moved;
          for (std::size_t at = 0; at < group.size() && decimal_less(zero, left); ++at) {
            covered[at] = decimal_less(left, fillable[at]) ? left : fillable[at];
            left = decimal_subtract(left, covered[at]).value_or(zero);
          }
        } else if (moved) {
          // Both sides: a movement could be either side's, so it counts only when it is exactly what
          // all of them would make. A resting order a listing shows in full adds nothing, so a
          // protective stop does not hold up the buy beside it.
          std::optional<Decimal64> total = zero;
          for (std::size_t at = 0; at < group.size() && total; ++at) {
            total = decimal_add(*total, signed_in(unconfirmed_[group[at]], fillable[at]));
          }
          // A sum of zero is no evidence: two opposite orders that have not filled at all look like
          // two that both have. They wait for their history, and once it has said what each one
          // filled, holdings that did not move are evidence after all.
          bool all_settled = true;
          for (const auto at : group) {
            all_settled = all_settled && unconfirmed_[at].settled_;
          }
          if (total && (total->coefficient() != 0 || all_settled) && !decimal_less(*total, *moved) &&
              !decimal_less(*moved, *total)) {
            covered = fillable;
          }
        }

        // Retired entries advance the baseline the rest are measured from, so the movement that
        // confirmed one is never counted again for the next.
        std::optional<Decimal64> advanced = unconfirmed_[first].baseline_;
        for (std::size_t at = 0; at < group.size(); ++at) {
          auto& entry = unconfirmed_[group[at]];
          if (!decimal_less(covered[at], entry.expected_)) {
            retire[group[at]] = true;
            if (advanced) {
              advanced = direction == OrderSide::BUY ?
                decimal_add(*advanced, signed_in(entry, entry.expected_)) :
                decimal_subtract(*advanced, signed_in(entry, entry.expected_));
            }
            continue;
          }
          auto residual = decimal_subtract(entry.expected_, covered[at]).value_or(zero);
          residual = decimal_subtract(residual, listed[at]).value_or(zero);
          if (!decimal_less(zero, residual)) {
            entry.residual_ = zero;
            entry.unexplained_since_.reset();
            // Explained again, so a later gap starts over: a fresh timeout and a fresh history read.
            entry.history_missed_ = false;
            continue;
          }
          entry.residual_ = residual;
          if (!entry.unexplained_since_) {
            entry.unexplained_since_ = now;
          }
          // Judged by the holdings read, not the clock: the listing (and so the history) is polled
          // before holdings in each iteration, and on a loop polling slower than the timeout the
          // clock alone marked a fill one holdings read before it would have shown.
          if ((entry.settled_ || entry.history_missed_) && timed_out(entry, now) &&
              portfolio.as_of_ - *entry.unexplained_since_ > confirmation_timeout_) {
            // A holdings answer read a whole timeout after the quantity became unexplained (or after the
            // history settled it) does not show it, and the history either said what it filled or had
            // nothing to say: an outside trade, a disagreement or an order nobody here can account for.
            // Withheld for the operator.
            ++orders_unconfirmed_timed_out_;
            ledger_.mark(
              entry.instrument_,
              entry.settled_ ?
                "an order this runner sent (" + entry.ref_.primary_ +
                  ") filled by the order history, and holdings did not show it within the confirmation "
                  "timeout" :
                "an order this runner sent (" + entry.ref_.primary_ +
                  ") was not confirmed by holdings, the order list or the order history in time"
            );
            expired[group[at]] = true;
            // What holdings did show of it stays credited to it, not to the next order.
            if (advanced) {
              advanced = direction == OrderSide::BUY ? decimal_add(*advanced, signed_in(entry, covered[at])) :
                                                       decimal_subtract(*advanced, signed_in(entry, covered[at]));
            }
            continue;
          }
          auto row = entry.row_;
          row.order_.quantity_ = residual;
          row.amendable_quantity_ = residual;
          portfolio.working_.push_back(std::move(row));
        }
        for (const auto at : group) {
          if (!retire[at] && advanced) {
            unconfirmed_[at].baseline_ = *advanced;
          }
        }
      }

      for (std::size_t at = unconfirmed_.size(); at-- > 0;) {
        if (retire[at] || expired[at]) {
          unconfirmed_.erase(unconfirmed_.begin() + static_cast<std::ptrdiff_t>(at));
        }
      }
      context_.set_portfolio(std::move(portfolio));
    }
  }

  /**
   * Carries this pass's reservation into the next one.
   *
   * **The in-pass view dies with the pass**, so without this a second pass before the next poll
   * starts from the broker's last word again and can re-spend what the first pass already
   * committed. The fix needs no matcher, because a working-order poll *replaces* what it
   * refreshes: `poll_working_orders` assigns `working_` wholesale, so an untracked reservation
   * written here is discarded the moment the broker's own listing arrives. (A holdings poll does not: it refreshes
   * positions and keeps `working_`.) Nothing has to recognise its own order in a listing -- which is
   * the half of reserve-and-match that carries a matcher's failure modes.
   *
   * **Always, for a runner that sends to a broker which lists working orders.** Target
   * reconciliation reads the same portfolio, and an order this runner just sent has to count as
   * working there, whatever the limits: a target restated on the next pass would otherwise be sent
   * again. It was scoped to the limits below until 2026-09-30, and the cold start showed the cost --
   * `SessionSource` then fed a whole history read one pass per bar before the next listing, so a
   * target strategy under default limits sent one order per history bar (`tests/test-live.cpp`,
   * `[own-orders]`). It now feeds history through `feed_warmup`, which sends nothing; the reservation
   * still covers every other pass before a listing (a runner fed by hand, two names in one bar
   * poll, or bar polls that come faster than the listing's). A listing and holdings bound its life
   * (`unconfirmed_orders`), so the loop holds such a run while no listing will come -- the source
   * publishes none, no working-order cadence is set, or the last listing is older than the portfolio
   * age policy (`PollingLoop::gate_for`).
   *
   * **Scoped to the limits that read it everywhere else.** A dry run, `live::bind_observer` and a
   * custom executor that reaches no broker write a row with no broker reference: a later target
   * that opposes it would ask to withdraw an order identified by nothing, and a dry run has no
   * listing to replace the row at all. A broker that publishes no working-order inquiry never
   * replaces it either. For them the reservation persists only for the limits that size against
   * what is outstanding, as before. `max_position_quantity_` is one of those limits: its ceiling
   * counts same-side working orders, so an accepted order that stopped being visible at the end of a
   * pass would let the next bar place another of the same size.
   *
   * A custom executor that reaches a broker but reports no reference (`reports_last_ref`) is still
   * counted as keeping its orders: a duplicate order is the worse failure, and the listing the loop
   * insists on retires the unaddressable row.
   *
   * A listing replaces `working_` wholesale, and so used to retire the row whether or not it carried the
   * order -- a market order that filled at once is in no listing, and holdings lagging the fill did not
   * show it either (lab F15). For a runner that keeps sent orders until listed, the row is now rebuilt
   * before every pass from what nothing explains yet (`track_unconfirmed`, `unconfirmed_orders`).
   */
  void persist_reservation(
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency,
    const std::optional<OrderRef<Tag>>& ref
  ) {
    const bool sells = guard_.limits().enforce_sellable_quantity_ && order.side_ == OrderSide::SELL;
    const bool buys = guard_.limits().enforce_buying_power_ && order.side_ == OrderSide::BUY;
    const bool capped = guard_.limits().max_position_quantity_.has_value();
    // The exposure ceiling reads what is outstanding for the same reason the position ceiling does,
    // so it needs the reservation for the same reason. Left out, a caller enabling only this limit
    // would get the within-pass fix and not the across-pass one: ten bought on each of two bars,
    // with no poll between, passes twice against a ceiling one of them should have reached. That is
    // precisely the omission `ai/pre-trade-checks` round 2 found for `max_position_quantity_`.
    const bool exposed = guard_.limits().max_gross_exposure_.has_value();
    // A stop is read by target reconciliation whatever the limits, which refuses a target while one
    // rests (`RejectReason::STOP_ORDER_RESTING`). Not persisted, a stop placed on this pass would be
    // invisible to a target on the next one until a working-order poll listed it -- and "flatten"
    // would then go out beside the stop, both of them sells.
    const bool conditional = carries_trigger_price(order.type_);
    // A broker that will list the order back, reached by this runner: the listing and holdings
    // account for the row (`track_unconfirmed`), so the reservation is always written. See above for
    // why every other runner keeps the scoping.
    if (!keeps_sent_orders_until_listed && !sells && !buys && !capped && !exposed && !conditional) {
      return;
    }
    auto persisted = context_.portfolio();
    reserve(
      persisted, order, reference_price, currency, ref,
      guard_.limits().enforce_buying_power_
    );
    context_.set_portfolio(std::move(persisted));
  }

  /**
   * Resolves what every instrument the exposure ceiling must value is denominated in, and marked at.
   *
   * **This is the half `RiskGuard` cannot do**, which is why the guard takes it as an input rather
   * than deriving it. A position states its own currency and last price; a working order on an
   * instrument with no position states neither, and an opening buy is exactly that case. The runner
   * is where reference data and the quote cache live, so it is where the lookup belongs -- the same
   * reasoning that puts `order_currency_` here.
   *
   * **An instrument this cannot resolve is left out, and leaving it out is how the refusal
   * happens.** `projected_gross_exposure` returns nothing when the basis does not cover something
   * the portfolio holds or has working, and the guard turns that into `VALUE_NOT_KNOWN`. Inventing
   * a currency or a price here would be the silent pass this whole branch is about.
   *
   * **Only fields somebody explicitly set are read**, which rules out the position's own
   * `market_value_.currency_` as a fallback however tempting it is. `Currency` default-constructs to
   * KRW and `HoldingPosition::last_price_` to zero, so an untouched position does not read as
   * unresolved -- it reads as Korean won marked at nothing, resolves, and contributes zero to the
   * sum. That is the silent omission this refusal exists to prevent, wearing the costume of an
   * answer. It is the same trap `backtest::ReplayLedger` was in.
   *
   * So: the currency comes from registered reference data, which is also where `order_currency_`
   * comes from, so the two cannot disagree about one instrument. The mark comes from the quote cache
   * or from a position's `last_price_` **when that price is not zero** -- a mark of zero is not a
   * valuation, and accepting one would value the position at nothing.
   *
   * The cost, and it is the intended direction: an account holding an instrument the caller never
   * registered refuses every order while the ceiling is on. Visible and diagnosable, rather than a
   * ceiling quietly measuring part of the portfolio.
   */
  std::vector<strategy::InstrumentBasis> exposure_basis_for(
    const strategy::Portfolio<Tag>& view,
    const std::vector<strategy::PlannedOrder>& pending
  ) const {
    std::vector<InstrumentId> instruments;
    const auto note = [&instruments](const InstrumentId& id) {
      for (const auto& seen : instruments) {
        if (seen == id) {
          return;
        }
      }
      instruments.push_back(id);
    };
    for (const auto& position : view.positions_) {
      // Flat rows carry no exposure and are skipped for the reason `projected_gross_exposure` gives:
      // a replay keeps a lot after it returns to flat, and requiring a mark for one would let a
      // closed position refuse orders in an instrument it has nothing to do with.
      if (position.quantity_.coefficient() == 0) {
        continue;
      }
      note(position.instrument_);
    }
    for (const auto& working : view.working_) {
      if (strategy::order_is_open(working.order_.status_)) {
        note(working.order_.instrument_);
      }
    }
    for (const auto& order : pending) {
      note(order.instrument_);
    }

    std::vector<strategy::InstrumentBasis> basis;
    basis.reserve(instruments.size());
    for (const auto& id : instruments) {
      const HoldingPosition* position = nullptr;
      for (const auto& held : view.positions_) {
        if (held.instrument_ == id) {
          position = &held;
          break;
        }
      }

      const auto reference = context_.instrument(id);
      if (!reference) {
        continue;
      }

      // **Positive, not merely non-zero.** `Context::set_last_price` stores whatever a bar or quote
      // carried, with no validation, so a malformed price reaches here from market data rather than
      // only from a caller mistake. A negative mark would make an instrument's exposure negative and
      // let it offset real holdings until the ceiling passed anything at all; zero values the
      // position at nothing, which is the silent omission this resolution exists to refuse. Either
      // leaves the instrument unresolved, and the guard turns that into `VALUE_NOT_KNOWN`.
      const auto zero = Decimal64::parse("0");
      const auto quoted = context_.last_price(id);
      // **A quote that is present and not a price leaves the instrument unresolved**, rather than
      // falling through to the position's older mark. Falling back would be the quiet outcome: the
      // ceiling would answer from a stale figure while the current quote said something impossible,
      // and nothing would report that the data was wrong. Absent is a different thing from invalid,
      // and only absent gets the fallback.
      if (quoted && !decimal_less(zero, *quoted)) {
        continue;
      }
      auto price = quoted;
      if (!price && position != nullptr && decimal_less(zero, position->last_price_)) {
        price = position->last_price_;
      }
      if (!price) {
        continue;
      }
      basis.push_back(strategy::InstrumentBasis{id, reference->currency_, *price});
    }
    return basis;
  }

  /** Writes a sellable quantity onto the position for `instrument`, when there is one. */
  static void write_sellable(
    strategy::Portfolio<Tag>& portfolio,
    const InstrumentId& instrument,
    const Decimal64& quantity
  ) {
    for (auto& position : portfolio.positions_) {
      if (position.instrument_ == instrument) {
        position.sellable_quantity_ = quantity;
        // **The inquiry is its own reading, so it carries its own basis -- on this row only.** The
        // holdings poll that built this portfolio established nothing about a figure it never
        // reported, and the inquiry answered for *this* instrument and no other. Writing the basis to
        // the portfolio would let one instrument's declaration certify every other row's figure: a
        // populated but undeclared quantity on another instrument would become enforceable, and a
        // wrong-sized sell of it would reach the wire. An executor that declares nothing leaves this
        // unestablished, and the guard then refuses.
        position.sellable_basis_ = runner_detail::sellable_inquiry_basis_of<Executor>();
        return;
      }
    }
  }

  /**
   * Records an accepted order in `view` so the rest of the pass sizes against it.
   *
   * Written as a working order because that is what it now is, and because the alternative --
   * adjusting the position -- would be a lie: nothing has filled.
   *
   * **It carries the reference the broker just issued**, and that is not cosmetic. No guard reads a
   * reference, but reconciliation does: a row opposing the next target is put on the cancel list by
   * its `ref_`, and a synthetic row with an empty one would have the runner ask a broker to
   * withdraw an order identified by nothing. The shape and the amendable quantity are filled for
   * the same reason -- `cancel_all` builds the instruction from them, and a broker that requires a
   * quantity would be sent zero. An order just accepted and unfilled is amendable in full.
   *
   * Where the executor could not supply a reference -- a dry run, a replay, a custom executor
   * that does not report one, or an order whose reply was lost (passed no reference on purpose: the
   * executor's last one belongs to an earlier order) -- the row is still written, because a limit
   * that ignored the order would be the worse failure. Whether such a row can still be withdrawn is the executor's
   * question, not this one's: a replay's engine matches a cancellation on instrument and side and
   * needs no reference at all, while `live::SessionExecutor` refuses to send one to a broker.
   *
   * `spends` decides whether a buy also draws down the published buying-power reading, and it is
   * the caller's buying-power check rather than anything this function can see. Review found the
   * two entangled: a caller who had enabled only the position ceiling watched
   * `portfolio().buying_power_in()` fall after a buy, which is a public figure a strategy is free
   * to size its next order from -- a behaviour change well past enforcing a ceiling.
   */
  static void reserve(
    strategy::Portfolio<Tag>& view,
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency,
    const std::optional<OrderRef<Tag>>& ref,
    bool spends
  ) {
    WorkingOrder<Tag> working;
    working.order_.instrument_ = order.instrument_;
    working.order_.side_ = order.side_;
    working.order_.status_ = OrderStatus::PENDING;
    working.order_.type_ = order.type_;
    working.order_.session_ = order.session_;
    // The venue this runner asked the order to be routed to, which is
    // `PlannedOrder::venue_` -- already resolved against the binding's default
    // route before the send -- and **not** the instrument's: the instrument's is
    // identity, and an order whose identity carried a route would stop matching
    // its own holding. A broker's own listing fills this field from what it
    // routed the order to; until one arrives, this row is what `cancel_all`
    // reads, and leaving it `UNSPECIFIED` would withdraw the order at the
    // broker's default venue rather than at the one it went to.
    working.order_.venue_ = order.venue_;
    working.order_.price_ = order.price_;
    // A stop's trigger, so the reserved row has the shape the broker's listing will report --
    // `cancel_all` restates it and `OrderUpdateWatch` compares it.
    working.order_.trigger_price_ = order.trigger_price_;
    working.order_.time_in_force_ = order.time_in_force_;
    working.order_.quantity_ = order.quantity_;
    working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    working.amendable_quantity_ = order.quantity_;
    if (ref) {
      working.ref_ = *ref;
    }
    // Marked as ours. A limit sizing against what is outstanding must count it; anything inferring
    // the broker's own behaviour from the listing must not, because this row is arithmetic rather
    // than something the broker said.
    working.reserved_ = true;
    view.working_.push_back(std::move(working));

    // A buy also spends money that the rest of the pass may not spend again. The estimate is the
    // same one the guard checked the order against -- its own price, or the last quote for a market
    // order -- and it is an *estimate*: a market order fills where it fills, and commission is not
    // in it. Under-reserving would be the dangerous direction, and neither of those makes the
    // reservation smaller than the order's stated cost.
    if (!spends || order.side_ != OrderSide::BUY || !currency) {
      return;
    }
    const auto price = order.price_ ? order.price_ : reference_price;
    if (!price) {
      return;
    }
    const auto spent = decimal_multiply(order.quantity_, *price);
    if (!spent) {
      return;
    }
    for (auto& reading : view.buying_power_) {
      if (!(reading.cash_.currency_ == *currency)) {
        continue;
      }
      if (const auto left = decimal_subtract(reading.cash_.amount_, *spent)) {
        reading.cash_.amount_ = *left;
      }
      return;
    }
  }

  /**
   * Returns why a plan derived from the current portfolio must not be acted on, or nothing.
   *
   * **The policy is evaluated here, against the snapshot in hand, rather than being pushed in as a
   * verdict somebody else computed earlier.** A `bool` set by the loop would be a judgement about
   * whatever portfolio was current when the loop last looked, and the loop looks at points a source
   * callback can get between: `PollingLoop::gate_for` calls `read_order_list` -- which is handed a
   * mutable runner and can feed -- before it ever reaches its own age check. A plan settled inside
   * that call would be checked against a stale verdict. Asking the question at settlement time makes
   * the answer structurally current, and there is no invalidation protocol to get wrong.
   */
  /**
   * Drops the deferred targets `instructions` supersede.
   *
   * Every pass, so a target or order the strategy states for an instrument -- in answer to an order
   * update, a bar, anything -- replaces the fallback before `retry_deferred` could release it. See
   * `deferred_targets` for the whole contract.
   */
  void drop_superseded_deferred(const std::vector<Instruction<Tag>>& instructions) {
    if (deferred_targets_.empty()) {
      return;
    }
    const auto drop = [this](const InstrumentId& instrument) {
      for (auto at = deferred_targets_.begin(); at != deferred_targets_.end(); ++at) {
        if (at->target_.instrument_ == instrument) {
          deferred_targets_.erase(at);
          return;
        }
      }
    };
    for (const auto& instruction : instructions) {
      if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
        drop(target->instrument_);
      } else if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
        drop(intent->instrument());
      }
    }
  }

  /**
   * The release loop behind `retry_deferred` and `retry_deferred_on_one_snapshot`. `evidence` is the
   * broker's history, or null for a caller whose own ledger decided every stop.
   */
  Report release_deferred(const std::vector<OrderSnapshot>* evidence) {
    bool released = false;
    for (auto at = deferred_targets_.begin(); at != deferred_targets_.end();) {
      if (stop_listed(at->target_.instrument_)) {
        ++at;
        continue;
      }
      if (evidence != nullptr) {
        bool decided = true;
        bool filled = false;
        for (const auto& stop : at->stops_) {
          const OrderSnapshot* row = nullptr;
          for (const auto& candidate : *evidence) {
            if (candidate.order_id_ == stop.primary_) {
              row = &candidate;
              break;
            }
          }
          if (row == nullptr) {
            decided = false;
            continue;
          }
          if (row->execution_.filled_quantity_.coefficient() != 0) {
            filled = true;
          } else if (strategy::order_is_open(row->status_)) {
            decided = false;
          }
        }
        if (filled) {
          ++deferred_dropped_after_fill_;
          at = deferred_targets_.erase(at);
          continue;
        }
        if (!decided) {
          ++at;
          continue;
        }
      }
      context_.submit(at->target_);
      at = deferred_targets_.erase(at);
      released = true;
    }
    // **Nothing released, nothing settled.** A pass has effects of its own -- it observes the
    // sellable convention from the portfolio -- and this runs on every working-order poll.
    if (!released) {
      return Report{};
    }
    // Exempt from `PORTFOLIO_SUPERSEDED` for this pass only; see `plan_withhold_reason`. Scoped, so a
    // settle that throws cannot leave the exemption on for every plan after it.
    struct Exempt {
      bool& flag_;
      explicit Exempt(bool& flag) : flag_(flag) { flag_ = true; }
      ~Exempt() { flag_ = false; }
      Exempt(const Exempt&) = delete;
      Exempt& operator=(const Exempt&) = delete;
    } exempt(releasing_deferred_);
    return settle();
  }

  /** Drops the retained target on `instrument`, if there is one. */
  void forget_retained(const InstrumentId& instrument) {
    if (instrument.symbol_.empty()) {
      return;
    }
    retained_targets_.erase(
      std::remove_if(
        retained_targets_.begin(), retained_targets_.end(),
        [&](const RetainedTarget& kept) { return kept.target_.instrument_ == instrument; }
      ),
      retained_targets_.end()
    );
    // Every kept intent for the instrument, not the first: a later statement supersedes all of them.
    kept_intents_.erase(
      std::remove_if(
        kept_intents_.begin(), kept_intents_.end(),
        [&](const KeptIntent& kept) { return kept.intent_.instrument() == instrument; }
      ),
      kept_intents_.end()
    );
  }

  /**
   * Keeps a target withheld for want of a trustworthy account, to plan again once there is one.
   *
   * Withheld because the account was never read, is too old, or predates the listing: each is a fact
   * about the data, and the next holdings read can change it. One per instrument, the latest kept, as
   * for `deferred_targets_`. Returns false, retaining nothing:
   *
   * - for anything but a `TargetPosition`. An `OrderIntent` states a quantity against the picture that
   *   was wrong, and replaying it later would act on a decision nobody re-made. (An intent the market
   *   phase stopped is a different case -- the picture was right, the hour was not -- and is kept by
   *   `keep_intent_for_phase`, with its holding checked at release.)
   * - while a stop, an operator's halt or a local failure holds. What a strategy says then -- `on_stop`
   *   included -- must not be replayed once the hold lifts.
   */
  bool retain_withheld(
    const Instruction<Tag>& instruction, bool for_phase = false,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    const auto* target = std::get_if<strategy::TargetPosition>(&instruction);
    if (target == nullptr) {
      return false;
    }
    if (guard_.halted(strategy::HaltReason::STOPPING) || guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return false;
    }
    // The latest statement wins, and so does its reason: a target retained for the account and then
    // restated and refused for the phase waits for the phase.
    for (auto& kept : retained_targets_) {
      if (kept.target_.instrument_ == target->instrument_) {
        kept = RetainedTarget{*target, for_phase, after};
        return true;
      }
    }
    retained_targets_.push_back(RetainedTarget{*target, for_phase, after});
    return true;
  }

  /**
   * Keeps a target whose order the market phase stopped, for the first permitted phase.
   *
   * Two routes reach it, both only for a runner that sends to a broker: the phase limit refused an
   * order that opens or grows a position, or it admitted one that reduces a position outside the
   * phase and the broker (or the binding) refused it -- which is what a broker not taking orders at
   * midnight does. See `retry_retained_targets`.
   */
  void retain_for_phase(
    const Instruction<Tag>& instruction, bool broker_answered = false,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    if constexpr (runner_detail::sends_to_a_broker<Executor>::value) {
      if (retain_withheld(instruction, /*for_phase=*/true, after)) {
        return;
      }
      keep_intent_for_phase(instruction, broker_answered, after);
    } else {
      (void)instruction;
      (void)broker_answered;
      (void)after;
    }
  }

  /**
   * Whether the error behind the placement just refused satisfies `predicate` (`broker_unreachable`,
   * `broker_rate_limited`).
   *
   * Read immediately after the send, which is the only moment `last_error()` is this order's. An
   * executor that states no error has said nothing about why, so its refusals stay final.
   */
  template <typename Predicate>
  bool last_error_is(Predicate predicate) const {
    if constexpr (runner_detail::states_last_error<Executor>::value) {
      const auto& error = executor_.last_error();
      return error && predicate(*error);
    } else {
      (void)predicate;
      return false;
    }
  }

  /**
   * Keeps an order intent whose order the market phase stopped, for the first permitted phase.
   *
   * The intent half of `retain_for_phase`: a daily strategy's protective stop, decided on the bar fed
   * at midnight and refused by a broker not taking orders then, would otherwise be restated at the
   * next midnight and refused again, and the position would never be protected live. Kept once per
   * instruction (one instruction can produce two refused orders), in the order stated, with the
   * instrument's holding at this moment, which `retry_retained_targets` checks before releasing it;
   * any intent carrying a trigger, a stop buy as much as a stop sell, is also dropped there when a stop
   * rests on the instrument. **Not** while a stop, an operator's halt or a local failure holds; **not** an intent carrying an
   * idempotency key that a broker answered, because a second order would reuse the key.
   */
  void keep_intent_for_phase(
    const Instruction<Tag>& instruction, bool broker_answered,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    const auto* intent = std::get_if<strategy::OrderIntent>(&instruction);
    if (intent == nullptr) {
      return;
    }
    if (guard_.halted(strategy::HaltReason::STOPPING) || guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return;
    }
    if (broker_answered && intent->client_order_id()) {
      return;
    }
    for (const auto* seen : kept_this_pass_) {
      if (seen == &instruction) {
        return;
      }
    }
    const auto held = context_.portfolio().net_quantity_of(intent->instrument());
    if (!held) {
      return;
    }
    kept_this_pass_.push_back(&instruction);
    kept_intents_.push_back(KeptIntent{*intent, *held, after});
  }

  /** Returns whether the portfolio lists an open stop order on `instrument`. */
  bool stop_listed(const InstrumentId& instrument) const {
    for (const auto& working : context_.portfolio().working_) {
      if (working.order_.instrument_ == instrument &&
          strategy::order_is_open(working.order_.status_) &&
          strategy::is_stop_order(working.order_)) {
        return true;
      }
    }
    return false;
  }

  /** Keeps a target whose plan is waiting on a stop's withdrawal; see `deferred_targets`. */
  void defer_if_waiting_on_stop(
    const Instruction<Tag>& instruction,
    const strategy::Plan<Tag>& planned
  ) {
    const auto* target = std::get_if<strategy::TargetPosition>(&instruction);
    if (target == nullptr) {
      return;
    }
    for (const auto& rejection : planned.rejected_) {
      if (rejection.reason_ != strategy::RejectReason::STOP_ORDER_RESTING) {
        continue;
      }
      // The stops it waits on, as the listing shows them now -- `PENDING_CANCEL` included, since that
      // is exactly a stop whose outcome is not yet known.
      DeferredTarget deferred;
      deferred.target_ = *target;
      deferred.deferred_at_ = context_.now();
      for (const auto& working : context_.portfolio().working_) {
        if (working.order_.instrument_ == target->instrument_ &&
            strategy::order_is_open(working.order_.status_) &&
            strategy::is_stop_order(working.order_)) {
          deferred.stops_.push_back(working.ref_);
        }
      }
      for (auto& kept : deferred_targets_) {
        if (kept.target_.instrument_ == target->instrument_) {
          kept = std::move(deferred);
          ++targets_deferred_;
          return;
        }
      }
      deferred_targets_.push_back(std::move(deferred));
      ++targets_deferred_;
      return;
    }
  }

  std::optional<WithheldPlanReason> plan_withhold_reason(bool would_cancel) const {
    const auto& portfolio = context_.portfolio();
    // **A picture the library can prove is mis-ordered, so no opt-out reaches it.** The opt-outs below
    // accept an account nobody read or one read too long ago; this one was read, recently, and before
    // the listing it would be reconciled with -- which is the order in which a fill between the two
    // reads makes a position vanish. Exempt only while a deferred target is being released: that
    // release fires on the stop's own history showing it ended unfilled, so the listing change behind
    // it is by construction not a fill.
    if (!releasing_deferred_ && portfolio.listing_newer_than_holdings()) {
      return WithheldPlanReason::PORTFOLIO_SUPERSEDED;
    }
    // A default-constructed `Portfolio` carries the epoch, and no broker reports a snapshot taken in
    // 1970. The same sentinel the loop uses; both writers of `as_of_` in this library stamp it.
    const bool never_read = portfolio.as_of_ == std::chrono::system_clock::time_point{};
    const bool too_old = !never_read && maximum_portfolio_age_ &&
      freshness_instant() - portfolio.as_of_ > *maximum_portfolio_age_;

    if (never_read || too_old) {
      // **A plan that would withdraw something is stopped whatever was opted out of.** The opt-outs
      // say the operator accepts *sizing* against an account this library cannot vouch for. They do
      // not say it should withdraw orders it cannot account for, and the two are not the same
      // concession: an account that merely looks flat makes a resting protective order look opposed.
      if (would_cancel) {
        return never_read ? WithheldPlanReason::PORTFOLIO_ABSENT : WithheldPlanReason::PORTFOLIO_STALE;
      }
      // A placement is the half the opt-outs are about, and it is also the half a driving loop's
      // hold would otherwise catch -- so it is stopped here only for a run that asked for the
      // account to be known.
      if (require_known_portfolio_ && !(never_read && allow_absent_portfolio_)) {
        return never_read ? WithheldPlanReason::PORTFOLIO_ABSENT : WithheldPlanReason::PORTFOLIO_STALE;
      }
      // **Falls through rather than returning.** An opted-out placement still has to face the hold
      // check below: returning early here was a defect, because it let a plan out under an operator's
      // halt on the strength of an unrelated opt-out.
    }
    // Any placement hold, not an enumerated few: the harm is that the replacement will be refused
    // while the withdrawal has already gone, and every reason that refuses a placement does that.
    if (guard_.blocking_reason()) {
      return WithheldPlanReason::PLACEMENT_HELD;
    }
    return std::nullopt;
  }

  /**
   * Returns the instant a snapshot's age is measured against.
   *
   * The later of the decision instant and the last clock reading a loop supplied, so neither can be
   * used to make an old snapshot look fresh: a late bar cannot rewind the clock, and a loop that has
   * not spoken yet cannot erase the decision instant.
   */
  std::chrono::system_clock::time_point freshness_instant() const {
    return observed_at_ > context_.now() ? observed_at_ : context_.now();
  }

  /** Returns the detail recorded with a withheld plan, naming the hold where there is one. */
  std::string withhold_detail(WithheldPlanReason reason) const {
    if (reason != WithheldPlanReason::PLACEMENT_HELD) {
      return {};
    }
    const auto holding = guard_.blocking_reason();
    return holding ? std::string(strategy::halt_reason_name(*holding)) : std::string{};
  }

  /** Returns the instrument an instruction is about, for reporting a plan that was not acted on. */
  static InstrumentId instruction_instrument(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->instrument_;
    }
    if (const auto* order = std::get_if<strategy::OrderIntent>(&instruction)) {
      return order->instrument();
    }
    return InstrumentId{};
  }

  /** Returns the tag an instruction carries, so a withheld plan can be attributed to it. */
  static std::string instruction_tag(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->tag_;
    }
    if (const auto* order = std::get_if<strategy::OrderIntent>(&instruction)) {
      return order->tag();
    }
    return {};
  }

  void send_cancel(const OrderRef<Tag>& ref, Report& report) {
    // Gated like a placement, and in the same order. A cancellation is the one instruction that
    // cannot make an unresolved *position* worse, which is why the ledger does not gate it -- but it
    // still goes through the journal, and a journal that cannot record it must not let it be sent
    // unrecorded.
    if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      ++instructions_unavailable_;
      report.unavailable_cancel_.push_back(ref);
      return;
    }
    // And a halt stops this runner *sending*, which a cancellation is -- but only a halt that is
    // about this instruction. The argument for refusing here is `StopIntent` reaching this runner
    // from a signal and from a file: exempting cancellations wholesale would let
    // `echo withdraw > /var/run/atl/stop` send an instruction that an operator's own halt forbade,
    // and a kill switch a control file can partly bypass is not one. An operator who wants out
    // resumes first, which is one deliberate act and is visible in the report either way.
    //
    // **That argument covers the holds somebody set, and not the holds that merely mean this runner
    // is uninformed.** Refusing to withdraw a resting order because the *holdings* poll failed is
    // worse than the exposure it guards: the order keeps resting and can fill, and the reference it
    // would be withdrawn by came from a working-order read that succeeded. So the question asked is
    // `InstructionClass::WITHDRAWAL`, and `strategy::halt_blocks` holds the division and its
    // reasoning. The row is still resolved below, and a reference the snapshot cannot place is
    // reported rather than sent blind.
    if (const auto holding =
          guard_.blocking_reason_for(strategy::InstructionClass::WITHDRAWAL)) {
      ++cancels_refused_;
      report.refused_cancel_.push_back(RefusedCancel<Tag>{ref, *holding});
      return;
    }
    // Resolved against the portfolio first, because building a cancellation needs the order's own
    // quantity on a broker that requires one. A reference not in the snapshot means the snapshot is
    // stale, and sending a cancellation for an order the runner cannot describe is exactly the kind
    // of blind instruction that should be visible instead.
    const auto* working = context_.portfolio().find_working(ref);
    if (working == nullptr) {
      ++cancels_unresolved_;
      report.unresolved_cancel_.push_back(ref);
      return;
    }
    const auto outcome = executor_.cancel(*working);
    note_executor_failure(outcome == SendOutcome::UNAVAILABLE);
    // Switched over rather than tested one outcome at a time, deliberately. Three rounds of review
    // found three different outcomes falling through to `would_cancel_` -- the bucket that says the
    // broker withdrew the order -- because each fix handled the case it was told about and left the
    // rest. A switch with no default makes the next added enumerator a compile error under
    // `-Werror`, which is the only version of this that stays fixed.
    switch (outcome) {
      case SendOutcome::ACCEPTED:
        // The broker took the request. Whether the order stops working is a later fact, which only
        // its own listing reports: cancellation is asynchronous, and a broker that answers here can
        // still reject the withdrawal afterwards.
        //
        // **Counted against the rate limit, like a placement.** A withdrawal is a request the
        // broker counted, and a window that does not see it is not the window the operator
        // configured -- a `WITHDRAW` over thirty resting orders is thirty requests the guard was
        // blind to, and the next placement is then checked against a window it believes is empty.
        // It is still never *refused* by that limit: `send_cancel` calls no `check`, because a rate
        // limit must never be the reason an order cannot be withdrawn.
        guard_.note_sent(context_.now());
        // A sent order still counted may have filled before the withdrawal; its history decides.
        note_withdrawal_accepted(ref);
        report.would_cancel_.push_back(ref);
        return;
      case SendOutcome::REJECTED:
        // The broker refused the request, which is an answer: nothing needs reconciling. It is not
        // a withdrawal that happened, and it is not proof the order is working either -- a fill is
        // one of the ordinary reasons a withdrawal is refused. It still spent a request.
        guard_.note_sent(context_.now());
        ++cancels_rejected_;
        report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/true});
        return;
      case SendOutcome::INDETERMINATE:
        // Marked *and* reported apart from `would_cancel_`. The mark was always here; reporting it
        // as a withdrawal that happened was not right beside it, because the order may still be
        // resting and a caller reading `would_cancel_` would believe it is not.
        ledger_.mark(working->order_.instrument_, "a cancellation request failed in transit");
        // Counted like an acceptance, for the same reason a placement is: the request may well have
        // reached the broker, and a window that assumes it did not is the one that under-counts.
        guard_.note_sent(context_.now());
        ++cancels_indeterminate_;
        report.indeterminate_cancel_.push_back(ref);
        return;
      case SendOutcome::REFUSED_LOCALLY:
        // No `note_sent`, for the reason the placement path gives: nothing was sent, so the broker
        // counted nothing, and a rate limit must never become the reason a later order cannot go.
        // Reported beside the broker's own refusals because what it means for the order is the
        // same -- it was not withdrawn -- while `by_broker_` on the entry says which of the two
        // it was. Not `last_error()`: a later instruction in the same pass clears that.
        ++cancels_rejected_;
        report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/false});
        return;
      case SendOutcome::UNAVAILABLE:
        // Not reported as a withdrawal that happened, because it did not. A cancellation nobody
        // sent must not read as one the broker has.
        ++instructions_unavailable_;
        report.unavailable_cancel_.push_back(ref);
        return;
    }
  }

  /**
   * Plans a weight-sized intent as the target position it states (`strategy::weight_target`).
   *
   * **A resting stop refuses it, and nothing is withdrawn.** Target reconciliation withdraws a
   * resting stop when a trade is needed and waits for the stop's own history before trading, which
   * only a `TargetPosition` in the instruction stream can do (`defer_if_waiting_on_stop`); a weight
   * intent is not one, and an intent has never touched a stop. So a weight on an instrument with a
   * stop resting is refused `STOP_ORDER_RESTING` and the stop stays -- unless the holding already
   * meets it, when nothing is traded or withdrawn at all.
   */
  strategy::Plan<Tag> plan_weight(
    const strategy::OrderIntent& intent,
    const strategy::ReconcileInputs& inputs
  ) const {
    strategy::Plan<Tag> plan;
    const auto converted = strategy::weight_target(intent, inputs);
    if (!converted.target_) {
      plan.rejected_.push_back(strategy::Rejection{
        converted.refused_.value_or(strategy::RejectReason::NOT_REPRESENTABLE), intent.instrument(),
        intent.tag()});
      return plan;
    }
    if (stop_listed(intent.instrument())) {
      // Met already: nothing to trade, and the stop protects what the weight means to hold, which
      // is `STOP_ORDER_RESTING`'s own rule for a met target. Unlike a target, nothing else on the
      // instrument is withdrawn either.
      const auto held = context_.portfolio().net_quantity_of(intent.instrument());
      const auto gap = held ? decimal_subtract(*held, converted.target_->target_quantity_) : std::nullopt;
      if (gap && gap->coefficient() == 0) {
        return plan;
      }
      plan.rejected_.push_back(strategy::Rejection{
        strategy::RejectReason::STOP_ORDER_RESTING, intent.instrument(), intent.tag()});
      return plan;
    }
    return strategy::reconcile(*converted.target_, context_.portfolio(), inputs);
  }

  /**
   * Reconciles one queued instruction against the current portfolio.
   *
   * The reference data is held in a local for the duration of the call, because
   * `ReconcileInputs` borrows it rather than copying: an `Instrument` carries a tick table, and
   * copying one per instruction per bar would be paying repeatedly for a value that does not
   * change.
   */
  strategy::Plan<Tag> plan_for(const Instruction<Tag>& instruction) {
    const auto id = instrument_of(instruction);
    const auto reference = context_.instrument(id);
    const auto band = band_for(id);

    strategy::ReconcileInputs inputs;
    inputs.instrument_ = reference ? &*reference : nullptr;
    inputs.reference_price_ = context_.last_price(id);
    inputs.limits_ = band.applied_;
    inputs.portfolio_value_ = portfolio_value_;
    inputs.portfolio_value_currency_ = portfolio_value_currency_;
    inputs.tolerance_ = tolerance_;

    auto plan = [&] {
      if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
        return strategy::reconcile(*target, context_.portfolio(), inputs);
      }
      if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
        // **A weight is a holding, so it is planned as the position it states.** Reconciled as an
        // order it would buy the whole allocation again at every rebalance, which is what
        // `SizeKind::TARGET_WEIGHT` promises it does not do; as a target it is diffed against the
        // position and the working orders, like any target.
        if (intent->size().kind() == strategy::SizeKind::TARGET_WEIGHT) {
          return plan_weight(*intent, inputs);
        }
        return strategy::reconcile(*intent, context_.portfolio(), inputs);
      }
      return strategy::Plan<Tag>{};
    }();

    // Counted per plan rather than per order, and counted even when the plan placed nothing: the
    // figure answers "how often did this run decline to use a band it was holding", which is a fact
    // about the policy. Whether it cost anything is the other counter's question.
    if (band.withheld_) {
      ++price_bands_withheld_;
      mark_withheld_band(plan, *band.withheld_);
    }
    return plan;
  }

  /**
   * Records what the broker said about `order`, where the executor can say.
   *
   * Called at the `REJECTED` outcome and nowhere else, which is the only moment the answer is this
   * order's: `SessionExecutor::last_error()` is cleared by the next accepted instruction in the same
   * pass, so reading it later would attribute somebody else's silence to this refusal -- or nothing
   * at all.
   */
  void note_broker_refusal(const strategy::PlannedOrder& order) {
    if constexpr (runner_detail::states_last_error<Executor>::value) {
      const auto& error = executor_.last_error();
      if (!error) {
        // **Cleared, not left alone.** An executor may answer `REJECTED` and state nothing; the
        // counter still moves, and a record kept from an *earlier* refusal would then be handed to
        // the next run as though it described this one -- the wrong instrument under the wrong tag.
        // `SessionExecutor` always states something on this path, so this is about a binding
        // somebody else writes: the contract is "the last refusal, or nothing", never "the last
        // refusal that happened to come with words".
        last_broker_refusal_.reset();
        return;
      }
      BrokerRefusal refusal;
      refusal.code_ = error->broker_code_;
      refusal.message_ = error->message_;
      refusal.instrument_ = order.instrument_;
      refusal.tag_ = order.tag_;
      last_broker_refusal_ = std::move(refusal);
    } else {
      static_cast<void>(order);
    }
  }

  /** Returns which instrument a queued instruction concerns. */
  static InstrumentId instrument_of(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->instrument_;
    }
    if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
      return intent->instrument();
    }
    return InstrumentId{};
  }

  /** A band and what its age is measured from; see `set_price_limits`. */
  struct DatedBand {
    /** The band as the broker published it. */
    PriceLimits limits_;
    /** When this runner was told about it, which is all an unstamped band has. */
    std::chrono::system_clock::time_point received_at_{};
    /**
     * Returns the instant the band's age is measured from; see `set_price_limits`.
     *
     * **The exchange's stamp may only make a band look older, never fresher.** It is there to catch
     * a successful fetch of a week-old band, which is an ageing question; a stamp *later* than the
     * moment this process received it answers nothing -- it is two clocks disagreeing, by ordinary
     * skew at best and by a broker's bug at worst -- and taking it at face value would let a band
     * date itself into the future and outlive any stated maximum. Capping it at the receipt instant
     * keeps the case it exists for and gives the other one no purchase, with no invented tolerance
     * for how far apart two clocks may be.
     */
    std::chrono::system_clock::time_point as_of() const noexcept {
      if (!limits_.exchange_time_) {
        return received_at_;
      }
      return *limits_.exchange_time_ < received_at_ ? *limits_.exchange_time_ : received_at_;
    }
  };
  /** Which band a plan may use, and which it was denied; see `band_for`. */
  struct BandDecision {
    /** The band to clamp with, when there is one this runner will stand behind. */
    std::optional<PriceLimits> applied_;
    /** The band that was held back, when one was, so the decision can be counted. */
    std::optional<PriceLimits> withheld_;
  };

  /**
   * Returns the band to apply to `id`'s prices, and separately the one that was held back.
   *
   * Exactly one of the two is filled when a band is registered, and neither when none is. The
   * withheld half exists so the decision can be counted and reported: a band silently dropped is
   * the same failure as a band silently applied.
   */
  BandDecision band_for(const InstrumentId& id) const {
    BandDecision decision;
    const auto found = price_limits_.find(id);
    if (found == price_limits_.end()) {
      return decision;
    }
    if (!maximum_price_limits_age_ && !allow_unbounded_price_limits_age_) {
      // No policy stated. Not applied, and deliberately not the same as an absent band: the caller
      // asked for clamping and is getting none, which `price_bands_withheld` reports and a driving
      // loop names in its notes.
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    // **A band dated after the instant it is being aged at is not applied, whatever the policy.**
    // `as_of` can no longer exceed the receipt stamp, so the only way here is a caller moving
    // `set_observed_at` backwards -- reporting day 20, registering a band, then reporting day 10.
    // The subtraction below would then be negative and pass every positive maximum, so a band from
    // the caller's own future would clamp. A duration this runner cannot compute is not an age, and
    // a band whose age is unknown is the case the whole policy exists for.
    if (found->second.as_of() > freshness_instant()) {
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    if (maximum_price_limits_age_ &&
        freshness_instant() - found->second.as_of() > *maximum_price_limits_age_) {
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    decision.applied_ = found->second.limits_;
    return decision;
  }

  /**
   * Marks the orders in `plan` whose price the band that was **not** applied would have moved.
   *
   * A statement about the planned price, not a prediction about the wire: clamping happens before
   * tick rounding (`reconcile_detail::prepare_price`), so a bound that binds here could in principle
   * round back to the same price. What the flag says is the checkable thing -- this order's price
   * lies outside the withheld band on the side the clamp corrects.
   */
  static void mark_withheld_band(strategy::Plan<Tag>& plan, const PriceLimits& withheld) {
    for (auto& order : plan.place_) {
      if (!order.price_) {
        continue;
      }
      bool would_move = false;
      static_cast<void>(strategy::reconcile_detail::clamp_to_limits(
        *order.price_, order.side_, std::optional<PriceLimits>(withheld), would_move
      ));
      order.price_would_have_been_clamped_ = would_move;
    }
  }

  Strategy strategy_;
  strategy::RiskGuard guard_;
  /** Placements sent that may have left an order resting; see `placements_at_broker`. */
  std::size_t placements_at_broker_{0};
  /** Orders sent under a phase the limits did not permit; see `sends_outside_permitted_phase`. */
  std::size_t sends_outside_permitted_phase_{0};
  /** How old a snapshot may be before a plan derived from it is withheld; see the setter. */
  std::optional<std::chrono::nanoseconds> maximum_portfolio_age_;
  /** The last wall-clock instant a driving loop reported; see `set_observed_at`. */
  std::chrono::system_clock::time_point observed_at_{};
  /** Whether the caller stated this runner may act with no snapshot at all; see the setter. */
  bool allow_absent_portfolio_{false};
  /**
   * Whether a placement needs a portfolio that has actually been read; see the setter.
   *
   * Defaulted from the executor: a runner that can reach a broker requires it, and one that cannot
   * does not. That is what makes the rule hold for a live runner assembled by hand rather than only
   * for one the binding factories produced.
   */
  bool require_known_portfolio_{runner_detail::sends_to_a_broker<Executor>::value};
  bool probing_sellable_{false};
  strategy::SellableConventionProbe<Tag> sellable_probe_;
  Context context_;
  /** Records the first local failure, keeping it rather than overwriting with a later one. */
  void note_local_failure(std::string detail) {
    // The *first* reason is kept, not the latest: a storage failure cascades, and the reason the
    // cascade started is the one an operator needs. Taking the hold again would only replace it.
    if (!guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      guard_.halt(strategy::HaltReason::LOCAL_FAILURE, std::move(detail));
    }
  }

  /**
   * Makes the executor's own local failure sticky on the runner, if it reported one.
   *
   * Called after every send rather than only after a refusal, because the two are not the same
   * event. A *pre-send* journal write that fails produces `UNAVAILABLE` and nothing was sent. A
   * *terminal* journal write that fails happens after the broker has already answered: the send
   * really did happen, so the outcome passed back stays the broker's, and only this call records
   * that the journal is no longer trustworthy.
   */
  void note_executor_failure(bool refused) {
    const auto failures = executor_journal_failures();
    if (failures > acknowledged_journal_failures_) {
      auto detail = executor_journal_error();
      note_local_failure(
        detail.empty() ? std::string("the order journal could not be written") : std::move(detail)
      );
      return;
    }
    if (refused && failures == 0) {
      // An executor with no journal behind it can still refuse locally, and it reports no count.
      // Nothing to acknowledge, so a refusal always halts and a resume that does not fix the cause
      // simply halts again on the next send -- which is the truthful outcome.
      note_local_failure("local machinery refused to send");
    }
  }

  /** Returns how many journal writes the executor has had fail, or zero when it cannot say. */
  std::size_t executor_journal_failures() const { return executor_journal_failures_impl(0); }

  template <typename E = Executor>
  auto executor_journal_failures_impl(int) const
    -> decltype(std::declval<const E&>().journal_failures()) {
    return executor_.journal_failures();
  }

  std::size_t executor_journal_failures_impl(...) const { return 0; }

  /** Returns what the executor says went wrong, or empty when it reports nothing or cannot. */
  std::string executor_journal_error() const { return executor_journal_error_impl(0); }

  template <typename E = Executor>
  auto executor_journal_error_impl(int) const
    -> decltype(std::declval<const E&>().last_journal_error(), std::string()) {
    return executor_.last_journal_error();
  }

  std::string executor_journal_error_impl(...) const { return {}; }

  Executor executor_;
  IndeterminacyLedger ledger_;
  std::size_t acknowledged_journal_failures_{0};
  std::map<InstrumentId, DatedBand> price_limits_;
  /** How old a band may be before it is not applied; see `set_maximum_price_limits_age`. */
  std::optional<std::chrono::nanoseconds> maximum_price_limits_age_;
  /** Whether the caller stated this runner applies a band of any age; see the setter. */
  bool allow_unbounded_price_limits_age_{false};
  /** Plans built while a band was held and not applied; see `price_bands_withheld`. */
  std::size_t price_bands_withheld_{0};
  /** Sent orders a broker refused after a withheld band would have moved them; see the accessor. */
  std::size_t unclamped_orders_rejected_{0};
  /** Orders nothing sent to a broker while carrying an unclamped price; see the accessor. */
  std::size_t unclamped_orders_refused_{0};
  /** Instructions that failed by each origin, and refusals a broker made; see the accessors. */
  std::size_t orders_unbuildable_{0};
  /** @copydoc Runner::orders_unbuildable_ */
  std::size_t orders_reconcile_withheld_{0};
  /** @copydoc Runner::orders_unbuildable_ */
  std::size_t orders_guard_refused_{0};
  /** @copydoc Runner::orders_unbuildable_ */
  std::size_t orders_binding_refused_{0};
  /** @copydoc Runner::orders_unbuildable_ */
  std::size_t orders_broker_refused_{0};
  /** @copydoc Runner::orders_unbuildable_ */
  std::size_t orders_indeterminate_{0};
  /** Withdrawals that failed, by origin; see `cancels_rejected`. */
  std::size_t cancels_rejected_{0};
  /** @copydoc Runner::cancels_rejected_ */
  std::size_t cancels_refused_{0};
  /** @copydoc Runner::cancels_rejected_ */
  std::size_t cancels_unresolved_{0};
  /** @copydoc Runner::cancels_rejected_ */
  std::size_t cancels_indeterminate_{0};
  /** Plans held back whole, for any of three reasons; see `plans_withheld`. */
  std::size_t plans_withheld_{0};
  std::size_t warmup_instructions_discarded_{0};
  /** One target waiting for a stop's withdrawal, and when a listing first showed the stop gone. */
  struct DeferredTarget {
    /** The target, exactly as the strategy stated it. */
    strategy::TargetPosition target_;
    /** The stops it waits on, whose own history rows decide its release. */
    std::vector<OrderRef<Tag>> stops_;
    /** When it was deferred, which bounds the history that can hold those rows. */
    std::chrono::system_clock::time_point deferred_at_{};
  };
  /** Targets waiting for a stop's withdrawal; see `deferred_targets`. */
  std::vector<DeferredTarget> deferred_targets_;

  std::vector<Unconfirmed> unconfirmed_;
  std::chrono::nanoseconds confirmation_timeout_{std::chrono::minutes(10)};
  std::size_t orders_unconfirmed_timed_out_{0};
  /** Targets withheld for want of a trustworthy account, one per instrument; see `retry_retained_targets`. */
  /** A retained target, and whether it waits for the market phase rather than for the account. */
  struct RetainedTarget {
    strategy::TargetPosition target_;
    bool for_phase_{false};
    /** Kept because the broker could not be reached: planned again only after a later holdings read. */
    std::optional<std::chrono::system_clock::time_point> after_;
  };
  std::vector<RetainedTarget> retained_targets_;
  /** An order intent kept for the phase, and the instrument's net holding when it was kept. */
  struct KeptIntent {
    strategy::OrderIntent intent_;
    Decimal64 held_;
    /** As for `RetainedTarget::after_`. */
    std::optional<std::chrono::system_clock::time_point> after_;
  };
  std::vector<KeptIntent> kept_intents_;
  std::size_t kept_intents_dropped_{0};
  /** The instructions kept for the phase in the current pass, so each is kept once. */
  std::vector<const Instruction<Tag>*> kept_this_pass_;
  /** True only inside a deferred release's settle, which `plan_withhold_reason` exempts. */
  bool releasing_deferred_{false};
  /** How many were ever deferred; see `targets_deferred`. */
  std::size_t targets_deferred_{0};
  /** How many were dropped after a fill; see `deferred_dropped_after_fill`. */
  std::size_t deferred_dropped_after_fill_{0};
  /** Instructions local machinery was unavailable to send; see the accessor. */
  std::size_t instructions_unavailable_{0};
  /** What the broker said about the most recent order it refused; see the accessor. */
  std::optional<BrokerRefusal> last_broker_refusal_;
  std::optional<strategy::Rejection> last_unbuildable_;
  std::optional<Decimal64> portfolio_value_;
  std::optional<Currency> portfolio_value_currency_;
  strategy::TradeTolerance tolerance_;
};

/**
 * A runner with no broker attached: handlers fire, instructions are recorded, nothing is sent.
 *
 * The plan's `DryRunner`, and now one configuration of the shared pipeline rather than a separate
 * implementation. That matters beyond tidiness: a dry run that ran different code from a live
 * session would be testing something other than what ships.
 *
 * **No adapter code exists in the binary.** This header reaches no broker and no transport header,
 * so a dry-run program instantiates none of either -- confirmed by symbol inspection of an installed
 * consumer, which finds no `algotradelib::network`, `algotradelib::kis` or
 * `algotradelib::tossinvest` symbol. There is therefore no instantiated code path from a dry run to
 * a market, which is a stronger guarantee than a runtime flag.
 *
 * It does not mean libssl is off the link line: the package exports one interface target carrying
 * the transport dependencies for every consumer. See `plan/todo/offline-only-target.md`.
 */
template <typename Strategy, typename CapsType = strategy::NoCaps>
using DryRunner = Runner<Strategy, CapsType, DryTag, RecordingExecutor<DryTag>>;

} // namespace runner
} // namespace algotradelib
