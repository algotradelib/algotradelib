#pragma once

/**
 * @file
 * @brief The production wiring of a live run, with no broker named: the part of
 * `example-live-program` a consumer keeps.
 *
 * `example-live-program.cpp` chooses a broker and calls `live_program::trade` or
 * `live_program::observe`. Everything that makes a run safe to restart is here, in the order it has
 * to happen, and the test suite drives it against an in-memory session -- which a `main` could not be.
 *
 * The order is the whole point, and each step exists because leaving it out fails silently:
 *
 *  1. **Open the journal before binding.** A runner that cannot record what it sends should not send.
 *  2. **Bind through the journal** (`live::bind_journaled`). `live::bind` has no journal, so an order in
 *     flight when the process dies leaves no trace, and the next start may send it again.
 *  3. **Recover before the loop starts** (`runner::recover_runner`), so whatever a previous process
 *     left owed is withheld until the broker's own lists resolve it.
 *  4. **Let the source resolve the journal** (`SessionSource::set_journal`). Without it every
 *     recovered instrument stays withheld forever.
 *  5. **State every age the loop asks about.** A run that states none holds; a run that says
 *     "unbounded" sizes against a portfolio nobody bounded.
 *  6. **Stop by signal or by control file**, and read `closing_complete()` at the end.
 */

#include <algotradelib/live>
#include <algotradelib/runner>

#include <chrono>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace live_program {

namespace atl = algotradelib;
namespace live = algotradelib::live;
namespace runner = algotradelib::runner;
namespace strategy = algotradelib::strategy;

/** What a run is told, gathered so `main` states it once and a test can state it differently. */
struct Settings {
  /** The instruments the source polls. */
  std::vector<atl::InstrumentId> universe_;
  /** The limits the guard enforces. */
  strategy::RiskLimits limits_;
  /**
   * How often each kind is polled. `trade` needs `working_orders_` set, shorter than
   * `maximum_portfolio_age_`: a runner that sends orders keeps them until the listing or holdings
   * account for them, and the loop holds (with a note) while no listing will come.
   */
  runner::PollSchedule schedule_;
  /** Which bars the source asks for, and how many: the history read at start is this many. */
  atl::BarInterval bar_interval_{atl::BarInterval::ONE_DAY};
  /** How many bars each bar poll asks for. */
  int bar_count_{100};
  /** How old a holdings snapshot -- and the order listing -- may be before the loop stops sizing against it. */
  std::chrono::nanoseconds maximum_portfolio_age_{std::chrono::minutes(2)};
  /** How old a price band may be before the loop stops clamping to it. */
  std::chrono::nanoseconds maximum_price_limits_age_{std::chrono::minutes(20)};
  /** Where the order journal lives. */
  std::string journal_path_;
  /** Which account the journal belongs to; a journal naming another refuses to open. */
  std::string journal_scope_;
  /** The file an operator writes `quiesce`, `withdraw` or `flatten` into to stop the run. */
  std::string control_file_;
  /** Iterations before returning; 0 runs until a stop. */
  std::size_t max_iterations_{0};
  /**
   * The venue an order is sent to, since a strategy's orders name none; `UNSPECIFIED` takes the
   * broker's own default. Kiwoom has none, so a Kiwoom run must state one. See
   * `live::SessionExecutor::default_route`.
   */
  atl::Venue default_route_{atl::Venue::UNSPECIFIED};
};

/** What the program exits with, so a supervisor can tell its endings apart. */
enum class Exit : int {
  /** The run ended, and any closing pass it made got every instruction out. */
  CLEAN = 0,
  /** A stop that closes ran and `closing_complete()` is false: read the broker's order list. */
  CLOSING_INCOMPLETE = 1,
  /** The journal would not open, so nothing was bound. */
  JOURNAL_REFUSED = 2,
  /** Recovery halted the runner; nothing was started. */
  RECOVERY_HALTED = 3,
  /** Reference data could not be read, so no order could be priced; nothing was started. */
  REFERENCE_UNREADABLE = 4,
  /** The loop gave up after repeated poll failures: it was not trading when it returned. */
  LOOP_ABANDONED = 5,
  /**
   * Local machinery failed during the run -- usually a journal write -- and the runner halted itself.
   * Whatever it was sending when that happened is unknown until the broker's order list is read.
   */
  LOCAL_FAILURE = 6,
  /** The program could not settle which account to trade (used by `example-live-program`). */
  ACCOUNT_UNRESOLVED = 7,
};

/**
 * @brief Reads each instrument's reference data (lot size, price grid) onto the runner, once; the loop
 *   does not poll it, and no order can be built without it.
 * @tparam Session The session type.
 * @tparam Runner The runner type.
 * @param session The session to read from.
 * @param bound The runner.
 * @param settings The settings (its universe).
 * @param out Where to print a failure.
 * @return `true` if every instrument was read; otherwise prints why and returns `false`.
 */
template <typename Session, typename Runner>
bool load_reference_data(Session& session, Runner& bound, const Settings& settings, std::ostream& out) {
  for (const auto& instrument : settings.universe_) {
    auto reference = session.instrument(instrument);
    if (!reference) {
      out << "could not read reference data for " << instrument.symbol_ << ": "
          << reference.error().message_ << '\n';
      return false;
    }
    bound.context().set_instrument(std::move(reference.value()));
  }
  return true;
}

/**
 * @brief Prints what recovery found, in the words an operator acts on.
 * @param recovery The recovery report.
 * @param out Where to print.
 */
inline void print_recovery(const runner::RecoveryReport& recovery, std::ostream& out) {
  if (recovery.clean()) {
    out << "recovery: clean -- the journal owes nothing\n";
    return;
  }
  out << "recovery: NOT clean\n";
  for (const auto& instrument : recovery.marked_) {
    out << "  withheld until the broker's lists resolve it: " << instrument.symbol_ << '\n';
  }
  if (recovery.truncated_) {
    out << "  the last line was torn: the previous process died while writing it\n";
  }
  if (recovery.unreadable_lines_ > 0) {
    out << "  " << recovery.unreadable_lines_ << " line(s) this library did not write\n";
  }
  if (recovery.unreadable_file_) {
    out << "  the journal exists and could not be read\n";
  }
  if (recovery.unattributable_) {
    out << "  an unresolved instruction names no instrument\n";
  }
  if (!recovery.detail_.empty()) {
    out << "  detail: " << recovery.detail_ << '\n';
  }
}

/**
 * @brief Prints how to stop the run, before it starts, so nobody reaches for `kill -9`.
 * @param settings The settings (its control file).
 * @param out Where to print.
 */
inline void print_how_to_stop(const Settings& settings, std::ostream& out) {
  out << "to stop: Ctrl-C or SIGTERM (quiesce), or\n"
      << "  echo quiesce  > " << settings.control_file_ << "   # stop placing, leave orders resting\n"
      << "  echo withdraw > " << settings.control_file_ << "   # cancel the account's working orders\n"
      << "  echo flatten  > " << settings.control_file_ << "   # withdraw, then close the account's positions\n"
      << "the file is not deleted: a word left in it is carried out at the next start; remove it\n"
      << "before trading again\n"
      << "a supervisor's grace period must exceed one loop iteration plus one request timeout\n";
}

/**
 * @brief Prints the loop's own figures and notes, which read the same in both modes.
 * @param report The loop report.
 * @param out Where to print.
 */
inline void print_loop(const runner::LoopReport& report, std::ostream& out) {
  out << "\niterations      : " << report.iterations_ << '\n'
      << "order list read : " << (report.order_list_read_ ? "yes" : "no") << '\n'
      << "polls failed    : bars " << report.failed(runner::PollKind::BARS) << '/'
      << report.attempted(runner::PollKind::BARS) << ", holdings "
      << report.failed(runner::PollKind::HOLDINGS) << '/' << report.attempted(runner::PollKind::HOLDINGS)
      << ", orders " << report.failed(runner::PollKind::WORKING_ORDERS) << '/'
      << report.attempted(runner::PollKind::WORKING_ORDERS) << '\n'
      << "blocked         : " << report.blocked_ << " iteration(s)\n"
      << "stopped by      : "
      << (report.stopped_by_ ? runner::stop_intent_name(*report.stopped_by_) : std::string_view("nothing"))
      << '\n'
      << "final gate      : "
      << (report.final_gate_ ? strategy::halt_reason_name(*report.final_gate_) : std::string_view("none"))
      << '\n'
      << "market phase    : " << strategy::market_phase_name(report.final_phase_) << '\n'
      // Every way an order the strategy wanted did not go out, each with its own count, so an empty
      // placement count explains itself.
      << "plans withheld  : " << report.plans_withheld_ << " (account unread, too old or older than the order listing, or a hold while it would cancel)\n"
      << "not buildable   : " << report.orders_unbuildable_ << '\n'
      << "withheld        : " << report.orders_reconcile_withheld_ << " (unresolved earlier orders)\n"
      << "refused by guard: " << report.orders_guard_refused_ << '\n'
      << "refused locally : " << report.orders_binding_refused_ << " (by the adapter, before sending)\n"
      << "refused by broker: " << report.orders_broker_refused_ << '\n'
      << "unknown outcome : " << report.orders_indeterminate_ << " (read the broker's order list)\n"
      << "unconfirmed     : " << report.orders_unconfirmed_timed_out_
      << " (sent, and not confirmed in time: the instrument is withheld until you clear it)\n";
  if (report.last_unbuildable_) {
    out << "last unbuildable: " << strategy::reject_reason_name(report.last_unbuildable_->reason_) << " for "
        << report.last_unbuildable_->instrument_.symbol_
        << (report.last_unbuildable_->tag_.empty() ? "" : " (" + report.last_unbuildable_->tag_ + ")") << '\n';
  }
  if (report.last_broker_refusal_) {
    out << "last refusal    : " << report.last_broker_refusal_->code_ << ' '
        << report.last_broker_refusal_->message_ << '\n';
  }
  if (report.abandoned_) {
    out << "ABANDONED       : after repeated failures polling "
        << (report.abandoned_kind_ ? runner::poll_kind_name(*report.abandoned_kind_) : std::string_view("?"))
        << '\n';
  }
  for (const auto& note : report.notes_) {
    out << "note            : " << note << '\n';
  }
}

/**
 * @brief Prints the loop report and what `closing_complete()` does and does not mean.
 * @param report The loop report.
 * @param out Where to print.
 */
inline void print_report(const runner::LoopReport& report, std::ostream& out) {
  print_loop(report, out);
  out << "closing complete: " << (report.closing_complete() ? "yes" : "NO")
      << " -- whether every closing instruction was taken, not whether the account is flat;\n"
      << "                  read the broker's order list and holdings to learn that\n";
}

/**
 * @brief Tests whether a run ended with a stop that sends closing instructions.
 * @param report The loop report.
 * @return `true` for a `WITHDRAW` or `FLATTEN` stop.
 */
inline bool stop_closes(const runner::LoopReport& report) noexcept {
  return report.stopped_by_ && *report.stopped_by_ != runner::StopIntent::QUIESCE;
}

/**
 * @brief Configures the loop the same way for both modes: every age stated, the control file installed.
 * @tparam Loop The loop type.
 * @param loop The loop.
 * @param settings The settings.
 */
template <typename Loop>
void configure(Loop& loop, const Settings& settings) {
  loop.set_maximum_portfolio_age(settings.maximum_portfolio_age_);
  loop.set_maximum_price_limits_age(settings.maximum_price_limits_age_);
  loop.set_control_file(runner::FileStopChannel(settings.control_file_));
}

/**
 * @brief Trades a strategy through a session, reading market data and reference data from another.
 *
 * **This places orders**, through `session` only; `market_data` is only read. Use a session that
 * cannot trade for `market_data` (a KIS session with an empty `KisAccount`, a Toss session with
 * account sequence 0), so that a swapped pair refuses rather than trades.
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The trading session type.
 * @tparam MarketData The market-data session type.
 * @tparam Clock The clock type.
 * @param session The session orders go through.
 * @param market_data The session market data is read from.
 * @param strategy_instance The strategy.
 * @param settings The settings.
 * @param stop The stop source the loop polls.
 * @param out Where the program prints.
 * @param clock The clock.
 * @return The exit code.
 */
template <typename Strategy, typename Session, typename MarketData, typename Clock = runner::SteadyClock>
Exit trade(
  Session& session,
  MarketData& market_data,
  Strategy strategy_instance,
  const Settings& settings,
  runner::StopSource& stop,
  std::ostream& out,
  Clock clock = {}
) {
  runner::FileJournal journal(settings.journal_path_, settings.journal_scope_);
  if (!journal.open()) {
    out << "journal refused: " << journal.rejection() << '\n'
        << "a runner that cannot record what it sends does not send\n";
    return Exit::JOURNAL_REFUSED;
  }

  auto bound = live::bind_journaled(
    session, journal, std::move(strategy_instance), settings.limits_, live::default_series_capacity,
    settings.default_route_
  );

  const auto recovery = runner::recover_runner(bound, journal);
  print_recovery(recovery, out);
  if (const auto failure = bound.local_failure()) {
    // Damage that names no single instrument halts everything, and restarting will find the same
    // line. There is deliberately no flag to trade past it: a flag that is needed on every restart
    // ends up in a unit file. The way out is by hand, and it keeps the evidence.
    out << "HALTED: " << *failure << '\n'
        << "nothing was started. To recover:\n"
        << "  1. read the broker's order list and holdings for this account yourself;\n"
        << "  2. resolve every order this journal may owe (cancel it, or accept that it exists);\n"
        << "  3. move the journal aside, keeping it:  mv " << settings.journal_path_ << ' '
        << settings.journal_path_ << ".damaged-$(date +%Y%m%d)\n"
        << "the next start then begins a new journal.\n";
    return Exit::RECOVERY_HALTED;
  }

  if (!load_reference_data(market_data, bound, settings, out)) {
    return Exit::REFERENCE_UNREADABLE;
  }
  // Said before trading as well as in the report at the end, so it is read before it matters.
  if constexpr (decltype(bound)::on_fill_never_fires) {
    out << "note: this strategy defines on_fill, which never fires on a live runner; anything it\n"
        << "      does there (a protective stop, say) will not happen. See writing-a-strategy.md.\n";
  }

  live::SessionSource<Session, runner::FileJournal, MarketData> source(
    session, market_data, settings.universe_
  );
  source.set_journal(journal);
  source.set_bar_query(settings.bar_interval_, settings.bar_count_);

  runner::PollingLoop<decltype(bound), decltype(source), Clock> loop(
    bound, source, settings.schedule_, std::move(clock)
  );
  configure(loop, settings);

  print_how_to_stop(settings, out);
  const auto report = loop.run(stop, settings.max_iterations_);
  print_report(report, out);

  // Checked before abandonment: sticky, set by a failed journal write as much as by recovery, and
  // the more urgent of the two -- what the runner was sending at that moment is unknown. A run can
  // end both ways, and the report above still says ABANDONED.
  if (const auto failure = bound.local_failure()) {
    out << "LOCAL FAILURE   : " << *failure << '\n'
        << "                  read the broker's order list before starting again\n";
    return Exit::LOCAL_FAILURE;
  }
  if (report.abandoned_) {
    return Exit::LOOP_ABANDONED;
  }
  return stop_closes(report) && !report.closing_complete() ? Exit::CLOSING_INCOMPLETE : Exit::CLEAN;
}

/**
 * @brief Trades a strategy through a session: journal, recover, loop, report.
 *
 * **This places orders.** `observe` is the same loop with nowhere to send.
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The session type.
 * @tparam Clock The clock type.
 * @param session The session (orders and market data).
 * @param strategy_instance The strategy.
 * @param settings The settings.
 * @param stop The stop source the loop polls.
 * @param out Where the program prints.
 * @param clock The clock.
 * @return The exit code.
 */
template <typename Strategy, typename Session, typename Clock = runner::SteadyClock>
Exit trade(
  Session& session,
  Strategy strategy_instance,
  const Settings& settings,
  runner::StopSource& stop,
  std::ostream& out,
  Clock clock = {}
) {
  return trade(session, session, std::move(strategy_instance), settings, stop, out, std::move(clock));
}

/**
 * @brief Runs a strategy against a session's account and another's market data, with nothing to send
 *   through.
 *
 * `live::bind_observer` gives the runner the session's real capabilities and no executor that reaches
 * the broker, so the pipeline, the guard and the gates all run, and no order can leave. No journal is
 * opened.
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The account session type.
 * @tparam MarketData The market-data session type.
 * @tparam Clock The clock type.
 * @param session The account session.
 * @param market_data The session market data is read from.
 * @param strategy_instance The strategy.
 * @param settings The settings.
 * @param stop The stop source the loop polls.
 * @param out Where the program prints.
 * @param clock The clock.
 * @return The exit code.
 */
template <typename Strategy, typename Session, typename MarketData, typename Clock = runner::SteadyClock>
Exit observe(
  Session& session,
  MarketData& market_data,
  Strategy strategy_instance,
  const Settings& settings,
  runner::StopSource& stop,
  std::ostream& out,
  Clock clock = {}
) {
  auto bound = live::bind_observer(session, std::move(strategy_instance), settings.limits_);
  if (!load_reference_data(market_data, bound, settings, out)) {
    return Exit::REFERENCE_UNREADABLE;
  }

  live::SessionSource<Session, runner::MemoryJournal, MarketData> source(
    session, market_data, settings.universe_
  );
  source.set_bar_query(settings.bar_interval_, settings.bar_count_);
  runner::PollingLoop<decltype(bound), decltype(source), Clock> loop(
    bound, source, settings.schedule_, std::move(clock)
  );
  configure(loop, settings);

  out << "observing: nothing can be sent (pass --live to trade)\n";
  print_how_to_stop(settings, out);
  const auto report = loop.run(stop, settings.max_iterations_);
  print_loop(report, out);
  // `closing_complete()` is deliberately not printed: an observer's executor accepts every
  // instruction without sending it, so "every closing instruction was taken" would be true and would
  // say nothing about the broker.
  out << "closing         : simulated -- the observer sent nothing, so nothing at the broker was\n"
      << "                  placed or withdrawn by this run\n";
  return report.abandoned_ ? Exit::LOOP_ABANDONED : Exit::CLEAN;
}

/**
 * @brief Observes a strategy against a session's real data, reading market data from it too.
 * @tparam Strategy The strategy type.
 * @tparam Session The session type.
 * @tparam Clock The clock type.
 * @param session The session.
 * @param strategy_instance The strategy.
 * @param settings The settings.
 * @param stop The stop source the loop polls.
 * @param out Where the program prints.
 * @param clock The clock.
 * @return The exit code.
 */
template <typename Strategy, typename Session, typename Clock = runner::SteadyClock>
Exit observe(
  Session& session,
  Strategy strategy_instance,
  const Settings& settings,
  runner::StopSource& stop,
  std::ostream& out,
  Clock clock = {}
) {
  return observe(session, session, std::move(strategy_instance), settings, stop, out, std::move(clock));
}

} // namespace live_program
