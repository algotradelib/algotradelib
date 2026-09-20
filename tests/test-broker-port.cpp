#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include "mock_broker.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace {

/** Returns the instrument used throughout these tests. */
InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** Returns a KIS acknowledgement as the adapter would hand one back. */
kis::KisOrderAck kis_ack() {
  kis::KisOrderAck ack;
  ack.ack_.order_id_ = "0000117057";
  ack.ack_.accepted_at_ = std::chrono::system_clock::now();
  ack.order_ = kis::KisOrderRef{"00950", "0000117057"};
  ack.broker_order_time_ = "121052";
  return ack;
}

/** Returns one KIS amendable-order row with the supplied original order number. */
kis::KisOpenOrder kis_open_order(std::string order_number, std::string original_number) {
  kis::KisOpenOrder open_order;
  open_order.order_ref_ = kis::KisOrderRef{"00950", std::move(order_number)};
  open_order.original_order_ = kis::KisOrderRef{"00950", std::move(original_number)};
  open_order.amendable_quantity_ = Decimal64::parse("7");
  open_order.order_.order_id_ = open_order.order_ref_.order_number_;
  open_order.order_.instrument_ = samsung();
  open_order.order_.type_ = OrderType::LIMIT;
  open_order.order_.price_ = Decimal64::parse("70000");
  open_order.order_.quantity_ = Decimal64::parse("10");
  open_order.order_.status_ = OrderStatus::PENDING;
  return open_order;
}

/** Returns a working order carrying an amendable quantity, as KIS reports one. */
WorkingOrder working_order(std::string_view broker) {
  WorkingOrder working;
  working.ref_ = OrderRef{broker, "0000117057", "00950"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.price_ = Decimal64::parse("70000");
  working.order_.quantity_ = Decimal64::parse("10");
  working.amendable_quantity_ = Decimal64::parse("7");
  return working;
}

} // namespace

TEST_CASE("every supported client satisfies the broker port") {
  ATL_ASSERT_BROKER(kis::KisClient);
  ATL_ASSERT_BROKER(kis::KisBlockingClient);
  ATL_ASSERT_BROKER(tossinvest::TossClient);
  ATL_ASSERT_BROKER(tossinvest::TossBlockingClient);
  ATL_ASSERT_BROKER(mock::MockClient);
  SUCCEED("conformance is checked at compile time");
}

TEST_CASE("capabilities record each broker's real differences") {
  static_assert(!BrokerCaps<kis::KisClient>::has_account_discovery);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_account_discovery);

  static_assert(!BrokerCaps<kis::KisClient>::has_client_order_id);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_client_order_id);

  static_assert(BrokerCaps<kis::KisClient>::has_amendable_quantity);
  static_assert(!BrokerCaps<tossinvest::TossClient>::has_amendable_quantity);

  static_assert(!BrokerCaps<kis::KisClient>::has_order_by_id);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_order_by_id);

  static_assert(BrokerCaps<kis::KisClient>::has_partial_cancel);
  static_assert(!BrokerCaps<tossinvest::TossClient>::has_partial_cancel);

  static_assert(BrokerCaps<kis::KisClient>::history_requires_range);
  static_assert(!BrokerCaps<tossinvest::TossClient>::history_requires_range);

  static_assert(!BrokerCaps<kis::KisClient>::has_history_cursor);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_history_cursor);

  // A blocking facade answers the same capability questions as its async client.
  static_assert(
    BrokerCaps<kis::KisClient>::has_partial_cancel ==
    BrokerCaps<kis::KisBlockingClient>::has_partial_cancel
  );
  static_assert(
    BrokerCaps<tossinvest::TossClient>::has_order_by_id ==
    BrokerCaps<tossinvest::TossBlockingClient>::has_order_by_id
  );
  SUCCEED("capabilities are compile-time constants");
}

TEST_CASE("an order reference is opaque, comparable, and hashable") {
  const OrderRef kis_ref{"kis", "0000117057", "00950"};
  const OrderRef same{"kis", "0000117057", "00950"};
  const OrderRef other_number{"kis", "0000117058", "00950"};
  const OrderRef other_broker{"tossinvest", "0000117057", "00950"};
  const OrderRef toss_ref{"tossinvest", "order-1", ""};

  REQUIRE(kis_ref == same);
  REQUIRE(kis_ref != other_number);
  REQUIRE(kis_ref != other_broker);

  REQUIRE(kis_ref.str() == "kis:0000117057:00950");
  REQUIRE(toss_ref.str() == "tossinvest:order-1");

  std::unordered_set<OrderRef> book;
  book.insert(kis_ref);
  book.insert(same);
  book.insert(other_number);
  book.insert(other_broker);
  REQUIRE(book.size() == 3);
  REQUIRE(book.count(kis_ref) == 1);
}

TEST_CASE("amendment factories carry the broker's own requirements") {
  const auto order = working_order("kis");

  const auto full = cancel_all(order);
  REQUIRE(full.all_remaining_);
  REQUIRE(full.quantity_.has_value());
  REQUIRE(*full.quantity_ == Decimal64::parse("7"));
  REQUIRE(full.shape_.type_ == OrderType::LIMIT);
  REQUIRE(full.shape_.price_.has_value());

  const auto partial = cancel_quantity(order, Decimal64::parse("3"));
  REQUIRE_FALSE(partial.all_remaining_);
  REQUIRE(*partial.quantity_ == Decimal64::parse("3"));

  const auto repriced = replace_with(order, OrderShape{OrderType::LIMIT, Decimal64::parse("71000")});
  REQUIRE(repriced.all_remaining_);
  REQUIRE(*repriced.shape_.price_ == Decimal64::parse("71000"));
  REQUIRE(*repriced.quantity_ == Decimal64::parse("7"));

  const auto resized =
    replace_with(order, OrderShape{OrderType::LIMIT, Decimal64::parse("71000")}, Decimal64::parse("4"));
  REQUIRE_FALSE(resized.all_remaining_);
  REQUIRE(*resized.quantity_ == Decimal64::parse("4"));

  // A broker that reports no amendable quantity yields an instruction without one.
  auto without_quantity = working_order("tossinvest");
  without_quantity.amendable_quantity_.reset();
  REQUIRE_FALSE(cancel_all(without_quantity).quantity_.has_value());
}

TEST_CASE("a working order names the reference an amendment must address") {
  WorkingOrder plain = working_order("kis");
  REQUIRE(plain.amend_reference() == plain.ref_);

  WorkingOrder amended = working_order("kis");
  amended.amend_ref_ = OrderRef{"kis", "0000117000", "00950"};
  REQUIRE(amended.amend_reference() == *amended.amend_ref_);
  REQUIRE(amended.amend_reference() != amended.ref_);
}

TEST_CASE("KIS normalizes an acknowledgement into a reference that can amend it") {
  using Traits = BrokerTraits<kis::KisClient>;

  auto placed = Traits::normalize_place(Result<kis::KisOrderAck>(kis_ack()));
  REQUIRE(placed.has_value());
  REQUIRE(placed.value().ack_.order_id_ == "0000117057");
  REQUIRE(placed.value().ref_.broker_ == "kis");
  REQUIRE(placed.value().ref_.primary_ == "0000117057");
  REQUIRE(placed.value().ref_.secondary_ == "00950");

  // Both KIS parts survive the round trip through the opaque reference.
  auto amendment = Traits::to_replacement(
    placed.value().ref_,
    AmendInstruction{OrderShape{OrderType::LIMIT, Decimal64::parse("70000")}, Decimal64::parse("1"), false, false}
  );
  REQUIRE(amendment.has_value());
  REQUIRE(amendment.value().order_.order_number_ == "0000117057");
  REQUIRE(amendment.value().order_.branch_number_ == "00950");

  BrokerError transport;
  transport.category_ = ErrorCategory::TRANSPORT;
  transport.message_ = "connection closed";
  auto failed = Traits::normalize_place(Result<kis::KisOrderAck>(transport));
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error().category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("Toss normalizes an acknowledgement into a single-part reference") {
  using Traits = BrokerTraits<tossinvest::TossClient>;

  OrderAck ack;
  ack.order_id_ = "order-1";
  ack.client_order_id_ = std::string("key-1");
  auto placed = Traits::normalize_place(Result<OrderAck>(ack));
  REQUIRE(placed.has_value());
  REQUIRE(placed.value().ref_.broker_ == "tossinvest");
  REQUIRE(placed.value().ref_.primary_ == "order-1");
  REQUIRE(placed.value().ref_.secondary_.empty());
  REQUIRE(placed.value().ack_.client_order_id_.has_value());
}

TEST_CASE("KIS working orders preserve both of its order references") {
  using Traits = BrokerTraits<kis::KisClient>;

  kis::KisOpenOrderPage page;
  page.received_at_ = std::chrono::system_clock::now();
  page.orders_.push_back(kis_open_order("0000117057", "0000117057"));
  page.orders_.push_back(kis_open_order("0000117060", "0000117000"));
  page.orders_.push_back(kis_open_order("0000117061", ""));

  auto orders = Traits::normalize_working_orders(Result<kis::KisOpenOrderPage>(page));
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().orders_.size() == 3);

  // An order that has never been amended addresses itself.
  const auto& first = orders.value().orders_[0];
  REQUIRE_FALSE(first.amend_ref_.has_value());
  REQUIRE(first.amend_reference().primary_ == "0000117057");
  REQUIRE(first.amendable_quantity_.has_value());
  REQUIRE(*first.amendable_quantity_ == Decimal64::parse("7"));

  // An amended order addresses the original order number KIS reported.
  const auto& second = orders.value().orders_[1];
  REQUIRE(second.ref_.primary_ == "0000117060");
  REQUIRE(second.amend_ref_.has_value());
  REQUIRE(second.amend_reference().primary_ == "0000117000");

  // An empty original order number is not turned into an unusable reference.
  const auto& third = orders.value().orders_[2];
  REQUIRE_FALSE(third.amend_ref_.has_value());
  REQUIRE(third.amend_reference().primary_ == "0000117061");
}

TEST_CASE("Toss working orders report no amendable quantity rather than zero") {
  using Traits = BrokerTraits<tossinvest::TossClient>;

  OrderPage page;
  page.received_at_ = std::chrono::system_clock::now();
  OrderSnapshot snapshot;
  snapshot.order_id_ = "order-1";
  snapshot.instrument_ = samsung();
  snapshot.quantity_ = Decimal64::parse("10");
  page.orders_.push_back(snapshot);

  auto orders = Traits::normalize_working_orders(Result<OrderPage>(page));
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().orders_.size() == 1);
  REQUIRE(orders.value().orders_[0].ref_.primary_ == "order-1");
  REQUIRE_FALSE(orders.value().orders_[0].amendable_quantity_.has_value());
  REQUIRE_FALSE(orders.value().orders_[0].amend_ref_.has_value());
}

TEST_CASE("an idempotency key is refused by a broker that does not honor one") {
  OrderRequest order;
  order.instrument_ = samsung();
  order.quantity_ = Decimal64::parse("1");
  order.price_ = Decimal64::parse("70000");

  REQUIRE_FALSE(BrokerTraits<kis::KisClient>::validate_place(order).has_value());
  REQUIRE_FALSE(BrokerTraits<tossinvest::TossClient>::validate_place(order).has_value());

  order.client_order_id_ = std::string("key-1");
  const auto refused = BrokerTraits<kis::KisClient>::validate_place(order);
  REQUIRE(refused.has_value());
  REQUIRE(refused->category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused->broker_code_ == "idempotency_key_unsupported");

  // Toss honors the key, so the same order passes.
  REQUIRE_FALSE(BrokerTraits<tossinvest::TossClient>::validate_place(order).has_value());
}

TEST_CASE("KIS refuses an amendment with no quantity instead of inventing one") {
  using Traits = BrokerTraits<kis::KisClient>;
  const OrderRef ref{"kis", "0000117057", "00950"};

  AmendInstruction without_quantity;
  without_quantity.shape_ = OrderShape{OrderType::LIMIT, Decimal64::parse("70000")};
  without_quantity.all_remaining_ = true;

  auto refused = Traits::to_cancellation(ref, without_quantity);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused.error().broker_code_ == "quantity_required");

  auto accepted = Traits::to_cancellation(ref, cancel_all(working_order("kis")));
  REQUIRE(accepted.has_value());
  REQUIRE(accepted.value().all_remaining_);
  REQUIRE(accepted.value().quantity_ == Decimal64::parse("7"));

  auto partial = Traits::to_cancellation(ref, cancel_quantity(working_order("kis"), Decimal64::parse("3")));
  REQUIRE(partial.has_value());
  REQUIRE_FALSE(partial.value().all_remaining_);
  REQUIRE(partial.value().quantity_ == Decimal64::parse("3"));
}

TEST_CASE("Toss refuses a partial cancellation instead of widening it") {
  using Traits = BrokerTraits<tossinvest::TossClient>;
  const OrderRef ref{"tossinvest", "order-1", ""};

  auto order = working_order("tossinvest");
  order.amendable_quantity_.reset();

  auto refused = Traits::to_cancellation(ref, cancel_quantity(order, Decimal64::parse("3")));
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused.error().broker_code_ == "partial_cancel_unsupported");

  auto accepted = Traits::to_cancellation(ref, cancel_all(order));
  REQUIRE(accepted.has_value());
  REQUIRE(accepted.value().order_id_ == "order-1");
}

TEST_CASE("a reference from another broker cannot address an order") {
  const OrderRef toss_ref{"tossinvest", "order-1", ""};
  const OrderRef kis_ref{"kis", "0000117057", "00950"};

  auto kis_refused = BrokerTraits<kis::KisClient>::to_replacement(toss_ref, cancel_all(working_order("kis")));
  REQUIRE_FALSE(kis_refused.has_value());
  REQUIRE(kis_refused.error().broker_code_ == "foreign_order_reference");

  auto toss_refused =
    BrokerTraits<tossinvest::TossClient>::to_cancellation(kis_ref, cancel_all(working_order("tossinvest")));
  REQUIRE_FALSE(toss_refused.has_value());
  REQUIRE(toss_refused.error().broker_code_ == "foreign_order_reference");
}

TEST_CASE("KIS refuses a history filter it cannot express") {
  using Traits = BrokerTraits<kis::KisClient>;
  const kis::KisAccount account{"12345678", "01"};

  OrderHistoryQuery empty;
  auto no_range = Traits::to_history_query(account, empty);
  REQUIRE_FALSE(no_range.has_value());
  REQUIRE(no_range.error().broker_code_ == "range_required");

  OrderHistoryQuery ranged;
  ranged.from_ = Date{2026, 9, 1};
  ranged.to_ = Date{2026, 9, 20};
  auto accepted = Traits::to_history_query(account, ranged);
  REQUIRE(accepted.has_value());
  REQUIRE(accepted.value().from_ == Date{2026, 9, 1});
  REQUIRE(accepted.value().to_ == Date{2026, 9, 20});

  auto with_symbol = ranged;
  with_symbol.symbol_ = std::string("005930");
  REQUIRE(Traits::to_history_query(account, with_symbol).error().broker_code_ == "symbol_filter_unsupported");

  auto with_cursor = ranged;
  with_cursor.cursor_ = std::string("cursor");
  REQUIRE(Traits::to_history_query(account, with_cursor).error().broker_code_ == "cursor_unsupported");

  auto with_limit = ranged;
  with_limit.limit_ = 20;
  REQUIRE(Traits::to_history_query(account, with_limit).error().broker_code_ == "limit_unsupported");
}

TEST_CASE("Toss maps a history query onto its closed listing") {
  using Traits = BrokerTraits<tossinvest::TossClient>;

  OrderHistoryQuery query;
  query.symbol_ = std::string("005930");
  query.cursor_ = std::string("cursor");
  query.limit_ = 50;
  query.from_ = Date{2026, 9, 1};

  auto converted = Traits::to_history_query(77, query);
  REQUIRE(converted.has_value());
  REQUIRE(converted.value().account_sequence_ == 77);
  REQUIRE(converted.value().scope_ == tossinvest::TossOrderScope::CLOSED);
  REQUIRE(converted.value().symbol_ == std::string("005930"));
  REQUIRE(converted.value().cursor_ == std::string("cursor"));
  REQUIRE(converted.value().limit_ == 50);
  REQUIRE(converted.value().from_ == Date{2026, 9, 1});

  // Working orders are the open listing, not a date-filtered history.
  REQUIRE(Traits::open_query(77).scope_ == tossinvest::TossOrderScope::OPEN);
  REQUIRE(Traits::open_query(77).account_sequence_ == 77);
}

TEST_CASE("a port verb completes once, on the caller handler's executor") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  QuoteSnapshot quote;
  quote.instrument_ = samsung();
  quote.last_price_ = Decimal64::parse("72000");
  client->quote_ = quote;

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  int completions = 0;
  bool on_caller_executor = false;
  session.async_quote(
    samsung(),
    net::bind_executor(
      strand,
      [&](Result<QuoteSnapshot> result) {
        ++completions;
        on_caller_executor = strand.running_in_this_thread();
        REQUIRE(result.has_value());
        REQUIRE(result.value().last_price_ == Decimal64::parse("72000"));
      }
    )
  );

  REQUIRE(completions == 0);
  ioc.run();
  REQUIRE(completions == 1);
  REQUIRE(on_caller_executor);
  REQUIRE(client->quote_calls_ == 1);
}

TEST_CASE("a pre-flight rejection never reaches the client and never runs inline") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  client->ack_ = mock::MockAck{"ticket-1", std::chrono::system_clock::now()};

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  OrderRequest order;
  order.instrument_ = samsung();
  order.quantity_ = Decimal64::parse("1");
  order.price_ = Decimal64::parse("70000");
  order.client_order_id_ = std::string("key-1");

  bool completed = false;
  session.async_place(
    order,
    net::bind_executor(
      strand,
      [&](Result<PlacedOrder> result) {
        completed = true;
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
        REQUIRE(result.error().broker_code_ == "idempotency_key_unsupported");
      }
    )
  );

  REQUIRE_FALSE(completed);
  ioc.run();
  REQUIRE(completed);
  REQUIRE(client->place_calls_ == 0);
}

TEST_CASE("a rejection completes without a caller executor or a running loop") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});

  OrderHistoryQuery query;
  query.symbol_ = std::string("005930");
  auto pending = session.async_order_history(query, net::use_future);
  auto result = pending.get();

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().broker_code_ == "symbol_filter_unsupported");
  REQUIRE(client->history_calls_ == 0);
}

TEST_CASE("the port normalizes and plumbs a full order round trip") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  const auto stamped = std::chrono::system_clock::now();
  client->ack_ = mock::MockAck{"ticket-1", stamped};

  mock::MockLiveOrders live;
  live.read_at_ = stamped;
  mock::MockLiveOrder row;
  row.ticket_ = "ticket-1";
  row.open_quantity_ = Decimal64::parse("7");
  row.order_.order_id_ = "ticket-1";
  row.order_.instrument_ = samsung();
  row.order_.type_ = OrderType::LIMIT;
  row.order_.price_ = Decimal64::parse("70000");
  row.order_.quantity_ = Decimal64::parse("10");
  live.rows_.push_back(row);
  client->live_orders_ = live;

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  OrderRequest order;
  order.instrument_ = samsung();
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");

  OrderRef placed_ref{"", "", ""};
  session.async_quote(samsung(), net::bind_executor(strand, [](Result<QuoteSnapshot>) {}));
  session.async_place(
    order,
    net::bind_executor(
      strand,
      [&](Result<PlacedOrder> result) {
        REQUIRE(result.has_value());
        REQUIRE(result.value().ack_.order_id_ == "ticket-1");
        REQUIRE(result.value().ack_.accepted_at_ == stamped);
        placed_ref = result.value().ref_;
      }
    )
  );
  ioc.run();

  REQUIRE(placed_ref.broker_ == "mock");
  REQUIRE(placed_ref.primary_ == "ticket-1");
  REQUIRE(client->place_calls_ == 1);

  WorkingOrders working{{}, {}};
  ioc.restart();
  session.async_working_orders(
    net::bind_executor(
      strand,
      [&](Result<WorkingOrders> result) {
        REQUIRE(result.has_value());
        working = std::move(result.value());
      }
    )
  );
  ioc.run();

  REQUIRE(working.orders_.size() == 1);
  REQUIRE(working.orders_[0].ref_ == placed_ref);
  REQUIRE(working.orders_[0].amendable_quantity_ == Decimal64::parse("7"));

  ioc.restart();
  session.async_cancel(
    working.orders_[0].amend_reference(),
    cancel_all(working.orders_[0]),
    net::bind_executor(strand, [](Result<PlacedOrder> result) { REQUIRE(result.has_value()); })
  );
  ioc.run();

  REQUIRE(client->cancel_calls_ == 1);
  REQUIRE(client->last_amendment_.has_value());
  REQUIRE(client->last_amendment_->ticket_ == "ticket-1");
  REQUIRE(client->last_amendment_->whole_);
  REQUIRE(client->last_amendment_->quantity_ == Decimal64::parse("7"));

  session.close();
  REQUIRE(client->close_calls_ == 1);
}

TEST_CASE("the port passes an expressible history filter through unchanged") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  OrderPage page;
  page.received_at_ = std::chrono::system_clock::now();
  page.next_cursor_ = std::string("next");
  client->history_ = page;

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust-7", 2});
  auto strand = net::make_strand(ioc);

  OrderHistoryQuery query;
  query.cursor_ = std::string("page-2");

  session.async_order_history(
    query,
    net::bind_executor(
      strand,
      [](Result<OrderPage> result) {
        REQUIRE(result.has_value());
        REQUIRE(result.value().next_cursor_ == std::string("next"));
      }
    )
  );
  ioc.run();

  REQUIRE(client->history_calls_ == 1);
  REQUIRE(client->last_selector_.has_value());
  REQUIRE(client->last_selector_->customer_ == "cust-7");
  REQUIRE(client->last_selector_->cursor_ == std::string("page-2"));
}
