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
