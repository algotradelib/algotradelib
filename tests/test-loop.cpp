#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>

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
 * mock would be asserting the parser against my own assumptions instead.
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
  return portfolio;
}

/**
 * Polls bars every `seconds`, waking every second.
 *
 * Iterations and polls are therefore *not* one-to-one: the tick bounds how quickly a stop is
 * noticed, so the loop wakes far more often than it polls. That is the intended behaviour and
 * several tests below assert it directly.
 */
runner::PollSchedule bars_every(int seconds) {
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(seconds);
  schedule.tick_ = std::chrono::seconds(1);
  return schedule;
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

} // namespace

TEST_CASE("a manual clock advances only when asked", "[loop][clock]") {
  runner::ManualClock clock{};
  const auto start = clock.now();

  REQUIRE(clock.waits() == 0);
  REQUIRE(clock.now() == start);

  clock.wait_for(30s);
  CHECK(clock.waits() == 1);
  CHECK(clock.now() == start + 30s);
  CHECK(clock.waited() == std::chrono::nanoseconds(30s));

  SECTION("a non-positive wait still counts as a wait but moves nothing") {
    clock.wait_for(0s);
    CHECK(clock.waits() == 2);
    CHECK(clock.now() == start + 30s);
    CHECK(clock.waited() == std::chrono::nanoseconds(30s));
  }

  SECTION("advancing out of band does not count as a wait") {
    clock.advance(5min);
    CHECK(clock.waits() == 1);
    CHECK(clock.now() == start + 30s + 5min);
  }
}

TEST_CASE("a steady clock returns immediately for a non-positive wait", "[loop][clock]") {
  // The only property of the real clock worth asserting, and the one a loop that has fallen behind
  // its cadence depends on: an overdue poll must not pause. Asserting anything about a *positive*
  // wait would be asserting the scheduler.
  const runner::SteadyClock clock{};
  const auto before = std::chrono::steady_clock::now();
  clock.wait_for(std::chrono::nanoseconds(-1));
  clock.wait_for(std::chrono::nanoseconds::zero());
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < 100ms);
}

TEST_CASE("a stop source starts empty and reports the intent it was given", "[loop][stop]") {
  StopSource source;
  REQUIRE_FALSE(source.requested());
  REQUIRE_FALSE(source.intent().has_value());

  source.request(StopIntent::QUIESCE);
  REQUIRE(source.requested());
  REQUIRE(source.intent() == StopIntent::QUIESCE);

  SECTION("clearing restores the empty state") {
    source.clear();
    CHECK_FALSE(source.requested());
    CHECK_FALSE(source.intent().has_value());
  }
}

TEST_CASE("a stop request escalates but never downgrades", "[loop][stop]") {
  // The behaviour that matters operationally. An operator who asks to flatten and then, a moment
  // later, receives a routine SIGTERM must not have the flatten silently reduced to a quiesce --
  // they would be left holding a position they had asked to close.
  SECTION("a weaker intent arriving after a stronger one is ignored") {
    StopSource source;
    source.request(StopIntent::FLATTEN);
    source.request(StopIntent::QUIESCE);
    CHECK(source.intent() == StopIntent::FLATTEN);
  }

  SECTION("a stronger intent arriving after a weaker one takes effect") {
    StopSource source;
    source.request(StopIntent::QUIESCE);
    source.request(StopIntent::WITHDRAW);
    CHECK(source.intent() == StopIntent::WITHDRAW);
    source.request(StopIntent::FLATTEN);
    CHECK(source.intent() == StopIntent::FLATTEN);
  }

  SECTION("repeating the same intent changes nothing") {
    StopSource source;
    source.request(StopIntent::WITHDRAW);
    source.request(StopIntent::WITHDRAW);
    CHECK(source.intent() == StopIntent::WITHDRAW);
  }
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

TEST_CASE("a disabled control file reads nothing", "[loop][stop]") {
  runner::FileStopChannel channel;
  CHECK_FALSE(channel.enabled());
  CHECK_FALSE(channel.poll().has_value());
  CHECK(channel.unrecognized().empty());
}

TEST_CASE("an absent control file is not an error", "[loop][stop]") {
  // The normal case for the whole life of a healthy run, so it must not report a problem.
  runner::FileStopChannel channel(temp_path("absent"));
  std::remove(channel.path().c_str());
  REQUIRE(channel.enabled());
  CHECK_FALSE(channel.poll().has_value());
  CHECK(channel.unrecognized().empty());
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

TEST_CASE("a loop polls on its interval, not once per iteration", "[loop][cadence]") {
  // The whole point of ManualClock: this asserts arithmetic, not scheduling luck.
  //
  // Bars every 60s with a 1s tick means the loop wakes 121 times and polls 3 times -- at t=0, t=60
  // and t=120. Waking more often than it polls is the design: the tick is what bounds how quickly an
  // operator's stop is noticed, and a loop trading daily bars must not take a day to respond.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_every(60), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 121);

  CHECK(report.iterations_ == 121);
  CHECK(report.attempted(runner::PollKind::BARS) == 3);
  CHECK(source.polls(runner::PollKind::BARS) == 3);
  // Disabled kinds are never polled, rather than polled and ignored.
  CHECK(report.attempted(runner::PollKind::QUOTES) == 0);
  CHECK(report.attempted(runner::PollKind::HOLDINGS) == 0);
  CHECK(report.attempted(runner::PollKind::WORKING_ORDERS) == 0);
  // It paced itself rather than spinning, which a poll count alone would not show.
  CHECK(loop.clock().waits() == 121);
  CHECK(loop.clock().waited() == std::chrono::nanoseconds(121 * std::chrono::seconds(1)));
}

TEST_CASE("matching the tick to the interval polls every iteration", "[loop][cadence]") {
  // The companion to the test above, and the schedule the behaviour tests use.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 5);

  CHECK(report.iterations_ == 5);
  CHECK(report.attempted(runner::PollKind::BARS) == 5);
}

TEST_CASE("a sparse cadence still notices a stop within one tick", "[loop][cadence]") {
  // A loop polling bars daily must not take a day to respond to an operator. The tick is the floor
  // on responsiveness, and this is what keeps the control file usable.
  InertRunner runner_instance;
  InertSource source;
  runner::PollSchedule schedule;
  schedule.bars_ = 24h;
  schedule.tick_ = 1s;
  InertLoop loop(runner_instance, source, schedule, runner::ManualClock{});
  StopSource stop;

  const auto report = loop.run(stop, 3);

  CHECK(report.iterations_ == 3);
  // Polled once; the following iterations had nothing due but still ran.
  CHECK(report.attempted(runner::PollKind::BARS) == 1);
  CHECK(loop.clock().waited() == std::chrono::nanoseconds(3s));
}

TEST_CASE("nothing is sent before the order list has been read", "[loop][gate]") {
  // The cheapest half of order-journal reconciliation, and the half that needs no persistence. A
  // runner that has just started cannot know whether a predecessor died mid-POST.
  InertRunner runner_instance;
  InertSource source;
  source.set_order_list(runner::PollOutcome::FAILED);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 3);

  CHECK_FALSE(report.order_list_read_);
  CHECK(report.blocked_ == 3);
  CHECK(report.final_gate_ == strategy::HaltReason::ORDER_LIST_UNREAD);
  // Blocked means halted, which is what actually stops a send.
  CHECK(runner_instance.guard().halted());
  // Retried each iteration rather than given up on, because the read is what lifts the gate.
  CHECK(source.order_list_reads() == 3);
}

TEST_CASE("a successful order-list read opens the gate once and stays open", "[loop][gate]") {
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 4);

  CHECK(report.order_list_read_);
  CHECK(report.blocked_ == 0);
  CHECK(!report.final_gate_.has_value());
  // Read once, not once per iteration: the gate is about this process having looked, not about
  // continuous polling.
  CHECK(source.order_list_reads() == 1);
  CHECK_FALSE(runner_instance.guard().halted());
}

TEST_CASE("a stale portfolio blocks sending", "[loop][gate]") {
  // Reconciling a target against a stale snapshot is how a position gets double-sized by a route
  // that working_quantity does not close.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(5));
  StopSource stop;

  SECTION("a portfolio that was never populated is stale") {
    const auto report = loop.run(stop, 2);
    CHECK(report.final_gate_ == strategy::HaltReason::PORTFOLIO_STALE);
    CHECK(report.blocked_ == 2);
    CHECK(runner_instance.guard().halted());
  }

  SECTION("a fresh snapshot opens the gate") {
    strategy::Portfolio<InertRunner::Tag> portfolio;
    portfolio.as_of_ = base_time();
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(!report.final_gate_.has_value());
    CHECK_FALSE(runner_instance.guard().halted());
  }

  SECTION("a snapshot older than the maximum closes it again") {
    strategy::Portfolio<InertRunner::Tag> portfolio;
    // Ten minutes before the clock's start, against a five-minute maximum.
    portfolio.as_of_ = base_time() - std::chrono::minutes(10);
    runner_instance.context().set_portfolio(portfolio);

    const auto report = loop.run(stop, 1);
    CHECK(report.final_gate_ == strategy::HaltReason::PORTFOLIO_STALE);
    CHECK(runner_instance.guard().halted());
  }
}

TEST_CASE("repeated poll failures give up rather than spinning", "[loop][failure]") {
  InertRunner runner_instance;
  InertSource source;
  source.set_default(runner::PollKind::BARS, runner::PollOutcome::FAILED);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  runner::FailurePolicy policy;
  policy.consecutive_limit_ = 3;
  loop.set_failure_policy(policy);
  StopSource stop;

  const auto report = loop.run(stop, 100);

  CHECK(report.abandoned_);
  CHECK(report.abandoned_kind_ == runner::PollKind::BARS);
  CHECK(report.iterations_ == 3);
  CHECK(report.failed(runner::PollKind::BARS) == 3);
  CHECK(runner_instance.guard().halted());
  CHECK_FALSE(report.notes_.empty());
  // The report must not say sending was permitted while the guard says it was not, and it must be
  // distinguishable from an operator's stop -- nobody asked for this one.
  CHECK(report.final_gate_ == strategy::HaltReason::POLL_FAILING);
  CHECK_FALSE(report.stopped_by_.has_value());
}

TEST_CASE("a rate-limit refusal is not a failure", "[loop][failure]") {
  // The limiter working as designed. Counting it as an outage would make a loop polling slightly too
  // fast shut itself down.
  InertRunner runner_instance;
  InertSource source;
  source.set_default(runner::PollKind::BARS, runner::PollOutcome::RATE_LIMITED);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  runner::FailurePolicy policy;
  policy.consecutive_limit_ = 3;
  loop.set_failure_policy(policy);
  StopSource stop;

  const auto report = loop.run(stop, 10);

  CHECK_FALSE(report.abandoned_);
  CHECK(report.iterations_ == 10);
  CHECK(report.failed(runner::PollKind::BARS) == 0);
}

TEST_CASE("nothing new is not a failure either", "[loop][failure]") {
  InertRunner runner_instance;
  InertSource source;  // defaults to UNCHANGED
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  runner::FailurePolicy policy;
  policy.consecutive_limit_ = 2;
  loop.set_failure_policy(policy);
  StopSource stop;

  const auto report = loop.run(stop, 6);

  CHECK_FALSE(report.abandoned_);
  CHECK(report.failed(runner::PollKind::BARS) == 0);
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

TEST_CASE("a stop request ends the run and is reported", "[loop][stop]") {
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::QUIESCE);

  const auto report = loop.run(stop, 100);

  // One iteration ran to completion before exiting: a quiesce finishes what is in flight rather
  // than abandoning it, which is what keeps nothing indeterminate.
  CHECK(report.iterations_ == 1);
  CHECK(report.stopped_by_ == StopIntent::QUIESCE);
  CHECK(report.final_gate_ == strategy::HaltReason::STOPPING);
  // A quiesce withdraws nothing and flattens nothing.
  CHECK(report.withdrawn_ == 0);
  CHECK(report.flattened_ == 0);
}

TEST_CASE("a control file stops the loop without a signal", "[loop][stop]") {
  const auto path = temp_path("loop-control");
  write_file(path, "quiesce\n");

  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_control_file(runner::FileStopChannel(path));
  StopSource stop;

  const auto report = loop.run(stop, 50);

  CHECK(report.iterations_ == 1);
  CHECK(report.stopped_by_ == StopIntent::QUIESCE);
  CHECK(stop.intent() == StopIntent::QUIESCE);
  std::remove(path.c_str());
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

TEST_CASE("the loop does not clear a halt an operator set by hand", "[loop][halt]") {
  // The loop takes named holds and releases only its own, so a person's halt is not something it
  // can touch. It does report it: an operator reading the tail of a log is told that they are the
  // reason nothing is being sent, rather than shown an open gate while nothing goes out.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.guard().halt();
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK(report.final_gate_ == strategy::HaltReason::OPERATOR);
  CHECK(report.blocked_ == 2);
  CHECK(runner_instance.guard().halted());
  CHECK(runner_instance.guard().halted(strategy::HaltReason::OPERATOR));
}

TEST_CASE("an operator halting into a closed gate survives the gate opening", "[loop][halt]") {
  // The defect this branch exists for, end to end. Before named holds, step 2 was a no-op because
  // the guard was already halted, and step 4 cleared the operator's intent along with the loop's.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(5));
  StopSource stop;

  // 1. The gate closes, because the holdings snapshot was never populated.
  const auto stale = loop.run(stop, 1);
  CHECK(stale.final_gate_ == strategy::HaltReason::PORTFOLIO_STALE);
  CHECK(runner_instance.guard().halted(strategy::HaltReason::PORTFOLIO_STALE));

  // 2. An operator, watching, decides independently to stop trading.
  runner_instance.guard().halt();

  // 3. Fresh holdings arrive, so the loop's own condition clears.
  strategy::Portfolio<InertRunner::Tag> portfolio;
  portfolio.as_of_ = base_time();
  runner_instance.context().set_portfolio(portfolio);

  // 4. The loop releases its hold -- and only its hold.
  const auto after = loop.run(stop, 1);
  CHECK_FALSE(runner_instance.guard().halted(strategy::HaltReason::PORTFOLIO_STALE));
  CHECK(runner_instance.guard().halted());
  CHECK(runner_instance.guard().halted(strategy::HaltReason::OPERATOR));
  CHECK(after.final_gate_ == strategy::HaltReason::OPERATOR);
}

TEST_CASE("an operator resuming does not open a gate the loop holds closed", "[loop][halt]") {
  // The mirror, and a fix that only worked in one direction would be worse than the bool: an
  // operator clearing their own halt must not be able to lift a hold the loop took for stale data.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(5));
  StopSource stop;

  runner_instance.guard().halt();
  const auto report = loop.run(stop, 1);
  CHECK(runner_instance.guard().halted(strategy::HaltReason::OPERATOR));

  // The loop reported the operator's hold, which outranks its own, but it took its own as well.
  CHECK(report.final_gate_ == strategy::HaltReason::OPERATOR);
  CHECK(runner_instance.guard().halted(strategy::HaltReason::PORTFOLIO_STALE));

  runner_instance.guard().resume();
  CHECK(runner_instance.guard().halted());
  CHECK(runner_instance.guard().halted(strategy::HaltReason::PORTFOLIO_STALE));
}

TEST_CASE("a withdraw cancels what is resting and leaves the position", "[loop][stop]") {
  // The middle intent, and the one whose cost is easy to miss: each cancellation is itself an
  // order-writing request that can fail indeterminately.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_portfolio(portfolio_with("10", "3"));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 10);

  CHECK(report.stopped_by_ == StopIntent::WITHDRAW);
  CHECK(report.withdrawn_ == 1);
  // A withdraw closes nothing, which is exactly what distinguishes it from a flatten.
  CHECK(report.flattened_ == 0);
  // The cancellation actually went out: lifting the halt for the closing instructions is the point.
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK(runner_instance.executor().placed().empty());
  // And the gate is closed again afterwards.
  CHECK(runner_instance.guard().halted());
}

TEST_CASE("a flatten cancels and then closes the position", "[loop][stop]") {
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", "3"));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.stopped_by_ == StopIntent::FLATTEN);
  // Cancelled once, by reconciliation rather than by the loop: a flatten that also cancelled directly
  // would send two withdrawals for the same order.
  CHECK(report.withdrawn_ == 1);
  CHECK(report.flattened_ == 1);
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK(report.closing_complete());
  INFO("closing: placed=" << report.closing_placed_
    << " rejected=" << report.closing_rejected_
    << " refused=" << report.closing_refused_
    << " withheld=" << report.closing_withheld_);
  for (const auto& note : report.notes_) {
    INFO("note: " << note);
  }
  // The closing order is a sell of everything held.
  REQUIRE(runner_instance.executor().placed().size() == 1);
  const auto& closing = runner_instance.executor().placed().front();
  CHECK(closing.instrument_ == samsung());
  CHECK(closing.side_ == algotradelib::OrderSide::SELL);
  CHECK(closing.quantity_ == algotradelib::Decimal64::parse("10"));
  CHECK(runner_instance.guard().halted());
}

TEST_CASE("a flatten with nothing held sends nothing", "[loop][stop]") {
  // A flatten must be safe to issue when already flat, because an operator under pressure will not
  // check first.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_portfolio(portfolio_with("0", nullptr));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.flattened_ == 0);
  CHECK(runner_instance.executor().placed().empty());
}

TEST_CASE("a flatten outside market hours is refused and says so", "[loop][stop]") {
  // The trap this report exists for. The guard permits only MarketPhase::OPEN by default, so a
  // flatten issued when the market is shut cannot send -- which is correct, and silent unless the
  // loop says otherwise. An operator who asked to be flat and heard nothing would believe they were.
  InertRunner runner_instance;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::CLOSED);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with("10", nullptr));
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  // It asked to close one position...
  CHECK(report.flattened_ == 1);
  // ...and nothing went out.
  CHECK(report.closing_placed_ == 0);
  CHECK(report.closing_refused_ == 1);
  CHECK_FALSE(report.closing_complete());
  CHECK(runner_instance.executor().placed().empty());
  // And the report says the position is still held, in words.
  REQUIRE_FALSE(report.notes_.empty());
  bool warned = false;
  for (const auto& note : report.notes_) {
    if (note.find("still held") != std::string::npos) {
      warned = true;
    }
  }
  CHECK(warned);
}

TEST_CASE("a local failure halts the loop and nothing lifts it", "[loop][gate]") {
  // A journal that cannot be written will fail on the next order too. Without this gate the loop
  // would produce one refusal per bar, each looking like the broker declining an instruction it
  // never saw.
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  runner_instance.ledger();  // the ledger is untouched: nothing was ever attempted.
  CHECK(runner_instance.ledger().empty());

  const auto before = loop.run(stop, 1);
  CHECK(!before.final_gate_.has_value());

  // What `SendOutcome::UNAVAILABLE` leaves behind on the runner.
  runner_instance.halt_locally("the order journal could not be written");

  const auto report = loop.run(stop, 3);
  CHECK(report.final_gate_ == strategy::HaltReason::LOCAL_FAILURE);
  CHECK(report.blocked_ == 3);
  CHECK(runner_instance.guard().halted());
  // Checked before the order-list read, so a runner whose journal is broken is not sent looking for
  // more work to fail at.
  CHECK(source.order_list_reads() == 1);

  // Only an operator clears it.
  runner_instance.resume_after_local_failure();
  const auto after = loop.run(stop, 1);
  CHECK(!after.final_gate_.has_value());
  CHECK_FALSE(runner_instance.guard().halted());
}

TEST_CASE("gate reason names are stable", "[loop][gate]") {
  CHECK(strategy::halt_reason_name(strategy::HaltReason::ORDER_LIST_UNREAD) == "order_list_unread");
  CHECK(strategy::halt_reason_name(strategy::HaltReason::PORTFOLIO_STALE) == "portfolio_stale");
  CHECK(strategy::halt_reason_name(strategy::HaltReason::STOPPING) == "stopping");
  CHECK(strategy::halt_reason_name(strategy::HaltReason::POLL_FAILING) == "poll_failing");
  CHECK(strategy::halt_reason_name(strategy::HaltReason::LOCAL_FAILURE) == "local_failure");
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
