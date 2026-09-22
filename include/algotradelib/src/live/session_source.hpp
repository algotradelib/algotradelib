#pragma once

#include "../runner/loop.hpp"

#include <algotradelib/broker>

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
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
 */
template <typename Session>
class SessionSource {
public:
  using Caps = typename Session::Caps;
  using BrokerTag = typename Session::BrokerTag;

  /**
   * Binds a session and the universe to poll.
   *
   * The session is borrowed, matching `SessionExecutor`: it owns a client with executor affinity, and
   * neither a runner nor a source owns the connection it uses.
   */
  SessionSource(Session& session, std::vector<InstrumentId> universe)
  : session_(&session), universe_(std::move(universe)) {}

  /** Sets the bar interval and depth each poll requests. */
  void set_bar_query(BarInterval interval, int count) {
    interval_ = interval;
    count_ = count;
  }

  /** Returns why the last failing poll failed, for a caller to log. */
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

  /** Polls `kind` and feeds whatever arrived to `runner`. */
  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    switch (kind) {
      case runner::PollKind::BARS:
        return poll_bars(runner_instance);
      case runner::PollKind::QUOTES:
        return poll_quotes(runner_instance);
      case runner::PollKind::HOLDINGS:
        return poll_holdings(runner_instance);
      case runner::PollKind::WORKING_ORDERS:
        return poll_working_orders(runner_instance);
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
      return poll_working_orders(runner_instance);
    } else {
      OrderHistoryQuery query;
      query.limit_ = history_limit_;
      auto result = session_->order_history(query);
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      last_error_.reset();
      // The rows are not matched against ledger entries here. Attribute matching is a heuristic
      // rather than an identity -- two identical orders seconds apart are indistinguishable in a
      // listing -- so automating it is deliberately left to
      // `plan/todo/order-journal-reconciliation.md`. What this call establishes is the thing the gate
      // actually needs: that this process has looked at the broker's own record before sending.
      return runner::PollOutcome::UPDATED;
    }
  }

private:
  /** Fetches bars for each instrument and feeds only those not seen before. */
  template <typename Runner>
  runner::PollOutcome poll_bars(Runner& runner_instance) {
    auto outcome = runner::PollOutcome::UNCHANGED;
    for (const auto& instrument : universe_) {
      BarQuery query;
      query.instrument_ = instrument;
      query.interval_ = interval_;
      query.count_ = count_;

      auto result = session_->bars(query);
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
      auto& seen = last_bar_[instrument.symbol_];
      for (const auto& bar : series.bars_) {
        if (seen && bar.timestamp_ <= *seen) {
          continue;
        }
        runner_instance.context().set_last_price(instrument, bar.close_);
        runner_instance.feed(
          strategy::make_bar_event(instrument, series.interval_, series.stamp_, bar)
        );
        seen = bar.timestamp_;
        outcome = runner::PollOutcome::UPDATED;
      }
    }
    last_error_.reset();
    return outcome;
  }

  /** Fetches a quote per instrument and feeds each one. */
  template <typename Runner>
  runner::PollOutcome poll_quotes(Runner& runner_instance) {
    auto outcome = runner::PollOutcome::UNCHANGED;
    for (const auto& instrument : universe_) {
      auto result = session_->quote(instrument);
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
    // The broker's own timestamp, not the local clock: it is when the positions were true, which is
    // what the loop's staleness gate has to measure against.
    portfolio.as_of_ = result.value().received_at_;
    runner_instance.context().set_portfolio(std::move(portfolio));
    return runner::PollOutcome::UPDATED;
  }

  /** Refreshes working orders, keeping positions. Compiles to nothing without the capability. */
  template <typename Runner>
  runner::PollOutcome poll_working_orders(Runner& runner_instance) {
    if constexpr (Caps::has_working_orders) {
      auto result = session_->working_orders();
      if (!result) {
        last_error_ = result.error();
        return poll_outcome_of(*last_error_);
      }
      last_error_.reset();

      auto portfolio = runner_instance.context().portfolio();
      portfolio.working_ = result.value().orders_;
      runner_instance.context().set_portfolio(std::move(portfolio));
      return runner::PollOutcome::UPDATED;
    } else {
      // Unreachable through the loop, which never schedules a kind the caller left unset, and a
      // caller who sets a cadence for an inquiry this broker lacks gets a poll that does nothing
      // rather than a compile error -- the cadence is data, not a capability claim.
      (void)runner_instance;
      return runner::PollOutcome::UNCHANGED;
    }
  }

  Session* session_;
  std::vector<InstrumentId> universe_;
  BarInterval interval_{BarInterval::ONE_DAY};
  int count_{100};
  int history_limit_{100};
  std::map<std::string, std::optional<std::chrono::system_clock::time_point>> last_bar_;
  std::optional<BrokerError> last_error_;
};

} // namespace live
} // namespace algotradelib
