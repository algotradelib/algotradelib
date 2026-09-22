#include <algotradelib/live>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;
namespace live = algotradelib::live;

namespace {

/** Compile-time identity of the fake broker. */
struct FakeBroker {
  static constexpr std::string_view name = "fake";
};

/** Capabilities a fake session can be given, to exercise both sides of a gate. */
struct WithKey : strategy::NoCaps {
  static constexpr bool has_client_order_id = true;
};
struct WithoutKey : strategy::NoCaps {
  static constexpr bool has_client_order_id = false;
};

/**
 * Capability variants for the working-order inquiry.
 *
 * `SessionSource::read_order_list` branches on `has_working_orders` at compile time -- working orders
 * where the broker offers them, order history where it does not -- so both branches need a broker to
 * be instantiated against, or only one of them is ever compiled.
 */
struct WithWorkingOrders : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
};
struct WithoutWorkingOrders : strategy::NoCaps {
  static constexpr bool has_working_orders = false;
};

/**
 * A session that records what it was asked and answers however a test says.
 *
 * `SessionExecutor` is duck-typed on the session, so this needs no broker adapter and no transport:
 * the live send path is exercised end to end with nothing on the wire. A transport failure mid-POST
 * is not something a real broker can be asked for, which is the whole reason this seam exists.
 */
template <typename CapsType>
class FakeSession {
public:
  using Caps = CapsType;
  using BrokerTag = FakeBroker;
  using Ref = OrderRef<FakeBroker>;
  using Placed = PlacedOrder<FakeBroker>;

  Result<Placed> place(OrderRequest order) {
    placed_.push_back(order);
    if (error_) {
      return Result<Placed>(*error_);
    }
    Placed ack;
    ack.ack_.order_id_ = "0001569139";
    ack.ref_ = Ref{"0001569139", ""};
    return Result<Placed>(std::move(ack));
  }

  Result<Placed> cancel(const Ref& ref, const AmendInstruction& instruction) {
    canceled_.push_back(instruction);
    canceled_refs_.push_back(ref);
    if (error_) {
      return Result<Placed>(*error_);
    }
    Placed ack;
    ack.ref_ = ref;
    return Result<Placed>(std::move(ack));
  }

  /** Makes every later call fail with `category` and `status`. */
  void fail_with(ErrorCategory category, int status = 0) {
    BrokerError error;
    error.category_ = category;
    error.http_status_ = status;
    error_ = error;
  }

  void succeed() { error_.reset(); }

  const std::vector<OrderRequest>& placed() const noexcept { return placed_; }
  const std::vector<AmendInstruction>& canceled() const noexcept { return canceled_; }
  const std::vector<Ref>& canceled_refs() const noexcept { return canceled_refs_; }

  // ---- the read side, which `SessionSource` polls ----

  /** Returns the scripted bar series, counting the call. */
  Result<BarSeries> bars(const BarQuery& query) {
    ++bar_calls_;
    if (error_) {
      return Result<BarSeries>(*error_);
    }
    BarSeries series = bars_;
    series.instrument_ = query.instrument_;
    return Result<BarSeries>(std::move(series));
  }

  /** Returns the scripted quote, counting the call. */
  Result<QuoteSnapshot> quote(const InstrumentId& instrument) {
    ++quote_calls_;
    if (error_) {
      return Result<QuoteSnapshot>(*error_);
    }
    QuoteSnapshot snapshot = quote_;
    snapshot.instrument_ = instrument;
    return Result<QuoteSnapshot>(std::move(snapshot));
  }

  /** Returns the scripted holdings, counting the call. */
  Result<HoldingsSnapshot> holdings() {
    ++holdings_calls_;
    if (error_) {
      return Result<HoldingsSnapshot>(*error_);
    }
    return Result<HoldingsSnapshot>(holdings_);
  }

  /** Returns the scripted working orders, counting the call. */
  Result<WorkingOrders<FakeBroker>> working_orders() {
    ++working_calls_;
    if (error_) {
      return Result<WorkingOrders<FakeBroker>>(*error_);
    }
    return Result<WorkingOrders<FakeBroker>>(working_orders_);
  }

  /** Returns the scripted history page, counting the call. */
  Result<OrderPage> order_history(const OrderHistoryQuery&) {
    ++history_calls_;
    if (error_) {
      return Result<OrderPage>(*error_);
    }
    return Result<OrderPage>(history_);
  }

  void set_bars(BarSeries series) { bars_ = std::move(series); }
  void set_quote(QuoteSnapshot snapshot) { quote_ = std::move(snapshot); }
  void set_holdings(HoldingsSnapshot snapshot) { holdings_ = std::move(snapshot); }
  void set_working_orders(WorkingOrders<FakeBroker> orders) {
    working_orders_ = std::move(orders);
  }
  void set_history(OrderPage page) { history_ = std::move(page); }

  std::size_t bar_calls() const noexcept { return bar_calls_; }
  std::size_t quote_calls() const noexcept { return quote_calls_; }
  std::size_t holdings_calls() const noexcept { return holdings_calls_; }
  std::size_t working_calls() const noexcept { return working_calls_; }
  std::size_t history_calls() const noexcept { return history_calls_; }

private:
  BarSeries bars_;
  QuoteSnapshot quote_;
  HoldingsSnapshot holdings_;
  WorkingOrders<FakeBroker> working_orders_;
  OrderPage history_;
  std::size_t bar_calls_{0};
  std::size_t quote_calls_{0};
  std::size_t holdings_calls_{0};
  std::size_t working_calls_{0};
  std::size_t history_calls_{0};

  std::vector<OrderRequest> placed_;
  std::vector<AmendInstruction> canceled_;
  std::vector<Ref> canceled_refs_;
  std::optional<BrokerError> error_;
};

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** A planned order as reconciliation would have produced it: on the grid, on the lot. */
strategy::PlannedOrder planned() {
  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.time_in_force_ = TimeInForce::DAY;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");
  order.tag_ = "test";
  return order;
}

WorkingOrder<FakeBroker> working() {
  WorkingOrder<FakeBroker> order;
  order.order_.instrument_ = samsung();
  order.order_.side_ = OrderSide::BUY;
  order.order_.status_ = OrderStatus::PENDING;
  order.order_.type_ = OrderType::LIMIT;
  order.order_.price_ = Decimal64::parse("70000");
  order.order_.quantity_ = Decimal64::parse("10");
  order.amendable_quantity_ = Decimal64::parse("10");
  order.ref_ = OrderRef<FakeBroker>{"0001569139", ""};
  return order;
}

} // namespace

TEST_CASE("an accepted order reaches the session with its fields intact") {
  // Reconciliation already put the price on the tick grid and the quantity on the lot, so this
  // boundary adds nothing numeric; it moves fields, and a field lost here reaches an exchange.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.placed().size() == 1);
  const auto& sent = session.placed()[0];
  REQUIRE(sent.instrument_.symbol_ == "005930");
  REQUIRE(sent.side_ == OrderSide::BUY);
  REQUIRE(sent.type_ == OrderType::LIMIT);
  REQUIRE(sent.time_in_force_ == TimeInForce::DAY);
  REQUIRE(sent.quantity_ == Decimal64::parse("10"));
  REQUIRE(sent.price_ == std::optional<Decimal64>(Decimal64::parse("70000")));

  // And the reference the broker issued is kept, which is what a later amendment must address.
  REQUIRE(executor.last_ref());
  REQUIRE(executor.last_ref()->primary_ == "0001569139");
  // The compile-time broker tag travels with it, which is what stops one broker's reference being
  // handed to another's session.
  REQUIRE(executor.last_ref()->str() == "fake:0001569139");
  REQUIRE_FALSE(executor.last_error());
}

TEST_CASE("a transport failure on the live path leaves the outcome unknown") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::TRANSPORT);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::INDETERMINATE);
  // It still reached the session, which is exactly why the outcome is unknown rather than refused.
  REQUIRE(session.placed().size() == 1);
  REQUIRE(executor.last_error());
  REQUIRE(executor.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("a broker refusal on the live path is an answer, not a doubt") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::HTTP, 400);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::REJECTED);
  REQUIRE_FALSE(executor.last_ref());
}

TEST_CASE("an unparseable reply is treated as possibly successful") {
  // The trap the classification exists for: transport plainly succeeded, so it reads like a
  // client-side problem, and the order may be live.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::PROTOCOL);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::INDETERMINATE);
}

TEST_CASE("an idempotency key a broker cannot honour is refused, never dropped") {
  // Dropping it would make a retry look safe on a broker offering no such guarantee, which is the
  // specific mistake OrderRequest::client_order_id_ documents. The strategy asked for a guarantee
  // the binding cannot provide.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  auto order = planned();
  order.client_order_id_ = "idem-1";
  REQUIRE(executor.place(order) == runner::SendOutcome::REJECTED);
  // Refused before the session was touched, so nothing went out without the guarantee.
  REQUIRE(session.placed().empty());
  REQUIRE(executor.last_error());
  REQUIRE(executor.last_error()->category_ == ErrorCategory::INVALID_ARGUMENT);
}

TEST_CASE("an idempotency key a broker does honour is carried through") {
  FakeSession<WithKey> session;
  live::SessionExecutor<FakeSession<WithKey>> executor(session);

  auto order = planned();
  order.client_order_id_ = "idem-1";
  REQUIRE(executor.place(order) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.placed().size() == 1);
  REQUIRE(session.placed()[0].client_order_id_ == std::optional<std::string>("idem-1"));
}

TEST_CASE("a cancellation carries the order's own shape and amendable quantity") {
  // Restating either here would be inventing a value the broker will act on. cancel_all reads them
  // off the order, which is what a broker requiring them receives.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  REQUIRE(executor.cancel(working()) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.canceled().size() == 1);
  const auto& instruction = session.canceled()[0];
  REQUIRE(instruction.all_remaining_);
  REQUIRE(instruction.shape_.type_ == OrderType::LIMIT);
  REQUIRE(instruction.shape_.price_ == std::optional<Decimal64>(Decimal64::parse("70000")));
  REQUIRE(instruction.quantity_ == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(session.canceled_refs()[0].primary_ == "0001569139");
}

TEST_CASE("a cancellation that fails in transit reports the doubt it leaves") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::CANCELLED);

  // A local abandonment: the withdrawal may already have been sent.
  REQUIRE(executor.cancel(working()) == runner::SendOutcome::INDETERMINATE);
}

// ---------------------------------------------------------------------------
// The whole pipeline over a session, with nothing on the wire
// ---------------------------------------------------------------------------

namespace {

/** Holds ten shares, stated as a target. */
struct HoldTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
  }
};

using LiveRunner = runner::Runner<
  HoldTen, WithoutKey, FakeBroker, live::SessionExecutor<FakeSession<WithoutKey>>
>;

Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

strategy::BarEvent bar_at(int hour, const char* close) {
  Bar bar;
  bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + hour);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);
}

} // namespace

TEST_CASE("a strategy drives a session through the same pipeline a dry run uses") {
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto report = live_runner.feed(bar_at(0, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  // It actually reached the session, on the lot. The target named no limit, so reconciliation
  // produced a market order -- and a market order carries no price, which is the invariant
  // OrderRequest states and every adapter enforces before any I/O.
  REQUIRE(session.placed().size() == 1);
  REQUIRE(session.placed()[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(session.placed()[0].type_ == OrderType::MARKET);
  REQUIRE_FALSE(session.placed()[0].price_);
}

TEST_CASE("a mid-flight failure on the live path stops the next order for that instrument") {
  // The acceptance criterion, end to end over a session: nothing further is submitted, and the
  // session is the only thing that could have sent it.
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

  session.fail_with(ErrorCategory::TRANSPORT);
  const auto first = live_runner.feed(bar_at(0, "70000"));
  REQUIRE(first.indeterminate_.size() == 1);
  REQUIRE(live_runner.ledger().requires_reconcile(samsung()));
  REQUIRE(session.placed().size() == 1);

  // The transport recovers; the doubt about the first order does not.
  session.succeed();
  const auto second = live_runner.feed(bar_at(24, "70000"));
  REQUIRE(second.withheld_.size() == 1);
  REQUIRE(second.would_place_.empty());
  // Still exactly one request ever made, which is the promise: no retry, no double position.
  REQUIRE(session.placed().size() == 1);

  // Only reading the broker's order list releases it.
  live_runner.ledger().resolve_after_reading_orders(samsung());
  const auto third = live_runner.feed(bar_at(48, "70000"));
  REQUIRE(third.would_place_.size() == 1);
  REQUIRE(session.placed().size() == 2);
}

// ---------------------------------------------------------------------------
// Binding in one call
// ---------------------------------------------------------------------------

TEST_CASE("binding reads the capabilities and the tag off the session") {
  // The repetition the factories remove, and the mismatch they make unexpressible: writing the four
  // parameters out lets a caller pair one broker's tag with another's capabilities.
  FakeSession<WithKey> session;
  auto bound = live::bind(session, HoldTen{}, {});

  STATIC_REQUIRE(std::is_same_v<decltype(bound)::Tag, FakeBroker>);
  STATIC_REQUIRE(decltype(bound)::Context::caps::has_client_order_id);

  bound.context().set_instrument(samsung_reference());
  bound.context().set_phase(strategy::MarketPhase::OPEN);
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto report = bound.feed(bar_at(0, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  // It reached the session, which is what `bind` is for.
  REQUIRE(session.placed().size() == 1);
}

TEST_CASE("an observing binding has the same types and nothing to send through") {
  // Same Caps and BrokerTag as the live binding, so a capability-gated strategy compiles the same
  // branch in both and observing cannot diverge from trading.
  FakeSession<WithKey> session;
  auto observing = live::bind_observer(session, HoldTen{}, {});

  STATIC_REQUIRE(std::is_same_v<decltype(observing)::Tag, FakeBroker>);
  STATIC_REQUIRE(decltype(observing)::Context::caps::has_client_order_id);

  observing.context().set_instrument(samsung_reference());
  observing.context().set_phase(strategy::MarketPhase::OPEN);
  observing.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto report = observing.feed(bar_at(0, "70000"));
  // The decision is reported as a placement, where it belongs -- not as a refusal, which is what
  // halting the guard would have produced.
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.refused_.empty());
  // And the session was never touched: there is nothing to send through, rather than a check that
  // declined to send.
  REQUIRE(session.placed().empty());
}

TEST_CASE("an observing binding records nothing in the ledger, because nothing was sent") {
  FakeSession<WithKey> session;
  session.fail_with(ErrorCategory::TRANSPORT);
  auto observing = live::bind_observer(session, HoldTen{}, {});
  observing.context().set_instrument(samsung_reference());
  observing.context().set_phase(strategy::MarketPhase::OPEN);
  observing.context().set_last_price(samsung(), Decimal64::parse("70000"));

  // The session would have failed, and it is never asked, so no doubt is created.
  observing.feed(bar_at(0, "70000"));
  REQUIRE(observing.ledger().empty());
}

// ---------------------------------------------------------------------------
// SessionSource: the read half of a live loop.
//
// Duck-typed on the session exactly as SessionExecutor is, so these run with no adapter and nothing
// on the wire. What they assert is the part a real broker could not be asked to demonstrate on
// demand: a rate-limit refusal, a failure mid-poll, and the same closed bar arriving twice.
// ---------------------------------------------------------------------------

namespace {

/** A strategy that counts what it was fed, so a test can prove the source actually fed it. */
struct CountEvents {
  std::size_t bars_{0};
  std::size_t quotes_{0};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {
    ++bars_;
  }

  template <class Ctx>
  void on_quote(Ctx&, const strategy::QuoteEvent&) {
    ++quotes_;
  }
};

using WorkingSession = FakeSession<WithWorkingOrders>;
using HistorySession = FakeSession<WithoutWorkingOrders>;

/** A two-bar series ending at `last_close`. */
BarSeries two_bars(const char* first_close, const char* last_close) {
  BarSeries series;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::CLOSE;
  Bar first;
  first.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  first.close_ = Decimal64::parse(first_close);
  first.high_ = first.close_;
  first.low_ = first.close_;
  Bar second = first;
  second.timestamp_ = first.timestamp_ + std::chrono::hours(24);
  second.close_ = Decimal64::parse(last_close);
  second.high_ = second.close_;
  second.low_ = second.close_;
  series.bars_ = {first, second};
  return series;
}

} // namespace

TEST_CASE("a rate-limit refusal is not counted as a poll failure") {
  // The limiter working as designed. Counting it as an outage would make a loop polling slightly too
  // fast shut itself down instead of simply slowing down.
  BrokerError limited;
  limited.category_ = ErrorCategory::RATE_LIMIT;
  CHECK(live::poll_outcome_of(limited) == runner::PollOutcome::RATE_LIMITED);

  for (const auto category : {
         ErrorCategory::TRANSPORT,
         ErrorCategory::PROTOCOL,
         ErrorCategory::HTTP,
         ErrorCategory::AUTHENTICATION,
         ErrorCategory::CANCELLED,
         ErrorCategory::INVALID_ARGUMENT,
       }) {
    BrokerError error;
    error.category_ = category;
    CHECK(live::poll_outcome_of(error) == runner::PollOutcome::FAILED);
  }
}

TEST_CASE("polling bars feeds each new bar exactly once") {
  // The property that matters: a poll returning the same closed bar again is the normal case between
  // closes, and feeding it twice would double every signal derived from it.
  WorkingSession session;
  session.set_bars(two_bars("70000", "71000"));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  const auto first = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(first == runner::PollOutcome::UPDATED);
  CHECK(live_runner.strategy().bars_ == 2);

  // Same series again: nothing new.
  const auto second = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(second == runner::PollOutcome::UNCHANGED);
  CHECK(live_runner.strategy().bars_ == 2);

  // One more bar appears, and only it is fed.
  auto extended = two_bars("70000", "71000");
  Bar third = extended.bars_.back();
  third.timestamp_ += std::chrono::hours(24);
  third.close_ = Decimal64::parse("72000");
  extended.bars_.push_back(third);
  session.set_bars(extended);

  const auto after = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(after == runner::PollOutcome::UPDATED);
  CHECK(live_runner.strategy().bars_ == 3);
}

TEST_CASE("a failing poll reports the failure and its reason") {
  WorkingSession session;
  session.set_bars(two_bars("70000", "71000"));
  session.fail_with(ErrorCategory::TRANSPORT);
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::BARS, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
  // Nothing was fed, and nothing is owed: a poll that failed sent nothing, so unlike an order write
  // it leaves no question about what exists.
  CHECK(live_runner.strategy().bars_ == 0);
  CHECK(live_runner.ledger().empty());
}

TEST_CASE("polling holdings stamps the portfolio with the broker's own time") {
  // The loop's staleness gate measures against as_of_, so it must be when the broker says the
  // positions were true rather than when the reply happened to arrive.
  WorkingSession session;
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  snapshot.positions_.push_back(position);
  snapshot.received_at_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  session.set_holdings(snapshot);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  const auto& portfolio = live_runner.context().portfolio();
  REQUIRE(portfolio.positions_.size() == 1);
  CHECK(portfolio.positions_[0].quantity_ == Decimal64::parse("10"));
  CHECK(portfolio.as_of_ == snapshot.received_at_);
}

TEST_CASE("holdings and working orders merge rather than replace each other") {
  // They are separate inquiries on separate cadences. Rebuilding the portfolio from either alone
  // would drop the other, and a dropped working order is one reconciliation places again.
  WorkingSession session;
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  snapshot.positions_.push_back(position);
  session.set_holdings(snapshot);
  WorkingOrders<FakeBroker> orders;
  orders.orders_.push_back(working());
  session.set_working_orders(orders);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  source.poll(runner::PollKind::HOLDINGS, live_runner);
  source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
  CHECK(live_runner.context().portfolio().positions_.size() == 1);
  CHECK(live_runner.context().portfolio().working_.size() == 1);

  // And in the other order, because neither cadence is guaranteed to fire first.
  auto other = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> other_source(session, {samsung()});
  other_source.poll(runner::PollKind::WORKING_ORDERS, other);
  other_source.poll(runner::PollKind::HOLDINGS, other);
  CHECK(other.context().portfolio().positions_.size() == 1);
  CHECK(other.context().portfolio().working_.size() == 1);
}

TEST_CASE("the order-list read follows the broker's capability") {
  // A compile-time choice, so both branches need a broker to instantiate against or only one is ever
  // compiled. The gate needs *an* order list, not a particular one.
  SECTION("a broker with working orders uses that inquiry") {
    WorkingSession session;
    auto live_runner = live::bind_observer(session, CountEvents{});
    live::SessionSource<WorkingSession> source(session, {samsung()});

    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK(session.working_calls() == 1);
    CHECK(session.history_calls() == 0);
  }

  SECTION("a broker without it falls back to order history") {
    HistorySession session;
    auto live_runner = live::bind_observer(session, CountEvents{});
    live::SessionSource<HistorySession> source(session, {samsung()});

    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK(session.history_calls() == 1);
    CHECK(session.working_calls() == 0);
  }
}

TEST_CASE("a failing order-list read leaves the gate closed") {
  // The gate exists because an empty ledger does not mean nothing is owed -- it means nothing is
  // remembered. A read that failed has established nothing, so it must not satisfy the gate.
  HistorySession session;
  session.fail_with(ErrorCategory::HTTP, 503);
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->http_status_ == 503);
}

TEST_CASE("a broker without the working-order inquiry never polls for one") {
  // A cadence is data, not a capability claim: a caller who sets one for an inquiry this broker lacks
  // gets a poll that does nothing rather than a build failure.
  HistorySession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UNCHANGED);
  CHECK(session.working_calls() == 0);
}

// ---------------------------------------------------------------------------
// Sweeping the ledger from a listing the source already fetched.
//
// The half of the order journal that needs a session. Everything about *what* a match means is
// decided in `runner::match_against` and tested without one; what these assert is that the rows a
// real inquiry returns reach it, that the ledger is cleared by what they settle, and that a source
// with no journal behaves exactly as it did before.
// ---------------------------------------------------------------------------

namespace {

/** The instant the journal entries below were recorded at. */
std::chrono::system_clock::time_point journaled_at() {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
}

/** An unanswered placement in a journal, which is what a killed process leaves behind. */
runner::JournalEntry journaled_placement(const char* id = "run-1") {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = runner::JournalOutcome::PENDING;
  entry.recorded_at_ = journaled_at();
  entry.instrument_ = samsung();
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::LIMIT;
  entry.quantity_ = Decimal64::parse("10");
  entry.price_ = Decimal64::parse("70000");
  return entry;
}

/** A listing row matching `journaled_placement`, as the broker would report it. */
OrderSnapshot listed_order(const char* order_id) {
  auto snapshot = working().order_;
  snapshot.order_id_ = order_id;
  snapshot.ordered_at_ = journaled_at();
  return snapshot;
}

/** Long enough after the send that an absent row means the order never existed. */
std::chrono::system_clock::time_point swept_at() {
  return journaled_at() + std::chrono::minutes(10);
}

} // namespace

TEST_CASE("a source with no journal reads the list and matches nothing") {
  // The pre-existing behaviour, unchanged: the gate lifts because this process has looked at the
  // broker's own record, and a mark nothing accounts for stays exactly where it is.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession> source(session, {samsung()});
  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
  CHECK(source.last_reconcile().matches_.empty());
}

TEST_CASE("the order-list read resolves what the broker's rows settle") {
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().order_id_ == "KRX-1");
  CHECK(source.last_reconcile().settled());
  // The instrument may trade again, and only reading the broker's own list got it there.
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("a working-order poll sweeps the ledger without a request of its own") {
  // The rows a working-order poll already fetches are exactly what the matcher needs, so
  // reconciliation rides along rather than asking the broker again.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UPDATED);
  CHECK(session.working_calls() == 1);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("the history fallback sweeps against the page it read") {
  // A broker with no working-order inquiry still reconciles, from the wider listing it does offer.
  HistorySession session;
  OrderPage page;
  page.orders_.push_back(listed_order("KRX-1"));
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<HistorySession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(session.history_calls() == 1);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an ambiguous listing leaves the instrument withheld") {
  // Two rows that could both be the journaled order. Guessing would either leave a live order
  // unwatched or double a position, so the hold stays and the report says why.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  for (const auto* id : {"KRX-1", "KRX-2"}) {
    auto listed = working();
    listed.order_ = listed_order(id);
    orders.orders_.push_back(listed);
  }
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(source.last_reconcile().ambiguous_);
  CHECK_FALSE(source.last_reconcile().settled());
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an empty listing settles nothing until the window has closed") {
  WorkingSession session;
  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  // Five seconds after the send, an absent row is not evidence of anything.
  live_runner.context().set_now(journaled_at() + std::chrono::seconds(5));
  source.read_order_list(live_runner);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::UNDECIDED);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));

  // Ten minutes after it, the order was never placed.
  live_runner.context().set_now(swept_at());
  source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::NOT_PLACED);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}
