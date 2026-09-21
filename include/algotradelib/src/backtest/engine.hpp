#pragma once

#include "./clock.hpp"
#include "./cost.hpp"
#include "./fill.hpp"
#include "./ledger.hpp"

#include "../runner/runner.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * Replaying bars through the same runner a live session uses.
 *
 * The third configuration of `Runner`, after `DryRunner` and the session-backed one. A strategy's
 * source is identical in all three — and if it were not, the surface settled in `strategy-core` would
 * have been wrong.
 *
 * **The bar a strategy is shown is never the bar it trades on.** An order decided on bar *n* is queued
 * and filled against bar *n+1*, which is what `QueueingExecutor` and `NextBarFillModel` enforce between
 * them. That is the one discipline a backtester exists to keep, and it is structural here rather than a
 * rule somebody has to remember.
 *
 * **Nothing assumes a bar is a day.** The clock advances by each bar's own period end, phases come from
 * a session window, and an annualized figure takes its period count from the caller. The same engine
 * replays minute bars and daily ones, which the tests assert with the same strategy and the same
 * assertions at both intervals.
 */

/** An executor that accepts orders and holds them for the next bar. */
template <typename BrokerTag>
class QueueingExecutor {
public:
  /**
   * Accepts an order and queues it.
   *
   * Always `ACCEPTED`: a replay has no transport that could fail, and inventing failures would be
   * inventing data. A caller wanting to study behaviour under transport failure has `Runner`'s executor
   * seam and the scripted executor the `strategy-core` tests use.
   */
  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    queued_.push_back(order);
    return runner::SendOutcome::ACCEPTED;
  }

  /**
   * Accepts a cancellation of an order still queued, and drops it.
   *
   * A replay's orders rest only between two bars, so a cancellation either finds one queued or refers
   * to something already filled — which is not an error, merely a race the engine resolved first.
   */
  runner::SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    for (auto at = queued_.begin(); at != queued_.end(); ++at) {
      if (at->instrument_ == working.order_.instrument_ && at->side_ == working.order_.side_) {
        queued_.erase(at);
        break;
      }
    }
    return runner::SendOutcome::ACCEPTED;
  }

  /** Removes and returns everything queued, in the order it was submitted. */
  std::vector<strategy::PlannedOrder> take() {
    std::vector<strategy::PlannedOrder> taken;
    taken.swap(queued_);
    return taken;
  }

  /** Returns what is queued without removing it. */
  const std::vector<strategy::PlannedOrder>& queued() const noexcept { return queued_; }

  /** Puts an order back, for a limit that has not yet been reached. */
  void requeue(strategy::PlannedOrder order) { queued_.push_back(std::move(order)); }

private:
  std::vector<strategy::PlannedOrder> queued_;
};

/** One point on the equity curve, recorded per bar. */
struct EquityPoint {
  std::chrono::system_clock::time_point at_{};
  /** Cash plus positions at that bar's close. */
  Decimal64 equity_;
  /** Return over the previous point, or `missing()` on the first. */
  math::Scalar period_return_{0};
};

/** What a replay did, for a report to summarize. */
struct ReplayResult {
  std::vector<EquityPoint> equity_;
  std::vector<Execution> executions_;
  /** Instructions reconciliation could not build. */
  std::vector<strategy::Rejection> rejected_;
  /** Instructions a guard refused. */
  std::vector<strategy::GuardRejection> refused_;
  /** Bars that did not follow their predecessor, which a replay does not interpolate across. */
  std::size_t gaps_{0};
  /** Bars replayed. */
  std::size_t bars_{0};
  /** Orders still queued when the replay ended, never filled. */
  std::size_t unfilled_{0};
};

/** Everything a replay needs beyond the strategy and the bars. */
struct ReplayConfig {
  /** Starting cash, in `currency_`. */
  Decimal64 cash_{Decimal64::parse("10000000")};
  Currency currency_{Currency{"KRW"}};
  /** Reference data for every instrument replayed; without it nothing can be built. */
  std::vector<Instrument> instruments_;
  /** Limits, applied exactly as they would be live. */
  strategy::RiskLimits limits_;
  /** Session the phases are derived against. */
  strategy::SessionWindow session_{strategy::SessionWindow::krx()};
  /** Trailing bars the context caches, for the indicators a strategy reads. */
  std::size_t series_capacity_{512};
};

/**
 * Replays a bar series through one strategy.
 *
 * Driven by `feed`, one bar at a time, so a caller can interleave bars from several instruments in
 * timestamp order — which is what a cross-sectional replay needs and what a single-series convenience
 * wrapper would have foreclosed.
 */
template <typename Strategy, typename CapsType = strategy::NoCaps>
class Backtester {
public:
  using Tag = BacktestTag;
  using Executor = QueueingExecutor<Tag>;
  using Runner = runner::Runner<Strategy, CapsType, Tag, Executor>;

  /** Constructs a replay of `strategy` under `config`, filling through `fills`. */
  Backtester(
    Strategy strategy = {},
    ReplayConfig config = {},
    NextBarFillModel fills = NextBarFillModel{}
  )
  : config_(std::move(config)),
    fills_(std::move(fills)),
    ledger_(config_.cash_, config_.currency_),
    clock_(config_.session_),
    runner_(std::move(strategy), config_.limits_, config_.series_capacity_) {
    for (const auto& reference : config_.instruments_) {
      runner_.context().set_instrument(reference);
    }
  }

  /** Returns the strategy, so a caller can read the state it kept. */
  const Strategy& strategy() const noexcept { return runner_.strategy(); }

  /** Returns the ledger, for positions, cash and realized profit. */
  const ReplayLedger& ledger() const noexcept { return ledger_; }

  /** Returns the fill model, so a report can state what it assumed. */
  const NextBarFillModel& fills() const noexcept { return fills_; }

  /** Returns what the replay has done so far. */
  const ReplayResult& result() const noexcept { return result_; }

  /**
   * Replays one bar.
   *
   * In this order, and the order is the whole correctness argument:
   *
   * 1. **Fill what bar *n-1* asked for, against this bar.** Before the strategy sees it, so a fill can
   *    never use information the order did not have.
   * 2. **Apply the fills** to the ledger and tell the strategy about them.
   * 3. **Mark positions** to this bar's close and record the equity point.
   * 4. **Advance the clock** to this bar's period end and fire a phase change if there was one.
   * 5. **Show the strategy the bar**, which may queue orders for the *next* one.
   *
   * Reversing steps 1 and 5 is exactly the lookahead defect, and doing 3 before 2 would value a
   * portfolio that had not yet been updated by its own fills.
   */
  void feed(const strategy::BarEvent& event, std::optional<bool> trading_day = true) {
    ++result_.bars_;
    if (previous_ && !ReplayClock::contiguous(*previous_, event)) {
      // A gap is counted rather than interpolated across: nothing happened in it that a strategy
      // should be told about, and inventing a bar would be inventing data.
      ++result_.gaps_;
    }

    settle_queued_against(event);
    ledger_.mark(event.instrument_, event.bar_.close_);
    record_equity(event);

    if (clock_.advance(event, trading_day)) {
      runner_.context().set_phase(clock_.phase());
      drain_reports(runner_.feed(clock_.phase()));
    } else {
      runner_.context().set_phase(clock_.phase());
    }

    runner_.context().set_portfolio(ledger_.snapshot<Tag>(clock_.now()));
    runner_.context().set_last_price(event.instrument_, event.bar_.close_);
    drain_reports(runner_.feed(event));
    previous_ = event;
  }

  /** Ends the replay, counting whatever never filled. */
  void finish() {
    drain_reports(runner_.stop());
    result_.unfilled_ = runner_.executor().queued().size();
  }

private:
  /** Fills orders queued by earlier bars against `event`, and tells the strategy. */
  void settle_queued_against(const strategy::BarEvent& event) {
    auto pending = runner_.executor().take();
    for (auto& order : pending) {
      if (!(order.instrument_ == event.instrument_)) {
        // A different instrument's order waits for that instrument's next bar, which is what makes an
        // interleaved multi-instrument replay behave.
        runner_.executor().requeue(std::move(order));
        continue;
      }
      const auto rule = rule_for(order.instrument_);
      auto decision = fills_.decide(order, event, rule);
      if (!decision.execution_) {
        if (decision.still_working_) {
          runner_.executor().requeue(std::move(order));
        }
        continue;
      }
      if (!ledger_.apply(*decision.execution_, config_.currency_)) {
        // Arithmetic that will not fit an exact decimal. Counted as unfilled rather than applied
        // partly, because a half-applied ledger is worse than an order that did not trade.
        continue;
      }
      result_.executions_.push_back(*decision.execution_);
      runner_.context().set_portfolio(ledger_.snapshot<Tag>(clock_.now()));
      drain_reports(runner_.feed(decision.execution_->fill_));
    }
  }

  /** Records equity at this bar, and the return since the previous point. */
  void record_equity(const strategy::BarEvent& event) {
    const auto equity = ledger_.equity_in(config_.currency_);
    if (!equity) {
      return;
    }
    EquityPoint point;
    point.at_ = event.period_end_;
    point.equity_ = *equity;
    point.period_return_ = math::missing();
    if (!result_.equity_.empty()) {
      const auto& previous = result_.equity_.back().equity_;
      if (previous.coefficient() != 0) {
        const auto moved = decimal_subtract(*equity, previous);
        if (moved) {
          const auto ratio = decimal_divide(*moved, previous, 8);
          if (ratio) {
            point.period_return_ = analytics_to_scalar(*ratio);
          }
        }
      }
    }
    result_.equity_.push_back(point);
  }

  /**
   * Converts an exact ratio to the scalar the metrics compute in.
   *
   * The one crossing of the decimal boundary in this engine, and it goes outward: a return is an input
   * to `sharpe` and the rest, which are defined on `Scalar`. Nothing converts back, because no figure
   * a report produces is ever sent to a broker.
   */
  static math::Scalar analytics_to_scalar(const Decimal64& value) {
    auto scaled = static_cast<math::Scalar>(value.coefficient());
    for (std::uint8_t at = 0; at < value.scale(); ++at) {
      scaled /= math::Scalar{10};
    }
    return scaled;
  }

  /** Returns the grid for `instrument`, unknown when no reference data was supplied. */
  PriceRule rule_for(const InstrumentId& instrument) const {
    const auto reference = runner_.context().instrument(instrument);
    if (!reference) {
      return PriceRule::unknown("no reference data supplied to the replay");
    }
    return reference->price_rule_;
  }

  /** Collects what a runner pass reported, so a replay ends with the whole picture. */
  template <typename Report>
  void drain_reports(const Report& report) {
    for (const auto& rejection : report.rejected_) {
      result_.rejected_.push_back(rejection);
    }
    for (const auto& refusal : report.refused_) {
      result_.refused_.push_back(refusal);
    }
  }

  ReplayConfig config_;
  NextBarFillModel fills_;
  ReplayLedger ledger_;
  ReplayClock clock_;
  Runner runner_;
  ReplayResult result_;
  std::optional<strategy::BarEvent> previous_;
};

} // namespace backtest
} // namespace algotradelib
