#pragma once

#include "./context.hpp"
#include "./dispatch.hpp"
#include "./execution.hpp"

#include "../strategy/reconcile.hpp"
#include "../strategy/risk.hpp"

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

/** What a dry run decided, in the order it decided. */
template <typename BrokerTag>
struct DryRunReport {
  /** Orders that passed reconciliation and every guard, and would have been sent. */
  std::vector<strategy::PlannedOrder> would_place_;
  /** Working orders that would have been withdrawn, before anything was placed. */
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
   * Cancellations naming an order the portfolio snapshot does not hold.
   *
   * Reported rather than sent blind. The snapshot is stale, and the order may already be gone; a
   * cancellation the runner cannot describe is one it should not invent.
   */
  std::vector<OrderRef<BrokerTag>> unresolved_cancel_;
  /** Lines the strategy logged. */
  std::vector<std::string> log_;

  /** Returns whether the run decided to do nothing at all. */
  bool quiet() const noexcept {
    return would_place_.empty() && would_cancel_.empty() && rejected_.empty() &&
      refused_.empty() && withheld_.empty() && indeterminate_.empty() &&
      unresolved_cancel_.empty();
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

  /** Calls `on_stop` and drains whatever it asked for. */
  Report stop() {
    dispatch::stop(strategy_, context_);
    return settle();
  }

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
    for (auto& order : pending) {
      // Checked before the guards, because an unresolved instrument is a safety hold rather than a
      // limit breach: reporting a notional ceiling for an order that was never going to be sent
      // would point an operator at the wrong problem.
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
      if (auto refusal = guard_.check(order, context_.portfolio(), inputs)) {
        report.refused_.push_back(std::move(*refusal));
        continue;
      }

      const auto outcome = executor_.place(order);
      switch (outcome) {
        case SendOutcome::ACCEPTED:
          // Counted against the rate limit only once the instruction actually went, so an order a
          // guard refused or the ledger withheld does not spend budget.
          guard_.note_sent(inputs.now_);
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
          ledger_.mark(order.instrument_, "an order request failed in transit");
          report.indeterminate_.push_back(std::move(order));
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
  void send_cancel(const OrderRef<Tag>& ref, Report& report) {
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
    if (outcome == SendOutcome::INDETERMINATE) {
      ledger_.mark(working->order_.instrument_, "a cancellation request failed in transit");
    }
    report.would_cancel_.push_back(ref);
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
  Context context_;
  Executor executor_;
  IndeterminacyLedger ledger_;
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
