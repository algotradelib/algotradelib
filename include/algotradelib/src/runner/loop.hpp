#pragma once

#include "./clock.hpp"
#include "./stop.hpp"

#include <algotradelib/core>
#include <algotradelib/strategy>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/** What a loop polls for, and the key its cadence is configured under. */
enum class PollKind {
  /** Completed bars, which is what most strategies trade on. */
  BARS,
  /** Current prices, for strategies that act between bars. */
  QUOTES,
  /** Positions and cash, which sizing is reconciled against. */
  HOLDINGS,
  /** Resting orders, which reconciliation and withdrawal both need. */
  WORKING_ORDERS,
};

/** Returns a stable lowercase name for `kind`. */
inline constexpr std::string_view poll_kind_name(PollKind kind) noexcept {
  switch (kind) {
    case PollKind::BARS:
      return "bars";
    case PollKind::QUOTES:
      return "quotes";
    case PollKind::HOLDINGS:
      return "holdings";
    case PollKind::WORKING_ORDERS:
      return "working_orders";
  }
  return "unknown";
}

/** Every kind, for iterating without repeating the list. */
inline constexpr PollKind all_poll_kinds[] = {
  PollKind::BARS,
  PollKind::QUOTES,
  PollKind::HOLDINGS,
  PollKind::WORKING_ORDERS,
};

/**
 * What became of one poll.
 *
 * Four values rather than a bool, because the loop owes a different response to each and two of
 * them are easy to conflate:
 *
 * - **`UNCHANGED` is not a failure.** Nothing new since the last poll is the normal case between
 *   bars, and counting it as a failure would escalate a quiet market into a shutdown.
 * - **`RATE_LIMITED` is not a failure either.** It is the limiter working as designed. The
 *   controller already handles the wait, and counting a refusal as an outage would make a loop
 *   polling slightly too fast quiesce itself.
 */
enum class PollOutcome {
  /** New data arrived and was fed to the runner. */
  UPDATED,
  /** The poll succeeded and there was nothing new. */
  UNCHANGED,
  /** The poll failed. Counted toward the failure budget. */
  FAILED,
  /** The poll was refused by the rate limiter. Not counted as a failure. */
  RATE_LIMITED,
};

/** Returns a stable lowercase name for `outcome`. */
inline constexpr std::string_view poll_outcome_name(PollOutcome outcome) noexcept {
  switch (outcome) {
    case PollOutcome::UPDATED:
      return "updated";
    case PollOutcome::UNCHANGED:
      return "unchanged";
    case PollOutcome::FAILED:
      return "failed";
    case PollOutcome::RATE_LIMITED:
      return "rate_limited";
  }
  return "unknown";
}

/**
 * How often each kind is polled.
 *
 * An unset interval disables that poll entirely, which is the honest default: a loop should not
 * invent a cadence for data the caller never said they wanted, and a broker without working-order
 * inquiry has no cadence to set.
 *
 * **`tick_` is the loop's own granularity**, and the floor on how quickly a stop is noticed through
 * the control file. It is not a poll: an iteration with nothing due still checks for a stop, which is
 * why a loop polling bars once a day still responds to an operator within a second.
 */
struct PollSchedule {
  std::optional<std::chrono::nanoseconds> bars_;
  std::optional<std::chrono::nanoseconds> quotes_;
  std::optional<std::chrono::nanoseconds> holdings_;
  std::optional<std::chrono::nanoseconds> working_orders_;
  std::chrono::nanoseconds tick_{std::chrono::seconds(1)};

  /** Returns the interval configured for `kind`, or nothing when it is disabled. */
  std::optional<std::chrono::nanoseconds> interval_for(PollKind kind) const noexcept {
    switch (kind) {
      case PollKind::BARS:
        return bars_;
      case PollKind::QUOTES:
        return quotes_;
      case PollKind::HOLDINGS:
        return holdings_;
      case PollKind::WORKING_ORDERS:
        return working_orders_;
    }
    return std::nullopt;
  }
};

/** How the loop behaves when polls keep failing. */
struct FailurePolicy {
  /**
   * Consecutive failures of one kind before the loop quiesces.
   *
   * Bounded rather than infinite because a loop spinning against a broker that is refusing it is
   * both useless and rude. Counted per kind, so a broken working-order inquiry does not mask a
   * healthy bar feed.
   */
  std::size_t consecutive_limit_{5};
};

/** What one iteration did. */
struct IterationReport {
  /** When the iteration ran. */
  std::chrono::system_clock::time_point at_{};
  /** What each poll returned, in the order they were attempted. */
  std::vector<std::pair<PollKind, PollOutcome>> polls_;
  /** Which hold blocked sending, or nothing when nothing did. */
  std::optional<strategy::HaltReason> gate_;
  /** Whether anything was fed to the runner. */
  bool fed_{false};

  /** Returns whether any poll brought new data. */
  bool updated() const noexcept {
    for (const auto& entry : polls_) {
      if (entry.second == PollOutcome::UPDATED) {
        return true;
      }
    }
    return false;
  }
};

/** What a whole run did, and why it ended. */
struct LoopReport {
  /** How many iterations ran. */
  std::size_t iterations_{0};
  /** How many polls of each kind were attempted, indexed by `PollKind`. */
  std::size_t attempted_[4] = {0, 0, 0, 0};
  /** How many polls of each kind failed. */
  std::size_t failed_[4] = {0, 0, 0, 0};
  /** How many iterations were blocked from sending, by reason. */
  std::size_t blocked_{0};
  /** The intent that ended the run, or nothing when it ended for another reason. */
  std::optional<StopIntent> stopped_by_;
  /** Which hold stopped the loop sending when it did, for an operator reading the tail of a log. */
  std::optional<strategy::HaltReason> final_gate_;
  /** Whether the loop gave up because a poll kept failing. */
  bool abandoned_{false};
  /** Which kind exhausted its failure budget, when one did. */
  std::optional<PollKind> abandoned_kind_;
  /** Whether the order list was successfully read at least once. */
  bool order_list_read_{false};
  /** Cancellations queued while honouring a `WITHDRAW` or `FLATTEN`. */
  std::size_t withdrawn_{0};
  /** Positions closed while honouring a `FLATTEN`. */
  std::size_t flattened_{0};
  /**
   * What the closing pass actually managed to send.
   *
   * Separate from `withdrawn_` and `flattened_`, which count what was *asked for*. The difference is
   * the whole point: a flatten that queues a closing order the guard then refuses has
   * `flattened_ == 1` and `closing_placed_ == 0`, and an operator who only saw the first number
   * would believe they were out of the market while still holding the position.
   */
  std::size_t closing_placed_{0};
  /**
   * Closing instructions that were rejected: reconciliation could not build them, or the broker
   * refused them.
   *
   * Both are answers rather than doubts -- nothing needs reconciling either way -- and both leave
   * the position exactly where it was.
   */
  std::size_t closing_rejected_{0};
  /** Closing instructions a guard refused. */
  std::size_t closing_refused_{0};
  /** Closing instructions withheld because the instrument's state is unresolved. */
  std::size_t closing_withheld_{0};
  /**
   * Closing instructions whose send failed in transit, leaving their outcome unknown.
   *
   * Cancellations included. A cancellation that failed in transit may have withdrawn the order or
   * may not, which is not the same as one that went.
   */
  std::size_t closing_indeterminate_{0};
  /**
   * Closing instructions local machinery refused to send, so no broker was asked.
   *
   * The journal could not be written. Different from every count above in what an operator does
   * about it: fix the storage, `resume_after_local_failure`, and stop again.
   */
  std::size_t closing_unavailable_{0};
  /**
   * Closing cancellations naming an order the portfolio snapshot does not hold.
   *
   * The snapshot is stale rather than the instruction wrong, and the answer is to read the broker's
   * own list rather than to retry blind.
   */
  std::size_t closing_unresolved_{0};

  /**
   * Returns whether every closing instruction the loop asked for actually went out.
   *
   * **Every way a closing instruction can fail to go counts here**, which is the whole value of the
   * call: an operator must not get `true` from a stop where the journal was broken, where the
   * broker rejected a cancellation, or where a cancellation named an order a stale snapshot did not
   * list. Those failed silently before, because they were reported on the runner's report and never
   * carried onto this one.
   *
   * **It is not a confirmation of anything at the exchange, and must not be read as one.** It says
   * every instruction the loop asked for was *accepted by the broker*, and acceptance is where this
   * library's knowledge stops:
   *
   * - A closing placement that was accepted may not have **filled**. Only a later holdings snapshot
   *   shows that, and a `WITHDRAW` deliberately leaves positions held anyway.
   * - A cancellation that was accepted may not have **completed**. Cancellation is asynchronous at
   *   both brokers -- Toss publishes `PENDING_CANCEL` and `CANCEL_REJECTED`, which returns the order
   *   to its previous state -- so an accepted withdrawal can still be working.
   *
   * `true` therefore means "every instruction I asked for was taken", never "I am flat" and never
   * "nothing is resting". Read the broker's own order list and holdings to learn the state; this
   * call tells you only whether anything failed to go.
   */
  bool closing_complete() const noexcept {
    return closing_rejected_ == 0 && closing_refused_ == 0 && closing_withheld_ == 0 &&
      closing_indeterminate_ == 0 && closing_unavailable_ == 0 && closing_unresolved_ == 0;
  }
  /** Notes an operator should read, such as an unrecognized control-file token. */
  std::vector<std::string> notes_;

  /** Returns how many polls of `kind` were attempted. */
  std::size_t attempted(PollKind kind) const noexcept {
    return attempted_[static_cast<int>(kind)];
  }

  /** Returns how many polls of `kind` failed. */
  std::size_t failed(PollKind kind) const noexcept { return failed_[static_cast<int>(kind)]; }
};

/**
 * A source that answers nothing, for driving a loop in a test.
 *
 * The counterpart to `RecordingExecutor`, and the reason every loop behaviour in this header is
 * testable with no session, no credentials and no network. A test queues what each poll should
 * return and then asserts what the loop did with it.
 *
 * Deliberately not a mock of a broker. It is a mock of *arrival*: the question the loop answers is
 * "what do I do when data shows up, or does not, or the list read fails", and none of that needs a
 * broker to be interesting.
 */
template <typename Runner>
class ManualSource {
public:
  /** Queues what the next poll of `kind` returns. */
  void queue(PollKind kind, PollOutcome outcome) {
    queued_[static_cast<int>(kind)].push_back(outcome);
  }

  /** Sets what every unqueued poll of `kind` returns. Defaults to `UNCHANGED`. */
  void set_default(PollKind kind, PollOutcome outcome) {
    defaults_[static_cast<int>(kind)] = outcome;
  }

  /** Sets what the order-list read returns. */
  void set_order_list(PollOutcome outcome) { order_list_ = outcome; }

  /** Returns how many times the order list was read. */
  std::size_t order_list_reads() const noexcept { return order_list_reads_; }

  /** Returns how many times `kind` was polled. */
  std::size_t polls(PollKind kind) const noexcept { return polls_[static_cast<int>(kind)]; }

  /** Returns the queued outcome for `kind`, or its default. */
  PollOutcome poll(PollKind kind, Runner&) {
    const int at = static_cast<int>(kind);
    ++polls_[at];
    if (!queued_[at].empty()) {
      const auto outcome = queued_[at].front();
      queued_[at].erase(queued_[at].begin());
      return outcome;
    }
    return defaults_[at];
  }

  /** Returns the configured order-list outcome. */
  PollOutcome read_order_list(Runner&) {
    ++order_list_reads_;
    return order_list_;
  }

private:
  std::vector<PollOutcome> queued_[4];
  PollOutcome defaults_[4] = {
    PollOutcome::UNCHANGED,
    PollOutcome::UNCHANGED,
    PollOutcome::UNCHANGED,
    PollOutcome::UNCHANGED,
  };
  PollOutcome order_list_{PollOutcome::UPDATED};
  std::size_t polls_[4] = {0, 0, 0, 0};
  std::size_t order_list_reads_{0};
};

/**
 * Drives a runner for as long as a program lives, and stops on purpose.
 *
 * `Runner::feed` is one pass and caller-driven, which is right for a backtest and right for a single
 * live decision. What was missing is everything a program needs *around* it, and the hazard is
 * specific: nothing stopped a caller writing `while (true) { runner.feed(next()); }`, at which point
 * they owned a cadence, a rate-limit interaction, a reconciliation obligation and a stop control,
 * with documentation as the only thing between them and a doubled position.
 *
 * **Parameterized on a source, for the same reason the runner is parameterized on an executor.** A
 * polling loop looks like a broker concern and almost none of it is: when to poll next, noticing a
 * stop, refusing to send before the first reconciliation, and deciding a snapshot is too stale to
 * size against are all broker-free. Only the fetching is not. So `SessionSource` does the fetching
 * in `<algotradelib/live>`, `ManualSource` stands in for it here, and every behaviour worth testing
 * is reachable without a network.
 *
 * ## How sending is blocked
 *
 * Through `RiskGuard::halt()`, which is already the library's one documented primitive for "stop
 * sending without withdrawing or flattening" -- exactly what a gate needs, and exactly what
 * `QUIESCE` means. Reusing it means no change to `Runner::settle()` and no second parallel
 * mechanism that could disagree with the first.
 *
 * ## The trap a stop must not hide
 *
 * `RiskLimits::permitted_phases_` defaults to `{MarketPhase::OPEN}`, so **a withdraw or a flatten
 * issued outside market hours is refused by the guard** -- correctly, because there is no session to
 * trade in, but an operator who asked to be flat and was told nothing would believe they were. That
 * is why `LoopReport` counts what the closing pass *sent* separately from what the loop *asked for*,
 * and why a refusal produces a note saying the position is still held. `closing_complete()` is the
 * one call that answers "did my stop actually happen".
 *
 * **The loop holds the guard, and it names what it holds.** Its gates are
 * `strategy::HaltReason` values it takes and releases itself -- `ORDER_LIST_UNREAD`,
 * `PORTFOLIO_STALE`, `STOPPING`, `POLL_FAILING`. It never takes `OPERATOR` or `LOCAL_FAILURE`,
 * which belong to a person and to the runner, so an operator halting by hand while the loop's gate
 * is closed is not lost when that gate opens. The loop still *reports* those holds: `final_gate_`
 * names the most fundamental hold in force, whoever owns it, so a run that sent nothing because
 * somebody halted it says so instead of showing an open gate.
 */
template <typename Runner, typename Source, typename Clock = SteadyClock>
class PollingLoop {
public:
  /** Constructs a loop driving `runner` from `source`. Both are borrowed, not owned. */
  PollingLoop(Runner& runner, Source& source, PollSchedule schedule = {}, Clock clock = {})
  : runner_(&runner), source_(&source), schedule_(std::move(schedule)), clock_(std::move(clock)) {}

  /** Returns the clock, so a test can read how long the loop believed it waited. */
  const Clock& clock() const noexcept { return clock_; }
  Clock& clock() noexcept { return clock_; }

  /** Returns the schedule. */
  const PollSchedule& schedule() const noexcept { return schedule_; }

  /** Sets how stale a holdings snapshot may be before sending is blocked. */
  void set_maximum_portfolio_age(std::chrono::nanoseconds age) { maximum_portfolio_age_ = age; }

  /** Sets the failure policy. */
  void set_failure_policy(FailurePolicy policy) { failure_policy_ = std::move(policy); }

  /** Attaches a control file an operator can write an intent into. */
  void set_control_file(FileStopChannel channel) { control_ = std::move(channel); }

  /**
   * Runs until `stop` is requested, a poll exhausts its failure budget, or `max_iterations` pass.
   *
   * `max_iterations` exists for tests and for a bounded operational run; zero means unbounded. A
   * caller who wants "forever" passes nothing, and a caller who wants one pass has always had
   * `Runner::feed`.
   */
  LoopReport run(StopSource& stop, std::size_t max_iterations = 0) {
    LoopReport report;
    for (auto& due : next_due_) {
      due.reset();
    }
    for (auto& count : consecutive_failures_) {
      count = 0;
    }

    while (max_iterations == 0 || report.iterations_ < max_iterations) {
      const auto iteration = step(stop, report);
      ++report.iterations_;
      if (iteration.gate_) {
        ++report.blocked_;
      }
      report.final_gate_ = iteration.gate_;
      last_ = iteration;

      if (report.abandoned_) {
        break;
      }
      if (const auto intent = stop.intent()) {
        honour(*intent, report);
        report.stopped_by_ = intent;
        break;
      }
      clock_.wait_for(until_next(iteration.at_));
    }

    // Deliberately **not** released here. A run that ended because an operator asked it to, or
    // because a poll kept failing, must leave the runner unable to send: releasing the halt on the
    // way out would reopen the gate at exactly the moment the loop stopped watching it. A caller who
    // genuinely wants to trade again resumes the guard themselves, which is a deliberate act.
    //
    // A healthy run that simply exhausted `max_iterations` never halted anything, so there is
    // nothing to leave behind in that case.
    return report;
  }

  /** Returns what the last iteration did. */
  const IterationReport& last() const noexcept { return last_; }

private:
  /** Runs one iteration: poll what is due, then let the runner act if the gate is open. */
  IterationReport step(StopSource& stop, LoopReport& report) {
    IterationReport iteration;
    iteration.at_ = clock_.now();

    // Read before polling, so an operator's stop is honoured without first spending requests on a
    // broker the loop is about to walk away from.
    check_control_file(stop, report);

    // **Established before any poll runs, because a poll is what feeds the runner.** A source's
    // `poll` calls `Runner::feed` itself -- `live::SessionSource::poll_bars` does, per instrument --
    // and `feed` settles, so an order can leave the process from inside a poll. Computing the gate
    // afterwards would mean the startup gate and the staleness gate were installed one poll too
    // late, and on the very first iteration they would not be installed at all: a bar arriving
    // before the first order-list read could place an order against a position a predecessor may
    // have left, which is the exact hazard the startup gate exists for.
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/true));

    for (const auto kind : all_poll_kinds) {
      if (!is_due(kind, iteration.at_)) {
        continue;
      }
      const auto outcome = source_->poll(kind, *runner_);
      ++report.attempted_[static_cast<int>(kind)];
      iteration.polls_.emplace_back(kind, outcome);
      schedule_next(kind, iteration.at_);
      note_outcome(kind, outcome, report);
      if (outcome == PollOutcome::UPDATED) {
        iteration.fed_ = true;
      }
      if (report.abandoned_) {
        // Recorded before returning, so the report does not claim sending was permitted while the
        // guard `note_outcome` just halted says otherwise.
        iteration.gate_ = runner_->guard().blocking_reason();
        return iteration;
      }
    }

    // Re-evaluated after the polls so the report and the next iteration reflect what those polls
    // changed -- a holdings refresh should lift the staleness hold now, not one iteration later.
    // The order list is deliberately not attempted a second time: one attempt per iteration, and
    // the gate simply stands until the next one.
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/false));
    // Reported from the guard rather than from what the loop just computed, so the report names the
    // most fundamental hold in force whoever owns it -- an operator's halt, or a journal that
    // failed, outranks anything the loop decided.
    iteration.gate_ = runner_->guard().blocking_reason();
    return iteration;
  }

  /**
   * Returns which of the loop's *own* conditions blocks sending, reading the order list if due.
   *
   * Only conditions the loop computes, because a hold is released by whoever took it. `OPERATOR`
   * belongs to a person and `LOCAL_FAILURE` to the runner; a loop that took either would be a
   * second holder of someone else's hold -- the defect `HaltReason` exists to prevent,
   * reintroduced one layer up.
   *
   * **Always evaluated, even while someone else's hold stands.** Short-circuiting on a foreign hold
   * would leave the loop's own hold un-maintained underneath it, so the moment an operator resumed,
   * the guard would be wide open against a portfolio nobody had rechecked.
   *
   * Called twice per iteration -- before the polls, which is what actually protects anything, and
   * after them so the report is current. `may_read` is false on the second call so the order-list
   * read is attempted at most once per iteration.
   */
  std::optional<strategy::HaltReason> gate_for(
    const StopSource& stop,
    std::chrono::system_clock::time_point at,
    LoopReport& report,
    bool may_read
  ) {
    if (stop.requested()) {
      return strategy::HaltReason::STOPPING;
    }
    if (!order_list_read_) {
      // Not attempted when the caller says not to, and not attempted at all while the runner's
      // journal is broken: a runner that cannot record what it does must not be sent looking for
      // more work to fail at. Either way the gate stands, which is the safe answer.
      if (!may_read || runner_->guard().halted(strategy::HaltReason::LOCAL_FAILURE)) {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
      const auto outcome = source_->read_order_list(*runner_);
      if (outcome == PollOutcome::UPDATED || outcome == PollOutcome::UNCHANGED) {
        order_list_read_ = true;
        report.order_list_read_ = true;
      } else {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
    }
    if (maximum_portfolio_age_) {
      // Measured from the snapshot's own `as_of_` rather than from when the poll returned, because
      // that is when the broker says the positions were true. A poll that succeeded at noon carrying
      // a snapshot the broker took at 11:00 is an hour stale, and the poll's own timestamp would call
      // it fresh.
      const auto as_of = runner_->context().portfolio().as_of_;
      if (as_of == std::chrono::system_clock::time_point{}) {
        // A default-constructed `Portfolio` carries the epoch, and no broker ever reports a snapshot
        // taken in 1970, so the epoch means "never populated". Treated as stale rather than as
        // fresh-but-empty, which is the safe direction: an empty portfolio read as current would let
        // a strategy size as though it held nothing.
        return strategy::HaltReason::PORTFOLIO_STALE;
      }
      if (at - as_of > *maximum_portfolio_age_) {
        return strategy::HaltReason::PORTFOLIO_STALE;
      }
    }
    return std::nullopt;
  }

  /** Reads the control file, escalating `stop` when it names an intent. */
  void check_control_file(StopSource& stop, LoopReport& report) {
    if (!control_.enabled()) {
      return;
    }
    if (const auto intent = control_.poll()) {
      stop.request(*intent);
      return;
    }
    if (!control_.unrecognized().empty()) {
      // Reported rather than guessed at, because picking the nearest intent would decide how much of
      // an operator's position to close on their behalf.
      report.notes_.push_back(
        "control file names no intent: '" + control_.unrecognized() + "'");
    }
  }

  /** Counts an outcome, and abandons the run when one kind keeps failing. */
  void note_outcome(PollKind kind, PollOutcome outcome, LoopReport& report) {
    const int at = static_cast<int>(kind);
    if (outcome == PollOutcome::FAILED) {
      ++report.failed_[at];
      ++consecutive_failures_[at];
      if (consecutive_failures_[at] >= failure_policy_.consecutive_limit_) {
        report.abandoned_ = true;
        report.abandoned_kind_ = kind;
        report.notes_.push_back(
          std::string("gave up after repeated failures polling ") + std::string(poll_kind_name(kind)));
        hold(strategy::HaltReason::POLL_FAILING);
      }
      return;
    }
    // A rate-limit refusal is the limiter working, and nothing new is the normal case between bars.
    // Neither resets the counter either: a kind alternating failure and refusal is still broken.
    if (outcome != PollOutcome::RATE_LIMITED) {
      consecutive_failures_[at] = 0;
    }
  }

  /** Carries out `intent`, which is where the three stops actually differ. */
  void honour(StopIntent intent, LoopReport& report) {
    hold(strategy::HaltReason::STOPPING);
    if (intent == StopIntent::QUIESCE) {
      return;
    }

    // The two intents reach the same end by different routes, and the difference is not cosmetic.
    //
    // A **withdraw** cancels explicitly, because nothing else will: there is no target to reconcile
    // against, so the working orders have to be named one by one. A cancellation is the one
    // instruction that cannot make an unresolved position worse, which is why the runner does not
    // gate it on the ledger.
    //
    // A **flatten** states a target of zero and lets reconciliation do the cancelling. Issuing
    // explicit cancels *and* a target would send two cancellations for the same order -- the loop's
    // own, then reconciliation's, because the portfolio snapshot still lists an order the loop just
    // cancelled. Reconciliation already knows how to withdraw what conflicts with a target and keep
    // what helps reach it, and duplicating that here would be both wasteful and less correct.
    const auto& portfolio = runner_->context().portfolio();
    if (intent == StopIntent::WITHDRAW) {
      for (const auto& working : portfolio.working_) {
        runner_->context().cancel(working.ref_, "stop-withdraw");
        ++report.withdrawn_;
      }
    } else {
      for (const auto& position : portfolio.positions_) {
        if (position.quantity_ == Decimal64{}) {
          // A closed position can sit in a snapshot at zero. A sell for none of it is an order the
          // broker would reject, for nothing.
          continue;
        }
        strategy::TargetPosition target;
        target.instrument_ = position.instrument_;
        target.target_quantity_ = Decimal64{};
        target.tag_ = "stop-flatten";
        runner_->context().submit(std::move(target));
        ++report.flattened_;
      }
    }

    // Lifted so the closing instructions can actually go out -- a withdraw that cannot send is not a
    // withdraw -- and closed again immediately afterwards. The window is exactly one `stop()` call
    // wide, and it is the only point at which a stop in progress can send anything.
    //
    // Only the loop's *own* hold lifts. An operator who halted by hand, or a runner whose journal
    // failed, still refuses every closing instruction -- placements through `RiskGuard::check` and
    // cancellations through the same holds in `Runner::send_cancel` -- and the closing counts say
    // so, so `closing_complete()` is false and the notes say the position is still held. A stop
    // arriving by signal or control file therefore cannot send anything through a halt somebody had
    // already set.
    hold(std::nullopt);
    const auto closing = runner_->stop();
    hold(strategy::HaltReason::STOPPING);

    report.closing_placed_ = closing.would_place_.size();
    if (intent == StopIntent::FLATTEN) {
      // Counted from every cancellation reconciliation produced, not only the ones that went: a
      // flatten does not cancel directly, and `withdrawn_` is what was *asked for*. The counts below
      // are what happened to them.
      report.withdrawn_ = closing.would_cancel_.size() + closing.indeterminate_cancel_.size() +
        closing.unavailable_cancel_.size() + closing.refused_cancel_.size() +
        closing.unresolved_cancel_.size() + closing.rejected_cancel_.size();
    }
    // **Both halves of every bucket.** A closing instruction is a placement or a cancellation, and
    // each can fail in the same five ways; counting only the placement half is how a stop that sent
    // nothing reported itself complete.
    report.closing_rejected_ = closing.rejected_.size() + closing.rejected_cancel_.size();
    report.closing_refused_ = closing.refused_.size() + closing.refused_cancel_.size();
    report.closing_withheld_ = closing.withheld_.size();
    report.closing_indeterminate_ =
      closing.indeterminate_.size() + closing.indeterminate_cancel_.size();
    report.closing_unavailable_ = closing.unavailable_.size() + closing.unavailable_cancel_.size();
    report.closing_unresolved_ = closing.unresolved_cancel_.size();

    // Named individually rather than summarized, because "your flatten did not go out" is the one
    // note in this whole report an operator must not have to infer.
    if (!closing.refused_.empty() || !closing.refused_cancel_.empty()) {
      report.notes_.push_back(
        "a guard refused " + std::to_string(report.closing_refused_) +
        " closing instruction(s); the position is still held");
    }
    if (!closing.rejected_.empty()) {
      report.notes_.push_back(
        "reconciliation could not build " + std::to_string(closing.rejected_.size()) +
        " closing instruction(s); the position is still held");
    }
    if (!closing.rejected_cancel_.empty()) {
      report.notes_.push_back(
        "the broker rejected " + std::to_string(closing.rejected_cancel_.size()) +
        " closing cancellation(s); read its order list for their state");
    }
    if (report.closing_withheld_ != 0) {
      report.notes_.push_back(
        "withheld " + std::to_string(report.closing_withheld_) +
        " closing instruction(s) pending reconciliation; read the broker's order list");
    }
    if (report.closing_indeterminate_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_indeterminate_) +
        " closing instruction(s) failed in transit; their outcome is unknown");
    }
    if (report.closing_unavailable_ != 0) {
      report.notes_.push_back(
        "local machinery could not send " + std::to_string(report.closing_unavailable_) +
        " closing instruction(s); no broker was asked and the position is still held");
    }
    if (report.closing_unresolved_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_unresolved_) +
        " closing cancellation(s) named an order the portfolio does not hold; read the broker's"
        " order list");
    }
  }

  /**
   * Takes `gate`'s hold on the guard and releases the one this loop took before it.
   *
   * There is no "did I halt it" flag any more, and that is the point: the loop names the hold it
   * takes and releases only that one, so an operator halting by hand into a loop's closed gate is
   * no longer invisible, and the loop reopening its gate no longer lifts their halt with it. The
   * hold is released and retaken rather than diffed, because taking a hold is idempotent.
   *
   * Only the loop's *own* conditions are held. `OPERATOR` belongs to a person and `LOCAL_FAILURE`
   * to the runner; both outlive any gate the loop computes, so they are reported and never taken or
   * released here.
   */
  void hold(std::optional<strategy::HaltReason> gate) {
    if (gate && !owns(*gate)) {
      gate.reset();
    }
    if (held_ == gate) {
      return;
    }
    if (held_) {
      runner_->guard().resume(*held_);
    }
    if (gate) {
      runner_->guard().halt(*gate);
    }
    held_ = gate;
  }

  /** Returns whether `kind` is due at `at`. */
  bool is_due(PollKind kind, std::chrono::system_clock::time_point at) const {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return false;
    }
    const auto& due = next_due_[static_cast<int>(kind)];
    return !due || at >= *due;
  }

  /** Records when `kind` is next due. */
  void schedule_next(PollKind kind, std::chrono::system_clock::time_point at) {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return;
    }
    next_due_[static_cast<int>(kind)] =
      at + std::chrono::duration_cast<std::chrono::system_clock::duration>(*interval);
  }

  /**
   * Returns how long to wait before the next iteration.
   *
   * The soonest due poll, capped at the tick so a stop through the control file is noticed within
   * one tick however sparse the polling is. Never negative: a loop that has fallen behind catches up
   * rather than pausing once per overdue poll.
   */
  std::chrono::nanoseconds until_next(std::chrono::system_clock::time_point at) const {
    auto wait = schedule_.tick_;
    for (const auto kind : all_poll_kinds) {
      const auto& due = next_due_[static_cast<int>(kind)];
      if (!due) {
        continue;
      }
      const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(*due - at);
      if (remaining < wait) {
        wait = remaining;
      }
    }
    return wait > std::chrono::nanoseconds::zero() ? wait : std::chrono::nanoseconds::zero();
  }

  Runner* runner_;
  Source* source_;
  PollSchedule schedule_;
  Clock clock_;
  FileStopChannel control_;
  FailurePolicy failure_policy_;
  std::optional<std::chrono::nanoseconds> maximum_portfolio_age_;
  std::optional<std::chrono::system_clock::time_point> next_due_[4];
  std::size_t consecutive_failures_[4] = {0, 0, 0, 0};
  bool order_list_read_{false};
  /** Returns whether `reason` is a condition this loop computes, and may therefore hold. */
  static constexpr bool owns(strategy::HaltReason reason) noexcept {
    return reason != strategy::HaltReason::OPERATOR &&
      reason != strategy::HaltReason::LOCAL_FAILURE;
  }

  /** The hold this loop currently owns, and the only one it will ever release. */
  std::optional<strategy::HaltReason> held_;
  IterationReport last_;
};

} // namespace runner
} // namespace algotradelib
