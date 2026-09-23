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
  /** Instructions reconciliation could not build, with the reason. */
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
   * Cancellations the broker refused.
   *
   * A refusal is an answer about the *request*: it was not accepted, so nothing needs reconciling.
   * It says nothing about the order, which may still be working or may have filled a moment
   * earlier -- a fill is one of the ordinary reasons a broker refuses a withdrawal. Either way the
   * request did not go, which is why this is not `would_cancel_`. The placement path maps a broker
   * refusal into `rejected_` the same way.
   */
  std::vector<OrderRef<BrokerTag>> rejected_cancel_;
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
    // Before anything is planned, because the portfolio here is the broker's own last word: a pass
    // adds its accepted orders to a copy, and the probe must not read this runner's intentions as
    // the broker's report.
    if (probing_sellable_) {
      sellable_probe_.observe(context_.portfolio());
    }

    Report report;
    const auto instructions = context_.drain();
    std::vector<strategy::PlannedOrder> pending;

    for (const auto& instruction : instructions) {
      if (const auto* cancel = std::get_if<strategy::CancelIntent<Tag>>(&instruction)) {
        send_cancel(cancel->ref_, report);
        continue;
      }

      const auto planned = plan_for(instruction);
      for (const auto& ref : planned.cancel_) {
        send_cancel(ref, report);
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
    // pass has sent.** The portfolio on the context is a picture of what the *broker* last
    // reported, and nothing puts a locally accepted order into it -- `portfolio.working_` is
    // written in exactly one place, a working-order poll (`live/session_source.hpp`). So a limit
    // read straight from the context is re-evaluated from the same starting point for every order,
    // and two orders that each fit can breach it together: with a hundred sellable and no resting
    // order, two sells of sixty both pass and twenty shares are sold that were not there.
    //
    // A copy per pass, mutated as orders are accepted, closes that within one `settle`. It does not
    // close the same hazard *across* passes, where a locally accepted order stays invisible until a
    // poll refreshes the listing; that needs a reservation that survives a pass and a way to know
    // when the listing it is waiting for has arrived, which is planned rather than done here.
    auto view = context_.portfolio();
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

      inputs.reference_price_ = context_.last_price(order.instrument_);
      // The currency the order is priced in, which only the reference data knows. A buying-power
      // limit is per currency, and an unresolved instrument leaves it absent rather than defaulting
      // to one: guessing here would spend one currency's budget on another's order.
      const auto reference = context_.instrument(order.instrument_);
      inputs.order_currency_ =
        reference ? std::optional<Currency>(reference->currency_) : std::nullopt;
      if (auto refusal = guard_.check(order, view, inputs)) {
        report.refused_.push_back(std::move(*refusal));
        continue;
      }

      const auto outcome = executor_.place(order);
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
          reserve(view, order, inputs.reference_price_, inputs.order_currency_);
          report.would_place_.push_back(std::move(order));
          break;
        case SendOutcome::REJECTED:
          // A refusal is an answer: the order does not exist, so nothing needs reconciling. It
          // still consumed a request, which is what the broker will have counted.
          guard_.note_sent(inputs.now_);
          report.rejected_.push_back(
            strategy::Rejection{
              strategy::RejectReason::NOT_REPRESENTABLE, order.instrument_, order.tag_
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
   * Records an accepted order in `view` so the rest of the pass sizes against it.
   *
   * Written as a working order because that is what it now is, and because the alternative --
   * adjusting the position -- would be a lie: nothing has filled. The reference is empty, which is
   * the one thing this synthetic row cannot supply; no guard reads it, and the row never leaves
   * this function's pass, so it cannot be mistaken for something a broker reported.
   */
  static void reserve(
    strategy::Portfolio<Tag>& view,
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency
  ) {
    WorkingOrder<Tag> working;
    working.order_.instrument_ = order.instrument_;
    working.order_.side_ = order.side_;
    working.order_.status_ = OrderStatus::PENDING;
    working.order_.quantity_ = order.quantity_;
    working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    view.working_.push_back(std::move(working));

    // A buy also spends money that the rest of the pass may not spend again. The estimate is the
    // same one the guard checked the order against -- its own price, or the last quote for a market
    // order -- and it is an *estimate*: a market order fills where it fills, and commission is not
    // in it. Under-reserving would be the dangerous direction, and neither of those makes the
    // reservation smaller than the order's stated cost.
    if (order.side_ != OrderSide::BUY || !currency) {
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
        report.would_cancel_.push_back(ref);
        return;
      case SendOutcome::REJECTED:
        // The broker refused the request, which is an answer: nothing needs reconciling. It is not
        // a withdrawal that happened, and it is not proof the order is working either -- a fill is
        // one of the ordinary reasons a withdrawal is refused.
        report.rejected_cancel_.push_back(ref);
        return;
      case SendOutcome::INDETERMINATE:
        // Marked *and* reported apart from `would_cancel_`. The mark was always here; reporting it
        // as a withdrawal that happened was not right beside it, because the order may still be
        // resting and a caller reading `would_cancel_` would believe it is not.
        ledger_.mark(working->order_.instrument_, "a cancellation request failed in transit");
        report.indeterminate_cancel_.push_back(ref);
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
