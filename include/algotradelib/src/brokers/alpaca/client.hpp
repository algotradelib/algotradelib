#pragma once

#include "./config.hpp"

#include "../../core/calendar.hpp"
#include "../../core/instrument.hpp"
#include "../../core/market_data.hpp"
#include "../../core/types.hpp"
#include "../../network/http_client.hpp"
#include "../../network/token_store.hpp"
#include "../common/adapter.hpp"
#include "../common/us.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib::alpaca {

// What Alpaca's paper host showed on 2026-11-20 is recorded in labs/atl-thirdparty/README.md; the stored docs are
// docs/api/alpaca at the workspace root.

/**
 * @brief The Alpaca broker tag: order references are typed by it, so an Alpaca reference works with no other
 *   broker.
 */
struct Broker {
  /** @brief The broker's name. */
  static constexpr std::string_view name = "alpaca";
};

/** @brief The Alpaca account selector, which is empty: the keys name the account, and no call takes a number. */
struct AlpacaAccount {
  /**
   * @brief Compares two selectors (the arguments are not used); always equal, since one key pair drives one account.
   * @return `true`.
   */
  friend bool operator==(const AlpacaAccount&, const AlpacaAccount&) {
    return true;
  }
};

/** @brief The one account Alpaca's `/v2/account` reports for the keys. */
struct AlpacaAccountInfo {
  /** @brief The account number as Alpaca reported it, for display. */
  std::string account_number_;
};

/** @brief Alpaca's acknowledgement of a placement or cancellation. */
struct AlpacaOrderAck {
  /** @brief The order id. */
  std::string order_id_;
  /** @brief The `client_order_id` Alpaca echoed, if any. */
  std::optional<std::string> client_order_id_;
  /** @brief When the acknowledgement arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/** @brief A cancellation of one whole order (Alpaca replacements are refused by this adapter). */
struct AlpacaAmendment {
  /** @brief The order id. */
  std::string order_id_;
};

/** @brief The open US equity orders, before normalizing. */
struct AlpacaOpenOrders {
  /** @brief The orders. */
  std::vector<OrderSnapshot> orders_;
  /** @brief When the listing arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

namespace detail {

/** @brief The most characters Alpaca takes in a `client_order_id`. */
inline constexpr std::size_t client_order_id_limit = 128;

/** @brief The most bars one request returns: one page of Alpaca's 10,000, less the forming bar that may be dropped. */
inline constexpr int bar_count_limit = 9999;

/** @brief How many days before now a calendar without a start begins. */
inline constexpr int calendar_days_back = 10;

/** @brief How many days after its first date a calendar covers. */
inline constexpr int calendar_span_days = 40;

/**
 * @brief Days since 1970-01-01 of a civil date (Howard Hinnant's `days_from_civil`).
 * @param date The date.
 * @return The day count.
 */
inline std::int64_t days_from_civil(const Date& date) {
  std::int64_t year = date.year_;
  const unsigned month = date.month_;
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + date.day_ - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

/**
 * @brief The UTC date of an instant.
 * @param instant The instant.
 * @return The date.
 */
inline Date utc_date(std::chrono::system_clock::time_point instant) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(instant.time_since_epoch()).count();
  return date_from_epoch_days(seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400);
}

/**
 * @brief New York's offset from UTC at an instant: US daylight time (since 2007) from the second Sunday of March at
 *   02:00 local to the first Sunday of November at 02:00 local.
 *
 * Used only to turn an order-history query's dates into instants; the market phase comes from the calendar's
 * instants.
 *
 * @param instant The instant.
 * @return -4 h in daylight time, else -5 h.
 */
inline std::chrono::hours eastern_offset(std::chrono::system_clock::time_point instant) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(instant.time_since_epoch()).count();
  const auto year = utc_date(instant - std::chrono::hours(5)).year_;
  // The n-th Sunday of a month: 1970-01-01 was a Thursday (day 4 counting Sunday as 0).
  const auto nth_sunday = [year](unsigned month, int nth) {
    const auto first = days_from_civil(Date{year, month, 1});
    const auto weekday = ((first % 7) + 7 + 4) % 7;
    return first + (7 - weekday) % 7 + 7 * (nth - 1);
  };
  const auto starts = nth_sunday(3, 2) * 86400 + 2 * 3600 + 5 * 3600;
  const auto ends = nth_sunday(11, 1) * 86400 + 2 * 3600 + 4 * 3600;
  return seconds >= starts && seconds < ends ? std::chrono::hours(-4) : std::chrono::hours(-5);
}

/**
 * @brief The instant a New York date begins.
 * @param date The date.
 * @return Its local midnight.
 */
inline std::chrono::system_clock::time_point eastern_midnight(const Date& date) {
  const auto utc_midnight =
    std::chrono::system_clock::time_point{} + std::chrono::seconds(days_from_civil(date) * 86400);
  // Local midnight is 04:00Z or 05:00Z, before either change (07:00Z in March, 06:00Z in November), so the offset at
  // 04:00Z of the date is the one at midnight, change days included.
  return utc_midnight - eastern_offset(utc_midnight + std::chrono::hours(4));
}

/**
 * @brief Writes a date as `YYYY-MM-DD`.
 * @param date The date.
 * @return The text.
 */
inline std::string iso_date(const Date& date) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", date.year_, date.month_, date.day_);
  return buffer;
}

/**
 * @brief Reads a date written `YYYY-MM-DD`.
 * @param text The text.
 * @return The date, or none if it is not that form or not a valid date.
 */
inline std::optional<Date> parse_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  const auto field = [text](std::size_t from, std::size_t count, int& out) {
    const auto parsed = std::from_chars(text.data() + from, text.data() + from + count, out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + from + count;
  };
  int year = 0;
  int month = 0;
  int day = 0;
  if (!field(0, 4, year) || !field(5, 2, month) || !field(8, 2, day) || month < 1 || day < 1) {
    return std::nullopt;
  }
  const Date date{year, static_cast<unsigned>(month), static_cast<unsigned>(day)};
  if (!is_valid_date(date)) {
    return std::nullopt;
  }
  return date;
}

/**
 * @brief Reads a decimal Alpaca sends as a string (trading replies) or a JSON number (market data).
 *
 * A JSON number arrives as a double; it is read back with the fewest decimal places (up to 15) that give the same
 * double, which are the digits Alpaca sent for any price with up to 15 significant digits.
 *
 * @param object The object.
 * @param key The member.
 * @return The value, or none when it is missing or null.
 * @throws std::runtime_error when it is not a number.
 */
inline std::optional<Decimal64> number(const json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || value->is_null()) {
    return std::nullopt;
  }
  try {
    if (value->is_string()) {
      return Decimal64::parse(std::string_view(value->as_string()));
    }
    if (value->is_int64()) {
      return Decimal64::parse(std::to_string(value->as_int64()));
    }
    if (value->is_uint64()) {
      return Decimal64::parse(std::to_string(value->as_uint64()));
    }
    if (value->is_double()) {
      // The fewest decimal places that give the same double back, found by exact division: no locale, and no
      // floating-point `std::to_chars`, which older standard libraries and macOS targets lack.
      const double number = value->as_double();
      double power = 1;
      for (std::uint8_t scale = 0; std::isfinite(number) && scale <= 15; ++scale, power *= 10) {
        const double scaled = std::round(number * power);
        if (std::fabs(scaled) < 9007199254740992.0) {
          const auto coefficient = static_cast<std::int64_t>(scaled);
          if (static_cast<double>(coefficient) / power == number) {
            return Decimal64(coefficient, scale);
          }
        }
      }
    }
  } catch (const std::exception&) {
  }
  throw std::runtime_error(std::string("a field is not a number: ") + key);
}

/**
 * @brief Reads a decimal that must be there.
 * @param object The object.
 * @param key The member.
 * @return The value.
 * @throws std::runtime_error when it is missing, null or not a number.
 */
inline Decimal64 required(const json::object& object, const char* key) {
  auto value = number(object, key);
  if (!value) {
    throw std::runtime_error(std::string("a field is missing: ") + key);
  }
  return *value;
}

/**
 * @brief Reads a string member.
 * @param object The object.
 * @param key The member.
 * @return Its value, or empty when it is missing or not a string.
 */
inline std::string text(const json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

/**
 * @brief Reads a timestamp member (RFC 3339, up to nanoseconds; the library keeps milliseconds).
 * @param object The object.
 * @param key The member.
 * @return The instant, or none when it is missing or null.
 * @throws std::runtime_error when it is present and unreadable.
 */
inline std::optional<std::chrono::system_clock::time_point> instant(const json::object& object, const char* key) {
  const auto value = text(object, key);
  if (value.empty()) {
    return std::nullopt;
  }
  auto parsed = parse_iso8601_timestamp(value);
  if (!parsed) {
    throw std::runtime_error(std::string("a time cannot be read: ") + key);
  }
  return parsed;
}

/**
 * @brief The library's status for Alpaca's.
 *
 * `expired` (a day order at the close) reads `CANCELED`, the library having no expiry; `done_for_day`, `stopped`,
 * `suspended`, `calculated`, `held` and any status not listed read `UNKNOWN`.
 *
 * @param status Alpaca's status.
 * @return The status.
 */
inline OrderStatus order_status(std::string_view status) {
  if (status == "new" || status == "accepted" || status == "pending_new" || status == "accepted_for_bidding") {
    return OrderStatus::PENDING;
  }
  if (status == "partially_filled") {
    return OrderStatus::PARTIAL_FILLED;
  }
  if (status == "filled") {
    return OrderStatus::FILLED;
  }
  if (status == "canceled" || status == "expired") {
    return OrderStatus::CANCELED;
  }
  if (status == "rejected") {
    return OrderStatus::REJECTED;
  }
  if (status == "pending_cancel") {
    return OrderStatus::PENDING_CANCEL;
  }
  if (status == "pending_replace") {
    return OrderStatus::PENDING_REPLACE;
  }
  if (status == "replaced") {
    return OrderStatus::REPLACED;
  }
  return OrderStatus::UNKNOWN;
}

/**
 * @brief Makes a refusal of something this adapter does not send (`INVALID_ARGUMENT`, not sent).
 * @param code The code.
 * @param message The message.
 * @return The error.
 */
inline BrokerError refusal(std::string code, std::string message) {
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = std::move(code);
  error.message_ = std::move(message);
  return error;
}

/**
 * @brief The identity this adapter gives a US listing: `Country::US` and `Venue::UNSPECIFIED`, as Toss's adapter.
 * @param symbol The symbol, such as `BRK.B`.
 * @return The identity.
 */
inline InstrumentId instrument_of(std::string symbol) {
  return InstrumentId{Country::US, Venue::UNSPECIFIED, std::move(symbol)};
}

/**
 * @brief Refuses an instrument that is not a US listing as this adapter identifies one: `Country::US`,
 *   `Venue::UNSPECIFIED`, and 1 to 16 characters of `A-Z`, `0-9` and `.`.
 * @param instrument The instrument.
 * @return The refusal (`instrument_unsupported`), or nothing.
 */
inline std::optional<BrokerError> unsupported_instrument(const InstrumentId& instrument) {
  if (
    instrument.country_ != Country::US || instrument.venue_ != Venue::UNSPECIFIED || instrument.symbol_.empty() ||
    instrument.symbol_.size() > 16 ||
    instrument.symbol_.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.") != std::string::npos
  ) {
    return refusal(
      "instrument_unsupported",
      "the Alpaca adapter takes US listings: Country::US, Venue::UNSPECIFIED and a symbol such as BRK.B"
    );
  }
  return std::nullopt;
}

/**
 * @brief Tests whether a text can be an Alpaca order id in a request path: 1 to 128 characters of `A-Z`, `a-z`, `0-9`,
 *   `-` and `_` (Alpaca's ids are UUIDs).
 *
 * Anything else is refused before sending, so `.` or `..` can never turn `DELETE /v2/orders/{id}` into
 * `DELETE /v2/orders`, which cancels every open order.
 *
 * @param id The id.
 * @return Whether it may be sent.
 */
inline bool valid_order_id(std::string_view id) {
  return !id.empty() && id.size() <= client_order_id_limit &&
    id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string_view::npos;
}

/**
 * @brief Makes the refusal of an order id `valid_order_id` rejects (`invalid_reference`, not sent).
 * @return The error.
 */
inline BrokerError invalid_order_id() {
  return refusal("invalid_reference", "an Alpaca order id is 1 to 128 characters of letters, digits, - and _");
}

/**
 * @brief Refuses a bar request this adapter cannot answer as asked, before any I/O: an interval other than 1, 5, 15
 *   and 30 minutes, an hour and a day, a count outside 1 to 9,999, or an instrument `unsupported_instrument` refuses.
 * @param query The query.
 * @return The refusal (not sent), or nothing.
 */
inline std::optional<BrokerError> validate_bar_query(const BarQuery& query) {
  const auto interval = query.interval_;
  if (
    interval != BarInterval::ONE_MINUTE && interval != BarInterval::FIVE_MINUTES &&
    interval != BarInterval::FIFTEEN_MINUTES && interval != BarInterval::THIRTY_MINUTES &&
    interval != BarInterval::ONE_HOUR && interval != BarInterval::ONE_DAY
  ) {
    return refusal("unsupported_interval", "the Alpaca adapter serves 1m, 5m, 15m, 30m, 1h and 1d bars");
  }
  if (query.count_ <= 0 || query.count_ > bar_count_limit) {
    return refusal("invalid_count", "ask Alpaca for 1 to 9999 bars per call");
  }
  return unsupported_instrument(query.instrument_);
}

/**
 * @brief Tests for a positive number.
 * @param value The number.
 * @return Whether it is above zero.
 */
inline bool positive(const Decimal64& value) {
  return decimal_normalized(value).coefficient() > 0;
}

/**
 * @brief Refuses an order this adapter does not send as described, before any I/O; every placement reaches it, the
 *   client's own `place_order` included.
 *
 * It sends limit and market day orders in the regular session, in whole shares, a limit priced on
 * `us::equity_price_rule()`, with no venue and no trigger price.
 * What needs a read (the asset's listing, the position for a sell, Alpaca's clock) is checked by
 * `AlpacaBlockingClient::place_order` after this.
 *
 * @param order The order.
 * @return The refusal (`INVALID_ARGUMENT`, not sent), or nothing.
 */
inline std::optional<BrokerError> validate_order(const OrderRequest& order) {
  if (auto refused = unsupported_instrument(order.instrument_)) {
    return refused;
  }
  if (order.venue_ != Venue::UNSPECIFIED) {
    return refusal("venue_unsupported", "Alpaca routes orders itself; leave the venue unspecified");
  }
  if (
    order.client_order_id_ &&
    (order.client_order_id_->empty() || order.client_order_id_->size() > client_order_id_limit)
  ) {
    return refusal("invalid_client_order_id", "Alpaca takes a client_order_id of 1 to 128 characters");
  }
  if (order.time_in_force_ != TimeInForce::DAY) {
    return refusal("time_in_force_unsupported", "the Alpaca adapter sends day orders only");
  }
  if (order.session_ != OrderSession::REGULAR) {
    return refusal("session_unsupported", "the Alpaca adapter sends regular-session orders only, no extended hours");
  }
  if (order.trigger_price_) {
    return refusal("order_type_unsupported", "the Alpaca adapter sends no stop orders, so no trigger price");
  }
  switch (order.type_) {
    case OrderType::LIMIT: {
      if (!order.price_ || !positive(*order.price_)) {
        return refusal("invalid_price", "a limit order needs a positive price");
      }
      const auto on_grid = us::equity_price_rule().on_tick(*order.price_);
      if (!on_grid || !*on_grid) {
        return refusal("off_tick", "a US limit price has at most 2 decimals from $1 and 4 below (Rule 612)");
      }
      break;
    }
    case OrderType::MARKET:
      if (order.price_) {
        return refusal("invalid_price", "a market order carries no price");
      }
      break;
    case OrderType::UNKNOWN:
    case OrderType::CONDITIONAL_LIMIT:
    case OrderType::BEST_LIMIT:
    case OrderType::PRIMARY_LIMIT:
    case OrderType::MID_PRICE:
    case OrderType::STOP_LIMIT:
    case OrderType::CLOSING_PRICE:
      return refusal("order_type_unsupported", "the Alpaca adapter sends limit and market orders only");
  }
  const auto quantity = decimal_normalized(order.quantity_);
  if (quantity.coefficient() <= 0 || quantity.scale() != 0) {
    return refusal("invalid_quantity", "the Alpaca adapter sends whole, positive share quantities only");
  }
  return std::nullopt;
}

/**
 * @brief Tests whether an asset's `exchange` is a US national exchange this adapter trades on.
 *
 * The trading API's `Exchange` values less `OTC`, the crypto venues and indexes; a value not listed is refused.
 *
 * @param exchange Alpaca's `exchange`.
 * @return Whether it is AMEX, ARCA, BATS, NYSE, NASDAQ, NYSEARCA or TXSE.
 */
inline bool listed_exchange(std::string_view exchange) {
  return exchange == "AMEX" || exchange == "ARCA" || exchange == "BATS" || exchange == "NYSE" || exchange == "NASDAQ" ||
    exchange == "NYSEARCA" || exchange == "TXSE";
}

/**
 * @brief Refuses an asset this adapter does not trade: anything but an active, tradable `us_equity` listed on a US
 *   national exchange.
 *
 * Alpaca also trades OTC equities, which follow another session, another price grid and a feed this adapter does not
 * read, so they are refused, as is any exchange not known to be national.
 * A reply that names another symbol, or leaves out `class`, `status`, `tradable` or `exchange`, is refused too.
 *
 * @param asset Alpaca's asset object.
 * @param symbol The symbol asked for.
 * @return The refusal (not sent), or nothing.
 */
inline std::optional<BrokerError> unlisted_asset(const json::object& asset, const std::string& symbol) {
  const auto* tradable = asset.if_contains("tradable");
  const auto asset_class = text(asset, "class");
  const auto status = text(asset, "status");
  const auto exchange = text(asset, "exchange");
  if (text(asset, "symbol") != symbol) {
    auto error = refusal("instrument_mismatch", "Alpaca's asset reply names another symbol than " + symbol);
    error.category_ = ErrorCategory::PROTOCOL;
    return error;
  }
  if (tradable == nullptr || !tradable->is_bool() || asset_class.empty() || status.empty() || exchange.empty()) {
    auto error =
      refusal("instrument_unreadable", "Alpaca's asset reply for " + symbol + " leaves out a field it needs");
    error.category_ = ErrorCategory::PROTOCOL;
    return error;
  }
  if (asset_class != "us_equity" || !listed_exchange(exchange)) {
    return refusal(
      "instrument_not_listed",
      symbol + " is a " + asset_class + " asset on " + exchange +
        ": the Alpaca adapter trades US equities listed on a national exchange only, not OTC"
    );
  }
  if (status != "active" || !tradable->as_bool()) {
    return refusal("instrument_not_tradable", symbol + " is not active and tradable at Alpaca");
  }
  return std::nullopt;
}

} // namespace detail

/**
 * @brief A blocking client for Alpaca's Trading API and Market Data API: exchange-listed US equities, whole shares,
 *   limit and market day orders in the regular session.
 *
 * Paper trading unless `AlpacaConfig::is_demo_` is `false`; live is the same code path on another host.
 * Each call runs its requests on a private event loop over two `network::AsyncHttpClient` connections (trading,
 * market data) and returns the result. Not thread-safe: one session uses it.
 *
 * When `HttpClientOptions::rate_limit_` is empty, each connection is paced at one request a second with a burst of
 * two, one of them reserved for orders (`RateLimitBudget{2, 1, 1}`), under Alpaca's 200 a minute even if both hosts
 * share one allowance; a budget you set is used as given, on each connection.
 *
 * Every placement is checked by `detail::validate_order`, then reads the asset (refusing OTC and anything not active
 * and tradable), for a sell the position (refusing a sell above it), and Alpaca's clock (refusing unless the regular
 * session is on), before the order is sent. A refusal for the clock is `CANCELLED` with `reached_broker_` false,
 * which a runner keeps and offers again; the others are not sent either.
 */
class AlpacaBlockingClient {
public:
  /**
   * @brief Constructs a client; it connects on the first request.
   * @param context The TLS context; borrowed, and must outlive the client.
   * @param config The keys and environment.
   * @param options HTTP options; an empty `rate_limit_` gets the default pacing above.
   * @param store Ignored: Alpaca has no token to share. Accepted so `network::make_client` can build the client.
   */
  AlpacaBlockingClient(
    ssl::context& context, AlpacaConfig config, network::HttpClientOptions options = {},
    std::shared_ptr<network::TokenStore> store = nullptr
  )
  : config_(std::move(config)) {
    static_cast<void>(store);
    if (!options.rate_limit_) {
      options.rate_limit_ = default_rate_limit();
    }
    trading_ = std::make_shared<network::AsyncHttpClient>(
      ioc_.get_executor(), context, config_.trading_host(), config_.port(), options
    );
    data_ = std::make_shared<network::AsyncHttpClient>(
      ioc_.get_executor(), context, config_.data_host(), config_.port(), options
    );
  }

  /**
   * @brief The pacing each connection gets when the caller sets none.
   * @return `RateLimitBudget{2, 1, 1}`.
   */
  static network::RateLimitBudget default_rate_limit() noexcept {
    return network::RateLimitBudget{2, 1, 1};
  }

  /**
   * @brief Returns the configuration.
   * @return The configuration.
   */
  const AlpacaConfig& config() const noexcept {
    return config_;
  }

  /**
   * @brief Returns the trading connection's rate-limit statistics; read them between calls.
   * @return The statistics, or null if the connection has no budget.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    const auto* limiter = trading_->rate_limits();
    return limiter ? &limiter->stats() : nullptr;
  }

  /**
   * @brief Returns the one account the keys drive (`/v2/account`).
   * @return The account, or an error; an account not in USD is a `PROTOCOL` error.
   */
  Result<std::vector<AlpacaAccountInfo>> accounts() {
    return attempt<std::vector<AlpacaAccountInfo>>([&] {
      const auto reply = get_object(*trading_, "/v2/account", network::RateLimitGroup::ACCOUNT);
      if (detail::text(reply, "currency") != "USD") {
        throw std::runtime_error("the account is not in USD");
      }
      AlpacaAccountInfo account;
      account.account_number_ = detail::text(reply, "account_number");
      if (account.account_number_.empty()) {
        throw std::runtime_error("the account has no account_number");
      }
      return std::vector<AlpacaAccountInfo>{std::move(account)};
    });
  }

  /**
   * @brief Returns the last trade on the IEX feed, in dollars.
   * @param instrument The instrument.
   * @return The quote, or an error.
   */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    if (auto refused = detail::unsupported_instrument(instrument)) {
      return Result<QuoteSnapshot>(std::move(*refused));
    }
    return attempt<QuoteSnapshot>([&] {
      const auto reply = get_object(
        *data_, "/v2/stocks/" + broker_detail::query_encode(instrument.symbol_) + "/trades/latest?feed=iex",
        network::RateLimitGroup::MARKET_DATA
      );
      const auto* row = reply.if_contains("trade");
      if (row == nullptr || !row->is_object()) {
        throw std::runtime_error("the latest trade has no trade");
      }
      QuoteSnapshot quote;
      quote.instrument_ = std::move(instrument);
      quote.currency_ = Currency::usd();
      quote.last_price_ = detail::required(row->as_object(), "p");
      quote.exchange_time_ = detail::instant(row->as_object(), "t");
      quote.received_at_ = clock_();
      return quote;
    });
  }

  /**
   * @brief Returns closed bars, oldest first: the newest `count_` (1 to 9,999) at or before `before_` (or now), on
   *   the IEX feed, in one request.
   *
   * Minute and hour bars are stamped at their start and daily bars at New York's midnight (`BarStamp::OPEN`); a bar
   * whose period has not ended is dropped. IEX carries a small part of the consolidated volume, and its intraday bars
   * include pre- and post-market trades.
   *
   * @param query The query.
   * @return The bars, or an error.
   */
  Result<BarSeries> bars(BarQuery query) {
    if (auto refused = detail::validate_bar_query(query)) {
      return Result<BarSeries>(std::move(*refused));
    }
    return attempt<BarSeries>([&] {
      const auto now = clock_();
      const auto end = query.before_ ? *query.before_ : now;
      const auto length =
        query.interval_ == BarInterval::ONE_DAY ? std::chrono::hours(24) : *bar_interval_length(query.interval_);
      const auto reply = get_object(
        *data_,
        "/v2/stocks/" + broker_detail::query_encode(query.instrument_.symbol_) + "/bars?timeframe=" +
          timeframe(query.interval_) + "&feed=iex&adjustment=" + (query.adjusted_ ? "all" : "raw") +
          "&sort=desc&start=2015-01-01T00:00:00Z&end=" + format_iso8601_utc(end + std::chrono::seconds(1)) +
          "&limit=" + std::to_string(query.count_ + 1),
        network::RateLimitGroup::MARKET_DATA
      );
      BarSeries series;
      series.instrument_ = query.instrument_;
      series.interval_ = query.interval_;
      series.stamp_ = BarStamp::OPEN;
      series.currency_ = Currency::usd();
      series.adjusted_ = query.adjusted_;
      const auto* rows = reply.if_contains("bars");
      if (rows != nullptr && !rows->is_null() && !rows->is_array()) {
        throw std::runtime_error("the bars are not a list");
      }
      if (rows != nullptr && rows->is_array()) {
        for (const auto& value : rows->as_array()) {
          if (static_cast<int>(series.bars_.size()) == query.count_) {
            break;
          }
          if (!value.is_object()) {
            throw std::runtime_error("a bar is not an object");
          }
          const auto& row = value.as_object();
          const auto stamp = detail::instant(row, "t");
          if (!stamp) {
            throw std::runtime_error("a bar has no time");
          }
          // A daily bar ends 24 hours after its New York midnight: New York's clock changes on a Sunday.
          if (*stamp + length > now || end < *stamp) {
            continue;
          }
          Bar bar;
          bar.timestamp_ = *stamp;
          bar.open_ = detail::number(row, "o");
          bar.high_ = detail::required(row, "h");
          bar.low_ = detail::required(row, "l");
          bar.close_ = detail::required(row, "c");
          bar.volume_ = detail::required(row, "v");
          series.bars_.push_back(std::move(bar));
        }
      }
      std::reverse(series.bars_.begin(), series.bars_.end());
      if (!series.ascending()) {
        throw std::runtime_error("the bars are out of order or repeated");
      }
      series.received_at_ = clock_();
      return series;
    });
  }

  /**
   * @brief Returns the US equity positions (`/v2/positions`), in dollars.
   *
   * The sellable figure is `qty_available`, with the basis `{WHOLE_HOLDING, DEDUCT, verified_ = false}`.
   * The read fails (`PROTOCOL`, `invalid_holdings_response`) when a symbol appears twice among all rows, when a row
   * names no symbol or class, and when a `us_equity` row is short or has no readable `qty`: a short equity position
   * anywhere in the account stops the read. Rows of other classes (crypto, options) are skipped.
   *
   * @param account The account (empty).
   * @return The holdings, or an error.
   */
  Result<HoldingsSnapshot> holdings(AlpacaAccount account = {}) {
    static_cast<void>(account);
    return attempt<HoldingsSnapshot>(
      [&] {
        const auto reply = get_json(*trading_, "/v2/positions", network::RateLimitGroup::ACCOUNT);
        if (!reply.is_array()) {
          throw std::runtime_error("the positions are not a list");
        }
        std::set<std::string> seen;
        for (const auto& value : reply.as_array()) {
          const auto* row = value.if_object();
          const auto symbol = row ? detail::text(*row, "symbol") : std::string();
          if (symbol.empty() || detail::text(*row, "asset_class").empty()) {
            throw std::runtime_error("a position names no symbol or asset class");
          }
          if (!seen.insert(symbol).second) {
            throw std::runtime_error("the positions list " + symbol + " twice");
          }
        }
        HoldingsSnapshot snapshot;
        for (const auto& value : reply.as_array()) {
          const auto& row = value.as_object();
          if (detail::text(row, "asset_class") != "us_equity") {
            // Crypto and options: never an instrument this adapter sends.
            continue;
          }
          if (detail::text(row, "side") != "long") {
            throw std::runtime_error("a US equity position is not long: " + detail::text(row, "symbol"));
          }
          HoldingPosition position;
          position.instrument_ = detail::instrument_of(detail::text(row, "symbol"));
          position.quantity_ = detail::required(row, "qty");
          position.sellable_quantity_ = detail::number(row, "qty_available");
          position.average_purchase_price_ = detail::required(row, "avg_entry_price");
          position.last_price_ = detail::required(row, "current_price");
          position.purchase_value_ = Money{detail::required(row, "cost_basis"), Currency::usd()};
          position.market_value_ = Money{detail::required(row, "market_value"), Currency::usd()};
          position.unrealized_profit_loss_ = Money{detail::required(row, "unrealized_pl"), Currency::usd()};
          position.unrealized_return_ = detail::required(row, "unrealized_plpc");
          snapshot.positions_.push_back(std::move(position));
        }
        // **What `qty_available` is evidence of: a treatment, not a measurement**, as KIS states its `ord_psbl_qty`.
        // Alpaca documents it as the shares "available minus open orders", one row per symbol (the duplicate check
        // above enforces that). `DEDUCT` is safe whatever Alpaca does: deducting resting sells Alpaca had already
        // deducted over-refuses a second sell while one rests, where the other error sells the same shares twice.
        // `verified_` stays false: nobody has measured it.
        snapshot.sellable_basis_ =
          SellableBasis{SellableBasis::Coverage::WHOLE_HOLDING, SellableBasis::RestingTreatment::DEDUCT, false};
        snapshot.received_at_ = clock_();
        return snapshot;
      },
      "invalid_holdings_response"
    );
  }

  /**
   * @brief Returns one instrument's reference data (`/v2/assets/{symbol}`), with lot 1, dollars and
   *   `us::equity_price_rule()`.
   *
   * Alpaca names no security type (an ETF and a stock are both `us_equity`), so it is `UNKNOWN`. An asset that is
   * not an active, tradable equity on a US national exchange (OTC included) is refused.
   *
   * @param instrument The instrument.
   * @return The instrument, or an error.
   */
  Result<Instrument> instrument(InstrumentId instrument) {
    if (auto refused = detail::unsupported_instrument(instrument)) {
      return Result<Instrument>(std::move(*refused));
    }
    return attempt<Instrument>([&] {
      const auto reply = get_object(
        *trading_, "/v2/assets/" + broker_detail::query_encode(instrument.symbol_), network::RateLimitGroup::REFERENCE
      );
      if (auto refused = detail::unlisted_asset(reply, instrument.symbol_)) {
        throw Failure{std::move(*refused)};
      }
      Instrument out;
      out.id_ = detail::instrument_of(instrument.symbol_);
      out.name_ = detail::text(reply, "name");
      out.english_name_ = out.name_;
      out.segment_ = market_segment(detail::text(reply, "exchange"));
      out.status_ = InstrumentStatus::ACTIVE;
      out.currency_ = Currency::usd();
      out.lot_size_ = Decimal64::parse("1");
      out.price_rule_ = us::equity_price_rule();
      return out;
    });
  }

  /**
   * @brief Returns the New York Stock Exchange's trading calendar with each open day's session hours, as instants
   *   (`/v3/calendar/XNYS`).
   *
   * Without `from`, it covers 41 days from the UTC date ten days before now, so the last trading day before now and
   * the next ones after it are both listed; with `from`, 41 days from it. A listed date is open, with `regular_` from
   * its core session and `extended_` from its pre-market start to its post-market end when both are stated; an
   * unlisted date is closed. A date listed twice or outside the range fails the read.
   *
   * @param country `Country::US`; any other is refused before sending.
   * @param from The first date, or empty for the default range.
   * @return The calendar, or an error.
   */
  Result<TradingCalendar> calendar(Country country, std::optional<Date> from = std::nullopt) {
    if (country != Country::US) {
      return Result<TradingCalendar>(detail::refusal("market_unsupported", "Alpaca's calendar is the US market's"));
    }
    if (from && !is_valid_date(*from)) {
      return Result<TradingCalendar>(detail::refusal("invalid_date", "the calendar needs a valid date"));
    }
    return attempt<TradingCalendar>(
      [&] {
        const auto first_day = from ?
          detail::days_from_civil(*from) :
          detail::days_from_civil(detail::utc_date(clock_())) - detail::calendar_days_back;
        const auto last_day = first_day + detail::calendar_span_days;
        const auto reply = get_object(
          *trading_,
          "/v3/calendar/XNYS?start=" + detail::iso_date(date_from_epoch_days(first_day)) +
            "&end=" + detail::iso_date(date_from_epoch_days(last_day)) + "&timezone=UTC",
          network::RateLimitGroup::REFERENCE
        );
        const auto* rows = reply.if_contains("calendar");
        if (rows == nullptr || !rows->is_array()) {
          throw std::runtime_error("the calendar has no list of days");
        }
        TradingCalendar calendar;
        calendar.country_ = Country::US;
        std::vector<std::optional<TradingDay>> listed(static_cast<std::size_t>(last_day - first_day + 1));
        for (const auto& value : rows->as_array()) {
          const auto* row = value.if_object();
          const auto date = row ? detail::parse_date(detail::text(*row, "date")) : std::nullopt;
          if (!date) {
            ++calendar.unreadable_days_;
            continue;
          }
          const auto day = detail::days_from_civil(*date);
          if (day < first_day || day > last_day) {
            throw Failure{calendar_error("calendar_out_of_range", "the calendar lists a date outside the range asked")};
          }
          auto& slot = listed[static_cast<std::size_t>(day - first_day)];
          if (slot) {
            throw Failure{
              calendar_error("calendar_duplicate_date", "the calendar lists " + detail::iso_date(*date) + " twice")
            };
          }
          slot = TradingDay{*date, true};
          slot->regular_ = session(*row, "core_start", "core_end");
          if (slot->regular_) {
            const auto span = session(*row, "pre_start", "post_end");
            if (
              span && span->opens_at_ <= slot->regular_->opens_at_ && slot->regular_->closes_at_ <= span->closes_at_
            ) {
              slot->extended_ = span;
            }
          }
        }
        for (auto day = first_day; day <= last_day; ++day) {
          const auto& slot = listed[static_cast<std::size_t>(day - first_day)];
          calendar.days_.push_back(slot ? *slot : TradingDay{date_from_epoch_days(day), false});
        }
        calendar.received_at_ = clock_();
        return calendar;
      },
      "invalid_calendar_response"
    );
  }

  /**
   * @brief Places an order (`POST /v2/orders`). The outcome can be unknown after an error.
   *
   * Before anything is sent, it refuses what `detail::validate_order` refuses; then it reads the asset
   * (`instrument_not_listed` for OTC or an exchange not known to be national, `instrument_not_tradable`), for a sell
   * the position (`no_position`, `sell_exceeds_position` above `qty_available` or `qty`), and last Alpaca's clock for
   * the New York Stock Exchange: unless its phase is `core`, the order is not sent (`market_closed`, `CANCELLED`,
   * `reached_broker_` false), since Alpaca would queue a day order for the next session. A failed read refuses the
   * order with the read's own category, not sent. The clock covers market-wide state (an unscheduled closure), not a single stock's halt, and
   * a fill between a read and the order is not seen.
   *
   * Alpaca's refusals seen on the paper host (422 `42210000`, 403 `40310000`) are `INVALID_ARGUMENT`; any other
   * failure leaves the outcome unknown (`PROTOCOL`, or `HTTP` for 5xx, or the transport's), to be read back.
   *
   * @param account The account (empty).
   * @param order The order.
   * @return The acknowledgement, or an error.
   */
  Result<AlpacaOrderAck> place_order(AlpacaAccount account, OrderRequest order) {
    static_cast<void>(account);
    if (auto refused = detail::validate_order(order)) {
      return Result<AlpacaOrderAck>(std::move(*refused));
    }
    if (auto refused = refuse_before_order(order)) {
      return Result<AlpacaOrderAck>(std::move(*refused));
    }
    json::object body;
    body["symbol"] = order.instrument_.symbol_;
    body["qty"] = decimal_normalized(order.quantity_).str();
    body["side"] = order.side_ == OrderSide::BUY ? "buy" : "sell";
    body["type"] = order.type_ == OrderType::LIMIT ? "limit" : "market";
    body["time_in_force"] = "day";
    if (order.type_ == OrderType::LIMIT) {
      body["limit_price"] = decimal_normalized(*order.price_).str();
    }
    if (order.client_order_id_) {
      body["client_order_id"] = *order.client_order_id_;
    }
    return send_order(http::verb::post, "/v2/orders", json::serialize(body), Call::PLACE, std::string());
  }

  /**
   * @brief Cancels a whole order (`DELETE /v2/orders/{id}`; 204, also for an order already cancelled). The outcome
   *   can be unknown after an error.
   * @param account The account (empty).
   * @param amendment The order.
   * @return The acknowledgement (the same id), or an error.
   */
  Result<AlpacaOrderAck> cancel_order(AlpacaAccount account, AlpacaAmendment amendment) {
    static_cast<void>(account);
    if (!detail::valid_order_id(amendment.order_id_)) {
      return Result<AlpacaOrderAck>(detail::invalid_order_id());
    }
    return send_order(
      http::verb::delete_, "/v2/orders/" + broker_detail::query_encode(amendment.order_id_), std::string(),
      Call::CANCEL, amendment.order_id_
    );
  }

  /**
   * @brief Returns the open US equity orders (`/v2/orders?status=open`, up to 499).
   *
   * The listing fails when an order id appears twice among all rows, when 500 rows come back (it may be cut), and
   * when a `us_equity` row's side or quantity cannot be read; rows of other classes are skipped.
   *
   * @param account The account (empty).
   * @return The orders, or an error.
   */
  Result<AlpacaOpenOrders> working_orders(AlpacaAccount account = {}) {
    static_cast<void>(account);
    return attempt<AlpacaOpenOrders>(
      [&] {
        const auto reply =
          get_json(*trading_, "/v2/orders?status=open&limit=500&direction=asc", network::RateLimitGroup::ACCOUNT);
        if (!reply.is_array()) {
          throw std::runtime_error("the orders are not a list");
        }
        if (reply.as_array().size() >= 500) {
          throw std::runtime_error("Alpaca listed 500 open orders: the listing may be cut");
        }
        AlpacaOpenOrders open;
        open.orders_ = orders_of(reply.as_array());
        open.received_at_ = clock_();
        return open;
      },
      "invalid_orders_response"
    );
  }

  /**
   * @brief Returns one order by its id (`/v2/orders/{id}`); an order that is not a US equity is a `PROTOCOL` error.
   * @param account The account (empty).
   * @param id The order id.
   * @return The order, or an error.
   */
  Result<OrderSnapshot> order_by_id(AlpacaAccount account, const std::string& id) {
    static_cast<void>(account);
    if (!detail::valid_order_id(id)) {
      return Result<OrderSnapshot>(detail::invalid_order_id());
    }
    return attempt<OrderSnapshot>(
      [&] {
        const auto row =
          get_object(*trading_, "/v2/orders/" + broker_detail::query_encode(id), network::RateLimitGroup::ACCOUNT);
        if (detail::text(row, "asset_class") != "us_equity") {
          throw std::runtime_error("the order is not a US equity order");
        }
        return order_of(row);
      },
      "invalid_order_response"
    );
  }

  /**
   * @brief Returns US equity orders submitted from `from_` to `to_` (New York dates), newest first, up to `limit_`
   *   (500 by default) rows.
   *
   * A full page carries a cursor, the last row's `submitted_at`, sent back as Alpaca's exclusive `until`. The page
   * fails as `working_orders` does; skipped rows still count toward the page.
   *
   * @param account The account (empty).
   * @param query The query.
   * @return The page, or an error.
   */
  Result<OrderPage> order_history(AlpacaAccount account, OrderHistoryQuery query) {
    static_cast<void>(account);
    return attempt<OrderPage>(
      [&] {
        const int limit = query.limit_ ? *query.limit_ : 500;
        std::string target = "/v2/orders?status=all&direction=desc&limit=" + std::to_string(limit);
        if (query.symbol_) {
          target += "&symbols=" + broker_detail::query_encode(*query.symbol_);
        }
        if (query.from_) {
          target += "&after=" + format_iso8601_utc(detail::eastern_midnight(*query.from_) - std::chrono::seconds(1));
        }
        if (query.cursor_) {
          target += "&until=" + broker_detail::query_encode(*query.cursor_);
        } else if (query.to_) {
          target += "&until=" + format_iso8601_utc(detail::eastern_midnight(*query.to_) + std::chrono::hours(24));
        }
        const auto reply = get_json(*trading_, target, network::RateLimitGroup::ACCOUNT);
        if (!reply.is_array()) {
          throw std::runtime_error("the orders are not a list");
        }
        OrderPage page;
        page.orders_ = orders_of(reply.as_array());
        if (limit > 0 && static_cast<int>(reply.as_array().size()) == limit) {
          const auto& last = reply.as_array().back().as_object();
          const auto submitted = detail::text(last, "submitted_at");
          if (submitted.empty()) {
            throw std::runtime_error("an order has no submission time: the next page cannot be asked");
          }
          page.next_cursor_ = submitted;
        }
        page.received_at_ = clock_();
        return page;
      },
      "invalid_orders_response"
    );
  }

private:
  friend struct AlpacaClientTestAccess;

  /** @brief A failure carried out of a reader. */
  struct Failure {
    /** @brief The error. */
    BrokerError error_;
  };

  /** @brief What a reply answers, which decides how its failure reads. */
  enum class Call {
    /** @brief Any read. */
    READ,
    /** @brief A placement. */
    PLACE,
    /** @brief A cancellation. */
    CANCEL,
  };

  // Alpaca's timeframe for an interval `validate_bar_query` admits.
  static std::string timeframe(BarInterval interval) {
    switch (interval) {
      case BarInterval::ONE_MINUTE:
        return "1Min";
      case BarInterval::FIVE_MINUTES:
        return "5Min";
      case BarInterval::FIFTEEN_MINUTES:
        return "15Min";
      case BarInterval::THIRTY_MINUTES:
        return "30Min";
      case BarInterval::ONE_HOUR:
        return "1Hour";
      case BarInterval::ONE_DAY:
      case BarInterval::ONE_WEEK:
      case BarInterval::ONE_MONTH:
      case BarInterval::ONE_YEAR:
        break;
    }
    return "1Day";
  }

  // A calendar reply that cannot be a calendar.
  static BrokerError calendar_error(std::string code, std::string message) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = std::move(code);
    error.message_ = "Alpaca: " + std::move(message);
    return error;
  }

  // A session's bounds from two RFC 3339 members, or none when either is missing or unreadable or they are not in
  // order.
  static std::optional<SessionHours> session(const json::object& row, const char* opens, const char* closes) {
    const auto start = parse_iso8601_timestamp(detail::text(row, opens));
    const auto end = parse_iso8601_timestamp(detail::text(row, closes));
    if (!start || !end || !(*start < *end)) {
      return std::nullopt;
    }
    return SessionHours{*start, *end};
  }

  // The reads made before an order is sent: the asset, the position for a sell, and the clock last.
  std::optional<BrokerError> refuse_before_order(const OrderRequest& order) {
    const auto& symbol = order.instrument_.symbol_;
    const auto unsent = [](BrokerError error, const char* what) {
      error.reached_broker_ = false;
      error.message_ = std::string(what) + " could not be read before the order, so it was not sent: " + error.message_;
      return error;
    };
    auto asset = send(
      *trading_, http::verb::get, "/v2/assets/" + broker_detail::query_encode(symbol), {},
      network::RateLimitGroup::ACCOUNT
    );
    if (asset && asset.value().result_int() == 404) {
      return detail::refusal("instrument_not_listed", "Alpaca lists no asset " + symbol);
    }
    auto asset_reply = read_object(std::move(asset));
    if (!asset_reply) {
      return unsent(std::move(asset_reply.error()), "the asset");
    }
    if (auto refused = detail::unlisted_asset(asset_reply.value(), symbol)) {
      return refused;
    }
    if (order.side_ == OrderSide::SELL) {
      auto position = send(
        *trading_, http::verb::get, "/v2/positions/" + broker_detail::query_encode(symbol), {},
        network::RateLimitGroup::ACCOUNT
      );
      if (position && position.value().result_int() == 404) {
        return detail::refusal("no_position", "no position in " + symbol + ": a sell would open a short");
      }
      auto position_reply = read_object(std::move(position));
      if (!position_reply) {
        return unsent(std::move(position_reply.error()), "the position");
      }
      try {
        const auto& row = position_reply.value();
        if (detail::text(row, "symbol") != symbol || detail::text(row, "asset_class") != "us_equity") {
          BrokerError error;
          error.category_ = ErrorCategory::PROTOCOL;
          error.broker_code_ = "position_mismatch";
          error.message_ = "Alpaca's position reply is not the US equity position in " + symbol;
          return unsent(std::move(error), "the position");
        }
        const auto available = detail::number(row, "qty_available");
        const auto held = detail::required(row, "qty");
        if (
          detail::text(row, "side") != "long" || !available || !detail::positive(held) ||
          decimal_less(*available, order.quantity_) || decimal_less(held, order.quantity_)
        ) {
          return detail::refusal(
            "sell_exceeds_position", "the sell exceeds the position available in " + symbol + ": it would open a short"
          );
        }
      } catch (const std::exception& exception) {
        BrokerError error;
        error.category_ = ErrorCategory::PROTOCOL;
        error.broker_code_ = "invalid_position_response";
        error.message_ = exception.what();
        return unsent(std::move(error), "the position");
      }
    }
    return refuse_unless_core();
  }

  // Reads Alpaca's clock for the New York Stock Exchange and refuses unless its phase is `core`.
  std::optional<BrokerError> refuse_unless_core() {
    auto reply =
      read_object(send(*trading_, http::verb::get, "/v3/clock?markets=XNYS", {}, network::RateLimitGroup::ACCOUNT));
    BrokerError error;
    error.reached_broker_ = false;
    if (!reply) {
      error = std::move(reply.error());
      error.reached_broker_ = false;
      error.message_ = "Alpaca's clock could not be read before the order, so it was not sent: " + error.message_;
      return error;
    }
    const json::object* clock = nullptr;
    int matches = 0;
    if (const auto* clocks = reply.value().if_contains("clocks"); clocks != nullptr && clocks->is_array()) {
      for (const auto& value : clocks->as_array()) {
        const auto* entry = value.if_object();
        const auto* market = entry ? entry->if_contains("market") : nullptr;
        if (market != nullptr && market->is_object() && detail::text(market->as_object(), "mic") == "XNYS") {
          clock = entry;
          ++matches;
        }
      }
    }
    const auto phase = clock ? detail::text(*clock, "phase") : std::string();
    if (matches != 1 || phase.empty()) {
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "clock_unreadable";
      error.message_ = "Alpaca's clock did not give one phase for XNYS, so the order was not sent";
      return error;
    }
    if (phase != "core") {
      // Kept by a runner (a category it retries when nothing was sent), so a flatten refused here is offered again.
      error.category_ = ErrorCategory::CANCELLED;
      error.broker_code_ = "market_closed";
      error.message_ = "Alpaca's clock says the regular session is not on (phase " + phase + " until " +
        detail::text(*clock, "phase_until") +
        "): the order was not sent, since Alpaca would queue it for the next session";
      return error;
    }
    return std::nullopt;
  }

  // Sends an order call; never sent again. A seen refusal is `INVALID_ARGUMENT`; anything else unknown.
  Result<AlpacaOrderAck> send_order(
    http::verb method, const std::string& target, std::string body, Call call, const std::string& asked_id
  ) {
    auto reply = send(*trading_, method, target, std::move(body), network::RateLimitGroup::TRADING);
    if (!reply) {
      return Result<AlpacaOrderAck>(std::move(reply.error()));
    }
    const auto& response = reply.value();
    const auto status = response.result_int();
    AlpacaOrderAck ack;
    ack.received_at_ = clock_();
    if (call == Call::CANCEL && status >= 200 && status < 300) {
      ack.order_id_ = asked_id;
      return Result<AlpacaOrderAck>(std::move(ack));
    }
    if (status < 200 || status >= 300) {
      return Result<AlpacaOrderAck>(reply_error(response, call));
    }
    BrokerError error;
    auto row = broker_detail::parse_object(response, error);
    if (!row) {
      return Result<AlpacaOrderAck>(std::move(error));
    }
    ack.order_id_ = detail::text(*row, "id");
    if (ack.order_id_.empty()) {
      error.category_ = ErrorCategory::PROTOCOL;
      error.http_status_ = static_cast<int>(status);
      error.broker_code_ = "missing_order_id";
      error.message_ = "Alpaca accepted an order and gave no id";
      return Result<AlpacaOrderAck>(std::move(error));
    }
    if (auto key = detail::text(*row, "client_order_id"); !key.empty()) {
      ack.client_order_id_ = std::move(key);
    }
    return Result<AlpacaOrderAck>(std::move(ack));
  }

  // The error a reply with a failure status carries.
  BrokerError reply_error(const network::HttpResponse& response, Call call) const {
    BrokerError error;
    const auto status = static_cast<int>(response.result_int());
    error.http_status_ = status;
    boost::system::error_code ec;
    const auto value = json::parse(response.body(), ec);
    const auto* object = ec ? nullptr : value.if_object();
    if (object != nullptr) {
      error.message_ = "Alpaca: " + detail::text(*object, "message");
      // Read without a throwing conversion: a code that is not a whole number is left out.
      if (const auto* code = object->if_contains("code"); code != nullptr && code->is_int64()) {
        error.broker_code_ = std::to_string(code->as_int64());
      } else if (code != nullptr && code->is_uint64()) {
        error.broker_code_ = std::to_string(code->as_uint64());
      }
    } else {
      error.message_ = "Alpaca answered HTTP " + std::to_string(status) + " with a body that is not JSON";
    }
    if (status == 401) {
      // The gateway's refusal of the keys, before any route (seen on every host).
      error.category_ = ErrorCategory::AUTHENTICATION;
    } else if (status == 429 && call == Call::READ) {
      error.category_ = ErrorCategory::RATE_LIMIT;
      if (const auto reset = broker_detail::integer_header(response, "X-RateLimit-Reset")) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(clock_().time_since_epoch()).count();
        error.retry_after_ = std::chrono::seconds(std::max<std::int64_t>(1, *reset - now));
      }
    } else if (call == Call::READ || status >= 500) {
      error.category_ = ErrorCategory::HTTP;
    } else if (
      (call == Call::PLACE &&
        ((status == 422 && error.broker_code_ == "42210000") || (status == 403 && error.broker_code_ == "40310000"))) ||
      (call == Call::CANCEL && status == 422 && error.broker_code_ == "42210000")
    ) {
      // Refusals seen on the paper host: the order was not placed, or not cancelled.
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
    } else {
      // A code not seen on an order call says nothing of the order (40010001 is also a duplicate client_order_id,
      // which says the order exists): read it back.
      error.category_ = ErrorCategory::PROTOCOL;
    }
    return error;
  }

  // Runs a reader; any failure is the library's error.
  template <typename T, typename Make>
  Result<T> attempt(Make make, const char* code = "invalid_response") {
    try {
      return Result<T>(make());
    } catch (const Failure& failure) {
      return Result<T>(failure.error_);
    } catch (const std::exception& exception) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = code;
      error.message_ = std::string("Alpaca: ") + exception.what();
      return Result<T>(std::move(error));
    }
  }

  // A successful reply's body as JSON, or the error a failed one carries.
  Result<json::value> read_json(Result<network::HttpResponse> reply) const {
    if (!reply) {
      return Result<json::value>(std::move(reply.error()));
    }
    const auto& response = reply.value();
    if (response.result_int() < 200 || response.result_int() >= 300) {
      return Result<json::value>(reply_error(response, Call::READ));
    }
    boost::system::error_code ec;
    auto value = json::parse(response.body(), ec);
    if (ec) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.http_status_ = static_cast<int>(response.result_int());
      error.broker_code_ = "invalid_json";
      error.message_ = "Alpaca's reply is not JSON";
      return Result<json::value>(std::move(error));
    }
    return Result<json::value>(std::move(value));
  }

  // A successful reply's body as a JSON object, or the error.
  Result<json::object> read_object(Result<network::HttpResponse> reply) const {
    auto value = read_json(std::move(reply));
    if (!value) {
      return Result<json::object>(std::move(value.error()));
    }
    if (!value.value().is_object()) {
      BrokerError error;
      error.category_ = ErrorCategory::PROTOCOL;
      error.broker_code_ = "invalid_json";
      error.message_ = "Alpaca's reply is not a JSON object";
      return Result<json::object>(std::move(error));
    }
    return Result<json::object>(std::move(value.value().as_object()));
  }

  // GETs JSON, throwing the failure.
  json::value get_json(network::AsyncHttpClient& http, const std::string& target, network::RateLimitGroup group) {
    auto value = read_json(send(http, http::verb::get, target, {}, group));
    if (!value) {
      throw Failure{std::move(value.error())};
    }
    return std::move(value.value());
  }

  // GETs a JSON object, throwing the failure.
  json::object get_object(network::AsyncHttpClient& http, const std::string& target, network::RateLimitGroup group) {
    auto value = read_object(send(http, http::verb::get, target, {}, group));
    if (!value) {
      throw Failure{std::move(value.error())};
    }
    return std::move(value.value());
  }

  // The request with both keys; every request is built here.
  network::HttpRequest make_request(
    http::verb method, std::string target, std::string body, network::RateLimitGroup group
  ) const {
    network::HttpRequest request;
    request.method_ = method;
    request.target_ = std::move(target);
    request.headers_ = {{"APCA-API-KEY-ID", config_.key()}, {"APCA-API-SECRET-KEY", config_.secret()}};
    if (!body.empty()) {
      request.headers_.emplace_back("Content-Type", "application/json");
    }
    request.body_ = std::move(body);
    request.group_ = group;
    return request;
  }

  // Sends one request and runs the private loop until it completes.
  Result<network::HttpResponse> send(
    network::AsyncHttpClient& http, http::verb method, std::string target, std::string body,
    network::RateLimitGroup group
  ) {
    std::optional<Result<network::HttpResponse>> result;
    ioc_.restart();
    http.async_request(
      make_request(method, std::move(target), std::move(body), group), [&result](Result<network::HttpResponse> value) {
        result.emplace(std::move(value));
      }
    );
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "the event loop stopped before the request completed";
      return Result<network::HttpResponse>(std::move(error));
    }
    return std::move(*result);
  }

  // The US equity orders among rows, after checking that no id appears twice among all of them.
  static std::vector<OrderSnapshot> orders_of(const json::array& rows) {
    std::set<std::string> seen;
    for (const auto& value : rows) {
      const auto* row = value.if_object();
      const auto id = row ? detail::text(*row, "id") : std::string();
      // A multi-leg option order (`order_class` mleg) states no asset class; it is skipped below with the others.
      if (id.empty() || (detail::text(*row, "asset_class").empty() && detail::text(*row, "order_class") != "mleg")) {
        throw std::runtime_error("an order has no id or asset class");
      }
      if (!seen.insert(id).second) {
        throw std::runtime_error("the orders list " + id + " twice");
      }
    }
    std::vector<OrderSnapshot> orders;
    for (const auto& value : rows) {
      const auto& row = value.as_object();
      if (detail::text(row, "asset_class") != "us_equity") {
        // Crypto and options: never an order this adapter sends.
        continue;
      }
      orders.push_back(order_of(row));
    }
    return orders;
  }

  // One order from Alpaca's order object.
  static OrderSnapshot order_of(const json::object& row) {
    OrderSnapshot order;
    order.order_id_ = detail::text(row, "id");
    if (order.order_id_.empty()) {
      throw std::runtime_error("an order has no id");
    }
    order.instrument_ = detail::instrument_of(detail::text(row, "symbol"));
    const auto side = detail::text(row, "side");
    if (side != "buy" && side != "sell") {
      throw std::runtime_error("an order's side cannot be read");
    }
    order.side_ = side == "buy" ? OrderSide::BUY : OrderSide::SELL;
    const auto type = detail::text(row, "type");
    order.type_ = type == "limit" ?
      OrderType::LIMIT :
      type == "market" ?
      OrderType::MARKET :
      type == "stop_limit" ?
      OrderType::STOP_LIMIT :
      OrderType::UNKNOWN;
    const auto condition = detail::text(row, "time_in_force");
    order.time_in_force_ = condition == "day" ?
      TimeInForce::DAY :
      condition == "ioc" ?
      TimeInForce::IMMEDIATE_OR_CANCEL :
      condition == "fok" ?
      TimeInForce::FILL_OR_KILL :
      TimeInForce::UNKNOWN;
    const auto* extended = row.if_contains("extended_hours");
    order.session_ =
      extended != nullptr && extended->is_bool() && extended->as_bool() ? OrderSession::UNKNOWN : OrderSession::REGULAR;
    order.status_ = detail::order_status(detail::text(row, "status"));
    order.price_ = detail::number(row, "limit_price");
    order.trigger_price_ = detail::number(row, "stop_price");
    order.quantity_ = detail::required(row, "qty");
    order.currency_ = Currency::usd();
    order.ordered_at_ = detail::instant(row, "submitted_at");
    order.canceled_at_ = detail::instant(row, "canceled_at");
    order.execution_.filled_quantity_ = detail::required(row, "filled_qty");
    order.execution_.average_filled_price_ = detail::number(row, "filled_avg_price");
    order.execution_.filled_at_ = detail::instant(row, "filled_at");
    return order;
  }

  AlpacaConfig config_;
  net::io_context ioc_;
  std::shared_ptr<network::AsyncHttpClient> trading_;
  std::shared_ptr<network::AsyncHttpClient> data_;
  std::function<std::chrono::system_clock::time_point()> clock_{[] {
    return std::chrono::system_clock::now();
  }};
};

} // namespace algotradelib::alpaca
