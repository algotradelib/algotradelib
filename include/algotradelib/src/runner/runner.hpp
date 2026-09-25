#pragma once

#include "./context.hpp"
#include "./dispatch.hpp"
#include "./execution.hpp"

#include "../strategy/reconcile.hpp"
#include "../strategy/risk.hpp"
#include "../strategy/sellable_convention.hpp"

#include <algotradelib/core>

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
  OrderRef<BrokerTag> ref_;
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
  /** Instructions a guard refused, with the limit and what was attempted. */
  std::vector<strategy::GuardRejection> refused_;
  /**
   * Orders the runner declined to send because the instrument's state is unresolved.
   *
   * Reported rather than retried. A retry is exactly what must not happen after a transport
   * failure, so these are surfaced for an operator and left alone.
   */
  std::vector<WithheldOrder> withheld_;
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
   * Never a broker's answer. The order journal could not be written, and so nothing was attempted.
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
    return would_place_.empty() && would_cancel_.empty() && rejected_.empty() &&
      refused_.empty() && withheld_.empty() && indeterminate_.empty() &&
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
  using Tag = BrokerTag;
  using Context = runner::Context<BrokerTag, CapsType>;
  using Report = DryRunReport<BrokerTag>;

  ATL_ASSERT_STRATEGY(Strategy, Context);

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
  Executor& executor() noexcept { return executor_; }

  /**
   * Returns the ledger of instruments whose order state is unresolved.
   *
   * Non-const so an operator, having read the broker's order list, can clear an entry. Nothing
   * else clears one.
   */
  const IndeterminacyLedger& ledger() const noexcept { return ledger_; }
  IndeterminacyLedger& ledger() noexcept { return ledger_; }

  /**
   * Returns why local machinery can no longer send, or nothing while it can.
   *
   * **Sticky on purpose.** A journal that cannot be written will fail on the next order too, and
   * the one after that. Clearing it on the next quiet bar would turn a storage outage into a
   * trickle of unexplained refusals; leaving it set is what "a runner that cannot journal should
   * not trade" actually requires.
   *
   * **It is a hold on the guard**, `strategy::HaltReason::LOCAL_FAILURE`, not a flag beside it.
   * That is the difference between a condition every send path has to remember to check and one
   * checked by the single function every order already passes through. It also means an operator
   * asking the guard why it will not trade is told about the journal, rather than being told only
   * that trading is halted.
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
    if (probing_sellable_) {
      sellable_probe_.observe(context_.portfolio());
    }
  }

  /** Returns what the probe has inferred, and the counts behind it. */
  const strategy::SellableConventionProbe<Tag>& sellable_convention() const noexcept {
    return sellable_probe_;
  }

  /** Returns the strategy, so a test can read the state it kept. */
  const Strategy& strategy() const noexcept { return strategy_; }
  Strategy& strategy() noexcept { return strategy_; }

  /** Returns the context, so a caller can arrange the world before feeding an event. */
  const Context& context() const noexcept { return context_; }
  Context& context() noexcept { return context_; }

  /** Returns the guard, so a caller can halt trading or read the limits. */
  const strategy::RiskGuard& guard() const noexcept { return guard_; }
  strategy::RiskGuard& guard() noexcept { return guard_; }

  /**
   * Records today's permitted price band for one instrument, as a live runner would have read it.
   *
   * Absent for an instrument means the broker published no band, which reconciliation treats as no
   * clamping rather than as a band of zero width.
   */
  void set_price_limits(const InstrumentId& id, PriceLimits limits) {
    price_limits_.insert_or_assign(id.symbol_, std::move(limits));
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
  void set_portfolio_value(Decimal64 value) { portfolio_value_ = std::move(value); }

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
    dispatch::bar(strategy_, context_, event);
    return settle();
  }

  /** Feeds one quote and calls the handler. */
  Report feed(const strategy::QuoteEvent& event) {
    context_.set_last_price(event.quote_.instrument_, event.quote_.last_price_);
    context_.set_now(event.quote_.received_at_);
    dispatch::quote(strategy_, context_, event);
    return settle();
  }

  /** Feeds one execution and calls the handler. */
  Report feed(const strategy::Fill& event) {
    dispatch::fill(strategy_, context_, event);
    return settle();
  }

  /** Feeds one order update and calls the handler. */
  Report feed(const OrderSnapshot& event) {
    dispatch::order_update(strategy_, context_, event);
    return settle();
  }

  /** Announces a phase change, setting it on the context first so a handler reads it. */
  Report feed(strategy::MarketPhase phase) {
    context_.set_phase(phase);
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

    Report report;
    const auto instructions = context_.drain();
    std::vector<strategy::PlannedOrder> pending;

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
      if (const auto* cancel = std::get_if<strategy::CancelIntent<Tag>>(&instruction)) {
        attempt_cancel(cancel->ref_);
        continue;
      }

      const auto planned = plan_for(instruction);
      for (const auto& ref : planned.cancel_) {
        attempt_cancel(ref);
      }
      for (const auto& rejection : planned.rejected_) {
        report.rejected_.push_back(rejection);
      }
      for (const auto& order : planned.place_) {
        pending.push_back(order);
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
    // `persist_reservation` closes the same hazard *across* passes, for the limits that read what
    // is outstanding. Nothing has to recognise its own order in a later listing: a poll assigns
    // `working_` and `positions_` wholesale, so the broker's own answer replaces the reservation
    // when it arrives. Between a placement and that poll, the context portfolio is therefore the
    // broker's last report **plus** what this runner has sent since -- which is what a limit has to
    // size against, and why the rows are marked so that anything inferring the broker's behaviour
    // from the listing can tell them apart.
    auto view = context_.portfolio();
    // Resolved once for the pass. The currencies and marks do not move as orders are accepted --
    // only quantities do, and the guard reads those from the view -- and resolving per order would
    // repeat a map lookup per instrument for every instrument. Built only when the ceiling is on, so
    // a caller who has not asked for it pays nothing.
    if (guard_.limits().max_gross_exposure_) {
      inputs.exposure_basis_ = exposure_basis_for(view, pending);
    }
    for (auto& order : pending) {
      // Checked before the guards, because an unresolved instrument is a safety hold rather than a
      // limit breach: reporting a notional ceiling for an order that was never going to be sent
      // would point an operator at the wrong problem.
      // Checked before the ledger and before the guards, and checked *per order* rather than once
      // at the top. A journal that failed on the first order of a batch will fail on the second, so
      // a batch that carried on would produce one unexplained refusal per instrument. This is also
      // what makes the condition hold for a direct `feed()`, which never reaches `PollingLoop`'s
      // gate at all.
      if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
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
        report.refused_.push_back(std::move(*decision.rejection_));
        continue;
      }

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
          // money is spent. An `INDETERMINATE` send needs no entry here: it marks the ledger, and
          // the next instruction for that instrument is withheld before it reaches a guard at all.
          reserve(
            view, order, inputs.reference_price_, inputs.order_currency_, last_ref(),
            guard_.limits().enforce_buying_power_
          );
          persist_reservation(order, inputs.reference_price_, inputs.order_currency_);
          report.would_place_.push_back(std::move(order));
          break;
        case SendOutcome::REJECTED:
          // A refusal is an answer: the order does not exist, so nothing needs reconciling. It
          // still consumed a request, which is what the broker will have counted.
          guard_.note_sent(inputs.now_);
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

  /**
   * Carries this pass's reservation into the next one, for a caller that asked for the checks.
   *
   * **The in-pass view dies with the pass**, so without this a second pass before the next poll
   * starts from the broker's last word again and can re-spend what the first pass already
   * committed. The fix needs no matcher, because a poll *replaces* what it refreshes:
   * `poll_working_orders` assigns `working_` wholesale and `poll_holdings` assigns `positions_`, so
   * a reservation written here is discarded the moment the broker's own answer arrives. Nothing has
   * to recognise its own order in a listing -- which is the half of reserve-and-match that carries a
   * matcher's failure modes.
   *
   * **Scoped to the limits that read it**, deliberately. The reservation is also visible to
   * reconciliation, which reads the same portfolio, and an order this runner just sent counting as
   * working is the right answer there too -- but it is a behaviour change, and it should reach only
   * callers whose limits actually size against what is outstanding rather than every existing
   * program on upgrade.
   *
   * `max_position_quantity_` is one of those limits, and it is the one that predates this branch:
   * its ceiling counts same-side working orders, so an accepted order that stopped being visible
   * at the end of a pass would let the next bar place another of the same size, for as long as no
   * working-order poll intervened. Review found that omission after the within-pass fix, which had
   * left the cross-pass half of the same defect open for a caller enabling only the ceiling.
   *
   * The residual window is the one nothing local can close: a listing fetched before the broker
   * registered the order replaces the reservation with a picture that does not contain it yet.
   * That is what `closing-state-confirmation.md` is about, and it is not made worse here.
   */
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

  void persist_reservation(
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency
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
    if (!sells && !buys && !capped && !exposed) {
      return;
    }
    auto persisted = context_.portfolio();
    reserve(
      persisted, order, reference_price, currency, last_ref(),
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
   * Where the executor could not supply a reference -- a dry run, a replay, or a custom executor
   * that does not report one -- the row is still written, because a limit that ignored the order
   * would be the worse failure. Whether such a row can still be withdrawn is the executor's
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
    working.order_.price_ = order.price_;
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

  void send_cancel(const OrderRef<Tag>& ref, Report& report) {
    // Gated like a placement, and in the same order. A cancellation is the one instruction that
    // cannot make an unresolved *position* worse, which is why the ledger does not gate it -- but it
    // still goes through the journal, and a journal that cannot record it must not let it be sent
    // unrecorded.
    if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      report.unavailable_cancel_.push_back(ref);
      return;
    }
    // And a halt stops this runner *sending*, which a cancellation is. Withdrawing reduces exposure
    // and it is tempting to let it through anyway; the argument against is that `StopIntent` reaches
    // this runner from a signal and from a file, so exempting cancellations would let
    // `echo withdraw > /var/run/atl/stop` send an instruction that an operator's own halt forbade.
    // A kill switch a control file can partly bypass is not one. An operator who wants out resumes
    // first, which is one deliberate act and is visible in the report either way.
    if (const auto holding = guard_.blocking_reason()) {
      report.refused_cancel_.push_back(RefusedCancel<Tag>{ref, *holding});
      return;
    }
    // Resolved against the portfolio first, because building a cancellation needs the order's own
    // quantity on a broker that requires one. A reference not in the snapshot means the snapshot is
    // stale, and sending a cancellation for an order the runner cannot describe is exactly the kind
    // of blind instruction that should be visible instead.
    const auto* working = context_.portfolio().find_working(ref);
    if (working == nullptr) {
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
        report.would_cancel_.push_back(ref);
        return;
      case SendOutcome::REJECTED:
        // The broker refused the request, which is an answer: nothing needs reconciling. It is not
        // a withdrawal that happened, and it is not proof the order is working either -- a fill is
        // one of the ordinary reasons a withdrawal is refused. It still spent a request.
        guard_.note_sent(context_.now());
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
        report.indeterminate_cancel_.push_back(ref);
        return;
      case SendOutcome::REFUSED_LOCALLY:
        // No `note_sent`, for the reason the placement path gives: nothing was sent, so the broker
        // counted nothing, and a rate limit must never become the reason a later order cannot go.
        // Reported beside the broker's own refusals because what it means for the order is the
        // same -- it was not withdrawn -- while `by_broker_` on the entry says which of the two
        // it was. Not `last_error()`: a later instruction in the same pass clears that.
        report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/false});
        return;
      case SendOutcome::UNAVAILABLE:
        // Not reported as a withdrawal that happened, because it did not. A cancellation nobody
        // sent must not read as one the broker has.
        report.unavailable_cancel_.push_back(ref);
        return;
    }
  }

  /**
   * Reconciles one queued instruction against the current portfolio.
   *
   * The reference data is held in a local for the duration of the call, because
   * `ReconcileInputs` borrows it rather than copying: an `Instrument` carries a tick table, and
   * copying one per instruction per bar would be paying repeatedly for a value that does not
   * change.
   */
  strategy::Plan<Tag> plan_for(const Instruction<Tag>& instruction) const {
    const auto id = instrument_of(instruction);
    const auto reference = context_.instrument(id);

    strategy::ReconcileInputs inputs;
    inputs.instrument_ = reference ? &*reference : nullptr;
    inputs.reference_price_ = context_.last_price(id);
    inputs.limits_ = limits_for(id);
    inputs.portfolio_value_ = portfolio_value_;
    inputs.tolerance_ = tolerance_;

    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return strategy::reconcile(*target, context_.portfolio(), inputs);
    }
    if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
      return strategy::reconcile(*intent, context_.portfolio(), inputs);
    }
    return strategy::Plan<Tag>{};
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

  /** Returns today's permitted band for `id`, when one was supplied. */
  std::optional<PriceLimits> limits_for(const InstrumentId& id) const {
    const auto found = price_limits_.find(id.symbol_);
    if (found == price_limits_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  Strategy strategy_;
  strategy::RiskGuard guard_;
  /** Placements sent that may have left an order resting; see `placements_at_broker`. */
  std::size_t placements_at_broker_{0};
  /** Orders sent under a phase the limits did not permit; see `sends_outside_permitted_phase`. */
  std::size_t sends_outside_permitted_phase_{0};
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
  std::map<std::string, PriceLimits> price_limits_;
  std::optional<Decimal64> portfolio_value_;
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
