#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <optional>
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

  /** Feeds one bar per bars poll, and records what the runner decided while doing it. */
  runner::PollOutcome poll(runner::PollKind kind, RunnerT& runner_instance) {
    if (kind != runner::PollKind::BARS) {
      return runner::PollOutcome::UNCHANGED;
    }
    const auto report = runner_instance.feed(bar_at(++bars_));
    placed_ += report.would_place_.size();
    refused_ += report.refused_.size();
    if (!report.refused_.empty()) {
      last_refusal_ = report.refused_.front().halt_reason_;
    }
    return runner::PollOutcome::UPDATED;
  }

  /** Returns the configured order-list outcome. */
  runner::PollOutcome read_order_list(RunnerT&) {
    ++order_list_reads_;
    return order_list_;
  }

  /** Returns how many orders the runner would have sent from inside a poll. */
  std::size_t placed() const noexcept { return placed_; }

  /** Returns how many the guard refused there. */
  std::size_t refused() const noexcept { return refused_; }

  /** Returns which hold refused the last one. */
  std::optional<strategy::HaltReason> last_refusal() const noexcept { return last_refusal_; }

private:
  runner::PollOutcome order_list_ = runner::PollOutcome::UPDATED;
  std::size_t order_list_reads_{0};
  std::size_t bars_{0};
  std::size_t placed_{0};
  std::size_t refused_{0};
  std::optional<strategy::HaltReason> last_refusal_;
};

using FeedingSource = FeedingSourceFor<SendingRunner>;
using SendingLoop = runner::PollingLoop<SendingRunner, FeedingSource, runner::ManualClock>;

/** A strategy that wants out of whatever it is shown, so a bar produces a sell. */
struct SellEverything {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = algotradelib::Decimal64{};
    ctx.submit(target);
  }
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
  // There is no uniquely correct finite ceiling on a loaded host; five seconds is where a stalled
  // machine is distinguishable from a slow one, and the assertion claims nothing beyond that.
  //
  // It is a `CHECK` so that the rest of this case still runs when it trips -- the case does fail,
  // which is the point; what it avoids is hiding any later assertion behind this one.
  const runner::SteadyClock clock{};
  const auto before = std::chrono::steady_clock::now();
  clock.wait_for(std::chrono::nanoseconds(-1));
  clock.wait_for(std::chrono::nanoseconds::zero());
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < 5s);
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
  // program's request count unchanged on upgrade.
  CHECK(source.polls(runner::PollKind::HOLDINGS) == 0);
}

TEST_CASE("a run reports what it learned about the broker's sellable figure", "[loop][probe]") {
  InertRunner runner_instance;
  InertSource source;
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  runner_instance.probe_sellable_convention();

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

  // One ordinary iteration first, so the startup read happens and the closing pass knows what is
  // outstanding. A stop requested before that runs against an unverified picture, which is its own
  // test below.
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);

  const auto report = loop.run(stop, 10);

  CHECK(report.stopped_by_ == StopIntent::FLATTEN);
  CHECK_FALSE(report.closing_source_unverified_);
  // Cancelled once and closed once. The pass states both itself rather than deriving the
  // cancellation from a target, so there is exactly one withdrawal per resting order.
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

  // One ordinary iteration first, so the closing source is verified and the pass actually builds a
  // closing order -- otherwise it withholds the close for its own reasons and the guard, which is
  // what this test is about, never sees one.
  loop.run(stop, 1);
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

TEST_CASE("nothing is sent before the first order-list read, from inside a poll", "[loop][gate]") {
  // The gate has to be in force *before* a poll runs, because a poll is what feeds the runner: a
  // real source calls `Runner::feed` per instrument and `feed` settles. With the gate computed
  // after the polls, the very first bar of a run could place an order against a position a
  // predecessor may have left resting -- the exact hazard the startup gate exists for -- and the
  // report would then say `order_list_unread` about an order that had already gone out.
  SendingRunner runner_instance;
  FeedingSource source;
  source.set_order_list(runner::PollOutcome::FAILED);
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 3);

  // The strategy submitted on every bar and the guard refused every one of them.
  CHECK(source.placed() == 0);
  CHECK(source.refused() == 3);
  CHECK(source.last_refusal() == strategy::HaltReason::ORDER_LIST_UNREAD);
  CHECK_FALSE(report.order_list_read_);
  CHECK(report.final_gate_ == strategy::HaltReason::ORDER_LIST_UNREAD);
}

TEST_CASE("the gate opens for the same iteration once the order list is read", "[loop][gate]") {
  // The other half, and the reason the gate is computed before the polls rather than simply being
  // held for a whole extra iteration: a successful read lifts it in time for that iteration's bar.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 1);

  CHECK(report.order_list_read_);
  CHECK(source.order_list_reads() == 1);
  CHECK(source.placed() == 1);
  CHECK(source.refused() == 0);
}

TEST_CASE("a stale portfolio blocks a send made from inside a poll", "[loop][gate]") {
  // Same ordering, the other gate. Sizing against a stale snapshot is how a position gets
  // double-sized by a route `working_quantity` does not close, so the hold must precede the feed.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(5));
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK(source.placed() == 0);
  CHECK(source.refused() == 2);
  CHECK(source.last_refusal() == strategy::HaltReason::PORTFOLIO_STALE);
  CHECK(report.final_gate_ == strategy::HaltReason::PORTFOLIO_STALE);
}

TEST_CASE("an operator's halt stops a send made from inside a poll", "[loop][halt]") {
  // A halt has to reach the one place an order actually leaves, which is inside a poll rather than
  // anywhere the loop itself calls.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.guard().halt();
  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  const auto report = loop.run(stop, 2);

  CHECK(source.placed() == 0);
  CHECK(source.refused() == 2);
  CHECK(source.last_refusal() == strategy::HaltReason::OPERATOR);
  CHECK(report.final_gate_ == strategy::HaltReason::OPERATOR);
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a withdraw with no halt standing still cancels", "[loop][stop]") {
  // The control case for the one above: the refusal must come from the halt, not from the closing
  // pass being broken.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_portfolio(portfolio_with("10", "5"));
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);  // the startup read, so the closing pass has a verified source
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  CHECK(report.closing_refused_ == 0);
  CHECK_FALSE(report.closing_source_unverified_);
  CHECK(report.closing_complete());
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);

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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);

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

TEST_CASE("a stop before the first order-list read is not a complete stop", "[loop][stop]") {
  // The sequence that matters: a stop requested before the loop's first iteration. `gate_for`
  // reports `STOPPING` and never reaches the startup read, so the closing pass is built from
  // whatever the holdings snapshot happens to hold -- and a predecessor's resting orders are not in
  // it. With nothing in the snapshot at all, the pass queues nothing, every failure count stays
  // zero, and before this the stop reported itself complete having established nothing.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::WITHDRAW);
  CHECK(report.withdrawn_ == 0);
  CHECK_FALSE(report.order_list_read_);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("could not verify what was outstanding") != std::string::npos);
  CHECK(report.notes_.front().find("order list has not been read") != std::string::npos);
}

TEST_CASE("a stop still withdraws what it can see, and says the picture was partial",
          "[loop][stop]") {
  // Sending nothing would be worse: withdrawing the orders the snapshot does hold beats withdrawing
  // none of them. The pass does both -- it sends, and it refuses to call the result complete.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_portfolio(portfolio_with("10", "5"));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::WITHDRAW);

  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
}

TEST_CASE("a stop with a read list but no holdings snapshot is not complete either", "[loop][stop]") {
  // The other half of the source. A `WITHDRAW` takes its references from the holdings snapshot and a
  // `FLATTEN` takes its positions from it, so an epoch-stamped default means the pass does not know
  // what it owes even though the order list was read.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a flatten withdraws a working order with no position behind it", "[loop][stop]") {
  // A flatten cancels through the zero targets it queues, so an instrument with nothing held gets
  // no target -- and before this its resting order survived the stop entirely. A working *buy*
  // against a flat book is the case that matters: it can fill after the loop exits, leaving a
  // position an operator believes they closed.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(portfolio_with(nullptr, "5"));
  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);  // the startup read, so the source is verified
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.flattened_ == 0);  // nothing held, so nothing to close
  CHECK(report.withdrawn_ == 1);  // but the order is still withdrawn
  CHECK(runner_instance.executor().canceled().size() == 1);
  CHECK_FALSE(report.closing_source_unverified_);
  CHECK(report.closing_complete());
}

TEST_CASE("a flatten does not withdraw the same order twice", "[loop][stop]") {
  // The control case for the one above. An instrument that *does* have a position gets a zero
  // target, and reconciliation withdraws its working order itself -- so the orphan sweep must skip
  // it. Two withdrawals for one order is worse than none: the second is an order-writing request
  // against an order that may already be gone.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("an order sent after the last working-order read leaves the stop unverified",
          "[loop][stop]") {
  // The sequence the holdings timestamp cannot catch, and it needs no stale data at all: read an
  // empty working-order list at startup, accept a buy from a bar, refresh holdings so `as_of_` is
  // fresh, then stop before the next working-order poll. `working_` still omits that buy, so a
  // `WITHDRAW` queues nothing -- and before this the fresh `as_of_` made the source look verified
  // and the stop reported complete without ever requesting the cancellation.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = base_time();
  runner_instance.context().set_portfolio(portfolio);

  SendingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  // One iteration: the startup read, then a bar that places an order from inside the poll.
  loop.run(stop, 1);
  REQUIRE(source.placed() == 1);
  REQUIRE(runner_instance.placements_at_broker() == 1);

  // Holdings refresh: `as_of_` moves, `working_` does not. This is what `poll_holdings` does.
  auto refreshed = runner_instance.context().portfolio();
  refreshed.as_of_ = base_time() + std::chrono::seconds(1);
  runner_instance.context().set_portfolio(refreshed);

  stop.request(StopIntent::WITHDRAW);
  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 0);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("an order was sent after the working orders were last read") !=
        std::string::npos);
}

TEST_CASE("a working-order poll re-verifies the source an earlier send invalidated",
          "[loop][stop]") {
  // The control case: the flag is about the picture being out of date, not about having ever sent.
  // Once the working orders are read again, a stop can report complete.
  SendingRunner runner_instance;
  FeedingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = base_time();
  runner_instance.context().set_portfolio(portfolio);

  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  SendingLoop loop(runner_instance, source, schedule, test_clock());
  StopSource stop;

  loop.run(stop, 2);  // startup read, a send, then a working-order poll on the next iteration

  stop.request(StopIntent::WITHDRAW);
  const auto report = loop.run(stop, 1);

  CHECK_FALSE(report.closing_source_unverified_);
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a flatten closes only the part no working order is already selling", "[loop][stop]") {
  // The partial case: a working sell for three against ten held. Withdraw the three and sell seven,
  // which is the most that cannot overshoot. If the withdrawal completes, three are still held and
  // the report says so; if it fails because the three filled, the account is flat. Selling ten
  // would risk being three short.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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
  working.order_.side_ = algotradelib::OrderSide::SELL;
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("3");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<InertRunner::Tag>{"0002", ""};
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
  CHECK(closing.side_ == algotradelib::OrderSide::SELL);
  CHECK(closing.quantity_ == algotradelib::Decimal64::parse("7"));
  CHECK(closing.type_ == algotradelib::OrderType::MARKET);
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

TEST_CASE("a source with no working-order listing cannot verify a stop", "[loop][stop]") {
  // The history fallback reads the broker's own record, which is what the startup gate needs, and
  // leaves `working_` empty -- so a `WITHDRAW` queues nothing. Before this the loop stamped that
  // read as a working-order refresh and the stop reported complete with an order still resting at
  // the broker.
  InertRunner runner_instance;
  HistoryOnlySource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  strategy::Portfolio<InertRunner::Tag> fresh;
  fresh.as_of_ = base_time();
  runner_instance.context().set_portfolio(fresh);
  HistoryOnlyLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;

  loop.run(stop, 1);  // the startup read succeeds, through history
  stop.request(StopIntent::WITHDRAW);
  const auto report = loop.run(stop, 1);

  CHECK(report.order_list_read_);
  CHECK(report.withdrawn_ == 0);
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
  REQUIRE_FALSE(report.notes_.empty());
  CHECK(report.notes_.front().find("does not publish the broker's outstanding orders") !=
        std::string::npos);
}

TEST_CASE("a flatten closes the whole position when the working order opposes it", "[loop][stop]") {
  // The control case for the sizing rule: an order on the *other* side cannot reduce the position,
  // so it cannot double up with the close and the full size is safe. Withdrawing it and selling
  // everything is exactly right, and nothing is contended.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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
  working.order_.side_ = algotradelib::OrderSide::BUY;  // opposes the close
  working.order_.status_ = algotradelib::OrderStatus::PENDING;
  working.order_.quantity_ = algotradelib::Decimal64::parse("4");
  working.order_.execution_.filled_quantity_ = algotradelib::Decimal64::parse("0");
  working.ref_ = algotradelib::OrderRef<InertRunner::Tag>{"0003", ""};
  portfolio.working_.push_back(working);
  runner_instance.context().set_portfolio(portfolio);

  InertLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  CHECK(report.withdrawn_ == 1);
  CHECK(report.flattened_ == 1);
  REQUIRE(runner_instance.executor().placed().size() == 1);
  CHECK(runner_instance.executor().placed().front().quantity_ ==
        algotradelib::Decimal64::parse("10"));
  CHECK(report.closing_contended_ == 0);
  CHECK(report.closing_complete());
}

TEST_CASE("a flatten of a short position is sized the same way", "[loop][stop]") {
  // The mirror, so the rule is not accidentally written for long positions only: a short of ten
  // with a working buy for four closes four fewer than it holds.
  InertRunner runner_instance;
  InertSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a flatten sends no close when a sell it cannot see may already be working",
          "[loop][stop]") {
  // The same double sell as round 8, reached through a listing that is *known* to be out of date.
  // Ten held, an empty working-order list read at startup, a bar that sells all ten, and a stop
  // before the next working-order poll. The snapshot still shows nothing working, so the
  // same-side deduction subtracts nothing and the pass would send a second full-size sell. Marking
  // the report unverified does not recall an instruction that has already gone out.
  SellingRunner runner_instance;
  SellingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));

  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = base_time();
  algotradelib::HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = algotradelib::Decimal64::parse("10");
  position.average_purchase_price_ = algotradelib::Decimal64::parse("70000");
  portfolio.positions_.push_back(position);
  runner_instance.context().set_portfolio(portfolio);

  SellingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
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
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a strategy's on_stop cannot close a position the flatten is already closing",
          "[loop][stop]") {
  // Two producers of instructions in one pass. The loop sizes its close against what it can prove
  // is outstanding; a strategy's `on_stop` target goes through none of that reasoning and would be
  // planned against the same snapshot. Ten held would become ten short if both sells filled.
  ClosingRunner runner_instance;
  ClosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(ten_held());

  ClosingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);  // the startup read, so the source is verified
  stop.request(StopIntent::FLATTEN);
  const auto report = loop.run(stop, 1);

  // Exactly one sell, and it is the loop's: the strategy's was refused while the hold stood.
  REQUIRE(runner_instance.executor().placed().size() == 1);
  CHECK(runner_instance.executor().placed().front().quantity_ ==
        algotradelib::Decimal64::parse("10"));
  CHECK(report.flattened_ == 1);
  CHECK(report.stop_handler_refused_ == 1);
  CHECK(report.stop_handler_sent_ == 0);
  REQUIRE_FALSE(report.notes_.empty());
  bool told = false;
  for (const auto& note : report.notes_) {
    if (note.find("the strategy's on_stop asked for") != std::string::npos) {
      told = true;
    }
  }
  CHECK(told);
}

TEST_CASE("a strategy's on_stop cannot place while the closing source is unverified",
          "[loop][stop]") {
  // The loop withholds its own close when it cannot enumerate what is outstanding. A strategy's
  // shutdown order must not walk through that constraint either -- it is the same exposure with a
  // different author.
  ClosingRunner runner_instance;
  ClosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(ten_held());

  ClosingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  stop.request(StopIntent::FLATTEN);  // before any listing is read
  const auto report = loop.run(stop, 1);

  CHECK(runner_instance.executor().placed().empty());
  CHECK(report.closing_source_unverified_);
  CHECK(report.stop_handler_refused_ == 1);
  CHECK(report.stop_handler_sent_ == 0);
  CHECK_FALSE(report.closing_complete());
}

TEST_CASE("a withdraw places nothing, whatever the strategy asks on the way out", "[loop][stop]") {
  // `WITHDRAW` promises no placements at all. A strategy's `on_stop` is the one route that could
  // have produced one, since the loop itself queues only cancellations.
  ClosingRunner runner_instance;
  ClosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
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

TEST_CASE("a quiesce still tells the strategy it is stopping", "[loop][stop]") {
  // The handler's purpose is to let a strategy know, and that should not depend on which stop the
  // operator chose. Nothing can go out under any of them.
  ClosingRunner runner_instance;
  ClosingSource source;
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), algotradelib::Decimal64::parse("70000"));
  runner_instance.context().set_portfolio(ten_held());

  ClosingLoop loop(runner_instance, source, bars_each_iteration(), test_clock());
  StopSource stop;
  loop.run(stop, 1);
  stop.request(StopIntent::QUIESCE);
  const auto report = loop.run(stop, 1);

  CHECK(report.stopped_by_ == StopIntent::QUIESCE);
  CHECK(report.stop_handler_refused_ == 1);
  CHECK(runner_instance.executor().placed().empty());
}
