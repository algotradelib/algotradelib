// Alpaca adapter: paper by default, the calendar's anchor, the refusals before any order request, and the replies.
//
// Bodies are trimmed to the fields the adapter reads, with no account number, account id or key. They come from the
// paper host on 2026-11-20 (labs/atl-thirdparty/tests/alpaca and the reads made for this branch): the asset, the
// latest trade, daily bars, an open order, the v3 clock (`pre`; `core`, `post` and `closed` are the same body with the
// phase changed) and the v3 calendar for 2026-11-01 to 2027-01-15, which `calendar_body` rebuilds row by row (checked
// field for field against the recording). The position rows carry the fields of the row the paper host gave for one
// SPY share on 2026-11-20 at 09:32 New York (labs/atl-thirdparty/README.md, "Market hours"), every number a string.

#include <algotradelib/alpaca>
#include <algotradelib/live>
#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace algotradelib::alpaca {

/** Reaches the client's connections, clock and request builder, which are private by design. */
struct AlpacaClientTestAccess {
  /** Points both connections at one loopback connection, so the requests arrive in order on it, unpaced. */
  static void connect(AlpacaBlockingClient& client, ssl::context& context, const std::string& port) {
    auto http = std::make_shared<network::AsyncHttpClient>(client.ioc_.get_executor(), context, "localhost", port);
    client.trading_ = http;
    client.data_ = http;
  }

  static void set_now(AlpacaBlockingClient& client, std::chrono::system_clock::time_point now) {
    client.clock_ = [now] {
      return now;
    };
  }

  static network::RateLimitBudget trading_budget(const AlpacaBlockingClient& client) {
    return client.trading_->rate_limits()->budget();
  }

  static network::RateLimitBudget data_budget(const AlpacaBlockingClient& client) {
    return client.data_->rate_limits()->budget();
  }

  static network::HttpRequest request(const AlpacaBlockingClient& client) {
    return client.make_request(http::verb::post, "/v2/orders", "{}", network::RateLimitGroup::TRADING);
  }

  static OrderSnapshot order(const std::string& row) {
    return AlpacaBlockingClient::order_of(json::parse(row).as_object());
  }
};

} // namespace algotradelib::alpaca

namespace {

using alpaca::AlpacaBlockingClient;
using alpaca::AlpacaClientTestAccess;
using Server = test::LocalHttpsServer;
using Traits = BrokerTraits<AlpacaBlockingClient>;

class ScopedVariable {
public:
  explicit ScopedVariable(std::string name) : name_(std::move(name)) {
    if (const char* value = std::getenv(name_.c_str())) {
      original_ = value;
    }
  }

  ~ScopedVariable() {
    if (original_) {
      set(*original_);
    } else {
      unset();
    }
  }

  void set(const std::string& value) const {
#ifdef _WIN32
    (void)_putenv_s(name_.c_str(), value.c_str());
#else
    (void)::setenv(name_.c_str(), value.c_str(), 1);
#endif
  }

  void unset() const {
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

std::chrono::system_clock::time_point at(const char* text) {
  return *parse_iso8601_timestamp(text);
}

alpaca::AlpacaConfig test_config() {
  alpaca::AlpacaConfig config;
  config.from_json_text(R"({"key_id":"test-key","secret_key":"test-secret"})", "test");
  return config;
}

// A client on the loopback server, its clock at `now`.
struct Loopback {
  Server server;
  ssl::context context{ssl::context::tls_client};
  std::unique_ptr<AlpacaBlockingClient> client;

  explicit Loopback(const char* now = "2026-11-20T15:00:00Z") {
    test::trustLocalhostCertificate(context);
    client = std::make_unique<AlpacaBlockingClient>(context, test_config());
    AlpacaClientTestAccess::connect(*client, context, server.port());
    AlpacaClientTestAccess::set_now(*client, at(now));
  }
};

constexpr const char* kAssetSpy =
  R"({"class":"us_equity","exchange":"ARCA","symbol":"SPY","name":"State Street SPDR S&P 500 ETF Trust",)"
  R"("status":"active","tradable":true})";

std::string clock_body(const char* phase) {
  return std::string(R"({"clocks":[{"is_market_day":true,"market":{"acronym":"NYSE","mic":"XNYS",)") +
    R"("name":"New York Stock Exchange","timezone":"America/New_York"},"phase":")" + phase +
    R"(","phase_until":"2026-11-20T09:30:00-05:00","timestamp":"2026-11-20T08:14:03.803639012-05:00"}]})";
}

std::string position_row(
  const char* symbol, const char* asset_class, const char* side, const char* qty, const char* available = nullptr
) {
  return std::string(R"({"symbol":")") + symbol + R"(","asset_class":")" + asset_class + R"(","side":")" + side +
    R"(","qty":")" + qty + R"(","qty_available":")" + (available ? available : qty) +
    R"(","avg_entry_price":"700.5","current_price":"714.65","cost_basis":"700.5","market_value":"714.65",)"
    R"("unrealized_pl":"14.15","unrealized_plpc":"0.0202"})";
}

std::string order_row(const char* id, const char* asset_class, const char* status) {
  return std::string(R"({"id":")") + id +
    R"(","client_order_id":"atl-1","submitted_at":"2026-11-20T10:22:20.045347184Z",)" +
    R"("symbol":"SPY","asset_class":")" + asset_class + R"(","qty":"1","filled_qty":"0","filled_avg_price":null,)" +
    R"("type":"limit","side":"buy","time_in_force":"day","limit_price":"500","stop_price":null,"status":")" + status +
    R"(","extended_hours":false})";
}

// The recorded v3 calendar (`timezone=UTC`) for the dates `first` to `last`: winter time throughout, 2026-11-26,
// 2026-12-25 and 2027-01-01 closed, 2026-11-27 and 2026-12-24 closing at 13:00 New York.
std::string calendar_body(const Date& first, const Date& last, const std::string& extra_row = {}) {
  const auto start = alpaca::detail::days_from_civil(first);
  const auto end = alpaca::detail::days_from_civil(last);
  std::string rows;
  for (auto day = start; day <= end; ++day) {
    const auto date = date_from_epoch_days(day);
    const auto text = alpaca::detail::iso_date(date);
    const auto weekday = ((day % 7) + 7 + 4) % 7; // 0 is Sunday
    if (weekday == 0 || weekday == 6 || text == "2026-11-26" || text == "2026-12-25" || text == "2027-01-01") {
      continue;
    }
    const bool early = text == "2026-11-27" || text == "2026-12-24";
    const auto next = alpaca::detail::iso_date(date_from_epoch_days(day + 1));
    rows += std::string(rows.empty() ? "" : ",") + R"({"date":")" + text + R"(","pre_start":")" + text +
      R"(T09:00:00Z","pre_end":")" + text + R"(T14:30:00Z","core_start":")" + text + R"(T14:30:00Z","core_end":")" +
      text + (early ? "T18:00:00Z" : "T21:00:00Z") + R"(","post_start":")" + text +
      (early ? "T18:00:00Z" : "T21:00:00Z") + R"(","post_end":")" +
      (early ? text + "T22:00:00Z" : next + "T01:00:00Z") + R"("})";
  }
  if (!extra_row.empty()) {
    rows += "," + extra_row;
  }
  return R"({"calendar":[)" + rows + R"(],"market":{"acronym":"NYSE","mic":"XNYS"}})";
}

OrderRequest order(OrderSide side, const char* quantity = "1") {
  OrderRequest request;
  request.instrument_ = alpaca::detail::instrument_of("SPY");
  request.side_ = side;
  request.type_ = OrderType::LIMIT;
  request.price_ = Decimal64::parse("700.01");
  request.quantity_ = Decimal64::parse(quantity);
  return request;
}

} // namespace

TEST_CASE("an Alpaca client is paper unless live is stated, and declares what it backs") {
  ScopedVariable key("ALPACA_KEY");
  ScopedVariable secret("ALPACA_SECRET");
  ScopedVariable demo("ALPACA_DEMO");
  key.set("env-key");
  secret.set("env-secret");
  demo.unset();

  alpaca::AlpacaConfig config;
  CHECK(std::string(config.trading_host()) == "paper-api.alpaca.markets");
  config.from_env();
  CHECK(std::string(config.trading_host()) == "paper-api.alpaca.markets");
  CHECK(config.key() == "env-key");
  demo.set("0");
  config.from_env();
  CHECK(std::string(config.trading_host()) == "api.alpaca.markets");
  CHECK(std::string(config.data_host()) == "data.alpaca.markets");
  demo.set("2");
  key.set("other-key");
  CHECK_THROWS_AS(config.from_env(), std::runtime_error);
  CHECK(config.key() == "env-key");
  CHECK_FALSE(config.is_demo_);

  config.from_json_text(R"({"key_id":"k","secret_key":"s"})", "text");
  CHECK(std::string(config.trading_host()) == "paper-api.alpaca.markets");
  config.from_json_text(R"({"key_id":"k","secret_key":"s","demo":false})", "text");
  CHECK(std::string(config.trading_host()) == "api.alpaca.markets");

  ssl::context context(ssl::context::tls_client);
  auto client = network::make_client<AlpacaBlockingClient>(
    context, test_config(), network::HttpClientOptions{}, network::ClientStores{}
  );
  REQUIRE(client);
  CHECK(std::string(client->config().trading_host()) == "paper-api.alpaca.markets");
  for (
    const auto& budget : {AlpacaClientTestAccess::trading_budget(*client), AlpacaClientTestAccess::data_budget(*client)}
  ) {
    CHECK(budget.burst_ == 2);
    CHECK(budget.per_second_ == 1);
    CHECK(budget.trading_reserve_ == 1);
  }
  const auto request = AlpacaClientTestAccess::request(*client);
  int keys = 0;
  for (const auto& [name, value] : request.headers_) {
    keys +=
      (name == "APCA-API-KEY-ID" && value == "test-key") || (name == "APCA-API-SECRET-KEY" && value == "test-secret");
  }
  CHECK(keys == 2);
}

TEST_CASE("an Alpaca calendar anchored by default brackets now on both sides") {
  static_assert(live::SessionSource<BlockingSession<AlpacaBlockingClient>>::states_calendar_sessions);
  struct Instant {
    const char* now_;
    const char* start_;
    const char* end_;
    strategy::MarketPhase phase_;
  };
  // New York is UTC-5 throughout.
  const std::vector<Instant> instants{
    {"2026-11-21T17:00:00Z", "2026-11-11", "2026-12-21", strategy::MarketPhase::CLOSED}, // Saturday noon
    {"2026-11-24T02:00:00Z", "2026-11-14", "2026-12-24", strategy::MarketPhase::POST_CLOSE}, // Monday 21:00
    {"2026-11-24T07:00:00Z", "2026-11-14", "2026-12-24", strategy::MarketPhase::POST_CLOSE}, // Tuesday 02:00
    {"2026-11-26T17:00:00Z", "2026-11-16", "2026-12-26", strategy::MarketPhase::CLOSED}, // Thanksgiving noon
    {"2026-11-27T18:30:00Z", "2026-11-17", "2026-12-27", strategy::MarketPhase::POST_CLOSE}, // after the 13:00 close
    {"2026-11-30T15:00:00Z", "2026-11-20", "2026-12-30", strategy::MarketPhase::OPEN}, // Monday 10:00
  };
  for (const auto& instant : instants) {
    INFO(instant.now_);
    Loopback loop(instant.now_);
    loop.server.script(
      "/v3/calendar/XNYS", 200,
      calendar_body(*alpaca::detail::parse_date(instant.start_), *alpaca::detail::parse_date(instant.end_))
    );
    const auto calendar = loop.client->calendar(Country::US);
    REQUIRE(calendar);
    CHECK(
      loop.server.requestCount(
        std::string("/v3/calendar/XNYS?start=") + instant.start_ + "&end=" + instant.end_ + "&timezone=UTC"
      ) == 1
    );
    CHECK(calendar.value().days_.size() == 41);
    CHECK(strategy::stated_session_phase(calendar.value(), at(instant.now_)) == instant.phase_);
  }

  Loopback loop("2026-11-21T17:00:00Z");
  const auto first = *alpaca::detail::parse_date("2026-11-11");
  const auto last = *alpaca::detail::parse_date("2026-12-21");
  loop.server.script("/v3/calendar/XNYS", 200, calendar_body(first, last, R"({"date":"2026-11-20"})"));
  const auto twice = loop.client->calendar(Country::US);
  REQUIRE_FALSE(twice);
  CHECK(twice.error().broker_code_ == "calendar_duplicate_date");
  loop.server.script("/v3/calendar/XNYS", 200, calendar_body(first, last, R"({"date":"2026-12-22"})"));
  const auto outside = loop.client->calendar(Country::US);
  REQUIRE_FALSE(outside);
  CHECK(outside.error().broker_code_ == "calendar_out_of_range");
}

TEST_CASE(
  "an Alpaca order is refused before any I/O unless it is a whole-share day limit or market order on the US grid"
) {
  Loopback loop;
  struct Refused {
    const char* code_;
    OrderRequest order_;
  };
  std::vector<Refused> cases;
  const auto with = [&cases](const char* code, auto change) {
    auto request = order(OrderSide::BUY);
    change(request);
    cases.push_back({code, std::move(request)});
  };
  with("instrument_unsupported", [](OrderRequest& o) {
    o.instrument_.country_ = Country::KR;
  });
  with("instrument_unsupported", [](OrderRequest& o) {
    o.instrument_.symbol_ = "spy";
  });
  with("venue_unsupported", [](OrderRequest& o) {
    o.venue_ = Venue::KRX;
  });
  with("invalid_client_order_id", [](OrderRequest& o) {
    o.client_order_id_ = std::string();
  });
  with("invalid_client_order_id", [](OrderRequest& o) {
    o.client_order_id_ = std::string(129, 'k');
  });
  with("time_in_force_unsupported", [](OrderRequest& o) {
    o.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  });
  with("session_unsupported", [](OrderRequest& o) {
    o.session_ = OrderSession::AFTER_HOURS;
  });
  with("order_type_unsupported", [](OrderRequest& o) {
    o.type_ = OrderType::STOP_LIMIT;
  });
  with("order_type_unsupported", [](OrderRequest& o) {
    o.trigger_price_ = Decimal64::parse("690");
  });
  with("invalid_price", [](OrderRequest& o) {
    o.price_.reset();
  });
  with("off_tick", [](OrderRequest& o) {
    o.price_ = Decimal64::parse("1.005");
  });
  with("invalid_price", [](OrderRequest& o) {
    o.type_ = OrderType::MARKET;
  });
  with("invalid_quantity", [](OrderRequest& o) {
    o.quantity_ = Decimal64::parse("0.5");
  });
  with("invalid_quantity", [](OrderRequest& o) {
    o.quantity_ = Decimal64::parse("0");
  });
  for (const auto& refused : cases) {
    INFO(refused.code_);
    const auto checked = Traits::validate_place(refused.order_);
    REQUIRE(checked);
    CHECK(checked->broker_code_ == refused.code_);
    const auto placed = loop.client->place_order({}, refused.order_);
    REQUIRE_FALSE(placed);
    CHECK(placed.error().broker_code_ == refused.code_);
    CHECK(placed.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK_FALSE(placed.error().reached_broker_);
  }
  CHECK(loop.server.connectionCount() == 0);

  // The grid's two bands at their boundary.
  for (
    const auto& [price, accepted] :
    std::vector<std::pair<const char*, bool>>{{"0.9999", true}, {"1.005", false}, {"1.01", true}}
  ) {
    auto request = order(OrderSide::BUY);
    request.price_ = Decimal64::parse(price);
    CHECK(Traits::validate_place(request).has_value() != accepted);
  }
  auto market = order(OrderSide::SELL, "3");
  market.type_ = OrderType::MARKET;
  market.price_.reset();
  CHECK_FALSE(Traits::validate_place(market));
  const auto replaced = Traits::to_replacement(OrderRef<alpaca::Broker>{}, AmendInstruction{});
  REQUIRE_FALSE(replaced);
  CHECK(replaced.error().broker_code_ == "replace_unsupported");

  // An order id that is not one never reaches a path: "." would make the cancellation DELETE /v2/orders (all orders).
  for (const char* id : {"", ".", "..", "a/b", "b0b0?x=1"}) {
    INFO(id);
    const auto cancel = loop.client->cancel_order({}, {id});
    REQUIRE_FALSE(cancel);
    CHECK(cancel.error().broker_code_ == "invalid_reference");
    CHECK_FALSE(loop.client->order_by_id({}, id));
    OrderRef<alpaca::Broker> ref;
    ref.primary_ = id;
    CHECK_FALSE(Traits::to_cancellation(ref, AmendInstruction{}));
  }
  CHECK(loop.server.connectionCount() == 0);
}

TEST_CASE(
  "an Alpaca order is not sent for an unlisted asset, while the regular session is off, or above the position"
) {
  Loopback loop;
  auto& server = loop.server;
  const auto refused = [&](const OrderRequest& request) {
    const auto placed = loop.client->place_order({}, request);
    REQUIRE_FALSE(placed);
    CHECK_FALSE(placed.error().reached_broker_);
    CHECK(server.requestCount("/v2/orders") == 0);
    return placed.error();
  };
  server.script("/v2/assets/SPY", 200, kAssetSpy);
  // Two of the three shares available: one sell of one share rests.
  server.script("/v2/positions/SPY", 200, position_row("SPY", "us_equity", "long", "3", "2"));
  server.script("/v3/clock", 200, clock_body("core"));

  SECTION("an asset not listed on a national exchange, not tradable or unknown is refused before the clock") {
    std::string otc = kAssetSpy;
    otc.replace(otc.find("ARCA"), 4, "OTC");
    for (
      const auto& [body, status, code] : std::vector<std::tuple<std::string, unsigned, const char*>>{
        {otc, 200, "instrument_not_listed"},
        {std::string(kAssetSpy).replace(std::string(kAssetSpy).find("ARCA"), 4, "MOON"), 200, "instrument_not_listed"},
        {std::string(kAssetSpy).replace(std::string(kAssetSpy).find("true"), 4, "false"), 200,
          "instrument_not_tradable"},
        {R"({"code":40410000,"message":"asset not found for SPY"})", 404, "instrument_not_listed"},
        {R"({"class":"us_equity","exchange":"ARCA","symbol":"SPY","status":"active"})", 200, "instrument_unreadable"},
        {std::string(kAssetSpy).replace(std::string(kAssetSpy).find("\"SPY\""), 5, "\"QQQ\""), 200,
          "instrument_mismatch"},
      }
    ) {
      server.script("/v2/assets/SPY", status, body);
      CHECK(refused(order(OrderSide::SELL)).broker_code_ == code);
    }
    CHECK(server.scriptedCount("/v2/positions/SPY") == 0);
    CHECK(server.scriptedCount("/v3/clock") == 0);
  }

  SECTION("a sell reads the asset, the position, then the clock, and is refused above the position") {
    server.script("/v3/clock", 200, clock_body("post"));
    CHECK(refused(order(OrderSide::SELL)).broker_code_ == "market_closed");
    CHECK(server.positionsOnConnection("/v2/assets/SPY") == std::vector<std::size_t>{1});
    CHECK(server.positionsOnConnection("/v2/positions/SPY") == std::vector<std::size_t>{2});
    CHECK(server.positionsOnConnection("/v3/clock?markets=XNYS") == std::vector<std::size_t>{3});
    CHECK(refused(order(OrderSide::SELL, "3")).broker_code_ == "sell_exceeds_position");
    server.script("/v2/positions/SPY", 200, position_row("QQQ", "us_equity", "long", "3"));
    CHECK(refused(order(OrderSide::SELL)).broker_code_ == "position_mismatch");
    server.script("/v2/positions/SPY", 404, R"({"code":40410000,"message":"position does not exist"})");
    CHECK(refused(order(OrderSide::SELL)).broker_code_ == "no_position");
    server.script("/v2/positions/SPY", 500, "{}");
    CHECK(refused(order(OrderSide::SELL)).category_ == ErrorCategory::HTTP);
  }

  SECTION("a placement is refused unless Alpaca's one XNYS clock says core, and a refused close is kept") {
    for (const char* phase : {"pre", "post", "closed"}) {
      server.script("/v3/clock", 200, clock_body(phase));
      const auto error = refused(order(OrderSide::BUY));
      CHECK(error.broker_code_ == "market_closed");
      CHECK(error.category_ == ErrorCategory::CANCELLED);
      CHECK(runner::outcome_of(error) == runner::SendOutcome::REFUSED_LOCALLY);
      // A closing sell released into a `pre` clock is kept for a later pass, never dropped.
      CHECK(runner::broker_unreachable(error));
    }
    server.script("/v3/clock", 200, R"({"clocks":[]})");
    CHECK(refused(order(OrderSide::BUY)).broker_code_ == "clock_unreadable");
    const auto one = clock_body("core");
    const auto entry = one.substr(11, one.size() - 13);
    server.script("/v3/clock", 200, R"({"clocks":[)" + entry + "," + entry + "]}");
    CHECK(refused(order(OrderSide::BUY)).broker_code_ == "clock_unreadable");
    server.script("/v3/clock", 401, "<html>401 Authorization Required</html>");
    const auto unread = refused(order(OrderSide::BUY));
    CHECK(unread.category_ == ErrorCategory::AUTHENTICATION);
    CHECK(runner::broker_unreachable(unread));
  }
}

TEST_CASE("Alpaca's seen refusals map to rejected and everything else on an order call to an unknown outcome") {
  Loopback loop;
  auto& server = loop.server;
  server.script("/v2/assets/SPY", 200, kAssetSpy);
  server.script("/v3/clock", 200, clock_body("core"));
  const auto placed = [&](unsigned status, const std::string& body) {
    server.script("/v2/orders", status, body);
    const auto result = loop.client->place_order({}, order(OrderSide::BUY));
    REQUIRE_FALSE(result);
    CHECK(result.error().reached_broker_);
    return runner::outcome_of(result.error());
  };
  CHECK(placed(422, R"({"code":42210000,"message":"sub-penny increment"})") == runner::SendOutcome::REJECTED);
  CHECK(placed(403, R"({"code":40310000,"message":"insufficient buying power"})") == runner::SendOutcome::REJECTED);
  CHECK(placed(401, "<html>401</html>") == runner::SendOutcome::REJECTED);
  CHECK(
    placed(422, R"({"code":40010001,"message":"client_order_id must be unique"})") == runner::SendOutcome::INDETERMINATE
  );
  CHECK(placed(403, R"({"code":40310001,"message":"wash trade"})") == runner::SendOutcome::INDETERMINATE);
  CHECK(placed(429, R"({"message":"too many requests"})") == runner::SendOutcome::INDETERMINATE);
  CHECK(placed(404, R"({"code":40410000,"message":"not found"})") == runner::SendOutcome::INDETERMINATE);
  CHECK(placed(503, "unavailable") == runner::SendOutcome::INDETERMINATE);
  CHECK(placed(200, R"({"status":"new"})") == runner::SendOutcome::INDETERMINATE);
  CHECK(
    placed(403, R"({"code":40310000,"message":"potential wash trade detected. use complex orders"})") ==
    runner::SendOutcome::REJECTED
  );

  // A placement Alpaca accepts carries only the fields that passed the checks.
  server.script("/v2/orders", 200, R"({"id":"b0b0b0b0-0000-4000-8000-000000000002","client_order_id":"atl-7"})");
  auto limit = order(OrderSide::BUY, "2");
  limit.client_order_id_ = std::string("atl-7");
  const auto accepted = loop.client->place_order({}, limit);
  REQUIRE(accepted);
  CHECK(accepted.value().order_id_ == "b0b0b0b0-0000-4000-8000-000000000002");
  CHECK(
    json::parse(server.lastBody("/v2/orders")) ==
    json::parse(
      R"({"symbol":"SPY","qty":"2","side":"buy","type":"limit","time_in_force":"day","limit_price":"700.01",)"
      R"("client_order_id":"atl-7"})"
    )
  );

  const auto cancelled = [&](unsigned status, const std::string& body) {
    server.script("/v2/orders/b0b0b0b0-0000-4000-8000-000000000001", status, body);
    return loop.client->cancel_order({}, {"b0b0b0b0-0000-4000-8000-000000000001"});
  };
  const auto done = cancelled(204, "");
  REQUIRE(done);
  CHECK(done.value().order_id_ == "b0b0b0b0-0000-4000-8000-000000000001");
  REQUIRE_FALSE(cancelled(422, R"({"code":42210000,"message":"order already replaced"})"));
  CHECK(
    runner::outcome_of(cancelled(422, R"({"code":42210000,"message":"x"})").error()) == runner::SendOutcome::REJECTED
  );
  CHECK(
    runner::outcome_of(cancelled(422, R"({"code":40010001,"message":"x"})").error()) ==
    runner::SendOutcome::INDETERMINATE
  );

  server.scriptSequence(
    "/v2/account", {{429, R"({"message":"too many requests"})", {{"X-RateLimit-Reset", "1795186805"}}}}
    // 2026-11-20T15:00:05Z
  );
  const auto limited = loop.client->accounts();
  REQUIRE_FALSE(limited);
  CHECK(limited.error().category_ == ErrorCategory::RATE_LIMIT);
  CHECK(limited.error().retry_after_ == std::chrono::seconds(5));
}

TEST_CASE("Alpaca's replies normalize in dollars and its holdings state their coverage") {
  Loopback loop("2026-11-20T15:00:00Z");
  auto& server = loop.server;

  CHECK(AlpacaClientTestAccess::order(order_row("a1", "us_equity", "expired")).status_ == OrderStatus::CANCELED);
  CHECK(AlpacaClientTestAccess::order(order_row("a1", "us_equity", "held")).status_ == OrderStatus::UNKNOWN);
  const auto open = AlpacaClientTestAccess::order(order_row("a1", "us_equity", "new"));
  CHECK(open.status_ == OrderStatus::PENDING);
  CHECK(open.currency_ == Currency::usd());

  server.script(
    "/v2/positions", 200,
    "[" + position_row("SPY", "us_equity", "long", "2") + "," + position_row("BTCUSD", "crypto", "long", "0.5") + "," +
      position_row("SPY261218C00700000", "us_option", "long", "1") + "]"
  );
  const auto holdings = loop.client->holdings();
  REQUIRE(holdings);
  REQUIRE(holdings.value().positions_.size() == 1);
  const auto& held = holdings.value().positions_.front();
  CHECK(held.instrument_ == alpaca::detail::instrument_of("SPY"));
  CHECK(held.market_value_.currency_ == Currency::usd());
  CHECK(held.sellable_quantity_ == Decimal64::parse("2"));
  const auto basis = holdings.value().sellable_basis_;
  CHECK(basis.coverage_ == SellableBasis::Coverage::WHOLE_HOLDING);
  CHECK(basis.resting_ == SellableBasis::RestingTreatment::DEDUCT);
  CHECK_FALSE(basis.verified_);
  for (
    const auto& rows : {
      position_row("SPY", "us_equity", "long", "2") + "," + position_row("SPY", "crypto", "long", "1"),
      position_row("SPY", "us_equity", "short", "2"),
      std::string(R"({"symbol":"SPY","asset_class":"us_equity","side":"long"})"),
    }
  ) {
    server.script("/v2/positions", 200, "[" + rows + "]");
    const auto failed = loop.client->holdings();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().broker_code_ == "invalid_holdings_response");
  }

  server.script(
    "/v2/orders", 200,
    "[" + order_row("a1", "us_equity", "new") + "," + order_row("a2", "crypto", "new") + "," +
      R"({"id":"a9","symbol":"","asset_class":"","order_class":"mleg","qty":"1","filled_qty":"0","side":"buy",)"
      R"("type":"limit","time_in_force":"day","status":"new"})" +
      "]"
  );
  const auto working = loop.client->working_orders();
  REQUIRE(working);
  CHECK(working.value().orders_.size() == 1);
  auto no_quantity = order_row("a3", "us_equity", "new");
  no_quantity.replace(no_quantity.find(R"("qty":"1",)"), 10, "");
  server.script("/v2/orders", 200, "[" + no_quantity + "]");
  CHECK_FALSE(loop.client->working_orders());
  server.script(
    "/v2/orders", 200, "[" + order_row("a1", "us_equity", "new") + "," + order_row("a1", "crypto", "new") + "]"
  );
  CHECK_FALSE(loop.client->working_orders());

  server.script(
    "/v2/orders", 200,
    "[" + order_row("a1", "us_equity", "filled") + "," + order_row("a2", "us_equity", "canceled") + "]"
  );
  OrderHistoryQuery history;
  history.limit_ = 2;
  const auto page = loop.client->order_history({}, history);
  REQUIRE(page);
  CHECK(page.value().next_cursor_ == std::string("2026-11-20T10:22:20.045347184Z"));

  // The 2026-11-20 bar is still forming at 10:00 New York, so the series ends on 11-19.
  server.script(
    "/v2/stocks/SPY/bars", 200,
    R"({"bars":[{"c":716.1,"h":720,"l":712,"o":715,"t":"2026-11-20T05:00:00Z","v":100},)"
    R"({"c":715.84,"h":739.77,"l":715.71,"o":737.54,"t":"2026-11-19T05:00:00Z","v":2674961},)"
    R"({"c":730.33,"h":735.01,"l":726.9,"o":733.415,"t":"2026-11-18T05:00:00Z","v":1350941}],"symbol":"SPY"})"
  );
  BarQuery query;
  query.instrument_ = alpaca::detail::instrument_of("SPY");
  query.count_ = 2;
  const auto bars = loop.client->bars(query);
  REQUIRE(bars);
  REQUIRE(bars.value().bars_.size() == 2);
  CHECK(bars.value().bars_.back().timestamp_ == at("2026-11-19T05:00:00Z"));
  CHECK(bars.value().bars_.back().close_ == Decimal64::parse("715.84"));
  CHECK(bars.value().currency_ == Currency::usd());

  server.script(
    "/v2/stocks/SPY/trades/latest", 200, R"({"symbol":"SPY","trade":{"p":714.65,"t":"2026-11-19T21:59:55.696315496Z"}})"
  );
  const auto quote = loop.client->quote(alpaca::detail::instrument_of("SPY"));
  REQUIRE(quote);
  CHECK(quote.value().last_price_ == Decimal64::parse("714.65"));
  CHECK(quote.value().currency_ == Currency::usd());
  server.script("/v2/assets/SPY", 200, kAssetSpy);
  const auto instrument = loop.client->instrument(alpaca::detail::instrument_of("SPY"));
  REQUIRE(instrument);
  CHECK(instrument.value().currency_ == Currency::usd());
  CHECK(instrument.value().price_rule_.on_tick(Decimal64::parse("700.01")) == std::optional<bool>(true));
}
