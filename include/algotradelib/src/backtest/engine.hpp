#pragma once

#include "./clock.hpp"
#include "./cost.hpp"
#include "./fill.hpp"
#include "./ledger.hpp"

#include "../runner/order_updates.hpp"
#include "../runner/runner.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
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
  /** The order as reconciliation planned it. */
  strategy::PlannedOrder order_;
  /** The reference the replay issued for it; it reaches no broker but has the live type. */
  OrderRef<BrokerTag> ref_;
  /** The bar it was accepted during. It cannot fill against that bar, which is the lookahead rule. */
  std::uint64_t accepted_on_{0};
  /**
   * The instant the strategy decided it, which is the **other** half of the lookahead rule.
   *
   * A bar counter alone was enough while only bars could place an order: an order decided at bar
   * *n*'s period end waits for bar *n+1*, whose period begins at or after that instant. A quote
   * breaks that, because it arrives **inside** a bar's period without opening a new one -- so an
   * order decided at 10:30 was eligible for the daily bar stamped 15:30 that day, and
   * `NextBarFillModel` credits a market order with that bar's *open*, which was 09:00. The replay
   * would then have filled a 10:30 decision at a 09:00 price: not lookahead, but its mirror image,
   * and just as false.
   *
   * **Empty means undated**, which is every order placed by a caller driving this executor directly,
   * and is treated as eligible -- the rule can only ever defer a fill, never invent one. An
   * `optional` rather than a zero sentinel because the epoch is a perfectly good instant for a
   * replay to reach: a quote stamped 1970-01-01 placed an order that then took the *undated* branch
   * and filled at a price from before it, which is the defect this field exists to prevent, wearing
   * the field's own sentinel.
   */
  std::optional<std::chrono::system_clock::time_point> decided_at_;
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
   * A replay reaches no broker, and stamps its own portfolio besides.
   *
   * Stated rather than left to a default, because `runner::runner_detail::sends_to_a_broker` defaults
   * to `true` so that an unmarked executor fails closed.
   */
  static constexpr bool reaches_a_broker = false;
  /**
   * Accepts an order, queues it, and issues it a reference.
   *
   * `ACCEPTED` for every shape this model can price, and no transport failure is ever invented: a
   * replay has none, and inventing one would be inventing data. A caller wanting to study behaviour
   * under transport failure has `Runner`'s executor seam and the scripted executor the
   * `strategy-core` tests use.
   *
   * **A shape the model cannot price is `REFUSED_LOCALLY`**, which is the same answer the live
   * executor gives for an order a broker will not take, and it reaches the strategy by the same
   * route. A bar carries an open, a high, a low and a close; it carries no book and no spread, so a
   * best-limit, primary-limit or mid-price order has no price here, a stop-limit has no trigger, a
   * conditional limit has no close transition, and a closing-price order has no session close.
   * Filling those at the bar's open -- which is what this did, because `reference_price` returns the
   * open for every non-limit basis -- reports a fill at a price the order would never have had, and
   * a backtest that invents a fill price is worse than one that refuses to model the order.
   * Likewise a session other than the regular one: a bar series is the regular session.
   *
   * The reference is this replay's own, monotonic, and stable for as long as the order is
   * outstanding. It exists because a strategy that can *see* two resting orders must be able to
   * withdraw the one it means.
   */
  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    if (!can_model(order)) {
      return runner::SendOutcome::REFUSED_LOCALLY;
    }
    QueuedOrder<BrokerTag> entry;
    entry.order_ = order;
    entry.ref_.primary_ = std::to_string(++issued_);
    entry.accepted_on_ = bar_;
    entry.decided_at_ = deciding_at_;
    last_ref_ = entry.ref_;
    queued_.push_back(std::move(entry));
    return runner::SendOutcome::ACCEPTED;
  }

  /**
   * Returns whether this replay can price `order` without inventing a number.
   *
   * See `place` for why each exclusion is an exclusion. Stated as an allow-list
   * so a price basis added later is refused until somebody decides how a bar
   * would fill it, rather than being silently filled at the open.
   */
  static bool can_model(const strategy::PlannedOrder& order) noexcept {
    const bool priceable = order.type_ == OrderType::LIMIT || order.type_ == OrderType::MARKET;
    return priceable && order.session_ == OrderSession::REGULAR;
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

  /**
   * States the instant any order accepted from here on was decided at.
   *
   * Set by the engine before every dispatch that can place one, because the executor has no clock
   * of its own and the context's belongs to the runner. See `QueuedOrder::decided_at_` for what it
   * is for.
   */
  void deciding_at(std::chrono::system_clock::time_point at) { deciding_at_ = at; }

private:
  std::vector<QueuedOrder<BrokerTag>> queued_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::uint64_t issued_{0};
  std::uint64_t bar_{0};
  std::optional<std::chrono::system_clock::time_point> deciding_at_;
};

/** One point on the equity curve, recorded per bar. */
struct EquityPoint {
  /** The bar's close this point was taken at. */
  std::chrono::system_clock::time_point at_{};
  /** Cash plus positions at that bar's close. */
  Decimal64 equity_;
  /** Return over the previous point, or `missing()` on the first. */
  math::Scalar period_return_{0};
};

/** What a replay did, for a report to summarize. */
struct ReplayResult {
  /** One point per bar that could be valued, in order. */
  std::vector<EquityPoint> equity_;
  /** Every execution the fill model produced, in order. */
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
  /**
   * The currency the equity curve is denominated in, which is the replay's accounting currency.
   *
   * Carried so a report can tell whether a figure derived from *both* the curve and the executions
   * is comparing like with like. It is not, whenever an execution happened in another currency --
   * and a ratio of one currency's notional to another's equity looks entirely ordinary.
   *
   * **Appended rather than placed with the other currency-ish members**, deliberately: this struct
   * is public, and a caller aggregate-initializing it positionally would have had every field
   * shift underneath them. A new member at the end is additive; one at the front is a source break
   * dressed as a field.
   */
  Currency currency_{Currency{"KRW"}};
  /**
   * Why the replay stopped early, when it did.
   *
   * Two origins, and they mean the same thing to a reader: an execution that could not be
   * **costed** -- a date the schedule does not cover, a venue it states no rate for, a market whose
   * trading day this replay cannot compute -- and one the ledger could not **record**. Either way a
   * trade the model produced did not reach the books.
   *
   * Present means every figure in this result and in the report derived from it describes the bars
   * **up to** the halt and not the run the caller asked for. The bar it happened on is counted in
   * `bars_`, because it was fed, but nothing downstream of the refusal ran on it.
   *
   * **Why a halt rather than a skipped fill.** The refused trade would have changed the position the
   * strategy carried, the orders it placed afterwards, and therefore the equity curve, the return,
   * the drawdown, the Sharpe and the turnover. Continuing would have produced a complete-looking
   * report describing a path that never happened, which is a more expensive failure than stopping.
   */
  std::optional<BrokerError> halted_;
  /**
   * Orders cleared from the queue by a halt, rather than left unfilled by the end of the data.
   *
   * Counted apart from `unfilled_` because they mean different things: an unfilled order is a replay
   * that ran to the end of its bars with something still resting, and an aborted one is a replay that
   * stopped being able to cost a trade while it was still working.
   */
  std::size_t aborted_{0};
  /**
   * Quotes replayed, which is the figure that says whether any were **fed and dispatched** at all.
   *
   * Separate from `bars_` rather than added to it: a run of one bar and four hundred quotes is not a
   * run of four hundred and one somethings, and every per-bar statistic the report derives -- the
   * return series, the drawdown, the turnover -- is about bars. The counter exists because a handler
   * that is wired and never fed looks exactly like one that is not wired at all, which is the defect
   * this slice is for. It counts what reached the dispatcher and not what a strategy did with it:
   * `on_quote` is optional, and a strategy defining none is counted here just the same.
   *
   * **Appended**, for the reason stated on `currency_` above: this struct is public and a caller
   * aggregate-initializing it positionally would have every later field shift underneath them.
   */
  std::size_t quotes_{0};
  /**
   * How many times an order was allowed to fill against a bar whose period start this engine could
   * not derive -- so it could **not** check that the decision preceded the period.
   *
   * **Eligibility decisions, not fills**: one per queued order per bar it was considered against, and
   * the fill model may then decline it anyway. The name says `fill` because that is what the check
   * guards; the unit is stated here because no name carries it.
   *
   * Only a **close-stamped weekly, monthly or yearly** bar can reach it. Every other shape -- every
   * intraday interval, every open-stamped bar, and a close-stamped daily bar -- states or derives its
   * period start, and the comparison is then exact. So this is **zero for every replay this
   * library's own examples and tests produce** apart from the ones that exist to pin it.
   *
   * **What non-zero tells a reader:** for those, the engine cannot promise the fill price came from
   * after the decision. **Any dated order can be one of them**, not only an order a quote placed.
   * The obvious case is a quote falling part-way through such a bar's period; the less obvious one
   * needs no quote at all -- a decision made on Tuesday's *daily* bar, against Friday's *weekly* bar
   * of the same instrument, whose period began on Monday. The bar-counter rule does not catch that,
   * because those are two different bars, and the period check cannot run on the weekly one.
   *
   * **Appended**, for the reason `currency_` above states.
   */
  std::size_t unchecked_fill_causality_{0};
};

/** Everything a replay needs beyond the strategy and the bars. */
struct ReplayConfig {
  /** Starting cash, in `currency_`. */
  Decimal64 cash_{Decimal64::parse("10000000")};
  /** The currency the replay accounts in. An equity curve is one currency's. */
  Currency currency_{Currency{"KRW"}};
  /** Reference data for every instrument replayed; without it nothing can be built. */
  std::vector<Instrument> instruments_;
  /** Limits, applied exactly as they would be live. */
  strategy::RiskLimits limits_;
  /** Session the phases are derived against. */
  strategy::SessionWindow session_{strategy::SessionWindow::krx()};
  /** Trailing bars the context caches, for the indicators a strategy reads. */
  std::size_t series_capacity_{512};
  /**
   * The trading calendar a replayed strategy reads, installed on the context.
   *
   * Empty by default, and empty is the honest answer: a replay's trading-day question is answered
   * per bar by `feed`'s own argument, because a bar exists and therefore its date traded. This is for
   * the *other* question -- `Context::calendar()`, which a strategy can ask about a date it has no bar
   * for, such as whether tomorrow trades.
   *
   * Here because the live loop now fills that field from a polled calendar. Leaving the replay side
   * empty would mean a strategy reading `ctx.calendar()` behaved differently in the two worlds, which
   * is the divergence this whole slice is about.
   *
   * It deliberately does **not** change what `feed` derives the phase from. Doing so would turn every
   * date a supplied calendar does not cover into `UNKNOWN` and refuse every order in replays that work
   * today, which is a decision for whoever wants it, with its own report caveat.
   */
  TradingCalendar calendar_;
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
  /** The broker tag a replay's order references carry. */
  using Tag = BacktestTag;
  /** The executor this configuration of `Runner` sends through. */
  using Executor = QueueingExecutor<Tag>;
  /** The `Runner` specialization a replay drives; the same engine a live session uses. */
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
    runner_.context().set_calendar(config_.calendar_);
    // Stated on the result so a report can compare an execution's currency against the curve's.
    result_.currency_ = config_.currency_;
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
    // **A halted replay consumes nothing further.** `feed` keeps returning `void` so no caller has to
    // change, and a caller that ignores `ReplayResult` sees a run that ends early rather than one
    // that carries on costing nothing: no bar counted, no fill, no mark, no equity point, and no
    // callback into the strategy. That is API compatibility, which is not the same as invisibility,
    // and it is why the halt is stated on the result and first in the report.
    if (result_.halted_) {
      return;
    }
    ++result_.bars_;
    if (previous_ && !ReplayClock::contiguous(*previous_, event)) {
      // A gap is counted rather than interpolated across: nothing happened in it that a strategy
      // should be told about, and inventing a bar would be inventing data.
      ++result_.gaps_;
    }

    // Opened before anything fills against this bar, so that an order placed during it carries a
    // later stamp than the ones already resting and cannot fill against the bar it was decided on.
    runner_.executor().open_bar();
    // And dated, for the half of that rule a counter cannot express -- see `QueuedOrder::decided_at_`.
    // A bar's decisions are all made at its period end, whichever handler makes them: `on_fill`
    // during settlement, `on_phase`, `on_bar`, and a handler answering an order update afterwards.
    runner_.executor().deciding_at(event.period_end_);

    settle_queued_against(event);
    // **The halt stops this bar too, not merely the next one.** Returning only from settlement left
    // `feed` to mark the book, record an equity point, advance the clock and call `on_phase` and
    // `on_bar` on the very bar whose fill could not be costed -- so the strategy decided once more
    // on a portfolio missing that trade, and could queue orders that then survived in the executor
    // because every later `feed` is a no-op and `finish` returns early. Nothing runs after the
    // refusal.
    if (result_.halted_) {
      return;
    }
    // A mark that will not fit leaves its lot at the previous price, so an equity point recorded
    // here would be stamped with this bar and computed at the last one. Skipped instead, which is
    // what `record_equity` already does when the total itself cannot be computed.
    if (ledger_.mark(event.instrument_, event.bar_.close_)) {
      record_equity(event);
    }

    // **The price moves with the portfolio, not after it.** `Runner::settle` values a market order
    // at `context_.last_price`, so a view whose positions are marked to this bar while the price is
    // still the previous bar's is not a fresher picture -- it is an incoherent one, and it is worse
    // than a uniformly stale picture because it looks current. After a close moves from 10 to 100, a
    // market buy from `on_phase` would be checked against buying power and exposure at 10 while the
    // positions beside it say 100, which can admit an order those limits exist to refuse.
    runner_.context().set_last_price(event.instrument_, event.bar_.close_);

    if (clock_.advance(event, trading_day)) {
      runner_.context().set_phase(clock_.phase());
      // **The clock moves with the price and the portfolio, for the same reason.**
      // `Runner::feed(MarketPhase)` does not advance the context's time -- only `feed(BarEvent)`
      // does, and that runs later -- so `on_phase` would decide with this bar's marks and close
      // beside the *previous* bar's instant. `RiskGuard` records a send at `GuardInputs::now_` and
      // counts sends within `rate_window_` of it, so across a gap longer than that window a phase
      // order is filed under the old instant and the bar order that follows does not see it: two
      // orders through a one-order window at what the replay calls the same moment.
      runner_.context().set_now(event.period_end_);
      // **Installed before the phase change is acted on, not after.** A strategy placing an order
      // from `on_phase` used to decide on a portfolio predating this bar's fills and this bar's
      // mark, while the engine already held a fresher answer and had simply not installed it yet.
      install_portfolio();
      drain_reports(runner_.feed(clock_.phase()));
    } else {
      runner_.context().set_phase(clock_.phase());
    }

    install_portfolio();
    drain_reports(runner_.feed(event));
    // **After the dispatch, not before it**, so an order this bar's `on_bar` or `on_phase` placed is
    // reported. Before it would report only what settlement had already changed.
    publish_order_updates();
    previous_ = event;
  }

  /**
   * Replays one quote, so a strategy that acts between bars can be evaluated against history.
   *
   * `on_quote` fired live and in no replay before this existed: `QuoteEvent` appeared nowhere in
   * `backtest/`, so `PollKind::QUOTES` was a supported live cadence with no counterpart, and a
   * strategy written on it ran against real money having been evaluated against nothing. That is
   * `issues/live-parity-audit.md`'s finding 2.
   *
   * **Interleaved by the caller, in timestamp order**, exactly as bars across several instruments
   * already are. `feed(const BarEvent&)` is caller-driven for that reason and a quote series in
   * `ReplayConfig` would have had to invent an interleaving rule this class deliberately leaves out.
   *
   * ## What a quote does, and why this function is so short
   *
   * **Almost all of it is `Runner::feed(const strategy::QuoteEvent&)`, which both modes already
   * share.** That function sets `Context::last_price` from the quote, advances the context clock to
   * the quote's `received_at_`, dispatches `on_quote` and settles. Live,
   * `live::SessionSource::poll_quotes` calls exactly that and does nothing else. So parity here is
   * structural rather than maintained by agreement, and the only question this function answers is
   * what a *replay* must add around it:
   *
   * | | Live | Here |
   * | --- | --- | --- |
   * | Updates the price a market order is valued at | yes, in `Runner::feed` | the same call |
   * | Advances the decision clock to the quote's instant | yes, in `Runner::feed` | the same call |
   * | Re-derives the market phase at that instant | yes, `PollingLoop` before each due poll | **yes**, here |
   * | Announces a phase change | yes, once per iteration | **yes**, before `on_quote` |
   * | Publishes order updates | **when** the working-order poll is due and the broker offers one | **yes**, always |
   * | Marks the book, records an equity point, fills anything | no ledger exists live | **no** |
   *
   * The last three rows are approximations rather than identities, and saying which way each one
   * errs is more useful than claiming they match:
   *
   * - **The announcement is earlier here than live.** A live loop announces after all its polls, so
   *   `on_phase` sees the quote *and* that iteration's other polls; this announces before `on_quote`,
   *   which is what `feed(const BarEvent&)` has always done for a bar. Following the bar path keeps
   *   one replay internally consistent, which matters more to a replay than matching the loop's
   *   interleaving; the cost is that an order placed from `on_phase` is visible to `on_quote` here
   *   and would not yet exist live.
   * - **The update is unconditional here and conditional live**, on the working-order cadence being
   *   set, the broker publishing the inquiry, and the listing arriving. So a replay announces an
   *   order a live run might see later or, between two polls, not at all.
   * - **The last row is where live has nothing to differ with**: an equity curve is a replay's own
   *   artefact, recorded per bar.
   *
   * **Two drafts of this comment were wrong in opposite directions, and both are worth knowing.**
   * The first claimed a quote left the price and the clock alone, having read
   * `live::SessionSource::poll_quotes` and not the runner beneath it. The second claimed a quote
   * should publish no order update, because that one call does not -- true of the call and false of
   * the live *iteration*, where `WORKING_ORDERS` follows `QUOTES` and can list the order moments
   * later. Parity is a property of the whole loop, not of the function with the matching name.
   *
   * ## The consequence a strategy author has to know
   *
   * **An order placed from `on_quote` fills against the next *bar*, not against the quote.** That is
   * `NextBarFillModel`, unchanged by this slice, and `ReportCaveats::quotes_` states it on every
   * report of a run that fed one. A fill model that executes against a quote is a different design
   * with its own argument, and this deliberately does not smuggle one in.
   *
   * A quote fed **before the first bar** is accepted and counted, and it is not a special case: the
   * clock and the phase both come from the quote itself, so a strategy decides at the quote's
   * instant under the quote's phase. Refusing it would be a rule the live path does not have, since
   * a quote poll can and does run before the first bar arrives. What such an order cannot do is
   * fill, until a bar exists to fill it against.
   *
   * **The caller's obligation is timestamp order**, and it is load-bearing rather than tidy. This
   * clock follows whatever instant it is handed, so a quote stamped before the previous bar's period
   * end moves the replay backwards, and everything derived from an instant -- the phase, the rate
   * window, which bar an order may fill against -- follows it there.
   */
  void feed(const strategy::QuoteEvent& event, std::optional<bool> trading_day = true) {
    // The same rule `feed(const BarEvent&)` opens with: a halted replay consumes nothing further,
    // because a strategy told about anything after the halt decides on a run that has stopped being
    // able to cost what it asks for.
    if (result_.halted_) {
      return;
    }
    ++result_.quotes_;
    // Dated before anything can place an order, so a decision made here is not credited a price from
    // earlier in the bar it will fill against. See `QueuedOrder::decided_at_`.
    runner_.executor().deciding_at(event.quote_.received_at_);

    // **The phase is re-derived at the quote's own instant, before the handler runs.** Without this
    // a quote at 10:30 was shown the phase as it stood at the previous close, and the default
    // `RiskLimits::permitted_phases_` refuses an opening order in every phase but `OPEN` -- so a
    // strategy that lives correctly refused in a replay. `PollingLoop` installs the phase before
    // each due poll, this is the same rule, and a change is announced first for the reason
    // `feed(const BarEvent&)` announces it first: a handler placing an order from `on_phase` must
    // decide against the phase this instant established, not the one before it.
    const bool phase_changed = clock_.advance_to(event.quote_.received_at_, trading_day);
    runner_.context().set_phase(clock_.phase());
    if (phase_changed) {
      // **The price and the clock move first, then the change is announced.** `Runner::feed` sets
      // both from the quote, but it runs *after* this, so announcing first left `on_phase` deciding
      // on the previous bar's close and stamping its send at the previous instant -- a market order
      // sized and guarded at the wrong value, which Codex round 1 found. This is the order
      // `feed(const BarEvent&)` already uses: mark, price, clock, announce, dispatch.
      runner_.context().set_last_price(event.quote_.instrument_, event.quote_.last_price_);
      runner_.context().set_now(event.quote_.received_at_);
      install_portfolio();
      drain_reports(runner_.feed(clock_.phase()));
      if (result_.halted_) {
        return;
      }
    }

    // Installed before the dispatch for the reason every other dispatch site here installs: a
    // handler reads `ctx.portfolio()`, and the engine holds a fresher answer than the last one it
    // pushed -- an order placed by the *previous* quote has had no settlement pass behind it.
    install_portfolio();
    drain_reports(runner_.feed(event));
    // **After the dispatch, exactly as the bar path does.** An earlier version of this branch left
    // it out, arguing that live a quote poll publishes no order update -- which is true of that one
    // call and false of the iteration it sits in: `WORKING_ORDERS` follows `QUOTES` in
    // `all_poll_kinds`, so an order placed from `on_quote` can be listed moments later and
    // announced. Codex round 0 found it. Publishing here is the closer parity, and it also announces
    // an order that would otherwise fill and be gone before any later observation point.
    publish_order_updates();
  }

  /**
   * Ends the replay, counting whatever never filled.
   *
   * The portfolio is installed first because `Runner::stop` calls `on_stop` and settles what it
   * asks for, and a shutdown handler withdrawing or flattening has to see the orders placed during
   * the final bar. Without this it sees them only when a limit happened to be enabled -- a
   * reservation covering the gap by accident, since `persist_reservation` writes nothing when no
   * limit needs it.
   */
  void finish() {
    if (result_.halted_) {
      // **The strategy is not stopped normally, and that is deliberate.** `Runner::stop` calls
      // `on_stop`, which may place or flatten -- and an order placed into a replay that has already
      // stopped being able to cost one would either fill uncosted or be silently dropped. Neither is
      // a shutdown a reader could interpret, so nothing runs: the queue was cleared at the halt and
      // `aborted_` says how much was in it. The report states that the strategy never saw a stop.
      return;
    }
    install_portfolio();
    drain_reports(runner_.stop());
    // The only boundary that can observe what `on_stop` placed or withdrew. Before `unfilled_` is
    // counted, so the count describes the queue the run actually ended with.
    publish_order_updates();
    result_.unfilled_ = runner_.executor().queued().size();
  }

private:
  /**
   * Clears every order still queued after a halt, counting them.
   *
   * They are withdrawn rather than left outstanding so that nothing downstream reads a terminal
   * replay as one with live working orders, and counted in `aborted_` rather than `unfilled_`
   * because "the data ran out with this resting" and "the replay stopped while this was resting"
   * are different facts about a run.
   */
  void abort_queue() {
    std::vector<OrderRef<Tag>> remaining;
    remaining.reserve(runner_.executor().queued().size());
    for (const auto& entry : runner_.executor().queued()) {
      remaining.push_back(entry.ref_);
    }
    for (const auto& ref : remaining) {
      runner_.executor().withdraw(ref);
    }
    result_.aborted_ = remaining.size();
  }

  /**
   * Returns when `event`'s period began, where that is answerable **from the event itself**.
   *
   * Three cases, in the order they are decided, and the ordering is the correction Codex round 1
   * forced:
   *
   * 1. **An open-stamped bar states it.** `BarStamp::OPEN` means `bar_.timestamp_` *is* the first
   *    instant of the period, for every interval. Nothing has to be derived.
   * 2. **An intraday interval has a fixed length**, so a close-stamped one began that long before it
   *    ended.
   * 3. **A close-stamped daily bar** began at the session open on its own local date, which is the
   *    date its period end falls on and where its open price comes from.
   *
   * **Everything else is unanswerable and says so.** A close-stamped weekly bar ends on Friday and
   * began on Monday; taking the session open on the *period end's* date would have called Friday
   * 09:00 its start, which is four days too late -- so a Tuesday quote would have passed the check
   * and filled at Monday's open, a price from two days before the decision. An earlier version of
   * this function did exactly that and called it conservative; it was the opposite. The same was
   * true of an open-stamped daily bar, whose period end is 24 hours after its start.
   *
   * What an empty answer means to the caller is in `may_fill`: not "no constraint", and not a bound
   * to be inferred from somewhere else either, but "this check cannot run -- allow the fill and
   * record that it was not checked".
   */
  std::optional<std::chrono::system_clock::time_point> period_start_of(
    const strategy::BarEvent& event
  ) const {
    if (event.stamp_ == BarStamp::OPEN) {
      return event.bar_.timestamp_;
    }
    if (const auto duration = interval_duration(event.interval_)) {
      return event.period_end_ - *duration;
    }
    if (event.interval_ == BarInterval::ONE_DAY) {
      return clock_.session().opens_at(event.period_end_);
    }
    return std::nullopt;
  }

  /**
   * Returns whether `entry` may fill against `event` **without being credited a price from before it
   * was decided**, and records where that question could not be answered.
   *
   * Three answers, and the third one is what four review rounds cost:
   *
   * 1. **An undated order is eligible.** That is every order placed by a caller driving
   *    `QueueingExecutor` directly, and this rule exists to defer fills rather than to invent them.
   *    **No test can reach this branch through `Backtester`**, because the engine stamps the instant
   *    before every dispatch that can place -- so a mutation removing it is not caught, and that is
   *    a property of the branch rather than a weakness in the tests. It is kept because
   *    `QueuedOrder::decided_at_` is an optional on a public type, and an undated one is a state the
   *    type admits; nothing outside this class calls this function, so no caller exercises it today.
   * 2. **Where `period_start_of` answers, the comparison is exact**: the bar's period must have begun
   *    **at or after** the decision. A period that began before it contains the decision, and its
   *    open price -- which is what `NextBarFillModel` credits a market order with -- is from before
   *    the strategy decided anything. This is every bar shape this library produces: intraday,
   *    open-stamped, and close-stamped daily.
   * 3. **Where it does not answer -- a close-stamped weekly, monthly or yearly bar -- this engine
   *    makes no inference at all.** It allows the fill, on the bar-counter rule that governed every
   *    fill before this branch, and **counts** it in `ReplayResult::unchecked_fill_causality_` so the
   *    report can say the check did not happen.
   *
   * **Three successive versions of this function tried to infer that period instead, and a reviewer
   * falsified all three.** The previous bar of the instrument: wrong, because two intervals of one
   * instrument overlap. The previous bar of the same instrument *and* interval: wrong, because
   * nothing in `feed`'s contract requires one stamp convention or anchor for a series, so two
   * same-interval weekly bars can overlap too. Each inference rested on an invariant that is usually
   * true and is not guaranteed -- and the repository's own rule covers exactly this: *a conservative
   * refusal beats a narrow test you cannot justify.* There is no refusal available here either,
   * because deferring an unprovable fill would stop a close-stamped weekly replay filling **anything**
   * -- every order in one is decided at a boundary this engine equally cannot place. So the honest
   * third answer is to check nothing, claim nothing, and report the gap.
   */
  bool may_fill(
    const QueuedOrder<Tag>& entry,
    const strategy::BarEvent& event
  ) {
    if (!entry.decided_at_) {
      return true;
    }
    if (const auto start = period_start_of(event)) {
      return !(*start < *entry.decided_at_);
    }
    ++result_.unchecked_fill_causality_;
    return true;
  }

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
      // **And, where the event says enough to check it, it cannot fill against a bar whose period
      // had already begun when it was decided.** The counter above is enough while only bars place
      // orders and no two periods overlap; a quote arrives inside a period without opening one, so
      // without this an order decided at 10:30 fills at that day's 09:00 open -- a price from before
      // the decision. Where the event does *not* say enough, `may_fill` allows the fill and records
      // that nothing was checked. See it for all three answers.
      if (!may_fill(entry, event)) {
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
      auto decision = fills_.decide(order, event, rule, clock_.session().local_date(event.period_end_));
      if (decision.costing_error_) {
        // **Checked before the generic no-execution branch**, because that branch would leave a
        // resting day limit queued to be retried on the next bar, and a refusal is not a bar that
        // failed to reach a price. The order goes whatever its type, exactly once.
        runner_.executor().withdraw(ref);
        result_.halted_ = std::move(*decision.costing_error_);
        // **The halt governs the engine, not just this order.** Returning here stops the other
        // orders eligible on this same bar from filling behind a cost the model could not compute,
        // and `feed` stops consuming bars from its next call onward.
        abort_queue();
        return;
      }
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
      // Stamped from the same reference data the ledger is given, so the two can never disagree
      // about what currency one fill was in.
      decision.execution_->currency_ = currency_for(order.instrument_);
      if (!ledger_.apply(*decision.execution_, currency_for(order.instrument_))) {
        // **A modelled fill the ledger cannot record halts the replay**, for the same reason a cost
        // it cannot compute does. `apply` is atomic, so nothing is half-written -- but the fill
        // happened in the model and did not happen in the books, and continuing would carry on from
        // a portfolio missing a trade the strategy was never told about. This used to `continue`,
        // which recreated at the ledger boundary exactly the silently-missing-trade path the
        // costing refusal above exists to remove.
        BrokerError error;
        error.category_ = ErrorCategory::INVALID_ARGUMENT;
        error.reached_broker_ = false;
        error.broker_code_ = "execution_not_representable";
        error.message_ = "an execution's cash arithmetic does not fit an exact decimal, so the "
          "ledger could not record a fill the model produced";
        result_.halted_ = std::move(error);
        abort_queue();
        return;
      }
      result_.executions_.push_back(*decision.execution_);
      install_portfolio();
      drain_reports(runner_.feed(decision.execution_->fill_));
    }
    // Closes the settlement batch: every withdrawal this bar made is now done, and so is anything
    // an `on_fill` handler placed or cancelled in response to one.
    publish_order_updates();
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
  /**
   * Reports every queued order whose state has changed since the last observation.
   *
   * ## Where this is called from, and why not simply beside `install_portfolio`
   *
   * At the three points where a *batch* of changes to the queue has finished: the end of
   * settlement, the end of `feed`, and the end of `finish`. The queue changes in exactly three
   * ways -- `place` and `cancel` from a strategy dispatch, and `withdraw` from settlement -- and
   * each of those three boundaries closes one batch of them.
   *
   * The obvious alternative, publishing from `install_portfolio`, is wrong and quietly so: the
   * last install of a bar runs *before* `feed(BarEvent)` dispatches `on_bar`, so an order placed
   * there enters the queue after it, and the next bar's settlement withdraws the order before that
   * bar's first install. An order placed on one bar and filled on the next -- which is every market
   * order -- would be present at no install at all and would be reported nowhere.
   *
   * ## The portfolio is installed before each dispatch, not once for the batch
   *
   * A handler told about an order has to be able to find it. With every optional limit disabled
   * `Runner::persist_reservation` writes no working row, so nothing else would put the order in
   * the view, and a strategy would be told about an order it could neither see nor withdraw.
   * Installing once for the batch is not enough either, because a handler answering the first
   * update may place or cancel, which invalidates the picture the second one reads.
   *
   * ## What a handler's own order does
   *
   * It enters the queue behind this observation and is reported at the *next* boundary, never
   * within this one, so a strategy cannot drive itself round a loop here. An order placed by
   * `on_order_update` during `finish` has no later boundary and is not reported at all.
   */
  void publish_order_updates() {
    if (result_.halted_) {
      // Nothing runs after a refusal, for the reason `feed` documents: a handler called here would
      // decide on a replay that has stopped being able to cost what it asks for.
      return;
    }
    std::vector<WorkingOrder<Tag>> rows;
    rows.reserve(runner_.executor().queued().size());
    for (const auto& entry : runner_.executor().queued()) {
      rows.push_back(working_row(entry));
    }
    for (const auto& update : updates_.changed(rows)) {
      // **Re-checked against the queue immediately before it is dispatched, not only when the batch
      // was observed.** A handler answering the first update of a batch can cancel a *different*
      // order in the same batch, and the event captured for that order is then a statement about an
      // order that no longer exists -- the portfolio installed below would correctly omit it while
      // the event announced it, which is exactly the contradiction the install exists to prevent.
      //
      // Checked on the state as well as the reference, because a handler can also replace an order
      // with another carrying the same one.
      const auto* still_queued = find_queued(update);
      if (still_queued == nullptr) {
        continue;
      }
      install_portfolio();
      drain_reports(runner_.feed(update));
      if (result_.halted_) {
        return;
      }
    }
  }

  /**
   * Returns the queued order `update` describes, if it is still outstanding in that same state.
   *
   * Matched on the reference and then on the observable state, so an order withdrawn or amended
   * since the batch was observed is not announced as it stood.
   */
  const QueuedOrder<Tag>* find_queued(const OrderSnapshot& update) const {
    for (const auto& entry : runner_.executor().queued()) {
      if (entry.ref_.primary_ != update.order_id_) {
        continue;
      }
      const auto row = working_row(entry);
      if (runner::order_update_detail::same_observable_state(row.order_, update)) {
        return &entry;
      }
    }
    return nullptr;
  }

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
    // **The replay is this order's broker, so it is the thing that issued the number.** A row left
    // with an empty `order_id_` is an order a reader cannot name, which a live row never is; the
    // reference's primary part is the only identifier a replay has, and it is already monotone and
    // stable for as long as the order is outstanding. Nothing in the library matched a replay
    // working row on this field while it was empty, so filling it changes no existing decision.
    row.order_.order_id_ = entry.ref_.primary_;
    row.order_.instrument_ = entry.order_.instrument_;
    row.order_.side_ = entry.order_.side_;
    row.order_.type_ = entry.order_.type_;
    row.order_.time_in_force_ = entry.order_.time_in_force_;
    row.order_.session_ = entry.order_.session_;
    row.order_.venue_ = entry.order_.venue_;
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
   * checked the order against — and it is an **estimate**, not a bound. A limit buy cannot fill
   * above its limit, so for those the commitment is at least the eventual spend before costs. A
   * market order has no stated price at all: it fills where the next bar opens, and slippage and
   * commission are both outside this figure, so the real spend can exceed what was committed. An
   * order that can be valued by neither route is left uncommitted rather than guessed at.
   *
   * **Scoped to the limit, exactly as `Runner::reserve` is** (`runner/runner.hpp:700-705`).
   * `buying_power_in()` is a public figure a strategy is free to size its next order from, so
   * lowering it for a caller who never enabled the check would change how their strategy sizes --
   * a behaviour change well past enforcing a ceiling, and the reason the runner passes `spends`
   * rather than always debiting. It also keeps `ReportCaveats::buying_power_is_ledger_cash` true
   * for every replay that does not ask for the check.
   */
  void commit_open_buy(strategy::Portfolio<Tag>& portfolio, const QueuedOrder<Tag>& entry) const {
    if (!config_.limits_.enforce_buying_power_ || entry.order_.side_ != OrderSide::BUY) {
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
  /** Turns this replay's own working listing into `on_order_update` events. */
  runner::OrderUpdateWatch<Tag> updates_;
  ReplayResult result_;
  std::optional<strategy::BarEvent> previous_;
};

} // namespace backtest
} // namespace algotradelib
