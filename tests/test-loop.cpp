#include <algotradelib/runner>
#include <algotradelib/vanilla>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace runner = algotradelib::runner;
namespace strategy = algotradelib::strategy;

using runner::StopIntent;
using runner::StopSource;

namespace {

using namespace std::chrono_literals;

/**
 * Writes `text` to `path`, for driving the control-file channel.
 *
 * The tests write real files rather than mocking the filesystem, because the thing worth testing is
 * the parsing of what an operator's `echo` actually produces -- trailing newline included -- and a
 * mock would be asserting the parser against assumptions instead.
 */
void write_file(const std::string& path, const std::string& text) {
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  REQUIRE(handle != nullptr);
  if (!text.empty()) {
    REQUIRE(std::fwrite(text.data(), 1, text.size(), handle) == text.size());
  }
  std::fclose(handle);
}

/** Returns a path inside the build directory, unique per test. */
std::string temp_path(const char* name) {
  return std::string("atl-test-stop-") + name;
}

/**
 * A strategy that does nothing, for testing the loop rather than a strategy.
 *
 * At namespace scope because a strategy's handlers are template members and C++ forbids those in a
 * class declared inside a function body. `ATL_ASSERT_STRATEGY` catches the mistake, but only after
 * the compiler has produced a confusing error, so the pattern is worth stating: every test strategy
 * in this repository lives out here for that reason.
 */
struct Inert {
  std::size_t bars_{0};

  template <class Ctx>
  void on_bar(Ctx&, const algotradelib::strategy::BarEvent&) {
    ++bars_;
  }
};

using InertRunner = runner::DryRunner<Inert>;
using InertSource = runner::ManualSource<InertRunner>;
using InertLoop = runner::PollingLoop<InertRunner, InertSource, runner::ManualClock>;

/**
 * A plausible wall-clock instant for a loop to start at.
 *
 * Not the epoch, deliberately: a default-constructed `Portfolio` carries the epoch and the loop
 * reads that as "never populated", so a test clock starting there would make a snapshot stamped
 * "now" look like no snapshot at all.
 */
std::chrono::system_clock::time_point base_time() {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 365 * 56);
}

/** A clock starting at `base_time()`. */
runner::ManualClock test_clock() {
  return runner::ManualClock{base_time()};
}

/**
 * A calendar in which the day `test_clock()` starts on either trades or does not.
 *
 * The loop derives the market phase from a configured `SessionWindow` and this, so installing one is
 * how a test says what the market is doing -- and it is why these tests no longer call
 * `context().set_phase()`. That call used to freeze a phase the loop never looked at, so every
 * assertion below about an order being sent or refused was an assertion about a hand-set field
 * rather than about the mechanism. Now it is the mechanism.
 *
 * The date comes from `SessionWindow::local_date`, which `tests/test-strategy.cpp` pins independently
 * against hand-computed dates -- so a bug in it fails there, loudly, rather than only making these
 * fixtures quietly agree with it.
 */
algotradelib::TradingCalendar krx_calendar(bool trades = true) {
  const auto session = strategy::SessionWindow::krx();
  algotradelib::TradingCalendar calendar;
  calendar.country_ = algotradelib::Country::KR;
  calendar.received_at_ = base_time();
  calendar.days_.push_back(algotradelib::TradingDay{session.local_date(base_time()), trades});
  return calendar;
}

/**
 * Installs that calendar where the loop reads it, which is the runner's context.
 *
 * Seeded directly rather than served through the source, because a source's calendar arrives
 * *during* the first iteration's polls and the loop installs the phase *before* them -- so a
 * single-iteration test served by a source would run its one iteration at `UNKNOWN`. That lag is real
 * and has its own tests; it is not what most of these cases are about.
 */
template <typename R>
void serve_calendar(R& runner_instance, bool trades = true) {
  runner_instance.context().set_calendar(krx_calendar(trades));
}

algotradelib::InstrumentId samsung() {
  return algotradelib::InstrumentId{
    algotradelib::Country::KR, algotradelib::Venue::COMBINED, "005930"};
}

/** Reference data the runner needs before it can size or price anything. */
algotradelib::Instrument samsung_reference() {
  algotradelib::Instrument reference;
  reference.id_ = samsung();
  reference.type_ = algotradelib::SecurityType::STOCK;
  reference.currency_ = algotradelib::Currency{"KRW"};
  reference.lot_size_ = algotradelib::Decimal64::parse("1");
  reference.price_rule_ = algotradelib::PriceRule::krx_equity();
  return reference;
}

/**
 * States each position's whole quantity as sellable, as a broker's holdings rows do: the guard checks a
 * sell against the figure by default, a closing sell included.
 */
template <typename Tag>
void state_sellable(strategy::Portfolio<Tag>& portfolio) {
  portfolio.sellable_basis_ = algotradelib::SellableBasis{
    algotradelib::SellableBasis::Coverage::WHOLE_HOLDING,
    algotradelib::SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  // The instrument's whole holding on each of its rows: rows of one instrument are never summed.
  for (auto& position : portfolio.positions_) {
    if (const auto net = portfolio.net_quantity_of(position.instrument_)) {
      position.sellable_quantity_ = *net;
    }
  }
}

/**
 * A portfolio holding `held` shares with `resting` shares working, stamped now.
 *
 * Built at namespace scope so the withdraw and flatten tests read as assertions about the loop
 * rather than about fixture construction.
 */
strategy::Portfolio<InertRunner::Tag> portfolio_with(const char* held, const char* resting) {
  using Tag = InertRunner::Tag;
  strategy::Portfolio<Tag> portfolio;
  portfolio.as_of_ = base_time();

  if (held != nullptr) {
    algotradelib::HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = algotradelib::Decimal64::parse(held);
    position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
    portfolio.positions_.push_back(position);
  }
  if (resting != nullptr) {
    algotradelib::WorkingOrder<Tag> working;
    working.order_.instrument_ = samsung();
    working.order_.side_ = algotradelib::OrderSide::BUY;
    working.order_.status_ = algotradelib::OrderStatus::PENDING;
    working.order_.quantity_ = algotradelib::Decimal64::parse(resting);
    working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
    working.ref_ = algotradelib::OrderRef<Tag>{"0001", ""};
    portfolio.working_.push_back(working);
  }
  state_sellable(portfolio);
  return portfolio;
}

/**
 * Polls bars on every iteration, by matching the tick to the interval.
 *
 * What the gate, failure and stop tests want, because they are about what the loop does with an
 * outcome rather than about when the next poll is due.
 */
runner::PollSchedule bars_each_iteration() {
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  return schedule;
}

/** A daily bar at a plausible instant, one per index. */
strategy::BarEvent bar_at(std::size_t index) {
  algotradelib::Bar bar;
  bar.timestamp_ = base_time() + std::chrono::hours(24 * static_cast<int>(index));
  bar.high_ = algotradelib::Decimal64::parse("70000");
  bar.low_ = bar.high_;
  bar.close_ = bar.high_;
  bar.volume_ = algotradelib::Decimal64::parse("1000000");
  return strategy::make_bar_event(
    samsung(), algotradelib::BarInterval::ONE_DAY, algotradelib::BarStamp::CLOSE, bar);
}

/**
 * A daily bar whose instants are one second apart, for a test about which clock ages a value.
 *
 * `bar_at` advances a day per index, which is what most of these tests want; a staleness test
 * measured against a policy of hours cannot use it, because the context's own instant would then
 * outrun the policy on its own and the test would pass whichever clock the rule read.
 */
strategy::BarEvent bar_seconds_apart(std::size_t index) {
  algotradelib::Bar bar;
  bar.timestamp_ = base_time() + std::chrono::seconds(static_cast<int>(index));
  bar.high_ = algotradelib::Decimal64::parse("70000");
  bar.low_ = bar.high_;
  bar.close_ = bar.high_;
  bar.volume_ = algotradelib::Decimal64::parse("1000000");
  return strategy::make_bar_event(
    samsung(), algotradelib::BarInterval::ONE_DAY, algotradelib::BarStamp::CLOSE, bar);
}

/**
 * A strategy that wants ten shares of whatever bar it is shown.
 *
 * `Inert` cannot expose an ordering bug in the loop, because a strategy that never submits cannot
 * send early. This one can, which is the whole point of it.
 */
struct HoldTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = algotradelib::Decimal64::parse("10");
    ctx.submit(target);
  }
};

using SendingRunner = runner::DryRunner<HoldTen>;

/** Wants ten shares at a stated limit, so the order carries a price a band can move. */
struct LimitTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = strategy::Price::checked(
      algotradelib::Decimal64::parse("80000"), reference->price_rule_
    );
    if (!limit) {
      return;
    }
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = algotradelib::Decimal64::parse("10");
    target.limit_ = limit;
    ctx.submit(target);
  }
};

using LimitRunner = runner::DryRunner<LimitTen>;

/**
 * A source that feeds the runner from inside `poll`, the way a real one does.
 *
 * `live::SessionSource::poll_bars` calls `Runner::feed` per instrument, and `feed` settles, so an
 * order can leave the process from inside a poll. `ManualSource` only reports an outcome, so no
 * test built on it can observe what the loop had established *before* that call.
 */
template <typename RunnerT>
class FeedingSourceFor {
public:
  /** It stands for a real working-order listing, as both supported brokers publish one. */
  static constexpr bool provides_working_orders = true;

  /** Sets what the order-list read returns. */
  void set_order_list(runner::PollOutcome outcome) { order_list_ = outcome; }

  /** Returns how many times the order list was read. */
  std::size_t order_list_reads() const noexcept { return order_list_reads_; }

  /**
   * Serves the calendar a test scripted, so a calendar poll can *change* the answer mid-iteration.
   *
   * A source can both replace the calendar and settle an order in one iteration, and the calendar is polled first, so a revision arriving now must reach the phase
   * the bar poll after it is checked against.
   */
  void set_calendar(algotradelib::TradingCalendar calendar) { calendar_ = std::move(calendar); }

  /**
   * Scripts a calendar installed from inside the **bars** poll, before the bar is fed.
   *
   * The one sequence the loop cannot make safe, because nothing of its runs between a source's
   * `set_calendar` and its own `Runner::feed`. `PollKind::CALENDAR` states the contract that forbids it
   * and the loop reports a source that breaks it; this is how that report gets tested.
   */
  void set_calendar_before_feeding(algotradelib::TradingCalendar calendar) {
    calendar_before_feeding_ = std::move(calendar);
  }

  /**
   * Scripts the band a price-limits poll installs, the way `live::SessionSource` installs one.
   *
   * Installed from inside that poll and nowhere else, so a test asserting that a band read in an
   * iteration governs the order that iteration's bar produces is asserting about the poll order
   * rather than about a value some fixture set beforehand.
   */
  void set_price_limits(algotradelib::PriceLimits limits) { price_limits_ = std::move(limits); }

  /**
   * Feeds bars a second apart instead of a day apart, so the loop's clock is what moves.
   *
   * Without this a staleness test cannot tell which clock is ageing the value: the bar fixture
   * advances a day per poll while a `ManualClock` advances by the tick, so a rule reading either one
   * passes, as the band-ageing test below once did.
   *
   * A second rather than nothing at all, because the context's `SeriesBuffer` refuses an observation
   * that is not later than the last -- so the instant has to move, and what matters is that it moves
   * by far less than the policy under test.
   */
  void creep_bar_time() { creep_bar_time_ = true; }

  /** Returns the price of the last order the runner would have sent from inside a poll. */
  const std::optional<algotradelib::Decimal64>& last_planned_price() const noexcept {
    return last_planned_price_;
  }

  /** Feeds one bar per bars poll, and records what the runner decided while doing it. */
  runner::PollOutcome poll(runner::PollKind kind, RunnerT& runner_instance) {
    if (kind == runner::PollKind::CALENDAR) {
      if (calendar_in_calendar_poll_) {
        runner_instance.context().set_calendar(*calendar_in_calendar_poll_);
        // The forbidden second half: feeding from the calendar poll, after revising the calendar.
        const auto report = runner_instance.feed(bar_at(++bars_));
        placed_ += report.would_place_.size();
        refused_ += report.refused_.size();
        if (!report.refused_.empty()) {
          last_guard_reason_ = report.refused_.front().reason_;
        }
        return runner::PollOutcome::UPDATED;
      }
      if (!calendar_) {
        return runner::PollOutcome::UNCHANGED;
      }
      runner_instance.context().set_calendar(*calendar_);
      return runner::PollOutcome::UPDATED;
    }
    if (kind == runner::PollKind::PRICE_LIMITS) {
      if (!price_limits_) {
        return runner::PollOutcome::UNCHANGED;
      }
      runner_instance.set_price_limits(samsung(), *price_limits_);
      return runner::PollOutcome::UPDATED;
    }
    if (kind != runner::PollKind::BARS) {
      return runner::PollOutcome::UNCHANGED;
    }
    if (calendar_before_feeding_) {
      runner_instance.context().set_calendar(*calendar_before_feeding_);
    }
    ++bars_;
    const auto report = runner_instance.feed(
      creep_bar_time_ ? bar_seconds_apart(bars_) : bar_at(bars_)
    );
    placed_ += report.would_place_.size();
    if (!report.would_place_.empty()) {
      last_planned_price_ = report.would_place_.back().price_;
    }
    refused_ += report.refused_.size();
    canceled_ += report.would_cancel_.size();
    withheld_plans_ += report.withheld_plans_.size();
    if (!report.withheld_plans_.empty()) {
      last_withheld_ = report.withheld_plans_.front().reason_;
    }
    if (!report.refused_.empty()) {
      last_refusal_ = report.refused_.front().halt_reason_;
      last_guard_reason_ = report.refused_.front().reason_;
    }
    return runner::PollOutcome::UPDATED;
  }

  /** Returns how many cancellations actually reached the executor from inside a poll. */
  std::size_t canceled() const noexcept { return canceled_; }

  /** Returns how many whole plans were held back before any part of them was sent. */
  std::size_t withheld_plans() const noexcept { return withheld_plans_; }

  /** Returns why the first such plan was held back. */
  std::optional<runner::WithheldPlanReason> last_withheld() const noexcept { return last_withheld_; }

  /**
   * Returns the configured order-list outcome, installing a calendar first when asked to.
   *
   * `gate_for` performs this read out of band, before any poll, so a source that learns the calendar
   * here is the one path the per-poll install would miss if the install ran *after* each poll instead
   * of before it. Both real sources read orders here and nothing else, but the loop cannot depend on
   * that: the source is a template parameter.
   */
  runner::PollOutcome read_order_list(RunnerT& runner_instance) {
    ++order_list_reads_;
    if (calendar_on_order_list_) {
      runner_instance.context().set_calendar(*calendar_on_order_list_);
    }
    return order_list_;
  }

  /** Scripts a calendar that arrives with the order-list read rather than with a calendar poll. */
  void set_calendar_on_order_list(algotradelib::TradingCalendar calendar) {
    calendar_on_order_list_ = std::move(calendar);
  }

  /**
   * Scripts a calendar installed during the **calendar** poll, which then feeds a bar.
   *
   * A source obeying the letter of the first half of `PollKind::CALENDAR`'s contract -- it writes the
   * calendar in the calendar poll and nowhere else -- and breaking the second, by feeding from that
   * poll. That sends under the replaced phase, and excluding the calendar poll from the check hid it.
   */
  void set_calendar_and_feed_in_calendar_poll(algotradelib::TradingCalendar calendar) {
    calendar_in_calendar_poll_ = std::move(calendar);
  }

  /** Returns how many orders the runner would have sent from inside a poll. */
  std::size_t placed() const noexcept { return placed_; }

  /** Returns how many the guard refused there. */
  std::size_t refused() const noexcept { return refused_; }

  /** Returns which hold refused the last one. */
  std::optional<strategy::HaltReason> last_refusal() const noexcept { return last_refusal_; }

  /**
   * Returns which limit refused the last one.
   *
   * Distinct from the hold above, and the distinction is the point: `TRADING_HALTED` says a gate was
   * shut, `PHASE_NOT_PERMITTED` says the market was. A test that only looked at the hold could not
   * tell an order refused for being out of hours from one refused by a stale portfolio.
   */
  std::optional<strategy::GuardReason> last_guard_reason() const noexcept {
    return last_guard_reason_;
  }

private:
  runner::PollOutcome order_list_ = runner::PollOutcome::UPDATED;
  std::size_t order_list_reads_{0};
  std::size_t bars_{0};
  std::size_t placed_{0};
  std::size_t refused_{0};
  std::optional<strategy::HaltReason> last_refusal_;
  std::size_t canceled_{0};
  std::size_t withheld_plans_{0};
  std::optional<runner::WithheldPlanReason> last_withheld_;
  std::optional<strategy::GuardReason> last_guard_reason_;
  std::optional<algotradelib::TradingCalendar> calendar_;
  std::optional<algotradelib::TradingCalendar> calendar_on_order_list_;
  std::optional<algotradelib::TradingCalendar> calendar_before_feeding_;
  std::optional<algotradelib::TradingCalendar> calendar_in_calendar_poll_;
  std::optional<algotradelib::PriceLimits> price_limits_;
  std::optional<algotradelib::Decimal64> last_planned_price_;
  bool creep_bar_time_{false};
};

using FeedingSource = FeedingSourceFor<SendingRunner>;
using SendingLoop = runner::PollingLoop<SendingRunner, FeedingSource, runner::ManualClock>;
using LimitSource = FeedingSourceFor<LimitRunner>;
using LimitLoop = runner::PollingLoop<LimitRunner, LimitSource, runner::ManualClock>;

/** A strategy that wants out of whatever it is shown, so a bar produces a sell. */
struct SellEverything {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = algotradelib::Decimal64{};
    ctx.submit(target);
  }

  template <class Ctx>
  void on_stop(Ctx&) {
    ++stops_;
  }

  std::size_t stops_{0};
};

using SellingRunner = runner::DryRunner<SellEverything>;

/** A strategy whose shutdown handler tries to close the position itself. */
struct ClosesOnStop {
  template <class Ctx>
  void on_stop(Ctx& ctx) {
    strategy::TargetPosition target;
    target.instrument_ = samsung();
    target.target_quantity_ = algotradelib::Decimal64{};
    target.tag_ = "strategy-stop";
    ctx.submit(target);
  }
};

using ClosingRunner = runner::DryRunner<ClosesOnStop>;
using ClosingSource = runner::ManualSource<ClosingRunner>;
using ClosingLoop = runner::PollingLoop<ClosingRunner, ClosingSource, runner::ManualClock>;

/** A portfolio holding ten shares, stamped now, with nothing working. */
strategy::Portfolio<runner::DryTag> ten_held() {
  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = base_time();
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("10");
  position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
  portfolio.positions_.push_back(position);
  return portfolio;
}
using SellingSource = FeedingSourceFor<SellingRunner>;
using SellingLoop = runner::PollingLoop<SellingRunner, SellingSource, runner::ManualClock>;

/**
 * An executor whose cancellations always come back with `Outcome`.
 *
 * `RecordingExecutor` always accepts, so nothing built on it can show what a closing pass does with
 * a cancellation the broker did not withdraw.
 */
template <runner::SendOutcome Outcome>
class CancelFailingExecutor {
public:

  /** A double reaches no broker, so it is exempt from having to have read the account. */
  static constexpr bool reaches_a_broker = false;
  runner::SendOutcome place(const strategy::PlannedOrder&) {
    return runner::SendOutcome::ACCEPTED;
  }

  runner::SendOutcome cancel(const algotradelib::WorkingOrder<runner::DryTag>&) {
    ++cancels_;
    return Outcome;
  }

  /** Returns how many cancellations reached it. */
  std::size_t cancels() const noexcept { return cancels_; }

private:
  std::size_t cancels_{0};
};

using LosingExecutor = CancelFailingExecutor<runner::SendOutcome::INDETERMINATE>;
using LosingRunner = runner::Runner<Inert, strategy::NoCaps, runner::DryTag, LosingExecutor>;
using LosingSource = runner::ManualSource<LosingRunner>;
using LosingLoop = runner::PollingLoop<LosingRunner, LosingSource, runner::ManualClock>;

using RejectingExecutor = CancelFailingExecutor<runner::SendOutcome::REJECTED>;
using RejectingRunner = runner::Runner<Inert, strategy::NoCaps, runner::DryTag, RejectingExecutor>;
using RejectingSource = runner::ManualSource<RejectingRunner>;
using RejectingLoop = runner::PollingLoop<RejectingRunner, RejectingSource, runner::ManualClock>;

/**
 * States a portfolio policy on a loop whose test is about something else.
 *
 * Two things are mandatory that were once optional: a run states how old a holdings snapshot
 * may be, and a run that has never read one holds rather than sizing against an empty account. Most
 * of the cases in this file predate that and are about the calendar, the phase, a stop, a cadence or
 * buying power. Most construct no portfolio because they never needed one; a few do construct one and
 * simply have no opinion about its age. Stating both opt-outs is how such a case says "not about the
 * portfolio" in one line, and it is exactly the pair an operator would state to keep the previous
 * placement behaviour.
 *
 * The cases that *are* about the portfolio gate do not call this, and assert the holds instead.
 */
template <typename Loop>
void not_about_the_portfolio(Loop& loop) {
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
}

/** A portfolio holding one resting order, for a closing pass to try to withdraw. */
template <typename Portfolio>
void give_one_resting_order(Portfolio& portfolio) {
  portfolio.as_of_ = base_time();
  algotradelib::WorkingOrder<runner::DryTag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = algotradelib::OrderSide::BUY;
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("5");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<runner::DryTag>{"0001", ""};
  portfolio.working_.push_back(working);
}

} // namespace

TEST_CASE("a steady clock returns immediately for a non-positive wait", "[loop][clock]") {
  // The only property of the real clock worth asserting, and the one a loop that has fallen behind
  // its cadence depends on: an overdue poll must not pause. Asserting anything about a *positive*
  // wait would be asserting the scheduler.
  //
  // **The bound is a coarse stall check, and it is weaker than its name suggests.** The same
  // objection applies to a tight ceiling here as to a positive wait: a busy host would fail it for a
  // reason that has nothing to do with this clock. `SteadyClock` reads the real clock by
  // construction, so elapsed time is the only observable there is -- `runner::ManualClock` is what
  // every other timing test uses, and it cannot answer this one.
  //
  // What it does **not** establish is that the wait returns *immediately*: an implementation that
  // slept for a second would pass, and forwarding a zero or negative duration to `sleep_for` does
  // not itself produce a positive wait, so the most likely regression is invisible here anyway.
  // There is no uniquely correct finite ceiling on a loaded host, and no finite value distinguishes
  // a stalled machine from a slow one with certainty. Five seconds is a judgement about where the
  // two stop overlapping in practice, and the assertion claims nothing beyond that.
  //
  // It stays a `CHECK` because it was one; the case still fails when it trips. There is no later
  // assertion here for that to preserve.
  const runner::SteadyClock clock{};
  const auto before = std::chrono::steady_clock::now();
  clock.wait_for(std::chrono::nanoseconds(-1));
  clock.wait_for(std::chrono::nanoseconds::zero());
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < 5s);
}

TEST_CASE("stop intents are ordered by how much they do", "[loop][stop]") {
  // stop_intent_covers depends on the enumerator order, so the order is asserted rather than
  // trusted: inserting a new intent in the wrong place would silently break escalation.
  CHECK(runner::stop_intent_covers(StopIntent::FLATTEN, StopIntent::QUIESCE));
  CHECK(runner::stop_intent_covers(StopIntent::FLATTEN, StopIntent::WITHDRAW));
  CHECK(runner::stop_intent_covers(StopIntent::WITHDRAW, StopIntent::QUIESCE));
  CHECK(runner::stop_intent_covers(StopIntent::QUIESCE, StopIntent::QUIESCE));
  CHECK_FALSE(runner::stop_intent_covers(StopIntent::QUIESCE, StopIntent::WITHDRAW));
  CHECK_FALSE(runner::stop_intent_covers(StopIntent::WITHDRAW, StopIntent::FLATTEN));
}

TEST_CASE("intent names round-trip through the control-file spelling", "[loop][stop]") {
  for (const auto intent : {StopIntent::QUIESCE, StopIntent::WITHDRAW, StopIntent::FLATTEN}) {
    CHECK(runner::stop_intent_from(runner::stop_intent_name(intent)) == intent);
  }
  CHECK_FALSE(runner::stop_intent_from("").has_value());
  CHECK_FALSE(runner::stop_intent_from("halt").has_value());
  CHECK_FALSE(runner::stop_intent_from("QUIESCE").has_value());
}

TEST_CASE("a control file names an intent as an operator would write it", "[loop][stop]") {
  const auto path = temp_path("intent");
  runner::FileStopChannel channel(path);

  SECTION("a bare word") {
    write_file(path, "withdraw");
    CHECK(channel.poll() == StopIntent::WITHDRAW);
    CHECK(channel.unrecognized().empty());
  }

  SECTION("with the trailing newline echo adds") {
    write_file(path, "flatten\n");
    CHECK(channel.poll() == StopIntent::FLATTEN);
  }

  SECTION("with leading whitespace") {
    write_file(path, "  \t quiesce \n");
    CHECK(channel.poll() == StopIntent::QUIESCE);
  }

  SECTION("only the first token is read") {
    write_file(path, "quiesce and then flatten\n");
    CHECK(channel.poll() == StopIntent::QUIESCE);
  }

  SECTION("an empty file names nothing and is not a typo") {
    write_file(path, "");
    CHECK_FALSE(channel.poll().has_value());
    CHECK(channel.unrecognized().empty());
  }

  SECTION("whitespace alone names nothing and is not a typo") {
    write_file(path, "\n\n  \n");
    CHECK_FALSE(channel.poll().has_value());
    CHECK(channel.unrecognized().empty());
  }

  SECTION("a typo is reported rather than guessed at") {
    // The important half. Picking the nearest intent on an operator's behalf would choose how much
    // of their position to close for them.
    write_file(path, "withdaw\n");
    CHECK_FALSE(channel.poll().has_value());
    CHECK(channel.unrecognized() == "withdaw");
  }

  SECTION("a recognized read clears a previous typo") {
    write_file(path, "nonsense\n");
    REQUIRE(channel.unrecognized().empty());
    REQUIRE_FALSE(channel.poll().has_value());
    REQUIRE(channel.unrecognized() == "nonsense");
    write_file(path, "quiesce\n");
    CHECK(channel.poll() == StopIntent::QUIESCE);
    CHECK(channel.unrecognized().empty());
  }

  std::remove(path.c_str());
}

TEST_CASE("installing the signal stop is explicit and reversible", "[loop][stop][signal]") {
  // No signal is raised here. The point of keeping StopSource signal-free is that the delivery
  // mechanism can be verified separately from the behaviour, and the behaviour is what the loop
  // depends on. Raising a real SIGTERM inside a test process risks killing the test runner if
  // installation ever regresses, which is a poor trade for what it would prove.
  REQUIRE(runner::SignalStop::installed() == nullptr);

  StopSource source;
  runner::SignalStop::install(source);
  CHECK(runner::SignalStop::installed() == &source);

  runner::SignalStop::uninstall();
  CHECK(runner::SignalStop::installed() == nullptr);
  // Uninstalling must not manufacture a stop, or a program handing control back to its host would
  // quiesce itself on the way.
  CHECK_FALSE(source.requested());
}

TEST_CASE("an unread buying power blocks sending, and only when asked for", "[loop][gate]") {
  // The hold exists because sizing a buy against an unknown budget is exactly what the check was
  // added to stop. It is taken only when the caller asked for the check: a loop with no
  // buying-power limit configured is an ordinary loop and must not be held for a reading nobody
  // wants.
  strategy::RiskLimits limits;
  limits.enforce_buying_power_ = true;

  SECTION("a loop with no reading holds") {
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    const auto report = loop.run(stop, 2);
    CHECK(report.final_gate_ == strategy::HaltReason::BUYING_POWER_UNREAD);
    CHECK(report.blocked_ == 2);
    CHECK(runner_instance.guard().halted(strategy::HaltReason::BUYING_POWER_UNREAD));
  }

  SECTION("a reading opens the gate") {
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    strategy::Portfolio<InertRunner::Tag> portfolio;
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("5000000"), algotradelib::Currency::krw()
        },
        base_time(),
      }
    );
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(!report.final_gate_.has_value());
    CHECK_FALSE(runner_instance.guard().halted());
  }

  SECTION("a reading older than the maximum closes it again") {
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    loop.set_maximum_buying_power_age(std::chrono::minutes(5));
    StopSource stop;

    strategy::Portfolio<InertRunner::Tag> portfolio;
    // The figure is aged against its own stamp, not against when a poll last succeeded.
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("5000000"), algotradelib::Currency::krw()
        },
        base_time() - std::chrono::minutes(10),
      }
    );
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(report.final_gate_ == strategy::HaltReason::BUYING_POWER_UNREAD);
    CHECK(runner_instance.guard().halted(strategy::HaltReason::BUYING_POWER_UNREAD));
  }

  SECTION("one currency answering does not stand in for another the source was told to ask") {
    // The gate cannot settle for "some reading exists" when the source knows it was configured for
    // two: an account trading both would report healthy polls while a budget it was told to watch
    // had never arrived, and the run would look fine from the outside.
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    source.set_buying_power_currencies(
      {algotradelib::Currency::krw(), algotradelib::Currency::usd()}
    );
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    strategy::Portfolio<InertRunner::Tag> portfolio;
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("5000000"), algotradelib::Currency::krw()
        },
        base_time(),
      }
    );
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(report.final_gate_ == strategy::HaltReason::BUYING_POWER_UNREAD);
    CHECK(runner_instance.guard().halted(strategy::HaltReason::BUYING_POWER_UNREAD));
  }

  SECTION("every configured currency answering opens it") {
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    source.set_buying_power_currencies(
      {algotradelib::Currency::krw(), algotradelib::Currency::usd()}
    );
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    strategy::Portfolio<InertRunner::Tag> portfolio;
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("5000000"), algotradelib::Currency::krw()
        },
        base_time(),
      }
    );
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("4000"), algotradelib::Currency::usd()
        },
        base_time(),
      }
    );
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(!report.final_gate_.has_value());
    CHECK_FALSE(runner_instance.guard().halted());
  }

  SECTION("a configured currency whose reading went stale closes it, though another is fresh") {
    // The case the failed poll leaves behind: USD keeps failing, KRW keeps answering, and the old
    // USD figure is deliberately kept so the guard has something to refuse against. What must not
    // happen is the gate staying open on the strength of the currency that did answer.
    InertRunner runner_instance(Inert{}, limits);
    InertSource source;
    source.set_buying_power_currencies(
      {algotradelib::Currency::krw(), algotradelib::Currency::usd()}
    );
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    loop.set_maximum_buying_power_age(std::chrono::minutes(5));
    StopSource stop;

    strategy::Portfolio<InertRunner::Tag> portfolio;
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("5000000"), algotradelib::Currency::krw()
        },
        base_time(),
      }
    );
    portfolio.buying_power_.push_back(
      algotradelib::BuyingPower{
        algotradelib::Money{
          algotradelib::Decimal64::parse("4000"), algotradelib::Currency::usd()
        },
        base_time() - std::chrono::minutes(10),
      }
    );
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(report.final_gate_ == strategy::HaltReason::BUYING_POWER_UNREAD);
    CHECK(runner_instance.guard().halted(strategy::HaltReason::BUYING_POWER_UNREAD));
  }

  SECTION("a loop that never asked for the check is not held") {
    InertRunner runner_instance;  // default limits: no buying-power check
    InertSource source;
    InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    const auto report = loop.run(stop, 2);
    CHECK(!report.final_gate_.has_value());
    CHECK_FALSE(runner_instance.guard().halted());
  }
}

TEST_CASE("the buying-power poll has its own cadence", "[loop][cadence]") {
  InertRunner runner_instance;
  InertSource source;
  runner::PollSchedule schedule;
  schedule.buying_power_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  InertLoop loop(runner_instance, source, schedule, test_clock());
  StopSource stop;

  loop.run(stop, 3);
  CHECK(source.polls(runner::PollKind::BUYING_POWER) == 3);
  // And a kind with no interval configured is never polled, which is what keeps an existing
  // program's request count unchanged on upgrade -- **with one exception, since `PollKind::CALENDAR`
  // exists.** That cadence is on by default, because the phase the loop derives from it decides
  // whether anything may be sent at all, so an upgrade does add calendar traffic. It is asserted here
  // rather than left implied, because the sentence above used to be true of every kind.
  CHECK(source.polls(runner::PollKind::HOLDINGS) == 0);
  CHECK(source.polls(runner::PollKind::CALENDAR) == 1);
}

TEST_CASE("a run reports what it learned about the broker's sellable figure", "[loop][probe]") {
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  runner_instance.probe_sellable_convention();
  // Served so the run has an established phase, because this test is about the probe's note and a
  // run that never learns the phase adds a note of its own.
  serve_calendar(runner_instance);

  // A quiet run says nothing rather than reporting "undetermined" on every iteration that never
  // sold anything: a note nobody needs is how a report stops being read.
  const auto quiet = loop.run(stop, 1);
  CHECK(quiet.notes_.empty());

  // Now the observation the probe exists for: a sell starts resting and the figure drops with it.
  strategy::Portfolio<InertRunner::Tag> before;
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("100");
  position.sellable_quantity_ = algotradelib::Decimal64::parse("100");
  before.positions_.push_back(position);
  // Stated, because the probe reads the held-to-sellable distance and that only means something when
  // both numbers cover the same shares. A fixture for a broker that published the figure says so.
  before.sellable_basis_ = algotradelib::SellableBasis{
    algotradelib::SellableBasis::Coverage::WHOLE_HOLDING,
    algotradelib::SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  runner_instance.context().set_portfolio(before);
  loop.run(stop, 1);

  auto after = before;
  after.positions_[0].sellable_quantity_ = algotradelib::Decimal64::parse("70");
  algotradelib::WorkingOrder<InertRunner::Tag> resting;
  resting.order_.instrument_ = samsung();
  resting.order_.side_ = algotradelib::OrderSide::SELL;
  resting.order_.status_ = algotradelib::OrderStatus::PENDING;
  resting.order_.quantity_ = algotradelib::Decimal64::parse("30");
  resting.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  after.working_.push_back(resting);
  runner_instance.context().set_portfolio(after);

  const auto learned = loop.run(stop, 1);
  REQUIRE(learned.notes_.size() == 1);
  CHECK(learned.notes_[0].find("net_of_resting") != std::string::npos);
  // The counts travel with the verdict, because one observation is evidence rather than proof.
  CHECK(learned.notes_[0].find("1 net") != std::string::npos);
}

TEST_CASE("an intermittent failure does not accumulate to a shutdown", "[loop][failure]") {
  // Consecutive, not cumulative: a broker that fails one poll in three is degraded, not down, and
  // shutting down on the fifth cumulative failure of a long healthy run would be wrong.
  InertRunner runner_instance;
  InertSource source;
  for (int i = 0; i < 4; ++i) {
    source.queue(runner::PollKind::BARS, runner::PollOutcome::FAILED);
    source.queue(runner::PollKind::BARS, runner::PollOutcome::UPDATED);
  }
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  runner::FailurePolicy policy;
  policy.consecutive_limit_ = 2;
  loop.set_failure_policy(policy);
  StopSource stop;

  const auto report = loop.run(stop, 8);

  CHECK_FALSE(report.abandoned_);
  CHECK(report.failed(runner::PollKind::BARS) == 4);
  CHECK(report.iterations_ == 8);
}

TEST_CASE("an unrecognized control-file token is reported, not guessed at", "[loop][stop]") {
  const auto path = temp_path("loop-typo");
  write_file(path, "withdaw\n");

  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_control_file(runner::FileStopChannel(path));
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK_FALSE(report.stopped_by_.has_value());
  CHECK(report.iterations_ == 2);
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("withdaw") != std::string::npos);
  std::remove(path.c_str());
}

TEST_CASE("a flatten with nothing held sends nothing", "[loop][stop]") {
  // A flatten must be safe to issue when already flat, because an operator under pressure will not
  // check first.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_portfolio(portfolio_with(nullptr, nullptr));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.withdrawn_ == 0);
  CHECK(report.flattened_ == 0);
  CHECK(runner_instance.executor().placed().empty());
  CHECK(runner_instance.executor().canceled().empty());
}

TEST_CASE("a zero-quantity position is not flattened", "[loop][stop]") {
  // A closed position can sit in a snapshot with quantity zero. Sending a sell for none of it would
  // be an order the broker rejects, for no reason.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_portfolio(portfolio_with("0", nullptr));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.flattened_ == 0);
  CHECK(runner_instance.executor().placed().empty());
}

TEST_CASE("a stop that cannot size a close says so instead of reporting completion",
          "[loop][stop]") {
  // A net can be representable while the *magnitude* that would close
  // it is not: two rows summing to the most negative `Decimal64` have no positive counterpart, so
  // the size of the buy that would flatten them cannot be stated. That path used to `continue` in
  // silence, moving no count -- so `closing_complete()` returned true over a position the pass had
  // never sized, which is the one thing that call promises it will not do.
  //
  // It is counted as `closing_unsizable_` and **not** as `closing_contended_`, because contention
  // means a competing order and its note sends an operator to the order list. Here the holdings
  // quantities are what could not be established, and the order list has nothing to say about it.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance, /*trades=*/false);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<InertRunner::Tag> portfolio;
  portfolio.as_of_ = base_time();
  // Sums to the minimum coefficient, which `decimal_add` reaches and `decimal_subtract` cannot
  // negate.
  for (const char* lot : {"-9223372036854775807", "-1"}) {
    algotradelib::HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = algotradelib::Decimal64::parse(lot);
    position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
    portfolio.positions_.push_back(position);
  }
  runner_instance.context().set_portfolio(portfolio);

  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  // Nothing was sent, and the report does not pretend otherwise.
  CHECK(runner_instance.executor().placed().empty());
  CHECK(report.flattened_ == 0);
  CHECK(report.closing_unsizable_ == 1);
  CHECK(report.closing_contended_ == 0);
  CHECK_FALSE(report.closing_complete());
  // And the note points at the holdings rather than at the order list.
  bool explained = false;
  for (const auto& note : report.notes_) {
    if (note.find("could be sized") != std::string::npos &&
        note.find("still held") != std::string::npos &&
        note.find("contended") == std::string::npos) {
      explained = true;
    }
  }
  CHECK(explained);
}

TEST_CASE("a stop counts a working close once for the instrument, not once per lot",
          "[loop][stop]") {
  // The other half of the same defect, and it is an under-close rather than a refusal. The
  // deduction subtracts what is already working on the closing side -- a figure for the whole
  // instrument -- and subtracting it once per row charged it twice. Rows of +60 and +40 with a
  // working SELL of 20 sent 40 and 20, so after every one of those filled, 20 of the position was
  // still held by a pass that reported it had flattened.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance, /*trades=*/false);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<InertRunner::Tag> portfolio;
  portfolio.as_of_ = base_time();
  for (const char* lot : {"60", "40"}) {
    algotradelib::HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = algotradelib::Decimal64::parse(lot);
    position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
    portfolio.positions_.push_back(position);
  }
  algotradelib::WorkingOrder<InertRunner::Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = algotradelib::OrderSide::SELL;
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("20");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<InertRunner::Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  state_sellable(portfolio);
  runner_instance.context().set_portfolio(portfolio);

  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.flattened_ == 1);
  REQUIRE(runner_instance.executor().placed().size() == 1);
  // 100 held, 20 already working on the closing side, so 80 is what is left to send. Sizing per row
  // subtracted the 20 twice and sent 40 + 20 = 60.
  CHECK(runner_instance.executor().placed().front().quantity_ ==
        algotradelib::Decimal64::parse("80"));
}

TEST_CASE("a runner halted before the loop starts sends nothing on the first iteration",
  "[loop][gate]") {
  // The loop polls and feeds *before* it calls `gate_for`, so a failure installed by recovery
  // before the first iteration can reach a scheduled event while the guard is still unhalted. The
  // enforcement therefore has to live in `Runner::settle()` as well, not only in the gate.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.halt_locally("recovery found damage that names no instrument");

  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 1);
  CHECK(report.final_gate_ == strategy::HaltReason::LOCAL_FAILURE);
  CHECK(report.blocked_ == 1);
  // The order list is never read either: a runner that cannot journal is not sent looking for more
  // work to fail at.
  CHECK(source.order_list_reads() == 0);
}

/** A schedule that reads bars and today's band on every iteration. */
runner::PollSchedule bars_and_band_each_iteration() {
  auto schedule = bars_each_iteration();
  schedule.price_limits_ = std::chrono::seconds(1);
  return schedule;
}

/** Arranges a limit-order loop with reference data, a calendar and a last price. */
void arrange_limit_loop(LimitRunner& runner_instance) {
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
}

/** A band whose ceiling is below the strategy's own limit, so applying it moves the price. */
algotradelib::PriceLimits band_at_71k() {
  algotradelib::PriceLimits band;
  band.upper_ = algotradelib::Decimal64::parse("71000");
  return band;
}

TEST_CASE("the policy note describes the runner's policy, not the loop's memory", "[loop][band]") {
  // A loop borrows a mutable runner and does not own its band policy, so a flag remembered here
  // could disagree with the real one in both directions -- and the note is read exactly when an
  // operator is working out why orders went out unclamped.
  SECTION("a maximum stated on the runner before the loop existed is a policy") {
    strategy::RiskLimits any_phase;
    any_phase.permitted_phases_ = std::nullopt;
    LimitRunner runner_instance(LimitTen{}, any_phase);
    LimitSource source;
    arrange_limit_loop(runner_instance);
    source.set_price_limits(band_at_71k());
    // Stated here rather than through the loop, which is an ordinary thing for a caller who
    // configured the runner first. The band goes stale during the run, so it is withheld -- and the
    // note must not then claim nobody stated a policy.
    runner_instance.set_maximum_price_limits_age(std::chrono::hours(6));
    auto schedule = bars_and_band_each_iteration();
    schedule.tick_ = std::chrono::hours(12);
    schedule.bars_ = std::chrono::hours(12);
    schedule.price_limits_ = std::chrono::hours(240);
    source.creep_bar_time();
    LimitLoop loop(runner_instance, source, schedule, test_clock());
    not_about_the_portfolio(loop);
    StopSource stop;

    const auto report = loop.run(stop, 3);

    CHECK(report.price_bands_withheld_ > 0);
    CHECK(report.notes_.empty());
  }

  SECTION("a withdrawal made on the runner brings the note back") {
    LimitRunner runner_instance;
    LimitSource source;
    arrange_limit_loop(runner_instance);
    source.set_price_limits(band_at_71k());
    LimitLoop loop(runner_instance, source, bars_and_band_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    loop.allow_unbounded_price_limits_age();
    // The documented withdrawal, which returns this runner to having stated no policy at all.
    runner_instance.set_maximum_price_limits_age(std::nullopt);
    StopSource stop;

    const auto report = loop.run(stop, 2);

    CHECK(report.price_bands_withheld_ == 2);
    REQUIRE(report.notes_.size() == 1);
    CHECK(
      report.notes_[0] ==
      "no price band age policy: call set_maximum_price_limits_age or "
      "allow_unbounded_price_limits_age"
    );
  }

  SECTION("the loop can express the withdrawal itself") {
    LimitRunner runner_instance;
    LimitSource source;
    arrange_limit_loop(runner_instance);
    source.set_price_limits(band_at_71k());
    LimitLoop loop(runner_instance, source, bars_and_band_each_iteration(), test_clock());
    not_about_the_portfolio(loop);
    loop.allow_unbounded_price_limits_age();
    loop.set_maximum_price_limits_age(std::nullopt);
    StopSource stop;

    const auto report = loop.run(stop, 1);

    CHECK(report.price_bands_withheld_ == 1);
    CHECK(report.notes_.size() == 1);
  }
}

TEST_CASE("a halt an operator set refuses a withdraw's cancellations", "[loop][stop][halt]") {
  // A withdraw reaches this runner from a signal and from a control file, so a cancellation exempt
  // from the halt would mean `echo withdraw > /var/run/atl/stop` could send an instruction the
  // operator's own halt forbade. It is refused, and the report says so rather than reporting a
  // withdraw that withdrew nothing.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_portfolio(portfolio_with("10", "5"));
  serve_calendar(runner_instance);
  runner_instance.guard().halt();
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::WITHDRAW);
  // Asked for, and refused: nothing was sent.
  CHECK(report.withdrawn_ == 1);
  CHECK(report.closing_refused_ == 1);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("the position is still held") != std::string::npos);
  CHECK(runner_instance.guard().halted(strategy::HaltReason::OPERATOR));
}

TEST_CASE("a withdraw that could not be journaled is not a complete stop", "[loop][stop]") {
  // `closing_complete()` is the one call an operator should not have to second-guess, and a local
  // failure used to leave every closing count at zero: the cancellation was correctly withheld on
  // the runner's report, and nothing carried it onto the loop's. `withdrawn_` said one, every
  // failure count said zero, and the stop reported itself complete having sent nothing.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_portfolio(portfolio_with("10", "5"));
  serve_calendar(runner_instance);
  runner_instance.halt_locally("the order journal could not be written");
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  CHECK(report.closing_unavailable_ == 1);
  // Not a guard refusal: no broker was asked, and the two ask for opposite responses.
  CHECK(report.closing_refused_ == 0);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("local machinery could not send") != std::string::npos);
}

TEST_CASE("a cancellation lost in transit is not a completed withdrawal", "[loop][stop]") {
  // The other way a stop can look finished without being finished. The order may now be withdrawn
  // or may still be resting, so the count that says "this went out" must not include it.
  LosingRunner runner_instance;
  LosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);

  strategy::Portfolio<runner::DryTag> portfolio;
  give_one_resting_order(portfolio);
  runner_instance.context().set_portfolio(portfolio);

  LosingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(runner_instance.executor().cancels() == 1);
  CHECK(report.withdrawn_ == 1);
  CHECK(report.closing_indeterminate_ == 1);
  CHECK_FALSE(report.closing_complete());
  // And the instrument is owed a reconciliation, which is the actual obligation left behind.
  CHECK(runner_instance.ledger().requires_reconcile(samsung()));
}

TEST_CASE("a cancellation the broker rejected is not a completed withdrawal", "[loop][stop]") {
  // The third outcome to fall through to `would_cancel_`, and the reason `send_cancel` now switches
  // over the whole enumeration instead of testing one case at a time. A rejection is an answer about
  // the request -- it did not go -- so a stop that got one has not done what it was asked, whatever
  // the order's own state turns out to be.
  RejectingRunner runner_instance;
  RejectingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);

  strategy::Portfolio<runner::DryTag> portfolio;
  give_one_resting_order(portfolio);
  runner_instance.context().set_portfolio(portfolio);

  RejectingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(runner_instance.executor().cancels() == 1);
  CHECK(report.withdrawn_ == 1);
  CHECK(report.closing_rejected_ == 1);
  CHECK_FALSE(report.closing_complete());
  // A rejection is an answer, so unlike an indeterminate cancellation it owes no reconciliation.
  CHECK_FALSE(runner_instance.ledger().requires_reconcile(samsung()));
  REQUIRE_FALSE(report.notes_.empty());
  // The note points at the broker's listing rather than naming who decided, because the bucket
  // also holds a withdrawal the binding itself refused and the consequence is the same.
  CHECK(report.notes_.front().find("read the broker's order list") != std::string::npos);
}

TEST_CASE("a withdraw pending before the first iteration reads the order list first", "[loop][stop]") {
  // A process started with `withdraw` in its control file. `gate_for` used to report `STOPPING` before
  // the startup read, so the closing pass was built from a picture that could not contain an order a
  // *predecessor* left resting, and the stop could never call itself complete. It now makes the read
  // the startup gate would have made, and then stops.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_portfolio(portfolio_with(nullptr, "5"));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::WITHDRAW);
  CHECK(source.order_list_reads() == 1);
  CHECK(report.order_list_read_);
  CHECK_FALSE(report.closing_source_unverified_);
  CHECK(report.withdrawn_ == 1);
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK(report.closing_complete());
}

TEST_CASE("a flatten pending before the first iteration closes the position", "[loop][stop]") {
  // The case that had no way out: a flatten in the control file at start closed nothing at all, so an
  // operator could not finish a flatten that a resting stop had contended. Now it reads the list, and
  // with nothing resting on the closing side it closes what is held.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", nullptr));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::FLATTEN);
  CHECK(report.order_list_read_);
  CHECK_FALSE(report.closing_source_unverified_);
  CHECK(report.closing_contended_ == 0);
  CHECK(report.flattened_ == 1);
  CHECK(report.closing_complete());
  REQUIRE(runner_instance.executor().placed().size() == 1);
  const auto& closing = runner_instance.executor().placed().front();
  CHECK(closing.side_ == algotradelib::OrderSide::SELL);
  CHECK(closing.quantity_ == algotradelib::Decimal64::parse("10"));
}

TEST_CASE("a stop whose first order-list read fails is not a complete stop", "[loop][stop]") {
  // The read is attempted, not assumed. When it fails the picture cannot contain an order a
  // predecessor left resting, so the pass closes nothing it could double, and says why. And the gate
  // is `STOPPING`, never `ORDER_LIST_UNREAD`: that hold lets reducing orders through, and once a stop
  // is pending nothing but the stop's own instructions may go out.
  InertRunner runner_instance;
  InertSource source;
  source.set_order_list(runner::PollOutcome::FAILED);
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", nullptr));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 1);

  CHECK(source.order_list_reads() == 1);
  CHECK(report.final_gate_ == strategy::HaltReason::STOPPING);
  CHECK_FALSE(report.order_list_read_);
  CHECK(report.closing_source_unverified_);
  CHECK(runner_instance.executor().placed().empty());
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  bool said = false;
  for (const auto& note : report.notes_) {
    said = said || note.find("order list has not been read") != std::string::npos;
  }
  CHECK(said);
}

TEST_CASE("a quiesce pending before the first iteration does not read the order list", "[loop][stop]") {
  // A quiesce sends nothing, so it needs no picture and does not spend the request.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::QUIESCE);

  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::QUIESCE);
  CHECK(source.order_list_reads() == 0);
  CHECK_FALSE(report.order_list_read_);
  CHECK(report.final_gate_ == strategy::HaltReason::STOPPING);
}

TEST_CASE("a stop with a read list but no holdings snapshot is not complete either", "[loop][stop]") {
  // The other half of the source. A `WITHDRAW` takes its references from the holdings snapshot and a
  // `FLATTEN` takes its positions from it, so an epoch-stamped default means the pass does not know
  // what it owes even though the order list was read.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);  // the startup read succeeds
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.order_list_read_);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("no holdings snapshot has arrived") != std::string::npos);
}

TEST_CASE("a stop against a stale holdings snapshot is not complete", "[loop][stop]") {
  // A snapshot older than the configured maximum is the same problem with a timestamp: the working
  // orders it lists may already be gone and the ones it omits may be resting.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  auto portfolio = portfolio_with("10", "5");
  portfolio.as_of_ = base_time() - std::chrono::minutes(10);
  runner_instance.context().set_portfolio(portfolio);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(5));
  StopSource stop;

  loop.run(stop, 1);
  stop.request(StopIntent::WITHDRAW);
  const auto report = loop.run(stop, 1);

  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("older than the configured maximum") != std::string::npos);
}

TEST_CASE("a flatten does not withdraw the same order twice", "[loop][stop]") {
  // The control case for the one above. An instrument that *does* have a position gets a zero
  // target, and reconciliation withdraws its working order itself -- so the orphan sweep must skip
  // it. Two withdrawals for one order is worse than none: the second is an order-writing request
  // against an order that may already be gone.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", "3"));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.flattened_ == 1);
  CHECK(report.withdrawn_ == 1);
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK(report.closing_complete());
}

TEST_CASE("a flatten does not sell a position twice to get out of it", "[loop][stop]") {
  // Ten held against a working sell for ten, and both ways of handling it are wrong. Letting
  // reconciliation treat the resting sell as progress sends nothing, and a limit order that may
  // never fill is not being out of the market. Selling the whole ten anyway ignores that a
  // cancellation is a *request*: the resting sell can fill while its withdrawal is still pending,
  // and ten long becomes ten short.
  //
  // So the order is withdrawn and nothing is sold, because everything held is already working on
  // the closing side -- and the pass says it could not prove which way that resolves.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<InertRunner::Tag> portfolio;
  portfolio.as_of_ = base_time();
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("10");
  position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
  portfolio.positions_.push_back(position);
  algotradelib::WorkingOrder<InertRunner::Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = algotradelib::OrderSide::SELL;  // same side as the close
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("10");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<InertRunner::Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  runner_instance.context().set_portfolio(portfolio);

  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  CHECK(runner_instance.executor().canceled().size() == 1);
  // Nothing sold: every share held is already working on the closing side, so any size at all is
  // the over-close.
  CHECK(report.flattened_ == 0);
  CHECK(runner_instance.executor().placed().empty());
  CHECK(report.closing_contended_ == 1);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("part of the position is still held") != std::string::npos);
}

TEST_CASE("a flatten of a short position is sized the same way", "[loop][stop]") {
  // The mirror, so the rule is not accidentally written for long positions only: a short of ten
  // with a working buy for four closes four fewer than it holds.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<InertRunner::Tag> portfolio;
  portfolio.as_of_ = base_time();
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("-10");
  position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
  portfolio.positions_.push_back(position);
  algotradelib::WorkingOrder<InertRunner::Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = algotradelib::OrderSide::BUY;  // same side as closing a short
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("4");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<InertRunner::Tag>{"0004", ""};
  portfolio.working_.push_back(working);
  runner_instance.context().set_portfolio(portfolio);

  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  REQUIRE(runner_instance.executor().placed().size() == 1);
  const auto& closing = runner_instance.executor().placed().front();
  CHECK(closing.side_ == algotradelib::OrderSide::BUY);
  CHECK(closing.quantity_ == algotradelib::Decimal64::parse("6"));
  CHECK(report.closing_contended_ == 1);
  CHECK_FALSE(report.closing_complete());
}

namespace {

/**
 * A source with no working-order inquiry, like a broker that has only order history.
 *
 * `SessionSource::read_order_list` falls back to reading history on such a broker and hands the rows
 * to journal reconciliation. That satisfies the startup gate -- this process has looked at the
 * broker's own record -- and never populates `portfolio.working_`, so a withdrawal cannot be built
 * from it.
 */
class HistoryOnlySource {
public:
  static constexpr bool provides_working_orders = false;

  runner::PollOutcome poll(runner::PollKind, InertRunner&) {
    return runner::PollOutcome::UNCHANGED;
  }

  runner::PollOutcome read_order_list(InertRunner&) { return runner::PollOutcome::UPDATED; }
};

using HistoryOnlyLoop =
  runner::PollingLoop<InertRunner, HistoryOnlySource, runner::ManualClock>;

} // namespace

TEST_CASE("a flatten sends no close when a sell it cannot see may already be working",
          "[loop][stop]") {
  // A double sell, reached through a listing that is *known* to be out of date.
  // Ten held, an empty working-order list read at startup, a bar that sells all ten, and a stop
  // before the next working-order poll. The snapshot still shows nothing working, so the
  // same-side deduction subtracts nothing and the pass would send a second full-size sell. Marking
  // the report unverified does not recall an instruction that has already gone out.
  SellingRunner runner_instance;
  SellingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = base_time();
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("10");
  position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
  portfolio.positions_.push_back(position);
  state_sellable(portfolio);
  runner_instance.context().set_portfolio(portfolio);

  SellingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());

  not_about_the_portfolio(loop);
  StopSource stop;

  // One iteration: the startup read sees an empty list, then the bar sells the position.
  loop.run(stop, 1);
  REQUIRE(runner_instance.executor().placed().size() == 1);
  REQUIRE(runner_instance.placements_at_broker() == 1);

  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  // No second sell. That is the whole assertion: the report can say what it likes afterwards, but
  // the order would already be at the broker.
  CHECK(runner_instance.executor().placed().size() == 1);
  CHECK(report.flattened_ == 0);
  CHECK(report.closing_contended_ == 1);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("an order this loop cannot see could close them too") !=
        std::string::npos);
}

TEST_CASE("a flatten on a source that cannot list orders sends no close", "[loop][stop]") {
  // The other route to the same exposure: a broker whose order-list read is order *history*, which
  // never populates `working_`. A resting sell is simply invisible, so the deduction has nothing to
  // subtract and a full-size close could double up with it.
  InertRunner runner_instance;
  HistoryOnlySource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", nullptr));

  HistoryOnlyLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(runner_instance.executor().placed().empty());
  CHECK(report.flattened_ == 0);
  CHECK(report.closing_contended_ == 1);
  CHECK_FALSE(report.closing_complete());
}

TEST_CASE("a withdraw places nothing, whatever the strategy asks on the way out", "[loop][stop]") {
  // `WITHDRAW` promises no placements at all. A strategy's `on_stop` is the one route that could
  // have produced one, since the loop itself queues only cancellations.
  ClosingRunner runner_instance;
  ClosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(ten_held());

  ClosingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::WITHDRAW);
  const auto report = loop.run(stop, 1);

  CHECK(runner_instance.executor().placed().empty());
  CHECK(report.closing_placed_ == 0);
  CHECK(report.stop_handler_refused_ == 1);
  CHECK(report.stop_handler_sent_ == 0);
  // The position is untouched, which is what a withdraw means.
  CHECK(report.flattened_ == 0);
}

// --- The market phase, live ---------------------------------------------------------------
//
// Everything below this line is about one defect: nothing outside `backtest/` once called `Context::set_phase`, so a live run held whatever the caller set at
// startup. `on_phase` never fired, `vanilla::TimedLiquidation` was inert live, and a caller who set
// nothing was refused every order. The tests above used to *conceal* that by setting `OPEN` by hand;
// they now install a calendar, so they exercise the mechanism instead.

/** Records every phase it is told about, in order. */
struct RecordsPhases {
  std::vector<strategy::MarketPhase> seen_;

  template <class Ctx>
  void on_phase(Ctx&, strategy::MarketPhase phase) {
    seen_.push_back(phase);
  }
};

using PhaseRunner = runner::DryRunner<RecordsPhases>;
using PhaseSource = runner::ManualSource<PhaseRunner>;
using PhaseLoop = runner::PollingLoop<PhaseRunner, PhaseSource, runner::ManualClock>;

TEST_CASE("a calendar that keeps failing gives up like any other poll", "[loop][phase]") {
  PhaseRunner runner_instance;
  PhaseSource source;
  source.set_default(runner::PollKind::CALENDAR, runner::PollOutcome::FAILED);
  runner::PollSchedule schedule;
  schedule.calendar_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  PhaseLoop loop(runner_instance, source, schedule, test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 20);

  CHECK(report.abandoned_);
  CHECK(report.abandoned_kind_ == runner::PollKind::CALENDAR);
  CHECK(report.final_gate_ == strategy::HaltReason::POLL_FAILING);
}

TEST_CASE("a calendar missing today because its row was unreadable names the cause", "[loop][phase][calendar]") {
  // Readable days either side, and today's row dropped: the calendar spans today and does not cover
  // it. The note names the date, as before, and now also that the broker's row for it was unreadable.
  PhaseRunner runner_instance;
  PhaseSource source;
  const auto session = strategy::SessionWindow::krx();
  const auto today = session.local_date(base_time());
  algotradelib::TradingCalendar holed;
  holed.received_at_ = base_time();
  holed.days_.push_back(algotradelib::TradingDay{algotradelib::Date{today.year_ - 1, 1, 2}, true});
  holed.days_.push_back(algotradelib::TradingDay{algotradelib::Date{today.year_ + 1, 1, 2}, true});
  holed.unreadable_days_ = 1;
  REQUIRE_FALSE(holed.is_trading_day(today).has_value());
  source.set_calendar(holed);
  runner::PollSchedule schedule;
  schedule.calendar_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  PhaseLoop loop(runner_instance, source, schedule, test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK(report.final_phase_ == strategy::MarketPhase::UNKNOWN);
  REQUIRE(report.notes_.size() == 1);
  CHECK(report.notes_[0].find("the calendar does not cover") != std::string::npos);
  CHECK(report.notes_[0].find("; the broker sent 1 calendar row whose date could not be read") !=
        std::string::npos);
}

TEST_CASE("a source that writes the calendar mid-poll is reported, since it cannot be stopped",
          "[loop][phase]") {
  // **The limit of the mechanism, made visible.** A source can replace the
  // calendar and then feed the runner inside one `poll`, and no loop code runs in between, so that
  // order is settled against the phase as it stood when the poll began. Re-deriving the phase before
  // every poll cannot fix it and this test does not pretend otherwise -- the order goes out under the
  // stale phase, which is asserted rather than hidden.
  //
  // What the loop owes is to notice. `PollKind::CALENDAR` states the contract, `SessionSource`
  // satisfies it, and a source that does not gets a line in the report naming the poll.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  serve_calendar(runner_instance, /*trades=*/true);
  source.set_calendar_before_feeding(krx_calendar(/*trades=*/false));
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  SendingLoop loop(runner_instance, source, schedule, test_clock());
  not_about_the_portfolio(loop);
  StopSource stop;

  const auto report = loop.run(stop, 1);

  // The order went out under the phase as it stood before the poll. This is the documented limit.
  CHECK(source.placed() == 1);
  CHECK(source.refused() == 0);
  // And the report says which poll did it, exactly once.
  CHECK(report.sends_under_a_revised_phase_ == 1);
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_[0].find("bars poll changed the market phase and sent a request") !=
        std::string::npos);
  // And it names the check rather than the verdict: the loop observed co-occurrence, not causation.
  CHECK(report.notes_[0].find("check whether that request used the earlier phase") !=
        std::string::npos);
}

TEST_CASE("a run delta that cannot be represented is counted, not dropped", "[loop]") {
  // `since` once skipped a currency whose own subtraction failed, so a run
  // reported absence where a charge had been observed -- the third shape of the same
  // wrong-answer: a figure missing rather than nil, with nothing saying which.
  //
  // Five quintillion won, corrected away, then restated at a tenth of a won: both additions
  // succeed and the two scales cannot be aligned to subtract.
  algotradelib::runner::ChargedCosts before;
  before.add(before.commission_, algotradelib::Money{
    algotradelib::Decimal64::parse("5000000000000000000"), algotradelib::Currency::krw()
  });

  auto after = before;
  after.add(after.commission_, algotradelib::Money{
    algotradelib::Decimal64::parse("-5000000000000000000"), algotradelib::Currency::krw()
  });
  after.add(after.commission_, algotradelib::Money{
    algotradelib::Decimal64::parse("0.1"), algotradelib::Currency::krw()
  });

  const auto delta = after.since(before);
  // The amount cannot be stated, and the run says so rather than reading as a quiet nil.
  CHECK_FALSE(delta.commission_in(algotradelib::Currency::krw()));
  CHECK(delta.unrepresentable_sums_ == 1);
}

TEST_CASE("a run that has never read its holdings does not size an entry against an empty account",
          "[loop][gate][portfolio]") {
  // The measurement behind the portfolio-absence gate. A live loop with the
  // default configuration -- no `maximum_portfolio_age_`, and no holdings cadence -- reaches a bar,
  // runs `on_bar`, reconciles a target against a `Portfolio` that was never populated, and sends.
  // An empty portfolio is the permissive answer for every ceiling and for the target subtraction,
  // so what goes out is sized as though the account held nothing.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK(report.order_list_read_);
  // Shape and multiplicity, not "a refusal occurred somewhere": one bar per iteration, two
  // iterations, and nothing may leave the process in either of them, in either direction.
  CHECK(source.placed() == 0);
  CHECK(source.canceled() == 0);
  // Reported as a plan held back on the snapshot rather than as a guard refusal, because the runner
  // decides it where the instruction is decided -- the loop's hold is not yet installed at every
  // point a source can settle one.
  CHECK(source.withheld_plans() == 2);
  CHECK(source.last_withheld() == runner::WithheldPlanReason::PORTFOLIO_ABSENT);
  CHECK(report.final_gate_ == strategy::HaltReason::PORTFOLIO_ABSENT);
}


namespace {

/**
 * A source for the closing-wait cases. Bars feed the strategy as a real source does; a working-order poll
 * answers; a holdings poll, when it answers, installs the holding the test scripted, stamped at the loop's
 * clock; the order-list read returns scripted outcomes, then `UPDATED`.
 */
class AccountingSource {
public:
  static constexpr bool provides_working_orders = true;

  const runner::ManualClock* clock_{nullptr};
  /** What a holdings poll shows, or nothing for a poll that reports no new answer. */
  std::optional<std::string> held_{"10"};
  /** What it shows once the stop has been requested; nothing for no new answer. */
  std::optional<std::string> held_after_stop_;
  /** A stop this source requests itself, after feeding that many bars, as an operator's arrives mid-run. */
  StopSource* stop_{nullptr};
  std::size_t stop_after_bar_{0};
  StopIntent stop_with_{StopIntent::FLATTEN};
  /** A second word, after that many bars (zero for none). */
  std::size_t second_word_after_bar_{0};
  StopIntent second_word_{StopIntent::FLATTEN};
  /** Holdings polls fail once the stop has been requested. */
  bool holdings_fail_after_stop_{false};
  /** The wall clock is set forward or back by this much at the first holdings poll after the stop. */
  runner::ManualClock* jumping_clock_{nullptr};
  std::chrono::nanoseconds jump_{0};
  /**
   * A target the source leaves queued on the context, unsettled, by the last poll of the iteration whose
   * holdings read is the first after the stop: the bars poll, after its feed.
   */
  bool queue_a_buy_after_stop_{false};
  bool queue_pending_{false};
  bool queued_a_buy_{false};
  std::deque<runner::PollOutcome> order_list_;
  std::size_t holdings_polls_{0};
  std::size_t working_polls_{0};
  std::size_t order_list_reads_{0};

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    if (kind == runner::PollKind::BARS) {
      runner_instance.feed(bar_at(bars_++));
      if (queue_pending_) {
        // After the feed, which settles whatever is queued: this one is still there when the iteration ends.
        queue_pending_ = false;
        queued_a_buy_ = true;
        strategy::TargetPosition target;
        target.instrument_ = samsung();
        target.target_quantity_ = algotradelib::Decimal64::parse("25");
        runner_instance.context().submit(target);
      }
      if (stop_ != nullptr && bars_ == stop_after_bar_) {
        stop_->request(stop_with_);
        stopped_ = true;
      }
      if (stop_ != nullptr && second_word_after_bar_ != 0 && bars_ == second_word_after_bar_) {
        stop_->request(second_word_);
      }
      return runner::PollOutcome::UPDATED;
    }
    if (kind == runner::PollKind::WORKING_ORDERS) {
      ++working_polls_;
      // Stamped as a real source stamps a listing, so holdings read before it are known to be older.
      auto portfolio = runner_instance.context().portfolio();
      portfolio.working_as_of_ = clock_->now();
      runner_instance.context().set_portfolio(portfolio);
      return runner::PollOutcome::UPDATED;
    }
    if (kind == runner::PollKind::HOLDINGS) {
      ++holdings_polls_;
      if (stopped_ && !after_stop_once_) {
        after_stop_once_ = true;
        if (jumping_clock_ != nullptr) {
          jumping_clock_->jump(jump_);
        }
        queue_pending_ = queue_a_buy_after_stop_;
      }
      if (stopped_ && holdings_fail_after_stop_) {
        return runner::PollOutcome::FAILED;
      }
      const auto& shown = stopped_ ? held_after_stop_ : held_;
      if (!shown) {
        return runner::PollOutcome::UNCHANGED;
      }
      auto portfolio = ten_held();
      portfolio.positions_.front().quantity_ = algotradelib::Decimal64::parse(*shown);
      portfolio.as_of_ = clock_->now();
      portfolio.working_as_of_ = runner_instance.context().portfolio().working_as_of_;
      portfolio.working_ = runner_instance.context().portfolio().working_;
      state_sellable(portfolio);
      runner_instance.context().set_portfolio(portfolio);
      return runner::PollOutcome::UPDATED;
    }
    return runner::PollOutcome::UNCHANGED;
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner&) {
    ++order_list_reads_;
    if (order_list_.empty()) {
      return runner::PollOutcome::UPDATED;
    }
    const auto outcome = order_list_.front();
    order_list_.pop_front();
    return outcome;
  }

private:
  std::size_t bars_{0};
  bool stopped_{false};
  bool after_stop_once_{false};
};

using AccountingLoop = runner::PollingLoop<SellingRunner, AccountingSource, runner::ManualClock>;

/** Bars every second; holdings and the order list once a minute, so only a forced read is soon. */
runner::PollSchedule slow_account_reads() {
  auto schedule = bars_each_iteration();
  schedule.holdings_ = std::chrono::seconds(60);
  schedule.working_orders_ = std::chrono::seconds(60);
  return schedule;
}

/** Ten held, and a runner whose strategy sells whatever it is shown. */
void hold_ten(SellingRunner& runner_instance) {
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  auto portfolio = ten_held();
  state_sellable(portfolio);
  runner_instance.context().set_portfolio(portfolio);
}

} // namespace

TEST_CASE("a flatten waits for the reads that account for this runner's last order", "[loop][stop][closing-wait]") {
  // Ten held, a bar that sells all ten, and a flatten before the order list or holdings are read again. Built
  // at once, the closing pass cannot tell whether the ten are still there and closes nothing. It waits instead.
  SellingRunner runner_instance;
  hold_ten(runner_instance);
  AccountingSource source;
  AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
  source.clock_ = &loop.clock();
  not_about_the_portfolio(loop);
  StopSource stop;
  // The stop arrives in the iteration whose bar sold: after that iteration's listing and holdings reads.
  source.stop_ = &stop;
  source.stop_after_bar_ = 1;

  SECTION("the next reads show the sale: the pass is verified and has nothing left to close") {
    source.held_after_stop_ = "0";
    const auto report = loop.run(stop, 0);
    CHECK(report.stopped_by_ == std::optional<StopIntent>(StopIntent::FLATTEN));
    // Both reads were made due at once, not at their one-minute cadence: one more of each.
    CHECK(source.holdings_polls_ == 2);
    CHECK(source.working_polls_ == 2);
    CHECK(report.closing_waited_ > std::chrono::nanoseconds::zero());
    CHECK(report.closing_waited_ < std::chrono::seconds(10));
    CHECK_FALSE(report.closing_wait_expired_);
    CHECK_FALSE(report.closing_source_unverified_);
    CHECK(report.closing_complete());
    // Nothing the strategy asked for while the stop waited went out, and no second sell.
    CHECK(runner_instance.executor().placed().size() == 1);
  }

  SECTION("holdings never answer: the wait ends at its limit and the pass is what it was") {
    const auto report = loop.run(stop, 0);
    CHECK(report.closing_wait_expired_);
    CHECK(report.closing_waited_ >= std::chrono::seconds(30));
    CHECK(report.closing_waited_ < std::chrono::seconds(40));
    CHECK(report.closing_source_unverified_);
    CHECK(report.flattened_ == 0);
    CHECK_FALSE(report.closing_complete());
    CHECK(runner_instance.executor().placed().size() == 1);
    // Read again every few seconds while it waited, not once.
    CHECK(source.holdings_polls_ >= 6);
    bool said = false;
    for (const auto& note : report.notes_) {
      said = said || note.find("the stop waited") != std::string::npos;
    }
    CHECK(said);
  }

  SECTION("a wait of zero is the old behaviour") {
    loop.set_closing_wait(std::chrono::nanoseconds::zero());
    source.held_after_stop_ = "0";
    const auto report = loop.run(stop, 0);
    CHECK(report.iterations_ == 1);
    CHECK(report.closing_waited_ == std::chrono::nanoseconds::zero());
    CHECK(report.closing_source_unverified_);
    CHECK_FALSE(report.closing_complete());
    CHECK(source.holdings_polls_ == 1);
  }

  SECTION("a quiesce never waits") {
    source.stop_with_ = StopIntent::QUIESCE;
    const auto report = loop.run(stop, 0);
    CHECK(report.stopped_by_ == std::optional<StopIntent>(StopIntent::QUIESCE));
    CHECK(report.iterations_ == 1);
    CHECK(report.closing_waited_ == std::chrono::nanoseconds::zero());
  }

  SECTION("the run's last iteration honours the stop rather than leave it pending") {
    const auto report = loop.run(stop, 1);
    CHECK(report.stopped_by_ == std::optional<StopIntent>(StopIntent::FLATTEN));
    CHECK(report.closing_waited_ == std::chrono::nanoseconds::zero());
    CHECK(report.closing_source_unverified_);
  }
}

TEST_CASE("a flatten whose picture is already verified does not wait", "[loop][stop][closing-wait]") {
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  serve_calendar(runner_instance);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", nullptr));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  not_about_the_portfolio(loop);
  StopSource stop;

  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 0);
  CHECK(report.iterations_ == 1);
  CHECK(report.closing_waited_ == std::chrono::nanoseconds::zero());
  CHECK(report.flattened_ == 1);
}

TEST_CASE("a flatten pending at start reads the order list again when the first read fails",
          "[loop][stop][closing-wait]") {
  // A process started with `flatten` in its control file made one attempt at the order list and, when that
  // failed, ended with the position held. The wait gives it the iterations to try again.
  SellingRunner runner_instance;
  hold_ten(runner_instance);
  AccountingSource source;
  source.order_list_ = {runner::PollOutcome::RATE_LIMITED};
  AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
  source.clock_ = &loop.clock();
  not_about_the_portfolio(loop);
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 0);
  CHECK(source.order_list_reads_ == 2);
  CHECK(report.order_list_read_);
  CHECK_FALSE(report.closing_wait_expired_);
  CHECK(report.flattened_ == 1);
  CHECK(report.closing_complete());
  REQUIRE(runner_instance.executor().placed().size() == 1);
  CHECK(runner_instance.executor().placed().front().side_ == algotradelib::OrderSide::SELL);
}

TEST_CASE("a waiting stop keeps its own time: the tick, the wall clock and a second word do not move it",
          "[loop][stop][closing-wait]") {
  SellingRunner runner_instance;
  hold_ten(runner_instance);
  AccountingSource source;
  StopSource stop;
  source.stop_ = &stop;
  source.stop_after_bar_ = 1;

  SECTION("a one-minute tick does not stretch a thirty-second wait") {
    auto schedule = slow_account_reads();
    schedule.bars_ = std::chrono::seconds(60);
    schedule.tick_ = std::chrono::seconds(60);
    AccountingLoop loop(runner_instance, source, schedule, test_clock());
    source.clock_ = &loop.clock();
    not_about_the_portfolio(loop);
    const auto report = loop.run(stop, 0);
    CHECK(report.closing_wait_expired_);
    CHECK(report.closing_waited_ >= std::chrono::seconds(30));
    CHECK(report.closing_waited_ < std::chrono::seconds(36));
    // On the five-second floor throughout: six or seven reads, not one a minute and not one a tick.
    CHECK(source.holdings_polls_ >= 6);
    CHECK(source.holdings_polls_ <= 8);
    CHECK(source.working_polls_ == source.holdings_polls_);
  }

  SECTION("a limit between two reads ends the wait at the limit, not at the next read or tick") {
    auto schedule = slow_account_reads();
    schedule.bars_ = std::chrono::seconds(60);
    schedule.tick_ = std::chrono::seconds(60);
    AccountingLoop loop(runner_instance, source, schedule, test_clock());
    source.clock_ = &loop.clock();
    loop.set_closing_wait(std::chrono::seconds(12));
    not_about_the_portfolio(loop);
    const auto report = loop.run(stop, 0);
    CHECK(report.closing_wait_expired_);
    CHECK(report.closing_waited_ == std::chrono::seconds(12));
    // Told once, when the stop was taken up, however many iterations it then waited.
    CHECK(report.iterations_ > 2);
    CHECK(runner_instance.strategy().stops_ == 1);
  }

  SECTION("the wall clock set forward or back does not change how long it waits") {
    // Measured on the wall clock, a step forward would end the wait at once and a step back would
    // stretch it by the step.
    AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
    source.clock_ = &loop.clock();
    source.jumping_clock_ = &loop.clock();
    not_about_the_portfolio(loop);
    SECTION("forward") {
      source.jump_ = std::chrono::hours(2);
      const auto report = loop.run(stop, 0);
      CHECK(report.closing_wait_expired_);
      CHECK(report.closing_waited_ >= std::chrono::seconds(30));
      CHECK(report.closing_waited_ < std::chrono::seconds(36));
    }
    SECTION("back") {
      // The stamps the pass is verified by are wall-clock stamps, so a step back can also end the wait
      // early (a listing read after the step looks older than the holdings read before it). What is
      // asserted here is only that the wait does not outlast its limit.
      source.jump_ = -std::chrono::hours(2);
      const auto report = loop.run(stop, 0);
      CHECK(report.closing_waited_ < std::chrono::seconds(36));
      CHECK(loop.clock().waited() < std::chrono::minutes(2));
    }
  }

  SECTION("a withdraw that becomes a flatten keeps the first deadline") {
    source.stop_with_ = StopIntent::WITHDRAW;
    source.second_word_after_bar_ = 20;
    AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
    source.clock_ = &loop.clock();
    not_about_the_portfolio(loop);
    const auto report = loop.run(stop, 0);
    CHECK(report.stopped_by_ == std::optional<StopIntent>(StopIntent::FLATTEN));
    CHECK(report.closing_waited_ >= std::chrono::seconds(30));
    CHECK(report.closing_waited_ < std::chrono::seconds(36));
  }

  SECTION("a negative limit, or none, is no wait and reports none run out") {
    for (const auto limit : {-std::chrono::seconds(5), std::chrono::seconds(0)}) {
      AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
      source.clock_ = &loop.clock();
      loop.set_closing_wait(limit);
      not_about_the_portfolio(loop);
      const auto report = loop.run(stop, 0);
      CHECK(report.iterations_ == 1);
      CHECK_FALSE(report.closing_wait_expired_);
    }
  }
}

TEST_CASE("a target left queued while a stop waits is not sent with the closing orders", "[loop][stop][closing-wait]") {
  // The closing instructions are settled with the loop's own hold lifted. A source that queued an
  // instruction during a read, without settling it, would have it settled in that window.
  SellingRunner runner_instance;
  hold_ten(runner_instance);
  AccountingSource source;
  StopSource stop;
  source.stop_ = &stop;
  source.stop_after_bar_ = 1;
  source.held_after_stop_ = "0";
  source.queue_a_buy_after_stop_ = true;
  AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
  source.clock_ = &loop.clock();
  not_about_the_portfolio(loop);

  const auto report = loop.run(stop, 0);
  REQUIRE(source.queued_a_buy_);
  CHECK(report.closing_complete());
  // The one sell the bar placed before the stop, and nothing since: the queued buy was settled under
  // the hold and refused.
  REQUIRE(runner_instance.executor().placed().size() == 1);
  CHECK(runner_instance.executor().placed().front().side_ == algotradelib::OrderSide::SELL);
  CHECK(report.stop_handler_sent_ == 0);
  CHECK(report.stop_handler_refused_ == 1);
}

TEST_CASE("a run abandoned while a stop waits leaves the stop's hold and reports the stop unfinished",
          "[loop][stop][closing-wait]") {
  SellingRunner runner_instance;
  hold_ten(runner_instance);
  AccountingSource source;
  StopSource stop;
  source.stop_ = &stop;
  source.stop_after_bar_ = 1;
  source.holdings_fail_after_stop_ = true;
  AccountingLoop loop(runner_instance, source, slow_account_reads(), test_clock());
  source.clock_ = &loop.clock();
  not_about_the_portfolio(loop);
  runner::FailurePolicy policy;
  policy.consecutive_limit_ = 3;
  loop.set_failure_policy(policy);

  const auto report = loop.run(stop, 0);
  CHECK(report.abandoned_);
  CHECK(report.stopped_by_ == std::optional<StopIntent>(StopIntent::FLATTEN));
  CHECK(report.closing_interrupted_);
  CHECK_FALSE(report.closing_complete());
  CHECK(report.flattened_ == 0);
  CHECK(report.withdrawn_ == 0);
  // `POLL_FAILING` lets a named withdrawal through; the stop asked for more than that.
  CHECK(runner_instance.guard().halted(strategy::HaltReason::STOPPING));
  CHECK(runner_instance.executor().placed().size() == 1);
}
