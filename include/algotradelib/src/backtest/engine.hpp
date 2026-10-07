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
 * @file
 * @brief The backtest engine: `Backtester` replays bars (and quotes) through the same `Runner` a
 *   live session uses.
 *
 * An order decided on bar *n* is queued and filled against bar *n+1* or later, never the bar it
 * was decided on. Nothing assumes a bar is a day: the same engine replays minute and daily bars.
 */

/**
 * @brief One order a replay accepted and has not yet filled or withdrawn.
 *
 * @tparam BrokerTag The order-reference tag.
 */
template <typename BrokerTag>
struct QueuedOrder {
  /** @brief The order as reconciliation planned it. */
  strategy::PlannedOrder order_;
  /** @brief The reference the replay issued for it. */
  OrderRef<BrokerTag> ref_;
  /** @brief The bar it was accepted during; it cannot fill against that bar. */
  std::uint64_t accepted_on_{0};
  /**
   * @brief When the strategy decided the order, if known.
   *
   * Where the engine knows when a bar's period began and this instant is exact, the order may fill
   * only against a bar whose period began at or after it (so a decision at 10:30 is not filled at
   * that day's 09:00 open). A nominal instant (`decision_is_exact_` false) is an upper bound: a
   * start at or after it still proves the order came first, while an earlier start is allowed and
   * counted in `ReplayResult::unchecked_fill_causality_`. Empty (an order placed by a caller driving
   * the executor directly) is always eligible.
   */
  std::optional<std::chrono::system_clock::time_point> decided_at_;
  /**
   * @brief Whether `decided_at_` is exact, or only an upper bound (the nominal period end of an
   *   open-stamped daily or longer bar).
   */
  bool decision_is_exact_{true};
  /**
   * @brief Whether a stop-limit order has been released, so it now rests as its limit.
   *
   * The order itself (and the strategy's working row) still reads as a stop, as a live listing does.
   */
  bool released_{false};
  /**
   * @brief The market-local date of the session a `DAY` order lives in, once known (the date of the
   *   first bar it could trade against); `Backtester` withdraws it when that session ends.
   */
  std::optional<Date> session_;
};

/**
 * @brief The replay's executor: accepts orders and holds them until they fill or are withdrawn.
 *
 * `queued()` is always the complete set of outstanding orders; the engine shows them to the strategy
 * as working orders.
 *
 * @tparam BrokerTag The order-reference tag.
 */
template <typename BrokerTag>
class QueueingExecutor {
public:

  /** @brief `false`: a replay reaches no broker. */
  static constexpr bool reaches_a_broker = false;
  /**
   * @brief Accepts and queues an order, issuing it a reference.
   *
   * A replay invents no transport failure.
   *
   * @param order The order.
   * @return `ACCEPTED`, or `REFUSED_LOCALLY` for an order the replay cannot price from bars (see
   *   `can_model`); a refused order reaches the strategy as a rejection, as a live binding's refusal
   *   does.
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
    entry.decision_is_exact_ = decision_is_exact_;
    last_ref_ = entry.ref_;
    queued_.push_back(std::move(entry));
    return runner::SendOutcome::ACCEPTED;
  }

  /**
   * @brief Tests whether a replay can price an order from bars.
   *
   * Only regular-session limit and market orders without a trigger, and `DAY` stop-limit orders
   * with both a trigger and a limit price. Best-limit, primary-limit, mid-price,
   * conditional-limit and closing-price orders need a book or a session close that bars do not
   * carry.
   *
   * @param order The order.
   * @return `true` if the replay can model it.
   */
  static bool can_model(const strategy::PlannedOrder& order) noexcept {
    if (order.session_ != OrderSession::REGULAR) {
      return false;
    }
    // A stop the model can release: both prices, and a day order (the only stop KIS and Kiwoom offer).
    if (order.type_ == OrderType::STOP_LIMIT) {
      return order.trigger_price_.has_value() && order.price_.has_value() &&
        order.time_in_force_ == TimeInForce::DAY;
    }
    const bool priceable = order.type_ == OrderType::LIMIT || order.type_ == OrderType::MARKET;
    // A trigger on a limit or market order would be ignored; refuse it.
    return priceable && !order.trigger_price_;
  }

  /**
   * @brief Withdraws the order a working row names, matched by reference.
   * @param working The working row.
   * @return `ACCEPTED` if it was queued; `REJECTED` if nothing matches (it filled or was already
   *   withdrawn).
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
   * @brief Returns the reference issued to the most recently accepted order.
   * @return The reference, or `std::nullopt` before any.
   */
  std::optional<OrderRef<BrokerTag>> last_ref() const { return last_ref_; }

  /**
   * @brief Returns every outstanding order.
   * @return The orders, oldest first.
   */
  const std::vector<QueuedOrder<BrokerTag>>& queued() const noexcept { return queued_; }

  /**
   * @brief Finds an outstanding order.
   * @param ref The reference.
   * @return A pointer into the queue (valid until the queue changes), or `nullptr`.
   */
  const QueuedOrder<BrokerTag>* find(const OrderRef<BrokerTag>& ref) const {
    for (const auto& entry : queued_) {
      if (entry.ref_ == ref) {
        return &entry;
      }
    }
    return nullptr;
  }

  /**
   * @brief Records the session date of a queued order; see `QueuedOrder::session_`.
   * @param ref The order.
   * @param session The session's market-local date.
   */
  void assign_session(const OrderRef<BrokerTag>& ref, Date session) {
    for (auto& entry : queued_) {
      if (entry.ref_ == ref) {
        entry.session_ = session;
        return;
      }
    }
  }

  /**
   * @brief Removes an order from the queue.
   * @param ref The order.
   * @return `true` if it was queued.
   */
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
   * @brief Marks a queued stop-limit order as released.
   * @param ref The order.
   * @return `true` if it was queued.
   */
  bool release(const OrderRef<BrokerTag>& ref) {
    for (auto& entry : queued_) {
      if (entry.ref_ == ref) {
        entry.released_ = true;
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Starts the next bar; orders accepted from now on cannot fill against it.
   */
  void open_bar() noexcept { ++bar_; }

  /**
   * @brief Returns the bar being replayed.
   * @return The bar number, counting from one.
   */
  std::uint64_t bar() const noexcept { return bar_; }

  /**
   * @brief Sets the decision instant stamped on orders accepted from now on.
   * @param at The instant.
   * @param exact Whether `at` is exact or only an upper bound; see `QueuedOrder::decision_is_exact_`.
   */
  void deciding_at(std::chrono::system_clock::time_point at, bool exact = true) {
    deciding_at_ = at;
    decision_is_exact_ = exact;
  }

private:
  std::vector<QueuedOrder<BrokerTag>> queued_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::uint64_t issued_{0};
  std::uint64_t bar_{0};
  std::optional<std::chrono::system_clock::time_point> deciding_at_;
  bool decision_is_exact_{true};
};

/** @brief One point on a replay's equity curve, recorded per bar. */
struct EquityPoint {
  /** @brief The bar's period end. */
  std::chrono::system_clock::time_point at_{};
  /** @brief Cash plus positions marked at the bar's close, in the replay's currency. */
  Decimal64 equity_;
  /**
   * @brief The return since the previous point; `missing()` on the first point, after an unvalued bar,
   *   and after a point whose equity was zero or negative (a return on no capital is not defined).
   */
  math::Scalar period_return_{0};
};

/** @brief Everything a replay produced; `summarize` turns it into a `Report`. */
struct ReplayResult {
  /** @brief One point per bar that could be valued, in order. */
  std::vector<EquityPoint> equity_;
  /** @brief Every execution, in order. */
  std::vector<Execution> executions_;
  /**
   * @brief Instructions not placed: reconciliation could not build them, or the replay cannot model
   *   them (`BINDING_REFUSED`; see `QueueingExecutor::can_model`).
   */
  std::vector<strategy::Rejection> rejected_;
  /** @brief Instructions a risk guard refused. */
  std::vector<strategy::GuardRejection> refused_;
  /** @brief Bars that did not follow their predecessor; gaps are not interpolated. */
  std::size_t gaps_{0};
  /** @brief The bars fed. */
  std::size_t bars_{0};
  /** @brief Orders still queued when `finish` ran. */
  std::size_t unfilled_{0};
  /** @brief The currency of the equity curve (the replay's accounting currency). */
  Currency currency_{Currency{"KRW"}};
  /**
   * @brief Why the replay stopped early, if it did.
   *
   * Set when an execution could not be costed (no rate for the date or venue, or a market whose
   * trading day the replay cannot compute), the ledger could not record one, or a position could not
   * be marked at a bar's close (`mark_not_representable`). Every figure then covers the bars up to
   * the halt; the halting bar is counted in `bars_`, but nothing after the refusal ran on it.
   */
  std::optional<BrokerError> halted_;
  /** @brief Orders still working when the replay halted, cleared from the queue (unlike `unfilled_`). */
  std::size_t aborted_{0};
  /** @brief The quotes fed; see `ReportCaveats::quotes_` for what quote-driven orders mean in a replay. */
  std::size_t quotes_{0};
  /**
   * @brief How many fill decisions could not be checked for causality; see
   *   `ReportCaveats::unchecked_fill_causality_`.
   *
   * Besides quotes, an order decided on one interval's bar and filled against another interval's
   * bar of the same instrument (a Tuesday daily bar against that week's weekly bar) can be one.
   * Separately, the nominal period end of an open-stamped monthly or yearly bar also dates its fills
   * and equity points, so a February fill can be dated in March.
   */
  std::size_t unchecked_fill_causality_{0};
  /**
   * @brief Bars whose equity total did not fit an exact decimal, so they have no curve point.
   *
   * The next point carries no return. The replay and its callbacks continue, with per-lot and
   * per-currency figures available, but the missing total also means no portfolio value for
   * weight sizing, so a weight-sized intent on such a bar is rejected (`NO_PORTFOLIO_VALUE`).
   */
  std::size_t unvalued_bars_{0};
  /** @brief The period end of the first unvalued bar, if any. */
  std::optional<std::chrono::system_clock::time_point> first_unvalued_;
  /** @brief Stop-limit orders a bar released; see `ReportCaveats::stop_releases_`. */
  std::size_t stop_releases_{0};
  /** @brief Fills of stops released inside a bar; see `ReportCaveats::stop_fills_intrabar_`. */
  std::size_t stop_fills_intrabar_{0};
  /** @brief Limit-type fills moved to stay in range; see `ReportCaveats::limit_fills_bounded_`. */
  std::size_t limit_fills_bounded_{0};
  /**
   * @brief Bars that closed with cash below zero in `currency_` (counted per bar fed, so two
   *   instruments count both).
   *
   * Under the default limits a replay borrows without limit: nothing refuses an unaffordable buy
   * unless `RiskLimits::enforce_buying_power_` is on, and even then a market buy is checked at the
   * reference price without commission and can overdraw on a gap. Short proceeds count as cash.
   * With negative starting cash every bar counts, while `lowest_cash_` (set by executions) may stay
   * empty.
   */
  std::size_t overdrawn_bars_{0};
  /** @brief The first bar end at which cash was below zero, if any. */
  std::optional<std::chrono::system_clock::time_point> first_overdrawn_;
  /** @brief The lowest cash any execution left in `currency_`, if below zero (checked at every fill). */
  std::optional<Money> lowest_cash_;
  /** @brief When `lowest_cash_` was reached (that execution's `Fill::at_`). */
  std::optional<std::chrono::system_clock::time_point> lowest_cash_at_;
  /** @brief `DAY` orders withdrawn unfilled when their session ended, as a broker does at the close. */
  std::size_t expired_day_orders_{0};
};

/** @brief A replay's settings, besides the strategy and the bars. */
struct ReplayConfig {
  /** @brief The starting cash, in `currency_`. */
  Decimal64 cash_{Decimal64::parse("10000000")};
  /** @brief The accounting currency; the equity curve is in this currency only. */
  Currency currency_{Currency{"KRW"}};
  /** @brief Reference data for every instrument replayed; orders on an instrument without it are refused. */
  std::vector<Instrument> instruments_;
  /** @brief The risk limits, applied as they are live. */
  strategy::RiskLimits limits_;
  /**
   * @brief The session window phases are derived from and `DAY` orders expire at; one for the whole
   *   replay.
   *
   * Set it to the replayed market's session: US bars under the KRX default would expire orders at
   * the KRX close.
   */
  strategy::SessionWindow session_{strategy::SessionWindow::krx()};
  /** @brief How many trailing bars the context caches per series, for indicators; must be positive. */
  std::size_t series_capacity_{512};
  /**
   * @brief The calendar `ctx.calendar()` returns, for a strategy asking about dates it has no bar
   *   for (such as whether tomorrow trades).
   *
   * Empty by default. It does not change the phase of a fed bar, which comes from `feed`'s
   * `trading_day` argument.
   */
  TradingCalendar calendar_;
};

/**
 * @brief Replays bars (and quotes) through one strategy, with a ledger and a fill model.
 *
 * Feed bars with `feed`, in timestamp order across instruments, then call `finish` and `summarize`
 * the `result()`.
 *
 * @code
 * backtest::Backtester<MyStrategy> replay(MyStrategy{}, config);
 * for (const auto& bar : bars) {
 *   replay.feed(strategy::make_bar_event(id, BarInterval::ONE_DAY, BarStamp::CLOSE, bar));
 * }
 * replay.finish();
 * const auto report = backtest::summarize(replay.result(), replay.fills());
 * @endcode
 *
 * @tparam Strategy The strategy type.
 * @tparam CapsType The broker capabilities the strategy sees as `Ctx::caps`.
 */
template <typename Strategy, typename CapsType = strategy::NoCaps>
class Backtester {
public:
  /** @brief The order-reference tag. */
  using Tag = BacktestTag;
  /** @brief The executor type. */
  using Executor = QueueingExecutor<Tag>;
  /** @brief The `runner::Runner` the replay drives, the same type a live session uses. */
  using Runner = runner::Runner<Strategy, CapsType, Tag, Executor>;

  /**
   * @brief Constructs a replay.
   * @param strategy The strategy.
   * @param config The settings.
   * @param fills The fill model (commission, slippage, limit pricing, sale tax).
   */
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
    // So a report can compare an execution's currency with the curve's.
    result_.currency_ = config_.currency_;
  }

  /**
   * @brief Returns the strategy, to read the state it kept.
   * @return The strategy.
   */
  const Strategy& strategy() const noexcept { return runner_.strategy(); }

  /**
   * @brief Returns the ledger: positions, cash and realized profit.
   * @return The ledger.
   */
  const ReplayLedger& ledger() const noexcept { return ledger_; }

  /**
   * @brief Returns the fill model, for `summarize`.
   * @return The fill model.
   */
  const NextBarFillModel& fills() const noexcept { return fills_; }

  /**
   * @brief Returns what the replay has produced so far.
   * @return The result.
   */
  const ReplayResult& result() const noexcept { return result_; }

  /**
   * @brief Replays one bar.
   *
   * In order: fill orders resting from earlier bars against this bar (and call `on_fill`), mark
   * positions at the close and record an equity point, advance the clock to the bar's period end
   * (calling `on_phase` on a change), then call `on_bar`, whose orders wait for a later bar. After a
   * halt it does nothing.
   *
   * @param event The bar; feed bars in timestamp order across instruments. Within one instrument
   *   and interval each bar's timestamp must be later than the last.
   * @param trading_day Whether the bar's date trades, for the phase; empty gives `UNKNOWN`.
   * @throws std::invalid_argument (with a linear algebra backend) if `series_capacity_` is zero, or
   *   the bar is not later than the previous bar of its instrument and interval. The exception
   *   propagates after the bar was counted and settled; it does not set `ReplayResult::halted_`.
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
    //
    // **Whether that instant is exact depends on the stamp**, and getting this wrong deferred real
    // fills by a month. A close-stamped bar's period end is its own timestamp, stated by the broker.
    // An open-stamped one is `bar_period_end`'s arithmetic, which is exact for a minute and
    // deliberately **nominal** for a day, a week, a month and a year -- and every shipped adapter
    // stamps every non-minute bar `OPEN`. February's nominal end is five days into March, so an
    // order decided on February's bar looked as though it had been decided inside March's period and
    // was deferred to April.
    runner_.executor().deciding_at(event.period_end_, period_end_is_exact(event));

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
    // **A mark that will not fit halts the replay**, like a fill it cannot cost or record. The lot
    // stays at its previous price, while `Runner::feed` is about to hand the strategy this bar's
    // close as the last price -- so `on_bar` would decide, and `Runner::settle` would size and check
    // an order, against a position valued one bar ago beside a price from this one. An order placed
    // from that view fills on a later bar and changes every figure after it. This used to skip only
    // the equity point and carry on, which reported nothing and let that order through.
    if (!ledger_.mark(event.instrument_, event.bar_.close_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "mark_not_representable";
      error.message_ = "a position in " + event.instrument_.symbol_ + " could not be valued at the "
        "bar's close without leaving an exact decimal, so the replay stopped rather than let a "
        "strategy decide on a position one bar behind its price";
      result_.halted_ = std::move(error);
      abort_queue();
      return;
    }
    record_equity(event);

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
   * @brief Replays one quote: moves the price and the clock to it, re-derives the phase (calling
   *   `on_phase` on a change), calls `on_quote`, and publishes order updates.
   *
   * A quote does not mark the ledger, record an equity point or fill anything. An order placed from
   * `on_quote` fills against a later bar, not the quote (see `ReportCaveats::quotes_`). Compared
   * with a live loop, `on_phase` fires before `on_quote` here (live, after the iteration's polls),
   * and order updates are always published here (live, only when the working-order poll runs).
   * A quote before the first bar is fine; its orders wait for a bar.
   *
   * @param event The quote; feed it in timestamp order with the bars, since the clock follows
   *   whatever instant it is given, even backwards.
   * @param trading_day Whether the quote's date trades; empty gives `UNKNOWN`.
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
      // sized and guarded at the wrong value. This is the order
      // `feed(const BarEvent&)` already uses: mark, price, clock, announce, dispatch.
      runner_.context().set_last_price(event.quote_.instrument_, event.quote_.last_price_);
      runner_.context().set_now(event.quote_.received_at_);
      install_portfolio();
      drain_reports(runner_.feed(clock_.phase()));
    }

    // Installed before the dispatch for the reason every other dispatch site here installs: a
    // handler reads `ctx.portfolio()`, and the engine holds a fresher answer than the last one it
    // pushed -- an order placed by the *previous* quote has had no settlement pass behind it.
    install_portfolio();
    drain_reports(runner_.feed(event));
    // **After the dispatch, exactly as the bar path does.** An earlier version left
    // it out, arguing that live a quote poll publishes no order update -- which is true of that one
    // call and false of the loop it sits in: an order placed from `on_quote` can be listed by a later
    // working-order poll and announced. Publishing here is the closer parity, and it also
    // announces an order that would otherwise fill and be gone before any later observation point.
    publish_order_updates();
  }

  /**
   * @brief Ends the replay: calls `on_stop` (with the current portfolio, including orders from the
   *   last bar), settles what it asked for, and counts the orders still queued.
   *
   * After a halt it does nothing; `on_stop` is not called.
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
  // Clears every order still queued after a halt, counting them.
  //
  // They are withdrawn rather than left outstanding so that nothing downstream reads a terminal
  // replay as one with live working orders, and counted in `aborted_` rather than `unfilled_`
  // because "the data ran out with this resting" and "the replay stopped while this was resting"
  // are different facts about a run.
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

  // Returns whether `event`'s period end is the instant itself rather than an upper bound on it.
  //
  // A close-stamped bar carries its period end as its timestamp, so the broker stated it. An
  // open-stamped bar's end is derived by adding the interval's nominal length, which is right for a
  // minute and an approximation for every calendar interval -- 24 hours is not a trading day, 31 days
  // is not a month, and 366 is not a year. The approximation always runs **long**, which is what
  // makes it usable: an upper bound on the decision instant still proves causality whenever a bar's
  // start is at or after it.
  static bool period_end_is_exact(const strategy::BarEvent& event) noexcept {
    return event.stamp_ == BarStamp::CLOSE || event.interval_ == BarInterval::ONE_MINUTE;
  }

  // Returns when `event`'s period began, where that is answerable **from the event itself**.
  //
  // Three cases, in the order they are decided; the order matters:
  //
  // 1. **An open-stamped bar states it.** `BarStamp::OPEN` means `bar_.timestamp_` *is* the first
  //    instant of the period, for every interval. Nothing has to be derived.
  // 2. **An intraday interval has a fixed length**, so a close-stamped one began that long before it
  //    ended.
  // 3. **A close-stamped daily bar** began at the session open on its own local date, which is the
  //    date its period end falls on and where its open price comes from. **That assumes the stamp
  //    falls on the session's own date**, which is true of a bar stamped at the close and false of
  //    one stamped at the following local midnight; such a series gets a start a day late.
  //
  // **Everything else is unanswerable and says so.** A close-stamped weekly bar ends on Friday and
  // began on Monday; taking the session open on the *period end's* date would have called Friday
  // 09:00 its start, which is four days too late -- so a Tuesday quote would have passed the check
  // and filled at Monday's open, a price from two days before the decision. An earlier version of
  // this function did exactly that and called it conservative; it was the opposite. The same was
  // true of an open-stamped daily bar, whose period end is 24 hours after its start.
  //
  // What an empty answer means to the caller is in `may_fill`: not "no constraint", and not a bound
  // to be inferred from somewhere else either, but "this check cannot run -- allow the fill and
  // record that it was not checked".
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

  // Returns whether `entry` may fill against `event`, and records where the question *could this be
  // credited a price from before it was decided* could not be answered at all.
  //
  // Three answers:
  //
  // 1. **An undated order is eligible.** That is every order placed by a caller driving
  //    `QueueingExecutor` directly, and this rule exists to defer fills rather than to invent them.
  //    **No test can reach this branch through `Backtester`**, because the engine stamps the instant
  //    before every dispatch that can place -- so a mutation removing it is not caught, and that is
  //    a property of the branch rather than a weakness in the tests. It is kept because
  //    `QueuedOrder::decided_at_` is an optional on a public type, and an undated one is a state the
  //    type admits; nothing outside this class calls this function, so no caller exercises it today.
  // 2. **Where `period_start_of` answers, the comparison is exact**: the bar's period must have begun
  //    **at or after** the decision. A period that began before it contains the decision, and its
  //    open price -- which is what `NextBarFillModel` credits a market order with -- is from before
  //    the strategy decided anything. This is every bar shape this library produces: intraday,
  //    open-stamped, and close-stamped daily.
  // 3. **Where either side of that comparison is a calendar boundary this engine cannot establish,
  //    it makes no inference at all.** Two ways in, and the second was found last: the bar's start is
  //    underivable (a close-stamped weekly, monthly or yearly bar), or the decision instant is only
  //    an upper bound (an open-stamped bar of any calendar interval, which is what every shipped
  //    adapter produces) and the bar's start falls before it. Either way the fill is allowed, on the
  //    bar-counter rule that governed every fill before this check existed, and **counted** in
  //    `ReplayResult::unchecked_fill_causality_` so the report can say the check did not happen.
  //
  // **Three successive versions of this function tried to infer that period instead, and all three
  // were wrong.** The previous bar of the instrument: wrong, because two intervals of one
  // instrument overlap. The previous bar of the same instrument *and* interval: wrong, because
  // nothing in `feed`'s contract requires one stamp convention or anchor for a series, so two
  // same-interval weekly bars can overlap too. Each inference rested on an invariant that is usually
  // true and is not guaranteed -- and the repository's own rule covers exactly this: *a conservative
  // refusal beats a narrow test you cannot justify.* There is no refusal available here either,
  // because deferring an unprovable fill would stop a close-stamped weekly replay filling **anything**
  // -- every order in one is decided at a boundary this engine equally cannot place. That is a
  // statement about the shape rather than about every interval in it: a sound lower bound of *period
  // end minus seven days* does exist for a week and would let a full-week series fill on time, but it
  // defers a week after every holiday-shortened one, and the 31-day and 366-day bounds defer nearly
  // every monthly and yearly order. So the honest third answer is to check nothing, claim nothing,
  // and report the gap.
  bool may_fill(
    const QueuedOrder<Tag>& entry,
    const strategy::BarEvent& event
  ) {
    if (!entry.decided_at_) {
      return true;
    }
    if (const auto start = period_start_of(event)) {
      if (!(*start < *entry.decided_at_)) {
        // Proven: the period began at or after the decision. A *nominal* decision instant is an
        // upper bound on the true one, so this proves it for those too.
        return true;
      }
      if (entry.decision_is_exact_) {
        return false;
      }
      // The start is known and the decision is only bounded, so this is the unprovable case again
      // from the other side: the order may have been decided before this period after all. Allowed
      // and counted, on the same terms as an underivable start.
      ++result_.unchecked_fill_causality_;
      return true;
    }
    ++result_.unchecked_fill_causality_;
    return true;
  }

  // Fills orders resting from an earlier bar against `event`, and tells the strategy.
  //
  // **Walked by reference rather than by iterator or index.** Applying a fill calls `on_fill`, which
  // may place or withdraw orders, so any position into the queue is invalid the moment the strategy
  // is told anything. The eligible references are collected first and each is looked up again when
  // its turn comes; one the callback withdrew is simply no longer there.
  //
  // The queue is never emptied, so an order this loop has not reached yet is outstanding, visible in
  // every portfolio installed along the way, and addressable by a cancellation from any callback.
  void settle_queued_against(const strategy::BarEvent& event) {
    const auto bar_date = clock_.session().local_date(event.bar_.timestamp_);
    // A session the replay had no closing bar for -- a missing last minute -- still ended: an order
    // of an earlier date is withdrawn before this bar can fill it.
    expire_day_orders(event, bar_date, false);
    std::vector<OrderRef<Tag>> eligible;
    std::vector<OrderRef<Tag>> first_session;
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
      // **Which session a DAY order lives in**, fixed at the first bar it is eligible against: that
      // bar's session is the one the order trades in. A decision the bar cannot fill without looking
      // ahead -- a quote at 10:00 against the day's bar, or the day's close -- trades in the next
      // session, as a market order decided there fills at the next open: one session, shifted, never
      // the several a resting order had before. Recorded here, beside the one `may_fill` call, so
      // `unchecked_fill_causality_` still counts each order once per bar.
      if (!entry.session_ && entry.order_.time_in_force_ == TimeInForce::DAY) {
        first_session.push_back(entry.ref_);
      }
    }
    for (const auto& ref : first_session) {
      runner_.executor().assign_session(ref, bar_date);
    }

    for (const auto& ref : eligible) {
      const auto* entry = runner_.executor().find(ref);
      if (entry == nullptr) {
        // Withdrawn by a callback earlier in this loop. Nothing to fill.
        continue;
      }
      const auto order = entry->order_;
      const auto rule = rule_for(order.instrument_);
      // An `OPEN`-stamped bar states its start, so a fill at its open has a known day; the period
      // end's day is nominal there -- a day late for a daily bar, days into the next month for a
      // monthly one. See `NextBarFillModel::decide`'s `open_date`.
      const auto open_date = event.stamp_ == BarStamp::OPEN
        ? std::optional<Date>(clock_.session().local_date(event.bar_.timestamp_))
        : std::nullopt;
      auto decision = fills_.decide(
        order, event, rule, clock_.session().local_date(event.period_end_), open_date,
        entry->released_
      );
      if (decision.released_) {
        // Recorded before anything below can withdraw the order, and whether or not it filled: a stop
        // that released and still rests is a limit from the next bar on.
        runner_.executor().release(ref);
        ++result_.stop_releases_;
      }
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
      note_overdraft(*decision.execution_);
      if (decision.intrabar_release_fill_) {
        ++result_.stop_fills_intrabar_;
      }
      if (decision.bounded_) {
        ++result_.limit_fills_bounded_;
      }
      result_.executions_.push_back(*decision.execution_);
      install_portfolio();
      drain_reports(runner_.feed(decision.execution_->fill_));
    }
    expire_day_orders(event, bar_date, true);
    // Closes the settlement batch: every withdrawal this bar made is now done, and so is anything
    // an `on_fill` handler placed or cancelled in response to one.
    publish_order_updates();
  }

  // Keeps the lowest cash an execution left in the replay's currency, when it is below zero.
  void note_overdraft(const Execution& execution) {
    if (!(execution.currency_ == result_.currency_)) {
      return;
    }
    const auto cash = ledger_.cash_in(execution.currency_);
    if (!cash || !decimal_less(*cash, Decimal64::parse("0"))) {
      return;
    }
    if (!result_.lowest_cash_ || decimal_less(*cash, result_.lowest_cash_->amount_)) {
      result_.lowest_cash_ = Money{*cash, execution.currency_};
      result_.lowest_cash_at_ = execution.fill_.at_;
    }
  }

  // Withdraws every DAY order of `event`'s instrument whose session has ended: before the bar's
  // fills (`after_fills` false), those of an earlier date; after them, also those of the bar's own
  // date when the bar ends the session.
  //
  // A KRX DAY order expires at the close. Kept queued, a replay's DAY limit or stop was
  // good-till-cancelled on daily bars -- a limit below the market waited for weeks, and a stop placed
  // at an entry still rested months later -- where live it was gone that evening. A bar ends a
  // session when its interval is a day or longer, or when it is a minute bar ending at or after the
  // session's close; a bar of a later date ends it in any case. Withdrawn inside the settlement batch,
  // before order updates are published and before the strategy is shown the bar, so nothing reads an
  // expired order as working. Counted in `ReplayResult::expired_day_orders_`.
  void expire_day_orders(const strategy::BarEvent& event, const Date& bar_date, bool after_fills) {
    // No fixed duration is what a day or longer has (`interval_duration`); an intraday interval has one.
    const bool daily_or_longer = !interval_duration(event.interval_);
    const auto closes = clock_.session().opens_at(event.bar_.timestamp_) - clock_.session().open_ +
      clock_.session().close_;
    const bool ends_session = after_fills && (daily_or_longer || !(event.period_end_ < closes));
    std::vector<OrderRef<Tag>> expired;
    for (const auto& entry : runner_.executor().queued()) {
      if (!(entry.order_.instrument_ == event.instrument_) || !entry.session_ ||
          entry.order_.time_in_force_ != TimeInForce::DAY) {
        continue;
      }
      if (*entry.session_ < bar_date || (*entry.session_ == bar_date && ends_session)) {
        expired.push_back(entry.ref_);
      }
    }
    for (const auto& ref : expired) {
      runner_.executor().withdraw(ref);
      ++result_.expired_day_orders_;
    }
  }

  // Records equity at this bar, and the return since the previous point.
  void record_equity(const strategy::BarEvent& event) {
    if (const auto cash = ledger_.cash_in(config_.currency_);
        cash && decimal_less(*cash, Decimal64::parse("0"))) {
      ++result_.overdrawn_bars_;
      if (!result_.first_overdrawn_) {
        result_.first_overdrawn_ = event.period_end_;
      }
    }
    const auto equity = ledger_.equity_in(config_.currency_);
    if (!equity) {
      // Every lot was marked, and the total of cash and lots still does not fit an exact decimal.
      // No point is recorded -- a point at an approximate total would be a figure nobody computed
      // -- and the omission is counted, because a gap in the curve has other causes too.
      ++result_.unvalued_bars_;
      if (!result_.first_unvalued_) {
        result_.first_unvalued_ = event.period_end_;
      }
      equity_gap_ = true;
      return;
    }
    EquityPoint point;
    point.at_ = event.period_end_;
    point.equity_ = *equity;
    point.period_return_ = math::missing();
    // **No return across an omitted point.** The one below would measure two or more bars and be
    // handed to the Sharpe ratio and the annualization as one bar's return, so the first point
    // after a gap carries none -- as the first point of the run does.
    const bool after_gap = equity_gap_;
    equity_gap_ = false;
    if (!result_.equity_.empty() && !after_gap) {
      const auto& previous = result_.equity_.back().equity_;
      // Only from positive equity. From a negative total the ratio's sign is reversed: a further loss
      // reads as a gain, and a run that lost more than its capital reported a positive Sharpe ratio.
      if (previous.coefficient() > 0) {
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

  // Converts an exact ratio to the scalar the metrics compute in.
  //
  // The one crossing of the decimal boundary in this engine, and it goes outward: a return is an input
  // to `sharpe` and the rest, which are defined on `Scalar`. Nothing converts back, because no figure
  // a report produces is ever sent to a broker.
  static math::Scalar analytics_to_scalar(const Decimal64& value) {
    auto scaled = static_cast<math::Scalar>(value.coefficient());
    for (std::uint8_t at = 0; at < value.scale(); ++at) {
      scaled /= math::Scalar{10};
    }
    return scaled;
  }

  // Returns the grid for `instrument`, unknown when no reference data was supplied.
  // Returns the currency `instrument` trades in, from the reference data the replay was given.
  //
  // **Every fill used to be booked in `config_.currency_`**, which is documented as the currency the
  // *starting cash* is in. A replay of a dollar-quoted instrument therefore banked dollars as won at
  // a rate of one -- an implicit currency conversion, which this library never makes silently.
  //
  // The fallback is defensive rather than a policy: `reconcile` refuses an instrument with no
  // reference data before a `PlannedOrder` exists (`strategy/reconcile.hpp`), so nothing can reach
  // the fill model unregistered. It is written the conservative way round anyway -- an unregistered
  // instrument books in the currency the ledger was opened in, which is the behaviour every existing
  // replay already had.
  // Installs the portfolio a strategy and its guards decide on.
  //
  // The single place the replay answers "what does this account look like", so that every callback
  // boundary is answered the same way.
  //
  // **It has to be a complete answer, because it overwrites one.**
  // `Runner::persist_reservation` writes an accepted order into the context portfolio and relies on
  // a wholesale replacement carrying what the reservation was standing in for
  // (`runner/runner.hpp:628-634`). Live that replacement is a broker's listing. Here it is this
  // function, and a partial answer is therefore not a smaller improvement -- it is a *loss*, because
  // the reservation it erased was carrying the part it left out.
  //
  // The reservation carried two things, so this carries both: the outstanding orders themselves,
  // and the money an outstanding buy has committed.
  // Reports every queued order whose state has changed since the last observation.
  //
  // ## Where this is called from, and why not simply beside `install_portfolio`
  //
  // At the three points where a *batch* of changes to the queue has finished: the end of
  // settlement, the end of `feed`, and the end of `finish`. The queue changes in exactly three
  // ways -- `place` and `cancel` from a strategy dispatch, and `withdraw` from settlement -- and
  // each of those three boundaries closes one batch of them.
  //
  // The obvious alternative, publishing from `install_portfolio`, is wrong and quietly so: the
  // last install of a bar runs *before* `feed(BarEvent)` dispatches `on_bar`, so an order placed
  // there enters the queue after it, and the next bar's settlement withdraws the order before that
  // bar's first install. An order placed on one bar and filled on the next -- which is every market
  // order -- would be present at no install at all and would be reported nowhere.
  //
  // ## The portfolio is installed before each dispatch, not once for the batch
  //
  // A handler told about an order has to be able to find it. With every optional limit disabled
  // `Runner::persist_reservation` writes no working row, so nothing else would put the order in
  // the view, and a strategy would be told about an order it could neither see nor withdraw.
  // Installing once for the batch is not enough either, because a handler answering the first
  // update may place or cancel, which invalidates the picture the second one reads.
  //
  // ## What a handler's own order does
  //
  // It enters the queue behind this observation and is reported at the *next* boundary, never
  // within this one, so a strategy cannot drive itself round a loop here. An order placed by
  // `on_order_update` during `finish` has no later boundary and is not reported at all.
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
    // **Once, after the whole batch.** Every pass that can withdraw a stop -- the bar's, the phase's,
    // the fills' and these updates' -- ends here, so the strategy has answered every update before a
    // fallback is released; releasing after each pass let a later update's instruction arrive after
    // the fallback had already been queued. The live path waits for a whole listing the same way.
    settle_deferred();
  }

  // Returns the queued order `update` describes, if it is still outstanding in that same state.
  //
  // Matched on the reference and then on the observable state, so an order withdrawn or amended
  // since the batch was observed is not announced as it stood.
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
    // The value a weight is sized against: the ledger's equity in the replay's currency at the
    // latest marks, which is what the equity curve records. Cleared when that total does not fit,
    // so a weight is refused on such a bar rather than sized against the previous one.
    if (const auto equity = ledger_.equity_in(config_.currency_)) {
      runner_.set_portfolio_value(Money{*equity, config_.currency_});
    } else {
      runner_.clear_portfolio_value();
    }
    auto portfolio = ledger_.snapshot<Tag>(clock_.now());
    for (const auto& entry : runner_.executor().queued()) {
      portfolio.working_.push_back(working_row(entry));
      commit_open_buy(portfolio, entry);
    }
    runner_.context().set_portfolio(std::move(portfolio));
  }

  // Returns the working-order row for an outstanding replay order.
  //
  // `reserved_` stays false, unlike a runner's own reservation. The flag distinguishes this
  // library's arithmetic from what a broker reported, and in a replay the engine *is* the broker:
  // this row is the listing. The one consequence worth stating is that
  // `strategy::SellableConventionProbe`, which reads unreserved rows as evidence about a broker's
  // convention, will read these -- and a replay's sellable figure is a model that never moves, so
  // the probe reports what the simulator does rather than what a broker does.
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
    // A stop's trigger, as a live listing reports it -- released or not, since neither broker's
    // listing says which.
    row.order_.trigger_price_ = entry.order_.trigger_price_;
    row.order_.quantity_ = entry.order_.quantity_;
    row.order_.currency_ = currency_for(entry.order_.instrument_);
    // Open, and nothing filled: a replay fills an order whole or not at all, so a row that exists
    // has traded nothing.
    row.order_.status_ = OrderStatus::PENDING;
    row.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    row.amendable_quantity_ = entry.order_.quantity_;
    return row;
  }

  // Takes what an outstanding buy has committed out of the buying power the replay models.
  //
  // **Rows alone do not fix `enforce_buying_power_`**, which reads the scalar and not the orders.
  // `ReplayLedger::snapshot` builds buying power from ledger cash, and that cash has not paid for
  // anything still resting -- so with a hundred of cash, a resting buy costing a hundred, and a
  // second buy costing a hundred in another instrument, the second passes and both can still fill.
  //
  // Valued at the order's own price, else the last close, which is the same estimate the guard
  // checked the order against — and it is an **estimate**, not a bound. A limit buy cannot fill
  // above its limit, so for those the commitment is at least the eventual spend before costs. A
  // market order has no stated price at all: it fills where the next bar opens, and slippage and
  // commission are both outside this figure, so the real spend can exceed what was committed. An
  // order that can be valued by neither route is left uncommitted rather than guessed at.
  //
  // **Scoped to the limit, exactly as `Runner::reserve` is** (`runner/runner.hpp:700-705`).
  // `buying_power_in()` is a public figure a strategy is free to size its next order from, so
  // lowering it for a caller who never enabled the check would change how their strategy sizes --
  // a behaviour change well past enforcing a ceiling, and the reason the runner passes `spends`
  // rather than always debiting. It also keeps `ReportCaveats::buying_power_is_ledger_cash` true
  // for every replay that does not ask for the check.
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

  // Collects what a runner pass reported, so a replay ends with the whole picture.
  template <typename Report>
  void drain_reports(const Report& report) {
    collect(report);
  }

  // Adds a pass's refusals to the result.
  template <typename Report>
  void collect(const Report& report) {
    for (const auto& rejection : report.rejected_) {
      result_.rejected_.push_back(rejection);
    }
    for (const auto& refusal : report.refused_) {
      result_.refused_.push_back(refusal);
    }
  }

  // Settles a target the runner deferred behind a stop, once this replay has withdrawn the stop and
  // the strategy has answered every update of the batch. Called only from `publish_order_updates`.
  //
  // **A replay's withdrawal is synchronous**: `QueueingExecutor::cancel` removes the stop from the
  // queue before the pass that asked for it returns. Waiting for the next bar's portfolio to show that
  // would place the flatten a bar late and fill it two bars late -- a delay the live path, whose
  // cancellation can be confirmed between bars, does not have. So after every pass the replay shows
  // the runner its book as it now stands and lets it release from that one snapshot: this ledger is the
  // broker, so the stop's absence and the holding after it are known together. See
  // `Runner::retry_deferred_on_one_snapshot`. An order placed here waits for the next bar like any
  // other, which is the lookahead rule.
  void settle_deferred() {
    if (runner_.deferred_targets().empty()) {
      return;
    }
    install_portfolio();
    collect(runner_.retry_deferred_on_one_snapshot());
  }

  ReplayConfig config_;
  NextBarFillModel fills_;
  ReplayLedger ledger_;
  ReplayClock clock_;
  Runner runner_;
  // Turns this replay's own working listing into `on_order_update` events.
  runner::OrderUpdateWatch<Tag> updates_;
  ReplayResult result_;
  // Whether a bar since the last equity point went unvalued, so the next point carries no return.
  bool equity_gap_{false};
  std::optional<strategy::BarEvent> previous_;
};

} // namespace backtest
} // namespace algotradelib
