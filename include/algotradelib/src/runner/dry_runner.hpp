#pragma once

#include "./context.hpp"
#include "./dispatch.hpp"

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
 * **No client exists in the binary.** This header reaches no broker and no transport, so a dry-run
 * program links no TLS and has no code path that could place an order. That is a stronger promise
 * than a flag: a flag can be wrong, whereas a symbol that is not linked cannot be called.
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
  /** Lines the strategy logged. */
  std::vector<std::string> log_;

  /** Returns whether the run decided to do nothing at all. */
  bool quiet() const noexcept {
    return would_place_.empty() && would_cancel_.empty() && rejected_.empty() &&
      refused_.empty();
  }
};

/**
 * Drives one strategy with no broker attached.
 *
 * Holds the strategy by value so a caller can read its state afterwards — a warm-up count, a
 * position it tracked — which is most of what a strategy test wants to assert.
 */
template <typename Strategy, typename CapsType = strategy::NoCaps>
class DryRunner {
public:
  /** The broker tag a dry run stands in for. References are never sent anywhere. */
  struct Tag {
    static constexpr std::string_view name = "dryrun";
  };

  using Context = runner::Context<Tag, CapsType>;
  using Report = DryRunReport<Tag>;

  ATL_ASSERT_STRATEGY(Strategy, Context);

  /** Constructs a runner with the given strategy, limits, and series warm-up window. */
  explicit DryRunner(
    Strategy strategy = {},
    strategy::RiskLimits limits = {},
    std::size_t series_capacity = 512
  )
  : strategy_(std::move(strategy)), guard_(std::move(limits)), context_(series_capacity) {}

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
        report.would_cancel_.push_back(cancel->ref_);
        continue;
      }

      const auto planned = plan_for(instruction);
      for (const auto& ref : planned.cancel_) {
        report.would_cancel_.push_back(ref);
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
      inputs.reference_price_ = context_.last_price(order.instrument_);
      if (auto refusal = guard_.check(order, context_.portfolio(), inputs)) {
        report.refused_.push_back(std::move(*refusal));
        continue;
      }
      // A dry run records the send that a live runner would perform, so the rate limit behaves
      // exactly as it will in production rather than being permanently unused here.
      guard_.note_sent(inputs.now_);
      report.would_place_.push_back(std::move(order));
    }

    report.log_ = context_.log().take();
    return report;
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
  std::map<std::string, PriceLimits> price_limits_;
  std::optional<Decimal64> portfolio_value_;
};

} // namespace runner
} // namespace algotradelib
