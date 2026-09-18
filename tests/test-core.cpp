#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
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
};
} // namespace algotradelib::tossinvest

namespace {

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
      REQUIRE(result.error().category == ErrorCategory::INVALID_ARGUMENT);
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
      REQUIRE(result.error().category == ErrorCategory::INVALID_ARGUMENT);
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
  options.queue_limit = 0;
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", "443", options
  );
  bool completed = false;
  client->async_request(network::HttpRequest{}, [&completed](Result<network::HttpResponse> result) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().broker_code == "queue_full");
    completed = true;
  });
  ioc.run();
  REQUIRE(completed);

  ioc.restart();
  auto future = client->async_request(network::HttpRequest{}, net::use_future);
  ioc.run();
  const auto future_result = future.get();
  REQUIRE_FALSE(future_result);
  REQUIRE(future_result.error().broker_code == "queue_full");
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
    request.target = target;
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
    request.target = target;
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
    request.target = "/drop-get-once";
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
    request.method = http::verb::post;
    request.target = "/drop-post";
    request.body = "order-like-payload";
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
  "Async HTTP enforces the configured response body limit",
  "[http][tls][limits]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  network::HttpClientOptions options;
  options.body_limit = 128;
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  network::HttpRequest request;
  request.target = "/large";
  bool completed = false;
  client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().category == ErrorCategory::TRANSPORT);
    REQUIRE(result.error().broker_code == "read");
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
  REQUIRE(kis_result.value().last_price == Decimal64(72000, 0));
  REQUIRE(kis_result.value().currency == Currency::krw());
  REQUIRE_FALSE(kis_result.value().exchange_time);

  network::HttpResponse toss_response{http::status::ok, 11};
  toss_response.body() =
    R"({"result":[{"symbol":"005930","timestamp":"2026-03-25T09:30:00.123+09:00","lastPrice":"72000","currency":"KRW"}]})";
  auto toss_result = tossinvest::TossClientTestAccess::parse(samsung, std::move(toss_response));
  REQUIRE(toss_result);
  REQUIRE(toss_result.value().last_price == Decimal64(72000, 0));
  REQUIRE(toss_result.value().currency == Currency::krw());
  REQUIRE(toss_result.value().exchange_time);

  network::HttpResponse toss_error{http::status::too_many_requests, 11};
  toss_error.body() =
    R"({"error":{"requestId":"request-1","code":"rate-limit-exceeded","message":"retry later"}})";
  auto error_result = tossinvest::TossClientTestAccess::parse(samsung, std::move(toss_error));
  REQUIRE_FALSE(error_result);
  REQUIRE(error_result.error().category == ErrorCategory::RATE_LIMIT);
  REQUIRE(error_result.error().request_id == "request-1");
}

} // namespace
