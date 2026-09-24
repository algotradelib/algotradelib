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
#include <string>
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

/**
 * One order the replay has accepted and not yet resolved.
 *
 * `accepted_on_` is the bar the order was placed during, and it is what keeps the no-lookahead rule
 * a property of the data rather than of which list an order is sitting in: an order accepted during
 * bar *n* is not eligible to fill against bar *n*, whoever asks and from wherever.
 */
template <typename BrokerTag>
struct QueuedOrder {
  strategy::PlannedOrder order_;
  OrderRef<BrokerTag> ref_;
  std::uint64_t accepted_on_{0};
};

/**
 * An executor that accepts orders and holds them until they fill or are withdrawn.
 *
 * **It is never drained.** An earlier version handed its whole queue to the engine for the length of
 * a fill loop, which left the executor empty at exactly the moments the engine showed a strategy a
 * portfolio and called back into it: an order the loop had not reached yet was outstanding and
 * invisible, and a cancellation from that callback had nothing to find. Holding everything in one
 * place makes `queued()` the complete set of outstanding orders at every instant, which is what the
 * engine projects into `strategy::Portfolio::working_`.
 */
template <typename BrokerTag>
class QueueingExecutor {
public:
  /**
   * Accepts an order, queues it, and issues it a reference.
   *
   * Always `ACCEPTED`: a replay has no transport that could fail, and inventing failures would be
   * inventing data. A caller wanting to study behaviour under transport failure has `Runner`'s executor
   * seam and the scripted executor the `strategy-core` tests use.
   *
   * The reference is this replay's own, monotonic, and stable for as long as the order is
   * outstanding. It exists because a strategy that can *see* two resting orders must be able to
   * withdraw the one it means.
   */
  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    QueuedOrder<BrokerTag> entry;
    entry.order_ = order;
    entry.ref_.primary_ = std::to_string(++issued_);
    entry.accepted_on_ = bar_;
    last_ref_ = entry.ref_;
    queued_.push_back(std::move(entry));
    return runner::SendOutcome::ACCEPTED;
  }

  /**
   * Withdraws the order the reference names, and refuses a reference that names nothing.
   *
   * **Matched on the reference, never on instrument and side.** Those identify a *kind* of order and
   * not an order: with two resting buys of one instrument, withdrawing the second by attributes
   * removes the first, and the strategy is told its request succeeded. The reference is the only
   * thing that distinguishes them, which is why the executor issues one.
   *
   * A reference that matches nothing is `REJECTED` rather than `ACCEPTED`, because the order is not
   * there to withdraw — it filled, or it was already withdrawn. `ACCEPTED` would claim a withdrawal
   * that did not happen, and `Runner` routes this answer to `rejected_cancel_`, which is where a
   * broker's refusal of a withdrawal belongs.
   */
  runner::SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    for (auto at = queued_.begin(); at != queued_.end(); ++at) {
      if (at->ref_ == working.ref_) {
        queued_.erase(at);
        return runner::SendOutcome::ACCEPTED;
      }
    }
    return runner::SendOutcome::REJECTED;
  }

  /**
   * Returns the reference issued to the order just accepted.
   *
   * Detected by `Runner`, which uses it to mark its own reservation rows. A replay is the one
   * executor that can honestly answer this without a broker, because it *is* the broker.
   */
  std::optional<OrderRef<BrokerTag>> last_ref() const { return last_ref_; }

  /** Returns every outstanding order, oldest first. */
  const std::vector<QueuedOrder<BrokerTag>>& queued() const noexcept { return queued_; }

  /** Returns the outstanding order `ref` names, or nothing. */
  const QueuedOrder<BrokerTag>* find(const OrderRef<BrokerTag>& ref) const {
    for (const auto& entry : queued_) {
      if (entry.ref_ == ref) {
        return &entry;
      }
    }
    return nullptr;
  }

  /** Drops the order `ref` names, and reports whether there was one. */
  bool withdraw(const OrderRef<BrokerTag>& ref) {
    for (auto at = queued_.begin(); at != queued_.end(); ++at) {
      if (at->ref_ == ref) {
        queued_.erase(at);
        return true;
      }
    }
    return false;
  }

  /**
   * Opens the next bar, after which everything already queued counts as resting from an earlier one.
   *
   * Called by the engine before anything is filled against the new bar, so that the orders a
   * strategy places *during* it carry a later stamp and cannot fill against it.
   */
  void open_bar() noexcept { ++bar_; }

  /** Returns the bar being replayed, counting from one. */
  std::uint64_t bar() const noexcept { return bar_; }

private:
  std::vector<QueuedOrder<BrokerTag>> queued_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::uint64_t issued_{0};
  std::uint64_t bar_{0};
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
  /**
   * Instructions reconciliation could not build.
   *
   * Only reconciliation's own reasons reach here, unlike a live run: `QueueingExecutor` accepts
   * everything it is handed, so no replay entry can be a broker's refusal or a binding's.
   */
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

    // Opened before anything fills against this bar, so that an order placed during it carries a
    // later stamp than the ones already resting and cannot fill against the bar it was decided on.
    runner_.executor().open_bar();

    settle_queued_against(event);
    // A mark that will not fit leaves its lot at the previous price, so an equity point recorded
    // here would be stamped with this bar and computed at the last one. Skipped instead, which is
    // what `record_equity` already does when the total itself cannot be computed.
    if (ledger_.mark(event.instrument_, event.bar_.close_)) {
      record_equity(event);
    }

    if (clock_.advance(event, trading_day)) {
      runner_.context().set_phase(clock_.phase());
      // **Installed before the phase change is acted on, not after.** A strategy placing an order
      // from `on_phase` used to decide on a portfolio predating this bar's fills and this bar's
      // mark, while the engine already held a fresher answer and had simply not installed it yet.
      install_portfolio();
      drain_reports(runner_.feed(clock_.phase()));
    } else {
      runner_.context().set_phase(clock_.phase());
    }

    install_portfolio();
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
  /**
   * Fills orders resting from an earlier bar against `event`, and tells the strategy.
   *
   * **Walked by reference rather than by iterator or index.** Applying a fill calls `on_fill`, which
   * may place or withdraw orders, so any position into the queue is invalid the moment the strategy
   * is told anything. The eligible references are collected first and each is looked up again when
   * its turn comes; one the callback withdrew is simply no longer there.
   *
   * The queue is never emptied, so an order this loop has not reached yet is outstanding, visible in
   * every portfolio installed along the way, and addressable by a cancellation from any callback.
   */
  void settle_queued_against(const strategy::BarEvent& event) {
    std::vector<OrderRef<Tag>> eligible;
    for (const auto& entry : runner_.executor().queued()) {
      // A different instrument's order waits for that instrument's next bar, which is what makes an
      // interleaved multi-instrument replay behave. An order accepted during this bar waits for the
      // next one, which is the no-lookahead rule.
      if (!(entry.order_.instrument_ == event.instrument_)) {
        continue;
      }
      if (entry.accepted_on_ >= runner_.executor().bar()) {
        continue;
      }
      eligible.push_back(entry.ref_);
    }

    for (const auto& ref : eligible) {
      const auto* entry = runner_.executor().find(ref);
      if (entry == nullptr) {
        // Withdrawn by a callback earlier in this loop. Nothing to fill.
        continue;
      }
      const auto order = entry->order_;
      const auto rule = rule_for(order.instrument_);
      auto decision = fills_.decide(order, event, rule);
      if (!decision.execution_) {
        if (!decision.still_working_) {
          // A market order the bar could not price, or a limit whose arithmetic did not fit. It does
          // not rest and it did not trade, so it stops being outstanding.
          runner_.executor().withdraw(ref);
        }
        continue;
      }
      // Removed before the ledger moves and before anything is shown a portfolio, so no view ever
      // carries a filled order as still working.
      runner_.executor().withdraw(ref);
      if (!ledger_.apply(*decision.execution_, currency_for(order.instrument_))) {
        // Arithmetic that will not fit an exact decimal. Counted as unfilled rather than applied
        // partly, because a half-applied ledger is worse than an order that did not trade.
        continue;
      }
      result_.executions_.push_back(*decision.execution_);
      install_portfolio();
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
  /**
   * Returns the currency `instrument` trades in, from the reference data the replay was given.
   *
   * **Every fill used to be booked in `config_.currency_`**, which is documented as the currency the
   * *starting cash* is in. A replay of a dollar-quoted instrument therefore banked dollars as won at
   * a rate of one -- the implicit conversion Decision 6 forbids and `holdings-core` exists to refuse.
   *
   * The fallback is defensive rather than a policy: `reconcile` refuses an instrument with no
   * reference data before a `PlannedOrder` exists (`strategy/reconcile.hpp`), so nothing can reach
   * the fill model unregistered. It is written the conservative way round anyway -- an unregistered
   * instrument books in the currency the ledger was opened in, which is the behaviour every existing
   * replay already had.
   */
  /**
   * Installs the portfolio a strategy and its guards decide on.
   *
   * The single place the replay answers "what does this account look like", so that every callback
   * boundary is answered the same way.
   *
   * **It has to be a complete answer, because it overwrites one.**
   * `Runner::persist_reservation` writes an accepted order into the context portfolio and relies on
   * a wholesale replacement carrying what the reservation was standing in for
   * (`runner/runner.hpp:628-634`). Live that replacement is a broker's listing. Here it is this
   * function, and a partial answer is therefore not a smaller improvement -- it is a *loss*, because
   * the reservation it erased was carrying the part it left out.
   *
   * The reservation carried two things, so this carries both: the outstanding orders themselves,
   * and the money an outstanding buy has committed.
   */
  void install_portfolio() {
    auto portfolio = ledger_.snapshot<Tag>(clock_.now());
    for (const auto& entry : runner_.executor().queued()) {
      portfolio.working_.push_back(working_row(entry));
      commit_open_buy(portfolio, entry);
    }
    runner_.context().set_portfolio(std::move(portfolio));
  }

  /**
   * Returns the working-order row for an outstanding replay order.
   *
   * `reserved_` stays false, unlike a runner's own reservation. The flag distinguishes this
   * library's arithmetic from what a broker reported, and in a replay the engine *is* the broker:
   * this row is the listing. The one consequence worth stating is that
   * `strategy::SellableConventionProbe`, which reads unreserved rows as evidence about a broker's
   * convention, will read these -- and a replay's sellable figure is a model that never moves, so
   * the probe reports what the simulator does rather than what a broker does.
   */
  WorkingOrder<Tag> working_row(const QueuedOrder<Tag>& entry) const {
    WorkingOrder<Tag> row;
    row.ref_ = entry.ref_;
    row.order_.instrument_ = entry.order_.instrument_;
    row.order_.side_ = entry.order_.side_;
    row.order_.type_ = entry.order_.type_;
    row.order_.time_in_force_ = entry.order_.time_in_force_;
    row.order_.price_ = entry.order_.price_;
    row.order_.quantity_ = entry.order_.quantity_;
    row.order_.currency_ = currency_for(entry.order_.instrument_);
    // Open, and nothing filled: a replay fills an order whole or not at all, so a row that exists
    // has traded nothing.
    row.order_.status_ = OrderStatus::PENDING;
    row.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    row.amendable_quantity_ = entry.order_.quantity_;
    return row;
  }

  /**
   * Takes what an outstanding buy has committed out of the buying power the replay models.
   *
   * **Rows alone do not fix `enforce_buying_power_`**, which reads the scalar and not the orders.
   * `ReplayLedger::snapshot` builds buying power from ledger cash, and that cash has not paid for
   * anything still resting -- so with a hundred of cash, a resting buy costing a hundred, and a
   * second buy costing a hundred in another instrument, the second passes and both can still fill.
   *
   * Valued at the order's own price, else the last close, which is the same estimate the guard
   * checked the order against. It is an estimate: a market order fills where it fills and
   * commission is not in it, and neither valuation is smaller than the order's stated cost, so the
   * error is not in the permissive direction. An order that can be valued at neither is left
   * uncommitted rather than guessed at.
   */
  void commit_open_buy(strategy::Portfolio<Tag>& portfolio, const QueuedOrder<Tag>& entry) const {
    if (entry.order_.side_ != OrderSide::BUY) {
      return;
    }
    const auto price = entry.order_.price_ ?
      entry.order_.price_ :
      runner_.context().last_price(entry.order_.instrument_);
    if (!price) {
      return;
    }
    const auto spent = decimal_multiply(entry.order_.quantity_, *price);
    if (!spent) {
      return;
    }
    const auto currency = currency_for(entry.order_.instrument_);
    for (auto& reading : portfolio.buying_power_) {
      if (!(reading.cash_.currency_ == currency)) {
        continue;
      }
      if (const auto left = decimal_subtract(reading.cash_.amount_, *spent)) {
        reading.cash_.amount_ = *left;
      }
      return;
    }
  }

  Currency currency_for(const InstrumentId& instrument) const {
    const auto reference = runner_.context().instrument(instrument);
    if (!reference) {
      return config_.currency_;
    }
    return reference->currency_;
  }

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
