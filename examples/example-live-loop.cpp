/**
 * @file
 * @brief A live runner loop with no broker, no credentials and no network.
 *
 * Runs the real `runner::PollingLoop` -- the same class a live session drives -- against a source
 * that makes its data up. That is possible because the loop is parameterized on where data comes
 * from, exactly as the runner is parameterized on where orders go, so everything about cadence,
 * stopping and the safety gates is exercisable with nothing on the wire.
 *
 * It is also a **template for a broker this library does not support**. `ScriptedSource` below is a
 * complete source in forty lines: anything that can answer "any new bars?" can drive this loop, and
 * the strategy above it does not change. Swap the body for your own HTTP calls and the rest of the
 * stack -- reconciliation, the guards, the indeterminacy rules, the stop intents -- comes along.
 *
 * Deliberately uses `ManualClock`, so the whole run finishes instantly rather than sleeping through a
 * cadence. A real program passes nothing and gets `SteadyClock`.
 */

#include <algotradelib/runner>
#include <algotradelib/vanilla>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;

namespace {

using Clock = std::chrono::system_clock;

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** Reference data, without which nothing can be priced onto a tick grid. */
Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/** A strategy that simply counts, so the output is about the loop rather than about signals. */
struct CountBars {
  std::size_t bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    ++bars_;
    ctx.log().note("saw bar " + std::to_string(bars_));
  }
};

using LoopRunner = runner::DryRunner<CountBars>;

/**
 * A source that invents a rising price.
 *
 * This is the whole seam for a dry run. A source owes the loop two things:
 *
 *  - `poll(kind, runner)` -- fetch that kind, feed what is new, and say what happened.
 *  - `read_order_list(runner)` -- read the broker's own order list, which is what lifts the startup
 *    gate. Until it succeeds once, the loop refuses to place anything at all.
 *
 * **Refuses to place**, not to send: a hold that means only that the loop is uninformed still lets a
 * cancellation the caller named through, because an order that cannot be withdrawn keeps resting and
 * can fill. `strategy::halt_blocks` is where that division is written down.
 *
 * Note what is *not* here: no cadence, no stop handling, no staleness rule, no failure budget. Those
 * belong to the loop, which is why a new broker does not have to reimplement them.
 *
 * **Two things are enough only because `LoopRunner` sends nothing.** A source feeding a runner that
 * sends to a broker also owes a working-order listing (published, polled, replacing `working_`
 * wholesale), holdings polls that keep `working_`, and bars fed once each, never while still forming,
 * with a read's older bars through `Runner::feed_warmup`. "Trying it without a broker" in
 * running-a-strategy-live.md lists them; `live::SessionSource` does each.
 */
class ScriptedSource {
public:
  explicit ScriptedSource(int bars) : remaining_(bars) {}

  runner::PollOutcome poll(runner::PollKind kind, LoopRunner& live_runner) {
    if (kind == runner::PollKind::PRICE_LIMITS) {
      // Today's permitted band, which reconciliation clamps a limit price into. A real source calls
      // `session.price_limits(instrument)` per instrument here; what matters is that it lands on the
      // runner rather than being fed as an event, and that it arrives **on a cadence** -- the band
      // is a per-trading-day value, so one read at startup describes the day the process started on.
      PriceLimits band;
      band.currency_ = Currency{"KRW"};
      band.upper_ = Decimal64::parse("91000");
      band.lower_ = Decimal64::parse("49000");
      live_runner.set_price_limits(samsung(), band);
      return runner::PollOutcome::UPDATED;
    }
    if (kind != runner::PollKind::BARS) {
      return runner::PollOutcome::UNCHANGED;
    }
    if (remaining_ <= 0) {
      // Nothing new is the normal case between closes, and is not a failure.
      return runner::PollOutcome::UNCHANGED;
    }
    --remaining_;

    Bar bar;
    bar.timestamp_ = Clock::time_point{} + std::chrono::hours(24 * (200 + fed_));
    bar.close_ = Decimal64::parse(std::to_string(70000 + fed_ * 500));
    bar.high_ = bar.close_;
    bar.low_ = bar.close_;
    bar.volume_ = Decimal64::parse("1000000");
    ++fed_;

    live_runner.context().set_last_price(samsung(), bar.close_);
    live_runner.feed(
      strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar)
    );
    return runner::PollOutcome::UPDATED;
  }

  runner::PollOutcome read_order_list(LoopRunner&) {
    ++reads_;
    // A real source calls working_orders() or order_history() here. Returning a failure until the
    // second attempt shows the gate doing its job.
    return reads_ < 2 ? runner::PollOutcome::FAILED : runner::PollOutcome::UPDATED;
  }

  int reads() const noexcept { return reads_; }

private:
  int remaining_;
  int fed_{0};
  int reads_{0};
};

/** Prints a loop report the way an operator would want to read it. */
void describe(const runner::LoopReport& report) {
  std::cout << "iterations      : " << report.iterations_ << '\n'
            << "bar polls       : " << report.attempted(runner::PollKind::BARS)
            << " (" << report.failed(runner::PollKind::BARS) << " failed)\n"
            << "blocked         : " << report.blocked_ << " iteration(s)\n"
            << "order list read : " << (report.order_list_read_ ? "yes" : "no") << '\n'
            << "stopped by      : "
            << (report.stopped_by_ ?
                  runner::stop_intent_name(*report.stopped_by_) :
                  std::string_view("nothing"))
            << '\n'
            << "final gate      : "
            << (report.final_gate_ ?
                 strategy::halt_reason_name(*report.final_gate_) :
                 std::string_view("none"))
            << '\n'
            // The phase the loop derived for itself, and how many changes it announced. Reported
            // because it decides most of what could be sent: `unknown` beside a default
            // `permitted_phases_` means every placement that would open a position was refused, and
            // an empty placement count on its own does not say that.
            << "market phase    : " << strategy::market_phase_name(report.final_phase_)
            << " (" << report.phase_transitions_ << " transition(s))\n"
            // And how many orders went out under a phase the limits do not permit. An order closing
            // a held position is permitted in any phase -- which is what lets a flatten after the
            // close reach the broker instead of being refused by the operator's own limit -- so this
            // is the line that says it happened. Non-zero is not a fault; a nightly flatten is one.
            << "sent off-phase  : " << report.sends_outside_permitted_phase_ << '\n'
            // And what the price-band policy decided. A band is a per-trading-day value, so a run
            // that outlives the day it read one has to decide what to do with it: this library does
            // not apply a band whose age nobody has bounded, and says so here rather than repricing
            // an order against yesterday's ceiling in silence. Both stay zero in this example
            // because `CountBars` places nothing -- the first counts plans, the second counts sent
            // orders a broker refused after a band was withheld.
            << "bands withheld  : " << report.price_bands_withheld_ << '\n'
            << "refused unclamped: " << report.unclamped_orders_rejected_ << '\n';
  for (const auto& note : report.notes_) {
    std::cout << "note            : " << note << '\n';
  }
}

/**
 * When this example's clock starts: 200 days after the epoch, at 09:00 in Seoul.
 *
 * Named rather than repeated, because three things now have to agree about it -- the clock, the
 * calendar's covered date, and the session window the phase is derived against. They used to be one.
 */
Clock::time_point start_time() {
  return Clock::time_point{} + std::chrono::hours(24 * 200);
}

} // namespace

/**
 * @brief Runs the polling loop end to end with no broker, no credentials and no network.
 * @return 0 on success.
 */
int main() {
  std::cout << "a live loop with no broker attached\n\n";

  LoopRunner live_runner;
  live_runner.context().set_instrument(samsung_reference());

  // **The phase is the loop's, not this program's.** This used to read
  // `set_phase(MarketPhase::OPEN)`, which froze a value nothing ever updated -- so the run traded as
  // though the market were permanently open and `on_phase` never fired at all. The loop now derives
  // the phase from its session window and the trading calendar, and it overwrites anything set here.
  //
  // A real program gets that calendar from `PollKind::CALENDAR`, which is on by default. This example
  // has no broker, so its source answers no calendar; the date is therefore uncovered, the phase is
  // `UNKNOWN`, and the report says so in as many words. Seeding one here is what a run with a broker
  // gets from its first calendar poll.
  algotradelib::TradingCalendar calendar;
  calendar.country_ = algotradelib::Country::KR;
  calendar.received_at_ = start_time();
  calendar.days_.push_back(algotradelib::TradingDay{
    strategy::SessionWindow::krx().local_date(start_time()), true});
  live_runner.context().set_calendar(calendar);

  ScriptedSource source(4);

  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::minutes(1);
  schedule.tick_ = std::chrono::minutes(1);
  // No broker here, so nothing could answer a calendar request; a real program leaves this at its
  // six-hour default.
  schedule.calendar_.reset();
  // **The band, on a cadence rather than once.** Nothing in this library used to refresh one, so an
  // operator who registered a band at startup was clamping every limit order to it for as long as
  // the process ran. Ten minutes here is arbitrary; the real constraint is that it be shorter than
  // the session, because the value changes with the previous close.
  schedule.price_limits_ = std::chrono::minutes(10);

  runner::PollingLoop<LoopRunner, ScriptedSource, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{start_time()});
  // **Two statements every placement-enabled run now owes, and this source cannot satisfy either.**
  //
  // A run states how old a holdings snapshot may be before it stops sizing against it, and a run
  // that has never read one holds rather than treating an empty account as a current one -- an
  // empty portfolio is the *permissive* answer to every ceiling and to every target subtraction.
  // Neither was required before, and "not required" meant a run that never thought about holdings
  // traded as though it held nothing.
  //
  // This example has no broker, so it reads no holdings ever. Saying so explicitly is what an
  // operator in the same position does; leaving them out would hold this loop for the whole run.
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
  // **The third statement, and the same shape as the first.** A band of unbounded age used to be
  // applied forever; now a run states how old one may be, or states that it accepts any age. Twenty
  // minutes against a ten-minute cadence, so one failed poll is tolerated and a run of failures is
  // not -- which is what the policy is actually bounding, since a failed poll deliberately leaves
  // the last good band in place.
  loop.set_maximum_price_limits_age(std::chrono::minutes(20));

  runner::StopSource stop;
  const auto report = loop.run(stop, 6);

  describe(report);

  std::cout << "\nthe strategy saw " << live_runner.strategy().bars_ << " bar(s)\n";

  // The gate refused the first iteration because the order list had not been read. That is the
  // cheapest half of reconciliation and it needs no persistence: a process that has just started
  // cannot know whether a predecessor died mid-POST, and an empty ledger does not mean nothing is
  // owed -- it means nothing is remembered.
  std::cout << "the order list was read " << source.reads() << " time(s) before the gate opened\n";

  std::cout << "\nhow a real operator would stop this:\n"
            << "  runner::SignalStop::install(stop);   // SIGINT and SIGTERM quiesce; SIGHUP ignored\n"
            << "  loop.set_control_file(runner::FileStopChannel(\"/var/run/atl/stop\"));\n"
            << "  echo withdraw > /var/run/atl/stop    // or quiesce, or flatten\n\n"
            << "and the trap worth knowing: a supervisor's grace period must exceed one loop\n"
            << "iteration plus one request timeout, or its SIGKILL turns the cleanest stop into\n"
            << "the worst one -- a process killed mid-request, with an order whose fate is unknown.\n";

  return 0;
}
