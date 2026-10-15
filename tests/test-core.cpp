#include <algotradelib/kis>
#include <algotradelib/src/brokers/kiwoom/config.hpp>
#include <algotradelib/runner>
#include <algotradelib/strategy>
#include <algotradelib/tossinvest>

#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include "local_https_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <vector>

using namespace algotradelib;

namespace algotradelib::kis {
/** A live config: the default is demo, and these request-shape tests describe the live account. */
inline KisConfig live_config() {
  KisConfig config;
  config.is_demo_ = false;
  return config;
}

struct KisClientTestAccess {
  /** Points `client` at a loopback server in place of KIS's host; its configuration is unchanged. */
  static void connect(KisClient& client, ssl::context& context, const std::string& port) {
    client.http_ = std::make_shared<network::AsyncHttpClient>(client.strand_, context, "localhost", port);
  }

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

  static std::string market_query(const kis::KisConfig& config, const std::string& symbol) {
    return KisClient::market_query(config, symbol);
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
    std::string& division,
    const KisConfig& config = live_config()
  ) {
    return KisClient::validate_order(config, order, division);
  }

  static void apply_order_shape(std::string_view division, OrderSnapshot& order) {
    KisClient::apply_order_shape(division, order);
  }

  static std::optional<BrokerError> amendment_division(
    const KisAmendment& amendment,
    std::string& division,
    const KisConfig& config = live_config()
  ) {
    // `validate_amendment`, not `order_division`: the submission path's own
    // decision. Calling `order_division` here re-derived the answer from the
    // same inputs, so the test agreed with a submission path that had stopped
    // passing the amendment's condition and session -- verified by mutation,
    // which this shim survived.
    return KisClient::validate_amendment(config, amendment, division);
  }

  static std::optional<BrokerError> cancel_division(
    const KisAmendment& amendment,
    std::string& division,
    const KisConfig& config = live_config()
  ) {
    return KisClient::validate_amendment(config, amendment, division, true);
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

  static bool calendar_covers(const TradingCalendar& calendar, const Date& from) {
    return KisClient::calendar_covers(calendar, from);
  }

  static std::size_t calendar_rows_consumed(const TradingCalendar& calendar) {
    return KisClient::calendar_rows_consumed(calendar);
  }

  using Operation = KisClient::OrderOperation;

  static Result<KisOrderAck> parse_order_ack(Result<network::HttpResponse> response) {
    return KisClient::parse_order_ack(std::move(response));
  }

  static Result<KisOrderAck> parse_order_ack(
    Result<network::HttpResponse> response,
    Operation operation,
    bool demo
  ) {
    return KisClient::parse_order_ack(std::move(response), operation, demo);
  }

  static Result<KisOpenOrderPage> parse_open_orders(
    Result<network::HttpResponse> response,
    bool demo = false
  ) {
    std::string foreign_cursor;
    std::string domestic_cursor;
    return KisClient::parse_open_orders_page(
      std::move(response), foreign_cursor, domestic_cursor, demo
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

  static std::optional<BrokerError> validate_order(const OrderRequest& order) {
    return TossClient::validate_order(order);
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

/**
 * The venue an `InstrumentId` carries for a Korean instrument, at both brokers.
 *
 * Named once and asserted from four places -- each broker's holdings parse and
 * each broker's order parse -- because those four must agree and two of them
 * did not. `InstrumentId` compares its venue, and
 * `Portfolio::working_quantity` (`strategy/portfolio.hpp:136`) matches a
 * working order's `instrument_` against the instrument a caller names. Every
 * consumer of that answer is a limit: the sellable deduction, the
 * working-order component of the position ceiling, `closes_toward_flat`, and
 * the reconciliation that decides whether to amend the order already out or
 * send a second one beside it.
 *
 * A disagreement makes all of them read zero. Nothing errors and no test fails
 * on its own; a number is simply smaller than it should be, and the last
 * consumer in that list sends an order it should not have sent. So the
 * convention is a constant rather than a repeated literal.
 */
constexpr Venue kKoreanInstrumentVenue = Venue::COMBINED;

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

  // More than three fractional digits are truncated, and a negative offset is subtracted.
  REQUIRE(parse_iso8601_timestamp("2026-03-25T00:30:00.123456Z") == utc);
  REQUIRE(parse_iso8601_timestamp("2026-03-24T15:30:00.123-09:00") == utc);

  // Anything but the exact layout is refused rather than read as some other instant.
  for (const char* malformed : {
         "2026-03-25T09:30:00+99:99", "2026-03-25T09:30:00+24:00", "2026-03-25T09:30:00+09:60",
         "2026-03-25T09:30:00Zgarbage", "2026-03-25T09:30:00.123oopsZ", "2026-03-25T09:30:00.Z",
         "2026-03-25T09:30:00", "2026-03-25T09:30:00+0900", "2026-03-25T09:30:00+09:00x",
         "2026-3-25T09:30:00Z", " 2026-03-25T09:30:00Z", "+2026-03-25T09:30:00Z",
       }) {
    INFO(malformed);
    REQUIRE_FALSE(parse_iso8601_timestamp(malformed));
  }
}

TEST_CASE("decimal arithmetic at the edges of the coefficient", "[core][decimal]") {
  // Two negatives whose common scale overflows: the smaller scale holds the larger magnitude.
  const Decimal64 tiny(-1, 2);
  const Decimal64 huge(-9000000000000000000LL, 0);
  REQUIRE_FALSE(decimal_less(tiny, huge));
  REQUIRE(decimal_less(huge, tiny));
  REQUIRE(decimal_less(Decimal64(1, 2), Decimal64(9000000000000000000LL, 0)));
  REQUIRE(decimal_less(huge, Decimal64(1, 2)));

  // Flooring the most negative coefficient would go below it: refused rather than overflowing.
  const Decimal64 lowest(std::numeric_limits<std::int64_t>::min(), 0);
  REQUIRE_FALSE(decimal_floor_to_multiple(lowest, Decimal64(3, 0)));
  REQUIRE(decimal_floor_to_multiple(lowest, Decimal64(2, 0)) == lowest);

  // A remainder that does not fit says "cannot tell", as `round_to_tick` does, not "off the grid".
  const Decimal64 unrepresentable(std::numeric_limits<std::int64_t>::max(), 19);
  REQUIRE_FALSE(PriceRule::krx_equity().on_tick(unrepresentable).has_value());
  REQUIRE_FALSE(PriceRule::krx_equity().round_to_tick(unrepresentable, OrderSide::BUY).has_value());
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
  "The version macros agree with the version CMake read out of the header",
  "[core][version]"
) {
  // CMake parses version.hpp before project(); this checks the parse against the preprocessor's view
  // of the same file, and that the string is built from the parts rather than typed twice.
  REQUIRE(std::string_view(ATL_VERSION_STRING) == ATL_TEST_PROJECT_VERSION);
  const std::string parts = std::to_string(ATL_VERSION_MAJOR) + "." + std::to_string(ATL_VERSION_MINOR) +
    "." + std::to_string(ATL_VERSION_PATCH) +
    (ATL_VERSION_DEV != 0 ? "." + std::to_string(ATL_VERSION_DEV) : std::string());
  REQUIRE(parts == ATL_VERSION_STRING);

  // The encoded release carries no development part, as BOOST_VERSION does.
  STATIC_REQUIRE(ATL_VERSION == ATL_VERSION_ENCODE(ATL_VERSION_MAJOR, ATL_VERSION_MINOR, ATL_VERSION_PATCH));
  STATIC_REQUIRE(ATL_VERSION_ENCODE(1, 2, 3) == 1002003);
  STATIC_REQUIRE(ATL_VERSION_ENCODE(0, 999, 999) < ATL_VERSION_ENCODE(1, 0, 0));
  STATIC_REQUIRE(ATL_VERSION_ENCODE(2, 0, 0) > ATL_VERSION_ENCODE(1, 999, 999));

  // Still the development version before any release.
  STATIC_REQUIRE(ATL_VERSION == ATL_VERSION_ENCODE(0, 0, 0));
  STATIC_REQUIRE(ATL_VERSION_DEV == 9000);
}

/**
 * A credential file that exists only for the duration of one test.
 *
 * Written relative to the working directory, as the journal tests do, and removed on the way out so a
 * run from the worktree root leaves nothing for `git add` to offer.
 */
class TempCredentialFile {
public:
  TempCredentialFile(const char* name, const std::string& text)
  : path_(std::string("atl-test-credentials-") + name + ".json") {
    std::FILE* handle = std::fopen(path_.c_str(), "wb");
    if (!handle) {
      throw std::runtime_error("failed to create test credential file");
    }
    std::fwrite(text.data(), 1, text.size(), handle);
    std::fclose(handle);
  }

  ~TempCredentialFile() { std::remove(path_.c_str()); }

  TempCredentialFile(const TempCredentialFile&) = delete;
  TempCredentialFile& operator=(const TempCredentialFile&) = delete;

  const std::string& str() const noexcept { return path_; }

private:
  std::string path_;
};

/** Returns the message `load` throws, or an empty string when it does not throw. */
template <typename Load>
std::string refusal_of(Load load) {
  try {
    load();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

TEST_CASE(
  "A KIS config is demo unless KIS_DEMO says 0, and refuses any other value",
  "[broker][kis][config][environment]"
) {
  ScopedEnvironmentVariable key("KIS_APP_KEY");
  ScopedEnvironmentVariable secret("KIS_APP_SECRET");
  ScopedEnvironmentVariable demo("KIS_DEMO");
  key.set("app-key");
  secret.set("app-secret");

  // Built in code, nothing stated: demo.
  REQUIRE(kis::KisConfig{}.is_demo_);

  demo.unset();
  kis::KisConfig unset;
  unset.is_demo_ = false;
  unset.from_env();
  REQUIRE(unset.is_demo_);
  REQUIRE(std::string_view(unset.rest_host()) == "openapivts.koreainvestment.com");

  demo.set("1");
  kis::KisConfig paper;
  paper.from_env();
  REQUIRE(paper.is_demo_);
  REQUIRE(paper.key() == "app-key");

  // Live is the one value that has to be written down.
  demo.set("0");
  kis::KisConfig live;
  live.from_env();
  REQUIRE_FALSE(live.is_demo_);
  REQUIRE(std::string_view(live.rest_host()) == "openapi.koreainvestment.com");

  for (const char* junk : {"true", "false", "yes", "", " 1", "1 ", "01", "2"}) {
#ifdef _WIN32
    // Windows has no empty variable: setting "" removes it, which is the unset case above.
    if (*junk == '\0') {
      continue;
    }
#endif
    demo.set(junk);
    kis::KisConfig refused;
    const auto message = refusal_of([&] { refused.from_env(); });
    INFO("KIS_DEMO=\"" << junk << "\"");
    REQUIRE(message.find(std::string("KIS_DEMO is \"") + junk + "\"") != std::string::npos);
    // Refused before anything was assigned: no half-loaded config is left to build a client from.
    REQUIRE(refused.key().empty());
  }

  // A refused load leaves an already loaded config on the endpoint it selected.
  demo.set("junk");
  key.set("other-key");
  CHECK_FALSE(refusal_of([&] { live.from_env(); }).empty());
  CHECK_FALSE(live.is_demo_);
  CHECK(live.key() == "app-key");
}

TEST_CASE(
  "A Kiwoom config is demo unless KIWOOM_DEMO says 0, and refuses any other value",
  "[broker][kiwoom][config][environment]"
) {
  ScopedEnvironmentVariable key("KIWOOM_APP_KEY");
  ScopedEnvironmentVariable secret("KIWOOM_SECRET_KEY");
  ScopedEnvironmentVariable demo("KIWOOM_DEMO");
  key.set("app-key");
  secret.set("secret-key");

  REQUIRE(kiwoom::KiwoomConfig{}.is_demo_);

  demo.unset();
  kiwoom::KiwoomConfig unset;
  unset.is_demo_ = false;
  unset.from_env();
  REQUIRE(unset.is_demo_);
  REQUIRE(std::string_view(unset.rest_host()) == "mockapi.kiwoom.com");

  demo.set("1");
  kiwoom::KiwoomConfig paper;
  paper.from_env();
  REQUIRE(paper.is_demo_);

  demo.set("0");
  kiwoom::KiwoomConfig live;
  live.from_env();
  REQUIRE_FALSE(live.is_demo_);
  REQUIRE(std::string_view(live.rest_host()) == "api.kiwoom.com");

  for (const char* junk : {"true", "no", "", "0 ", "1.0"}) {
#ifdef _WIN32
    // Windows has no empty variable: setting "" removes it, which is the unset case above.
    if (*junk == '\0') {
      continue;
    }
#endif
    demo.set(junk);
    kiwoom::KiwoomConfig refused;
    const auto message = refusal_of([&] { refused.from_env(); });
    INFO("KIWOOM_DEMO=\"" << junk << "\"");
    REQUIRE(message.find(std::string("KIWOOM_DEMO is \"") + junk + "\"") != std::string::npos);
    REQUIRE(refused.key().empty());
  }
}

TEST_CASE(
  "A KIS credential file is demo unless its demo key says false",
  "[broker][kis][config][json]"
) {
  const TempCredentialFile paper(
    "kis-paper", R"({"app_key":"app-key","app_secret":"app-secret","demo":true})"
  );
  kis::KisConfig paper_config;
  paper_config.from_json(paper.str());
  REQUIRE(paper_config.is_demo_);
  REQUIRE(paper_config.key() == "app-key");

  const TempCredentialFile live(
    "kis-live", R"({"app_key":"app-key","app_secret":"app-secret","demo":false})"
  );
  kis::KisConfig live_config;
  live_config.from_json(live.str());
  REQUIRE_FALSE(live_config.is_demo_);

  const TempCredentialFile missing(
    "kis-missing", R"({"app_key":"app-key","app_secret":"app-secret"})"
  );
  kis::KisConfig missing_config;
  missing_config.is_demo_ = false;
  missing_config.from_json(missing.str());
  REQUIRE(missing_config.is_demo_);

  // A file refused for any reason leaves an already loaded config as it was. A refusal that changed
  // half the fields could leave a live config pointed at the wrong keys, or the reverse.
  const TempCredentialFile malformed_secret(
    "kis-malformed", R"({"app_key":"other-key","app_secret":7,"demo":true})"
  );
  CHECK_FALSE(refusal_of([&] { live_config.from_json(malformed_secret.str()); }).empty());
  CHECK_FALSE(live_config.is_demo_);
  CHECK(live_config.key() == "app-key");
  CHECK(live_config.secret() == "app-secret");
  CHECK(std::string_view(live_config.rest_host()) == "openapi.koreainvestment.com");

  for (const char* demo : {R"("false")", R"("1")", "1", "0", "null"}) {
    const TempCredentialFile junk(
      "kis-junk",
      std::string(R"({"app_key":"app-key","app_secret":"app-secret","demo":)") + demo + "}"
    );
    kis::KisConfig refused;
    const auto message = refusal_of([&] { refused.from_json(junk.str()); });
    INFO("\"demo\": " << demo);
    REQUIRE(message.find("not true or false") != std::string::npos);
    REQUIRE(message.find(junk.str()) != std::string::npos);
    REQUIRE(refused.key().empty());
  }
}

TEST_CASE(
  "A KIS credential file also selects the account",
  "[broker][kis][config][json]"
) {
  const TempCredentialFile full(
    "kis-account",
    R"({"app_key":"k","app_secret":"s","account_number":"12345678","account_product_code":"22"})"
  );
  kis::KisAccount account;
  account.from_json(full.str());
  CHECK(account.account_number_ == "12345678");
  CHECK(account.product_code_ == "22");

  const TempCredentialFile cash("kis-account-cash", R"({"account_number":"12345678"})");
  kis::KisAccount defaulted;
  defaulted.from_json(cash.str());
  CHECK(defaulted.product_code_ == "01");

  // A refusal names the file and the field, and repeats no digit the file holds.
  for (const char* body :
       {R"({"account_number":"1234567"})", R"({"account_number":12345678})",
        R"({"account_number":"98765432","account_product_code":"7"})", R"({"app_key":"k"})"}) {
    const TempCredentialFile junk("kis-account-junk", body);
    kis::KisAccount refused;
    const auto message = refusal_of([&] { refused.from_json(junk.str()); });
    INFO(body);
    REQUIRE_FALSE(message.empty());
    CHECK(message.find(junk.str()) != std::string::npos);
    for (const char* digits : {"1234567", "98765432"}) {
      CHECK(message.find(digits) == std::string::npos);
    }
    CHECK(refused.account_number_.empty());
  }
}

TEST_CASE(
  "A Kiwoom credential file is demo unless its demo key says false",
  "[broker][kiwoom][config][json]"
) {
  const TempCredentialFile live(
    "kiwoom-live", R"({"appkey":"app-key","secretkey":"secret-key","demo":false})"
  );
  kiwoom::KiwoomConfig live_config;
  live_config.from_json(live.str());
  REQUIRE_FALSE(live_config.is_demo_);
  REQUIRE(live_config.key() == "app-key");

  const TempCredentialFile missing(
    "kiwoom-missing", R"({"appkey":"app-key","secretkey":"secret-key"})"
  );
  kiwoom::KiwoomConfig missing_config;
  missing_config.is_demo_ = false;
  missing_config.from_json(missing.str());
  REQUIRE(missing_config.is_demo_);

  const TempCredentialFile malformed_secret(
    "kiwoom-malformed", R"({"appkey":"other-key","secretkey":null,"demo":true})"
  );
  CHECK_FALSE(refusal_of([&] { live_config.from_json(malformed_secret.str()); }).empty());
  CHECK_FALSE(live_config.is_demo_);
  CHECK(live_config.key() == "app-key");
  CHECK(live_config.secret() == "secret-key");
  CHECK(std::string_view(live_config.rest_host()) == "api.kiwoom.com");

  for (const char* demo : {R"("true")", "0", "[]"}) {
    const TempCredentialFile junk(
      "kiwoom-junk",
      std::string(R"({"appkey":"app-key","secretkey":"secret-key","demo":)") + demo + "}"
    );
    kiwoom::KiwoomConfig refused;
    const auto message = refusal_of([&] { refused.from_json(junk.str()); });
    INFO("\"demo\": " << demo);
    REQUIRE(message.find("not true or false") != std::string::npos);
    REQUIRE(refused.key().empty());
  }
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
  "a Toss sellable-quantity inquiry refuses a symbol that would change its URL",
  "[toss][account]"
) {
  // The symbol goes into the query string; one carrying `&` or `?` must not reach the wire.
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  auto client =
    std::make_shared<tossinvest::TossClient>(ioc.get_executor(), context, tossinvest::TossConfig{});
  bool completed = false;
  client->async_sellable_quantity(
    InstrumentId{Country::KR, Venue::COMBINED, "005930&symbol=000660"}, 1,
    [&completed](Result<Decimal64> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_instrument");
      REQUIRE_FALSE(result.error().reached_broker_);
      completed = true;
    }
  );
  ioc.run();
  REQUIRE(completed);
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
  "A POST after an idle pause is not written into a connection the server closed",
  "[http][tls][retry][idle]"
) {
  // The server closes a kept-alive connection idle for 200 ms, as KIS's hosts do after five seconds.
  // A POST is never replayed, so written into that connection it failed after its bytes left: an
  // order nobody received, reported as one that may exist (observed on a KIS demo account).
  const auto post_after_pause = [](
    std::chrono::milliseconds limit, std::chrono::milliseconds pause, bool server_closes_idle = true
  ) {
    test::LocalHttpsServer server;
    if (server_closes_idle) {
      server.set_idle_timeout(std::chrono::milliseconds(200));
    }
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    network::HttpClientOptions options;
    options.unreplayed_idle_limit_ = limit;
    auto client = std::make_shared<network::AsyncHttpClient>(
      ioc.get_executor(), context, "localhost", server.port(), options
    );

    network::HttpRequest read;
    read.target_ = "/read";
    bool read_ok = false;
    client->async_request(std::move(read), [&read_ok](Result<network::HttpResponse> result) {
      read_ok = static_cast<bool>(result);
    });
    ioc.run();
    REQUIRE(read_ok);

    std::this_thread::sleep_for(pause);
    network::HttpRequest order;
    order.method_ = http::verb::post;
    order.target_ = "/order";
    order.body_ = "order-like-payload";
    std::optional<Result<network::HttpResponse>> outcome;
    client->async_request(std::move(order), [&outcome](Result<network::HttpResponse> result) {
      outcome.emplace(std::move(result));
    });
    ioc.restart();
    // Bounded, so a client that reconnects for ever fails the test instead of hanging it.
    ioc.run_for(std::chrono::seconds(10));
    REQUIRE(outcome.has_value());
    return std::make_pair(std::move(*outcome), server.connectionCount());
  };

  SECTION("past the limit, the POST goes out on a fresh connection") {
    const auto [outcome, connections] = post_after_pause(std::chrono::milliseconds(100), std::chrono::milliseconds(400));
    REQUIRE(outcome);
    CHECK(outcome.value().body() == "/order");
    CHECK(connections == 2);
  }

  SECTION("within the limit, the POST reuses the connection") {
    const auto [outcome, connections] =
      post_after_pause(std::chrono::seconds(5), std::chrono::milliseconds(50), /*server_closes_idle=*/false);
    REQUIRE(outcome);
    CHECK(connections == 1);
  }

  SECTION("a limit of zero or below reconnects once per request, never in a loop") {
    // A fresh connection starts its own idle clock; without the once-per-request bound it would trip
    // its own check and the POST would never be written.
    for (const auto limit : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
      const auto [outcome, connections] =
        post_after_pause(limit, std::chrono::milliseconds(50), /*server_closes_idle=*/false);
      REQUIRE(outcome);
      CHECK(outcome.value().body() == "/order");
      CHECK(connections <= 2);
    }
  }

  SECTION("without the limit it fails as a request that may have reached the broker") {
    // The control: the case the limit exists for, reproduced.
    // A second's pause against a 200 ms server timeout, so a slow test thread cannot hide the close.
    const auto [outcome, connections] = post_after_pause(std::chrono::hours(1), std::chrono::milliseconds(1000));
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().category_ == ErrorCategory::TRANSPORT);
    CHECK(outcome.error().reached_broker_);
    CHECK(connections == 1);
  }

  SECTION("the blocking client reconnects the same way") {
    test::LocalHttpsServer server;
    server.set_idle_timeout(std::chrono::milliseconds(200));
    net::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    test::trustLocalhostCertificate(context);
    network::HttpClientOptions options;
    options.unreplayed_idle_limit_ = std::chrono::milliseconds(100);
    network::HttpClient client(ioc, context, "localhost", server.port(), options);
    REQUIRE(client.get("/read").body() == "/read");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(client.post("/order", "order-like-payload").body() == "/order");
    CHECK(server.connectionCount() == 2);
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

  live.is_demo_ = false;
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
  "two KIS rows for one holding that disagree cannot authorize a sell",
  "[broker][kis][holdings]"
) {
  // **It starts at the wire rather than at a hand-built portfolio.** `parse_holdings_page` appends every `output1` row and `append_holdings_page`
  // concatenates pages, neither checking for a repeated `pdno` -- so a KIS response listing one
  // instrument twice with different `ord_psbl_qty` reaches the guard, and every row carries the same
  // enforceable basis the completed traversal assigns. Picking the first row's figure would put a sell
  // above the real limit on the wire whenever the larger row comes first.
  network::HttpResponse response{http::status::ok, 11};
  response.body() = R"({
    "rt_cd":"0",
    "msg_cd":"MCA00000",
    "output1":[{
      "pdno":"005930",
      "prdt_name":"Samsung Electronics",
      "hldg_qty":"100",
      "ord_psbl_qty":"100",
      "pchs_avg_pric":"70000.0000",
      "pchs_amt":"7000000",
      "prpr":"72000",
      "evlu_amt":"7200000",
      "evlu_pfls_amt":"200000",
      "evlu_pfls_rt":"2.857142"
    },{
      "pdno":"005930",
      "prdt_name":"Samsung Electronics",
      "hldg_qty":"30",
      "ord_psbl_qty":"30",
      "pchs_avg_pric":"70000.0000",
      "pchs_amt":"2100000",
      "prpr":"72000",
      "evlu_amt":"2160000",
      "evlu_pfls_amt":"60000",
      "evlu_pfls_rt":"2.857142"
    }],
    "output2":[{
      "pchs_amt_smtl_amt":"9100000",
      "evlu_amt_smtl_amt":"9360000",
      "evlu_pfls_smtl_amt":"260000"
    }]
  })";

  std::string foreign_cursor;
  std::string domestic_cursor;
  const auto parsed = kis::KisClientTestAccess::parse_holdings_page(
    std::move(response), foreign_cursor, domestic_cursor
  );
  REQUIRE(parsed);
  // The parser does not dedupe, which is the premise rather than the defect: two rows arrive.
  REQUIRE(parsed.value().positions_.size() == 2);

  // The basis a completed KIS traversal assigns, applied to the whole snapshot as the client does.
  strategy::Portfolio<kis::Broker> portfolio;
  portfolio.positions_ = parsed.value().positions_;
  portfolio.as_of_ = std::chrono::system_clock::now();
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };

  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const strategy::RiskGuard guard(limits);

  strategy::PlannedOrder sell;
  sell.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  sell.side_ = OrderSide::SELL;
  sell.quantity_ = Decimal64::parse("100");
  sell.price_ = Decimal64::parse("72000");

  strategy::GuardInputs inputs;
  inputs.now_ = std::chrono::system_clock::now();
  inputs.phase_ = strategy::MarketPhase::OPEN;

  const auto refused = guard.check(sell, portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == strategy::GuardReason::SELLABLE_BASIS_UNESTABLISHED);

  // **And in the other row order**, because a first-row rule is right by accident in one of them: with
  // 30 first, a sell of 100 would be refused for the quantity and the test would pass while the defect
  // stood. Asking for 30 is what exposes it.
  strategy::Portfolio<kis::Broker> reversed = portfolio;
  std::swap(reversed.positions_[0], reversed.positions_[1]);
  sell.quantity_ = Decimal64::parse("30");
  const auto also = guard.check(sell, reversed, inputs);
  REQUIRE(also);
  REQUIRE(also->reason_ == strategy::GuardReason::SELLABLE_BASIS_UNESTABLISHED);
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
  // The fourth of the four places that must agree; see kKoreanInstrumentVenue.
  REQUIRE(result.value().positions_[0].instrument_.venue_ == kKoreanInstrumentVenue);
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

/** Builds a market sell of a fraction of one US share, the one fractional order Toss accepts. */
OrderRequest us_market_fractional_sell() {
  OrderRequest order;
  order.instrument_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
  order.side_ = OrderSide::SELL;
  order.type_ = OrderType::MARKET;
  order.time_in_force_ = TimeInForce::DAY;
  order.quantity_ = Decimal64::parse("0.123456");
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
  live.is_demo_ = false;
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
    // `korean_limit_buy` states no routing venue, so the field is omitted and
    // KIS applies its documented default. Routing is `OrderRequest::venue_`;
    // the instrument's venue is identity and naming a venue there is refused.
    REQUIRE_FALSE(body.contains("EXCG_ID_DVSN_CD"));
  }

  SECTION("a combined route is transmitted as SOR") {
    auto order = korean_limit_buy();
    order.venue_ = Venue::COMBINED;
    const auto request =
      kis::KisClientTestAccess::place_order_request(account, live, order, "00");
    // Expressing no venue is not the same as expressing the combined one: the
    // broker's documented default is the honest outcome for a caller who asked
    // for nothing, and a substitution for a caller who asked for something.
    // The combined book is routed, which is what KIS calls SOR.
    REQUIRE(json::parse(request.body_).as_object().at("EXCG_ID_DVSN_CD").as_string() == "SOR");
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
    order.venue_ = Venue::NXT;
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
  {"22", OrderType::STOP_LIMIT, TimeInForce::DAY, OrderSession::REGULAR, true, true, false, true},
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
  order.venue_ = venue;
  order.type_ = shape.type_;
  order.time_in_force_ = shape.condition_;
  order.session_ = shape.session_;
  // A basis priced by the caller keeps the fixture's price; one priced by the
  // book must carry none, because `validate_price` refuses a stray price and
  // would mask the answer this test is asking for.
  //
  // This read `!= OrderType::LIMIT`, which cleared the price on
  // `CONDITIONAL_LIMIT` too -- so every `02` case in this table was a
  // priceless conditional-limit order, and the table agreed with a validator
  // that refused the priced one. A fixture that arranges for the defect not to
  // matter; `carries_caller_price` is the same predicate production uses.
  if (!carries_caller_price(shape.type_)) {
    order.price_.reset();
  }
  // The same for the trigger: `22` needs one and `validate_trigger` would refuse it without, which
  // is an answer about the trigger rather than about the code. Before `22` was requestable this row
  // expected a refusal and got one for exactly that reason, so the table could not tell the two
  // apart.
  if (carries_trigger_price(shape.type_)) {
    order.trigger_price_ = Decimal64::parse("59000");
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
    order.venue_ = Venue::NXT;
    std::string division;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    // The sheet lists 01 under [KRX] and [SOR] and not under [NXT]. This used
    // to compose 01 regardless and let the broker refuse it, at the far end of
    // a POST that may never be replayed.
    REQUIRE(error);
    REQUIRE_FALSE(error->reached_broker_);
    REQUIRE(division.empty());

    order.venue_ = Venue::KRX;
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

TEST_CASE(
  "naming a venue on the instrument is refused rather than routed on",
  "[broker][order][identity]"
) {
  // The refusal that keeps an order's identity equal to its holding's. Written
  // without a test, and a surviving mutation said so: deleting it failed
  // nothing, because every other case in this file already uses the identity
  // venue. The value it refuses is one callers once wrote, so the
  // path is reachable and the refusal is what makes the round trip hold.
  SECTION("KIS") {
    auto order = korean_limit_buy();
    order.instrument_.venue_ = Venue::KRX;
    std::string division;
    const auto error = kis::KisClientTestAccess::validate_order(order, division);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "invalid_instrument");
    REQUIRE_FALSE(error->reached_broker_);

    // The same routing, expressed where routing belongs, is accepted.
    order.instrument_.venue_ = kKoreanInstrumentVenue;
    order.venue_ = Venue::KRX;
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division));
    REQUIRE(division == "00");
  }

  SECTION("Toss, which offers no venue choice at all") {
    boost::asio::io_context ioc;
    ssl::context context(ssl::context::tls_client);
    auto client = std::make_shared<tossinvest::TossClient>(
      ioc.get_executor(), context, tossinvest::TossConfig{}
    );
    const auto expect = [&](OrderRequest order, std::string_view code) {
      bool completed = false;
      client->async_place_order(7, std::move(order), [&](Result<OrderAck> result) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().broker_code_ == code);
        REQUIRE_FALSE(result.error().reached_broker_);
        completed = true;
      });
      ioc.restart();
      ioc.run();
      REQUIRE(completed);
    };

    auto on_instrument = korean_limit_buy();
    on_instrument.instrument_.venue_ = Venue::KRX;
    expect(std::move(on_instrument), "invalid_instrument");

    // And Toss routes nowhere, so naming a route is refused as well.
    auto routed = korean_limit_buy();
    routed.venue_ = Venue::KRX;
    expect(std::move(routed), "venue_unavailable");
  }
}

TEST_CASE(
  "an order routed to a named venue is still the instrument it was placed in",
  "[broker][kis][order][identity]"
) {
  // The end-to-end shape of the defect the venue split exists to prevent, from
  // the request a caller writes to the verdict recovery reaches. Routing an
  // order to NXT must change nothing about *which instrument* it is, at any
  // step, because every limit and every reconciliation asks that question by
  // comparing `InstrumentId`s and a holding carries no venue at all.
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;
  live.is_demo_ = false;

  auto order = korean_limit_buy();
  order.venue_ = Venue::NXT;

  // 1. The request routes to NXT and still names the identity instrument.
  std::string division;
  REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division, live));
  const auto body = json::parse(
    kis::KisClientTestAccess::place_order_request(account, live, order, division).body_
  ).as_object();
  REQUIRE(body.at("EXCG_ID_DVSN_CD").as_string() == "NXT");
  REQUIRE(order.instrument_.venue_ == kKoreanInstrumentVenue);

  // 2. The holding and the listing of that order agree on the instrument, and
  //    the listing reports the routing venue beside it.
  auto holdings = kis::KisClientTestAccess::parse_holdings(
    json_response(
      R"({"rt_cd":"0","ctx_area_fk100":"","ctx_area_nk100":"","output1":[{)"
      R"("pdno":"005930","prdt_name":"Samsung","hldg_qty":"10","ord_psbl_qty":"10",)"
      R"("pchs_avg_pric":"70000.0000","pchs_amt":"700000","prpr":"72000",)"
      R"("evlu_amt":"720000","evlu_pfls_amt":"20000","evlu_pfls_rt":"2.857142"}],)"
      R"("output2":[{"pchs_amt_smtl_amt":"700000","evlu_amt_smtl_amt":"720000",)"
      R"("evlu_pfls_smtl_amt":"20000"}]})"
    )
  );
  REQUIRE(holdings);
  auto listing = kis::KisClientTestAccess::parse_open_orders(
    json_response(
      R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"0001569139",)"
      R"("orgn_odno":"","ord_dvsn_name":"limit","pdno":"005930","prdt_name":"Samsung",)"
      R"("rvse_cncl_dvsn_name":"modify","ord_qty":"10","ord_unpr":"70000",)"
      R"("ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0","psbl_qty":"10",)"
      R"("sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00","excg_id_dvsn_cd":"NXT"}]})"
    )
  );
  REQUIRE(listing);
  const auto& resting = listing.value().orders_.front().order_;
  const auto& position = holdings.value().positions_.front();
  REQUIRE(resting.instrument_ == position.instrument_);
  REQUIRE(resting.instrument_ == order.instrument_);
  REQUIRE(resting.venue_ == Venue::NXT);

  // 3. A portfolio built from both finds the resting order against the
  //    instrument the caller named. This is the number that silently read zero.
  strategy::Portfolio<kis::Broker> portfolio;
  portfolio.positions_ = holdings.value().positions_;
  WorkingOrder<kis::Broker> working;
  working.order_ = resting;
  portfolio.working_.push_back(working);
  REQUIRE(
    portfolio.working_quantity(order.instrument_, OrderSide::BUY) == Decimal64::parse("10")
  );
  REQUIRE(portfolio.net_quantity_of(order.instrument_) == Decimal64::parse("10"));

  // 4. A cancellation of it addresses the venue it was routed to.
  working.amendable_quantity_ = Decimal64::parse("10");
  REQUIRE(cancel_all(working).venue_ == Venue::NXT);

  // 5. And recovery, after the window has closed, finds it. This is the leg the
  //    whole split exists for: a journal entry recorded for an NXT-routed order
  //    and the broker's listing of that same order must be the same
  //    instrument, or the verdict is NOT_PLACED, the hold clears, and the
  //    runner sends a second live order.
  runner::JournalEntry entry;
  entry.id_ = "run-1";
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = runner::JournalOutcome::PENDING;
  // The adapter reads no order time from KIS's open-order inquiry, and a row without one contradicts
  // no send window; so the entry is stamped at a fixed instant (13:14:38 KST, as listed).
  REQUIRE_FALSE(resting.ordered_at_);
  entry.recorded_at_ = std::chrono::system_clock::from_time_t(1773116078);
  entry.instrument_ = order.instrument_;
  entry.side_ = order.side_;
  entry.type_ = order.type_;
  entry.time_in_force_ = order.time_in_force_;
  entry.session_ = order.session_;
  entry.quantity_ = order.quantity_;
  entry.price_ = order.price_;

  runner::OpenInstruction instruction;
  instruction.entry_ = entry;
  instruction.opened_at_ = entry.recorded_at_;

  const auto closed = entry.recorded_at_ + std::chrono::minutes(10);
  const auto match = runner::match_against(instruction, {resting}, closed);
  REQUIRE(match.verdict_ == runner::MatchVerdict::MATCHED);
  REQUIRE(match.order_id_ == "0001569139");
  // Stated as its own assertion because it is the harm, not the mechanism.
  REQUIRE(match.verdict_ != runner::MatchVerdict::NOT_PLACED);
}

TEST_CASE("every KIS market-data request reads the tape the adapter was told to", "[broker][kis]") {
  // The defect this replaces: five of the seven domestic market-data verbs sent `J` (KRX alone)
  // unconditionally while the quote derived its code from `InstrumentId::venue_`, whose default
  // means the consolidated tape. So a caller's last price and their bars described different
  // markets, and `Context::last_price` -- what the exposure ceiling is measured against -- is
  // written from both.
  kis::KisConfig config;

  SECTION("consolidated is the default") {
    REQUIRE(std::string(kis::market_division_code(config.market_tape_)) == "UN");
    REQUIRE(
      kis::KisClientTestAccess::market_query(config, "005930") ==
      "?FID_COND_MRKT_DIV_CODE=UN&FID_INPUT_ISCD=005930"
    );
  }

  SECTION("each tape has one code, and the fragment carries it") {
    config.market_tape_ = kis::MarketTape::KRX;
    REQUIRE(std::string(kis::market_division_code(config.market_tape_)) == "J");
    REQUIRE(
      kis::KisClientTestAccess::market_query(config, "005930") ==
      "?FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=005930"
    );

    config.market_tape_ = kis::MarketTape::NXT;
    REQUIRE(std::string(kis::market_division_code(config.market_tape_)) == "NX");
    REQUIRE(
      kis::KisClientTestAccess::market_query(config, "005930") ==
      "?FID_COND_MRKT_DIV_CODE=NX&FID_INPUT_ISCD=005930"
    );
  }

  SECTION("the instrument cannot influence it, by construction") {
    // `market_query` takes a symbol and a config and **no `InstrumentId`**, which is the guarantee
    // rather than an assertion about it: there is no identity for a tape to be derived from. That
    // matters because the first draft of this change did derive it from `InstrumentId::venue_`,
    // and setting that field to name a book makes an order invalid -- see the next test.
    REQUIRE(
      kis::KisClientTestAccess::market_query(config, "000660").find("FID_INPUT_ISCD=000660") !=
      std::string::npos
    );
  }
}

TEST_CASE("a KIS client hands its token on and adopts only a live one", "[broker][kis]") {
  // KIS issues one token per key per minute (EGW00133), so a process run per action keeps its token
  // for the next. No request is made here: adopting is a local decision.
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  kis::KisBlockingClient client(tls, kis::KisConfig{});
  REQUIRE(client.token().access_token_.empty());

  kis::KisRest expired;
  expired.access_token_ = "expired-token";
  expired.expires_at_ = std::chrono::system_clock::now() - std::chrono::minutes(1);
  CHECK_FALSE(client.adopt_token(expired));
  kis::KisRest empty;
  empty.expires_at_ = std::chrono::system_clock::now() + std::chrono::hours(20);
  CHECK_FALSE(client.adopt_token(empty));
  // Inside the five-minute margin every other validity check uses.
  kis::KisRest ending;
  ending.access_token_ = "ending-token";
  ending.expires_at_ = std::chrono::system_clock::now() + std::chrono::minutes(2);
  CHECK_FALSE(client.adopt_token(ending));
  CHECK(client.token().access_token_.empty());

  kis::KisRest live;
  live.access_token_ = "kept-token";
  live.expires_at_ = std::chrono::system_clock::now() + std::chrono::hours(20);
  REQUIRE(client.adopt_token(live));
  CHECK(client.token().access_token_ == "kept-token");
  CHECK(client.token().expires_at_ == live.expires_at_);
  CHECK(client.token().valid());
}

TEST_CASE("a KIS demo account trades on KRX and is told so before I/O", "[broker][kis][order]") {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig demo;
  demo.is_demo_ = true;
  kis::KisConfig live;
  live.is_demo_ = false;

  SECTION("the default order names no venue and works on either account") {
    // Routing is opt-in. An order that states no `venue_` omits
    // `EXCG_ID_DVSN_CD`, and KIS documents an omitted field as proceeding at
    // KRX -- which a demo account can reach. This is the state an earlier
    // version broke by routing on `instrument_.venue_`, whose
    // default is `COMBINED` and which had started meaning `SOR`: every default
    // demo order would have been refused, at the far end of a POST this library
    // never replays.
    auto order = korean_limit_buy();
    std::string division;
    for (const auto& config : {demo, live}) {
      REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division, config));
      REQUIRE(division == "00");
      REQUIRE_FALSE(json::parse(
        kis::KisClientTestAccess::place_order_request(account, config, order, division).body_
      ).as_object().contains("EXCG_ID_DVSN_CD"));
    }
  }

  SECTION("an explicit combined route is refused on demo and sent live") {
    auto order = korean_limit_buy();
    order.venue_ = Venue::COMBINED;
    std::string division;
    // SOR is a venue the sheet says a demo account does not have, and asking
    // for it explicitly is a request this adapter can answer before I/O rather
    // than after a POST.
    const auto error = kis::KisClientTestAccess::validate_order(order, division, demo);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "venue_unavailable");
    REQUIRE_FALSE(error->reached_broker_);

    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division, live));
    REQUIRE(json::parse(
      kis::KisClientTestAccess::place_order_request(account, live, order, division).body_
    ).as_object().at("EXCG_ID_DVSN_CD").as_string() == "SOR");
  }

  SECTION("a venue the demo account cannot reach is refused before any request") {
    auto order = korean_limit_buy();
    order.venue_ = Venue::NXT;
    std::string division;
    const auto error = kis::KisClientTestAccess::validate_order(order, division, demo);
    REQUIRE(error);
    REQUIRE(error->broker_code_ == "venue_unavailable");
    REQUIRE_FALSE(error->reached_broker_);
    // The same request is fine on a live account, so the refusal is about the
    // configuration rather than about the order.
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division, live));
  }

  SECTION("a demo order is checked against KRX's code set, not the route's") {
    // 중간가 (`21`) is published for KRX and NXT and not for SOR. A demo
    // account reaches KRX whatever it asks for, so checking an explicit
    // combined route against SOR's smaller list would refuse an order the
    // account can actually place -- if the route were not already refused
    // above, which it is. The case that matters is an explicit KRX route,
    // which demo does allow.
    auto order = korean_limit_buy();
    order.type_ = OrderType::MID_PRICE;
    order.price_.reset();
    order.venue_ = Venue::KRX;
    std::string division;
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(order, division, demo));
    REQUIRE(division == "21");

    // And an explicit SOR route does not publish 21 on a live account, so the
    // per-venue table is still what decides.
    order.venue_ = Venue::COMBINED;
    REQUIRE(kis::KisClientTestAccess::validate_order(order, division, live));
  }

  SECTION("an amendment on a demo account omits the venue too") {
    kis::KisAmendment amendment;
    amendment.order_ = kis::KisOrderRef{"0000117057", "00950"};
    // A demo listing reports no venue this adapter recognizes, so an amendment
    // built from one carries `UNSPECIFIED` and omits the field, exactly as the
    // placement does.
    amendment.venue_ = Venue::UNSPECIFIED;
    amendment.quantity_ = Decimal64::parse("1");
    amendment.price_ = Decimal64::parse("70000");
    std::string division;
    REQUIRE_FALSE(kis::KisClientTestAccess::amendment_division(amendment, division, demo));
    const auto body = json::parse(
      kis::KisClientTestAccess::amend_order_request(account, demo, amendment, false, division).body_
    ).as_object();
    REQUIRE_FALSE(body.contains("EXCG_ID_DVSN_CD"));
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

  SECTION("a code outside the preservation alphabet leaves the field empty, not wrong") {
    OrderSnapshot order;
    // `Code` takes A-Z, 0-9 and `_`. Every code either broker sends today fits,
    // so this is about a future one. Empty is the same answer as "the broker
    // reported no code", which is the honest one: the alternative is an
    // allocation on every row, or throwing out of a response parse over a
    // single odd value.
    kis::KisClientTestAccess::apply_order_shape("00-a", order);
    REQUIRE(order.shape_code_.view().empty());
    REQUIRE(order.type_ == OrderType::UNKNOWN);
    REQUIRE(order.time_in_force_ == TimeInForce::UNKNOWN);
    REQUIRE(order.session_ == OrderSession::UNKNOWN);
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
  live.is_demo_ = false;

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
    working.order_.instrument_ = InstrumentId{Country::KR, kKoreanInstrumentVenue, "005930"};
    // The venue the broker routed it to, which is not the instrument's.
    working.order_.venue_ = Venue::KRX;
    working.amendable_quantity_ = Decimal64::parse("5");

    const auto instruction = cancel_all(working);
    REQUIRE(instruction.shape_.type_ == OrderType::BEST_LIMIT);
    REQUIRE(instruction.shape_.time_in_force_ == TimeInForce::FILL_OR_KILL);
    REQUIRE(instruction.shape_.session_ == OrderSession::AFTER_MARKET);
    REQUIRE(instruction.venue_ == Venue::KRX);
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
  "KIS answers the modifiable-order read on demo from today's order history",
  "[broker][kis][order]"
) {
  // Demo has no `TTTC0084R`. Refused, it kept a demo loop's startup gate closed for good.
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig demo;
  demo.is_demo_ = true;

  SECTION("the request is the daily inquiry, today, unfilled orders only") {
    const auto request =
      kis::KisClientTestAccess::open_orders_request(account, demo, "", "", false);
    REQUIRE(request.target_.find("/uapi/domestic-stock/v1/trading/inquire-daily-ccld") == 0);
    CHECK(header_of(request, "tr_id") == "VTTC0081R");
    CHECK(request.target_.find("&CCLD_DVSN=02&") != std::string::npos);
    const auto day = kis::today_in_seoul();
    char today[9];
    std::snprintf(today, sizeof today, "%04d%02u%02u", day.year_, day.month_, day.day_);
    CHECK(request.target_.find(std::string("INQR_STRT_DT=") + today) != std::string::npos);
    CHECK(request.target_.find(std::string("INQR_END_DT=") + today) != std::string::npos);
  }

  SECTION("each row with a remaining quantity is listed, and only those") {
    // Each row is of a kind the filter must handle, not a sequence a demo session produced; the
    // observed sequence is in the next section. Row 5 is a hypothesis, since a partial cancel has not
    // been observed: a remnant that still rests is listed whatever `cncl_yn` says, because hiding a
    // resting order is the error that sends it twice.
    auto result = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output1":[)"
        R"({"ord_dt":"20261002","ord_gno_brno":"06010","odno":"0000011111","orgn_odno":"",)"
        R"("pdno":"005930","prdt_name":"Samsung","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("ord_qty":"1","ord_unpr":"200000","ord_tmd":"090102","tot_ccld_qty":"0",)"
        R"("tot_ccld_amt":"0","avg_prvs":"0","cncl_yn":"N","rmn_qty":"1","rjct_qty":"0"},)"
        R"({"ord_dt":"20261002","ord_gno_brno":"06010","odno":"0000011112","orgn_odno":"",)"
        R"("pdno":"005930","prdt_name":"Samsung","sll_buy_dvsn_cd":"01","ord_dvsn_cd":"01",)"
        R"("ord_qty":"1","ord_unpr":"0","ord_tmd":"090103","tot_ccld_qty":"1",)"
        R"("tot_ccld_amt":"274500","avg_prvs":"274500","cncl_yn":"N","rmn_qty":"0","rjct_qty":"0"},)"
        R"({"ord_dt":"20261002","ord_gno_brno":"06010","odno":"0000011113","orgn_odno":"",)"
        R"("pdno":"005930","prdt_name":"Samsung","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("ord_qty":"1","ord_unpr":"200000","ord_tmd":"090104","tot_ccld_qty":"0",)"
        R"("tot_ccld_amt":"0","avg_prvs":"0","cncl_yn":"Y","rmn_qty":"0","rjct_qty":"0"},)"
        R"({"ord_dt":"20261002","ord_gno_brno":"06010","odno":"0000011115","orgn_odno":"0000011111",)"
        R"("pdno":"005930","prdt_name":"Samsung","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("ord_qty":"1","ord_unpr":"201000","ord_tmd":"090200","tot_ccld_qty":"0",)"
        R"("tot_ccld_amt":"0","avg_prvs":"0","cncl_yn":"N","rmn_qty":"1","rjct_qty":"0"},)"
        R"({"ord_dt":"20261002","ord_gno_brno":"06010","odno":"0000011116","orgn_odno":"",)"
        R"("pdno":"005930","prdt_name":"Samsung","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("ord_qty":"3","ord_unpr":"200000","ord_tmd":"090300","tot_ccld_qty":"1",)"
        R"("tot_ccld_amt":"200000","avg_prvs":"200000","cncl_yn":"Y","rmn_qty":"1","rjct_qty":"0"})"
        R"(]})"
      ),
      /*demo=*/true
    );
    REQUIRE(result);
    REQUIRE(result.value().orders_.size() == 3);
    const auto& resting = result.value().orders_[0];
    CHECK(resting.order_ref_.branch_number_ == "06010");
    CHECK(resting.order_ref_.order_number_ == "0000011111");
    CHECK(resting.amendable_quantity_ == Decimal64::parse("1"));
    CHECK(resting.order_.side_ == OrderSide::BUY);
    CHECK(resting.order_.type_ == OrderType::LIMIT);
    CHECK(resting.order_.price_ == Decimal64::parse("200000"));
    CHECK(resting.order_.status_ == OrderStatus::PENDING);
    CHECK(resting.stop_released_.empty());

    const auto& amended = result.value().orders_[1];
    CHECK(amended.order_ref_.order_number_ == "0000011115");
    CHECK(amended.original_order_.branch_number_ == "06010");
    CHECK(amended.original_order_.order_number_ == "0000011111");

    const auto& remnant = result.value().orders_[2];
    CHECK(remnant.order_ref_.order_number_ == "0000011116");
    CHECK(remnant.amendable_quantity_ == Decimal64::parse("1"));
    CHECK(remnant.order_.status_ == OrderStatus::PARTIAL_FILLED);
  }

  SECTION("the rows a demo session produced: place, amend, cancel") {
    // Observed on 2026-10-02 at 09:01-09:06 KST on a demo account: the fields the probe printed from
    // `CCLD_DVSN=00`, with placeholders for the rest. After an amendment the original keeps `cncl_yn`
    // `N` and drops to `rmn_qty` 0; the cancel is a row of its own (`cncl_yn` `Y`, price 0, `rmn_qty`
    // 0) naming the order it cancels, which keeps `cncl_yn` `N` (and `cncl_cfrm_qty` 1, not read). A
    // new order's `orgn_odno` is all zeros. `CCLD_DVSN=02`, which the client sends, returned one row
    // after the amendment and none after the cancel, as asserted here on the wider rows.
    const auto row = [](const char* odno, const char* original, const char* price, const char* remaining,
                        const char* canceled) {
      return std::string(R"({"ord_dt":"20261002","ord_gno_brno":"00950","odno":")") + odno +
        R"(","orgn_odno":")" + original + R"(","pdno":"005930","prdt_name":"Samsung",)"
        R"("sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00","ord_qty":"1","ord_unpr":")" + price +
        R"(","ord_tmd":"090351","tot_ccld_qty":"0","tot_ccld_amt":"0","avg_prvs":"0","cncl_yn":")" +
        canceled + R"(","rmn_qty":")" + remaining + R"(","rjct_qty":"0"})";
    };
    const auto page = [](const std::vector<std::string>& rows) {
      std::string body = R"({"rt_cd":"0","output1":[)";
      for (std::size_t i = 0; i < rows.size(); ++i) {
        body += (i == 0 ? "" : ",") + rows[i];
      }
      return body + "]}";
    };

    auto amended = kis::KisClientTestAccess::parse_open_orders(
      json_response(page({row("0000003493", "0000002391", "221000", "1", "N"),
                          row("0000002391", "0000000000", "220000", "0", "N")})),
      /*demo=*/true
    );
    REQUIRE(amended);
    REQUIRE(amended.value().orders_.size() == 1);
    CHECK(amended.value().orders_[0].order_ref_.branch_number_ == "00950");
    CHECK(amended.value().orders_[0].order_ref_.order_number_ == "0000003493");
    CHECK(amended.value().orders_[0].original_order_.order_number_ == "0000002391");
    CHECK(amended.value().orders_[0].order_.price_ == Decimal64::parse("221000"));

    auto canceled = kis::KisClientTestAccess::parse_open_orders(
      json_response(page({row("0000004393", "0000003493", "0", "0", "Y"),
                          row("0000003493", "0000002391", "221000", "0", "N"),
                          row("0000002391", "0000000000", "220000", "0", "N")})),
      /*demo=*/true
    );
    REQUIRE(canceled);
    CHECK(canceled.value().orders_.empty());
  }

  SECTION("a live configuration still asks the modifiable-order inquiry") {
    kis::KisConfig live;
    live.is_demo_ = false;
    const auto request =
      kis::KisClientTestAccess::open_orders_request(account, live, "", "", false);
    CHECK(header_of(request, "tr_id") == "TTTC0084R");
  }
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

  SECTION("a rate limit KIS answers with HTTP 500 is one on a read, and stays undecided on an order") {
    // The shape KIS actually sent on 2026-10-02, on reads: HTTP 500 and a gateway body.
    const char* body =
      R"({"rt_cd":"1","msg1":"초당 거래건수를 초과하였습니다.","msg_cd":"EGW00201","message":"EGW00201"})";

    auto read = kis::KisClientTestAccess::parse_order_history(json_response(body, 500));
    REQUIRE_FALSE(read);
    CHECK(read.error().category_ == ErrorCategory::RATE_LIMIT);
    CHECK(read.error().broker_code_ == "EGW00201");
    CHECK(read.error().http_status_ == 500);
    CHECK(read.error().message_ == body);

    auto listing = kis::KisClientTestAccess::parse_open_orders(json_response(body, 500));
    REQUIRE_FALSE(listing);
    CHECK(listing.error().category_ == ErrorCategory::RATE_LIMIT);

    // An order keeps the code but not the classification: as `HTTP` it is indeterminate and held
    // for the order list, where a rate limit would be retried -- and doubled, had KIS processed it.
    auto order = kis::KisClientTestAccess::parse_order_ack(json_response(body, 500));
    REQUIRE_FALSE(order);
    CHECK(order.error().category_ == ErrorCategory::HTTP);
    CHECK(order.error().broker_code_ == "EGW00201");
    CHECK(runner::outcome_of(order.error()) == runner::SendOutcome::INDETERMINATE);
    CHECK_FALSE(runner::broker_rate_limited(order.error()));
    // The read's error, by contrast, is what the rate-limit retry and the loop's pacing read.
    CHECK(runner::broker_rate_limited(read.error()));
  }

  SECTION("a non-2xx KIS body keeps its category and gains its code") {
    // A gateway code that is not a rate limit, on HTTP 500: still `HTTP`, so an order stays
    // indeterminate rather than refused.
    auto other = kis::KisClientTestAccess::parse_order_history(
      json_response(R"({"rt_cd":"1","msg1":"token","msg_cd":"EGW00123"})", 500)
    );
    REQUIRE_FALSE(other);
    CHECK(other.error().category_ == ErrorCategory::HTTP);
    CHECK(other.error().broker_code_ == "EGW00123");

    // The token endpoint's shape names its code `error_code`.
    auto token = kis::KisClientTestAccess::parse_order_history(json_response(
      R"json({"error_code":"EGW00133","error_description":"접근토큰 발급 잠시 후 다시 시도하세요(1분당 1회)"})json",
      403
    ));
    REQUIRE_FALSE(token);
    CHECK(token.error().category_ == ErrorCategory::HTTP);
    CHECK(token.error().broker_code_ == "EGW00133");

    // A body that is not JSON is today's error, unchanged.
    auto gateway = kis::KisClientTestAccess::parse_order_history(json_response("Bad Gateway", 502));
    REQUIRE_FALSE(gateway);
    CHECK(gateway.error().category_ == ErrorCategory::HTTP);
    CHECK(gateway.error().broker_code_.empty());
    CHECK(gateway.error().message_ == "Bad Gateway");
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

  SECTION("nothing filled and nothing remaining, with no cancel flag, is CANCELED") {
    // Observed on demo (2026-10-02): a cancelled order and an amended original both read so; the
    // cancel flag is on the cancel request's own row.
    REQUIRE(
      derive(quantity("10"), quantity("0"), quantity("0"), quantity("0"), false) ==
      OrderStatus::CANCELED
    );
  }

  SECTION("a partial fill with nothing remaining and no cancel flag is CANCELED, its fill kept") {
    REQUIRE(
      derive(quantity("10"), quantity("4"), quantity("0"), quantity("0"), false) ==
      OrderStatus::CANCELED
    );
  }
}

TEST_CASE(
  "KIS modifiable-order inquiry reports the amendable quantity",
  "[broker][kis][order]"
) {
  const kis::KisAccount account{"12345678", "01"};
  kis::KisConfig live;
  live.is_demo_ = false;

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
    live.is_demo_ = false;
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

  SECTION("a fractional US market sell is accepted and sent at its own scale") {
    const auto order = us_market_fractional_sell();
    REQUIRE_FALSE(tossinvest::TossClientTestAccess::validate_order(order));
    const auto body =
      json::parse(tossinvest::TossClientTestAccess::place_order_request(7, order).body_)
        .as_object();
    REQUIRE(body.at("symbol").as_string() == "AAPL");
    REQUIRE(body.at("side").as_string() == "SELL");
    REQUIRE(body.at("orderType").as_string() == "MARKET");
    REQUIRE(body.at("quantity").as_string() == "0.123456");
    REQUIRE_FALSE(body.contains("price"));
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

  SECTION("a trigger is refused by the client itself, not only by the session") {
    // A caller of `TossClient` does not pass through `BrokerSession`, and Toss has no field for a
    // trigger: sent, this would be acknowledged as the plain limit it is not.
    auto order = korean_limit_buy();
    order.trigger_price_ = Decimal64::parse("69000");
    expect_rejection(std::move(order), "unsupported_trigger_price");
  }

  SECTION("a fractional quantity is rejected") {
    auto order = korean_limit_buy();
    order.quantity_ = Decimal64::parse("0.5");
    expect_rejection(std::move(order), "invalid_quantity");
  }

  SECTION("a fraction is refused everywhere but a US market sell") {
    // Toss's `fractionalQuantityUsSellOnly`: a US buy, a US limit sell and a Korean market sell
    // all carry a fraction Toss refuses, so the library refuses them first.
    auto us_buy = us_market_fractional_sell();
    us_buy.side_ = OrderSide::BUY;
    expect_rejection(std::move(us_buy), "invalid_quantity");

    auto us_limit = us_market_fractional_sell();
    us_limit.type_ = OrderType::LIMIT;
    us_limit.price_ = Decimal64::parse("400");
    expect_rejection(std::move(us_limit), "invalid_quantity");

    auto korean = korean_limit_buy();
    korean.side_ = OrderSide::SELL;
    korean.type_ = OrderType::MARKET;
    korean.price_.reset();
    korean.quantity_ = Decimal64::parse("0.5");
    expect_rejection(std::move(korean), "invalid_quantity");
  }

  SECTION("a US market sell is refused past six decimals and at zero") {
    // Seven places is refused even when the seventh is a zero, because `str()` would send it.
    for (const auto quantity :
         {Decimal64::parse("0.1234567"), Decimal64(1234560, 7), Decimal64::parse("0"),
          Decimal64::parse("-0.5")}) {
      auto order = us_market_fractional_sell();
      order.quantity_ = quantity;
      expect_rejection(std::move(order), "invalid_quantity");
    }
  }

  SECTION("an amendment quantity stays whole even for a US market order") {
    // A US amendment cannot change the quantity at all (`us-modify-quantity-not-supported`), and a
    // Korean one is whole shares, so the fractional rule never reaches the modify path.
    bool completed = false;
    tossinvest::TossAmendment amendment;
    amendment.order_id_ = "abc";
    amendment.type_ = OrderType::MARKET;
    amendment.quantity_ = Decimal64::parse("0.5");
    client->async_replace_order(7, std::move(amendment), [&](Result<OrderAck> result) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().broker_code_ == "invalid_quantity");
      completed = true;
    });
    ioc.restart();
    ioc.run();
    REQUIRE(completed);
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
  live.is_demo_ = false;
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

// The bodies below are constructed from KIS's documented reply envelope with the code and the message
// the demo server sent (2026-10-12 to 2026-10-14). No reply body was recorded.
namespace {

using KisOperation = kis::KisClientTestAccess::Operation;

constexpr const char* KIS_NOTHING_TO_CANCEL = "모의투자 정정/취소할 수량이 없습니다.";
constexpr const char* KIS_NOTHING_HELD = "모의투자 잔고내역이 없습니다.";
constexpr const char* KIS_MARKET_CLOSED = "모의투자 장종료 입니다.";
constexpr const char* KIS_MARKET_NOT_OPEN = "모의투자 장시작전 입니다.";

/** A KIS failure envelope with the given code and message, and `extra` appended inside the object. */
std::string kis_refusal_body(const std::string& code, const std::string& message, const std::string& extra = "") {
  return R"({"rt_cd":"1","msg_cd":")" + code + R"(","msg1":")" + message + "\"" + extra + "}";
}

/** A KIS real-server refusal body: the real server stated `APBK3061` with `rt_cd` `"7"`. */
std::string kis_real_refusal_body(const std::string& code, const std::string& message, const std::string& extra = "") {
  return R"({"rt_cd":"7","msg_cd":")" + code + R"(","msg1":")" + message + "\"" + extra + "}";
}

/** Whether an order reply is read as the recognized refusal `code`. */
bool kis_recognized(const Result<kis::KisOrderAck>& result, const std::string& code) {
  return !result && result.error().category_ == ErrorCategory::INVALID_ARGUMENT &&
    result.error().reached_broker_ && result.error().broker_code_ == code;
}

} // namespace

TEST_CASE("a KIS demo cash order refusal the client recognizes is a rejection", "[broker][kis][order]") {
  SECTION("a cancellation with nothing left to cancel") {
    const auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(kis_refusal_body("40330000", KIS_NOTHING_TO_CANCEL)), KisOperation::CANCEL, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "40330000");
    CHECK(
      result.error().message_ ==
      std::string(KIS_NOTHING_TO_CANCEL) + " (recognized refusal: KIS demo, cash cancellation, 40330000)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
  }

  SECTION("a placement with nothing held to sell") {
    const auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(kis_refusal_body("40240000", KIS_NOTHING_HELD)), KisOperation::PLACE, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "40240000");
    CHECK(
      result.error().message_ ==
      std::string(KIS_NOTHING_HELD) + " (recognized refusal: KIS demo, cash placement, 40240000)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
  }

  SECTION("a placement after the market has closed") {
    const auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(kis_refusal_body("40580000", KIS_MARKET_CLOSED)), KisOperation::PLACE, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "40580000");
    CHECK(
      result.error().message_ ==
      std::string(KIS_MARKET_CLOSED) + " (recognized refusal: KIS demo, cash placement, 40580000)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
  }

  SECTION("a placement before the market has opened") {
    const auto result = kis::KisClientTestAccess::parse_order_ack(
      json_response(kis_refusal_body("40570000", KIS_MARKET_NOT_OPEN)), KisOperation::PLACE, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "40570000");
    CHECK(
      result.error().message_ ==
      std::string(KIS_MARKET_NOT_OPEN) + " (recognized refusal: KIS demo, cash placement, 40570000)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);

    const auto unknown = [](const Result<kis::KisOrderAck>& reply) {
      return !reply && reply.error().category_ == ErrorCategory::PROTOCOL;
    };
    const std::string body = kis_refusal_body("40570000", KIS_MARKET_NOT_OPEN);
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::PLACE, false)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::CANCEL, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::AMEND, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::UNCLASSIFIED, true)));
    for (const char* code : {"40570001", "4057000", "405700000", " 40570000", "40570000 "}) {
      CAPTURE(code);
      CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(
        json_response(kis_refusal_body(code, KIS_MARKET_NOT_OPEN)), KisOperation::PLACE, true
      )));
    }
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(
      json_response(kis_refusal_body("40570000", KIS_MARKET_NOT_OPEN, R"(,"output":{"ODNO":"0001569157"})")),
      KisOperation::PLACE,
      true
    )));
  }

  SECTION("an empty acknowledgement beside it, or the original order echoed, changes nothing") {
    for (const char* extra : {
           R"(,"output":{})",
           R"(,"output":{"KRX_FWDG_ORD_ORGNO":"","ODNO":"","ORD_TMD":""})",
           R"(,"output":{"ORGN_ODNO":"0001569157"})",
           R"(,"msg2":"an unrelated field")",
         }) {
      CAPTURE(extra);
      CHECK(kis_recognized(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body("40330000", KIS_NOTHING_TO_CANCEL, extra)), KisOperation::CANCEL, true
        ),
        "40330000"
      ));
    }
  }
}

TEST_CASE("a KIS order refusal outside the recognized replies stays an unknown outcome", "[broker][kis][order]") {
  const auto unknown = [](const Result<kis::KisOrderAck>& result) {
    return !result && result.error().category_ == ErrorCategory::PROTOCOL &&
      runner::outcome_of(result.error()) == runner::SendOutcome::INDETERMINATE;
  };
  const std::string cancel = kis_refusal_body("40330000", KIS_NOTHING_TO_CANCEL);
  const std::string place = kis_refusal_body("40240000", KIS_NOTHING_HELD);
  const std::string closed = kis_refusal_body("40580000", KIS_MARKET_CLOSED);

  SECTION("the real server, another operation, or no operation") {
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(closed), KisOperation::PLACE, false)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(closed), KisOperation::CANCEL, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(closed), KisOperation::AMEND, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(closed), KisOperation::UNCLASSIFIED, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(closed))));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(cancel), KisOperation::CANCEL, false)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place), KisOperation::PLACE, false)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(cancel), KisOperation::PLACE, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(cancel), KisOperation::AMEND, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place), KisOperation::CANCEL, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place), KisOperation::AMEND, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(cancel), KisOperation::UNCLASSIFIED, true)));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(cancel))));

    // What the unrecognized reading carries: the broker's message alone, and no status.
    const auto live = kis::KisClientTestAccess::parse_order_ack(json_response(cancel), KisOperation::CANCEL, false);
    CHECK(live.error().message_ == KIS_NOTHING_TO_CANCEL);
    CHECK(live.error().http_status_ == 0);
    CHECK(live.error().broker_code_ == "40330000");
  }

  SECTION("a success status other than 200") {
    CHECK(unknown(
      kis::KisClientTestAccess::parse_order_ack(json_response(cancel, 201), KisOperation::CANCEL, true)
    ));
    CHECK(unknown(
      kis::KisClientTestAccess::parse_order_ack(json_response(cancel, 202), KisOperation::CANCEL, true)
    ));
  }

  SECTION("another code, or the code inside other text") {
    for (const char* code : {"40330001", "4033000", "403300000", " 40330000", "40330000 ", "APBK0919", ""}) {
      CAPTURE(code);
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body(code, KIS_NOTHING_TO_CANCEL)), KisOperation::CANCEL, true
        )
      ));
    }
    for (const char* code : {"40580001", "4058000", "405800000", " 40580000", "40580000 ", "40330000", ""}) {
      CAPTURE(code);
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body(code, KIS_MARKET_CLOSED)), KisOperation::PLACE, true
        )
      ));
    }
    // The code in the message, under another code.
    CHECK(unknown(
      kis::KisClientTestAccess::parse_order_ack(
        json_response(kis_refusal_body("APBK0919", "40330000")), KisOperation::CANCEL, true
      )
    ));
    // A code that only begins with it: compared as the whole JSON string.
    CHECK(unknown(
      kis::KisClientTestAccess::parse_order_ack(
        json_response(R"({"rt_cd":"1","msg_cd":"40330000\u0000X","msg1":"m"})"), KisOperation::CANCEL, true
      )
    ));
  }

  SECTION("a status, code or message of another value or type") {
    for (const char* body : {
           R"({"rt_cd":"7","msg_cd":"40330000","msg1":"m"})",
           R"({"rt_cd":" 1","msg_cd":"40330000","msg1":"m"})",
           R"({"rt_cd":"1","msg_cd":40330000,"msg1":"m"})",
           R"({"rt_cd":"1","msg_cd":"40330000"})",
           R"({"rt_cd":"1","msg_cd":"40330000","msg1":""})",
           R"({"rt_cd":"1","msg_cd":"40330000","msg1":null})",
           R"({"rt_cd":"1","msg_cd":"40330000","msg1":["m"]})",
         }) {
      CAPTURE(body);
      CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::CANCEL, true)));
    }
    // A status that is not a string is not a reply this parser reads at all.
    for (const char* body : {
           R"({"rt_cd":1,"msg_cd":"40330000","msg1":"m"})",
           R"({"msg_cd":"40330000","msg1":"m"})",
         }) {
      CAPTURE(body);
      const auto result =
        kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::CANCEL, true);
      REQUIRE_FALSE(result);
      CHECK(result.error().category_ == ErrorCategory::PROTOCOL);
      CHECK(result.error().broker_code_ == "invalid_order_response");
    }
  }

  SECTION("anything an acknowledgement carries, or an output that is not an object") {
    for (const char* extra : {
           R"(,"output":{"ODNO":"0001569157"})",
           R"(,"output":{"ODNO":1569157})",
           R"(,"output":{"ODNO":null})",
           R"(,"output":{"ODNO":" "})",
           R"(,"output":{"ODNO":"","ORD_TMD":"155211"})",
           R"(,"output":{"KRX_FWDG_ORD_ORGNO":"06010"})",
           R"(,"output":null)",
           R"(,"output":[])",
           R"(,"output":[{"ODNO":""}])",
           R"(,"output":"")",
           R"(,"output":0)",
         }) {
      CAPTURE(extra);
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body("40330000", KIS_NOTHING_TO_CANCEL, extra)), KisOperation::CANCEL, true
        )
      ));
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body("40240000", KIS_NOTHING_HELD, extra)), KisOperation::PLACE, true
        )
      ));
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_refusal_body("40580000", KIS_MARKET_CLOSED, extra)), KisOperation::PLACE, true
        )
      ));
    }
  }

  SECTION("an HTTP error with the same body keeps its own reading") {
    const auto server = kis::KisClientTestAccess::parse_order_ack(json_response(cancel, 500), KisOperation::CANCEL, true);
    REQUIRE_FALSE(server);
    CHECK(server.error().category_ == ErrorCategory::HTTP);
    CHECK(runner::outcome_of(server.error()) == runner::SendOutcome::INDETERMINATE);

    const auto client = kis::KisClientTestAccess::parse_order_ack(json_response(cancel, 400), KisOperation::CANCEL, true);
    REQUIRE_FALSE(client);
    CHECK(client.error().category_ == ErrorCategory::HTTP);
    CHECK(client.error().http_status_ == 400);
    CHECK(client.error().message_.find("recognized refusal") == std::string::npos);
  }

  SECTION("a rate limit and an accepted order are read as before") {
    const auto limited = kis::KisClientTestAccess::parse_order_ack(
      json_response(R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})"),
      KisOperation::CANCEL,
      true
    );
    REQUIRE_FALSE(limited);
    CHECK(limited.error().category_ == ErrorCategory::RATE_LIMIT);

    const auto accepted = kis::KisClientTestAccess::parse_order_ack(
      json_response(
        R"({"rt_cd":"0","msg_cd":"40330000","msg1":"m",)"
        R"("output":{"KRX_FWDG_ORD_ORGNO":"06010","ODNO":"0001569157","ORD_TMD":"155211"}})"
      ),
      KisOperation::CANCEL,
      true
    );
    REQUIRE(accepted);
    CHECK(accepted.value().order_.order_number_ == "0001569157");
  }
}

// Through the client's own request and reply path, against a loopback server that answers the token
// request and then the order request with a constructed reply: the operation and the environment the
// parser is given are the ones the public call fixes, not a test's argument.
namespace {

constexpr const char* KIS_ORDER_CASH = "/uapi/domestic-stock/v1/trading/order-cash";
constexpr const char* KIS_ORDER_AMEND = "/uapi/domestic-stock/v1/trading/order-rvsecncl";
constexpr const char* KIS_AFTER_MARKET_ONLY = "[애프터마켓] 애프터 지정가/최유리/최우선 주문만 가능합니다.";

/** A KIS client with the given environment, its requests sent to `server`. */
std::shared_ptr<kis::KisClient> scripted_kis(
  net::io_context& ioc, ssl::context& context, const test::LocalHttpsServer& server, bool demo
) {
  kis::KisConfig config;
  config.is_demo_ = demo;
  auto client = std::make_shared<kis::KisClient>(ioc.get_executor(), context, std::move(config));
  kis::KisClientTestAccess::connect(*client, context, server.port());
  return client;
}

/** A limit order of one share on KRX. */
OrderRequest kis_scripted_order(OrderSide side) {
  OrderRequest order;
  order.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  order.side_ = side;
  order.type_ = OrderType::LIMIT;
  order.time_in_force_ = TimeInForce::DAY;
  order.venue_ = Venue::KRX;
  order.quantity_ = Decimal64::parse("1");
  order.price_ = Decimal64::parse("70000");
  return order;
}

/** An amendment of a working order to one share at 71,000 KRW. */
kis::KisAmendment kis_scripted_amendment() {
  kis::KisAmendment amendment;
  amendment.order_ = kis::KisOrderRef{"00950", "0000117057"};
  amendment.type_ = OrderType::LIMIT;
  amendment.time_in_force_ = TimeInForce::DAY;
  amendment.session_ = OrderSession::REGULAR;
  amendment.venue_ = Venue::KRX;
  amendment.quantity_ = Decimal64::parse("1");
  amendment.price_ = Decimal64::parse("71000");
  return amendment;
}

enum class KisCall { PLACE_BUY, PLACE_SELL, CANCEL, AMEND };

/** Makes one public order call with `reply` scripted on its endpoint, and returns what the client returned. */
Result<kis::KisOrderAck> kis_call(bool demo, KisCall call, const std::string& reply, test::LocalHttpsServer& server) {
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  server.script(
    "/oauth2/tokenP",
    200,
    R"({"access_token":"test-token","token_type":"Bearer","expires_in":86400,)"
    R"("access_token_token_expired":"2099-12-31 23:59:59"})"
  );
  server.script(call == KisCall::PLACE_BUY || call == KisCall::PLACE_SELL ? KIS_ORDER_CASH : KIS_ORDER_AMEND, 200, reply);
  auto client = scripted_kis(ioc, context, server, demo);
  const kis::KisAccount account{"12345678", "01"};
  std::future<Result<kis::KisOrderAck>> result;
  switch (call) {
    case KisCall::PLACE_BUY:
      result = client->async_place_order(account, kis_scripted_order(OrderSide::BUY), net::use_future);
      break;
    case KisCall::PLACE_SELL:
      result = client->async_place_order(account, kis_scripted_order(OrderSide::SELL), net::use_future);
      break;
    case KisCall::CANCEL:
      result = client->async_cancel_order(account, kis_scripted_amendment(), net::use_future);
      break;
    case KisCall::AMEND:
      result = client->async_replace_order(account, kis_scripted_amendment(), net::use_future);
      break;
  }
  ioc.run();
  return result.get();
}

} // namespace

TEST_CASE("a KIS order reply is read with the operation and environment of the call that sent it", "[broker][kis][order]") {
  test::LocalHttpsServer server;
  const std::string after_market = kis_real_refusal_body("APBK3061", KIS_AFTER_MARKET_ONLY);
  const std::string nothing_held = kis_refusal_body("40240000", KIS_NOTHING_HELD);
  const std::string nothing_to_cancel = kis_refusal_body("40330000", KIS_NOTHING_TO_CANCEL);

  SECTION("the real server's after-market refusal of a placement, buy or sell") {
    for (const auto call : {KisCall::PLACE_BUY, KisCall::PLACE_SELL}) {
      const auto result = kis_call(false, call, after_market, server);
      REQUIRE_FALSE(result);
      CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
      CHECK(result.error().reached_broker_);
      CHECK(result.error().http_status_ == 200);
      CHECK(result.error().broker_code_ == "APBK3061");
      CHECK(
        result.error().message_ ==
        std::string(KIS_AFTER_MARKET_ONLY) + " (recognized refusal: KIS real, cash placement, APBK3061)"
      );
      CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
      CHECK(server.lastHeader(KIS_ORDER_CASH, "tr_id") == (call == KisCall::PLACE_BUY ? "TTTC0012U" : "TTTC0011U"));
    }
  }

  SECTION("the same reply from the demo server, or on a cancellation or an amendment, stays unknown") {
    for (const auto call : {KisCall::PLACE_BUY, KisCall::PLACE_SELL}) {
      const auto demo = kis_call(true, call, after_market, server);
      REQUIRE_FALSE(demo);
      CHECK(demo.error().category_ == ErrorCategory::PROTOCOL);
      CHECK(server.lastHeader(KIS_ORDER_CASH, "tr_id") == (call == KisCall::PLACE_BUY ? "VTTC0012U" : "VTTC0011U"));
    }
    for (const auto call : {KisCall::CANCEL, KisCall::AMEND}) {
      const auto amended = kis_call(false, call, after_market, server);
      REQUIRE_FALSE(amended);
      CHECK(amended.error().category_ == ErrorCategory::PROTOCOL);
      CHECK(runner::outcome_of(amended.error()) == runner::SendOutcome::INDETERMINATE);
    }
  }

  SECTION("a demo server's refusal is recognized from the demo server only") {
    const auto demo_sell = kis_call(true, KisCall::PLACE_SELL, nothing_held, server);
    REQUIRE_FALSE(demo_sell);
    CHECK(demo_sell.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(demo_sell.error().message_.find("(recognized refusal: KIS demo, cash placement, 40240000)") != std::string::npos);
    const auto real_sell = kis_call(false, KisCall::PLACE_SELL, nothing_held, server);
    REQUIRE_FALSE(real_sell);
    CHECK(real_sell.error().category_ == ErrorCategory::PROTOCOL);

    const auto demo_cancel = kis_call(true, KisCall::CANCEL, nothing_to_cancel, server);
    REQUIRE_FALSE(demo_cancel);
    CHECK(demo_cancel.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(demo_cancel.error().message_.find("(recognized refusal: KIS demo, cash cancellation, 40330000)") != std::string::npos);
    CHECK(server.lastHeader(KIS_ORDER_AMEND, "tr_id") == "VTTC0013U");
    CHECK(json::parse(server.lastBody(KIS_ORDER_AMEND)).as_object().at("RVSE_CNCL_DVSN_CD").as_string() == "02");

    // The amendment shares the endpoint and the transaction id; only the cancellation is recognized.
    const auto demo_amend = kis_call(true, KisCall::AMEND, nothing_to_cancel, server);
    REQUIRE_FALSE(demo_amend);
    CHECK(demo_amend.error().category_ == ErrorCategory::PROTOCOL);
    CHECK(json::parse(server.lastBody(KIS_ORDER_AMEND)).as_object().at("RVSE_CNCL_DVSN_CD").as_string() == "01");

    const auto real_cancel = kis_call(false, KisCall::CANCEL, nothing_to_cancel, server);
    REQUIRE_FALSE(real_cancel);
    CHECK(real_cancel.error().category_ == ErrorCategory::PROTOCOL);
  }
}

TEST_CASE("a KIS real-server refusal is recognized under the same conditions as a demo one", "[broker][kis][order]") {
  const auto unknown = [](const Result<kis::KisOrderAck>& result) {
    return !result && result.error().category_ == ErrorCategory::PROTOCOL &&
      runner::outcome_of(result.error()) == runner::SendOutcome::INDETERMINATE;
  };
  const std::string place = kis_real_refusal_body("APBK3061", KIS_AFTER_MARKET_ONLY);

  SECTION("the row, with the empty acknowledgement a refusal may carry") {
    const auto result = kis::KisClientTestAccess::parse_order_ack(json_response(place), KisOperation::PLACE, false);
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "APBK3061");
    CHECK(
      result.error().message_ ==
      std::string(KIS_AFTER_MARKET_ONLY) + " (recognized refusal: KIS real, cash placement, APBK3061)"
    );
    for (const char* extra : {
           R"(,"output":{})",
           R"(,"output":{"KRX_FWDG_ORD_ORGNO":"","ODNO":"","ORD_TMD":""})",
           R"(,"msg2":"an unrelated field")",
         }) {
      CAPTURE(extra);
      CHECK(kis_recognized(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_real_refusal_body("APBK3061", KIS_AFTER_MARKET_ONLY, extra)), KisOperation::PLACE, false
        ),
        "APBK3061"
      ));
    }
  }

  SECTION("another operation, the demo server, another status, or another code") {
    for (const auto operation : {KisOperation::CANCEL, KisOperation::AMEND, KisOperation::UNCLASSIFIED}) {
      CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place), operation, false)));
    }
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place))));
    CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place), KisOperation::PLACE, true)));
    for (const int status : {201, 202}) {
      CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(place, status), KisOperation::PLACE, false)));
    }
    for (const char* code : {"APBK3062", "APBK306", "APBK30610", " APBK3061", "APBK3061 ", "apbk3061", "40580000", ""}) {
      CAPTURE(code);
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_real_refusal_body(code, KIS_AFTER_MARKET_ONLY)), KisOperation::PLACE, false
        )
      ));
    }
    // The code in the message, under another code.
    CHECK(unknown(
      kis::KisClientTestAccess::parse_order_ack(
        json_response(kis_real_refusal_body("APBK0919", "APBK3061")), KisOperation::PLACE, false
      )
    ));
  }

  SECTION("a status, code or message of another value or type") {
    // A success status, or one that is not a string, is not a reply the refusal rule reads.
    for (const char* body : {
           R"({"rt_cd":"0","msg_cd":"APBK3061","msg1":"m"})",
           R"({"rt_cd":1,"msg_cd":"APBK3061","msg1":"m"})",
           R"({"msg_cd":"APBK3061","msg1":"m"})",
         }) {
      CAPTURE(body);
      const auto result = kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::PLACE, false);
      REQUIRE_FALSE(result);
      CHECK(result.error().category_ == ErrorCategory::PROTOCOL);
      CHECK(result.error().broker_code_ == "invalid_order_response");
    }
    // The demo server's refusal status, `"1"`, is not the status the real server stated this code with.
    for (const char* body : {
           R"({"rt_cd":"1","msg_cd":"APBK3061","msg1":"m"})",
           R"({"rt_cd":" 7","msg_cd":"APBK3061","msg1":"m"})",
           R"({"rt_cd":"7 ","msg_cd":"APBK3061","msg1":"m"})",
           R"({"rt_cd":"7","msg_cd":["APBK3061"],"msg1":"m"})",
           R"({"rt_cd":"7","msg_cd":"APBK3061"})",
           R"({"rt_cd":"7","msg_cd":"APBK3061","msg1":""})",
           R"({"rt_cd":"7","msg_cd":"APBK3061","msg1":null})",
         }) {
      CAPTURE(body);
      CHECK(unknown(kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::PLACE, false)));
    }
  }

  SECTION("anything an acknowledgement carries, or an output that is not an object") {
    for (const char* extra : {
           R"(,"output":{"ODNO":"0001569157"})",
           R"(,"output":{"ODNO":null})",
           R"(,"output":{"ODNO":"","ORD_TMD":"194112"})",
           R"(,"output":{"KRX_FWDG_ORD_ORGNO":"06010"})",
           R"(,"output":null)",
           R"(,"output":[])",
           R"(,"output":"")",
         }) {
      CAPTURE(extra);
      CHECK(unknown(
        kis::KisClientTestAccess::parse_order_ack(
          json_response(kis_real_refusal_body("APBK3061", KIS_AFTER_MARKET_ONLY, extra)), KisOperation::PLACE, false
        )
      ));
    }
  }

  SECTION("an HTTP error with the same body keeps its own reading") {
    for (const int status : {400, 500}) {
      const auto result = kis::KisClientTestAccess::parse_order_ack(json_response(place, status), KisOperation::PLACE, false);
      REQUIRE_FALSE(result);
      CHECK(result.error().category_ == ErrorCategory::HTTP);
      CHECK(result.error().message_.find("recognized refusal") == std::string::npos);
    }
  }
}

TEST_CASE("an HTTP error carries at most the start of its reply body", "[broker]") {
  const auto limit = broker_detail::reply_excerpt_limit;
  REQUIRE(limit == 512);

  // A broker's error envelope fits and is carried whole.
  const std::string envelope = R"({"rt_cd":"1","msg_cd":"EGW00201","msg1":"초당 거래건수를 초과하였습니다."})";
  const auto small = json_response(envelope, 500);
  REQUIRE(small);
  CHECK(broker_detail::http_error(small.value()).message_ == envelope);

  // A long body is cut at the limit and says how much was left out.
  const auto long_reply = json_response(std::string(limit + 100, 'x'), 502);
  REQUIRE(long_reply);
  const auto cut = broker_detail::http_error(long_reply.value());
  CHECK(cut.http_status_ == 502);
  CHECK(cut.message_ == std::string(limit, 'x') + "... (100 more bytes)");

  // Exactly at the limit is whole.
  CHECK(broker_detail::reply_excerpt(std::string(limit, 'y')) == std::string(limit, 'y'));

  // A three-byte Korean character straddling the limit is left out whole rather than split.
  const std::string korean = std::string(limit - 1, 'z') + "초과";  // 511 + 3 + 3 bytes
  const auto excerpt = broker_detail::reply_excerpt(korean);
  CHECK(excerpt == std::string(limit - 1, 'z') + "... (6 more bytes)");

  // A four-byte character ending one byte past the limit is left out whole, three bytes back.
  const std::string emoji = std::string(limit - 3, 'e') + "\xF0\x9F\x98\x80" + "tail";
  CHECK(broker_detail::reply_excerpt(emoji) == std::string(limit - 3, 'e') + "... (8 more bytes)");

  // A body that is not UTF-8 keeps its prefix: the cut moves back at most three bytes.
  const std::string not_utf8 = "A" + std::string(600, '\x80');
  CHECK(broker_detail::reply_excerpt(not_utf8) == "A" + std::string(limit - 4, '\x80') + "... (92 more bytes)");
}

TEST_CASE("a reply whose body is not a JSON object keeps its HTTP status", "[broker][order]") {
  const auto unreadable = [](const auto& result, int status) {
    return !result && result.error().category_ == ErrorCategory::PROTOCOL && result.error().reached_broker_ &&
      result.error().broker_code_ == "invalid_json" && result.error().http_status_ == status &&
      runner::outcome_of(result.error()) == runner::SendOutcome::INDETERMINATE;
  };

  SECTION("at Toss, which reads the body before the status") {
    for (const int status : {200, 400, 404, 429, 500}) {
      CAPTURE(status);
      for (const char* body : {"", "<html>Bad Request</html>", "[]", "\"text\""}) {
        CAPTURE(body);
        CHECK(unreadable(tossinvest::TossClientTestAccess::parse_order_ack(json_response(body, status)), status));
        CHECK(unreadable(tossinvest::TossClientTestAccess::parse_buying_power(json_response(body, status)), status));
      }
    }
    // The message is the parser's, never the body.
    const auto html = tossinvest::TossClientTestAccess::parse_order_ack(json_response("<html>secret</html>", 400));
    REQUIRE_FALSE(html);
    CHECK(html.error().message_.find("secret") == std::string::npos);
  }

  SECTION("at KIS, which reads the status first") {
    for (const char* body : {"", "<html></html>", "[]"}) {
      CAPTURE(body);
      CHECK(unreadable(
        kis::KisClientTestAccess::parse_order_ack(json_response(body), KisOperation::PLACE, false), 200
      ));
    }
    // A non-2xx reply is still an HTTP error, whatever its body.
    const auto server = kis::KisClientTestAccess::parse_order_ack(json_response("<html></html>", 500));
    REQUIRE_FALSE(server);
    CHECK(server.error().category_ == ErrorCategory::HTTP);
    CHECK(server.error().http_status_ == 500);
  }
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

TEST_CASE("a KIS minute page repeating the previous page's oldest bar adds it once", "[broker][kis][bars]") {
  // Observed on 2026-10-04: a read before 2026-09-01 13:22 KST returned the bar at each page join
  // twice, identical. The runner's bar cache throws on a repeated timestamp, so the series must not carry one.
  BarQuery query;
  query.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  query.interval_ = BarInterval::ONE_MINUTE;
  const auto row = [](const char* time, const char* close) {
    return std::string(R"({"stck_bsop_date":"20260901","stck_cntg_hour":")") + time +
      R"(","stck_prpr":")" + close + R"(","stck_oprc":")" + close + R"(","stck_hgpr":")" + close +
      R"(","stck_lwpr":")" + close + R"(","cntg_vol":"100"})";
  };
  const auto page = [&](std::initializer_list<std::string> rows) {
    std::string body = R"({"rt_cd":"0","output2":[)";
    bool first = true;
    for (const auto& r : rows) {
      body += (first ? "" : ",") + r;
      first = false;
    }
    return json_response(body + "]}");
  };
  std::vector<Bar> descending;
  REQUIRE_FALSE(kis::KisClientTestAccess::parse_bars_page(
    page({row("112500", "254500"), row("112400", "254400"), row("112300", "254300")}), query, true, descending
  ));
  REQUIRE(descending.size() == 3);

  // The next page starts with the bar the last one ended on.
  REQUIRE_FALSE(kis::KisClientTestAccess::parse_bars_page(
    page({row("112300", "254300"), row("112200", "254200"), row("112100", "254100")}), query, true, descending
  ));
  REQUIRE(descending.size() == 5);
  BarSeries series;
  series.bars_.assign(descending.rbegin(), descending.rend());
  CHECK(series.ascending());
  CHECK(series.bars_[2].close_ == Decimal64::parse("254300"));

  // A page with nothing older adds nothing, which the request reads as an empty page and ends on.
  REQUIRE_FALSE(kis::KisClientTestAccess::parse_bars_page(
    page({row("112200", "254200"), row("112100", "254100")}), query, true, descending
  ));
  CHECK(descending.size() == 5);
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

  SECTION("a Toss day whose date cannot be read produces no row and is counted") {
    // This used to fail the whole response, which cost `today` whenever a neighbour was damaged.
    auto alone = tossinvest::TossClientTestAccess::parse_calendar(
      Country::KR, json_response(R"({"result":{"today":{"date":"2026-13-45"}}})")
    );
    REQUIRE(alone);
    CHECK(alone.value().days_.empty());
    CHECK(alone.value().unreadable_days_ == 1);

    auto neighbour = tossinvest::TossClientTestAccess::parse_calendar(
      Country::KR,
      json_response(
        R"({"result":{"previousBusinessDay":{"date":"2026-03-20"},)"
        R"("today":{"date":"2026-03-23","integrated":{"open":"09:00","close":"15:30"}},)"
        R"("nextBusinessDay":{"date":"2026-03-2x"}}})"
      )
    );
    REQUIRE(neighbour);
    CHECK(neighbour.value().is_trading_day(Date{2026, 3, 23}) == true);
    CHECK(neighbour.value().days_.size() == 2);
    CHECK(neighbour.value().unreadable_days_ == 1);
  }
}

TEST_CASE("a KIS calendar page that fails leaves the calendar as it was", "[core][calendar][kis]") {
  TradingCalendar calendar;
  calendar.days_.push_back(TradingDay{Date{2026, 8, 31}, true});
  std::string search;
  std::string next;
  // Good rows first, then one that is not an object: the page fails, and nothing it held is kept.
  const auto failure = kis::KisClientTestAccess::parse_calendar_page(
    json_response(
      R"({"rt_cd":"0","output":[{"bass_dt":"20260901","opnd_yn":"Y"},{"bass_dt":"2026090x"},7]})"
    ),
    calendar, search, next
  );
  REQUIRE(failure);
  CHECK(failure->broker_code_ == "invalid_calendar_response");
  CHECK(calendar.days_.size() == 1);
  CHECK(calendar.unreadable_days_ == 0);
}

TEST_CASE("the KIS calendar cache covers a day only where it holds that day's row", "[core][calendar][kis]") {
  TradingCalendar calendar;
  calendar.days_.push_back(TradingDay{Date{2026, 9, 1}, true});
  calendar.days_.push_back(TradingDay{Date{2026, 9, 3}, true});
  calendar.unreadable_days_ = 1;
  // 2026-09-02 lies inside the span and has no row, because its row was unreadable. Serving the cache
  // for it would answer "not covered" at every poll without asking again, so a repaired response
  // could never restore the day: the cache must not claim it.
  CHECK_FALSE(kis::KisClientTestAccess::calendar_covers(calendar, Date{2026, 9, 2}));
  CHECK(kis::KisClientTestAccess::calendar_covers(calendar, Date{2026, 9, 1}));
  CHECK(kis::KisClientTestAccess::calendar_covers(calendar, Date{2026, 9, 3}));
  CHECK_FALSE(kis::KisClientTestAccess::calendar_covers(calendar, Date{2026, 9, 4}));
}

TEST_CASE("a KIS page of only unreadable rows is progress, not the end of the listing", "[core][calendar][kis]") {
  // Pagination stops on a page that consumed nothing. A page whose every date was unreadable keeps
  // no day and must still count, or the pages after it -- today's row, possibly -- are never asked for.
  TradingCalendar calendar;
  calendar.days_.push_back(TradingDay{Date{2026, 9, 1}, true});
  const auto before = kis::KisClientTestAccess::calendar_rows_consumed(calendar);
  std::string search;
  std::string next;
  const auto failure = kis::KisClientTestAccess::parse_calendar_page(
    json_response(R"({"rt_cd":"0","output":[{"bass_dt":"2026090x"},{"bass_dt":""}]})"),
    calendar, search, next
  );
  REQUIRE_FALSE(failure);
  CHECK(calendar.days_.size() == 1);
  CHECK(kis::KisClientTestAccess::calendar_rows_consumed(calendar) == before + 2);
}

// Through the client's own request path: the loopback server answers the token request and then each
// calendar page in turn, so the wiring around the two helpers above is exercised, not only the helpers.
namespace {

constexpr const char* KIS_CALENDAR = "/uapi/domestic-stock/v1/quotations/chk-holiday";

/** A calendar page with the given cursors, continuation and rows. */
test::LocalHttpsServer::Scripted kis_calendar_page(const std::string& cursor, bool more, const std::string& rows) {
  return test::LocalHttpsServer::Scripted{
    200,
    R"({"rt_cd":"0","msg_cd":"MCA00000","msg1":"ok","ctx_area_fk":")" + cursor + R"(","ctx_area_nk":")" + cursor +
      R"(","output":[)" + rows + "]}",
    {{"tr_cont", more ? "M" : "D"}},
  };
}

/** Asks `client` for the calendar from `from`, without a refresh, and runs the exchange to completion. */
Result<TradingCalendar> kis_calendar_from(net::io_context& ioc, kis::KisClient& client, Date from) {
  auto result = client.async_calendar(from, false, net::use_future);
  ioc.restart();
  ioc.run();
  return result.get();
}

} // namespace

TEST_CASE("a KIS calendar request reads on past a page of only unreadable rows", "[core][calendar][kis]") {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  server.script(
    "/oauth2/tokenP",
    200,
    R"({"access_token":"test-token","token_type":"Bearer","expires_in":86400,)"
    R"("access_token_token_expired":"2099-12-31 23:59:59"})"
  );
  server.scriptSequence(
    KIS_CALENDAR,
    {
      kis_calendar_page("P1", true, R"({"bass_dt":"2026090x"},{"bass_dt":""})"),
      kis_calendar_page("P2", false, R"({"bass_dt":"20260901","opnd_yn":"Y"},{"bass_dt":"20260902","opnd_yn":"N"})"),
    }
  );
  auto client = scripted_kis(ioc, context, server, false);

  const auto calendar = kis_calendar_from(ioc, *client, Date{2026, 9, 1});
  REQUIRE(calendar);
  // The first page kept no day; the second was still asked for, as a continuation.
  CHECK(server.scriptedCount(KIS_CALENDAR) == 2);
  CHECK(server.lastHeader(KIS_CALENDAR, "tr_cont") == "N");
  REQUIRE(calendar.value().days_.size() == 2);
  CHECK(calendar.value().unreadable_days_ == 2);
  CHECK(calendar.value().is_trading_day(Date{2026, 9, 1}) == std::optional<bool>(true));
  CHECK(calendar.value().is_trading_day(Date{2026, 9, 2}) == std::optional<bool>(false));
}

TEST_CASE("a KIS calendar day dropped as unreadable is asked for again but a cached day is not", "[core][calendar][kis]") {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  server.script(
    "/oauth2/tokenP",
    200,
    R"({"access_token":"test-token","token_type":"Bearer","expires_in":86400,)"
    R"("access_token_token_expired":"2099-12-31 23:59:59"})"
  );
  server.scriptSequence(
    KIS_CALENDAR,
    {
      kis_calendar_page(
        "C1", false, R"({"bass_dt":"20260901","opnd_yn":"Y"},{"bass_dt":"2026090x"},{"bass_dt":"20260903","opnd_yn":"Y"})"
      ),
      kis_calendar_page(
        "C2",
        false,
        R"({"bass_dt":"20260901","opnd_yn":"Y"},{"bass_dt":"20260902","opnd_yn":"Y"},{"bass_dt":"20260903","opnd_yn":"Y"})"
      ),
    }
  );
  auto client = scripted_kis(ioc, context, server, false);

  const auto first = kis_calendar_from(ioc, *client, Date{2026, 9, 1});
  REQUIRE(first);
  CHECK(server.scriptedCount(KIS_CALENDAR) == 1);
  CHECK_FALSE(first.value().is_trading_day(Date{2026, 9, 2}).has_value());

  // A day the cache holds is served without a request.
  const auto cached = kis_calendar_from(ioc, *client, Date{2026, 9, 3});
  REQUIRE(cached);
  CHECK(server.scriptedCount(KIS_CALENDAR) == 1);

  // The day whose row was dropped lies inside the cached span, and is asked for again.
  const auto repaired = kis_calendar_from(ioc, *client, Date{2026, 9, 2});
  REQUIRE(repaired);
  CHECK(server.scriptedCount(KIS_CALENDAR) == 2);
  CHECK(repaired.value().is_trading_day(Date{2026, 9, 2}) == std::optional<bool>(true));

  // And now the repaired day is cached.
  const auto again = kis_calendar_from(ioc, *client, Date{2026, 9, 2});
  REQUIRE(again);
  CHECK(server.scriptedCount(KIS_CALENDAR) == 2);
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
    REQUIRE(instrument.listed_on_ == StatedDate::stated(Date{1996, 12, 26}));
    REQUIRE(instrument.delisted_on_.state() == StatedDate::State::NOT_STATED);
    REQUIRE(*instrument.shares_outstanding_ == Decimal64::parse("728002365"));

    // The grid is the library's own (the adapter chooses it from the market, not from a reply), and
    // this is a cash equity so the equity ladder applies.
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
    REQUIRE(result.value().listed_on_ == StatedDate::stated(Date{1996, 12, 26}));
    REQUIRE(result.value().delisted_on_.state() == StatedDate::State::NOT_STATED);

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
    REQUIRE(result.value().listed_on_ == StatedDate::stated(Date{2017, 1, 1}));
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
  "Toss holds a stated commission bound apart from an absent one",
  "[core][capacity][toss]"
) {
  const auto parse = [](std::string_view start, std::string_view end) {
    return tossinvest::TossClientTestAccess::parse_commissions(
      json_response(
        std::string(R"({"result":[{"marketCountry":"KR","commissionRate":"0.00015",)") +
        R"("startDate":)" + std::string(start) + R"(,"endDate":)" + std::string(end) + "}]}"
      )
    );
  };

  SECTION("null is the broker's own spelling for open-ended, and is not damage") {
    const auto schedule = parse("null", "null");
    REQUIRE(schedule);
    REQUIRE(schedule.value().rates_.size() == 1);
    const auto& rate = schedule.value().rates_[0];
    CHECK_FALSE(rate.from_);
    CHECK_FALSE(rate.from_unreadable_);
    CHECK_FALSE(rate.until_unreadable_);
    CHECK(schedule.value().unreadable_bounds_ == 0);
    // Reading `null` as damage would refuse every open-ended rate Toss publishes, which is most of
    // them.
    CHECK(rate.applies_on(Date{2026, 9, 23}) == RateApplicability::YES);
  }

  SECTION("an impossible day is stated and unreadable, not absent") {
    const auto schedule = parse("null", R"("2026-13-01")");
    REQUIRE(schedule);
    const auto& rate = schedule.value().rates_[0];
    CHECK_FALSE(rate.until_);
    CHECK(rate.until_unreadable_);
    CHECK(schedule.value().unreadable_bounds_ == 1);
    // The row survives -- its rate is intact and one bad bound must not lose a schedule's other rows
    // -- and it no longer answers as though the broker had said "no scheduled change".
    CHECK(rate.applies_on(Date{2026, 9, 23}) == RateApplicability::UNREADABLE);
  }

  SECTION("a numeric prefix is unreadable rather than a different year") {
    // `std::stoi` took the prefix, so this was the year 20: a valid, plausible date that is not the
    // one the broker sent, arriving downstream looking like an ordinary answer.
    const auto schedule = parse(R"("20x6-01-01")", "null");
    REQUIRE(schedule);
    const auto& rate = schedule.value().rates_[0];
    CHECK_FALSE(rate.from_);
    CHECK(rate.from_unreadable_);
    CHECK(rate.applies_on(Date{2026, 9, 23}) == RateApplicability::UNREADABLE);
  }

  SECTION("a field with no convertible number at all is unreadable, not a failed response") {
    // This threw out of `std::stoi` and failed the whole schedule. One unreadable bound is now the
    // same answer as any other unreadable bound.
    const auto schedule = parse(R"("x026-01-01")", "null");
    REQUIRE(schedule);
    CHECK(schedule.value().rates_[0].from_unreadable_);
  }

  SECTION("a bound that is not a string is stated in a form nothing here can read") {
    const auto schedule = parse("20260101", "null");
    REQUIRE(schedule);
    CHECK(schedule.value().rates_[0].from_unreadable_);
    CHECK(schedule.value().unreadable_bounds_ == 1);
  }

  SECTION("both bounds damaged counts the row once") {
    const auto schedule = parse(R"("2026-00-01")", R"("2026-02-30")");
    REQUIRE(schedule);
    CHECK(schedule.value().rates_[0].from_unreadable_);
    CHECK(schedule.value().rates_[0].until_unreadable_);
    // The count is per row because the caller's response -- refetch, or state the rates by hand --
    // is one decision whichever end of the window was lost.
    CHECK(schedule.value().unreadable_bounds_ == 1);
  }
}

namespace stated_date_break {

template <typename T, typename = void>
struct testable_as_bool : std::false_type {};
template <typename T>
struct testable_as_bool<T, std::void_t<decltype(static_cast<bool>(std::declval<const T&>()))>>
: std::true_type {};

template <typename T, typename = void>
struct dereferenceable : std::false_type {};
template <typename T>
struct dereferenceable<T, std::void_t<decltype(*std::declval<const T&>())>> : std::true_type {};

template <typename T, typename = void>
struct comparable_with_date : std::false_type {};
template <typename T>
struct comparable_with_date<T, std::void_t<decltype(std::declval<const T&>() == std::declval<Date>())>>
: std::true_type {};

template <typename T, typename = void>
struct orderable_with_date : std::false_type {};
template <typename T>
struct orderable_with_date<T, std::void_t<decltype(std::declval<const T&>() < std::declval<Date>())>>
: std::true_type {};

template <typename T, typename = void>
struct has_value_member : std::false_type {};
template <typename T>
struct has_value_member<T, std::void_t<decltype(std::declval<const T&>().has_value())>>
: std::true_type {};

template <typename T, typename = void>
struct exposes_date_member : std::false_type {};
template <typename T>
struct exposes_date_member<T, std::void_t<decltype(std::declval<const T&>().date_)>>
: std::true_type {};

} // namespace stated_date_break

TEST_CASE("an instrument date cannot be read the way an optional was", "[core][reference]") {
  // Every one of these is a spelling of the old reading, under which an unreadable delisting date
  // read as "still listed". Pinned so a convenience operator added later is a failing test, not a
  // quiet return of the defect.
  using stated_date_break::comparable_with_date;
  using stated_date_break::dereferenceable;
  using stated_date_break::exposes_date_member;
  using stated_date_break::has_value_member;
  using stated_date_break::orderable_with_date;
  using stated_date_break::testable_as_bool;
  STATIC_REQUIRE_FALSE(testable_as_bool<StatedDate>::value);
  STATIC_REQUIRE_FALSE(dereferenceable<StatedDate>::value);
  STATIC_REQUIRE_FALSE(comparable_with_date<StatedDate>::value);
  STATIC_REQUIRE_FALSE(orderable_with_date<StatedDate>::value);
  STATIC_REQUIRE_FALSE(has_value_member<StatedDate>::value);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<std::optional<Date>, StatedDate>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<Date, StatedDate>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<StatedDate, std::optional<Date>>);
  // And the payload is not reachable without naming the state: an unread `Date` is 0000-00-00,
  // which orders before every real date.
  STATIC_REQUIRE_FALSE(exposes_date_member<StatedDate>::value);
  CHECK_THROWS_AS(StatedDate{}.date(), std::logic_error);
  CHECK_THROWS_AS(StatedDate::unreadable().date(), std::logic_error);
  CHECK(StatedDate::stated(Date{2020, 1, 2}).date() == Date{2020, 1, 2});
}

TEST_CASE("an instrument's dates rule a day out only from a date that was read", "[core][reference]") {
  const Date listed{2010, 5, 3};
  const Date delisted{2020, 7, 1};
  const auto with = [](StatedDate list, StatedDate delist) {
    Instrument instrument;
    instrument.listed_on_ = list;
    instrument.delisted_on_ = delist;
    return instrument;
  };
  const auto stated = [](Date date) { return StatedDate::stated(date); };
  const auto unreadable = StatedDate::unreadable();
  const StatedDate absent;

  // The boundaries, which a test of days well either side would not pin: the listing day itself is
  // not ruled out, and the delisting day is.
  CHECK(with(stated(listed), absent).dates_exclude(listed) == DateExclusion::NOT_EXCLUDED);
  CHECK(with(stated(listed), absent).dates_exclude(Date{2010, 5, 2}) == DateExclusion::EXCLUDED);
  CHECK(with(absent, stated(delisted)).dates_exclude(delisted) == DateExclusion::EXCLUDED);
  CHECK(with(absent, stated(delisted)).dates_exclude(Date{2020, 6, 30}) == DateExclusion::NOT_EXCLUDED);
  CHECK(with(stated(listed), stated(delisted)).dates_exclude(Date{2015, 1, 1}) == DateExclusion::NOT_EXCLUDED);

  // A date that was read and rules the day out decides it, whatever the other date says.
  CHECK(with(stated(listed), unreadable).dates_exclude(Date{2009, 1, 1}) == DateExclusion::EXCLUDED);
  CHECK(with(unreadable, stated(delisted)).dates_exclude(Date{2021, 1, 1}) == DateExclusion::EXCLUDED);

  // Otherwise an unreadable date might rule it out, and the answer says so.
  CHECK(with(stated(listed), unreadable).dates_exclude(Date{2015, 1, 1}) == DateExclusion::UNREADABLE);
  CHECK(with(unreadable, stated(delisted)).dates_exclude(Date{2015, 1, 1}) == DateExclusion::UNREADABLE);
  CHECK(with(unreadable, absent).dates_exclude(Date{2015, 1, 1}) == DateExclusion::UNREADABLE);

  // No dates rule nothing out -- which is not a statement that the instrument is listed.
  CHECK(with(absent, absent).dates_exclude(Date{2015, 1, 1}) == DateExclusion::NOT_EXCLUDED);
}

TEST_CASE("KIS reads an instrument date as stated, not stated, or unreadable", "[core][reference][kis]") {
  const auto parse = [](std::string_view listing) {
    return kis::KisClientTestAccess::parse_instrument(
      InstrumentId{Country::KR, Venue::COMBINED, "000660"},
      json_response(
        std::string(R"({"rt_cd":"0","output":{"mket_id_cd":"STK","scty_grp_id_cd":"ST",)") +
        R"("std_pdno":"KR7000660001","prdt_name":"x","prdt_eng_name":"x",)" +
        R"("scts_mket_lstg_dt":)" + std::string(listing) + R"(,"scts_mket_lstg_abol_dt":""}})"
      )
    );
  };
  using State = StatedDate::State;

  const auto read = parse(R"("19961226")");
  REQUIRE(read);
  CHECK(read.value().listed_on_ == StatedDate::stated(Date{1996, 12, 26}));

  // The sheet's own blank, JSON null, and the zero date the rest of the workbook uses as a blank.
  for (const auto* blank : {R"("")", "null", R"("00000000")"}) {
    const auto result = parse(blank);
    REQUIRE(result);
    CHECK(result.value().listed_on_.state() == State::NOT_STATED);
  }

  // The prefix case first, with its premise: `std::stoi` reads "2x" as 2, so the old reader
  // produced 1996-12-02 here -- a different, valid date.
  REQUIRE(std::stoi("2x") == 2);
  // And the reason every character is checked as a digit before any arithmetic: ':' is the
  // character after '9', so digit arithmetic without that check reads "0:" as 10 and "1996120:" as
  // 1996-12-10.
  REQUIRE(':' - '0' == 10);
  for (const auto* damaged :
       {R"("1996122x")", R"("1996120:")", R"("1996-12-26")", R"("19961332")", R"("2024023")",
        R"("1996 226")", "19961226"}) {
    const auto result = parse(damaged);
    // The row survives: a damaged date costs nothing else in it.
    REQUIRE(result);
    CHECK(result.value().listed_on_.state() == State::UNREADABLE);
    CHECK(result.value().currency_ == Currency::krw());
    CHECK(result.value().price_rule_.kind_ != PriceRule::Kind::UNKNOWN);
  }
}

TEST_CASE(
  "Toss records an instrument date it cannot read, and keeps the row",
  "[core][reference][toss]"
) {
  const auto hynix = InstrumentId{Country::KR, Venue::KRX, "000660"};
  const auto parse = [&](std::string_view list, std::string_view delist, std::string_view status) {
    return tossinvest::TossClientTestAccess::parse_instrument(
      hynix,
      json_response(
        std::string(R"({"result":[{"symbol":"000660","name":"SK","englishName":"SK",)") +
        R"("isinCode":"KR7000660001","market":"KOSPI","securityType":"STOCK",)" +
        R"("isCommonShare":true,"status":")" + std::string(status) + R"(","currency":"KRW",)" +
        R"("listDate":)" + std::string(list) + R"(,"delistDate":)" + std::string(delist) +
        R"(,"sharesOutstanding":"728002365"}]})"
      )
    );
  };
  const Date today{2026, 9, 29};

  // This read was once refused, because an empty `delisted_on_` read as "not delisted". That field
  // can no longer be read that way, so the row survives -- currency and price grid included -- and
  // the damage is recorded where the date would have been.
  const auto damaged_delisting = parse(R"("1996-12-26")", R"("2026-13-01")", "ACTIVE");
  REQUIRE(damaged_delisting);
  CHECK(damaged_delisting.value().delisted_on_.state() == StatedDate::State::UNREADABLE);
  CHECK(damaged_delisting.value().listed_on_ == StatedDate::stated(Date{1996, 12, 26}));
  CHECK(damaged_delisting.value().currency_ == Currency::krw());
  CHECK(damaged_delisting.value().price_rule_.kind_ != PriceRule::Kind::UNKNOWN);
  CHECK(damaged_delisting.value().dates_exclude(today) == DateExclusion::UNREADABLE);

  // The numeric-prefix case read as the year 20 before the parser required digits.
  const auto damaged_listing = parse(R"("19x6-12-26")", "null", "ACTIVE");
  REQUIRE(damaged_listing);
  CHECK(damaged_listing.value().listed_on_.state() == StatedDate::State::UNREADABLE);
  CHECK(damaged_listing.value().delisted_on_.state() == StatedDate::State::NOT_STATED);

  // A value that is not a string at all is a date stated in a form nothing here reads.
  const auto numeric = parse("19961226", "null", "ACTIVE");
  REQUIRE(numeric);
  CHECK(numeric.value().listed_on_.state() == StatedDate::State::UNREADABLE);

  // A stated date and an absent one are both ordinary answers, and null still means no delisting.
  const auto sound = parse(R"("1996-12-26")", "null", "ACTIVE");
  REQUIRE(sound);
  CHECK(sound.value().listed_on_ == StatedDate::stated(Date{1996, 12, 26}));
  CHECK(sound.value().delisted_on_.state() == StatedDate::State::NOT_STATED);
  CHECK(sound.value().dates_exclude(today) == DateExclusion::NOT_EXCLUDED);

  // Toss documents `null` as information not provided, so a scheduled listing with no dates makes
  // no claim about being listed: the dates rule nothing out, and the status is what says it is not
  // trading yet.
  const auto scheduled = parse("null", "null", "SCHEDULED");
  REQUIRE(scheduled);
  CHECK(scheduled.value().listed_on_.state() == StatedDate::State::NOT_STATED);
  CHECK(scheduled.value().dates_exclude(today) == DateExclusion::NOT_EXCLUDED);
  CHECK(scheduled.value().status_ == InstrumentStatus::SCHEDULED);
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
  REQUIRE(schedule.value().rate_for(Country::KR, Date{2026, 9, 23}).rate_);
  REQUIRE_FALSE(schedule.value().rate_for(Country::US, Date{2026, 9, 23}).rate_);

  // The spec's "unlimited" example: a null end is a rate with no scheduled change.
  const auto unlimited = tossinvest::TossClientTestAccess::parse_commissions(
    json_response(
      R"({"result":[{"marketCountry":"KR","commissionRate":"0.00015",)"
      R"("startDate":"2026-01-01","endDate":null}]})"
    )
  );
  REQUIRE(unlimited);
  REQUIRE_FALSE(unlimited.value().rates_[0].until_);
  REQUIRE(unlimited.value().rate_for(Country::KR, Date{2099, 1, 1}).rate_);

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
  REQUIRE(mixed.value().rate_for(Country::KR, Date{2026, 9, 23}).rate_);
  REQUIRE_FALSE(mixed.value().rate_for(Country::US, Date{2026, 9, 23}).rate_);

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
  const auto korean = nameless.value().rate_for(Country::KR, Date{2026, 9, 23}).rate_;
  REQUIRE(korean);
  REQUIRE(korean->rate_ == Decimal64::parse("0.00015"));

  // A complete schedule says so, which is what makes the count worth reading.
  REQUIRE(schedule.value().dropped_ == 0);
  REQUIRE(schedule.value().unreadable_bounds_ == 0);

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
  live.is_demo_ = false;
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

TEST_CASE("a venue key keeps three kinds apart") {
  // The defect this type replaced: an unnormalized row defaulted to `Venue::UNSPECIFIED`, which was
  // also how a row said "any venue" -- so a rate published for a market nobody normalized could be
  // charged on every venue. The kinds are compared before the values for that reason.
  const auto neutral = VenueKey::any();
  const auto krx = VenueKey::normalized(Venue::KRX);
  const auto raw_krx = VenueKey::raw("KRX");
  REQUIRE(krx);
  REQUIRE(raw_krx);

  CHECK(neutral.is_any());
  CHECK(krx->matches(*krx));
  CHECK_FALSE(krx->matches(neutral));
  CHECK_FALSE(neutral.matches(*krx));
  // Same text, different kind, and they must not be the same key: nothing has established that the
  // broker's `KRX` means this library's `Venue::KRX`.
  CHECK_FALSE(raw_krx->matches(*krx));
  CHECK_FALSE(krx->matches(*raw_krx));
  CHECK(raw_krx->matches(*VenueKey::raw("KRX")));
  CHECK_FALSE(raw_krx->matches(*VenueKey::raw("NXT")));

  // `UNSPECIFIED` is the absence of a statement, which is `any()`, so it is refused here rather than
  // quietly becoming a fourth spelling of neutral.
  CHECK_FALSE(VenueKey::normalized(Venue::UNSPECIFIED));
  // A code too long to preserve, and an empty one, are both refused rather than stored as blank.
  CHECK_FALSE(VenueKey::raw("VERYLONGVENUECODE"));
  CHECK_FALSE(VenueKey::raw(""));
}

TEST_CASE("KIS places, amends and reads back a stop-limit order's trigger", "[broker][kis][trigger]") {
  using Traits = BrokerTraits<kis::KisClient>;
  const kis::KisAccount account{"12345678", "01"};
  const kis::KisConfig live;

  auto stop = korean_limit_buy();
  stop.type_ = OrderType::STOP_LIMIT;
  stop.venue_ = Venue::KRX;
  stop.price_ = Decimal64::parse("69000");
  stop.trigger_price_ = Decimal64::parse("69500");

  SECTION("22 is composed, and the trigger goes in CNDT_PRIC beside the limit") {
    std::string division;
    REQUIRE_FALSE(kis::KisClientTestAccess::validate_order(stop, division));
    REQUIRE(division == "22");
    const auto body = json::parse(
      kis::KisClientTestAccess::place_order_request(account, live, stop, division).body_
    ).as_object();
    CHECK(body.at("ORD_DVSN").as_string() == "22");
    CHECK(body.at("ORD_UNPR").as_string() == "69000");
    CHECK(body.at("CNDT_PRIC").as_string() == "69500");
  }

  SECTION("every other body is sent without the field, as before") {
    const auto body = json::parse(
      kis::KisClientTestAccess::place_order_request(account, live, korean_limit_buy(), "00").body_
    ).as_object();
    CHECK_FALSE(body.contains("CNDT_PRIC"));
  }

  SECTION("a trigger the order cannot carry, or a stop without one, is refused before I/O") {
    const auto refused = [](OrderRequest order) {
      std::string division;
      const auto error = kis::KisClientTestAccess::validate_order(order, division);
      REQUIRE(error);
      CHECK(error->broker_code_ == "invalid_trigger_price");
      CHECK_FALSE(error->reached_broker_);
    };
    auto missing = stop;
    missing.trigger_price_.reset();
    refused(missing);
    auto on_limit = korean_limit_buy();
    on_limit.trigger_price_ = Decimal64::parse("69500");
    refused(on_limit);
    for (const auto* value : {"0", "-69500", "69500.5"}) {
      auto bad = stop;
      bad.trigger_price_ = Decimal64::parse(value);
      INFO("trigger " << value);
      refused(bad);
    }
  }

  SECTION("22 is not published under SOR, so a combined route is refused") {
    auto routed = stop;
    routed.venue_ = Venue::COMBINED;
    std::string division;
    const auto error = kis::KisClientTestAccess::validate_order(routed, division);
    REQUIRE(error);
    CHECK(error->broker_code_ != "invalid_trigger_price");
  }

  WorkingOrder<kis::Broker> working;
  working.ref_.primary_ = "0001569139";
  working.ref_.secondary_ = "06010";
  working.order_.type_ = OrderType::STOP_LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64::parse("69000");
  working.order_.trigger_price_ = Decimal64::parse("69500");
  working.amendable_quantity_ = Decimal64::parse("10");

  const auto amend_body = [&](const kis::KisAmendment& amendment, bool cancel) {
    std::string division;
    const auto refused = cancel ?
      kis::KisClientTestAccess::cancel_division(amendment, division) :
      kis::KisClientTestAccess::amendment_division(amendment, division);
    REQUIRE_FALSE(refused);
    return json::parse(
      kis::KisClientTestAccess::amend_order_request(account, live, amendment, cancel, division).body_
    ).as_object();
  };

  SECTION("a reprice restates the resting trigger") {
    auto amendment = Traits::to_replacement(working.ref_, reprice(working, Decimal64::parse("68000")));
    REQUIRE(amendment);
    const auto body = amend_body(amendment.value(), false);
    CHECK(body.at("ORD_DVSN").as_string() == "22");
    CHECK(body.at("ORD_UNPR").as_string() == "68000");
    CHECK(body.at("CNDT_PRIC").as_string() == "69500");
  }

  SECTION("a shape that leaves the trigger unset keeps it") {
    auto amendment = Traits::to_replacement(
      working.ref_, replace_with(working, OrderShape{OrderType::STOP_LIMIT, Decimal64::parse("68000")})
    );
    REQUIRE(amendment);
    CHECK(amend_body(amendment.value(), false).at("CNDT_PRIC").as_string() == "69500");
  }

  SECTION("a stop whose trigger KIS never reported cannot be modified, and can be cancelled") {
    auto unreported = working;
    unreported.order_.trigger_price_.reset();
    auto modified = Traits::to_replacement(unreported.ref_, reprice(unreported, Decimal64::parse("68000")));
    REQUIRE(modified);
    std::string division;
    const auto error = kis::KisClientTestAccess::amendment_division(modified.value(), division);
    REQUIRE(error);
    CHECK(error->broker_code_ == "invalid_trigger_price");

    auto canceled = Traits::to_cancellation(unreported.ref_, cancel_all(unreported));
    REQUIRE(canceled);
    std::string cancel_division;
    REQUIRE_FALSE(kis::KisClientTestAccess::cancel_division(canceled.value(), cancel_division));
    const auto body = amend_body(canceled.value(), true);
    CHECK(body.at("RVSE_CNCL_DVSN_CD").as_string() == "02");
    CHECK_FALSE(body.contains("CNDT_PRIC"));
  }

  SECTION("a cancellation restates a valid stop trigger and drops one that is not") {
    auto canceled = Traits::to_cancellation(working.ref_, cancel_all(working));
    REQUIRE(canceled);
    CHECK(amend_body(canceled.value(), true).at("CNDT_PRIC").as_string() == "69500");

    // A hand-built cancellation of a plain limit that states a trigger: withdrawn, never refused
    // for it, and the inapplicable field is not sent.
    auto limit = working;
    limit.order_.type_ = OrderType::LIMIT;
    auto stray = Traits::to_cancellation(limit.ref_, cancel_all(limit));
    REQUIRE(stray);
    std::string division;
    REQUIRE_FALSE(kis::KisClientTestAccess::cancel_division(stray.value(), division));
    const auto body = json::parse(
      kis::KisClientTestAccess::amend_order_request(account, live, stray.value(), true, division).body_
    ).as_object();
    CHECK(body.at("ORD_DVSN").as_string() == "00");
    CHECK_FALSE(body.contains("CNDT_PRIC"));
  }

  SECTION("the working listing reports the trigger and the raw release flag") {
    auto orders = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"0001569139",)"
        R"("orgn_odno":"","ord_dvsn_name":"stop","pdno":"005930",)"
        R"("prdt_name":"Samsung","rvse_cncl_dvsn_name":"modify","ord_qty":"10",)"
        R"("ord_unpr":"69000","ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0",)"
        R"("psbl_qty":"10","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"22",)"
        R"("stpm_cndt_pric":"69500","stpm_efct_occr_yn":"N"},)"
        R"({"ord_gno_brno":"06010","odno":"0001569140",)"
        R"("orgn_odno":"","ord_dvsn_name":"limit","pdno":"005930",)"
        R"("prdt_name":"Samsung","rvse_cncl_dvsn_name":"modify","ord_qty":"10",)"
        R"("ord_unpr":"70000","ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0",)"
        R"("psbl_qty":"10","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("stpm_cndt_pric":"0"},)"
        R"({"ord_gno_brno":"06010","odno":"0001569141",)"
        R"("orgn_odno":"","ord_dvsn_name":"stop","pdno":"005930",)"
        R"("prdt_name":"Samsung","rvse_cncl_dvsn_name":"modify","ord_qty":"10",)"
        R"("ord_unpr":"69000","ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0",)"
        R"("psbl_qty":"10","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"22",)"
        R"("stpm_cndt_pric":"abc"}]})"
      )
    );
    REQUIRE(orders);
    const auto& rows = orders.value().orders_;
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].order_.type_ == OrderType::STOP_LIMIT);
    CHECK(rows[0].order_.trigger_price_ == Decimal64::parse("69500"));
    CHECK(rows[0].stop_released_ == "N");
    // An ordinary order reports zero, which is not a trigger.
    CHECK_FALSE(rows[1].order_.trigger_price_);
    CHECK(rows[1].stop_released_.empty());
    // An unreadable trigger costs the trigger, not the row -- the order is still listed.
    CHECK(rows[2].order_.type_ == OrderType::STOP_LIMIT);
    CHECK_FALSE(rows[2].order_.trigger_price_);
  }

  SECTION("a positive trigger on a row that is not a stop is recorded, and blocks a modification") {
    auto orders = kis::KisClientTestAccess::parse_open_orders(
      json_response(
        R"({"rt_cd":"0","output":[{"ord_gno_brno":"06010","odno":"0001569140",)"
        R"("orgn_odno":"","ord_dvsn_name":"limit","pdno":"005930",)"
        R"("prdt_name":"Samsung","rvse_cncl_dvsn_name":"modify","ord_qty":"10",)"
        R"("ord_unpr":"70000","ord_tmd":"131438","tot_ccld_qty":"0","tot_ccld_amt":"0",)"
        R"("psbl_qty":"10","sll_buy_dvsn_cd":"02","ord_dvsn_cd":"00",)"
        R"("stpm_cndt_pric":"69500"}]})"
      )
    );
    REQUIRE(orders);
    const auto& row = orders.value().orders_.front();
    // KIS contradicting its own code is kept where a caller can see it, not discarded.
    REQUIRE(row.order_.trigger_price_ == Decimal64::parse("69500"));

    WorkingOrder<kis::Broker> contradicted;
    contradicted.ref_.primary_ = "0001569140";
    contradicted.ref_.secondary_ = "06010";
    contradicted.order_ = row.order_;
    contradicted.amendable_quantity_ = row.amendable_quantity_;
    auto modified = Traits::to_replacement(contradicted.ref_, reprice(contradicted, Decimal64::parse("69000")));
    REQUIRE(modified);
    std::string division;
    const auto error = kis::KisClientTestAccess::amendment_division(modified.value(), division);
    REQUIRE(error);
    CHECK(error->broker_code_ == "invalid_trigger_price");
    auto canceled = Traits::to_cancellation(contradicted.ref_, cancel_all(contradicted));
    REQUIRE(canceled);
    std::string cancel_division;
    CHECK_FALSE(kis::KisClientTestAccess::cancel_division(canceled.value(), cancel_division));
  }

  SECTION("the history listing reports the trigger") {
    auto result = kis::KisClientTestAccess::parse_order_history(
      json_response(
        R"({"rt_cd":"0","output1":[{"ord_dt":"20260328","ord_gno_brno":"06010",)"
        R"("odno":"0001569157","orgn_odno":"","ord_dvsn_name":"stop",)"
        R"("sll_buy_dvsn_cd":"01","sll_buy_dvsn_cd_name":"sell","pdno":"005930",)"
        R"("prdt_name":"Samsung","ord_qty":"10","ord_unpr":"69000","ord_tmd":"093000",)"
        R"("tot_ccld_qty":"0","avg_prvs":"0","cncl_yn":"N","tot_ccld_amt":"0",)"
        R"("ord_dvsn_cd":"22","cncl_cfrm_qty":"0","rmn_qty":"10","rjct_qty":"0",)"
        R"("stpm_cndt_pric":"69500"}]})"
      )
    );
    REQUIRE(result);
    CHECK(result.value().orders_.front().trigger_price_ == Decimal64::parse("69500"));
  }
}
