#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include "mock_broker.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
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
  open_order.order_ref_ = kis::KisOrderRef{"06010", std::move(order_number)};
  open_order.original_order_ = kis::KisOrderRef{"06010", std::move(original_number)};
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
template <typename Broker>
WorkingOrder<Broker> working_order() {
  WorkingOrder<Broker> working;
  working.ref_ = OrderRef<Broker>{"0000117057", "00950"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.price_ = Decimal64::parse("70000");
  working.order_.quantity_ = Decimal64::parse("10");
  working.amendable_quantity_ = Decimal64::parse("7");
  return working;
}

/** Completion handler accepting any signature, for compile-time probes only. */
struct AnySink {
  /** Accepts and discards any completion argument. */
  template <typename T>
  void operator()(T) const {}
};

/** Detects whether `Session` accepts a cancellation addressed by `Ref`. */
template <typename Session, typename Ref, typename = void>
struct can_cancel : std::false_type {};

template <typename Session, typename Ref>
struct can_cancel<
  Session,
  Ref,
  std::void_t<
    decltype(
      std::declval<Session&>().async_cancel(
        std::declval<Ref>(), std::declval<AmendInstruction>(), AnySink{}
      )
    )
  >
> : std::true_type {};

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
  // Both answer what may be spent; only KIS prices a quantity as well.
  static_assert(BrokerCaps<kis::KisClient>::has_buying_power);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_buying_power);
  static_assert(BrokerCaps<kis::KisClient>::has_buying_power_quantity);
  static_assert(!BrokerCaps<tossinvest::TossClient>::has_buying_power_quantity);

  // Only one publishes a commission schedule, and the absent one is absent rather than empty.
  static_assert(BrokerCaps<tossinvest::TossClient>::has_commission_schedule);
  static_assert(!BrokerCaps<kis::KisClient>::has_commission_schedule);

  // Where the sellable figure comes from, which is what a runner branches on: free at KIS, one
  // request per sold instrument at Toss. This deliberately says nothing about which endpoints the
  // brokers publish -- KIS publishes a dedicated inquiry this library does not use.
  static_assert(BrokerCaps<kis::KisClient>::sellable_source == SellableSource::HOLDINGS);
  static_assert(BrokerCaps<tossinvest::TossClient>::sellable_source == SellableSource::INQUIRY);

  static_assert(
    BrokerCaps<kis::KisClient>::sellable_source ==
    BrokerCaps<kis::KisBlockingClient>::sellable_source
  );
  static_assert(
    BrokerCaps<tossinvest::TossClient>::has_commission_schedule ==
    BrokerCaps<tossinvest::TossBlockingClient>::has_commission_schedule
  );
  SUCCEED("capabilities are compile-time constants");
}

TEST_CASE("a strategy gates account-capacity calls on the capability rather than on a try") {
  // What the gates are for: one source compiles against both brokers, and the branch the broker
  // cannot serve is never instantiated. Removing an `if constexpr` here is a compile error naming
  // the broker and the verb, not a run-time surprise at four in the afternoon.
  const auto commission_rate = [](auto& session) -> std::optional<Decimal64> {
    using Client = typename std::remove_reference_t<decltype(session)>::Client;
    if constexpr (BrokerCaps<Client>::has_commission_schedule) {
      auto schedule = session.commissions();
      if (!schedule) {
        return std::nullopt;
      }
      const auto rate = schedule.value().rate_for(Country::KR, Date{2026, 9, 23});
      return rate ? std::optional<Decimal64>(rate->rate_) : std::nullopt;
    } else {
      // Nothing to ask, and nothing invented: a caller here states its own cost model.
      static_cast<void>(session);
      return std::nullopt;
    }
  };
  static_cast<void>(commission_rate);

  const auto sellable_costs_a_request = [](auto tag) {
    using Client = typename decltype(tag)::type;
    return BrokerCaps<Client>::sellable_source == SellableSource::INQUIRY;
  };
  REQUIRE_FALSE(sellable_costs_a_request(std::common_type<kis::KisBlockingClient>{}));
  REQUIRE(sellable_costs_a_request(std::common_type<tossinvest::TossBlockingClient>{}));
}

TEST_CASE("a broker's two clients share one order-reference type") {
  // A reference read through the blocking facade must be usable with the
  // asynchronous client of the same broker, and with no other broker.
  static_assert(
    std::is_same_v<
      BrokerSession<kis::KisClient>::Ref,
      BlockingSession<kis::KisBlockingClient>::Ref
    >
  );
  static_assert(
    std::is_same_v<
      BrokerSession<tossinvest::TossClient>::Ref,
      BlockingSession<tossinvest::TossBlockingClient>::Ref
    >
  );
  static_assert(
    !std::is_same_v<
      BrokerSession<kis::KisClient>::Ref,
      BrokerSession<tossinvest::TossClient>::Ref
    >
  );
  SUCCEED("reference identity is a compile-time property");
}

TEST_CASE("a reference from another broker cannot be compiled into a cancellation") {
  static_assert(can_cancel<BrokerSession<kis::KisClient>, OrderRef<kis::Broker>>::value);
  static_assert(
    can_cancel<BrokerSession<tossinvest::TossClient>, OrderRef<tossinvest::Broker>>::value
  );

  // The broker lives in the reference's type, so mixing brokers is ill-formed
  // rather than a run-time rejection, and costs nothing to enforce.
  static_assert(!can_cancel<BrokerSession<kis::KisClient>, OrderRef<tossinvest::Broker>>::value);
  static_assert(!can_cancel<BrokerSession<tossinvest::TossClient>, OrderRef<kis::Broker>>::value);
  static_assert(!can_cancel<BrokerSession<kis::KisClient>, OrderRef<mock::Broker>>::value);
  SUCCEED("broker mismatch is rejected at compile time");
}

TEST_CASE("an order reference is opaque, comparable, and hashable") {
  using KisRef = OrderRef<kis::Broker>;
  const KisRef reference{"0000117057", "00950"};
  const KisRef same{"0000117057", "00950"};
  const KisRef other_number{"0000117058", "00950"};
  const OrderRef<tossinvest::Broker> toss_ref{"order-1", ""};

  REQUIRE(reference == same);
  REQUIRE(reference != other_number);

  // The broker name is available without being stored in the value.
  static_assert(KisRef::broker() == std::string_view("kis"));
  static_assert(OrderRef<tossinvest::Broker>::broker() == std::string_view("tossinvest"));
  REQUIRE(reference.str() == "kis:0000117057:00950");
  REQUIRE(toss_ref.str() == "tossinvest:order-1");

  std::unordered_set<KisRef> book;
  book.insert(reference);
  book.insert(same);
  book.insert(other_number);
  REQUIRE(book.size() == 2);
  REQUIRE(book.count(reference) == 1);
}

TEST_CASE("amendment factories carry the broker's own requirements") {
  const auto order = working_order<kis::Broker>();

  const auto full = cancel_all(order);
  REQUIRE(full.all_remaining_);
  REQUIRE(full.quantity_.has_value());
  REQUIRE(*full.quantity_ == Decimal64::parse("7"));
  REQUIRE(full.shape_.type_ == OrderType::LIMIT);
  REQUIRE(full.shape_.price_.has_value());

  const auto partial = cancel_quantity(order, Decimal64::parse("3"));
  REQUIRE_FALSE(partial.all_remaining_);
  REQUIRE(*partial.quantity_ == Decimal64::parse("3"));

  const auto repriced =
    replace_with(order, OrderShape{OrderType::LIMIT, Decimal64::parse("71000")});
  REQUIRE(repriced.all_remaining_);
  REQUIRE(*repriced.shape_.price_ == Decimal64::parse("71000"));
  REQUIRE(*repriced.quantity_ == Decimal64::parse("7"));

  const auto resized = replace_with(
    order, OrderShape{OrderType::LIMIT, Decimal64::parse("71000")}, Decimal64::parse("4")
  );
  REQUIRE_FALSE(resized.all_remaining_);
  REQUIRE(*resized.quantity_ == Decimal64::parse("4"));

  // A broker that reports no amendable quantity yields an instruction without one.
  auto without_quantity = working_order<tossinvest::Broker>();
  without_quantity.amendable_quantity_.reset();
  REQUIRE_FALSE(cancel_all(without_quantity).quantity_.has_value());
}

TEST_CASE("KIS normalizes an acknowledgement into a reference that can amend it") {
  using Traits = BrokerTraits<kis::KisClient>;

  auto placed = Traits::normalize_place(Result<kis::KisOrderAck>(kis_ack()));
  REQUIRE(placed.has_value());
  REQUIRE(placed.value().ack_.order_id_ == "0000117057");
  REQUIRE(placed.value().ref_.primary_ == "0000117057");
  REQUIRE(placed.value().ref_.secondary_ == "00950");

  // Both KIS parts survive the round trip through the opaque reference.
  auto amendment = Traits::to_replacement(
    placed.value().ref_,
    AmendInstruction{
      OrderShape{OrderType::LIMIT, Decimal64::parse("70000")},
      Venue::KRX,
      Decimal64::parse("1"),
      false,
      false,
    }
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
  REQUIRE(placed.value().ref_.primary_ == "order-1");
  REQUIRE(placed.value().ref_.secondary_.empty());
  REQUIRE(placed.value().ack_.client_order_id_.has_value());
}

TEST_CASE("a KIS amendment addresses the live order, never the superseded one") {
  using Traits = BrokerTraits<kis::KisClient>;

  // KIS documents the inquiry's `odno` as the order number assigned when the
  // order was placed, and `orgn_odno` as "정정/취소주문 인경우 원주문번호" — present
  // only when the row is itself a correction, naming the order that correction
  // replaced. These values are the inquiry's own documented response example: the
  // live amendable order is 0001569139 and 0001569136 is already superseded.
  kis::KisOpenOrderPage page;
  page.received_at_ = std::chrono::system_clock::now();
  page.orders_.push_back(kis_open_order("0001569139", "0001569136"));
  page.orders_.push_back(kis_open_order("0001569138", ""));

  auto orders = Traits::normalize_working_orders(Result<kis::KisOpenOrderPage>(page));
  REQUIRE(orders.has_value());
  REQUIRE(orders.value().orders_.size() == 2);

  const auto& corrected = orders.value().orders_[0];
  REQUIRE(corrected.ref_.primary_ == "0001569139");
  REQUIRE(corrected.ref_.primary_ != "0001569136");
  REQUIRE(corrected.amendable_quantity_.has_value());
  REQUIRE(*corrected.amendable_quantity_ == Decimal64::parse("7"));

  // The amendment KIS receives must carry the live order number in ORGN_ODNO.
  auto amendment = Traits::to_cancellation(corrected.ref_, cancel_all(corrected));
  REQUIRE(amendment.has_value());
  REQUIRE(amendment.value().order_.order_number_ == "0001569139");

  const auto& plain = orders.value().orders_[1];
  REQUIRE(plain.ref_.primary_ == "0001569138");
  auto plain_amendment = Traits::to_cancellation(plain.ref_, cancel_all(plain));
  REQUIRE(plain_amendment.value().order_.order_number_ == "0001569138");
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
  const OrderRef<kis::Broker> reference{"0000117057", "00950"};

  AmendInstruction without_quantity;
  without_quantity.shape_ = OrderShape{OrderType::LIMIT, Decimal64::parse("70000")};
  without_quantity.all_remaining_ = true;

  auto refused = Traits::to_cancellation(reference, without_quantity);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused.error().broker_code_ == "quantity_required");

  auto accepted = Traits::to_cancellation(reference, cancel_all(working_order<kis::Broker>()));
  REQUIRE(accepted.has_value());
  REQUIRE(accepted.value().all_remaining_);
  REQUIRE(accepted.value().quantity_ == Decimal64::parse("7"));

  auto partial = Traits::to_cancellation(
    reference, cancel_quantity(working_order<kis::Broker>(), Decimal64::parse("3"))
  );
  REQUIRE(partial.has_value());
  REQUIRE_FALSE(partial.value().all_remaining_);
  REQUIRE(partial.value().quantity_ == Decimal64::parse("3"));
}

TEST_CASE("Toss refuses a partial cancellation instead of widening it") {
  using Traits = BrokerTraits<tossinvest::TossClient>;
  const OrderRef<tossinvest::Broker> reference{"order-1", ""};

  auto order = working_order<tossinvest::Broker>();
  order.amendable_quantity_.reset();

  auto refused = Traits::to_cancellation(reference, cancel_quantity(order, Decimal64::parse("3")));
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused.error().broker_code_ == "partial_cancel_unsupported");

  auto accepted = Traits::to_cancellation(reference, cancel_all(order));
  REQUIRE(accepted.has_value());
  REQUIRE(accepted.value().order_id_ == "order-1");
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
  REQUIRE(
    Traits::to_history_query(account, with_symbol).error().broker_code_ ==
    "symbol_filter_unsupported"
  );

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
      [&](Result<PlacedOrder<mock::Broker>> result) {
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

  using Session = BrokerSession<mock::MockClient>;
  Session session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  OrderRequest order;
  order.instrument_ = samsung();
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");

  Session::Ref placed_ref;
  session.async_quote(samsung(), net::bind_executor(strand, [](Result<QuoteSnapshot>) {}));
  session.async_place(
    order,
    net::bind_executor(
      strand,
      [&](Result<Session::Placed> result) {
        REQUIRE(result.has_value());
        REQUIRE(result.value().ack_.order_id_ == "ticket-1");
        REQUIRE(result.value().ack_.accepted_at_ == stamped);
        placed_ref = result.value().ref_;
      }
    )
  );
  ioc.run();

  REQUIRE(placed_ref.primary_ == "ticket-1");
  REQUIRE(placed_ref.str() == "mock:ticket-1");
  REQUIRE(client->place_calls_ == 1);

  Session::WorkingSet working;
  ioc.restart();
  session.async_working_orders(
    net::bind_executor(
      strand,
      [&](Result<Session::WorkingSet> result) {
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
    working.orders_[0].ref_,
    cancel_all(working.orders_[0]),
    net::bind_executor(strand, [](Result<Session::Placed> result) { REQUIRE(result.has_value()); })
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

TEST_CASE("bar intervals are a per-interval capability, not one flag") {
  // Both brokers publish minute and daily bars; only KIS publishes the longer
  // periods, so a strategy wanting weekly bars knowingly binds itself to KIS.
  static_assert(BrokerCaps<kis::KisClient>::supports_interval(BarInterval::ONE_MINUTE));
  static_assert(BrokerCaps<kis::KisClient>::supports_interval(BarInterval::ONE_DAY));
  static_assert(BrokerCaps<kis::KisClient>::supports_interval(BarInterval::ONE_WEEK));
  static_assert(BrokerCaps<kis::KisClient>::supports_interval(BarInterval::ONE_YEAR));

  static_assert(BrokerCaps<tossinvest::TossClient>::supports_interval(BarInterval::ONE_MINUTE));
  static_assert(BrokerCaps<tossinvest::TossClient>::supports_interval(BarInterval::ONE_DAY));
  static_assert(!BrokerCaps<tossinvest::TossClient>::supports_interval(BarInterval::ONE_WEEK));
  static_assert(!BrokerCaps<tossinvest::TossClient>::supports_interval(BarInterval::ONE_MONTH));

  // A blocking facade answers as its asynchronous client does.
  static_assert(
    BrokerCaps<tossinvest::TossBlockingClient>::supports_interval(BarInterval::ONE_WEEK) ==
    BrokerCaps<tossinvest::TossClient>::supports_interval(BarInterval::ONE_WEEK)
  );
  SUCCEED("interval support is a compile-time predicate");
}

TEST_CASE("an interval a broker does not publish is refused before any request") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  BarSeries scripted;
  scripted.instrument_ = samsung();
  client->bars_ = scripted;

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  BarQuery weekly;
  weekly.instrument_ = samsung();
  weekly.interval_ = BarInterval::ONE_WEEK;

  bool completed = false;
  session.async_bars(
    weekly,
    net::bind_executor(
      strand,
      [&](Result<BarSeries> result) {
        completed = true;
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
        REQUIRE(result.error().broker_code_ == "unsupported_interval");
      }
    )
  );

  REQUIRE_FALSE(completed);
  ioc.run();
  REQUIRE(completed);

  // Refused before the wire: resampling a shorter interval would have answered a
  // different question from the one asked.
  REQUIRE(client->bar_calls_ == 0);
}

TEST_CASE("a supported interval reaches the client with the query intact") {
  net::io_context ioc;
  auto client = std::make_shared<mock::MockClient>(ioc.get_executor());
  BarSeries scripted;
  scripted.instrument_ = samsung();
  scripted.interval_ = BarInterval::ONE_DAY;
  scripted.received_at_ = std::chrono::system_clock::now();
  client->bars_ = scripted;

  BrokerSession<mock::MockClient> session(client, mock::MockAccount{"cust", 1});
  auto strand = net::make_strand(ioc);

  BarQuery daily;
  daily.instrument_ = samsung();
  daily.interval_ = BarInterval::ONE_DAY;
  daily.count_ = 250;
  daily.adjusted_ = false;

  session.async_bars(
    daily,
    net::bind_executor(
      strand,
      [](Result<BarSeries> result) {
        REQUIRE(result.has_value());
        REQUIRE(result.value().interval_ == BarInterval::ONE_DAY);
      }
    )
  );
  ioc.run();

  REQUIRE(client->bar_calls_ == 1);
  REQUIRE(client->last_bar_query_.has_value());
  REQUIRE(client->last_bar_query_->count_ == 250);
  REQUIRE_FALSE(client->last_bar_query_->adjusted_);
  REQUIRE(client->last_bar_query_->instrument_.symbol_ == "005930");
}

TEST_CASE("an order book verb exists only on a broker that publishes one") {
  static_assert(BrokerCaps<kis::KisClient>::has_depth);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_depth);
  static_assert(BrokerCaps<kis::KisBlockingClient>::has_depth);
  static_assert(!BrokerCaps<mock::MockClient>::has_depth);

  // Unlike a reference from the wrong broker, this one cannot be probed with the
  // detection idiom. A foreign reference fails overload resolution, which is
  // SFINAE-friendly; an unsupported verb exists as a member and fails a
  // `static_assert` inside its body, which is a hard error by design — the message
  // naming the verb and the capability is worth more to whoever hits it than
  // detectability is to this test. The diagnostic itself is verified by hand and
  // recorded in the pull-request report.
  SUCCEED("order book support is a compile-time capability");
}

TEST_CASE("market-data capabilities record what each broker actually offers") {
  // Both publish a book, price bounds, and recent executions.
  static_assert(BrokerCaps<kis::KisClient>::has_price_limits);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_price_limits);
  static_assert(BrokerCaps<kis::KisClient>::has_trade_prints);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_trade_prints);

  // Only Toss quotes a list in one request. Looping single quotes instead would
  // wear the same signature with several times the request cost, so the absence is
  // a compile error rather than an emulation.
  static_assert(!BrokerCaps<kis::KisClient>::has_batch_prices);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_batch_prices);
  static_assert(!BrokerCaps<kis::KisBlockingClient>::has_batch_prices);
  static_assert(BrokerCaps<tossinvest::TossBlockingClient>::has_batch_prices);

  // The in-memory broker offers only bars, which is what keeps the gates honest.
  static_assert(!BrokerCaps<mock::MockClient>::has_price_limits);
  static_assert(!BrokerCaps<mock::MockClient>::has_trade_prints);
  static_assert(!BrokerCaps<mock::MockClient>::has_batch_prices);
  SUCCEED("market-data support is a compile-time capability");
}

TEST_CASE("calendar capabilities record how much one request answers") {
  static_assert(BrokerCaps<kis::KisClient>::has_calendar);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_calendar);

  // KIS answers from a base date onwards; Toss answers with the requested day and
  // the business day either side. A caller needing a month in one call can have it
  // from one broker and not the other.
  static_assert(BrokerCaps<kis::KisClient>::has_calendar_range);
  static_assert(!BrokerCaps<tossinvest::TossClient>::has_calendar_range);

  // Session windows are Toss's alone and are not normalized, so this capability
  // points at `session.client()` rather than gating a port verb.
  static_assert(!BrokerCaps<kis::KisClient>::has_calendar_sessions);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_calendar_sessions);

  static_assert(!BrokerCaps<mock::MockClient>::has_calendar);
  SUCCEED("calendar reach is a compile-time capability");
}

TEST_CASE("KIS refuses a calendar for a market it does not publish") {
  // Its holiday inquiry is the domestic market's, so another country is refused
  // rather than answered with Korea's dates.
  const auto refused = BrokerTraits<kis::KisClient>::validate_calendar(Country::US, std::nullopt);
  REQUIRE(refused.has_value());
  REQUIRE(refused->category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(refused->broker_code_ == "unsupported_market");

  REQUIRE_FALSE(
    BrokerTraits<kis::KisClient>::validate_calendar(Country::KR, std::nullopt).has_value()
  );

  // Toss publishes both markets it serves.
  REQUIRE_FALSE(
    BrokerTraits<tossinvest::TossClient>::validate_calendar(Country::US, std::nullopt).has_value()
  );

  // An impossible date is refused by both, before any request.
  REQUIRE(
    BrokerTraits<kis::KisClient>::validate_calendar(Country::KR, Date{2026, 2, 30}).has_value()
  );
  REQUIRE(
    BrokerTraits<tossinvest::TossClient>::validate_calendar(Country::KR, Date{2026, 2, 30})
      .has_value()
  );
}

TEST_CASE("reference data is answered one instrument at a time by both brokers") {
  static_assert(BrokerCaps<kis::KisClient>::has_reference_data);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_reference_data);
  static_assert(!BrokerCaps<mock::MockClient>::has_reference_data);

  // Toss answers for up to two hundred symbols and KIS for one, so the port asks for
  // one — which both can do — and a caller building a universe goes through
  // `session.client()` rather than having the verb loop on its behalf.
  static_assert(!BrokerCaps<kis::KisClient>::has_batch_reference_data);
  static_assert(BrokerCaps<tossinvest::TossClient>::has_batch_reference_data);
  SUCCEED("reference-data reach is a compile-time capability");
}
