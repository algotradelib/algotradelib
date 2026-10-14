#pragma once

#include <algotradelib/broker>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * A broker that exists only in memory, beside the three the library ships.
 *
 * Its purpose is twofold. It proves that binding a new broker to the port needs
 * no edit to any core or existing broker header — everything below lives in this
 * test header, including the `BrokerTraits` specialization. And it gives the port
 * a deterministic, offline broker whose completion behavior can be asserted
 * without a socket.
 *
 * Its vendor types are deliberately *unlike* both real brokers: the account
 * selector is a struct, the acknowledgement is one opaque string with a
 * timestamp, and the amendable-order result is its own row type. A port that can
 * absorb this shape is not quietly modelled on KIS or Toss.
 */
namespace mock {

namespace net = boost::asio;
using algotradelib::BrokerError;
using algotradelib::Decimal64;
using algotradelib::ErrorCategory;
using algotradelib::HoldingsSnapshot;
using algotradelib::InstrumentId;
using algotradelib::OrderPage;
using algotradelib::OrderRequest;
using algotradelib::BarInterval;
using algotradelib::BarQuery;
using algotradelib::BarSeries;
using algotradelib::OrderSnapshot;
using algotradelib::QuoteSnapshot;
using algotradelib::Result;

/** Application configuration for the in-memory broker. */
struct MockConfig {
  std::string label_;
};

/** Account selector for the in-memory broker: a struct, as KIS's is. */
struct MockAccount {
  std::string customer_;
  int slot_{0};
};

/** Acknowledgement shape unique to the in-memory broker. */
struct MockAck {
  std::string ticket_;
  std::chrono::system_clock::time_point stamped_at_;
};

/** Modification and cancellation instruction unique to the in-memory broker. */
struct MockAmendment {
  std::string ticket_;
  bool whole_{true};
  std::optional<Decimal64> quantity_;
  std::optional<Decimal64> price_;
};

/** One amendable order row, with the broker's own field names. */
struct MockLiveOrder {
  OrderSnapshot order_;
  std::string ticket_;
  Decimal64 open_quantity_;
};

/** Amendable-order result shape unique to the in-memory broker. */
struct MockLiveOrders {
  std::vector<MockLiveOrder> rows_;
  std::chrono::system_clock::time_point read_at_;
};

/** Selector for the in-memory broker's order history. */
struct MockHistorySelector {
  std::string customer_;
  std::optional<std::string> cursor_;
};

/**
 * Asynchronous in-memory client with the same completion contract as the real ones.
 *
 * Every operation answers from a scripted value, posts through a strand, and
 * completes through the caller handler's associated executor exactly once, which
 * is what makes it usable to assert the port's own completion behavior.
 */
class MockClient : public std::enable_shared_from_this<MockClient> {
public:
  /** Constructs a client whose work is serialized on a strand over `executor`. */
  explicit MockClient(net::any_io_executor executor)
  : strand_(net::make_strand(std::move(executor))) {}

  /** Scripted quote answer. */
  std::optional<QuoteSnapshot> quote_;

  /** Scripted holdings answer. */
  std::optional<HoldingsSnapshot> holdings_;

  /** Scripted acknowledgement answer for every order-writing operation. */
  std::optional<MockAck> ack_;

  /** Scripted amendable-order answer. */
  std::optional<MockLiveOrders> live_orders_;

  /** Scripted order-history answer. */
  std::optional<OrderPage> history_;

  /** Scripted bar answer. */
  std::optional<BarSeries> bars_;

  /** The last bar query this client was asked to answer. */
  std::optional<BarQuery> last_bar_query_;

  /** Count of wire-equivalent attempts per operation, for exactly-once checks. */
  int quote_calls_{0};
  int holdings_calls_{0};
  int place_calls_{0};
  int replace_calls_{0};
  int cancel_calls_{0};
  int live_order_calls_{0};
  int history_calls_{0};
  int bar_calls_{0};

  /** The last amendment this client was asked to carry out. */
  std::optional<MockAmendment> last_amendment_;

  /** The last history selector this client was asked to use. */
  std::optional<MockHistorySelector> last_selector_;

  /** Answers a quote request. */
  template <typename Token>
  auto async_quote(InstrumentId instrument, Token&& token) {
    static_cast<void>(instrument);
    return start<QuoteSnapshot>(
      token,
      [](MockClient& client) {
        ++client.quote_calls_;
        return client.answer<QuoteSnapshot>(client.quote_, "no quote scripted");
      }
    );
  }

  /** Answers a bar request. */
  template <typename Token>
  auto async_bars(BarQuery query, Token&& token) {
    return start<BarSeries>(
      token,
      [query = std::move(query)](MockClient& client) mutable {
        ++client.bar_calls_;
        client.last_bar_query_ = std::move(query);
        return client.answer<BarSeries>(client.bars_, "no bars scripted");
      }
    );
  }

  /** Answers a holdings request. */
  template <typename Token>
  auto async_holdings(MockAccount account, Token&& token) {
    static_cast<void>(account);
    return start<HoldingsSnapshot>(
      token,
      [](MockClient& client) {
        ++client.holdings_calls_;
        return client.answer<HoldingsSnapshot>(client.holdings_, "no holdings scripted");
      }
    );
  }

  /** Answers an order submission. */
  template <typename Token>
  auto async_place(MockAccount account, OrderRequest order, Token&& token) {
    static_cast<void>(account);
    static_cast<void>(order);
    return start<MockAck>(
      token,
      [](MockClient& client) {
        ++client.place_calls_;
        return client.answer<MockAck>(client.ack_, "no acknowledgement scripted");
      }
    );
  }

  /** Answers an order modification. */
  template <typename Token>
  auto async_replace(MockAccount account, MockAmendment amendment, Token&& token) {
    static_cast<void>(account);
    return start<MockAck>(
      token,
      [amendment = std::move(amendment)](MockClient& client) mutable {
        ++client.replace_calls_;
        client.last_amendment_ = std::move(amendment);
        return client.answer<MockAck>(client.ack_, "no acknowledgement scripted");
      }
    );
  }

  /** Answers an order cancellation. */
  template <typename Token>
  auto async_cancel(MockAccount account, MockAmendment amendment, Token&& token) {
    static_cast<void>(account);
    return start<MockAck>(
      token,
      [amendment = std::move(amendment)](MockClient& client) mutable {
        ++client.cancel_calls_;
        client.last_amendment_ = std::move(amendment);
        return client.answer<MockAck>(client.ack_, "no acknowledgement scripted");
      }
    );
  }

  /** Answers an amendable-order inquiry. */
  template <typename Token>
  auto async_live_orders(MockAccount account, Token&& token) {
    static_cast<void>(account);
    return start<MockLiveOrders>(
      token,
      [](MockClient& client) {
        ++client.live_order_calls_;
        return client.answer<MockLiveOrders>(client.live_orders_, "no live orders scripted");
      }
    );
  }

  /** Answers an order-history inquiry. */
  template <typename Token>
  auto async_history(MockHistorySelector selector, Token&& token) {
    return start<OrderPage>(
      token,
      [selector = std::move(selector)](MockClient& client) mutable {
        ++client.history_calls_;
        client.last_selector_ = std::move(selector);
        return client.answer<OrderPage>(client.history_, "no history scripted");
      }
    );
  }

  /** Releases nothing; present so the port's `close()` has a target. */
  void close() { ++close_calls_; }

  /** Count of `close()` calls, so the port's forwarding can be asserted. */
  int close_calls_{0};

private:
  /** Returns a scripted value, or a protocol error naming what was missing. */
  template <typename T>
  Result<T> answer(std::optional<T>& scripted, const char* missing) {
    if (scripted) {
      return Result<T>(*scripted);
    }
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = "unscripted";
    error.message_ = missing;
    return Result<T>(std::move(error));
  }

  /**
   * Posts `produce` to the strand and completes through the handler's executor.
   *
   * This mirrors what both real clients do, so a test that asserts the port's
   * completion behavior against the mock is asserting the same contract.
   */
  template <typename T, typename Token, typename Produce>
  auto start(Token& token, Produce produce) {
    return net::async_initiate<Token, void(Result<T>)>(
      [self = shared_from_this(), produce = std::move(produce)](auto&& handler) mutable {
        auto completion =
          net::any_completion_handler<void(Result<T>)>(std::forward<decltype(handler)>(handler));
        net::post(
          self->strand_,
          [self, produce = std::move(produce), completion = std::move(completion)]() mutable {
            auto result = produce(*self);
            auto executor = net::get_associated_executor(completion, self->strand_);
            net::dispatch(
              executor,
              [completion = std::move(completion), result = std::move(result)]() mutable {
                std::move(completion)(std::move(result));
              }
            );
          }
        );
      },
      token
    );
  }

  net::strand<net::any_io_executor> strand_;
};

/** Compile-time identity of the in-memory broker. */
struct Broker {
  /** Normalized broker name. */
  static constexpr std::string_view name = "mock";
};

} // namespace mock

namespace algotradelib {

namespace mock_port {

/**
 * The capabilities the in-memory broker answers, apart from its other members so a probe can build an
 * adapter that omits one. `data_only` and `working_orders_state_terminal` are declared in the
 * specialization instead, so a probe inheriting this omits exactly the one it does not restate.
 */
struct Capabilities {
  /** No account listing. */
  static constexpr bool has_account_discovery = false;

  /** No idempotency key. */
  static constexpr bool has_client_order_id = false;

  /** Publishes amendable orders. */
  static constexpr bool has_working_orders = true;

  /** Reports the quantity still open. */
  static constexpr bool has_amendable_quantity = true;

  /** Reads no trigger price, so the session refuses one. */
  static constexpr bool has_trigger_price = false;

  /** No single-order inquiry. */
  static constexpr bool has_order_by_id = false;

  /** Cancels a stated quantity. */
  static constexpr bool has_partial_cancel = true;

  /** Needs no calendar range. */
  static constexpr bool history_requires_range = false;

  /** Exposes a cursor. */
  static constexpr bool has_history_cursor = true;

  /** Honours no row limit: `to_history_query` does not carry one. */
  static constexpr bool has_history_limit = false;

  /** Does not filter by instrument. */
  static constexpr bool has_history_symbol_filter = false;

  /** Answers nothing about spending power. */
  static constexpr bool has_buying_power = false;
  static constexpr bool has_buying_power_quantity = false;

  /** States no sellable figure anywhere. */
  static constexpr SellableSource sellable_source = SellableSource::NONE;

  /** Publishes no commission schedule. */
  static constexpr bool has_commission_schedule = false;

  /** The in-memory broker publishes no market data beyond bars. */
  static constexpr bool has_depth = false;
  static constexpr bool has_price_limits = false;
  static constexpr bool has_trade_prints = false;
  static constexpr bool has_batch_prices = false;
  static constexpr bool has_reference_data = false;
  static constexpr bool has_batch_reference_data = false;
  static constexpr bool has_calendar = false;
  static constexpr bool has_calendar_range = false;
  static constexpr bool has_calendar_sessions = false;
};

/**
 * What the in-memory broker's two adaptations share: the associated types and the pure validators,
 * converters and normalizers. `Members` adds the asynchronous thunks and `BrokerTraits<MockBlockingClient>`
 * the blocking ones, as every shipped broker shares one core between its two clients.
 */
struct Normalizers {
  /** Configuration type. */
  using Config = mock::MockConfig;

  /** Account selector type. */
  using Account = mock::MockAccount;

  /** Compile-time identity of the in-memory broker. */
  using BrokerTag = mock::Broker;

  /** Acknowledgement type. */
  using PlaceAck = mock::MockAck;

  /** Amendment type. */
  using Amendment = mock::MockAmendment;

  /** History selector type. */
  using HistoryQuery = mock::MockHistorySelector;

  /** Amendable-order result type. */
  using WorkingOrdersResult = mock::MockLiveOrders;

  /** Normalized broker name. */
  static constexpr std::string_view name = mock::Broker::name;

  /**
   * The in-memory broker publishes minute and daily bars, like both real ones, and
   * refuses the longer periods only KIS has.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return interval == BarInterval::ONE_MINUTE || interval == BarInterval::ONE_DAY;
  }

  /** Refuses a bar request this broker cannot answer, before any I/O. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "unsupported_interval";
    error.message_ = "the in-memory broker publishes minute and daily bars only";
    return error;
  }


  /** Returns an opaque reference carrying the broker's ticket. */
  static OrderRef<mock::Broker> make_ref(std::string ticket) {
    OrderRef<mock::Broker> ref;
    ref.primary_ = std::move(ticket);
    return ref;
  }

  /**
   * How often the session asked the adapter to validate a placement or convert a modification, so a test
   * can show a session refusal never reached the adapter (`tests/test-session-parity.cpp`).
   */
  static inline int validate_place_calls_ = 0;
  /** See `validate_place_calls_`. */
  static inline int to_replacement_calls_ = 0;

  /** Refuses an idempotency key this broker does not honor. */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    ++validate_place_calls_;
    if (!order.client_order_id_) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "idempotency_key_unsupported";
    error.message_ = "the in-memory broker honors no client order identifier";
    return error;
  }

  /** Normalizes the broker's acknowledgement. */
  static Result<PlacedOrder<mock::Broker>> normalize_place(Result<PlaceAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<mock::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<mock::Broker> placed;
    placed.ack_.order_id_ = raw.value().ticket_;
    placed.ack_.accepted_at_ = raw.value().stamped_at_;
    placed.ref_ = make_ref(raw.value().ticket_);
    return Result<PlacedOrder<mock::Broker>>(std::move(placed));
  }

  /** Normalizes the broker's amendable-order rows. */
  static Result<WorkingOrders<mock::Broker>> normalize_working_orders(
    Result<WorkingOrdersResult> raw
  ) {
    if (!raw) {
      return Result<WorkingOrders<mock::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<mock::Broker> orders;
    orders.received_at_ = raw.value().read_at_;
    orders.orders_.reserve(raw.value().rows_.size());
    for (auto& row : raw.value().rows_) {
      WorkingOrder<mock::Broker> working;
      working.order_ = std::move(row.order_);
      working.ref_ = make_ref(row.ticket_);
      working.amendable_quantity_ = row.open_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<mock::Broker>>(std::move(orders));
  }

  /** Converts a normalized modification, refusing a stated quantity of zero. */
  static Result<Amendment> to_replacement(
    const OrderRef<mock::Broker>& ref,
    const AmendInstruction& instruction
  ) {
    ++to_replacement_calls_;
    if (instruction.quantity_ && *instruction.quantity_ == Decimal64::parse("0")) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "zero_quantity";
      error.message_ = "the in-memory broker takes no modification to a quantity of zero";
      return Result<Amendment>(std::move(error));
    }
    Amendment amendment;
    amendment.ticket_ = ref.primary_;
    amendment.whole_ = instruction.all_remaining_;
    amendment.quantity_ = instruction.quantity_;
    amendment.price_ = instruction.shape_.price_;
    return Result<Amendment>(std::move(amendment));
  }

  /** Converts a normalized cancellation. */
  static Result<Amendment> to_cancellation(
    const OrderRef<mock::Broker>& ref,
    const AmendInstruction& instruction
  ) {
    return to_replacement(ref, instruction);
  }

  /** Converts a normalized history query. */
  static Result<HistoryQuery> to_history_query(
    const Account& account,
    const OrderHistoryQuery& query
  ) {
    if (query.symbol_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "symbol_filter_unsupported";
      error.message_ = "the in-memory broker does not filter history by instrument";
      return Result<HistoryQuery>(std::move(error));
    }
    HistoryQuery selector;
    selector.customer_ = account.customer_;
    selector.cursor_ = query.cursor_;
    return Result<HistoryQuery>(std::move(selector));
  }

};

/**
 * Broker port adaptation for the asynchronous in-memory client, apart from its capabilities.
 *
 * Nothing outside this test header is touched to add it, which is the property
 * being demonstrated.
 */
struct Members : Normalizers {
  /** Concrete client this specialization adapts. */
  using Client = mock::MockClient;

  /** This client completes through a completion token. */
  static constexpr bool blocking = false;

  /** The in-memory broker issues no network requests and is never paced. */
  static const network::RateLimitStats* rate_limit_stats(const Client&) { return nullptr; }

  /** Forwards a bar request. */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a holdings request. */
  template <typename Token>
  static auto holdings(Client& client, const Account& account, Token&& token) {
    return client.async_holdings(account, std::forward<Token>(token));
  }

  /** Forwards an order submission. */
  template <typename Token>
  static auto place_order(Client& client, const Account& account, OrderRequest order, Token&& token) {
    return client.async_place(account, std::move(order), std::forward<Token>(token));
  }

  /** Forwards an order modification. */
  template <typename Token>
  static auto replace_order(Client& client, const Account& account, Amendment amendment, Token&& token) {
    return client.async_replace(account, std::move(amendment), std::forward<Token>(token));
  }

  /** Forwards an order cancellation. */
  template <typename Token>
  static auto cancel_order(Client& client, const Account& account, Amendment amendment, Token&& token) {
    return client.async_cancel(account, std::move(amendment), std::forward<Token>(token));
  }

  /** Forwards an amendable-order inquiry. */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_live_orders(account, std::forward<Token>(token));
  }

  /** Forwards an order-history inquiry. */
  template <typename Token>
  static auto order_history(Client& client, const Account& account, HistoryQuery query, Token&& token) {
    static_cast<void>(account);
    return client.async_history(std::move(query), std::forward<Token>(token));
  }
};

} // namespace mock_port

/** The in-memory client's adaptation: its members and its capabilities. */
template <>
struct BrokerTraits<mock::MockClient> : mock_port::Members, mock_port::Capabilities {
  /** Takes orders. */
  static constexpr bool data_only = false;

  /**
   * Set so the `true` answer is compiled through the public capability, not only detected.
   *
   * Every shipped adapter answers `false`, so without a fixture stating `true` the constant's
   * other branch would never be instantiated, and a capability constant nothing instantiates
   * goes unchecked. Nothing in the library reads it yet; it is a statement
   * a caller gates on.
   */
  static constexpr bool working_orders_state_terminal = true;
};

} // namespace algotradelib

namespace mock {

/**
 * A blocking facade over `MockClient`, built the way every shipped blocking client is: it owns an event loop
 * and drives the asynchronous client on it to completion, once per call (`kis::KisBlockingClient::run`).
 *
 * Exists so one test can drive `BrokerSession` and `BlockingSession` through the same scripted broker and
 * hold them to the same pre-flight and the same results (`tests/test-session-parity.cpp`). The scripted
 * answers and call counters are the inner client's, through `mock()`.
 */
class MockBlockingClient {
public:
  /** Constructs a blocking client over a fresh in-memory client. */
  MockBlockingClient() : client_(std::make_shared<MockClient>(ioc_.get_executor())) {}

  /** The in-memory client underneath: its scripted answers and call counters. */
  MockClient& mock() noexcept { return *client_; }

  /** Answers a quote request. */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return run<QuoteSnapshot>([&](auto handler) { client_->async_quote(std::move(instrument), std::move(handler)); });
  }
  /** Answers a bar request. */
  Result<BarSeries> bars(BarQuery query) {
    return run<BarSeries>([&](auto handler) { client_->async_bars(std::move(query), std::move(handler)); });
  }
  /** Answers a holdings request. */
  Result<HoldingsSnapshot> holdings(const MockAccount& account) {
    return run<HoldingsSnapshot>([&](auto handler) { client_->async_holdings(account, std::move(handler)); });
  }
  /** Answers an order submission. */
  Result<MockAck> place(const MockAccount& account, OrderRequest order) {
    return run<MockAck>([&](auto handler) { client_->async_place(account, std::move(order), std::move(handler)); });
  }
  /** Answers an order modification. */
  Result<MockAck> replace(const MockAccount& account, MockAmendment amendment) {
    return run<MockAck>([&](auto handler) { client_->async_replace(account, std::move(amendment), std::move(handler)); });
  }
  /** Answers an order cancellation. */
  Result<MockAck> cancel(const MockAccount& account, MockAmendment amendment) {
    return run<MockAck>([&](auto handler) { client_->async_cancel(account, std::move(amendment), std::move(handler)); });
  }
  /** Answers an amendable-order inquiry. */
  Result<MockLiveOrders> live_orders(const MockAccount& account) {
    return run<MockLiveOrders>([&](auto handler) { client_->async_live_orders(account, std::move(handler)); });
  }
  /** Answers an order-history inquiry. */
  Result<OrderPage> history(MockHistorySelector selector) {
    return run<OrderPage>([&](auto handler) { client_->async_history(std::move(selector), std::move(handler)); });
  }

private:
  /** Drives one asynchronous operation to completion on the private loop. */
  template <typename T, typename Start>
  Result<T> run(Start start) {
    std::optional<Result<T>> result;
    ioc_.restart();
    start([&result](Result<T> value) { result.emplace(std::move(value)); });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "event loop stopped before completion";
      return Result<T>(std::move(error));
    }
    return std::move(*result);
  }

  net::io_context ioc_;
  std::shared_ptr<MockClient> client_;
};

} // namespace mock

namespace algotradelib {

/** The blocking in-memory client's adaptation: the shared core, the capabilities, and blocking thunks. */
template <>
struct BrokerTraits<mock::MockBlockingClient> : mock_port::Normalizers, mock_port::Capabilities {
  /** Concrete client this specialization adapts. */
  using Client = mock::MockBlockingClient;
  /** This client returns each result directly. */
  static constexpr bool blocking = true;
  /** Takes orders. */
  static constexpr bool data_only = false;
  /** As the asynchronous mock's. */
  static constexpr bool working_orders_state_terminal = true;

  /** Never paced. */
  static const network::RateLimitStats* rate_limit_stats(const Client&) { return nullptr; }
  /** Forwards a quote request. */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) { return client.quote(std::move(instrument)); }
  /** Forwards a bar request. */
  static Result<BarSeries> bars(Client& client, BarQuery query) { return client.bars(std::move(query)); }
  /** Forwards a holdings request. */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) { return client.holdings(account); }
  /** Forwards an order submission. */
  static Result<PlaceAck> place_order(Client& client, const Account& account, OrderRequest order) {
    return client.place(account, std::move(order));
  }
  /** Forwards an order modification. */
  static Result<PlaceAck> replace_order(Client& client, const Account& account, Amendment amendment) {
    return client.replace(account, std::move(amendment));
  }
  /** Forwards an order cancellation. */
  static Result<PlaceAck> cancel_order(Client& client, const Account& account, Amendment amendment) {
    return client.cancel(account, std::move(amendment));
  }
  /** Forwards an amendable-order inquiry. */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.live_orders(account);
  }
  /** Forwards an order-history inquiry. */
  static Result<OrderPage> order_history(Client& client, const Account& account, HistoryQuery query) {
    static_cast<void>(account);
    return client.history(std::move(query));
  }
};

} // namespace algotradelib
