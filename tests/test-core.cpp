#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <chrono>
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

  static network::HttpRequest cancel_order_request(
    std::int64_t account_sequence,
    std::string_view order_id
  ) {
    return TossClient::make_cancel_order_request(account_sequence, "access-token", order_id);
  }

  static network::HttpRequest orders_request(const TossOrderQuery& query) {
    return TossClient::make_orders_request("access-token", query);
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
    REQUIRE_FALSE(body.contains("EXCG_ID_DVSN_CD"));
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

  SECTION("an at-the-close condition is rejected rather than downgraded to DAY") {
    auto order = korean_limit_buy();
    order.time_in_force_ = TimeInForce::AT_THE_CLOSE;
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

  SECTION("an unrecognized order division does not fail the listing") {
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
    REQUIRE(open_order.order_.type_ == OrderType::UNKNOWN);
    REQUIRE(open_order.order_.time_in_force_ == TimeInForce::UNKNOWN);
    REQUIRE_FALSE(open_order.order_.price_);
    REQUIRE(open_order.order_.side_ == OrderSide::SELL);
  }

  SECTION("an empty result is an empty page rather than an error") {
    auto result =
      kis::KisClientTestAccess::parse_open_orders(json_response(R"({"rt_cd":"0","output":[]})"));
    REQUIRE(result);
    REQUIRE(result.value().orders_.empty());
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

  SECTION("a US at-the-close limit order uses the documented validity code") {
    auto order = korean_limit_buy();
    order.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
    order.time_in_force_ = TimeInForce::AT_THE_CLOSE;
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

  SECTION("a malformed idempotency key is rejected") {
    auto order = korean_limit_buy();
    order.client_order_id_ = "not valid!";
    expect_rejection(std::move(order), "invalid_client_order_id");
  }

  SECTION("an at-the-close condition on a Korean order is rejected") {
    auto order = korean_limit_buy();
    order.time_in_force_ = TimeInForce::AT_THE_CLOSE;
    expect_rejection(std::move(order), "unsupported_time_in_force");
  }

  SECTION("an at-the-open condition on a US order is rejected") {
    auto order = korean_limit_buy();
    order.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
    order.time_in_force_ = TimeInForce::AT_THE_OPEN;
    expect_rejection(std::move(order), "unsupported_time_in_force");
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
