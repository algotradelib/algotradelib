#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace algotradelib;

namespace algotradelib::kis {
struct KisClientTestAccess {
  static Result<QuoteSnapshot> parse(
    InstrumentId instrument,
    Result<network::HttpResponse> response
  ) {
    return KisClient::parse_quote(std::move(instrument), std::move(response));
  }

  static Result<HoldingsSnapshot> parse_holdings(
    Result<network::HttpResponse> response
  ) {
    return KisClient::parse_holdings(std::move(response));
  }

  static Result<BuyingPower> parse_buying_power(Result<network::HttpResponse> response) {
    return KisClient::parse_buying_power(std::move(response));
  }

  static network::HttpRequest buying_power_request(
    const KisConfig& config,
    std::string_view access_token,
    const KisAccount& account
  ) {
    return KisClient::buying_power_request(config, access_token, account);
  }

  static Result<HoldingsSnapshot> parse_holdings_page(
    Result<network::HttpResponse> response,
    std::string& foreign_cursor,
    std::string& domestic_cursor
  ) {
    return KisClient::parse_holdings_page(
      std::move(response), foreign_cursor, domestic_cursor
    );
  }

  static network::HttpRequest holdings_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view foreign_cursor,
    std::string_view domestic_cursor,
    bool continuation
  ) {
    return KisClient::make_holdings_request(
      account, config, "access-token", foreign_cursor, domestic_cursor, continuation
    );
  }

  static std::optional<BrokerError> append_holdings_page(
    HoldingsSnapshot& snapshot,
    std::size_t& page_count,
    std::string& foreign_cursor,
    std::string& domestic_cursor,
    HoldingsSnapshot page,
    std::string next_foreign_cursor,
    std::string next_domestic_cursor,
    bool has_next
  ) {
    return KisClient::append_holdings_page(
      snapshot,
      page_count,
      foreign_cursor,
      domestic_cursor,
      std::move(page),
      std::move(next_foreign_cursor),
      std::move(next_domestic_cursor),
      has_next
    );
  }
  static network::HttpRequest place_order_request(
    const KisAccount& account,
    const KisConfig& config,
    const OrderRequest& order,
    std::string_view division
  ) {
    return KisClient::make_place_order_request(
      account, config, "access-token", order, division
    );
  }

  static network::HttpRequest amend_order_request(
    const KisAccount& account,
    const KisConfig& config,
    const KisAmendment& amendment,
    bool cancel,
    std::string_view division
  ) {
    return KisClient::make_amend_order_request(
      account, config, "access-token", amendment, cancel, division
    );
  }

  static network::HttpRequest open_orders_request(
    const KisAccount& account,
    const KisConfig& config,
    std::string_view foreign_cursor,
    std::string_view domestic_cursor,
    bool continuation
  ) {
    return KisClient::make_open_orders_request(
      account, config, "access-token", foreign_cursor, domestic_cursor, continuation
    );
  }

  static network::HttpRequest order_history_request(
    const KisAccount& account,
    const KisConfig& config,
    const KisHistoryRange& range
  ) {
    return KisClient::make_order_history_request(
      account, config, "access-token", range, "", "", false
    );
  }

  static std::optional<BrokerError> validate_order(
    const OrderRequest& order,
    std::string& division
  ) {
    return KisClient::validate_order(order, division);
  }

  static void apply_order_shape(std::string_view division, OrderSnapshot& order) {
    KisClient::apply_order_shape(division, order);
  }

  static std::optional<BrokerError> amendment_division(
    const KisAmendment& amendment,
    std::string& division
  ) {
    return KisClient::order_division(
      amendment.type_, amendment.time_in_force_, amendment.session_, amendment.venue_, division
    );
  }

  static bool valid_order_reference(const KisOrderRef& order) {
    return KisClient::valid_order_reference(order);
  }

  static std::optional<BrokerError> parse_bars_page(
    Result<network::HttpResponse> response,
    const BarQuery& query,
    bool minutes,
    std::vector<Bar>& into
  ) {
    return KisClient::parse_bars_page(std::move(response), query, minutes, into);
  }

  static constexpr int shortest_days_per_bar(BarInterval interval) {
    return KisClient::shortest_days_per_bar(interval);
  }

  static Result<DepthSnapshot> parse_depth(
    InstrumentId instrument,
    Result<network::HttpResponse> response
  ) {
    return KisClient::parse_depth(std::move(instrument), std::move(response));
  }

  static Result<PriceLimits> parse_price_limits(Result<network::HttpResponse> response) {
    return KisClient::parse_price_limits(InstrumentId{}, std::move(response));
  }

  static Result<std::vector<TradePrint>> parse_trades(
    Result<network::HttpResponse> response,
    int count
  ) {
    return KisClient::parse_trades(InstrumentId{}, std::move(response), count);
  }

  static Result<Instrument> parse_instrument(
    InstrumentId requested,
    Result<network::HttpResponse> response
  ) {
    return KisClient::parse_instrument(std::move(requested), std::move(response));
  }

  static std::optional<BrokerError> parse_calendar_page(
    Result<network::HttpResponse> response,
    TradingCalendar& calendar,
    std::string& search_cursor,
    std::string& next_cursor
  ) {
    return KisClient::parse_calendar_page(
      std::move(response), calendar, search_cursor, next_cursor
    );
  }

  static Result<KisOrderAck> parse_order_ack(Result<network::HttpResponse> response) {
    return KisClient::parse_order_ack(std::move(response));
  }

  static Result<KisOpenOrderPage> parse_open_orders(Result<network::HttpResponse> response) {
    std::string foreign_cursor;
    std::string domestic_cursor;
    return KisClient::parse_open_orders_page(
      std::move(response), foreign_cursor, domestic_cursor
    );
  }

  static Result<OrderPage> parse_order_history(Result<network::HttpResponse> response) {
    std::string foreign_cursor;
    std::string domestic_cursor;
    return KisClient::parse_order_history_page(
      std::move(response), foreign_cursor, domestic_cursor
    );
  }

  static OrderStatus derive_order_status(
    const Decimal64& ordered,
    const Decimal64& filled,
    const Decimal64& remaining,
    const Decimal64& rejected,
    bool canceled
  ) {
    return KisClient::derive_order_status(ordered, filled, remaining, rejected, canceled);
  }
};
} // namespace algotradelib::kis

namespace algotradelib::tossinvest {
struct TossClientTestAccess {
  static Result<QuoteSnapshot> parse(
    InstrumentId instrument,
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_quote(std::move(instrument), std::move(response));
  }

  static Result<std::vector<TossAccount>> parse_accounts(
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_accounts(std::move(response));
  }

  static Result<HoldingsSnapshot> parse_holdings(
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_holdings(std::move(response));
  }

  static Result<BuyingPower> parse_buying_power(Result<network::HttpResponse> response) {
    return TossClient::parse_buying_power(std::move(response));
  }

  static Result<Decimal64> parse_sellable_quantity(Result<network::HttpResponse> response) {
    return TossClient::parse_sellable_quantity(std::move(response));
  }

  static Result<CommissionSchedule> parse_commissions(Result<network::HttpResponse> response) {
    return TossClient::parse_commissions(std::move(response));
  }
  static network::HttpRequest place_order_request(
    std::int64_t account_sequence,
    const OrderRequest& order
  ) {
    return TossClient::make_place_order_request(account_sequence, "access-token", order);
  }

  static network::HttpRequest replace_order_request(
    std::int64_t account_sequence,
    const TossAmendment& amendment
  ) {
    return TossClient::make_replace_order_request(account_sequence, "access-token", amendment);
  }

  static std::optional<BrokerError> validate_amendment_shape(OrderType type) {
    return TossClient::validate_amendment_shape(type);
  }

  static network::HttpRequest cancel_order_request(
    std::int64_t account_sequence,
    std::string_view order_id
  ) {
    return TossClient::make_cancel_order_request(account_sequence, "access-token", order_id);
  }

  static network::HttpRequest orders_request(const TossOrderQuery& query) {
    return TossClient::make_orders_request("access-token", query);
  }

  static Result<DepthSnapshot> parse_depth(
    InstrumentId instrument,
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_depth(std::move(instrument), std::move(response));
  }

  static Result<PriceLimits> parse_price_limits(Result<network::HttpResponse> response) {
    return TossClient::parse_price_limits(InstrumentId{}, std::move(response));
  }

  static Result<std::vector<TradePrint>> parse_trades(Result<network::HttpResponse> response) {
    return TossClient::parse_trades(InstrumentId{}, std::move(response));
  }

  static std::optional<BrokerError> parse_bars_page(
    Result<network::HttpResponse> response,
    const BarQuery& query,
    std::vector<Bar>& into,
    std::optional<std::string>& cursor,
    Currency& currency
  ) {
    return TossClient::parse_bars_page(std::move(response), query, into, cursor, currency);
  }

  static Result<std::vector<QuoteSnapshot>> parse_prices(
    const std::vector<InstrumentId>& requested,
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_prices(requested, std::move(response));
  }

  static Result<Instrument> parse_instrument(
    InstrumentId requested,
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_instrument(std::move(requested), std::move(response));
  }

  static Result<TradingCalendar> parse_calendar(
    Country country,
    Result<network::HttpResponse> response
  ) {
    return TossClient::parse_calendar(country, std::move(response));
  }

  static Result<OrderAck> parse_order_ack(Result<network::HttpResponse> response) {
    return TossClient::parse_order_ack(std::move(response));
  }

  static Result<OrderPage> parse_orders(Result<network::HttpResponse> response) {
    return TossClient::parse_orders(std::move(response));
  }

  static Result<OrderSnapshot> parse_order(Result<network::HttpResponse> response) {
    return TossClient::parse_order(std::move(response));
  }
};
} // namespace algotradelib::tossinvest

namespace {

class ScopedEnvironmentVariable {
public:
  explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
    if (const char* value = std::getenv(name_.c_str())) {
      original_ = value;
    }
  }

  ~ScopedEnvironmentVariable() noexcept {
    if (!original_) {
      unset();
      return;
    }
#ifdef _WIN32
    (void)_putenv_s(name_.c_str(), original_->c_str());
#else
    (void)::setenv(name_.c_str(), original_->c_str(), 1);
#endif
  }

  void set(const std::string& value) const {
#ifdef _WIN32
    if (_putenv_s(name_.c_str(), value.c_str()) != 0) {
      throw std::runtime_error("failed to set test environment variable");
    }
#else
    if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("failed to set test environment variable");
    }
#endif
  }

  void unset() const noexcept {
#ifdef _WIN32
    (void)_putenv_s(name_.c_str(), "");
#else
    (void)::unsetenv(name_.c_str());
#endif
  }

private:
  std::string name_;
  std::optional<std::string> original_;
};

struct TestCredentials : network::ClientCredentials<TestCredentials> {
  static constexpr std::string_view key_field() { return "key"; }
  static constexpr std::string_view secret_field() { return "secret"; }
  void set(
    std::string key,
    std::string secret
  ) {
    key_ = std::move(key);
    secret_ = std::move(secret);
  }
};

TEST_CASE(
  "Decimal64 preserves exact broker prices",
  "[core][decimal]"
) {
  REQUIRE(Decimal64::parse("72000") == Decimal64(72000, 0));
  REQUIRE(Decimal64::parse("185.700") == Decimal64(1857, 1));
  REQUIRE(Decimal64::parse("-0.050") == Decimal64(-5, 2));
  REQUIRE(
    Decimal64::parse("-9223372036854775808").coefficient() ==
    std::numeric_limits<std::int64_t>::min()
  );
  REQUIRE(Decimal64::parse("000.000").str() == "0");
  REQUIRE_THROWS_AS(Decimal64::parse("9223372036854775808"), std::out_of_range);
}

TEST_CASE(
  "ISO 8601 timestamps normalize offsets",
  "[core][time]"
) {
  const auto timestamp = parse_iso8601_timestamp("2026-03-25T09:30:00.123+09:00");
  const auto utc = parse_iso8601_timestamp("2026-03-25T00:30:00.123Z");
  REQUIRE(timestamp);
  REQUIRE(utc);
  REQUIRE(*timestamp == *utc);
  REQUIRE_FALSE(parse_iso8601_timestamp("2026-03-25 09:30:00"));
}

TEST_CASE(
  "Holdings values preserve quantities and separate currency totals",
  "[core][holdings]"
) {
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
  position.name_ = "Apple";
  position.quantity_ = Decimal64::parse("1.2500");
  position.average_purchase_price_ = Decimal64::parse("180.50");
  position.last_price_ = Decimal64::parse("185.70");
  position.purchase_value_ = Money{Decimal64::parse("225.625"), Currency::usd()};
  position.market_value_ = Money{Decimal64::parse("232.125"), Currency::usd()};
  position.unrealized_profit_loss_ = Money{Decimal64::parse("6.50"), Currency::usd()};
  position.unrealized_return_ = Decimal64::parse("0.0288088643");
  snapshot.positions_.push_back(position);
  snapshot.totals_.push_back(
    HoldingsTotals{
      Currency::krw(),
      Decimal64::parse("0"),
      Decimal64::parse("0"),
      Decimal64::parse("0"),
    }
  );
  snapshot.totals_.push_back(
    HoldingsTotals{
      Currency::usd(),
      Decimal64::parse("225.625"),
      Decimal64::parse("232.125"),
      Decimal64::parse("6.50"),
    }
  );

  REQUIRE(snapshot.positions_.front().quantity_ == Decimal64::parse("1.25"));
  REQUIRE(snapshot.positions_.front().market_value_.currency_ == Currency::usd());
  REQUIRE(snapshot.totals_.size() == 2);
  REQUIRE(snapshot.totals_[0].currency_ == Currency::krw());
  REQUIRE(snapshot.totals_[1].currency_ == Currency::usd());
}

TEST_CASE(
  "KIS account loads explicit and default product codes from environment",
  "[broker][kis][account][environment]"
) {
  ScopedEnvironmentVariable account_number("KIS_CANO");
  ScopedEnvironmentVariable product_code("KIS_ACNT_PRDT_CD");

  account_number.set("12345678");
  product_code.set("02");
  kis::KisAccount explicit_account;
  explicit_account.from_env();
  REQUIRE(explicit_account.account_number_ == "12345678");
  REQUIRE(explicit_account.product_code_ == "02");

  product_code.unset();
  kis::KisAccount default_account;
  default_account.from_env();
  REQUIRE(default_account.account_number_ == "12345678");
  REQUIRE(default_account.product_code_ == "01");

  account_number.unset();
  kis::KisAccount missing_account;
  REQUIRE_THROWS_AS(missing_account.from_env(), std::runtime_error);
}

TEST_CASE(
  "Credentials serialize without corrupting reserved characters",
  "[auth][encoding]"
) {
  TestCredentials credentials;
  credentials.set("client id", "a+b&c");
  REQUIRE(
    credentials.to_form_urlencoded_body() ==
    "grant_type=client_credentials&client_id=client%20id&client_secret=a%2Bb%26c"
  );
  const auto document = json::parse(credentials.to_jsonbody()).as_object();
  REQUIRE(document.at("key").as_string() == "client id");
  REQUIRE(document.at("secret").as_string() == "a+b&c");
}

TEST_CASE(
  "Broker clients reject unsupported instruments before network I/O",
  "[broker][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);

  auto kis_client = std::make_shared<kis::KisClient>(ioc.get_executor(), context, kis::KisConfig{});
  bool kis_completed = false;
  kis_client->async_quote(
    InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"},
    [&kis_completed](Result<QuoteSnapshot> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      kis_completed = true;
    }
  );

  auto toss_client =
    std::make_shared<tossinvest::TossClient>(ioc.get_executor(), context, tossinvest::TossConfig{});
  bool toss_completed = false;
  toss_client->async_quote(
    InstrumentId{Country::KR, Venue::COMBINED, "bad symbol"},
    [&toss_completed](Result<QuoteSnapshot> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      toss_completed = true;
    }
  );

  ioc.run();
  REQUIRE(kis_completed);
  REQUIRE(toss_completed);
}

TEST_CASE(
  "Async HTTP queue rejects excess callback and future requests",
  "[http][queue]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  network::HttpClientOptions options;
  options.queue_limit_ = 0;
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", "443", options
  );
  bool completed = false;
  client->async_request(network::HttpRequest{}, [&completed](Result<network::HttpResponse> result) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code_ == "queue_full");
    completed = true;
  });
  ioc.run();
  REQUIRE(completed);

  ioc.restart();
  auto future = client->async_request(network::HttpRequest{}, net::use_future);
  ioc.run();
  const auto future_result = future.get();
  REQUIRE_FALSE(future_result);
  REQUIRE(future_result.error().broker_code_ == "queue_full");
}

TEST_CASE(
  "Async HTTP preserves queue order on one TLS connection",
  "[http][tls][persistence]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  std::vector<std::string> bodies;
  for (const std::string target : {"/first", "/second"}) {
    network::HttpRequest request;
    request.target_ = target;
    client->async_request(std::move(request), [&bodies](Result<network::HttpResponse> result) {
      REQUIRE(result);
      bodies.push_back(result.value().body());
    });
  }

  ioc.run();
  REQUIRE(bodies == std::vector<std::string>{"/first", "/second"});
  REQUIRE(server.connectionCount() == 1);
}

TEST_CASE(
  "Async HTTP reconnects after a server-requested close",
  "[http][tls][reconnect]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  std::vector<std::string> bodies;
  for (const std::string target : {"/close", "/after-close"}) {
    network::HttpRequest request;
    request.target_ = target;
    client->async_request(std::move(request), [&bodies](Result<network::HttpResponse> result) {
      REQUIRE(result);
      bodies.push_back(result.value().body());
    });
  }

  ioc.run();
  REQUIRE(bodies == std::vector<std::string>{"/close", "/after-close"});
  REQUIRE(server.connectionCount() == 2);
}

TEST_CASE(
  "Async HTTP retries GET once but never replays POST",
  "[http][tls][retry]"
) {
  SECTION("GET is retried after a dropped response") {
    test::LocalHttpsServer server;
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    auto client = std::make_shared<network::AsyncHttpClient>(
      ioc.get_executor(), context, "localhost", server.port()
    );

    network::HttpRequest request;
    request.target_ = "/drop-get-once";
    bool completed = false;
    client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
      REQUIRE(result);
      REQUIRE(result.value().body() == "/drop-get-once");
      completed = true;
    });

    ioc.run();
    REQUIRE(completed);
    REQUIRE(server.requestCount("/drop-get-once") == 2);
    REQUIRE(server.connectionCount() == 2);
  }

  SECTION("POST is not replayed after a dropped response") {
    test::LocalHttpsServer server;
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    auto client = std::make_shared<network::AsyncHttpClient>(
      ioc.get_executor(), context, "localhost", server.port()
    );

    network::HttpRequest request;
    request.method_ = http::verb::post;
    request.target_ = "/drop-post";
    request.body_ = "order-like-payload";
    bool completed = false;
    client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
      REQUIRE_FALSE(result);
      completed = true;
    });

    ioc.run();
    REQUIRE(completed);
    REQUIRE(server.requestCount("/drop-post") == 1);
    REQUIRE(server.connectionCount() == 1);
  }
}

TEST_CASE(
  "Blocking HTTP retries GET once without exception-driven control flow",
  "[http][tls][blocking][retry]"
) {
  SECTION("GET is retried after a dropped response") {
    test::LocalHttpsServer server;
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    network::HttpClient client(ioc, context, "localhost", server.port());

    const auto response = client.get("/drop-get-once");
    REQUIRE(response.body() == "/drop-get-once");
    REQUIRE(server.requestCount("/drop-get-once") == 2);
    REQUIRE(server.connectionCount() == 2);
  }

  SECTION("POST reports the first dropped response") {
    test::LocalHttpsServer server;
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    network::HttpClient client(ioc, context, "localhost", server.port());

    REQUIRE_THROWS_AS(
      client.post("/drop-post", "order-like-payload"),
      beast::system_error
    );
    REQUIRE(server.requestCount("/drop-post") == 1);
    REQUIRE(server.connectionCount() == 1);
  }
}

TEST_CASE(
  "Async HTTP enforces the configured response body limit",
  "[http][tls][limits]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  network::HttpClientOptions options;
  options.body_limit_ = 128;
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  network::HttpRequest request;
  request.target_ = "/large";
  bool completed = false;
  client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::TRANSPORT);
    REQUIRE(result.error().broker_code_ == "read");
    completed = true;
  });

  ioc.run();
  REQUIRE(completed);
}

TEST_CASE(
  "KIS and Toss quote payloads normalize to shared types",
  "[broker][quote]"
) {
  const InstrumentId samsung{Country::KR, Venue::COMBINED, "005930"};

  network::HttpResponse kis_response{http::status::ok, 11};
  kis_response.body() = R"({"rt_cd":"0","msg_cd":"MCA00000","output":{"stck_prpr":"72000"}})";
  auto kis_result = kis::KisClientTestAccess::parse(samsung, std::move(kis_response));
  REQUIRE(kis_result);
  REQUIRE(kis_result.value().last_price_ == Decimal64(72000, 0));
  REQUIRE(kis_result.value().currency_ == Currency::krw());
  REQUIRE_FALSE(kis_result.value().exchange_time_);

  network::HttpResponse toss_response{http::status::ok, 11};
  toss_response.body() =
    R"({"result":[{"symbol":"005930","timestamp":"2026-03-25T09:30:00.123+09:00","lastPrice":"72000","currency":"KRW"}]})";
  auto toss_result = tossinvest::TossClientTestAccess::parse(samsung, std::move(toss_response));
  REQUIRE(toss_result);
  REQUIRE(toss_result.value().last_price_ == Decimal64(72000, 0));
  REQUIRE(toss_result.value().currency_ == Currency::krw());
  REQUIRE(toss_result.value().exchange_time_);

  network::HttpResponse toss_error{http::status::too_many_requests, 11};
  toss_error.body() =
    R"({"error":{"requestId":"request-1","code":"rate-limit-exceeded","message":"retry later"}})";
  auto error_result = tossinvest::TossClientTestAccess::parse(samsung, std::move(toss_error));
  REQUIRE_FALSE(error_result);
  REQUIRE(error_result.error().category_ == ErrorCategory::RATE_LIMIT);
  REQUIRE(error_result.error().request_id_ == "request-1");
}

TEST_CASE(
  "KIS holdings request selects environment and continuation",
  "[broker][kis][holdings]"
) {
  const auto header_value = [](const network::HttpRequest& request, std::string_view name) {
    for (const auto& header : request.headers_) {
      if (header.first == name) {
        return header.second;
      }
    }
    return std::string{};
  };
  const kis::KisAccount account{"12345678", "01"};

  kis::KisConfig live;
  const auto initial = kis::KisClientTestAccess::holdings_request(account, live, "", "", false);
  REQUIRE(header_value(initial, "tr_id") == "TTTC8434R");
  REQUIRE(header_value(initial, "tr_cont").empty());
  REQUIRE(initial.target_.find("CANO=12345678") != std::string::npos);
  REQUIRE(initial.target_.find("ACNT_PRDT_CD=01") != std::string::npos);

  kis::KisConfig demo;
  demo.is_demo_ = true;
  const auto next =
    kis::KisClientTestAccess::holdings_request(account, demo, "cursor key", "next", true);
  REQUIRE(header_value(next, "tr_id") == "VTTC8434R");
  REQUIRE(header_value(next, "tr_cont") == "N");
  REQUIRE(next.target_.find("CTX_AREA_FK100=cursor%20key") != std::string::npos);
}

TEST_CASE(
  "KIS holdings payload normalizes positions and summary",
  "[broker][kis][holdings]"
) {
  network::HttpResponse response{http::status::ok, 11};
  response.body() = R"({
    "rt_cd":"0",
    "msg_cd":"MCA00000",
    "ctx_area_fk100":"foreign-next",
    "ctx_area_nk100":"domestic-next",
    "output1":[{
      "pdno":"005930",
      "prdt_name":"Samsung Electronics",
      "hldg_qty":"10",
      "ord_psbl_qty":"7",
      "pchs_avg_pric":"70000.0000",
      "pchs_amt":"700000",
      "prpr":"72000",
      "evlu_amt":"720000",
      "evlu_pfls_amt":"20000",
      "evlu_pfls_rt":"2.857142"
    }],
    "output2":[{
      "pchs_amt_smtl_amt":"700000",
      "evlu_amt_smtl_amt":"720000",
      "evlu_pfls_smtl_amt":"20000"
    }]
  })";

  std::string foreign_cursor;
  std::string domestic_cursor;
  const auto result = kis::KisClientTestAccess::parse_holdings_page(
    std::move(response), foreign_cursor, domestic_cursor
  );
  REQUIRE(result);
  REQUIRE(foreign_cursor == "foreign-next");
  REQUIRE(domestic_cursor == "domestic-next");
  REQUIRE(result.value().positions_.size() == 1);
  const auto& position = result.value().positions_.front();
  const InstrumentId expected_instrument{Country::KR, Venue::COMBINED, "005930"};
  REQUIRE(position.instrument_ == expected_instrument);
  REQUIRE(position.quantity_ == Decimal64::parse("10"));
  REQUIRE(position.sellable_quantity_ == Decimal64::parse("7"));
  REQUIRE(position.average_purchase_price_ == Decimal64::parse("70000"));
  REQUIRE(position.unrealized_return_ == Decimal64::parse("0.02857142"));
  REQUIRE(result.value().totals_.size() == 1);
  REQUIRE(result.value().totals_.front().market_value_ == Decimal64::parse("720000"));
}

TEST_CASE(
  "KIS percentage values use normalized ratio representation",
  "[broker][kis][holdings][decimal]"
) {
  REQUIRE(broker_detail::percent_to_ratio("0.00") == Decimal64::parse("0"));
  REQUIRE(broker_detail::percent_to_ratio("10.00") == Decimal64::parse("0.1"));
  REQUIRE(broker_detail::percent_to_ratio("100.00") == Decimal64::parse("1"));
  REQUIRE(broker_detail::percent_to_ratio("2.857142") == Decimal64::parse("0.02857142"));
  REQUIRE(broker_detail::percent_to_ratio("-12.50") == Decimal64::parse("-0.125"));
}

TEST_CASE(
  "KIS holdings pagination accumulates pages and rejects repeated cursors",
  "[broker][kis][holdings][pagination]"
) {
  HoldingsSnapshot snapshot;
  std::size_t page_count{0};
  std::string foreign_cursor;
  std::string domestic_cursor;

  HoldingsSnapshot first_page;
  HoldingPosition first_position;
  first_position.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  first_page.positions_.push_back(std::move(first_position));
  first_page.totals_.push_back(
    HoldingsTotals{
      Currency::krw(),
      Decimal64::parse("700000"),
      Decimal64::parse("720000"),
      Decimal64::parse("20000"),
    }
  );
  auto error = kis::KisClientTestAccess::append_holdings_page(
    snapshot,
    page_count,
    foreign_cursor,
    domestic_cursor,
    std::move(first_page),
    "foreign-next",
    "domestic-next",
    true
  );
  REQUIRE_FALSE(error);
  REQUIRE(page_count == 1);
  REQUIRE(snapshot.positions_.size() == 1);
  REQUIRE(foreign_cursor == "foreign-next");
  REQUIRE(domestic_cursor == "domestic-next");

  HoldingsSnapshot second_page;
  HoldingPosition second_position;
  second_position.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  second_page.positions_.push_back(std::move(second_position));
  second_page.totals_.push_back(
    HoldingsTotals{
      Currency::krw(),
      Decimal64::parse("1000000"),
      Decimal64::parse("1100000"),
      Decimal64::parse("100000"),
    }
  );
  error = kis::KisClientTestAccess::append_holdings_page(
    snapshot,
    page_count,
    foreign_cursor,
    domestic_cursor,
    std::move(second_page),
    "",
    "",
    false
  );
  REQUIRE_FALSE(error);
  REQUIRE(page_count == 2);
  REQUIRE(snapshot.positions_.size() == 2);
  REQUIRE(snapshot.positions_.back().instrument_.symbol_ == "000660");
  REQUIRE(snapshot.totals_.front().market_value_ == Decimal64::parse("1100000"));

  HoldingsSnapshot repeated_page;
  error = kis::KisClientTestAccess::append_holdings_page(
    snapshot,
    page_count,
    foreign_cursor,
    domestic_cursor,
    std::move(repeated_page),
    "foreign-next",
    "domestic-next",
    true
  );
  REQUIRE(error);
  REQUIRE(error->broker_code_ == "invalid_pagination");
  REQUIRE(page_count == 2);
}

TEST_CASE(
  "KIS holdings rejects invalid accounts before network I/O",
  "[broker][kis][holdings][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client = std::make_shared<kis::KisClient>(ioc.get_executor(), context, kis::KisConfig{});
  bool completed = false;
  client->async_holdings(
    kis::KisAccount{"1234-678", "1"},
    [&completed](Result<HoldingsSnapshot> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      REQUIRE(result.error().broker_code_ == "invalid_account");
      completed = true;
    }
  );
  ioc.run();
  REQUIRE(completed);
}

TEST_CASE(
  "Toss account discovery preserves unknown account types",
  "[broker][toss][accounts]"
) {
  network::HttpResponse response{http::status::ok, 11};
  response.body() = R"({"result":[
    {"accountNo":"12345678901","accountSeq":1,"accountType":"BROKERAGE"},
    {"accountNo":"98765432100","accountSeq":2,"accountType":"FUTURE_ACCOUNT_TYPE"}
  ]})";

  const auto result = tossinvest::TossClientTestAccess::parse_accounts(std::move(response));
  REQUIRE(result);
  REQUIRE(result.value().size() == 2);
  REQUIRE(result.value()[0].sequence_ == 1);
  REQUIRE(result.value()[1].account_type_ == "FUTURE_ACCOUNT_TYPE");
}

TEST_CASE(
  "Toss holdings normalize multiple currencies and reject unknown countries",
  "[broker][toss][holdings]"
) {
  network::HttpResponse response{http::status::ok, 11};
  response.body() = R"({"result":{
    "totalPurchaseAmount":{"krw":"6500000","usd":"1553"},
    "marketValue":{
      "amount":{"krw":"7200000","usd":"1785"},
      "amountAfterCost":{"krw":"7050000","usd":"1771.43"}
    },
    "profitLoss":{
      "amount":{"krw":"700000","usd":"232"},
      "amountAfterCost":{"krw":"550000","usd":"218.43"},
      "rate":"0.1179",
      "rateAfterCost":"0.0983"
    },
    "dailyProfitLoss":{"amount":{"krw":"100000","usd":"25"},"rate":"0.0141"},
    "items":[
      {
        "symbol":"005930","name":"Samsung Electronics","marketCountry":"KR",
        "currency":"KRW","quantity":"100","lastPrice":"72000",
        "averagePurchasePrice":"65000",
        "marketValue":{"purchaseAmount":"6500000","amount":"7200000","amountAfterCost":"7050000"},
        "profitLoss":{
          "amount":"700000","amountAfterCost":"550000",
          "rate":"0.1077","rateAfterCost":"0.0846"
        },
        "dailyProfitLoss":{"amount":"100000","rate":"0.0141"},
        "cost":{"commission":"14400","tax":"135600"}
      },
      {
        "symbol":"AAPL","name":"Apple Inc.","marketCountry":"US",
        "currency":"USD","quantity":"10","lastPrice":"178.5",
        "averagePurchasePrice":"155.3",
        "marketValue":{"purchaseAmount":"1553","amount":"1785","amountAfterCost":"1771.43"},
        "profitLoss":{
          "amount":"232","amountAfterCost":"218.43",
          "rate":"0.1494","rateAfterCost":"0.1406"
        },
        "dailyProfitLoss":{"amount":"25","rate":"0.0142"},
        "cost":{"commission":"3.57","tax":"10"}
      }
    ]
  }})";

  const auto result = tossinvest::TossClientTestAccess::parse_holdings(std::move(response));
  REQUIRE(result);
  REQUIRE(result.value().positions_.size() == 2);
  REQUIRE(result.value().positions_[0].instrument_.country_ == Country::KR);
  REQUIRE(result.value().positions_[0].sellable_quantity_ == std::nullopt);
  REQUIRE(result.value().positions_[1].instrument_.country_ == Country::US);
  REQUIRE(result.value().positions_[1].market_value_.amount_ == Decimal64::parse("1785"));
  REQUIRE(result.value().totals_.size() == 2);
  REQUIRE(result.value().totals_[0].currency_ == Currency::krw());
  REQUIRE(result.value().totals_[1].currency_ == Currency::usd());
  REQUIRE(result.value().totals_[1].unrealized_profit_loss_ == Decimal64::parse("232"));

  network::HttpResponse future_response{http::status::ok, 11};
  future_response.body() = R"({"result":{
    "totalPurchaseAmount":{"krw":"0","usd":null},
    "marketValue":{"amount":{"krw":"0","usd":null}},
    "profitLoss":{"amount":{"krw":"0","usd":null}},
    "items":[{
      "symbol":"7203","name":"Future market","marketCountry":"JP",
      "currency":"JPY","quantity":"1","lastPrice":"1000","averagePurchasePrice":"900",
      "marketValue":{"purchaseAmount":"900","amount":"1000"},
      "profitLoss":{"amount":"100","rate":"0.1111"}
    }]
  }})";
  const auto future =
    tossinvest::TossClientTestAccess::parse_holdings(std::move(future_response));
  REQUIRE_FALSE(future);
  REQUIRE(future.error().category_ == ErrorCategory::PROTOCOL);
  REQUIRE(future.error().broker_code_ == "invalid_holdings_response");
  REQUIRE(future.error().message_ == "unsupported marketCountry: JP");
}

TEST_CASE(
  "Toss holdings reject invalid account sequences before network I/O",
  "[broker][toss][holdings][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client =
    std::make_shared<tossinvest::TossClient>(ioc.get_executor(), context, tossinvest::TossConfig{});
  bool completed = false;
  client->async_holdings(0, [&completed](Result<HoldingsSnapshot> result) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    REQUIRE(result.error().broker_code_ == "invalid_account");
    completed = true;
  });
  ioc.run();
  REQUIRE(completed);
}

TEST_CASE(
  "Holdings parsers preserve empty results and broker errors",
  "[broker][holdings][errors]"
) {
  SECTION("Toss accepts an empty portfolio") {
    network::HttpResponse response{http::status::ok, 11};
    response.body() = R"({"result":{
      "totalPurchaseAmount":{"krw":"0","usd":null},
      "marketValue":{"amount":{"krw":"0","usd":null}},
      "profitLoss":{"amount":{"krw":"0","usd":null}},
      "items":[]
    }})";
    const auto result = tossinvest::TossClientTestAccess::parse_holdings(std::move(response));
    REQUIRE(result);
    REQUIRE(result.value().positions_.empty());
    REQUIRE(result.value().totals_.size() == 1);
    REQUIRE(result.value().totals_.front().market_value_ == Decimal64::parse("0"));
  }

  SECTION("Toss rejects inconsistent overview currencies") {
    network::HttpResponse response{http::status::ok, 11};
    response.body() = R"({"result":{
      "totalPurchaseAmount":{"krw":"0","usd":"10"},
      "marketValue":{"amount":{"krw":"0","usd":null}},
      "profitLoss":{"amount":{"krw":"0","usd":"1"}},
      "items":[]
    }})";
    const auto result = tossinvest::TossClientTestAccess::parse_holdings(std::move(response));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_holdings_response");
  }

  SECTION("Toss preserves structured HTTP errors") {
    network::HttpResponse response{http::status::too_many_requests, 11};
    response.body() =
      R"({"error":{"requestId":"holding-1","code":"rate-limit","message":"retry"}})";
    response.set("Retry-After", "3");
    const auto result = tossinvest::TossClientTestAccess::parse_holdings(std::move(response));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::RATE_LIMIT);
    REQUIRE(result.error().request_id_ == "holding-1");
    REQUIRE(result.error().retry_after_ == std::chrono::seconds(3));
  }

  SECTION("KIS preserves response codes") {
    network::HttpResponse response{http::status::ok, 11};
    response.body() = R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"request failed"})";
    const auto result = kis::KisClientTestAccess::parse_holdings(std::move(response));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "APBK0919");
    REQUIRE(result.error().message_ == "request failed");
  }
}

} // namespace

namespace {

/** Returns one header value from a built request, or an empty string. */
std::string header_of(const network::HttpRequest& request, std::string_view name) {
  for (const auto& header : request.headers_) {
    if (header.first == name) {
      return header.second;
    }
  }
  return std::string{};
}

/** Builds a successful JSON response for a parser test. */
Result<network::HttpResponse> json_response(std::string body, int status = 200) {
  network::HttpResponse response(static_cast<http::status>(status), 11);
  response.set(http::field::content_type, "application/json");
  response.body() = std::move(body);
  response.prepare_payload();
  return Result<network::HttpResponse>(std::move(response));
}

/** Builds one commission row without depending on the order of the struct's fields. */
CommissionRate rate_row(
  Country country,
  Decimal64 rate,
  std::optional<Date> from,
  std::optional<Date> until
) {
  CommissionRate row;
  row.country_ = country;
  row.rate_ = std::move(rate);
  row.from_ = std::move(from);
  row.until_ = std::move(until);
  return row;
}

/** Builds a limit buy order for ten shares of one Korean equity. */
OrderRequest korean_limit_buy() {
  OrderRequest order;
  order.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.time_in_force_ = TimeInForce::DAY;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");
  return order;
}

} // namespace

TEST_CASE(
  "KIS refuses a client order id through the whole placement path",
  "[broker][kis][order][validation]"
) {
  // No credentials and no reachable server: the refusal has to arrive before
  // authentication, which is the only way this test can pass at all.
  ssl::context tls(ssl::context::tls_client);
  kis::KisConfig config;
  kis::KisBlockingClient client(tls, std::move(config));

  auto order = korean_limit_buy();
  order.client_order_id_ = std::string("key-1");

  const auto result = client.place_order(kis::KisAccount{"12345678", "01"}, order);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  REQUIRE(result.error().broker_code_ == "idempotency_key_unsupported");

  // The predicate is shared, so the broker port cannot disagree with the client.
  const auto direct = kis::unsupported_client_order_id(order);
  REQUIRE(direct);
  REQUIRE(direct->broker_code_ == result.error().broker_code_);
  REQUIRE(direct->message_ == result.error().message_);

  const auto through_port = BrokerTraits<kis::KisClient>::validate_place(order);
  REQUIRE(through_port);
  REQUIRE(through_port->broker_code_ == result.error().broker_code_);

  // Toss honours the key, so the identical order passes there.
  REQUIRE_FALSE(BrokerTraits<tossinvest::TossClient>::validate_place(order));
}

TEST_CASE("Calendar dates validate Gregorian boundaries", "[core][order][date]") {
  REQUIRE(is_valid_date(Date{2024, 2, 29}));
  REQUIRE_FALSE(is_valid_date(Date{2025, 2, 29}));
  REQUIRE_FALSE(is_valid_date(Date{2026, 4, 31}));
  REQUIRE(Date{2026, 3, 1} < Date{2026, 3, 31});
}

TEST_CASE("A day count converts to the Gregorian date it names", "[core][date]") {
  REQUIRE(date_from_epoch_days(0) == Date{1970, 1, 1});
  REQUIRE(date_from_epoch_days(1) == Date{1970, 1, 2});
  // Negative counts run backwards, which is what a replay handed pre-epoch bars needs.
  REQUIRE(date_from_epoch_days(-1) == Date{1969, 12, 31});

  // The cases a hand-rolled conversion gets wrong: a leap day, the day after it, and the two
  // centuries that disagree about whether they have one.
  REQUIRE(date_from_epoch_days(19782) == Date{2024, 2, 29});
  REQUIRE(date_from_epoch_days(19783) == Date{2024, 3, 1});
  REQUIRE(date_from_epoch_days(11016) == Date{2000, 2, 29});
  REQUIRE(date_from_epoch_days(-25508) == Date{1900, 3, 1});
  REQUIRE(date_from_epoch_days(-25509) == Date{1900, 2, 28});
}

TEST_CASE("Every date in a leap-year span round-trips through its day count", "[core][date]") {
  // A spot check proves the cases somebody thought of. Walking a span proves the ones they did
  // not: the day count is incremented independently of the date, so a conversion that drifts by a
  // day anywhere in four years -- which is exactly how these are usually wrong -- fails here.
  const auto days_in = [](int year, unsigned month) -> unsigned {
    static constexpr unsigned table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return table[month - 1] + ((month == 2 && leap) ? 1 : 0);
  };

  // 2024-01-01 is 19723 days after the epoch.
  std::int64_t count = 19723;
  std::size_t checked = 0;
  for (int year = 2024; year <= 2027; ++year) {
    for (unsigned month = 1; month <= 12; ++month) {
      for (unsigned day = 1; day <= days_in(year, month); ++day) {
        REQUIRE(date_from_epoch_days(count) == Date{year, month, day});
        REQUIRE(is_valid_date(date_from_epoch_days(count)));
        ++count;
        ++checked;
      }
    }
  }
  // 2024 is a leap year, so four years is 366 + 365 * 3 days.
  REQUIRE(checked == 1461);
}

TEST_CASE(
  "KIS order request selects the transaction id for side and environment",
  "[broker][kis][order]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;
  kis::KisConfig demo;
  demo.is_demo_ = true;

  SECTION("a live buy and sell use their documented transaction ids") {
    auto buy = korean_limit_buy();
    const auto buy_request =
      kis::KisClientTestAccess::place_order_request(account, live, buy, "00");
    REQUIRE(buy_request.method_ == http::verb::post);
    REQUIRE(buy_request.target_ == "/uapi/domestic-stock/v1/trading/order-cash");
    REQUIRE(header_of(buy_request, "tr_id") == "TTTC0012U");

    buy.side_ = OrderSide::SELL;
    const auto sell_request =
      kis::KisClientTestAccess::place_order_request(account, live, buy, "00");
    REQUIRE(header_of(sell_request, "tr_id") == "TTTC0011U");
  }

  SECTION("a demo buy and sell use their documented transaction ids") {
    auto order = korean_limit_buy();
    REQUIRE(
      header_of(kis::KisClientTestAccess::place_order_request(account, demo, order, "00"), "tr_id")
        == "VTTC0012U"
    );
    order.side_ = OrderSide::SELL;
    REQUIRE(
      header_of(kis::KisClientTestAccess::place_order_request(account, demo, order, "00"), "tr_id")
        == "VTTC0011U"
    );
  }

  SECTION("a limit order transmits its price and account selection") {
    const auto order = korean_limit_buy();
    const auto request =
      kis::KisClientTestAccess::place_order_request(account, live, order, "00");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("CANO").as_string() == "12345678");
    REQUIRE(body.at("ACNT_PRDT_CD").as_string() == "01");
    REQUIRE(body.at("PDNO").as_string() == "005930");
    REQUIRE(body.at("ORD_DVSN").as_string() == "00");
    REQUIRE(body.at("ORD_QTY").as_string() == "10");
    REQUIRE(body.at("ORD_UNPR").as_string() == "70000");
    // `korean_limit_buy` leaves the default venue, which is `COMBINED`. That
    // used to omit `EXCG_ID_DVSN_CD` entirely, and the workbook says an omitted
    // field proceeds as KRX -- so asking for the combined book silently got one
    // venue. The combined book is routed, which is what KIS calls SOR.
    REQUIRE(body.at("EXCG_ID_DVSN_CD").as_string() == "SOR");
  }

  SECTION("an unspecified venue still omits the exchange field") {
    auto order = korean_limit_buy();
    order.instrument_.venue_ = Venue::UNSPECIFIED;
    const auto request =
      kis::KisClientTestAccess::place_order_request(account, live, order, "00");
    // Expressing no venue is not the same as expressing the combined one: the
    // broker's documented default is the honest outcome for a caller who asked
    // for nothing, and a substitution for a caller who asked for something.
    REQUIRE_FALSE(json::parse(request.body_).as_object().contains("EXCG_ID_DVSN_CD"));
  }

  SECTION("a market order sends a zero unit price rather than omitting it") {
    auto order = korean_limit_buy();
    order.type_ = OrderType::MARKET;
    order.price_.reset();
    const auto request =
      kis::KisClientTestAccess::place_order_request(account, live, order, "01");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("ORD_DVSN").as_string() == "01");
    REQUIRE(body.at("ORD_UNPR").as_string() == "0");
  }

  SECTION("an explicit venue is transmitted as its exchange code") {
    auto order = korean_limit_buy();
    order.instrument_.venue_ = Venue::NXT;
    const auto request =
      kis::KisClientTestAccess::place_order_request(account, live, order, "00");
    REQUIRE(json::parse(request.body_).as_object().at("EXCG_ID_DVSN_CD").as_string() == "NXT");
  }
}


namespace {

/**
 * KIS's `ORD_DVSN` table, transcribed a second time and by hand.
 *
 * Deliberately not read from `kOrderShapes`. A test that asked the mapping
 * whether it agreed with itself would pass for any mapping; this is the
 * workbook's own content, typed out from the sheets named below, so the
 * assertions below compare the library against the document rather than
 * against its own opinion.
 *
 * Sources, both from `한국투자증권_오픈API_전체문서_20260916_030000.xlsx`:
 *   - `주식주문(현금)`, the request table: 00-06, 11-16, 21-24, 27-29, 41-47.
 *   - `주식정정취소가능주문조회`, a response table: adds 07, omits 27-29 and 41-47.
 */
struct ShapeCase {
  const char* code_;
  OrderType type_;
  TimeInForce condition_;
  OrderSession session_;
  bool krx_;
  bool nxt_;
  bool sor_;
  bool requestable_;
};

constexpr ShapeCase kWorkbookShapes[] = {
  {"00", OrderType::LIMIT, TimeInForce::DAY, OrderSession::REGULAR, true, true, true, true},
  {"01", OrderType::MARKET, TimeInForce::DAY, OrderSession::REGULAR, true, false, true, true},
  {"02", OrderType::CONDITIONAL_LIMIT, TimeInForce::DAY, OrderSession::REGULAR,
   true, false, false, true},
  {"03", OrderType::BEST_LIMIT, TimeInForce::DAY, OrderSession::REGULAR, true, true, true, true},
  {"04", OrderType::PRIMARY_LIMIT, TimeInForce::DAY, OrderSession::REGULAR,
   true, true, true, true},
  {"05", OrderType::CLOSING_PRICE, TimeInForce::DAY, OrderSession::PRE_MARKET,
   true, false, false, true},
  {"06", OrderType::CLOSING_PRICE, TimeInForce::DAY, OrderSession::AFTER_HOURS,
   true, false, false, true},
  {"07", OrderType::LIMIT, TimeInForce::DAY, OrderSession::AFTER_HOURS_AUCTION,
   true, false, false, false},
  {"11", OrderType::LIMIT, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,
   true, true, true, true},
  {"12", OrderType::LIMIT, TimeInForce::FILL_OR_KILL, OrderSession::REGULAR,
   true, true, true, true},
  {"13", OrderType::MARKET, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,
   true, true, true, true},
  {"14", OrderType::MARKET, TimeInForce::FILL_OR_KILL, OrderSession::REGULAR,
   true, true, true, true},
  {"15", OrderType::BEST_LIMIT, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,
   true, true, true, true},
  {"16", OrderType::BEST_LIMIT, TimeInForce::FILL_OR_KILL, OrderSession::REGULAR,
   true, true, true, true},
  {"21", OrderType::MID_PRICE, TimeInForce::DAY, OrderSession::REGULAR, true, true, false, true},
  {"22", OrderType::STOP_LIMIT, TimeInForce::DAY, OrderSession::REGULAR, true, true, false, false},
  {"23", OrderType::MID_PRICE, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,
   true, true, false, true},
  {"24", OrderType::MID_PRICE, TimeInForce::FILL_OR_KILL, OrderSession::REGULAR,
   true, true, false, true},
  {"27", OrderType::LIMIT, TimeInForce::DAY, OrderSession::GTP, false, true, false, true},
  {"28", OrderType::BEST_LIMIT, TimeInForce::DAY, OrderSession::GTP, false, true, false, true},
  {"29", OrderType::PRIMARY_LIMIT, TimeInForce::DAY, OrderSession::GTP, false, true, false, true},
  {"41", OrderType::LIMIT, TimeInForce::DAY, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"42", OrderType::LIMIT, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"43", OrderType::LIMIT, TimeInForce::FILL_OR_KILL, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"44", OrderType::BEST_LIMIT, TimeInForce::DAY, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"45", OrderType::BEST_LIMIT, TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"46", OrderType::BEST_LIMIT, TimeInForce::FILL_OR_KILL, OrderSession::AFTER_MARKET,
   true, false, false, true},
  {"47", OrderType::PRIMARY_LIMIT, TimeInForce::DAY, OrderSession::AFTER_MARKET,
   true, false, false, true},
};

/** Builds a request for `shape` at `venue`, priced so only the shape can refuse it. */
OrderRequest shaped_order(const ShapeCase& shape, Venue venue) {
  auto order = korean_limit_buy();
  order.instrument_.venue_ = venue;
  order.type_ = shape.type_;
  order.time_in_force_ = shape.condition_;
  order.session_ = shape.session_;
  // Only a limit basis takes a price; every other basis must not carry one, and
  // `validate_price` would refuse first and mask the answer this test wants.
  if (shape.type_ != OrderType::LIMIT) {
    order.price_.reset();
  }
  return order;
}

} // namespace

TEST_CASE(
  "KIS composes every order code the workbook publishes, and only where it publishes it",
  "[broker][kis][order]"
) {
  SECTION("each venue accepts exactly the codes its own list carries") {
    for (const auto& shape : kWorkbookShapes) {
      const std::array<std::pair<Venue, bool>, 3> venues{{
        {Venue::KRX, shape.krx_},
        {Venue::NXT, shape.nxt_},
        {Venue::COMBINED, shape.sor_},
      }};
      for (const auto& [venue, published] : venues) {
        std::string division;
        const auto error =
          kis::KisClientTestAccess::validate_order(shaped_order(shape, venue), division);
        const bool allowed = published && shape.requestable_;
        INFO("code " << shape.code_ << " at venue " << static_cast<int>(venue));
        if (allowed) {
          REQUIRE_FALSE(error);
          REQUIRE(division == shape.code_);
        } else {
          REQUIRE(error);
          REQUIRE(error->category_ == ErrorCategory::INVALID_ARGUMENT);
          // Nothing was sent, so nothing may be charged to the broker or the
          // rate limiter. This is the property that makes the refusal a
          // correction rather than a wasted order.
          REQUIRE_FALSE(error->reached_broker_);
        }
      }
    }
  }

  SECTION("no two published shapes compose the same code") {
    std::vector<std::string> composed;
    for (const auto& shape : kWorkbookShapes) {
      if (!shape.requestable_) {
        continue;
      }
      const auto venue = shape.krx_ ? Venue::KRX : Venue::NXT;
      std::string division;
      REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(shaped_order(shape, venue), division));
      composed.push_back(division);
    }
    const auto before = composed.size();
    std::sort(composed.begin(), composed.end());
    composed.erase(std::unique(composed.begin(), composed.end()), composed.end());
    // A collision would mean two different orders arriving at KIS as the same
    // one, which no single-shape assertion above can detect.
    REQUIRE(composed.size() == before);
  }

  SECTION("a market order bound for NXT is refused rather than sent as a KRX code") {
    auto order = korean_limit_buy();
    order.type_ = OrderType::MARKET;
    order.price_.reset();
    order.instrument_.venue_ = Venue::NXT;
    std::string division;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    // The sheet lists 01 under [KRX] and [SOR] and not under [NXT]. This used
    // to compose 01 regardless and let the broker refuse it, at the far end of
    // a POST that may never be replayed.
    REQUIRE(error);
    REQUIRE_FALSE(error->reached_broker_);
    REQUIRE(division.empty());

    order.instrument_.venue_ = Venue::KRX;
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
    REQUIRE(division == "01");
  }

  SECTION("a code KIS reports but does not accept is refused on a request") {
    auto order = korean_limit_buy();
    order.session_ = OrderSession::AFTER_HOURS_AUCTION;
    std::string division;
    // 07 is on the inquiry sheet and not on the order sheet.
    REQUIRE(kis::KisClientTestAccess::validate_order(order, division));
  }
}

TEST_CASE("KIS reads back every order code the workbook publishes", "[broker][kis][order]") {
  SECTION("each code normalizes to its own three facts and keeps itself") {
    for (const auto& shape : kWorkbookShapes) {
      OrderSnapshot order;
      kis::KisClientTestAccess::apply_order_shape(shape.code_, order);
      INFO("code " << shape.code_);
      REQUIRE(order.type_ == shape.type_);
      REQUIRE(order.time_in_force_ == shape.condition_);
      REQUIRE(order.session_ == shape.session_);
      REQUIRE(order.shape_code_.view() == shape.code_);
    }
  }

  SECTION("a code outside the table is unknown in all three and still reported") {
    for (const auto* code : {"08", "31", "99", ""}) {
      OrderSnapshot order;
      kis::KisClientTestAccess::apply_order_shape(code, order);
      INFO("code " << code);
      REQUIRE(order.type_ == OrderType::UNKNOWN);
      REQUIRE(order.time_in_force_ == TimeInForce::UNKNOWN);
      REQUIRE(order.session_ == OrderSession::UNKNOWN);
      REQUIRE(order.shape_code_.view() == code);
    }
  }
}

TEST_CASE("a KIS amendment restates the shape it is amending", "[broker][kis][order]") {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;

  SECTION("an immediate-or-cancel order amended to a new price stays immediate-or-cancel") {
    kis::KisAmendment amendment;
    amendment.order_ = kis::KisOrderRef{"0000117057", "00950"};
    amendment.type_ = OrderType::LIMIT;
    amendment.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
    amendment.session_ = OrderSession::REGULAR;
    amendment.venue_ = Venue::KRX;
    amendment.quantity_ = Decimal64::parse("1");
    amendment.price_ = Decimal64::parse("71000");

    std::string division;
    REQUIRE_FALSE(kis::KisClientTestAccess::amendment_division(amendment, division));
    // 11, not 00. The amendment path sent `DAY` unconditionally, so this order
    // reached KIS as a plain day order at the new price -- a different order
    // from the one being amended, and one nobody asked for.
    REQUIRE(division == "11");

    const auto body = json::parse(
      kis::KisClientTestAccess::amend_order_request(account, live, amendment, false, division).body_
    ).as_object();
    REQUIRE(body.at("ORD_DVSN").as_string() == "11");
    REQUIRE(body.at("EXCG_ID_DVSN_CD").as_string() == "KRX");
  }

  SECTION("an amendment to a venue that does not publish the code is refused") {
    kis::KisAmendment amendment;
    amendment.order_ = kis::KisOrderRef{"0000117057", "00950"};
    amendment.type_ = OrderType::MARKET;
    amendment.venue_ = Venue::NXT;
    amendment.quantity_ = Decimal64::parse("1");
    std::string division;
    REQUIRE(kis::KisClientTestAccess::amendment_division(amendment, division));
  }

  SECTION("a cancellation copies the whole shape rather than the basis alone") {
    WorkingOrder<kis::Broker> working;
    working.order_.type_ = OrderType::BEST_LIMIT;
    working.order_.time_in_force_ = TimeInForce::FILL_OR_KILL;
    working.order_.session_ = OrderSession::AFTER_MARKET;
    working.order_.instrument_ = InstrumentId{Country::KR, Venue::KRX, "005930"};
    working.amendable_quantity_ = Decimal64::parse("5");

    const auto instruction = cancel_all(working);
    REQUIRE(instruction.shape_.type_ == OrderType::BEST_LIMIT);
    REQUIRE(instruction.shape_.time_in_force_ == TimeInForce::FILL_OR_KILL);
    REQUIRE(instruction.shape_.session_ == OrderSession::AFTER_MARKET);
    REQUIRE(instruction.venue_ == Venue::KRX);
  }
}

TEST_CASE(
  "KIS amendment distinguishes modification from cancellation",
  "[broker][kis][order]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;

  kis::KisAmendment amendment;
  amendment.order_ = kis::KisOrderRef{"06010", "0001569157"};
  amendment.type_ = OrderType::LIMIT;
  amendment.quantity_ = Decimal64::parse("3");
  amendment.price_ = Decimal64::parse("71000");

  SECTION("a partial modification carries the original order and a partial flag") {
    const auto request =
      kis::KisClientTestAccess::amend_order_request(account, live, amendment, false, "00");
    REQUIRE(request.method_ == http::verb::post);
    REQUIRE(request.target_ == "/uapi/domestic-stock/v1/trading/order-rvsecncl");
    REQUIRE(header_of(request, "tr_id") == "TTTC0013U");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("KRX_FWDG_ORD_ORGNO").as_string() == "06010");
    REQUIRE(body.at("ORGN_ODNO").as_string() == "0001569157");
    REQUIRE(body.at("RVSE_CNCL_DVSN_CD").as_string() == "01");
    REQUIRE(body.at("QTY_ALL_ORD_YN").as_string() == "N");
  }

  SECTION("a whole-remainder cancellation sets both documented flags") {
    amendment.all_remaining_ = true;
    const auto request =
      kis::KisClientTestAccess::amend_order_request(account, live, amendment, true, "00");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("RVSE_CNCL_DVSN_CD").as_string() == "02");
    REQUIRE(body.at("QTY_ALL_ORD_YN").as_string() == "Y");
  }
}

TEST_CASE(
  "KIS rejects unrepresentable orders before any network I/O",
  "[broker][kis][order][validation]"
) {
  std::string division;

  SECTION("an order carrying a client order id is rejected") {
    // KIS has nowhere to put it — `gt_uid` is documented `[법인 전용]` — and dropping
    // it would leave a caller believing a resubmission after an indeterminate POST
    // is safe when it would place a second live order.
    auto order = korean_limit_buy();
    order.client_order_id_ = std::string("key-1");
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->category_ == ErrorCategory::INVALID_ARGUMENT);
    REQUIRE(error->broker_code_ == "idempotency_key_unsupported");

    // The same order without the field is representable.
    order.client_order_id_.reset();
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
    REQUIRE(division == "00");
  }

  SECTION("a high-value confirmation is accepted and ignored") {
    // Unlike a client order id, this gates an order rather than describing one:
    // ignoring it cannot make KIS transmit an order the caller did not describe.
    auto order = korean_limit_buy();
    order.confirm_high_value_ = true;
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
  }

  SECTION("a fractional quantity is rejected") {
    auto order = korean_limit_buy();
    order.quantity_ = Decimal64::parse("1.5");
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->category_ == ErrorCategory::INVALID_ARGUMENT);
    REQUIRE(error->broker_code_ == "invalid_quantity");
  }

  SECTION("a zero quantity is rejected") {
    auto order = korean_limit_buy();
    order.quantity_ = Decimal64::parse("0");
    REQUIRE(kis::KisClientTestAccess::validate_order(order, division));
  }

  SECTION("a limit order without a price is rejected") {
    auto order = korean_limit_buy();
    order.price_.reset();
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_price");
  }

  SECTION("a market order carrying a price is rejected") {
    auto order = korean_limit_buy();
    order.type_ = OrderType::MARKET;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_price");
  }

  SECTION("a closing-auction session is rejected rather than downgraded to the regular one") {
    auto order = korean_limit_buy();
    order.session_ = OrderSession::CLOSING_AUCTION;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "unsupported_order");
  }

  SECTION("an unknown order type is rejected on a request path") {
    auto order = korean_limit_buy();
    order.type_ = OrderType::UNKNOWN;
    REQUIRE(kis::KisClientTestAccess::validate_order(order, division));
  }

  SECTION("a non-Korean instrument is rejected") {
    auto order = korean_limit_buy();
    order.instrument_.country_ = Country::US;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_instrument");
  }

  SECTION("a malformed domestic symbol is rejected") {
    auto order = korean_limit_buy();
    order.instrument_.symbol_ = "5930";
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_instrument");
  }

  SECTION("a seven-character ETN product number is accepted") {
    // KIS documents PDNO as six characters, or seven for an ETN, so a
    // seven-character symbol must not be rejected before the wire.
    auto order = korean_limit_buy();
    order.instrument_.symbol_ = "5800010";
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
  }

  SECTION("a symbol longer than an ETN product number is rejected") {
    auto order = korean_limit_buy();
    order.instrument_.symbol_ = "58000101";
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_instrument");
  }

  SECTION("a fractional won price is rejected") {
    auto order = korean_limit_buy();
    order.price_ = Decimal64::parse("70000.5");
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_price");
  }

  SECTION("a valid day limit order maps to the documented order division") {
    const auto order = korean_limit_buy();
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
    REQUIRE(division == "00");
  }
}

TEST_CASE(
  "KIS order placement rejects a malformed account before authentication",
  "[broker][kis][order][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client =
    std::make_shared<kis::KisClient>(ioc.get_executor(), context, kis::KisConfig{});

  bool completed = false;
  client->async_place_order(
    kis::KisAccount{"123", "01"},
    korean_limit_buy(),
    [&completed](Result<kis::KisOrderAck> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      REQUIRE(result.error().broker_code_ == "invalid_account");
      completed = true;
    }
  );
  ioc.run();
  REQUIRE(completed);
}

TEST_CASE(
  "KIS amendment validates an order reference before authentication",
  "[broker][kis][order][validation]"
) {
  // KIS's own documented order-rvsecncl request example sends
  // KRX_FWDG_ORD_ORGNO blank, so an empty branch number must stay usable. The
  // predicate is checked directly for the accepted cases because a valid
  // reference would otherwise continue to authentication and network I/O.
  SECTION("an empty branch number is accepted with a ten-digit order number") {
    REQUIRE(
      kis::KisClientTestAccess::valid_order_reference(kis::KisOrderRef{"", "0001569157"})
    );
  }

  SECTION("a five-digit branch number is accepted") {
    REQUIRE(
      kis::KisClientTestAccess::valid_order_reference(kis::KisOrderRef{"06010", "0001569157"})
    );
  }

  SECTION("a malformed non-empty branch number is rejected") {
    REQUIRE_FALSE(
      kis::KisClientTestAccess::valid_order_reference(kis::KisOrderRef{"610", "0001569157"})
    );
  }

  SECTION("an order number that is not ten digits is rejected") {
    REQUIRE_FALSE(
      kis::KisClientTestAccess::valid_order_reference(kis::KisOrderRef{"06010", "1569157"})
    );
    REQUIRE_FALSE(
      kis::KisClientTestAccess::valid_order_reference(kis::KisOrderRef{"06010", ""})
    );
  }

  SECTION("a malformed reference fails through the async entry point") {
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    auto client =
      std::make_shared<kis::KisClient>(ioc.get_executor(), context, kis::KisConfig{});

    kis::KisAmendment amendment;
    amendment.order_ = kis::KisOrderRef{"06010", "not-a-number"};
    amendment.quantity_ = Decimal64::parse("1");
    amendment.price_ = Decimal64::parse("70000");

    bool completed = false;
    client->async_replace_order(
      kis::KisAccount{"12345678", "01"},
      std::move(amendment),
      [&completed](Result<kis::KisOrderAck> result) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
        REQUIRE(result.error().broker_code_ == "invalid_order_reference");
        completed = true;
      }
    );
    ioc.run();
    REQUIRE(completed);
  }
}

TEST_CASE(
  "KIS order history rejects invalid or reversed dates before authentication",
  "[broker][kis][order][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client =
    std::make_shared<kis::KisClient>(ioc.get_executor(), context, kis::KisConfig{});

  const auto expect_rejection = [&](kis::KisHistoryRange range) {
    bool completed = false;
    client->async_order_history(
      kis::KisAccount{"12345678", "01"},
      range,
      [&completed](Result<OrderPage> result) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
        REQUIRE(result.error().broker_code_ == "invalid_date_range");
        completed = true;
      }
    );
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  };

  expect_rejection(kis::KisHistoryRange{Date{2026, 2, 29}, Date{2026, 3, 1}});
  expect_rejection(kis::KisHistoryRange{Date{2026, 3, 31}, Date{2026, 3, 1}});
}

TEST_CASE(
  "KIS refuses the modifiable-order inquiry on a demo account",
  "[broker][kis][order][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  kis::KisConfig demo;
  demo.is_demo_ = true;
  auto client = std::make_shared<kis::KisClient>(ioc.get_executor(), context, demo);

  bool completed = false;
  client->async_open_orders(
    kis::KisAccount{"12345678", "01"},
    [&completed](Result<kis::KisOpenOrderPage> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      REQUIRE(result.error().broker_code_ == "demo_unsupported");
      completed = true;
    }
  );
  ioc.run();
  REQUIRE(completed);
}

TEST_CASE(
  "KIS order acknowledgement preserves the branch code needed to amend",
  "[broker][kis][order]"
) {
  SECTION("an accepted order reports both identifier parts") {
    auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(
        R"({"rt_cd":"0","msg_cd":"APBK0013","msg1":"order sent",)"
        R"("output":{"KRX_FWDG_ORD_ORGNO":"06010","ODNO":"0001569157","ORD_TMD":"155211"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().order_.branch_number_ == "06010");
    REQUIRE(result.value().order_.order_number_ == "0001569157");
    REQUIRE(result.value().ack_.order_id_ == "0001569157");
    REQUIRE(result.value().broker_order_time_ == "155211");
  }

  SECTION("a declared broker failure preserves its code and message") {
    auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"insufficient balance"})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "APBK0919");
    REQUIRE(result.error().message_ == "insufficient balance");
  }

  SECTION("a Toss rate-limit refusal carries both hints it publishes") {
    // Toss reports its allowance and its retry delay as headers, and the hints are
    // read where the refusal is built, so no operation can omit one of them.
    network::HttpResponse response(http::status::too_many_requests, 11);
    response.set(http::field::content_type, "application/json");
    response.set("Retry-After", "2");
    response.set("X-RateLimit-Limit", "5");
    response.body() = R"({"error":{"code":"rate-limit-exceeded","message":"slow down"}})";
    response.prepare_payload();

    auto result = tossinvest::TossClientTestAccess::parse_order_ack(
      Result<network::HttpResponse>(std::move(response))
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::RATE_LIMIT);
    REQUIRE(result.error().broker_code_ == "rate-limit-exceeded");
    REQUIRE(result.error().retry_after_.has_value());
    REQUIRE(*result.error().retry_after_ == std::chrono::seconds(2));
    REQUIRE(result.error().rate_limit_.has_value());
    REQUIRE(*result.error().rate_limit_ == 5);
  }

  SECTION("a rate limit declared in the response body is categorized as one") {
    // KIS reports a rate limit as a message code in an HTTP 200 body, with no
    // Retry-After header, so it has to be recognized while parsing or it reaches
    // the caller as an ordinary protocol failure.
    auto ledger = kis::KisClientTestAccess::parse_order_ack(
      json_response(
        R"({"rt_cd":"1","msg_cd":"EGW00215",)"
        R"("msg1":"원장에서 허용 가능한 초당 거래건수를 초과하였습니다."})"
      )
    );
    REQUIRE_FALSE(ledger);
    REQUIRE(ledger.error().category_ == ErrorCategory::RATE_LIMIT);
    REQUIRE(ledger.error().broker_code_ == "EGW00215");

    auto personal = kis::KisClientTestAccess::parse_order_ack(
      json_response(
        R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})"
      )
    );
    REQUIRE_FALSE(personal);
    REQUIRE(personal.error().category_ == ErrorCategory::RATE_LIMIT);
  }

  SECTION("a scheduled trading-control window is not a rate limit") {
    // EGW00317 reports a window during which the endpoint is unavailable. Backing
    // off would misreport a closed window as congestion.
    auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(
        R"({"rt_cd":"1","msg_cd":"EGW00317",)"
        R"("msg1":"09:00 ~ 09:30 시간 동안에는 조회하실 수 없습니다."})"
      )
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "EGW00317");
  }

  SECTION("a successful response without an output object is a protocol error") {
    auto result =
      kis::KisClientTestAccess::parse_order_ack(json_response(R"({"rt_cd":"0"})"));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code_ == "invalid_order_response");
  }

  SECTION("an HTTP failure is reported without parsing a body") {
    auto result =
      kis::KisClientTestAccess::parse_order_ack(json_response("gateway down", 502));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::HTTP);
    REQUIRE(result.error().http_status_ == 502);
  }
}

TEST_CASE(
  "KIS derives an order status from quantity columns alone",
  "[broker][kis][order][status]"
) {
  const auto quantity = [](const char* value) { return Decimal64::parse(value); };
  const auto derive = kis::KisClientTestAccess::derive_order_status;

  SECTION("a rejection with no fill is REJECTED") {
    REQUIRE(
      derive(quantity("10"), quantity("0"), quantity("0"), quantity("10"), false) ==
      OrderStatus::REJECTED
    );
  }

  SECTION("a cancellation with no fill is CANCELED") {
    REQUIRE(
      derive(quantity("10"), quantity("0"), quantity("10"), quantity("0"), true) ==
      OrderStatus::CANCELED
    );
  }

  SECTION("a cancellation after a partial fill stays CANCELED") {
    REQUIRE(
      derive(quantity("10"), quantity("4"), quantity("6"), quantity("0"), true) ==
      OrderStatus::CANCELED
    );
  }

  SECTION("a fully filled order is FILLED") {
    REQUIRE(
      derive(quantity("10"), quantity("10"), quantity("0"), quantity("0"), false) ==
      OrderStatus::FILLED
    );
  }

  SECTION("a partially filled order is PARTIAL_FILLED") {
    REQUIRE(
      derive(quantity("10"), quantity("4"), quantity("6"), quantity("0"), false) ==
      OrderStatus::PARTIAL_FILLED
    );
  }

  SECTION("an unfilled order with a remainder is PENDING") {
    REQUIRE(
      derive(quantity("10"), quantity("0"), quantity("10"), quantity("0"), false) ==
      OrderStatus::PENDING
    );
  }

  SECTION("a row that fits no documented combination is UNKNOWN") {
    REQUIRE(
      derive(quantity("10"), quantity("0"), quantity("0"), quantity("0"), false) ==
      OrderStatus::UNKNOWN
    );
  }
}

TEST_CASE(
  "KIS modifiable-order inquiry reports the amendable quantity",
  "[broker][kis][order]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;

  SECTION("the request targets the documented endpoint and transaction id") {
    const auto request =
      kis::KisClientTestAccess::open_orders_request(account, live, "", "", false);
    REQUIRE(request.method_ == http::verb::get);
    REQUIRE(
      request.target_.find("/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl") == 0
    );
    REQUIRE(header_of(request, "tr_id") == "TTTC0084R");
  }

  SECTION("a returned row normalizes its order and keeps its amendable quantity") {
    auto result = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"0001569139",)"
        R"("orgn_odno":"0001569136","ord_dvsn_name":"limit","pdno":"005930",)"
        R"("prdt_name":"Samsung","rvse_cncl_dvsn_name":"modify","ord_qty":"10",)"
        R"("ord_unpr":"70000","ord_tmd":"131438","tot_ccld_qty":"4","tot_ccld_amt":"280000",)"
        R"("psbl_qty":"6","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00"}]})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().orders_.size() == 1);
    const auto& open_order = result.value().orders_.front();
    REQUIRE(open_order.order_ref_.branch_number_ == "06010");
    REQUIRE(open_order.order_ref_.order_number_ == "0001569139");
    REQUIRE(open_order.original_order_.order_number_ == "0001569136");
    REQUIRE(open_order.amendable_quantity_ == Decimal64::parse("6"));
    REQUIRE(open_order.order_.side_ == OrderSide::BUY);
    REQUIRE(open_order.order_.type_ == OrderType::LIMIT);
    REQUIRE(open_order.order_.price_ == Decimal64::parse("70000"));
    REQUIRE(open_order.order_.status_ == OrderStatus::PARTIAL_FILLED);
  }

  // This section used `23` as its unrecognized code, which this library now
  // recognizes as a mid-price immediate-or-cancel order. The case it was
  // written for is still real -- KIS may return a code added after this
  // release -- so it keeps its shape with a code the workbook does not
  // publish, and the combination it used to reach by accident is asserted
  // deliberately in the section below it.
  SECTION("an order division outside the published table does not fail the listing") {
    auto result = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"1","orgn_odno":"",)"
        R"("ord_dvsn_name":"x","pdno":"005930","prdt_name":"Samsung",)"
        R"("rvse_cncl_dvsn_name":"x","ord_qty":"10","ord_unpr":"0","ord_tmd":"131438",)"
        R"("tot_ccld_qty":"0","tot_ccld_amt":"0","psbl_qty":"10","sll_buy_dvsn_cd":"01",)"
        R"("ord_dvsn_cd":"99"}]})"
      )
    );
    REQUIRE(result);
    const auto& open_order = result.value().orders_.front();
    REQUIRE(open_order.order_.type_ == OrderType::UNKNOWN);
    REQUIRE(open_order.order_.time_in_force_ == TimeInForce::UNKNOWN);
    REQUIRE(open_order.order_.session_ == OrderSession::UNKNOWN);
    // Unknown is not the same as discarded: the caller still gets the code.
    REQUIRE(open_order.order_.shape_code_.view() == "99");
    REQUIRE_FALSE(open_order.order_.price_);
    REQUIRE(open_order.order_.side_ == OrderSide::SELL);
  }

  SECTION("a published order division is normalized into all three facts") {
    auto result = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"1","orgn_odno":"",)"
        R"("ord_dvsn_name":"x","pdno":"005930","prdt_name":"Samsung",)"
        R"("rvse_cncl_dvsn_name":"x","ord_qty":"10","ord_unpr":"0","ord_tmd":"131438",)"
        R"("tot_ccld_qty":"0","tot_ccld_amt":"0","psbl_qty":"10","sll_buy_dvsn_cd":"01",)"
        R"("ord_dvsn_cd":"23"}]})"
      )
    );
    REQUIRE(result);
    const auto& open_order = result.value().orders_.front();
    REQUIRE(open_order.order_.type_ == OrderType::MID_PRICE);
    REQUIRE(open_order.order_.time_in_force_ == TimeInForce::IMMEDIATE_OR_CANCEL);
    REQUIRE(open_order.order_.session_ == OrderSession::REGULAR);
    REQUIRE(open_order.order_.shape_code_.view() == "23");
  }

  SECTION("an empty result is an empty page rather than an error") {
    auto result =
      kis::KisClientTestAccess::parse_open_orders(json_response(R"({"rt_cd":"0","output":[]})"));
    REQUIRE(result);
    REQUIRE(result.value().orders_.empty());
  }

  SECTION("an unknown side is a protocol error rather than a buy order") {
    auto result = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"1","orgn_odno":"",)"
        R"("ord_dvsn_name":"limit","pdno":"005930","prdt_name":"Samsung",)"
        R"("rvse_cncl_dvsn_name":"","ord_qty":"10","ord_unpr":"70000",)"
        R"("ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0",)"
        R"("psbl_qty":"10","sll_buy_dvsn_cd":"03","ord_dvsn_cd":"00"}]})"
      )
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_open_order_response");
  }
}

TEST_CASE(
  "KIS order history normalizes executions and Korean order times",
  "[broker][kis][order][history]"
) {
  const kis::KisAccount account{"12345678", "01"};

  SECTION("the request carries the inclusive range and environment transaction id") {
    kis::KisConfig live;
    const auto request = kis::KisClientTestAccess::order_history_request(
      account, live, kis::KisHistoryRange{Date{2026, 3, 1}, Date{2026, 3, 31}}
    );
    REQUIRE(request.target_.find("INQR_STRT_DT=20260301") != std::string::npos);
    REQUIRE(request.target_.find("INQR_END_DT=20260331") != std::string::npos);
    REQUIRE(request.target_.find("EXCG_ID_DVSN_CD=ALL") != std::string::npos);
    REQUIRE(header_of(request, "tr_id") == "TTTC0081R");

    kis::KisConfig demo;
    demo.is_demo_ = true;
    const auto demo_request = kis::KisClientTestAccess::order_history_request(
      account, demo, kis::KisHistoryRange{Date{2026, 3, 1}, Date{2026, 3, 31}}
    );
    REQUIRE(header_of(demo_request, "tr_id") == "VTTC0081R");
    REQUIRE(demo_request.target_.find("EXCG_ID_DVSN_CD=KRX") != std::string::npos);
  }

  SECTION("a filled row reports its execution and Korea Standard Time order stamp") {
    auto result = kis::KisClientTestAccess::parse_order_history(
      json_response(
        R"({"rt_cd":"0","output1":[{"ord_dt":"20260328","ord_gno_brno":"06010",)"
        R"("odno":"0001569157","orgn_odno":"","ord_dvsn_name":"limit",)"
        R"("sll_buy_dvsn_cd":"02","sll_buy_dvsn_cd_name":"buy","pdno":"005930",)"
        R"("prdt_name":"Samsung","ord_qty":"10","ord_unpr":"70000","ord_tmd":"093000",)"
        R"("tot_ccld_qty":"10","avg_prvs":"70000","cncl_yn":"N","tot_ccld_amt":"700000",)"
        R"("ord_dvsn_cd":"00","cncl_cfrm_qty":"0","rmn_qty":"0","rjct_qty":"0"}]})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().orders_.size() == 1);
    const auto& order = result.value().orders_.front();
    REQUIRE(order.order_id_ == "0001569157");
    REQUIRE(order.status_ == OrderStatus::FILLED);
    REQUIRE(order.side_ == OrderSide::BUY);
    REQUIRE(order.execution_.filled_quantity_ == Decimal64::parse("10"));
    REQUIRE(order.execution_.average_filled_price_ == Decimal64::parse("70000"));
    REQUIRE(order.execution_.filled_amount_->amount_ == Decimal64::parse("700000"));
    REQUIRE(order.execution_.filled_amount_->currency_ == Currency::krw());
    REQUIRE(order.ordered_at_);
    REQUIRE(
      *order.ordered_at_ == *parse_iso8601_timestamp("2026-03-28T09:30:00+09:00")
    );
  }

  SECTION("an unfilled row reports no average price and stays PENDING") {
    auto result = kis::KisClientTestAccess::parse_order_history(
      json_response(
        R"({"rt_cd":"0","output1":[{"ord_dt":"20260328","ord_gno_brno":"06010",)"
        R"("odno":"0001569158","orgn_odno":"","ord_dvsn_name":"limit",)"
        R"("sll_buy_dvsn_cd":"01","sll_buy_dvsn_cd_name":"sell","pdno":"005930",)"
        R"("prdt_name":"Samsung","ord_qty":"10","ord_unpr":"70000","ord_tmd":"093000",)"
        R"("tot_ccld_qty":"0","avg_prvs":"0","cncl_yn":"N","tot_ccld_amt":"0",)"
        R"("ord_dvsn_cd":"00","cncl_cfrm_qty":"0","rmn_qty":"10","rjct_qty":"0"}]})"
      )
    );
    REQUIRE(result);
    const auto& order = result.value().orders_.front();
    REQUIRE(order.status_ == OrderStatus::PENDING);
    REQUIRE_FALSE(order.execution_.average_filled_price_);
    REQUIRE(order.side_ == OrderSide::SELL);
  }

  SECTION("a declared broker failure is reported rather than an empty page") {
    auto result = kis::KisClientTestAccess::parse_order_history(
      json_response(R"({"rt_cd":"1","msg_cd":"APBK0656","msg1":"invalid period"})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code_ == "APBK0656");
  }
}

TEST_CASE(
  "Toss order creation transmits the documented request shape",
  "[broker][toss][order]"
) {
  SECTION("a Korean limit buy carries its price and default validity") {
    const auto request =
      tossinvest::TossClientTestAccess::place_order_request(7, korean_limit_buy());
    REQUIRE(request.method_ == http::verb::post);
    REQUIRE(request.target_ == "/api/v1/orders");
    REQUIRE(header_of(request, "X-Tossinvest-Account") == "7");
    REQUIRE(header_of(request, "Authorization") == "Bearer access-token");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("symbol").as_string() == "005930");
    REQUIRE(body.at("side").as_string() == "BUY");
    REQUIRE(body.at("orderType").as_string() == "LIMIT");
    REQUIRE(body.at("timeInForce").as_string() == "DAY");
    REQUIRE(body.at("quantity").as_string() == "10");
    REQUIRE(body.at("price").as_string() == "70000");
    REQUIRE_FALSE(body.contains("clientOrderId"));
    REQUIRE_FALSE(body.contains("confirmHighValueOrder"));
  }

  SECTION("a market order omits the price entirely") {
    auto order = korean_limit_buy();
    order.type_ = OrderType::MARKET;
    order.price_.reset();
    const auto body =
      json::parse(tossinvest::TossClientTestAccess::place_order_request(7, order).body_)
        .as_object();
    REQUIRE(body.at("orderType").as_string() == "MARKET");
    REQUIRE_FALSE(body.contains("price"));
  }

  SECTION("an idempotency key and high-value confirmation are transmitted when given") {
    auto order = korean_limit_buy();
    order.client_order_id_ = "my-order-001";
    order.confirm_high_value_ = true;
    const auto body =
      json::parse(tossinvest::TossClientTestAccess::place_order_request(7, order).body_)
        .as_object();
    REQUIRE(body.at("clientOrderId").as_string() == "my-order-001");
    REQUIRE(body.at("confirmHighValueOrder").as_bool());
  }

  SECTION("a US closing-auction limit order uses the documented wire code") {
    auto order = korean_limit_buy();
    order.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
    order.session_ = OrderSession::CLOSING_AUCTION;
    order.price_ = Decimal64::parse("185.5");
    const auto body =
      json::parse(tossinvest::TossClientTestAccess::place_order_request(7, order).body_)
        .as_object();
    REQUIRE(body.at("timeInForce").as_string() == "CLS");
    REQUIRE(body.at("price").as_string() == "185.5");
  }
}

TEST_CASE(
  "Toss amendment and cancellation target one order",
  "[broker][toss][order]"
) {
  SECTION("a modification sends only the fields it changes") {
    tossinvest::TossAmendment amendment;
    amendment.order_id_ = "order-1";
    amendment.type_ = OrderType::LIMIT;
    amendment.quantity_ = Decimal64::parse("15");
    amendment.price_ = Decimal64::parse("71000");
    const auto request =
      tossinvest::TossClientTestAccess::replace_order_request(7, amendment);
    REQUIRE(request.method_ == http::verb::post);
    REQUIRE(request.target_ == "/api/v1/orders/order-1/modify");
    const auto body = json::parse(request.body_).as_object();
    REQUIRE(body.at("orderType").as_string() == "LIMIT");
    REQUIRE(body.at("quantity").as_string() == "15");
    REQUIRE(body.at("price").as_string() == "71000");
  }

  SECTION("a US modification may omit the quantity Toss forbids") {
    tossinvest::TossAmendment amendment;
    amendment.order_id_ = "order-1";
    amendment.price_ = Decimal64::parse("185.5");
    const auto body =
      json::parse(tossinvest::TossClientTestAccess::replace_order_request(7, amendment).body_)
        .as_object();
    REQUIRE_FALSE(body.contains("quantity"));
  }

  SECTION("a cancellation posts an empty body to the order's cancel route") {
    const auto request = tossinvest::TossClientTestAccess::cancel_order_request(7, "order-1");
    REQUIRE(request.method_ == http::verb::post);
    REQUIRE(request.target_ == "/api/v1/orders/order-1/cancel");
    REQUIRE(request.body_ == "{}");
  }
}

TEST_CASE(
  "Toss order listing omits paging where Toss ignores it",
  "[broker][toss][order]"
) {
  tossinvest::TossOrderQuery query;
  query.account_sequence_ = 7;
  query.cursor_ = "cursor value";
  query.limit_ = 50;

  SECTION("an open listing sends neither cursor nor limit") {
    query.scope_ = tossinvest::TossOrderScope::OPEN;
    const auto request = tossinvest::TossClientTestAccess::orders_request(query);
    REQUIRE(request.target_.find("status=OPEN") != std::string::npos);
    REQUIRE(request.target_.find("cursor=") == std::string::npos);
    REQUIRE(request.target_.find("limit=") == std::string::npos);
  }

  SECTION("a closed listing pages and encodes its cursor") {
    query.scope_ = tossinvest::TossOrderScope::CLOSED;
    query.symbol_ = "005930";
    query.from_ = Date{2026, 3, 1};
    query.to_ = Date{2026, 3, 31};
    const auto request = tossinvest::TossClientTestAccess::orders_request(query);
    REQUIRE(request.target_.find("status=CLOSED") != std::string::npos);
    REQUIRE(request.target_.find("cursor=cursor%20value") != std::string::npos);
    REQUIRE(request.target_.find("limit=50") != std::string::npos);
    REQUIRE(request.target_.find("symbol=005930") != std::string::npos);
    REQUIRE(request.target_.find("from=2026-03-01") != std::string::npos);
    REQUIRE(request.target_.find("to=2026-03-31") != std::string::npos);
  }
}

TEST_CASE(
  "Toss rejects unusable order instructions before any network I/O",
  "[broker][toss][order][validation]"
) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client =
    std::make_shared<tossinvest::TossClient>(ioc.get_executor(), context, tossinvest::TossConfig{});

  const auto expect_rejection = [&](OrderRequest order, std::string_view code) {
    bool completed = false;
    client->async_place_order(7, std::move(order), [&](Result<OrderAck> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      REQUIRE(result.error().broker_code_ == code);
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  };

  SECTION("a fractional quantity is rejected") {
    auto order = korean_limit_buy();
    order.quantity_ = Decimal64::parse("0.5");
    expect_rejection(std::move(order), "invalid_quantity");
  }

  SECTION("an execution condition Toss does not publish is refused before I/O") {
    for (const auto condition :
         {TimeInForce::IMMEDIATE_OR_CANCEL, TimeInForce::FILL_OR_KILL}) {
      auto order = korean_limit_buy();
      order.time_in_force_ = condition;
      expect_rejection(std::move(order), "unsupported_time_in_force");
    }
  }

  SECTION("a price basis Toss does not publish is refused before I/O") {
    for (const auto type :
         {OrderType::MID_PRICE, OrderType::BEST_LIMIT, OrderType::PRIMARY_LIMIT,
          OrderType::CONDITIONAL_LIMIT, OrderType::STOP_LIMIT, OrderType::CLOSING_PRICE}) {
      auto order = korean_limit_buy();
      order.type_ = type;
      order.price_.reset();
      expect_rejection(std::move(order), "unsupported_order_type");
    }
  }

  SECTION("a session Toss does not publish is refused before I/O") {
    for (const auto session :
         {OrderSession::PRE_MARKET, OrderSession::AFTER_HOURS, OrderSession::AFTER_MARKET,
          OrderSession::GTP, OrderSession::AFTER_HOURS_AUCTION}) {
      auto order = korean_limit_buy();
      order.session_ = session;
      expect_rejection(std::move(order), "unsupported_session");
    }
  }

  SECTION("a modification to a basis Toss cannot express is refused, not coerced to LIMIT") {
    tossinvest::TossAmendment amendment;
    amendment.order_id_ = "abc";
    amendment.type_ = OrderType::MID_PRICE;
    amendment.quantity_ = Decimal64::parse("1");
    // The modify path had no basis check at all, and `order_type_code` answered
    // "LIMIT" for everything that was not MARKET, so this reached Toss as a
    // limit order with no price.
    REQUIRE(tossinvest::TossClientTestAccess::validate_amendment_shape(amendment.type_));
  }

  SECTION("a malformed idempotency key is rejected") {
    auto order = korean_limit_buy();
    order.client_order_id_ = "not valid!";
    expect_rejection(std::move(order), "invalid_client_order_id");
  }

  SECTION("a fractional won price is rejected") {
    auto order = korean_limit_buy();
    order.price_ = Decimal64::parse("70000.5");
    expect_rejection(std::move(order), "invalid_price");
  }

  SECTION("a closing-auction session on a Korean order is rejected") {
    auto order = korean_limit_buy();
    order.session_ = OrderSession::CLOSING_AUCTION;
    expect_rejection(std::move(order), "unsupported_session");
  }

  SECTION("an opening-auction session on a US order is rejected") {
    auto order = korean_limit_buy();
    order.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
    order.session_ = OrderSession::OPENING_AUCTION;
    expect_rejection(std::move(order), "unsupported_session");
  }

  SECTION("a non-positive account sequence is rejected") {
    bool completed = false;
    client->async_place_order(0, korean_limit_buy(), [&completed](Result<OrderAck> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_account");
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  }

  SECTION("an empty order identifier is rejected on cancellation") {
    bool completed = false;
    client->async_cancel_order(7, "", [&completed](Result<OrderAck> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_order_reference");
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  }

  SECTION("an invalid order-list date is rejected") {
    tossinvest::TossOrderQuery query;
    query.account_sequence_ = 7;
    query.from_ = Date{2026, 2, 29};
    bool completed = false;
    client->async_orders(std::move(query), [&completed](Result<OrderPage> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_date_range");
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  }

  SECTION("a reversed order-list range is rejected") {
    tossinvest::TossOrderQuery query;
    query.account_sequence_ = 7;
    query.from_ = Date{2026, 3, 31};
    query.to_ = Date{2026, 3, 1};
    bool completed = false;
    client->async_orders(std::move(query), [&completed](Result<OrderPage> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_date_range");
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
  }
}

TEST_CASE(
  "Toss order responses normalize fills and tolerate unknown codes",
  "[broker][toss][order]"
) {
  SECTION("an acknowledgement echoes the idempotency key when one was sent") {
    auto result = tossinvest::TossClientTestAccess::parse_order_ack(
      json_response(R"({"result":{"orderId":"order-1","clientOrderId":"my-order-001"}})")
    );
    REQUIRE(result);
    REQUIRE(result.value().order_id_ == "order-1");
    REQUIRE(result.value().client_order_id_ == "my-order-001");
  }

  SECTION("a conflict preserves its broker code and request id") {
    auto result = tossinvest::TossClientTestAccess::parse_order_ack(
      json_response(
        R"({"error":{"requestId":"01HXYZ","code":"already-filled",)"
        R"("message":"already filled"}})",
        409
      )
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().http_status_ == 409);
    REQUIRE(result.error().broker_code_ == "already-filled");
    REQUIRE(result.error().request_id_ == "01HXYZ");
  }

  SECTION("a rate-limited response is categorized as such") {
    auto result = tossinvest::TossClientTestAccess::parse_order_ack(
      json_response(R"({"error":{"code":"too-many-requests","message":"slow down"}})", 429)
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::RATE_LIMIT);
  }

  SECTION("an amount-based order keeps the amount it committed") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[)"
        R"({"orderId":"c","symbol":"AAPL","side":"BUY","orderType":"MARKET",)"
        R"("timeInForce":"CLS","status":"PENDING","price":null,"quantity":"0",)"
        R"("orderAmount":"100.5","currency":"USD","orderedAt":"2026-03-29T10:00:00+09:00",)"
        R"("canceledAt":null,"execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}}],"nextCursor":null,"hasNext":false}})"
      )
    );
    REQUIRE(result);
    const auto& order = result.value().orders_.front();
    // An amount order states money and lets the fill decide the quantity. The
    // snapshot dropped `orderAmount` entirely, so it reported a quantity of
    // zero and could not say what the order had actually committed.
    REQUIRE(order.order_amount_ == std::optional<Decimal64>(Decimal64::parse("100.5")));
    REQUIRE(order.quantity_ == Decimal64::parse("0"));
    REQUIRE(order.session_ == OrderSession::CLOSING_AUCTION);
    REQUIRE(order.time_in_force_ == TimeInForce::DAY);
    REQUIRE(order.shape_code_.view() == "CLS");
  }

  SECTION("a validity code Toss adds later is unknown in both facts, not assumed to be DAY") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[)"
        R"({"orderId":"d","symbol":"005930","side":"BUY","orderType":"LIMIT",)"
        R"("timeInForce":"XYZ","status":"PENDING","price":"70000","quantity":"1",)"
        R"("orderAmount":null,"currency":"KRW","orderedAt":"2026-03-29T09:30:00+09:00",)"
        R"("canceledAt":null,"execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}}],"nextCursor":null,"hasNext":false}})"
      )
    );
    REQUIRE(result);
    const auto& order = result.value().orders_.front();
    REQUIRE(order.session_ == OrderSession::UNKNOWN);
    REQUIRE(order.time_in_force_ == TimeInForce::UNKNOWN);
    REQUIRE(order.shape_code_.view() == "XYZ");
  }

  SECTION("a listing normalizes a Korean and a US order independently") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[)"
        R"({"orderId":"a","symbol":"005930","side":"BUY","orderType":"LIMIT",)"
        R"("timeInForce":"DAY","status":"PENDING","price":"70000","quantity":"10",)"
        R"("orderAmount":null,"currency":"KRW","orderedAt":"2026-03-29T09:30:00+09:00",)"
        R"("canceledAt":null,"execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}},)"
        R"({"orderId":"b","symbol":"AAPL","side":"SELL","orderType":"LIMIT",)"
        R"("timeInForce":"DAY","status":"PARTIAL_FILLED","price":"185.5","quantity":"5",)"
        R"("orderAmount":null,"currency":"USD","orderedAt":"2026-03-29T10:00:00+09:00",)"
        R"("canceledAt":null,"execution":{"filledQuantity":"2",)"
        R"("averageFilledPrice":"185.25","filledAmount":"370.5","commission":"0.66",)"
        R"("tax":"0","filledAt":"2026-03-29T10:00:05+09:00","settlementDate":"2026-03-31"}}],)"
        R"("nextCursor":null,"hasNext":false}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().orders_.size() == 2);
    REQUIRE_FALSE(result.value().next_cursor_);

    const auto& korean = result.value().orders_.front();
    // A quantity order reports no amount, and a day order is the regular
    // session -- Toss spells the session on its `timeInForce` field.
    REQUIRE_FALSE(korean.order_amount_);
    REQUIRE(korean.session_ == OrderSession::REGULAR);
    REQUIRE(korean.time_in_force_ == TimeInForce::DAY);
    REQUIRE(korean.instrument_.country_ == Country::KR);
    REQUIRE(korean.currency_ == Currency::krw());
    REQUIRE(korean.status_ == OrderStatus::PENDING);
    REQUIRE(korean.execution_.filled_quantity_ == Decimal64::parse("0"));
    REQUIRE_FALSE(korean.execution_.filled_amount_);
    REQUIRE_FALSE(korean.canceled_at_);

    const auto& american = result.value().orders_.back();
    REQUIRE(american.instrument_.country_ == Country::US);
    REQUIRE(american.currency_ == Currency::usd());
    REQUIRE(american.status_ == OrderStatus::PARTIAL_FILLED);
    REQUIRE(american.execution_.commission_->currency_ == Currency::usd());
    REQUIRE(american.execution_.commission_->amount_ == Decimal64::parse("0.66"));
    REQUIRE(american.execution_.settlement_date_ == Date{2026, 3, 31});
    REQUIRE(american.execution_.filled_at_);
  }

  SECTION("an unknown status, type, and validity do not fail the listing") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[{"orderId":"a","symbol":"005930","side":"BUY",)"
        R"("orderType":"TRAILING_STOP","timeInForce":"GTC","status":"SUSPENDED",)"
        R"("quantity":"10","currency":"KRW","orderedAt":"2026-03-29T09:30:00+09:00",)"
        R"("execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}}],"nextCursor":"next-page","hasNext":true}})"
      )
    );
    REQUIRE(result);
    const auto& order = result.value().orders_.front();
    REQUIRE(order.type_ == OrderType::UNKNOWN);
    REQUIRE(order.time_in_force_ == TimeInForce::UNKNOWN);
    REQUIRE(order.status_ == OrderStatus::UNKNOWN);
    REQUIRE_FALSE(order.price_);
    REQUIRE(result.value().next_cursor_ == "next-page");
  }

  SECTION("an unknown side is a protocol error rather than a buy order") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[{"orderId":"a","symbol":"005930","side":"HOLD",)"
        R"("orderType":"LIMIT","timeInForce":"DAY","status":"PENDING","price":"70000",)"
        R"("quantity":"10","currency":"KRW","orderedAt":"2026-03-29T09:30:00+09:00",)"
        R"("execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}}],"nextCursor":null,"hasNext":false}})"
      )
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_orders_response");
  }

  SECTION("an unsupported currency is not silently classified as Korean") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(
        R"({"result":{"orders":[{"orderId":"a","symbol":"7203","side":"BUY",)"
        R"("orderType":"LIMIT","timeInForce":"DAY","status":"PENDING","price":"1000",)"
        R"("quantity":"10","currency":"JPY","orderedAt":"2026-03-29T09:30:00+09:00",)"
        R"("execution":{"filledQuantity":"0","averageFilledPrice":null,)"
        R"("filledAmount":null,"commission":null,"tax":null,"filledAt":null,)"
        R"("settlementDate":null}}],"nextCursor":null,"hasNext":false}})"
      )
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_orders_response");
  }

  SECTION("an empty listing is an empty page rather than an error") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(R"({"result":{"orders":[],"nextCursor":null,"hasNext":false}})")
    );
    REQUIRE(result);
    REQUIRE(result.value().orders_.empty());
  }

  SECTION("a malformed order object is a protocol error") {
    auto result = tossinvest::TossClientTestAccess::parse_orders(
      json_response(R"({"result":{"orders":[{"orderId":"a"}],"hasNext":false}})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_orders_response");
  }

  SECTION("a single order detail normalizes the same way as a listing row") {
    auto result = tossinvest::TossClientTestAccess::parse_order(
      json_response(
        R"({"result":{"orderId":"a","symbol":"005930","side":"BUY","orderType":"MARKET",)"
        R"("timeInForce":"DAY","status":"FILLED","price":null,"quantity":"10",)"
        R"("currency":"KRW","orderedAt":"2026-03-28T09:30:00+09:00",)"
        R"("canceledAt":null,"execution":{"filledQuantity":"10",)"
        R"("averageFilledPrice":"70000","filledAmount":"700000","commission":"1400",)"
        R"("tax":"0","filledAt":"2026-03-28T09:31:15+09:00","settlementDate":"2026-03-30"}}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().type_ == OrderType::MARKET);
    REQUIRE_FALSE(result.value().price_);
    REQUIRE(result.value().status_ == OrderStatus::FILLED);
    REQUIRE(result.value().execution_.commission_->amount_ == Decimal64::parse("1400"));
  }
}

TEST_CASE(
  "A dropped connection during an order POST is never replayed",
  "[broker][order][tls][retry]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;
  auto order_request = kis::KisClientTestAccess::place_order_request(
    account, live, korean_limit_buy(), "00"
  );
  REQUIRE(order_request.method_ == http::verb::post);
  REQUIRE_FALSE(order_request.body_.empty());

  // The transport decides replay policy from the method, so the real order
  // body is sent against a server that drops the connection mid-response.
  order_request.target_ = "/drop-post";

  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  bool completed = false;
  client->async_request(
    std::move(order_request),
    [&completed](Result<network::HttpResponse> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().category_ == ErrorCategory::TRANSPORT);
      completed = true;
    }
  );

  ioc.run();
  REQUIRE(completed);
  REQUIRE(server.requestCount("/drop-post") == 1);
  REQUIRE(server.connectionCount() == 1);
}

TEST_CASE(
  "Toss candles normalize into ascending bars with its stamp convention",
  "[broker][tossinvest][bars]"
) {
  BarQuery query;
  query.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  std::vector<Bar> bars;
  std::optional<std::string> cursor;
  Currency currency;

  SECTION("a daily page is stamped at the open and reports its cursor") {
    query.interval_ = BarInterval::ONE_DAY;
    // The documented response example, newest first.
    const auto failure = tossinvest::TossClientTestAccess::parse_bars_page(
      json_response(
        R"({"result":{"candles":[)"
        R"({"timestamp":"2026-03-25T00:00:00+09:00","openPrice":"71600","highPrice":"72300",)"
        R"("lowPrice":"71500","closePrice":"72000","volume":"3521000","currency":"KRW"},)"
        R"({"timestamp":"2026-03-24T00:00:00+09:00","openPrice":"71200","highPrice":"71800",)"
        R"("lowPrice":"71000","closePrice":"71600","volume":"2984000","currency":"KRW"}],)"
        R"("nextBefore":"2026-03-24T00:00:00+09:00"}})"
      ),
      query,
      bars,
      cursor,
      currency
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 2);

    // Accumulated in the order Toss sent them: newest first, reversed once at the end.
    REQUIRE(bars.front().close_ == Decimal64::parse("72000"));
    REQUIRE(bars.back().close_ == Decimal64::parse("71600"));

    // The stamp and currency belong to the series and are asserted where the series
    // is built; a bar carries neither.
    REQUIRE(bars.front().open_.has_value());
    REQUIRE(*bars.front().open_ == Decimal64::parse("71600"));
    REQUIRE(bars.front().volume_ == Decimal64::parse("3521000"));
    REQUIRE(currency == Currency::krw());
    REQUIRE(cursor == std::string("2026-03-24T00:00:00+09:00"));
  }

  SECTION("a minute page is stamped at the close") {
    query.interval_ = BarInterval::ONE_MINUTE;
    const auto failure = tossinvest::TossClientTestAccess::parse_bars_page(
      json_response(
        R"({"result":{"candles":[)"
        R"({"timestamp":"2026-03-25T09:32:00+09:00","openPrice":"72000","highPrice":"72100",)"
        R"("lowPrice":"71950","closePrice":"72050","volume":"15200","currency":"KRW"}],)"
        R"("nextBefore":null}})"
      ),
      query,
      bars,
      cursor,
      currency
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 1);

    // A null cursor is the last page, and is not a cursor of its own.
    REQUIRE_FALSE(cursor.has_value());
  }

  SECTION("an empty page reports no cursor so paging cannot continue forever") {
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = tossinvest::TossClientTestAccess::parse_bars_page(
      json_response(R"({"result":{"candles":[],"nextBefore":"2026-03-24T00:00:00+09:00"}})"),
      query,
      bars,
      cursor,
      currency
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.empty());
    REQUIRE_FALSE(cursor.has_value());
  }

  SECTION("a malformed candle is a protocol error") {
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = tossinvest::TossClientTestAccess::parse_bars_page(
      json_response(R"({"result":{"candles":[{"timestamp":"not-a-time"}]}})"),
      query,
      bars,
      cursor,
      currency
    );
    REQUIRE(failure);
    REQUIRE(failure->category_ == ErrorCategory::PROTOCOL);
    REQUIRE(failure->broker_code_ == "invalid_candle_response");
  }
}

TEST_CASE(
  "KIS bars normalize from its period and minute endpoints",
  "[broker][kis][bars]"
) {
  BarQuery query;
  query.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  std::vector<Bar> bars;

  SECTION("a period bar is stamped by its trading date") {
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = kis::KisClientTestAccess::parse_bars_page(
      json_response(
        R"({"rt_cd":"0","msg_cd":"MCA00000","output2":[)"
        R"({"stck_bsop_date":"20260325","stck_clpr":"72000","stck_oprc":"71600",)"
        R"("stck_hgpr":"72300","stck_lwpr":"71500","acml_vol":"3521000",)"
        R"("acml_tr_pbmn":"253000000000","mod_yn":"N"}]})"
      ),
      query,
      false,
      bars
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 1);
    REQUIRE(*bars.front().open_ == Decimal64::parse("71600"));
    REQUIRE(bars.front().close_ == Decimal64::parse("72000"));
    REQUIRE(bars.front().volume_ == Decimal64::parse("3521000"));
    REQUIRE(bars.front().traded_value_.has_value());
  }

  SECTION("a trading day with no trade has no open, and that is not zero") {
    // KIS sets mod_yn when the day produced no execution, so there is no open price
    // to report. Substituting zero or the previous close would invent a bar.
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = kis::KisClientTestAccess::parse_bars_page(
      json_response(
        R"({"rt_cd":"0","output2":[)"
        R"({"stck_bsop_date":"20260325","stck_clpr":"72000","stck_oprc":"0",)"
        R"("stck_hgpr":"72000","stck_lwpr":"72000","acml_vol":"0","mod_yn":"Y"}]})"
      ),
      query,
      false,
      bars
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 1);
    REQUIRE_FALSE(bars.front().open_.has_value());
    REQUIRE(bars.front().close_ == Decimal64::parse("72000"));
  }

  SECTION("a minute bar reads the execution time, price, and volume columns") {
    query.interval_ = BarInterval::ONE_MINUTE;
    const auto failure = kis::KisClientTestAccess::parse_bars_page(
      json_response(
        R"({"rt_cd":"0","output2":[)"
        R"({"stck_bsop_date":"20260325","stck_cntg_hour":"093200","stck_prpr":"72050",)"
        R"("stck_oprc":"72000","stck_hgpr":"72100","stck_lwpr":"71950","cntg_vol":"15200"}]})"
      ),
      query,
      true,
      bars
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 1);
    REQUIRE(bars.front().close_ == Decimal64::parse("72050"));
    REQUIRE(bars.front().volume_ == Decimal64::parse("15200"));
  }

  SECTION("blank padding rows are skipped rather than parsed") {
    // KIS pads a short page with empty rows instead of shortening the array.
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = kis::KisClientTestAccess::parse_bars_page(
      json_response(
        R"({"rt_cd":"0","output2":[)"
        R"({"stck_bsop_date":"20260325","stck_clpr":"72000","stck_oprc":"71600",)"
        R"("stck_hgpr":"72300","stck_lwpr":"71500","acml_vol":"3521000","mod_yn":"N"},)"
        R"({"stck_bsop_date":"","stck_clpr":"","stck_oprc":"","stck_hgpr":"",)"
        R"("stck_lwpr":"","acml_vol":""}]})"
      ),
      query,
      false,
      bars
    );
    REQUIRE_FALSE(failure);
    REQUIRE(bars.size() == 1);
  }

  SECTION("a declared broker failure ends the page") {
    query.interval_ = BarInterval::ONE_DAY;
    const auto failure = kis::KisClientTestAccess::parse_bars_page(
      json_response(R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"no data"})"), query, false, bars
    );
    REQUIRE(failure);
    REQUIRE(failure->broker_code_ == "APBK0919");
    REQUIRE(bars.empty());
  }
}

TEST_CASE(
  "a KIS period page spans enough calendar days to hold a page of bars",
  "[broker][kis][bars]"
) {
  // KIS selects period bars by date range, so the range has to be sized in calendar
  // days. A fixed span would under-deliver badly for the longer intervals: a
  // hundred monthly bars would need thirty pages and a hundred yearly bars would
  // exhaust the page budget, returning a fraction of the request silently.
  using kis::KisClientTestAccess;
  REQUIRE(KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_DAY) == 1);
  REQUIRE(KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_WEEK) == 7);
  REQUIRE(KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_MONTH) == 28);
  REQUIRE(KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_YEAR) == 365);

  // The multiplier is the shortest a bar of that interval can be, so one page's
  // range can never hold more than the hundred rows KIS returns.
  REQUIRE(100 * KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_WEEK) == 700);
  REQUIRE(100 * KisClientTestAccess::shortest_days_per_bar(BarInterval::ONE_YEAR) == 36500);
}

TEST_CASE(
  "an order book is ordered best first whatever order the broker sent",
  "[broker][depth]"
) {
  const auto samsung = InstrumentId{Country::KR, Venue::COMBINED, "005930"};

  SECTION("Toss asks arrive descending in its own example and are still corrected") {
    // Toss documents `asks` as ascending and its worked example lists them
    // descending. Trusting either would be a guess, and taking the best offer off
    // the wrong end of the book makes the spread negative.
    auto result = tossinvest::TossClientTestAccess::parse_depth(
      samsung,
      json_response(
        R"({"result":{"timestamp":"2026-03-25T09:30:00.123+09:00","currency":"KRW",)"
        R"("asks":[{"price":"72300","volume":"1200"},{"price":"72200","volume":"3400"},)"
        R"({"price":"72100","volume":"8500"}],)"
        R"("bids":[{"price":"72000","volume":"5200"},{"price":"71900","volume":"4100"},)"
        R"({"price":"71800","volume":"2700"}]}})"
      )
    );
    REQUIRE(result);
    const auto& book = result.value();
    REQUIRE(book.asks_.size() == 3);
    REQUIRE(book.bids_.size() == 3);

    REQUIRE(book.best_ask()->price_ == Decimal64::parse("72100"));
    REQUIRE(book.best_ask()->quantity_ == Decimal64::parse("8500"));
    REQUIRE(book.best_bid()->price_ == Decimal64::parse("72000"));
    REQUIRE(book.asks_.back().price_ == Decimal64::parse("72300"));
    REQUIRE(book.bids_.back().price_ == Decimal64::parse("71800"));

    REQUIRE(book.spread()->str() == "100");
    REQUIRE(book.mid()->str() == "72050");
    REQUIRE(book.currency_ == Currency::krw());
    REQUIRE(book.exchange_time_.has_value());
  }

  SECTION("a United States book keeps its own currency and decimal prices") {
    const auto apple = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
    auto result = tossinvest::TossClientTestAccess::parse_depth(
      apple,
      json_response(
        R"({"result":{"currency":"USD",)"
        R"("asks":[{"price":"185.75","volume":"250"},{"price":"185.70","volume":"410"}],)"
        R"("bids":[{"price":"185.65","volume":"180"},{"price":"185.60","volume":"320"}]}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().best_ask()->price_ == Decimal64::parse("185.70"));
    REQUIRE(result.value().best_bid()->price_ == Decimal64::parse("185.65"));
    REQUIRE(result.value().spread()->str() == "0.05");
    REQUIRE(result.value().currency_ == Currency::usd());

    // No timestamp is reported for this book, and that is not the epoch.
    REQUIRE_FALSE(result.value().exchange_time_.has_value());
  }

  SECTION("an empty side yields no best price rather than zero") {
    auto result = tossinvest::TossClientTestAccess::parse_depth(
      samsung, json_response(R"({"result":{"currency":"KRW","asks":[],"bids":[]}})")
    );
    REQUIRE(result);
    REQUIRE_FALSE(result.value().best_bid().has_value());
    REQUIRE_FALSE(result.value().best_ask().has_value());
    REQUIRE_FALSE(result.value().spread().has_value());
  }

  SECTION("KIS numbered level fields normalize into the same book") {
    // KIS sends ten flat field groups rather than arrays, nearest level first.
    auto result = kis::KisClientTestAccess::parse_depth(
      samsung,
      json_response(
        R"({"rt_cd":"0","msg_cd":"MCA00000","output1":{"aspr_acpt_hour":"093000",)"
        R"("askp1":"72100","askp2":"72200","askp3":"72300",)"
        R"("askp_rsqn1":"8500","askp_rsqn2":"3400","askp_rsqn3":"1200",)"
        R"("bidp1":"72000","bidp2":"71900","bidp3":"71800",)"
        R"("bidp_rsqn1":"5200","bidp_rsqn2":"4100","bidp_rsqn3":"2700"}})"
      )
    );
    REQUIRE(result);
    const auto& book = result.value();
    REQUIRE(book.asks_.size() == 3);
    REQUIRE(book.bids_.size() == 3);
    REQUIRE(book.best_ask()->price_ == Decimal64::parse("72100"));
    REQUIRE(book.best_bid()->price_ == Decimal64::parse("72000"));
    REQUIRE(book.spread()->str() == "100");
    REQUIRE(book.exchange_time_.has_value());
  }

  SECTION("a KIS level that is not resting is skipped, not recorded at zero") {
    // Outside trading hours KIS reports the far levels as zero or blank. An offer
    // at nothing would be the best offer in the book.
    auto result = kis::KisClientTestAccess::parse_depth(
      samsung,
      json_response(
        R"({"rt_cd":"0","output1":{"askp1":"72100","askp2":"0","askp3":"",)"
        R"("askp_rsqn1":"8500","askp_rsqn2":"0","askp_rsqn3":"",)"
        R"("bidp1":"72000","bidp2":"0","bidp_rsqn1":"5200","bidp_rsqn2":"0"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().asks_.size() == 1);
    REQUIRE(result.value().bids_.size() == 1);
    REQUIRE(result.value().best_ask()->price_ == Decimal64::parse("72100"));
  }

  SECTION("a declared KIS failure is reported rather than an empty book") {
    auto result = kis::KisClientTestAccess::parse_depth(
      samsung, json_response(R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"no data"})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code_ == "APBK0919");
  }
}

TEST_CASE("price bounds distinguish a market with no limit from a limit of zero") {
  SECTION("Toss reports both bounds for a Korean equity") {
    auto result = tossinvest::TossClientTestAccess::parse_price_limits(
      json_response(
        R"({"result":{"timestamp":"2026-03-25T09:30:00.123+09:00",)"
        R"("upperLimitPrice":"93000","lowerLimitPrice":"50400","currency":"KRW"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(*result.value().upper_ == Decimal64::parse("93000"));
    REQUIRE(*result.value().lower_ == Decimal64::parse("50400"));
    REQUIRE(result.value().currency_ == Currency::krw());
    REQUIRE(result.value().exchange_time_.has_value());
  }

  SECTION("Toss reports null bounds for a market without a price limit") {
    // A bound of zero would read as "may not trade above nothing", which is the
    // opposite of what an unlimited market means.
    auto result = tossinvest::TossClientTestAccess::parse_price_limits(
      json_response(
        R"({"result":{"timestamp":"2026-03-25T22:30:00.456+09:00",)"
        R"("upperLimitPrice":null,"lowerLimitPrice":null,"currency":"USD"}})"
      )
    );
    REQUIRE(result);
    REQUIRE_FALSE(result.value().upper_.has_value());
    REQUIRE_FALSE(result.value().lower_.has_value());
    REQUIRE(result.value().currency_ == Currency::usd());
  }

  SECTION("KIS reports both bounds on its current-price inquiry") {
    auto result = kis::KisClientTestAccess::parse_price_limits(
      json_response(
        R"({"rt_cd":"0","msg_cd":"MCA00000","output":{"stck_prpr":"72000",)"
        R"("stck_mxpr":"93000","stck_llam":"50400"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(*result.value().upper_ == Decimal64::parse("93000"));
    REQUIRE(*result.value().lower_ == Decimal64::parse("50400"));
    REQUIRE(result.value().currency_ == Currency::krw());
  }

  SECTION("a blank or zero KIS bound is absent rather than a bound of nothing") {
    auto result = kis::KisClientTestAccess::parse_price_limits(
      json_response(R"({"rt_cd":"0","output":{"stck_mxpr":"0","stck_llam":""}})")
    );
    REQUIRE(result);
    REQUIRE_FALSE(result.value().upper_.has_value());
    REQUIRE_FALSE(result.value().lower_.has_value());
  }
}

TEST_CASE("recent executions normalize from both brokers") {
  SECTION("Toss returns a bare array of prints") {
    auto result = tossinvest::TossClientTestAccess::parse_trades(
      json_response(
        R"({"result":[)"
        R"({"price":"72000","volume":"120","timestamp":"2026-03-25T09:30:42.000+09:00",)"
        R"("currency":"KRW"},)"
        R"({"price":"71900","volume":"50","timestamp":"2026-03-25T09:30:41.500+09:00",)"
        R"("currency":"KRW"}]})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().size() == 2);
    REQUIRE(result.value().front().price_ == Decimal64::parse("72000"));
    REQUIRE(result.value().front().quantity_ == Decimal64::parse("120"));
    REQUIRE(result.value().front().currency_ == Currency::krw());
    REQUIRE(result.value().front().exchange_time_.has_value());
  }

  SECTION("KIS pads its window with blank rows, which are skipped") {
    auto result = kis::KisClientTestAccess::parse_trades(
      json_response(
        R"({"rt_cd":"0","output":[)"
        R"({"stck_cntg_hour":"093042","stck_prpr":"72000","cntg_vol":"120"},)"
        R"({"stck_cntg_hour":"093041","stck_prpr":"71900","cntg_vol":"50"},)"
        R"({"stck_cntg_hour":"","stck_prpr":"","cntg_vol":""}]})"
      ),
      0
    );
    REQUIRE(result);
    REQUIRE(result.value().size() == 2);
    REQUIRE(result.value().front().price_ == Decimal64::parse("72000"));
    REQUIRE(result.value().front().currency_ == Currency::krw());
  }

  SECTION("a KIS count trims the fixed window KIS returns") {
    // KIS answers with a window of its own choosing rather than a requested size,
    // so the count trims what arrived instead of asking for less.
    auto result = kis::KisClientTestAccess::parse_trades(
      json_response(
        R"({"rt_cd":"0","output":[)"
        R"({"stck_cntg_hour":"093042","stck_prpr":"72000","cntg_vol":"120"},)"
        R"({"stck_cntg_hour":"093041","stck_prpr":"71900","cntg_vol":"50"}]})"
      ),
      1
    );
    REQUIRE(result);
    REQUIRE(result.value().size() == 1);
  }
}

TEST_CASE("a batch quote keeps the instrument identities the caller stated") {
  const std::vector<InstrumentId> requested = {
    InstrumentId{Country::KR, Venue::KRX, "005930"},
    InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"},
  };

  SECTION("each row is matched back to the instrument it was asked about") {
    // Rebuilding an instrument from the symbol alone would lose the country and
    // venue the caller stated, which is how a KRX-only quote becomes ambiguous.
    auto result = tossinvest::TossClientTestAccess::parse_prices(
      requested,
      json_response(
        R"({"result":[)"
        R"({"symbol":"005930","timestamp":"2026-03-25T09:30:00.123+09:00",)"
        R"("lastPrice":"72000","currency":"KRW"},)"
        R"({"symbol":"AAPL","timestamp":"2026-03-25T22:30:00.456+09:00",)"
        R"("lastPrice":"185.70","currency":"USD"}]})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().size() == 2);
    REQUIRE(result.value()[0].instrument_.venue_ == Venue::KRX);
    REQUIRE(result.value()[0].last_price_ == Decimal64::parse("72000"));
    REQUIRE(result.value()[1].instrument_.country_ == Country::US);
    REQUIRE(result.value()[1].currency_ == Currency::usd());
  }

  SECTION("a row for a symbol that was not asked for is a protocol error") {
    auto result = tossinvest::TossClientTestAccess::parse_prices(
      requested,
      json_response(R"({"result":[{"symbol":"000660","lastPrice":"1","currency":"KRW"}]})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category_ == ErrorCategory::PROTOCOL);
    REQUIRE(result.error().broker_code_ == "invalid_price_response");
  }
}

TEST_CASE("a trading calendar tells closed apart from not covered") {
  TradingCalendar calendar;
  calendar.days_ = {
    TradingDay{Date{2026, 3, 20}, true},   // Friday
    TradingDay{Date{2026, 3, 21}, false},  // Saturday
    TradingDay{Date{2026, 3, 22}, false},  // Sunday
    TradingDay{Date{2026, 3, 23}, true},   // Monday
  };

  REQUIRE(calendar.is_trading_day(Date{2026, 3, 20}) == true);
  REQUIRE(calendar.is_trading_day(Date{2026, 3, 21}) == false);

  // A date the calendar does not cover is unknown, not closed. Reporting closed
  // would stop a strategy on a day that is in fact open; reporting open would let
  // it place orders into a holiday.
  REQUIRE_FALSE(calendar.is_trading_day(Date{2026, 3, 24}).has_value());
  REQUIRE_FALSE(calendar.is_trading_day(Date{2026, 3, 19}).has_value());

  REQUIRE(calendar.next_trading_day(Date{2026, 3, 20}) == Date{2026, 3, 23});
  REQUIRE(calendar.previous_trading_day(Date{2026, 3, 23}) == Date{2026, 3, 20});
  REQUIRE_FALSE(calendar.next_trading_day(Date{2026, 3, 23}).has_value());
  REQUIRE_FALSE(calendar.previous_trading_day(Date{2026, 3, 20}).has_value());

  REQUIRE(calendar.covered()->first == Date{2026, 3, 20});
  REQUIRE(calendar.covered()->second == Date{2026, 3, 23});
  REQUIRE_FALSE(TradingCalendar{}.covered().has_value());
}

TEST_CASE("both brokers' calendars normalize to the same trading days") {
  SECTION("KIS reports four flags per date and only the market-open one is read") {
    // KIS separately reports a banking business day, a securities business day, a
    // market-open day, and a settlement day. It directs callers placing an order to
    // use opnd_yn, so that is the one normalized; folding the four into one would
    // silently pick a meaning.
    TradingCalendar calendar;
    std::string search;
    std::string next;
    const auto failure = kis::KisClientTestAccess::parse_calendar_page(
      json_response(
        R"({"rt_cd":"0","msg_cd":"MCA00000","ctx_area_fk":"","ctx_area_nk":"","output":[)"
        R"({"bass_dt":"20260320","wday_dvsn_cd":"06","bzdy_yn":"Y","tr_day_yn":"Y",)"
        R"("opnd_yn":"Y","sttl_day_yn":"Y"},)"
        R"({"bass_dt":"20260321","wday_dvsn_cd":"07","bzdy_yn":"N","tr_day_yn":"N",)"
        R"("opnd_yn":"N","sttl_day_yn":"N"},)"
        R"({"bass_dt":"20260323","wday_dvsn_cd":"02","bzdy_yn":"Y","tr_day_yn":"Y",)"
        R"("opnd_yn":"Y","sttl_day_yn":"Y"}]})"
      ),
      calendar,
      search,
      next
    );
    REQUIRE_FALSE(failure);
    REQUIRE(calendar.days_.size() == 3);
    REQUIRE(calendar.is_trading_day(Date{2026, 3, 20}) == true);
    REQUIRE(calendar.is_trading_day(Date{2026, 3, 21}) == false);
    REQUIRE(calendar.is_trading_day(Date{2026, 3, 23}) == true);
  }

  SECTION("a bank holiday that is not a market holiday still closes the market") {
    // bzdy_yn and opnd_yn genuinely disagree on some dates, which is the reason
    // only one of them is the answer.
    TradingCalendar calendar;
    std::string search;
    std::string next;
    kis::KisClientTestAccess::parse_calendar_page(
      json_response(
        R"({"rt_cd":"0","output":[)"
        R"({"bass_dt":"20260501","bzdy_yn":"Y","tr_day_yn":"Y","opnd_yn":"N",)"
        R"("sttl_day_yn":"N"}]})"
      ),
      calendar,
      search,
      next
    );
    REQUIRE(calendar.is_trading_day(Date{2026, 5, 1}) == false);
  }

  SECTION("Toss reports three days and marks the open one by its trading hours") {
    // Toss documents the hours as null when both KRX and NXT are closed, so the
    // presence of the hours is the answer; the hours themselves are Toss's own and
    // are not normalized.
    auto result = tossinvest::TossClientTestAccess::parse_calendar(
      Country::KR,
      json_response(
        R"({"result":{)"
        R"("previousBusinessDay":{"date":"2026-03-20"},)"
        R"("today":{"date":"2026-03-23","integrated":{"open":"09:00","close":"15:30"}},)"
        R"("nextBusinessDay":{"date":"2026-03-24"}}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().country_ == Country::KR);
    REQUIRE(result.value().days_.size() == 3);

    // Ascending, whatever order the object listed them in.
    REQUIRE(result.value().days_.front().date_ == Date{2026, 3, 20});
    REQUIRE(result.value().days_.back().date_ == Date{2026, 3, 24});
    REQUIRE(result.value().is_trading_day(Date{2026, 3, 23}) == true);
  }

  SECTION("Toss reporting no hours for today means the market is closed") {
    auto result = tossinvest::TossClientTestAccess::parse_calendar(
      Country::KR,
      json_response(
        R"({"result":{"previousBusinessDay":{"date":"2026-03-20"},)"
        R"("today":{"date":"2026-03-21","integrated":null},)"
        R"("nextBusinessDay":{"date":"2026-03-23"}}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().is_trading_day(Date{2026, 3, 21}) == false);
    REQUIRE(result.value().is_trading_day(Date{2026, 3, 20}) == true);
  }

  SECTION("a malformed calendar date is a protocol error") {
    auto result = tossinvest::TossClientTestAccess::parse_calendar(
      Country::KR, json_response(R"({"result":{"today":{"date":"2026-13-45"}}})")
    );
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code_ == "invalid_calendar_response");
  }
}

TEST_CASE("instrument reference data normalizes from both brokers") {
  const auto hynix = InstrumentId{Country::KR, Venue::KRX, "000660"};

  SECTION("Toss reports a rich record and the caller's identity survives") {
    auto result = tossinvest::TossClientTestAccess::parse_instrument(
      hynix,
      json_response(
        R"({"result":[{"symbol":"000660","name":"SK하이닉스","englishName":"SKhynix",)"
        R"("isinCode":"KR7000660001","market":"KOSPI","securityType":"STOCK",)"
        R"("isCommonShare":true,"status":"ACTIVE","currency":"KRW",)"
        R"("listDate":"1996-12-26","delistDate":null,"sharesOutstanding":"728002365"}]})"
      )
    );
    REQUIRE(result);
    const auto& instrument = result.value();

    // The venue the caller stated survives; it is not rebuilt from the symbol.
    REQUIRE(instrument.id_.venue_ == Venue::KRX);
    REQUIRE(instrument.name_ == "SK하이닉스");
    REQUIRE(instrument.isin_ == "KR7000660001");
    REQUIRE(instrument.segment_.known_ == MarketSegment::Known::KOSPI);
    REQUIRE(instrument.type_ == SecurityType::STOCK);
    REQUIRE(instrument.status_ == InstrumentStatus::ACTIVE);
    REQUIRE(instrument.common_share_);
    REQUIRE(instrument.listed_on_ == Date{1996, 12, 26});
    REQUIRE_FALSE(instrument.delisted_on_.has_value());
    REQUIRE(*instrument.shares_outstanding_ == Decimal64::parse("728002365"));

    // The grid is the library's own, because no broker publishes tick size, and this
    // is a cash equity so the equity ladder applies.
    REQUIRE(instrument.price_rule_.kind_ == PriceRule::Kind::STATED);
    REQUIRE(instrument.price_rule_.tick_at(Decimal64::parse("70000"))->str() == "100");
  }

  SECTION("an unfamiliar Toss security type stays unknown rather than nearby") {
    auto result = tossinvest::TossClientTestAccess::parse_instrument(
      hynix,
      json_response(
        R"({"result":[{"symbol":"000660","name":"x","englishName":"x","isinCode":"x",)"
        R"("market":"KR_ETC","securityType":"SOMETHING_NEW","status":"ACTIVE",)"
        R"("currency":"KRW"}]})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().type_ == SecurityType::UNKNOWN);

    // And the unfamiliar market segment is preserved rather than discarded.
    REQUIRE(result.value().segment_.other_.view() == "KR_ETC");
  }

  SECTION("KIS reports a padded product number, so the symbol is not parsed from it") {
    // KIS answers with pdno 00000A000660 for 000660. Parsing a symbol back out would
    // be inventing a rule KIS has not stated, so the caller's identity is kept.
    auto result = kis::KisClientTestAccess::parse_instrument(
      hynix,
      json_response(
        R"({"rt_cd":"0","msg_cd":"MCA00000","output":{"pdno":"00000A000660",)"
        R"("prdt_type_cd":"300","mket_id_cd":"STK","scty_grp_id_cd":"ST",)"
        R"("std_pdno":"KR7000660001","lstg_stqt":"728002365",)"
        R"("scts_mket_lstg_dt":"19961226","scts_mket_lstg_abol_dt":"",)"
        R"("kosdaq_mket_lstg_dt":"","prdt_name":"SK하이닉스","prdt_eng_name":"SKhynix"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().id_.symbol_ == "000660");
    REQUIRE(result.value().id_.venue_ == Venue::KRX);
    REQUIRE(result.value().segment_.known_ == MarketSegment::Known::KOSPI);
    REQUIRE(result.value().type_ == SecurityType::STOCK);
    REQUIRE(result.value().isin_ == "KR7000660001");

    // KIS reports a listing date per market and blanks the ones that do not apply.
    REQUIRE(result.value().listed_on_ == Date{1996, 12, 26});
    REQUIRE_FALSE(result.value().delisted_on_.has_value());

    // KIS publishes no listing status, and one is not inferred from a date.
    REQUIRE(result.value().status_ == InstrumentStatus::UNKNOWN);
  }

  SECTION("a KOSDAQ listing takes its date from the KOSDAQ column") {
    auto result = kis::KisClientTestAccess::parse_instrument(
      InstrumentId{Country::KR, Venue::COMBINED, "035720"},
      json_response(
        R"({"rt_cd":"0","output":{"mket_id_cd":"KSQ","scty_grp_id_cd":"ST",)"
        R"("std_pdno":"x","prdt_name":"x","prdt_eng_name":"x",)"
        R"("scts_mket_lstg_dt":"","kosdaq_mket_lstg_dt":"20170101",)"
        R"("kosdaq_mket_lstg_abol_dt":""}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().segment_.known_ == MarketSegment::Known::KOSDAQ);
    REQUIRE(result.value().listed_on_ == Date{2017, 1, 1});
  }

  SECTION("a KONEX listing keeps its own market code rather than a nearby one") {
    auto result = kis::KisClientTestAccess::parse_instrument(
      InstrumentId{Country::KR, Venue::COMBINED, "123456"},
      json_response(
        R"({"rt_cd":"0","output":{"mket_id_cd":"KNX","scty_grp_id_cd":"KN",)"
        R"("std_pdno":"x","prdt_name":"x","prdt_eng_name":"x"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().segment_.known_ == MarketSegment::Known::UNKNOWN);
    REQUIRE(result.value().segment_.other_.view() == "KNX");
    REQUIRE(result.value().type_ == SecurityType::STOCK);
  }

  SECTION("a KIS security group with no counterpart stays unknown") {
    // An ELW is a warrant, but not the 신주인수권 Toss's STOCK_WARRANTS means, so it is
    // not forced into a nearby value.
    auto result = kis::KisClientTestAccess::parse_instrument(
      InstrumentId{Country::KR, Venue::COMBINED, "555555"},
      json_response(
        R"({"rt_cd":"0","output":{"mket_id_cd":"STK","scty_grp_id_cd":"EW",)"
        R"("std_pdno":"x","prdt_name":"x","prdt_eng_name":"x"}})"
      )
    );
    REQUIRE(result);
    REQUIRE(result.value().type_ == SecurityType::UNKNOWN);
  }
}

TEST_CASE("an ETF gets no price grid rather than the equity one", "[broker][reference]") {
  // KRX prices funds on a different ladder from cash equities, and neither broker
  // publishes either. Attaching the equity table would round a price onto a ladder the
  // exchange does not use, and the order would be rejected with the library having
  // confidently produced the price.
  auto result = tossinvest::TossClientTestAccess::parse_instrument(
    InstrumentId{Country::KR, Venue::COMBINED, "069500"},
    json_response(
      R"({"result":[{"symbol":"069500","name":"KODEX 200","englishName":"KODEX200",)"
      R"("isinCode":"KR7069500007","market":"KOSPI","securityType":"ETF",)"
      R"("status":"ACTIVE","currency":"KRW"}]})"
    )
  );
  REQUIRE(result);
  REQUIRE(result.value().type_ == SecurityType::ETF);
  REQUIRE(result.value().price_rule_.kind_ == PriceRule::Kind::UNKNOWN);
  REQUIRE_FALSE(
    result.value().price_rule_.round_to_tick(Decimal64::parse("34567"), OrderSide::BUY)
      .has_value()
  );

  // The same instrument's cash-equity neighbour does get the ladder.
  auto equity = kis::KisClientTestAccess::parse_instrument(
    InstrumentId{Country::KR, Venue::COMBINED, "005930"},
    json_response(
      R"({"rt_cd":"0","output":{"mket_id_cd":"STK","scty_grp_id_cd":"ST","std_pdno":"x",)"
      R"("prdt_name":"x","prdt_eng_name":"x"}})"
    )
  );
  REQUIRE(equity);
  REQUIRE(equity.value().price_rule_.kind_ == PriceRule::Kind::STATED);

  // And a KIS ETN, whose security group is EN, is unknown for the same reason.
  auto note = kis::KisClientTestAccess::parse_instrument(
    InstrumentId{Country::KR, Venue::COMBINED, "550011"},
    json_response(
      R"({"rt_cd":"0","output":{"mket_id_cd":"STK","scty_grp_id_cd":"EN","std_pdno":"x",)"
      R"("prdt_name":"x","prdt_eng_name":"x"}})"
    )
  );
  REQUIRE(note);
  REQUIRE(note.value().type_ == SecurityType::ETN);
  REQUIRE(note.value().price_rule_.kind_ == PriceRule::Kind::UNKNOWN);
}

// ---------------------------------------------------------------------------
// Decimal multiplication, division, and flooring onto a multiple
// ---------------------------------------------------------------------------

TEST_CASE("a product adds the scales so it stays exact") {
  const auto product =
    decimal_multiply(Decimal64::parse("1.25"), Decimal64::parse("4.2"));
  REQUIRE(product);
  // 1.25 * 4.2 is exactly 5.25, and nothing has rounded it.
  REQUIRE(*product == Decimal64::parse("5.25"));

  const auto shares = decimal_multiply(Decimal64::parse("70000"), Decimal64::parse("10"));
  REQUIRE(shares);
  REQUIRE(*shares == Decimal64::parse("700000"));

  const auto signed_product =
    decimal_multiply(Decimal64::parse("-2.5"), Decimal64::parse("4"));
  REQUIRE(signed_product);
  REQUIRE(*signed_product == Decimal64::parse("-10"));
}

TEST_CASE("a product that would overflow is refused rather than wrapped") {
  // A wrapped price is exactly the failure this type exists to prevent, so the check happens
  // before the multiplication: signed overflow is undefined behaviour, not a large number.
  const Decimal64 huge(4000000000LL, 0);
  REQUIRE_FALSE(decimal_multiply(huge, huge));

  // And a scale sum past what the type can hold is refused too.
  const Decimal64 deep(1, 200);
  REQUIRE_FALSE(decimal_multiply(deep, deep));
}

TEST_CASE("a quotient truncates at the scale the caller asked for") {
  // At scale zero this answers "how many whole divisors fit", which is what sizing shares needs.
  const auto whole = decimal_divide(Decimal64::parse("1000000"), Decimal64::parse("70000"), 0);
  REQUIRE(whole);
  REQUIRE(*whole == Decimal64::parse("14"));

  // More places, more of the answer -- and still truncated rather than rounded, because a value
  // this library rounded would be one the caller did not choose.
  const auto precise = decimal_divide(Decimal64::parse("10"), Decimal64::parse("3"), 4);
  REQUIRE(precise);
  REQUIRE(*precise == Decimal64::parse("3.3333"));

  // Truncation toward zero on a negative quotient.
  const auto negative = decimal_divide(Decimal64::parse("-10"), Decimal64::parse("3"), 0);
  REQUIRE(negative);
  REQUIRE(*negative == Decimal64::parse("-3"));
}

TEST_CASE("dividing by zero reports nothing rather than throwing") {
  REQUIRE_FALSE(decimal_divide(Decimal64::parse("1"), Decimal64::parse("0"), 2));
}

TEST_CASE("flooring onto a multiple goes toward negative infinity on both sides of zero") {
  const auto lot = Decimal64::parse("10");
  REQUIRE(*decimal_floor_to_multiple(Decimal64::parse("47"), lot) == Decimal64::parse("40"));
  REQUIRE(*decimal_floor_to_multiple(Decimal64::parse("40"), lot) == Decimal64::parse("40"));
  REQUIRE(*decimal_floor_to_multiple(Decimal64::parse("7"), lot) == Decimal64::parse("0"));
  // Toward negative infinity, not toward zero: the result is always at most the input, which is
  // what makes a rounded quantity a shortfall rather than an excess on either side.
  REQUIRE(*decimal_floor_to_multiple(Decimal64::parse("-47"), lot) == Decimal64::parse("-50"));

  // A fractional step works the same way.
  REQUIRE(
    *decimal_floor_to_multiple(Decimal64::parse("1.07"), Decimal64::parse("0.25")) ==
    Decimal64::parse("1")
  );

  // A step with no multiples to choose between is refused.
  REQUIRE_FALSE(decimal_floor_to_multiple(Decimal64::parse("47"), Decimal64::parse("0")));
  REQUIRE_FALSE(decimal_floor_to_multiple(Decimal64::parse("47"), Decimal64::parse("-10")));
}

TEST_CASE(
  "buying power is a broker's answer and is never confused with cash",
  "[core][capacity]"
) {
  // The type exists to keep two different quantities apart, so the test that matters is that a
  // holder of one cannot read it as the other. `Money` carries the currency, which is what makes a
  // per-currency answer a normal value rather than a special case.
  const BuyingPower power{Money{Decimal64::parse("5000000"), Currency::krw()}, {}};
  REQUIRE(power.cash_.amount_ == Decimal64::parse("5000000"));
  REQUIRE(power.cash_.currency_ == Currency::krw());

  // A fractional foreign answer survives, because Toss reports US buying power with a fraction.
  const BuyingPower foreign{Money{Decimal64::parse("3500.5"), Currency::usd()}, {}};
  REQUIRE(foreign.cash_.amount_ == Decimal64::parse("3500.5"));
  REQUIRE_FALSE(foreign.cash_.currency_ == Currency::krw());
}

TEST_CASE(
  "a commission rate applies only within the window the broker stated",
  "[core][capacity]"
) {
  CommissionRate rate;
  rate.country_ = Country::KR;
  rate.rate_ = Decimal64::parse("0.00015");
  rate.from_ = Date{2026, 1, 1};
  rate.until_ = Date{2026, 12, 31};

  REQUIRE(rate.applies_on(Date{2026, 1, 1}));
  REQUIRE(rate.applies_on(Date{2026, 12, 31}));
  REQUIRE(rate.applies_on(Date{2026, 6, 15}));
  REQUIRE_FALSE(rate.applies_on(Date{2025, 12, 31}));
  REQUIRE_FALSE(rate.applies_on(Date{2027, 1, 1}));

  // An absent bound is open-ended in that direction, which is how a null date arrives.
  CommissionRate open_ended = rate;
  open_ended.until_ = std::nullopt;
  REQUIRE(open_ended.applies_on(Date{2099, 1, 1}));
  REQUIRE_FALSE(open_ended.applies_on(Date{2025, 12, 31}));

  CommissionRate unbounded;
  unbounded.rate_ = Decimal64::parse("0.001");
  REQUIRE(unbounded.applies_on(Date{1999, 1, 1}));
  REQUIRE(unbounded.applies_on(Date{2099, 1, 1}));
}

TEST_CASE(
  "a schedule answers per country and date, and says nothing rather than guessing",
  "[core][capacity]"
) {
  CommissionSchedule schedule;
  schedule.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.00015"), Date{2026, 1, 1}, std::nullopt)
  );
  schedule.rates_.push_back(
    rate_row(Country::US, Decimal64::parse("0.001"), std::nullopt, Date{2026, 6, 30})
  );

  const auto domestic = schedule.rate_for(Country::KR, Date{2026, 9, 23});
  REQUIRE(domestic);
  REQUIRE(domestic->rate_ == Decimal64::parse("0.00015"));

  // The same date answers differently per country, which is the whole reason country is a key.
  const auto foreign = schedule.rate_for(Country::US, Date{2026, 9, 23});
  REQUIRE_FALSE(foreign);
  REQUIRE(schedule.rate_for(Country::US, Date{2026, 6, 30}));

  // A date before every window is unanswered rather than charged at zero, because a rate of zero
  // is a claim the broker did not make.
  REQUIRE_FALSE(schedule.rate_for(Country::KR, Date{2025, 12, 31}));

  // An empty schedule is a real answer from a broker that publishes them, and still answers nothing.
  const CommissionSchedule empty;
  REQUIRE_FALSE(empty.rate_for(Country::KR, Date{2026, 9, 23}));
}

TEST_CASE(
  "Toss buying power normalizes to money carrying the broker's own currency",
  "[core][capacity][toss]"
) {
  // The spec's own KRW example.
  const auto won = tossinvest::TossClientTestAccess::parse_buying_power(
    json_response(R"({"result":{"currency":"KRW","cashBuyingPower":"5000000"}})")
  );
  REQUIRE(won);
  REQUIRE(won.value().cash_.amount_ == Decimal64::parse("5000000"));
  REQUIRE(won.value().cash_.currency_ == Currency::krw());

  // And its USD example, which is fractional -- a figure a whole-won assumption would truncate.
  const auto dollars = tossinvest::TossClientTestAccess::parse_buying_power(
    json_response(R"({"result":{"currency":"USD","cashBuyingPower":"3500.5"}})")
  );
  REQUIRE(dollars);
  REQUIRE(dollars.value().cash_.amount_ == Decimal64::parse("3500.5"));
  REQUIRE(dollars.value().cash_.currency_ == Currency::usd());

  // A broker error is the error, not an empty reading that would read as "no money".
  const auto refused = tossinvest::TossClientTestAccess::parse_buying_power(
    json_response(
      R"({"error":{"code":"invalid-request","message":"지원하지 않는 통화입니다."}})", 400
    )
  );
  REQUIRE_FALSE(refused);

  const auto malformed = tossinvest::TossClientTestAccess::parse_buying_power(
    json_response(R"({"result":{"currency":"KRW"}})")
  );
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().category_ == ErrorCategory::PROTOCOL);
}

TEST_CASE(
  "Toss sellable quantity keeps a fractional share",
  "[core][capacity][toss]"
) {
  const auto domestic = tossinvest::TossClientTestAccess::parse_sellable_quantity(
    json_response(R"({"result":{"sellableQuantity":"100"}})")
  );
  REQUIRE(domestic);
  REQUIRE(domestic.value() == Decimal64::parse("100"));

  // The spec's US example: fractional shares are real, and rounding one would overstate what may
  // be sold.
  const auto foreign = tossinvest::TossClientTestAccess::parse_sellable_quantity(
    json_response(R"({"result":{"sellableQuantity":"5.5"}})")
  );
  REQUIRE(foreign);
  REQUIRE(foreign.value() == Decimal64::parse("5.5"));

  // Nothing sellable is a number, and must survive as one rather than becoming an absence.
  const auto none = tossinvest::TossClientTestAccess::parse_sellable_quantity(
    json_response(R"({"result":{"sellableQuantity":"0"}})")
  );
  REQUIRE(none);
  REQUIRE(none.value() == Decimal64::parse("0"));
}

TEST_CASE(
  "Toss commission rows carry their windows, including open-ended ones",
  "[core][capacity][toss]"
) {
  // The spec's "standard" example: a bounded domestic window and a US row with a null start.
  const auto schedule = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[)"
      R"({"marketCountry":"KR","commissionRate":"0.00015",)"
      R"("startDate":"2026-01-01","endDate":"2026-12-31"},)"
      R"({"marketCountry":"US","commissionRate":"0.001",)"
      R"("startDate":null,"endDate":"2026-06-30"}]})"
    )
  );
  REQUIRE(schedule);
  REQUIRE(schedule.value().rates_.size() == 2);
  REQUIRE(schedule.value().rates_[0].country_ == Country::KR);
  REQUIRE(schedule.value().rates_[0].rate_ == Decimal64::parse("0.00015"));
  REQUIRE(schedule.value().rates_[0].from_ == Date{2026, 1, 1});
  REQUIRE(schedule.value().rates_[0].until_ == Date{2026, 12, 31});
  // A null start is open-ended, not today: the rate applied before this account existed.
  REQUIRE_FALSE(schedule.value().rates_[1].from_);
  REQUIRE(schedule.value().rates_[1].until_ == Date{2026, 6, 30});

  // Answering per country and date is the whole point of keeping the windows.
  REQUIRE(schedule.value().rate_for(Country::KR, Date{2026, 9, 23}));
  REQUIRE_FALSE(schedule.value().rate_for(Country::US, Date{2026, 9, 23}));

  // The spec's "unlimited" example: a null end is a rate with no scheduled change.
  const auto unlimited = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[{"marketCountry":"KR","commissionRate":"0.00015",)"
      R"("startDate":"2026-01-01","endDate":null}]})"
    )
  );
  REQUIRE(unlimited);
  REQUIRE_FALSE(unlimited.value().rates_[0].until_);
  REQUIRE(unlimited.value().rate_for(Country::KR, Date{2099, 1, 1}));

  // A market this library does not normalize is preserved, not dropped: Toss's own specification
  // requires accepting unknown values of this enum, so a schedule that quietly omitted the row
  // would be a successful, incomplete answer with no way for a caller to see what was missing.
  const auto mixed = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[{"marketCountry":"JP","commissionRate":"0.002","startDate":null,"endDate":null},)"
      R"({"marketCountry":"KR","commissionRate":"0.00015","startDate":null,"endDate":null}]})"
    )
  );
  REQUIRE(mixed);
  REQUIRE(mixed.value().rates_.size() == 2);
  REQUIRE_FALSE(mixed.value().rates_[0].normalized());
  REQUIRE(mixed.value().rates_[0].other_.view() == "JP");
  REQUIRE(mixed.value().rates_[0].rate_ == Decimal64::parse("0.002"));
  REQUIRE(mixed.value().rates_[1].normalized());
  REQUIRE(mixed.value().rates_[1].country_ == Country::KR);

  // The preserved row is visible but never answers a question asked in terms of `Country`: an
  // unnormalized market is not a country, and reporting it as one would be worse than dropping it.
  REQUIRE(mixed.value().rate_for(Country::KR, Date{2026, 9, 23}));
  REQUIRE_FALSE(mixed.value().rate_for(Country::US, Date{2026, 9, 23}));

  // A code too long for `Code<16>` cannot be preserved, and that row alone is skipped rather than
  // reported under a market it does not belong to or failing the whole schedule.
  const auto unstorable = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[{"marketCountry":"A_MARKET_NAME_FAR_TOO_LONG_TO_STORE",)"
      R"("commissionRate":"0.002","startDate":null,"endDate":null},)"
      R"({"marketCountry":"KR","commissionRate":"0.00015","startDate":null,"endDate":null}]})"
    )
  );
  REQUIRE(unstorable);
  REQUIRE(unstorable.value().rates_.size() == 1);
  REQUIRE(unstorable.value().rates_[0].country_ == Country::KR);
  // And the loss is visible: a schedule that had quietly shortened itself would read as the whole
  // answer to a caller comparing it against the broker's own statement.
  REQUIRE(unstorable.value().dropped_ == 1);

  // A row naming no market at all is the dangerous one, because an empty code *stores*: an empty
  // `other_` is how a row says it names a normalized market, so this would have become a Korean
  // rate -- winning `rate_for(KR, ...)` against a rate Toss never published.
  const auto nameless = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[{"marketCountry":"","commissionRate":"0.9",)"
      R"("startDate":null,"endDate":null},)"
      R"({"marketCountry":"KR","commissionRate":"0.00015","startDate":null,"endDate":null}]})"
    )
  );
  REQUIRE(nameless);
  REQUIRE(nameless.value().rates_.size() == 1);
  REQUIRE(nameless.value().dropped_ == 1);
  const auto korean = nameless.value().rate_for(Country::KR, Date{2026, 9, 23});
  REQUIRE(korean);
  REQUIRE(korean->rate_ == Decimal64::parse("0.00015"));

  // A complete schedule says so, which is what makes the count worth reading.
  REQUIRE(schedule.value().dropped_ == 0);

  // An account with no stated rates is a real answer, and not the same as having no inquiry.
  const auto empty = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(R"({"result":[]})")
  );
  REQUIRE(empty);
  REQUIRE(empty.value().rates_.empty());
}

TEST_CASE(
  "KIS buying power asks for the amount alone, with the symbol and price blank",
  "[core][capacity][kis]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;
  kis::KisConfig demo;
  demo.is_demo_ = true;

  const auto request = kis::KisClientTestAccess::buying_power_request(live, "token", account);
  REQUIRE(request.method_ == http::verb::get);
  REQUIRE(
    request.target_.rfind("/uapi/domestic-stock/v1/trading/inquire-psbl-order", 0) == 0
  );
  REQUIRE(header_of(request, "tr_id") == "TTTC8908R");

  // The blanks are the whole request: they are how KIS is asked for the account-level amount
  // rather than a per-instrument quantity, and both fields are required so they are sent empty
  // rather than omitted.
  REQUIRE(request.target_.find("PDNO=&") != std::string::npos);
  REQUIRE(request.target_.find("ORD_UNPR=&") != std::string::npos);
  REQUIRE(request.target_.find("CANO=12345678") != std::string::npos);
  REQUIRE(request.target_.find("ACNT_PRDT_CD=01") != std::string::npos);

  // Both inclusions are off deliberately: either would report won an order cannot spend.
  REQUIRE(request.target_.find("CMA_EVLU_AMT_ICLD_YN=N") != std::string::npos);
  REQUIRE(request.target_.find("OVRS_ICLD_YN=N") != std::string::npos);

  // The demo environment has its own transaction id, so a demo key is not silently sent a live one.
  const auto demo_request = kis::KisClientTestAccess::buying_power_request(demo, "token", account);
  REQUIRE(header_of(demo_request, "tr_id") == "VTTC8908R");
}

TEST_CASE(
  "KIS buying power normalizes the credit-free amount, not the maximum",
  "[core][capacity][kis]"
) {
  // The response shape from the published sheet. `max_buy_amt` is deliberately larger here: it
  // includes 미수, which Toss never reports, and normalizing it would make one library type mean
  // cash at one broker and cash-plus-credit at the other -- in the direction that permits more.
  const auto reading = kis::KisClientTestAccess::parse_buying_power(
    json_response(
      R"({"rt_cd":"0","msg_cd":"APBK0013","msg1":"조회되었습니다","output":{)"
      R"("ord_psbl_cash":"741191178","ord_psbl_sbst":"0","ruse_psbl_amt":"0",)"
      R"("fund_rpch_chgs":"0","psbl_qty_calc_unpr":"0",)"
      R"("nrcvb_buy_amt":"741191178","nrcvb_buy_qty":"0",)"
      R"("max_buy_amt":"2223573534","max_buy_qty":"0","cma_evlu_amt":"0",)"
      R"("ovrs_re_use_amt_wcrc":"0","ord_psbl_frcr_amt_wcrc":"0"}})"
    )
  );
  REQUIRE(reading);
  REQUIRE(reading.value().cash_.amount_ == Decimal64::parse("741191178"));
  REQUIRE(reading.value().cash_.currency_ == Currency::krw());

  // A declared failure is an error, never an empty reading that would read as "no money".
  const auto refused = kis::KisClientTestAccess::parse_buying_power(
    json_response(R"({"rt_cd":"1","msg_cd":"APBK0919","msg1":"조회할 수 없습니다"})")
  );
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error().broker_code_ == "APBK0919");

  const auto malformed = kis::KisClientTestAccess::parse_buying_power(
    json_response(R"({"rt_cd":"0","output":{"ord_psbl_cash":"1"}})")
  );
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().category_ == ErrorCategory::PROTOCOL);
}
