#include <algotradelib/live>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
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

private:
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
