// Both sessions held to the same order behaviour, through the same scripted broker.
//
// `BrokerSession` and `BlockingSession` each write their own place, replace and cancel. The rules that matter
// -- an unsupported trigger refused before the adapter, on placement and modification but never on cancellation
// -- are shared through `port_detail::prepare_place` / `prepare_replace`, and this file is what makes a drift
// between the two classes a failing test rather than a review item. Every case runs once per session.
#include "mock_broker.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

using namespace algotradelib;

namespace {

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

const mock::MockAccount kAccount{"cust", 1};

using Placed = PlacedOrder<mock::Broker>;
using Ref = OrderRef<mock::Broker>;

/** The asynchronous session, each verb run to completion on its own loop. */
struct AsyncDriver {
  static constexpr const char* name = "BrokerSession";
  net::io_context ioc_;
  std::shared_ptr<mock::MockClient> client_ = std::make_shared<mock::MockClient>(ioc_.get_executor());
  BrokerSession<mock::MockClient> session_{client_, kAccount};

  mock::MockClient& mock() { return *client_; }

  template <typename Future>
  auto finish(Future future) {
    ioc_.restart();
    ioc_.run();
    return future.get();
  }
  Result<Placed> place(OrderRequest order) { return finish(session_.async_place(std::move(order), net::use_future)); }
  Result<Placed> replace(const Ref& ref, AmendInstruction instruction) {
    return finish(session_.async_replace(ref, std::move(instruction), net::use_future));
  }
  Result<Placed> cancel(const Ref& ref, AmendInstruction instruction) {
    return finish(session_.async_cancel(ref, std::move(instruction), net::use_future));
  }
  Result<OrderPage> order_history(OrderHistoryQuery query) {
    return finish(session_.async_order_history(std::move(query), net::use_future));
  }
};

/** The blocking session over the blocking mock, which drives the same in-memory client. */
struct BlockingDriver {
  static constexpr const char* name = "BlockingSession";
  std::shared_ptr<mock::MockBlockingClient> client_ = std::make_shared<mock::MockBlockingClient>();
  BlockingSession<mock::MockBlockingClient> session_{client_, kAccount};

  mock::MockClient& mock() { return client_->mock(); }

  Result<Placed> place(OrderRequest order) { return session_.place(std::move(order)); }
  Result<Placed> replace(const Ref& ref, AmendInstruction instruction) { return session_.replace(ref, instruction); }
  Result<Placed> cancel(const Ref& ref, AmendInstruction instruction) { return session_.cancel(ref, instruction); }
  Result<OrderPage> order_history(OrderHistoryQuery query) { return session_.order_history(query); }
};

/** A plain limit order the mock accepts. */
OrderRequest limit_order() {
  OrderRequest order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.quantity_ = Decimal64::parse("1");
  order.price_ = Decimal64::parse("70000");
  return order;
}

/** A resting stop-limit order, as a working-order listing would report it. */
WorkingOrder<mock::Broker> resting_stop() {
  WorkingOrder<mock::Broker> working;
  working.ref_.primary_ = "ticket-1";
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::STOP_LIMIT;
  working.order_.price_ = Decimal64::parse("69000");
  working.order_.trigger_price_ = Decimal64::parse("69500");
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.amendable_quantity_ = Decimal64::parse("10");
  return working;
}

/** A plain resting limit order, with no trigger. */
WorkingOrder<mock::Broker> resting_limit() {
  auto working = resting_stop();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.trigger_price_.reset();
  return working;
}

/** Scripts the mock to accept every order instruction. */
template <typename Driver>
void accept_orders(Driver& driver) {
  driver.mock().ack_ = mock::MockAck{"ticket-1", std::chrono::system_clock::now()};
}

} // namespace

TEMPLATE_TEST_CASE("both sessions refuse a trigger the broker cannot send, before the adapter", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  accept_orders(driver);
  auto order = limit_order();
  order.trigger_price_ = Decimal64::parse("69000");

  const auto placed = driver.place(order);
  REQUIRE_FALSE(placed);
  CHECK(placed.error().broker_code_ == "unsupported_trigger_price");
  CHECK_FALSE(placed.error().reached_broker_);
  CHECK(driver.mock().place_calls_ == 0);
}

TEMPLATE_TEST_CASE("both sessions run the adapter's own placement check", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  accept_orders(driver);
  auto order = limit_order();
  order.client_order_id_ = std::string("key-1");

  const auto placed = driver.place(order);
  REQUIRE_FALSE(placed);
  CHECK(placed.error().broker_code_ == "idempotency_key_unsupported");
  CHECK(driver.mock().place_calls_ == 0);
}

TEMPLATE_TEST_CASE("both sessions check the trigger before the adapter's placement check", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  // Both refuse; the order pins which runs first, so the two sessions cannot disagree on the error reported.
  TestType driver;
  accept_orders(driver);
  auto order = limit_order();
  order.trigger_price_ = Decimal64::parse("69000");
  order.client_order_id_ = std::string("key-1");

  const auto placed = driver.place(order);
  REQUIRE_FALSE(placed);
  CHECK(placed.error().broker_code_ == "unsupported_trigger_price");
  CHECK(driver.mock().place_calls_ == 0);
}

TEMPLATE_TEST_CASE("both sessions send a valid order once and normalize the answer alike", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  accept_orders(driver);

  const auto placed = driver.place(limit_order());
  REQUIRE(placed);
  CHECK(placed.value().ack_.order_id_ == "ticket-1");
  CHECK(placed.value().ref_.primary_ == "ticket-1");
  CHECK(driver.mock().place_calls_ == 1);
}

TEMPLATE_TEST_CASE("both sessions pass a broker's own refusal through unchanged", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;  // nothing scripted: the mock answers with an error, as a broker refusing would

  const auto placed = driver.place(limit_order());
  REQUIRE_FALSE(placed);
  CHECK(placed.error().reached_broker_);
  CHECK(placed.error().message_ == "no acknowledgement scripted");
  CHECK(driver.mock().place_calls_ == 1);
}

TEMPLATE_TEST_CASE("both sessions refuse a modification that would carry a trigger", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  accept_orders(driver);
  const auto stop = resting_stop();

  SECTION("restating it") {
    const auto replaced = driver.replace(stop.ref_, reprice(stop, Decimal64::parse("68000")));
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().broker_code_ == "unsupported_trigger_price");
    CHECK_FALSE(replaced.error().reached_broker_);
  }
  SECTION("keeping the resting one") {
    const auto replaced =
      driver.replace(stop.ref_, replace_with(stop, OrderShape{OrderType::STOP_LIMIT, Decimal64::parse("68000")}));
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().broker_code_ == "unsupported_trigger_price");
  }
  CHECK(driver.mock().replace_calls_ == 0);
}

TEMPLATE_TEST_CASE("both sessions send a valid modification once", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  accept_orders(driver);
  const auto limit = resting_limit();

  const auto replaced = driver.replace(limit.ref_, reprice(limit, Decimal64::parse("68000")));
  REQUIRE(replaced);
  CHECK(driver.mock().replace_calls_ == 1);
  REQUIRE(driver.mock().last_amendment_);
  CHECK(driver.mock().last_amendment_->ticket_ == "ticket-1");
}

TEMPLATE_TEST_CASE("both sessions send a cancellation that carries a trigger", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  // `cancel_all` restates the trigger the listing reported. Refusing it would leave a stop order the caller
  // cannot withdraw, which is why cancellation has no trigger check in either session.
  TestType driver;
  accept_orders(driver);
  const auto stop = resting_stop();

  const auto cancelled = driver.cancel(stop.ref_, cancel_all(stop));
  REQUIRE(cancelled);
  CHECK(driver.mock().cancel_calls_ == 1);
}

TEMPLATE_TEST_CASE("both sessions refuse a history query the adapter cannot convert", "[port][parity]",
                   AsyncDriver, BlockingDriver) {
  TestType driver;
  OrderHistoryQuery query;
  query.symbol_ = std::string("005930");

  const auto history = driver.order_history(query);
  REQUIRE_FALSE(history);
  CHECK(history.error().broker_code_ == "symbol_filter_unsupported");
  CHECK(driver.mock().history_calls_ == 0);
}
