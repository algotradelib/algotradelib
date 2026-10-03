#pragma once

#include "../runner/loop.hpp"
#include "../runner/order_updates.hpp"
#include "../runner/reconcile_orders.hpp"

#include <algorithm>
#include <algotradelib/broker>

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace live {

/**
 * Returns what a failed inquiry means for a poll.
 *
 * The distinction that matters is `RATE_LIMIT`, which is **not a failure**: it is the limiter working
 * as designed, and counting it toward a failure budget would make a loop polling slightly too fast
 * shut itself down instead of simply slowing down. Everything else is a genuine failure — but note
 * that a failure here is far less consequential than one on an order write: nothing was sent, so
 * nothing needs reconciling and no ledger entry is owed. A quote that did not arrive is an absence of
 * information, not an unknown position.
 *
 * That asymmetry is why this is a separate function from `runner::outcome_of` rather than a reuse of
 * it. They answer different questions: that one asks whether an order might exist, this one only
 * asks whether to try again.
 */
inline runner::PollOutcome poll_outcome_of(const BrokerError& error) noexcept {
  return error.category_ == ErrorCategory::RATE_LIMIT ?
    runner::PollOutcome::RATE_LIMITED :
    runner::PollOutcome::FAILED;
}

namespace session_source_detail {

/**
 * Whether `Runner` can be fed a bar as history, which every `runner::Runner` can. Required by
 * `poll_bars`: a runner without it would send what was decided on a cold start's history.
 */
template <typename Runner, typename = void>
struct feeds_warmup : std::false_type {};

template <typename Runner>
struct feeds_warmup<
  Runner,
  std::void_t<decltype(std::declval<Runner&>().feed_warmup(std::declval<const strategy::BarEvent&>()))>
> : std::true_type {};

} // namespace session_source_detail

/**
 * Polls a bound session and feeds what arrives to a runner.
 *
 * The only part of the live loop that performs I/O, and the counterpart to `SessionExecutor`: that one
 * is the only thing that *writes* to a broker, this one the only thing that *reads* on a loop's
 * behalf. Everything about when to poll, when to stop and when to refuse to send lives in
 * `runner::PollingLoop`, which is why all of that is testable with `runner::ManualSource` and no
 * network.
 *
 * **Rate limiting needs nothing here.** Each client already tags its own requests — `MARKET_DATA` for
 * quotes and bars, `ACCOUNT` for holdings and order inquiries, `TRADING` only for placements and
 * cancellations — and `RateLimitController` holds `trading_reserve_` back from every non-trading
 * group. So a loop polling hard cannot starve an order of capacity, and this class does not have to
 * arrange that; it would only be able to get it wrong.
 *
 * **Capabilities are compile-time.** A broker with no working-order inquiry compiles no call to one,
 * and the loop simply never polls that kind. Nothing is checked at run time and nothing throws.
 *
 * **Reconciliation is opt-in and costs no extra request.** Bind an order journal with
 * `set_journal` and every listing this source reads is also swept against what the journal says is
 * owed, resolving the runner's ledger where the broker's own rows settle the question. Without a
 * journal the class behaves exactly as it did before: the gate still lifts on a clean start, and
 * nothing is matched.
 *
 * **Market data may come from a second session.** `MarketData` serves the four market reads (bars,
 * quotes, price limits, calendar) and `Session` everything about the account. A KIS demo account,
 * whose host refuses the calendar (`CTCA0903R`, `EGW02006`), reads it through a session built from
 * the real account's keys and an empty `KisAccount`, which refuses every account request and order
 * before any I/O. `MarketData` may also be another broker's session. The two-argument constructor
 * reads both from one session, as before.
 */
template <
  typename Session,
  typename Journal = runner::MemoryJournal,
  typename MarketData = Session
>
class SessionSource {
public:
  /**
   * One figure of one order, and **the filled quantity it was accepted at**.
   *
   * The quantity lives per figure rather than per order, and review found why it has to: a silent
   * row advanced the order's quantity while the figure stayed behind at the old one, so the fee
   * that then arrived for the *new* quantity looked like a second opinion about the old one and was
   * discarded as a conflict. What decides whether a reading is new information is the quantity
   * **this figure** was last accepted at, not how far the order has filled since.
   */
  struct ObservedFigure {
    /** The amount the broker stated, empty until it has stated one. */
    std::optional<Money> amount_;
    /** The filled quantity `amount_` was accepted at. */
    Decimal64 at_;
  };

  /** One order's stated charges, as the accumulator last accepted them. */
  struct ObservedCharge {
    /** The commission the broker stated, with the quantity it was accepted at. */
    ObservedFigure commission_;
    /** The tax the broker stated, with the quantity it was accepted at. */
    ObservedFigure tax_;
    /** Whether this order has already been counted as one that states a cost. */
    bool counted_cost_{false};
  };

  /** The bound broker's capability set, forwarded from the session. */
  using Caps = typename Session::Caps;
  /** The bound broker's tag, forwarded from the session. */
  using BrokerTag = typename Session::BrokerTag;

  /**
   * Whether a successful order-list read leaves the broker's outstanding orders in the portfolio.
   *
   * False on a broker without the working-order inquiry, and that is not a technicality. The
   * fallback path reads *history* and hands its rows to journal reconciliation, which is enough for
   * the startup gate -- this process has looked at the broker's own record before sending -- and is
   * not enough to build a withdrawal from, because `portfolio.working_` is never populated. A
   * `PollingLoop` reads this to decide whether a closing pass knows what is outstanding; telling it
   * yes here would make a `WITHDRAW` report a complete stop with an order still resting.
   */
  static constexpr bool provides_working_orders = Caps::has_working_orders;

  /**
   * Binds a session and the universe to poll.
   *
   * The session is borrowed, matching `SessionExecutor`: it owns a client with executor affinity, and
   * neither a runner nor a source owns the connection it uses.
   */
  SessionSource(Session& session, std::vector<InstrumentId> universe)
  : session_(&session), market_data_(&session), universe_(std::move(universe)) {}

  /**
   * Binds a trading session, a session for the market reads, and the universe to poll.
   *
   * `market_data` is borrowed too. Only bars, quotes, price limits and the calendar are read from it,
   * gated on its own capabilities. Nothing is sent through either: orders go through the executor
   * the runner was bound with.
   */
  SessionSource(Session& session, MarketData& market_data, std::vector<InstrumentId> universe)
  : session_(&session), market_data_(&market_data), universe_(std::move(universe)) {}

  /**
   * Binds the journal whose unresolved instructions each listing is swept against.
   *
   * Borrowed rather than owned, and deliberately the *same* journal the executor writes to: one
   * file, one writer, and one reader that knows what the writer recorded.
   *
   * **A sweep happens wherever rows arrive**, which is the startup read and every working-order
   * poll, because both already fetch exactly what the matcher needs. A broker with no working-order
   * inquiry has only the startup read, and that asymmetry is a consequence of what the broker
   * offers rather than a choice made here.
   */
  void set_journal(Journal& journal) { journal_ = &journal; }

  /** Sets how much slack attribute matching allows. */
  void set_match_window(runner::MatchWindow window) { window_ = window; }

  /** Returns what the last sweep settled and what it left owed. */
  const runner::ReconcileReport& last_reconcile() const noexcept { return reconcile_; }

  /**
   * Sets which currencies to ask about what may be spent.
   *
   * Empty by default, and a buying-power poll with none configured does nothing rather than
   * guessing one: a broker prices the answer per currency, and picking a default here would size a
   * won order against a dollar budget on an account holding both.
   */
  void set_buying_power_currencies(std::vector<Currency> currencies) {
    currencies_ = std::move(currencies);
  }

  /**
   * Returns the currencies configured above, which is what a complete poll has to cover.
   *
   * Published so the loop's gate can hold for a budget that never arrived. Without it the gate can
   * only ask whether *any* reading exists, and one currency answering would open the gate for an
   * account trading two -- the order guard would still refuse the unread one, but the run would
   * report healthy polls while a configured budget was missing.
   */
  const std::vector<Currency>& buying_power_currencies() const noexcept { return currencies_; }

  /**
   * Returns how many bars a poll met before their period had ended, and left for later.
   *
   * Nonzero is normal for a daily query polled during the session, and it is the evidence that the
   * broker publishes the session still trading. Monotone across every run against this source.
   */
  std::size_t bars_not_yet_closed() const noexcept { return bars_not_yet_closed_; }

  /**
   * Returns how many bars were fed as history (`Runner::feed_warmup`): every new bar of a read but each
   * instrument's newest, and a newest bar whose period did not end after all of those. Their
   * decisions were discarded, never sent. Monotone.
   */
  std::size_t bars_fed_as_warmup() const noexcept { return bars_fed_as_warmup_; }

  /** Sets the bar interval and depth each poll requests. */
  void set_bar_query(BarInterval interval, int count) {
    interval_ = interval;
    count_ = count;
  }

  /**
   * Sets which market's trading calendar to ask for.
   *
   * `Country::KR` by default: it is the domestic market of both brokers this library ships, the only
   * one KIS publishes at all, and the one `strategy::SessionWindow::krx()` states the hours for. A
   * wrong value is refused by the port before a socket is written -- KIS's `validate_calendar` names
   * the reason -- rather than answering about the wrong market.
   */
  void set_calendar_country(Country country) { calendar_country_ = country; }

  /** Returns the market the calendar poll asks about. */
  Country calendar_country() const noexcept { return calendar_country_; }

  /** Returns why the last failing poll failed, for a caller to log. */
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

  /** Polls `kind` and feeds whatever arrived to `runner`. */
  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    switch (kind) {
      case runner::PollKind::CALENDAR:
        return poll_calendar(runner_instance);
      case runner::PollKind::BARS:
        return poll_bars(runner_instance);
      case runner::PollKind::QUOTES:
        return poll_quotes(runner_instance);
      case runner::PollKind::HOLDINGS:
        return poll_holdings(runner_instance);
      case runner::PollKind::WORKING_ORDERS:
        return poll_working_orders(runner_instance);
      case runner::PollKind::BUYING_POWER:
        return poll_buying_power(runner_instance);
      case runner::PollKind::PRICE_LIMITS:
        return poll_price_limits(runner_instance);
    }
    return runner::PollOutcome::UNCHANGED;
  }

  /**
   * Reads the broker's own order list, which is what lifts the loop's startup gate.
   *
   * **Which inquiry is used is a compile-time decision, and both are correct answers to a different
   * question.** `working_orders()` is the cheaper and more direct one — it lists exactly what is
   * resting — but it is capability-gated, because not every broker offers it. `order_history()` is
   * always available and answers a wider question at more cost.
   *
   * So this prefers working orders where they exist and falls back to history where they do not. A
   * broker offering neither cannot satisfy the gate, and that is a **compile error** rather than a
   * run-time refusal: a broker this loop cannot trade safely should fail to build, not fail at four
   * in the afternoon.
   */
  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    if constexpr (Caps::has_working_orders) {
      // **No order updates from this read, and the reason is a hazard rather than a preference.**
      // This runs inside `PollingLoop::gate_for`, which the loop calls *before* it applies the
      // startup hold, and it is handed a mutable runner -- so a `Runner::feed` here would dispatch
      // to a strategy that can still submit, against the picture the gate exists to withhold. The
      // listing is adopted as the baseline instead, which is also the honest reading of it: a
      // predecessor's resting order is initial state, not a change, and it is installed on
      // `Portfolio::working_` by the same call, which is where a strategy reads what it inherited.
      //
      // The cost is stated rather than hidden: an order present at startup that fills or is
      // cancelled before the first scheduled poll produces no update at all.
      return poll_working_orders(runner_instance, /*publish=*/false);
    } else {
      OrderHistoryQuery query;
      query.limit_ = history_limit_;
      auto result = session_->order_history(query);
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      last_error_.reset();
      // What this call establishes, with or without a journal, is the thing the gate actually needs:
      // that this process has looked at the broker's own record before sending.
      reconcile(runner_instance, result.value().orders_);
      return runner::PollOutcome::UPDATED;
    }
  }

  /**
   * Returns the broker-stated cost observed on the order snapshots polled so far.
   *
   * Absolute and monotone across every run made against this source; `Loop::run` differences it so
   * a report carries its own run's observations rather than the previous run's as well. Read
   * `runner::ChargedCosts` before believing it is a total -- it is a subtotal of unknown coverage,
   * because a working-order listing cannot show an order that filled between two polls.
   */
  const runner::ChargedCosts& charged_costs() const noexcept { return charged_; }

private:
  /**
   * Fetches bars for each instrument and feeds only those not seen before, in period-end order
   * across the universe; only each instrument's newest may send (older ones are fed as history, see
   * `Runner::feed_warmup`).
   */
  template <typename Runner>
  runner::PollOutcome poll_bars(Runner& runner_instance) {
    // **Every instrument is read before anything is fed**, and then the new bars of all of them are
    // fed in period-end order, ties in universe order -- the order a replay feeds them. Fed one
    // instrument at a time, a cold start handed a cross-sectional strategy all of one name's history
    // and then the next name's from its beginning, so the decision instant ran backwards between
    // names and the strategy's state diverged from a replay's. A query that fails feeds nothing this
    // poll; the next poll reads everything again.
    struct Collected {
      std::size_t instrument_{0};
      strategy::BarEvent event_;
      bool newest_{false};
    };
    std::vector<Collected> collected;
    for (std::size_t at = 0; at < universe_.size(); ++at) {
      const auto& instrument = universe_[at];
      BarQuery query;
      query.instrument_ = instrument;
      query.interval_ = interval_;
      query.count_ = count_;

      auto result = market_data_->bars(query);
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      const auto& series = result.value();
      if (series.bars_.empty()) {
        continue;
      }

      // Only bars newer than the last one fed, and in order. A poll returning the same closed bar
      // again is the normal case between closes, and feeding it twice would double every signal
      // derived from it.
      //
      // **And only bars whose period has ended.** Every shipped adapter's daily query runs up to
      // today, and the broker answers today's row while the session is still trading -- KIS
      // documents `mod_yn` for "the current business day" on `inquire-daily-itemchartprice`, and
      // Kiwoom's daily close field is `cur_prc`, the current price. Fed, that bar reached the
      // strategy with the price so far as its close, `last_bar_` moved past it, and the session's
      // real close was never fed. So a bar ending after the instant the loop reported is left for
      // a later poll, which reads it again. Its period end is `bar_period_end`'s, the instant the
      // runner would have stamped the decision with, so the decision is never dated ahead of the
      // clock either.
      //
      // The instant is the later of the loop's (`Runner::observed_at`) and when this machine
      // received the series (`BarSeries::received_at_`, which every shipped adapter stamps), so a
      // source driven by hand still has one, and one a finished `run` left behind cannot hold back a
      // bar received closed since; with neither, every bar is fed, as before. **Only where the
      // period end is known**: a close-stamped bar states it, and a day or a minute stamped at its
      // open ends one interval later. A week, a month or a year stamped at its open is fed as
      // before, because no adapter's convention for those stamps has been observed and a wrong
      // guess would hold every closed week back by a week.
      const auto instant = std::max(runner_instance.observed_at(), series.received_at_);
      const bool knowable = series.stamp_ == BarStamp::CLOSE ||
        series.interval_ == BarInterval::ONE_DAY || series.interval_ == BarInterval::ONE_MINUTE;
      const bool timed = knowable && instant != std::chrono::system_clock::time_point{};
      const auto& seen = last_bar_[instrument.symbol_];
      const auto first = collected.size();
      for (const auto& bar : series.bars_) {
        if (seen && bar.timestamp_ <= *seen) {
          continue;
        }
        if (timed && instant < strategy::bar_period_end(bar, series.interval_, series.stamp_)) {
          // Counted once per bar, not once per poll: a daily bar is met by every poll of its session.
          auto& held = held_bar_[instrument.symbol_];
          if (!held || !(*held == bar.timestamp_)) {
            held = bar.timestamp_;
            ++bars_not_yet_closed_;
          }
          break;
        }
        collected.push_back(Collected{
          at, strategy::make_bar_event(instrument, series.interval_, series.stamp_, bar), false});
      }
      // **Only the newest bar of an instrument's read may send.** The ones before it -- a cold
      // start's history, a backlog after an outage -- are history: the strategy is shown them, so its
      // state evolves as a replay's does, and what it decides on them is discarded
      // (`Runner::feed_warmup`). Otherwise a restart acts on every signal in its history at today's
      // price, which for an event-triggered strategy is a trade nobody decided today.
      if (collected.size() > first) {
        collected.back().newest_ = true;
      }
    }

    std::stable_sort(collected.begin(), collected.end(), [](const Collected& a, const Collected& b) {
      return a.event_.period_end_ < b.event_.period_end_;
    });
    // **And not at or before another instrument's history.** An instrument whose read ends early --
    // a name suspended today, or for days -- has a newest bar no later than the other names' older
    // ones; sent there, a cross-sectional strategy would act on a stale cross-section at today's
    // price. So a newest bar sends only when its period ended strictly after every history bar of the
    // read; one that did not is history too, and that instrument's decision waits for its next bar.
    // Strictly: a tie with a history bar is the same stale session whichever name the universe lists
    // first.
    std::optional<std::chrono::system_clock::time_point> latest_history;
    for (const auto& item : collected) {
      if (!item.newest_ && (!latest_history || *latest_history < item.event_.period_end_)) {
        latest_history = item.event_.period_end_;
      }
    }
    static_assert(
      session_source_detail::feeds_warmup<Runner>::value,
      "SessionSource feeds a read's older bars through Runner::feed_warmup; a runner without it "
      "would send decisions made on history"
    );
    auto outcome = runner::PollOutcome::UPDATED;
    if (collected.empty()) {
      outcome = runner::PollOutcome::UNCHANGED;
    }
    for (const auto& item : collected) {
      const auto& instrument = universe_[item.instrument_];
      if (!item.newest_ || (latest_history && !(*latest_history < item.event_.period_end_))) {
        runner_instance.feed_warmup(item.event_);
        ++bars_fed_as_warmup_;
        last_bar_[instrument.symbol_] = item.event_.bar_.timestamp_;
        continue;
      }
      runner_instance.context().set_last_price(instrument, item.event_.bar_.close_);
      runner_instance.feed(item.event_);
      last_bar_[instrument.symbol_] = item.event_.bar_.timestamp_;
    }
    last_error_.reset();
    return outcome;
  }

  /** Fetches a quote per instrument and feeds each one. */
  template <typename Runner>
  runner::PollOutcome poll_quotes(Runner& runner_instance) {
    auto outcome = runner::PollOutcome::UNCHANGED;
    for (const auto& instrument : universe_) {
      auto result = market_data_->quote(instrument);
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      strategy::QuoteEvent event;
      event.quote_ = result.value();
      runner_instance.feed(event);
      outcome = runner::PollOutcome::UPDATED;
    }
    last_error_.reset();
    return outcome;
  }

  /**
   * Refreshes positions, keeping whatever working orders the context already holds.
   *
   * The two inquiries are separate calls on their own cadences, so each must merge rather than
   * replace: rebuilding the portfolio from a holdings snapshot alone would silently drop every
   * working order, and reconciliation would then place orders it had already placed.
   */
  template <typename Runner>
  runner::PollOutcome poll_holdings(Runner& runner_instance) {
    auto result = session_->holdings();
    if (!result) {
      last_error_ = result.error();
      return poll_outcome_of(*last_error_);
    }
    last_error_.reset();

    auto portfolio = runner_instance.context().portfolio();
    portfolio.positions_ = result.value().positions_;
    // **The basis travels with the rows it describes, and both directions of leaving it behind are
    // defects.** Keeping the previous basis would let a snapshot that established nothing be measured
    // against an older read's declaration -- a third-party source's undeclared figure authorizing a
    // sell. Dropping it would leave the portfolio unestablished after a broker that *did* declare, so
    // every checked sell would refuse at both shipped adapters. It is assigned, not merged, for the
    // same reason `positions_` is: this is one read, and its figures mean what this read says.
    portfolio.sellable_basis_ = result.value().sellable_basis_;
    // When the snapshot arrived, which is the best the two brokers allow: neither timestamps a
    // holdings answer, so `received_at_` is this machine's clock at parse time. The loop's staleness
    // gate therefore measures time since arrival, and understates the age of the figures by the
    // request's own latency.
    portfolio.as_of_ = result.value().received_at_;
    runner_instance.context().set_portfolio(std::move(portfolio));
    return runner::PollOutcome::UPDATED;
  }

  /**
   * Refreshes what may be spent, per configured currency, keeping everything else.
   *
   * One request per currency, on the holdings cadence. A currency whose answer fails leaves the
   * previous reading in place and reports the failure, rather than dropping a figure the guard is
   * about to size against -- the loop's own staleness gate is what decides when an old reading
   * stops being good enough, and it can only do that if the reading is still there.
   */
  template <typename Runner>
  runner::PollOutcome poll_buying_power(Runner& runner_instance) {
    if constexpr (!Caps::has_buying_power) {
      static_cast<void>(runner_instance);
      return runner::PollOutcome::UNCHANGED;
    } else {
      if (currencies_.empty()) {
        return runner::PollOutcome::UNCHANGED;
      }
      auto portfolio = runner_instance.context().portfolio();
      bool updated = false;
      // Kept apart so the winning outcome and the reason reported for it stay the same currency's.
      std::optional<BrokerError> outright;
      std::optional<BrokerError> throttled;
      for (const auto& currency : currencies_) {
        auto result = session_->buying_power(currency);
        if (!result) {
          // **A real failure outranks a rate-limit refusal**, wherever in the sweep each landed.
          // They are not the same news: the limiter refusing is the limiter working, and the loop
          // deliberately does not count it against a poll kind, while an outage must count. Taking
          // whichever came last would let a currency that fails every sweep be reported as merely
          // throttled the moment another currency was throttled behind it, and the failing budget
          // would age on unnoticed.
          //
          // The reason is kept beside its outcome rather than overwritten, because `last_error()`
          // promises why the last failing poll failed: reporting `FAILED` next to a rate-limit
          // reason would send a reader after the wrong currency.
          if (poll_outcome_of(result.error()) == runner::PollOutcome::FAILED) {
            if (!outright) {
              outright = result.error();
            }
          } else if (!throttled) {
            throttled = result.error();
          }
          continue;
        }
        bool replaced = false;
        for (auto& reading : portfolio.buying_power_) {
          if (reading.cash_.currency_ == currency) {
            reading = result.value();
            replaced = true;
            break;
          }
        }
        if (!replaced) {
          portfolio.buying_power_.push_back(result.value());
        }
        updated = true;
      }
      if (updated) {
        runner_instance.context().set_portfolio(std::move(portfolio));
      }
      const auto& reason = outright ? outright : throttled;
      if (reason) {
        last_error_ = *reason;
      } else {
        last_error_.reset();
      }
      // **A partial answer is a failed poll**, even though the currencies that answered were still
      // applied. Reporting `UPDATED` because *something* arrived would reset the loop's consecutive
      // failure count, and a currency failing every time would then never abandon the run: the loop
      // would report healthy polls indefinitely while orders in that currency were sized against a
      // figure from hours ago. The readings are kept because the alternative -- dropping a budget
      // the guard is about to size against -- is worse, and the staleness gate is what decides when
      // an old reading stops being good enough. `last_error_` survives for the same reason.
      if (outright) {
        return runner::PollOutcome::FAILED;
      }
      if (throttled) {
        return poll_outcome_of(*throttled);
      }
      return updated ? runner::PollOutcome::UPDATED : runner::PollOutcome::UNCHANGED;
    }
  }

  /**
   * Refreshes today's permitted price band for each instrument, and records it on the runner.
   *
   * The second poll whose result is not an event, and the second that **installs and feeds nothing**
   * -- `runner::PollKind::PRICE_LIMITS` states that obligation and why it is stated for a poll that
   * cannot break it. The band lands in `Runner::set_price_limits`, which dates it, and
   * reconciliation reads it when a plan is built.
   *
   * Swept per instrument, with the partial-failure rules `poll_buying_power` argues in full and for
   * the same reasons: **a real failure outranks a rate-limit refusal** wherever in the sweep each
   * landed, the reason is kept beside its own outcome so `last_error()` describes the failure it
   * reports, and **a partial answer is a failed poll** even though the instruments that answered are
   * still installed.
   *
   * **A failure leaves the previous band in place**, as the calendar and the budget both do. Erasing
   * it would turn a timed-out request into an unclamped order, which is the opposite of the
   * direction a failure should push; the runner's age policy is what decides when the band that is
   * still there has stopped being usable, and it can only decide that if it is still there.
   */
  template <typename Runner>
  runner::PollOutcome poll_price_limits(Runner& runner_instance) {
    if constexpr (!MarketData::Caps::has_price_limits) {
      // A broker that publishes no band compiles no request, and a loop with this cadence set
      // against one simply never reads a band -- Decision 2b, the capability is absent and absent is
      // not faked.
      static_cast<void>(runner_instance);
      return runner::PollOutcome::UNCHANGED;
    } else {
      if (universe_.empty()) {
        return runner::PollOutcome::UNCHANGED;
      }
      bool updated = false;
      std::optional<BrokerError> outright;
      std::optional<BrokerError> throttled;
      for (const auto& instrument : universe_) {
        auto result = market_data_->price_limits(instrument);
        if (!result) {
          if (poll_outcome_of(result.error()) == runner::PollOutcome::FAILED) {
            if (!outright) {
              outright = result.error();
            }
          } else if (!throttled) {
            throttled = result.error();
          }
          continue;
        }
        runner_instance.set_price_limits(instrument, std::move(result.value()));
        updated = true;
      }
      const auto& reason = outright ? outright : throttled;
      if (reason) {
        last_error_ = *reason;
      } else {
        last_error_.reset();
      }
      if (outright) {
        return runner::PollOutcome::FAILED;
      }
      if (throttled) {
        return poll_outcome_of(*throttled);
      }
      return updated ? runner::PollOutcome::UPDATED : runner::PollOutcome::UNCHANGED;
    }
  }

  /**
   * Refreshes which dates the market trades, and puts the answer where the loop and a strategy read it.
   *
   * The only poll whose result is not an event. It lands in `Context::set_calendar`, from which
   * `runner::PollingLoop` derives the market phase and a strategy answers "does tomorrow trade" --
   * one copy, one owner, and no way for the loop and the strategy to be looking at different
   * calendars.
   *
   * `from` is left absent so each broker supplies its own "today", which is today *there*: KIS dates
   * every request in Korean local time and its traits layer fills that in. Asking with a date derived
   * from this process's clock would consult the wrong day for the hours either side of local midnight.
   *
   * **A failure leaves the previous calendar in place**, exactly as `poll_buying_power` leaves the
   * previous budget. Erasing it would take a run that knew today was a trading day and make it
   * `UNKNOWN` because a reference-data request timed out, which refuses every order; the loop's own
   * reasoning about an uncovered date is what decides when an old answer has stopped being usable,
   * and it can only do that if the answer is still there.
   */
  template <typename Runner>
  runner::PollOutcome poll_calendar(Runner& runner_instance) {
    if constexpr (!MarketData::Caps::has_calendar) {
      // A broker that publishes no calendar compiles no request. The loop then never establishes a
      // phase, which it reports rather than guesses at -- Decision 2b: the capability is absent, and
      // absent is not faked.
      static_cast<void>(runner_instance);
      return runner::PollOutcome::UNCHANGED;
    } else {
      auto result = market_data_->calendar(calendar_country_);
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      last_error_.reset();
      runner_instance.context().set_calendar(std::move(result.value()));
      return runner::PollOutcome::UPDATED;
    }
  }

  /**
   * Accumulates what the broker stated it charged, from one working-order listing.
   *
   * **One invariant holds the arithmetic together: what `charged_seen_` holds for a figure is
   * exactly what has been contributed to the totals for it, and at which filled quantity.** Every
   * rule below exists to keep that true, and each was a place review found it broken.
   *
   * - **A figure is cumulative per order**, so a total moves by the *delta* against what is held
   *   rather than by the whole figure again.
   * - **A reading that states nothing withdraws nothing**, and does not move the quantity the held
   *   figure belongs to. Silence that advanced the quantity made the next real fee look like a
   *   disagreement about the previous one, and it was dropped.
   * - **A figure for a quantity this figure has not been accepted at is new information**, whether
   *   the order was silent in between or this is its first figure at all.
   * - **Two present figures at one quantity are a conflict**, and the held one stands: nothing in a
   *   snapshot says which the broker sent later.
   * - **An order does not change the currency of its own charge**, so a reading that does is a
   *   conflict rather than a replacement that would strand what was accumulated in the first.
   * - **Absence is not zero.** An order stating nothing contributes to no currency's total.
   */
  void observe_charged(const std::vector<OrderSnapshot>& rows) {
    std::vector<std::string> present;
    present.reserve(rows.size());
    for (const auto& row : rows) {
      present.push_back(row.order_id_);
      const auto& execution = row.execution_;
      const auto states_cost = execution.commission_ || execution.tax_;

      auto held = charged_seen_.find(row.order_id_);
      if (held == charged_seen_.end()) {
        ObservedCharge fresh;
        fresh.counted_cost_ = states_cost;
        charged_seen_.emplace(row.order_id_, std::move(fresh));
        held = charged_seen_.find(row.order_id_);
        if (states_cost) {
          ++charged_.orders_stating_cost_;
        } else {
          // Counted once, at first sight, and never taken back when a figure arrives later: both
          // counters are monotone so that differencing them across runs cannot underflow, and an
          // order that began without a figure is in both counts by design.
          ++charged_.orders_stating_none_;
        }
      } else if (states_cost && !held->second.counted_cost_) {
        held->second.counted_cost_ = true;
        ++charged_.orders_stating_cost_;
      }

      take_figure(
        charged_.commission_, held->second.commission_,
        execution.commission_, execution.filled_quantity_
      );
      take_figure(
        charged_.tax_, held->second.tax_, execution.tax_, execution.filled_quantity_
      );
    }

    for (const auto& previous : charged_listing_) {
      bool still_there = false;
      for (const auto& id : present) {
        if (id == previous) {
          still_there = true;
          break;
        }
      }
      if (!still_there) {
        // A row that was there and is not. It says nothing about *why*, and nothing about whether a
        // charge was missed -- see the field's own documentation.
        ++charged_.working_list_disappearances_;
      }
    }
    charged_listing_ = std::move(present);
  }

  /**
   * Moves one running total by a new reading, keeping `held` in step with what the total was given.
   *
   * `quantity` is how much of the order had filled when the broker stated `now`. A figure is taken
   * when that exceeds the quantity this figure was last accepted at; at or below it, two present
   * amounts that differ are a conflict and the held one stands.
   */
  void take_figure(
    std::vector<runner::ChargedCosts::StatedTotal>& totals,
    ObservedFigure& held,
    const std::optional<Money>& now,
    const Decimal64& quantity
  ) {
    if (!now) {
      // Silence withdraws nothing, and leaves this figure where it was -- including the quantity it
      // belongs to, so a fee arriving later for the filled quantity is still new information.
      return;
    }
    if (!held.amount_) {
      if (charged_.add(totals, *now)) {
        held.amount_ = now;
        held.at_ = quantity;
      }
      return;
    }
    if (!(held.amount_->currency_ == now->currency_)) {
      // An order does not change the currency of its own charge. Replacing it would strand what was
      // accumulated in the first currency and base every later delta on the second.
      ++charged_.conflicting_restatements_;
      return;
    }
    if (!decimal_less(held.at_, quantity)) {
      if (!(held.amount_->amount_ == now->amount_)) {
        // Two present figures for one filled quantity, and nothing says which is later.
        ++charged_.conflicting_restatements_;
      }
      return;
    }
    const auto moved = decimal_subtract(now->amount_, held.amount_->amount_);
    if (!moved) {
      // The delta between two figures the broker stated does not fit -- a held five quintillion
      // restated at a tenth of a won reaches it. Counted for the same reason a failed sum is: the
      // figure is not contributed, and a reader has to be able to see that rather than read the
      // total as complete.
      ++charged_.unrepresentable_sums_;
      return;
    }
    if (charged_.add(totals, Money{*moved, now->currency_})) {
      held.amount_ = now;
      held.at_ = quantity;
    }
  }

  /** Refreshes working orders, keeping positions. Compiles to nothing without the capability. */
  template <typename Runner>
  runner::PollOutcome poll_working_orders(Runner& runner_instance, bool publish = true) {
    if constexpr (Caps::has_working_orders) {
      auto result = session_->working_orders();
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      last_error_.reset();

      auto portfolio = runner_instance.context().portfolio();
      portfolio.working_ = result.value().orders_;
      // Stamped with the listing, so a plan sized before the next holdings read can tell the account
      // is older than the orders it is reconciled with; see `Portfolio::listing_newer_than_holdings`.
      portfolio.working_as_of_ = result.value().received_at_;
      runner_instance.context().set_portfolio(std::move(portfolio));

      // Swept from the rows just fetched, so reconciliation costs no request of its own. A
      // `WorkingOrder` carries the normalized snapshot the matcher reads; nothing broker-specific
      // reaches it.
      std::vector<OrderSnapshot> rows;
      rows.reserve(result.value().orders_.size());
      for (const auto& working : result.value().orders_) {
        rows.push_back(working.order_);
      }
      // **Observed here because this is where the rows exist.** `Source::poll` hands the loop a
      // `PollOutcome` and nothing else, and these rows are swept into reconciliation and dropped --
      // so a cost accumulated in the loop could only ever be accumulated from nothing.
      observe_charged(rows);
      reconcile(runner_instance, rows);

      // **Last, and after reconciliation, deliberately.** `Runner::feed` settles, so an order can
      // leave the process from inside this call -- and a strategy answering an update must do so
      // against the portfolio this poll installed, with reconciliation already done and any local
      // failure it raised already standing as a hold. Feeding earlier would let a strategy act on a
      // poll whose own conclusions were still being drawn.
      //
      // `publish` is false only for the startup read; see `read_order_list` for why that is a
      // hazard rather than a preference.
      //
      // **No halt check between updates, which the replay does have, and the asymmetry is the
      // point.** A hold here refuses what a strategy *sends* and does not make what the broker
      // *said* untrue -- a runner that has stopped trading still wants its handler told that a
      // resting order filled. A halted replay is the opposite case: the engine can no longer model
      // what it is being asked to, so no callback may run at all.
      if (publish) {
        for (const auto& update : updates_.changed(result.value().orders_)) {
          runner_instance.feed(update);
        }
      } else {
        updates_.adopt(result.value().orders_);
      }
      // **After** the updates, so a strategy's own `on_order_update` has spoken first. This listing is
      // the only thing that sees a stop leave, and a disappearance feeds no callback -- so where a
      // deferred target's stops are all gone, the broker's own record of them is read here, on demand,
      // and decides it. A failed read leaves the target waiting and reports like any poll failure. See
      // `Runner::deferred_targets`.
      //
      // The same read serves an order this runner sent that only the history can settle now (one a
      // listing carried and no longer does, one it withdrew, one past the confirmation timeout; see
      // `Runner::unconfirmed_awaiting_evidence`). Settled first, so a released target is planned against
      // what the history said became of it.
      const auto deferred_since = runner_instance.deferred_awaiting_evidence();
      const auto unconfirmed_since = runner_instance.unconfirmed_awaiting_evidence();
      if (deferred_since || unconfirmed_since) {
        const auto since = !deferred_since ? *unconfirmed_since :
          !unconfirmed_since               ? *deferred_since :
                                             std::min(*deferred_since, *unconfirmed_since);
        std::vector<OrderSnapshot> evidence;
        // Read through the later of the decision instant and the loop's clock, and never before
        // `since`: a sent order's `since` is the loop's clock, and on daily bars the decision instant
        // is the last session's end -- after a weekend that ended the range before the day the order
        // was sent, and the order could never be found.
        const auto until = std::max({runner_instance.context().now(), runner_instance.observed_at(), since});
        if (!read_evidence(since, until, evidence)) {
          return poll_outcome_of(*last_error_);
        }
        if (unconfirmed_since) {
          runner_instance.confirm_from_history(evidence);
        }
        if (deferred_since) {
          runner_instance.retry_deferred(evidence);
        }
      }
      return runner::PollOutcome::UPDATED;
    } else {
      (void)publish;
      // Unreachable through the loop, which never schedules a kind the caller left unset, and a
      // caller who sets a cadence for an inquiry this broker lacks gets a poll that does nothing
      // rather than a compile error -- the cadence is data, not a capability claim.
      (void)runner_instance;
      return runner::PollOutcome::UNCHANGED;
    }
  }

  /**
   * Resolves what `rows` settle, leaving withheld whatever they do not.
   *
   * Does nothing at all without a journal, which is what keeps the pre-existing behaviour exactly
   * as it was: a source with no journal reads the list, lifts the gate, and matches nothing.
   *
   * `now` comes from the runner's own context rather than from the page's `received_at_`. The two
   * are different clocks -- the context's is local, like the journal's stamps, and the page's is the
   * broker's -- and the matcher already widens for that difference in one place. Measuring the
   * window against the broker's clock as well would apply the allowance twice.
   */
  template <typename Runner>
  void reconcile(Runner& runner_instance, const std::vector<OrderSnapshot>& rows) {
    if (journal_ == nullptr) {
      return;
    }
    const auto open = runner::unresolved_instructions(journal_->replay());
    if (open.empty()) {
      reconcile_ = runner::ReconcileReport{};
      return;
    }

    auto candidates = rows;
    if (runner::any_placement(open)) {
      // **An open-orders listing cannot answer the placement question.** Both brokers' working-order
      // inquiry returns only what is still resting -- Toss maps it to its `OPEN` scope, KIS to the
      // modifiable set -- and a market or aggressive limit order can fill and leave that listing
      // before the first sweep. Matching against it alone would then find nothing, and after the
      // window closed would report `NOT_PLACED` and clear the ledger for an order that existed and
      // executed. So a sweep with any placement owed reads the history as well.
      //
      // Paid for only when something is actually owed, which is rare: the normal sweep still costs
      // no request of its own.
      std::vector<OrderSnapshot> history;
      if (!read_history(open, history)) {
        // Seeing part of the picture is worse than seeing none. `NOT_PLACED` is a conclusion drawn
        // from absence, and absence from a listing that failed to arrive is not evidence.
        reconcile_ = runner::decide_nothing(
          open, "the order history could not be read, so nothing could be concluded from absence"
        );
        return;
      }
      if constexpr (Caps::history_requires_range) {
        runner::merge_rows(candidates, history);
      } else {
        // Read one date at a time; see `runner::merge_dated_history`.
        runner::merge_dated_history(candidates, history);
      }
    }

    // The decision instant, as always -- except before anything was fed, as at the startup read after
    // a restart, when it is still the epoch. A resolution stamped there reads back as damage and halts
    // the next start (lab F29), so the loop's last reading stands in, or the wall clock without a loop.
    auto now = runner_instance.context().now();
    if (now == std::chrono::system_clock::time_point{}) {
      now = runner_instance.observed_at() != std::chrono::system_clock::time_point{} ?
        runner_instance.observed_at() :
        std::chrono::system_clock::now();
    }
    reconcile_ = runner::reconcile_against(
      open,
      candidates,
      now,
      runner_instance.ledger(),
      *journal_,
      window_
    );
    if (reconcile_.unrecorded_) {
      // The sweep decided, and the decision could not be written down. Trading on a resolution that
      // exists only in memory is how a restart re-opens a question that was already answered -- and
      // a journal that cannot be written will not manage the next order either.
      runner_instance.halt_locally(
        "the order journal could not record a reconciliation, so the hold stands"
      );
      return;
    }
    // **A placement whose reply was lost, now answered** (lab F35): the runner has counted it since the
    // send, and keeps counting a matched one until holdings show it -- the ledger, just cleared, no
    // longer withholds the instrument, and a filled market order is in no listing.
    for (const auto& match : reconcile_.matches_) {
      if (match.verdict_ != runner::MatchVerdict::MATCHED && match.verdict_ != runner::MatchVerdict::NOT_PLACED) {
        continue;
      }
      for (const auto& instruction : open) {
        if (instruction.entry_.id_ != match.entry_id_ || instruction.entry_.action_ != runner::JournalAction::PLACE) {
          continue;
        }
        runner_instance.resolve_indeterminate_placement(
          instruction.entry_.instrument_, instruction.entry_.side_,
          match.verdict_ == runner::MatchVerdict::MATCHED ? std::optional<std::string>(match.order_id_) :
                                                            std::nullopt
        );
        break;
      }
    }
  }

  /**
   * Fills `into` with the order history covering everything `open` could have reached the broker in.
   *
   * Returns whether the whole range was read. A partial read is a failure, not a smaller success,
   * because every row this misses is one the matcher will count as absent.
   *
   * Both broker differences are handled here rather than pushed onto the caller: a calendar range
   * where the broker requires one, and cursor paging where the broker offers one. The page cap is a
   * guard against a cursor that never terminates, and exhausting it is reported as a failure for the
   * same reason -- a truncated listing is a listing with holes in it.
   */
  bool read_history(const std::vector<runner::OpenInstruction>& open,
    std::vector<OrderSnapshot>& into) {
    if constexpr (Caps::history_requires_range) {
      return read_history_in(runner::history_range_for(open, window_), into);
    } else {
      // **One stated date at a time** for a broker whose history takes a single date -- Kiwoom's
      // `kt00007` requires it and refuses a range, so an undated sweep failed every time and a journalled
      // Kiwoom runner could resolve nothing. The days are the ones `history_range_for` spans: a day
      // either side of the instructions' own window, in UTC, which contains every market-local day.
      auto earliest = open.front().opened_at_;
      auto latest = runner::instruction_sent_by(open.front(), window_);
      for (const auto& instruction : open) {
        if (instruction.opened_at_ < earliest) {
          earliest = instruction.opened_at_;
        }
        const auto sent_by = runner::instruction_sent_by(instruction, window_);
        if (sent_by > latest) {
          latest = sent_by;
        }
      }
      return read_history_days(earliest, latest, into);
    }
  }

  /**
   * Reads the history one stated date at a time, for every UTC day from the day before `from` to the
   * day after `to`: a broker's order date is its own market's, and that span contains it.
   */
  bool read_history_days(
    std::chrono::system_clock::time_point from,
    std::chrono::system_clock::time_point to,
    std::vector<OrderSnapshot>& into
  ) {
    std::vector<Date> days;
    const auto add = [&days](const Date& day) {
      for (const auto& seen : days) {
        if (seen.year_ == day.year_ && seen.month_ == day.month_ && seen.day_ == day.day_) {
          return;
        }
      }
      days.push_back(day);
    };
    const auto until = to + std::chrono::hours(24);
    for (auto at = from - std::chrono::hours(24); at < until; at += std::chrono::hours(24)) {
      add(runner::date_of(at));
    }
    add(runner::date_of(until));
    // Pages are concatenated, dropping only an **exact** copy -- the same id and instrument with the
    // same order time stated on both. Two days' orders can share a number, so the id alone is not
    // identity, and a row with no time is never assumed to be another's copy. See
    // `runner::merge_dated_history`.
    for (const auto& day : days) {
      std::vector<OrderSnapshot> page;
      if (!read_history_in({day, day}, page, true)) {
        return false;
      }
      for (const auto& row : page) {
        bool copy = false;
        for (const auto& held : into) {
          if (runner::exact_copy(held, row)) {
            copy = true;
            break;
          }
        }
        if (!copy) {
          into.push_back(row);
        }
      }
    }
    return true;
  }

  /**
   * Fills `into` with the history a deferred target's stops can appear in: from the day before it was
   * deferred to today.
   *
   * **Two query shapes, because the brokers that hold stops disagree.** KIS takes a range
   * (`history_requires_range`), so one query covers it. Kiwoom's `kt00007` takes **one** date, which it
   * requires, and refuses a range -- so each day is asked for on its own, from the day before the
   * deferral (or the send) to the day after now, both in UTC so the market-local day is always inside:
   * three or four requests within a week, six across a weekend on daily bars, repeated on each listing
   * poll while an entry waits. A broker taking neither shape is asked each day as well, which states a
   * date it may ignore rather than omitting one it may need.
   */
  bool read_evidence(
    std::chrono::system_clock::time_point since,
    std::chrono::system_clock::time_point now,
    std::vector<OrderSnapshot>& into
  ) {
    if constexpr (Caps::history_requires_range) {
      return read_history_in(
        {runner::date_of(since - std::chrono::hours(24)), runner::date_of(now + std::chrono::hours(24))},
        into
      );
    } else {
      // **Through the day after `now` in UTC**, the same upper bound as the ranged query. `date_of` is a
      // UTC date and a broker's order date is its own market's: at 08:45 KST it is still the previous
      // day in UTC, and stopping at `date_of(now)` never asked for the Korean day the stop was on.
      return read_history_days(since, now, into);
    }
  }

  /**
   * Fills `into` with the order history over `range` where the broker takes one, or wherever `dated`
   * asks for the dates to be stated regardless; see `read_history`.
   */
  bool read_history_in(
    const std::pair<Date, Date>& range,
    std::vector<OrderSnapshot>& into,
    bool dated = Caps::history_requires_range
  ) {
    OrderHistoryQuery query;
    if constexpr (Caps::has_history_limit) {
      // Gated, because KIS does not ignore a limit it cannot honour -- it refuses the whole inquiry
      // with `limit_unsupported`. An ungated assignment here made every KIS placement sweep take the
      // history-failure branch, so a KIS runner recovered nothing and withheld forever.
      query.limit_ = history_limit_;
    }
    if (dated) {
      query.from_ = range.first;
      query.to_ = range.second;
    }

    for (int page = 0; page < maximum_history_pages_; ++page) {
      auto result = session_->order_history(query);
      if (!result) {
        last_error_ = result.error();
        return false;
      }
      runner::merge_rows(into, result.value().orders_);
      if constexpr (Caps::has_history_cursor) {
        if (!result.value().next_cursor_ || result.value().next_cursor_->empty()) {
          return true;
        }
        query.cursor_ = result.value().next_cursor_;
      } else {
        return true;
      }
    }
    return false;
  }

  Session* session_;
  MarketData* market_data_;
  std::vector<InstrumentId> universe_;
  std::vector<Currency> currencies_;
  /** What the broker has stated it charged, across every run made against this source. */
  runner::ChargedCosts charged_;
  /** The last accepted reading per order id, so a cumulative figure is not added twice. */
  std::map<std::string, ObservedCharge> charged_seen_;
  /** The order ids in the previous working-order listing, for the disappearance diagnostic. */
  std::vector<std::string> charged_listing_;
  Country calendar_country_{Country::KR};
  Journal* journal_{nullptr};
  int maximum_history_pages_{20};
  runner::MatchWindow window_;
  runner::ReconcileReport reconcile_;
  /** Turns successive working-order listings into `on_order_update` events. */
  runner::OrderUpdateWatch<BrokerTag> updates_;
  BarInterval interval_{BarInterval::ONE_DAY};
  int count_{100};
  std::size_t bars_not_yet_closed_{0};
  std::size_t bars_fed_as_warmup_{0};
  std::map<std::string, std::optional<std::chrono::system_clock::time_point>> held_bar_;
  int history_limit_{100};
  std::map<std::string, std::optional<std::chrono::system_clock::time_point>> last_bar_;
  std::optional<BrokerError> last_error_;
};

} // namespace live
} // namespace algotradelib
