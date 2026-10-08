#pragma once

#include "../../network/oauth.hpp"

#include "../../core/types.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {
namespace kiwoom {

// https://openapi.kiwoom.com/guide/apiguide
// Token issue:  au10001  POST https://api.kiwoom.com/oauth2/token
// Token revoke: au10002  POST https://api.kiwoom.com/oauth2/revoke
struct KiwoomConfig;
struct KiwoomRest;

/**
 * @brief Which venue's prices Kiwoom market-data requests read (see `KiwoomConfig::market_tape_`).
 *
 * Kiwoom encodes it as a suffix on the symbol: `005930` is KRX, `005930_NX` NXT, `005930_AL` both
 * combined (SOR).
 */
enum class MarketTape {
  /** @brief KRX and NXT combined, the SOR book (`_AL`). */
  CONSOLIDATED,
  /** @brief KRX alone (no suffix). */
  KRX,
  /** @brief NXT alone (`_NX`). */
  NXT,
};

/**
 * @brief Returns the symbol suffix for a tape.
 * @param tape The tape.
 * @return `""` for KRX, `"_NX"` or `"_AL"`.
 */
inline constexpr const char* tape_suffix(MarketTape tape) noexcept {
  switch (tape) {
    case MarketTape::KRX:
      return "";
    case MarketTape::NXT:
      return "_NX";
    case MarketTape::CONSOLIDATED:
      return "_AL";
  }
  return "_AL";
}

/**
 * @brief Reads Kiwoom's token expiry, `yyyyMMddHHmmss` in Korean time.
 * @param text The text.
 * @return The instant, or the epoch if the text is not fourteen digits (out-of-range fields are not
 *   checked: they normalize into some instant).
 */
inline std::chrono::system_clock::time_point parse_compact_expiry(std::string_view text) {
  if (text.size() != 14) {
    return {};
  }
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return {};
    }
  }
  const auto number = [text](std::size_t offset, std::size_t length) {
    int value = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
      value = value * 10 + (text[index] - '0');
    }
    return value;
  };
  std::tm parts{};
  parts.tm_year = number(0, 4) - 1900;
  parts.tm_mon = number(4, 2) - 1;
  parts.tm_mday = number(6, 2);
  parts.tm_hour = number(8, 2);
  parts.tm_min = number(10, 2);
  parts.tm_sec = number(12, 2);
  parts.tm_isdst = 0;
#ifdef _WIN32
  const std::time_t utc = _mkgmtime(&parts) - 9 * 3600;
#else
  const std::time_t utc = timegm(&parts) - 9 * 3600;
#endif
  return std::chrono::system_clock::from_time_t(utc);
}

/**
 * @brief What the venue named on a Kiwoom holdings request (`kt00018`'s `dmst_stex_tp`) means for
 *   the positions it lists; Kiwoom does not document it, so you assert it (see
 *   `KiwoomConfig::holdings_position_scope_`).
 */
enum class KiwoomPositionScope {
  /**
   * @brief Not stated: every new order and amendment is refused (cancellations are not), and the
   *   session's holdings verb refuses (the client's own holdings request still answers).
   */
  UNSPECIFIED,
  /**
   * @brief Asserted: the venue only selects which venue's price values each position, and every
   *   position is listed (the more plausible reading, but unverified).
   */
  VALUATION_ONLY,
};

/**
 * @brief What a Kiwoom holdings row's tradable quantity (`trde_able_qty`) covers; undocumented, so
 *   you assert it (see `KiwoomConfig::holdings_sellable_scope_`).
 */
enum class KiwoomSellableScope {
  /** @brief Not stated: the sellable basis is unestablished, and a sell checked against it is refused. */
  UNSPECIFIED,
  /** @brief Asserted: it covers every share held of the instrument, on any venue. */
  WHOLE_HOLDING,
};

/**
 * @brief Kiwoom credentials (app key and secret key), demo or real account, the market-data tape,
 *   and the holdings settings an account must state before trading.
 *
 * Load with `from_env()` (`KIWOOM_APP_KEY`, `KIWOOM_SECRET_KEY`, `KIWOOM_DEMO`) or
 * `from_json(filename)`. Holdings cannot be read until `holdings_venue_` is set. Orders cannot be
 * placed or amended, and the session (so a runner) reads no holdings, until `holdings_position_scope_`
 * is set; a checked sell also needs `holdings_sellable_scope_`.
 */
struct KiwoomConfig : public network::ClientCredentials<KiwoomConfig> {
  /**
   * @brief Whether to use Kiwoom's demo (모의투자) endpoints; **`true` unless stated otherwise**.
   *
   * Real trading must be stated: `KIWOOM_DEMO=0`, `"demo": false` in the credential file, or
   * `is_demo_ = false`. Real-account keys sent to the demo endpoints are refused at the first token
   * request, so a forgotten setting fails rather than trades.
   */
  bool is_demo_ = true;

  /**
   * @brief Which venue's prices every Kiwoom market-data request reads (quotes, depth, trades, price
   *   bands, bars); KRX and NXT combined by default.
   *
   * It does not change the instrument's identity or where orders go.
   */
  MarketTape market_tape_ = MarketTape::CONSOLIDATED;

  /**
   * @brief The venue holdings requests (`kt00018`) name: `Venue::KRX` or `Venue::NXT`. **No
   *   default: holdings requests are refused until it is set.**
   *
   * `kt00018` has no all-venue value, and the library does not request both and merge them: if the
   * venue only selects the valuation price, merging would double every position. If instead it
   * filters positions, one venue's request omits what is held on the other, so a position ceiling
   * would see too little. Which it is, Kiwoom does not document; see `holdings_position_scope_`.
   * `Venue::COMBINED` is refused.
   */
  Venue holdings_venue_ = Venue::UNSPECIFIED;

  /**
   * @brief Your assertion of what `holdings_venue_` means for the positions listed. **No default:
   *   until it is set, every new order and amendment is refused and the session reads no holdings**
   *   (cancellations, including `cancel_quantity()`, still go).
   *
   * Kiwoom does not document it, and two reads cannot settle it (the account can change between
   * them). A wrong assertion undercounts the account: a buy can pass a ceiling already reached, and
   * a sell can be refused or sized too small.
   */
  KiwoomPositionScope holdings_position_scope_ = KiwoomPositionScope::UNSPECIFIED;

  /**
   * @brief Your assertion of what a holdings row's tradable quantity covers; separate from the
   *   position scope, since Kiwoom documents neither.
   *
   * `UNSPECIFIED` (the default) leaves the sellable basis unestablished, so `RiskGuard` refuses a
   * checked sell. `WHOLE_HOLDING` supplies the coverage; resting sells are deducted either way.
   */
  KiwoomSellableScope holdings_sellable_scope_ = KiwoomSellableScope::UNSPECIFIED;

  /**
   * @brief Reads the credentials from `KIWOOM_APP_KEY` and `KIWOOM_SECRET_KEY`, and the environment
   *   from `KIWOOM_DEMO` (see `demo_from_env`); called by `from_env()`. Nothing changes if it throws.
   * @throws std::runtime_error if a key variable is missing or `KIWOOM_DEMO` is invalid.
   */
  void from_env_impl() {
    const char* key = std::getenv("KIWOOM_APP_KEY");
    const char* secret = std::getenv("KIWOOM_SECRET_KEY");
    if (!key || !secret) {
      throw std::runtime_error("Add KIWOOM_APP_KEY and KIWOOM_SECRET_KEY environment variables.");
    }
    // Read before anything is assigned, so a refused load leaves this config as it was.
    const bool demo = demo_from_env();
    key_ = key;
    secret_ = secret;
    is_demo_ = demo;
  }

  /**
   * @brief Reads the credentials from a JSON file; called by `from_json(filename)`. Nothing changes
   *   if it throws.
   *
   * Format: `{ "appkey": "...", "secretkey": "...", "demo": false }` (Kiwoom's own field names);
   * without `"demo"` the config is demo.
   *
   * @param filename The file.
   * @throws std::runtime_error if the file cannot be read or parsed, a key is missing or not a
   *   string, or `"demo"` is not a boolean.
   */
  void from_json_impl(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Kiwoom credential file: " + filename);
    }
    const std::string content(
      std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{}
    );
    boost::system::error_code ec;
    const auto jv = json::parse(content, ec);
    if (ec || !jv.is_object()) {
      throw std::runtime_error(
        "Failed to parse Kiwoom credentials file: " + filename +
        (ec ? " — " + ec.message() : std::string(" — not a JSON object"))
      );
    }
    const auto& doc = jv.as_object();
    const json::value* key = doc.if_contains("appkey");
    const json::value* secret = doc.if_contains("secretkey");
    if (!key || !secret || !key->is_string() || !secret->is_string()) {
      throw std::runtime_error("Credential file should contain appkey and secretkey as strings");
    }
    // Everything is read and checked before anything is assigned, so a refused file leaves this
    // config -- and the endpoint it already selected -- exactly as it was.
    const bool demo = demo_from_json(doc, filename);
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
    is_demo_ = demo;
  }

  /**
   * @brief Reads `KIWOOM_DEMO`.
   * @return `true` (demo) if unset or `1`, `false` (real) if `0`.
   * @throws std::runtime_error for any other value.
   */
  static bool demo_from_env() {
    const char* demo = std::getenv("KIWOOM_DEMO");
    if (!demo || std::string_view(demo) == "1") {
      return true;
    }
    if (std::string_view(demo) == "0") {
      return false;
    }
    throw std::runtime_error(
      std::string("KIWOOM_DEMO is \"") + demo + "\"; set it to 1 for the demo endpoints or 0 to trade "
      "live, or leave it unset for demo."
    );
  }

  /**
   * @brief Reads a credential file's `"demo"`.
   * @param doc The file's contents.
   * @param filename The file, for the error message.
   * @return `true` if absent, otherwise its value.
   * @throws std::runtime_error if it is not a boolean.
   */
  static bool demo_from_json(const json::object& doc, const std::string& filename) {
    const json::value* demo = doc.if_contains("demo");
    if (!demo) {
      return true;
    }
    if (!demo->is_bool()) {
      throw std::runtime_error(
        "Kiwoom credential file " + filename + " has a \"demo\" that is not true or false; write "
        "true for the demo endpoints or false to trade live, or leave it out for demo."
      );
    }
    return demo->get_bool();
  }

  /**
   * @brief The JSON field Kiwoom expects the application key under.
   * @return `"appkey"`.
   */
  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  /**
   * @brief The JSON field Kiwoom expects the application secret under.
   * @return `"secretkey"`.
   */
  static constexpr std::string_view secret_field() noexcept { return "secretkey"; }

  /**
   * @brief The REST host for the selected environment.
   * @return `"mockapi.kiwoom.com"` (demo) or `"api.kiwoom.com"`.
   */
  const char* rest_host() const noexcept {
    return is_demo_ ? "mockapi.kiwoom.com" : "api.kiwoom.com";
  }

  /**
   * @brief The REST port, which is the same for both environments.
   * @return `"443"`.
   */
  const char* rest_port() const noexcept { return "443"; }

  /**
   * @brief The WebSocket host for the selected environment.
   * @return `"mockapi.kiwoom.com"` (demo) or `"api.kiwoom.com"`.
   */
  const char* ws_host() const noexcept {
    return is_demo_ ? "mockapi.kiwoom.com" : "api.kiwoom.com";
  }

  /**
   * @brief The WebSocket port, which is the same for both environments.
   * @return `"10000"`.
   */
  const char* ws_port() const noexcept { return "10000"; }

  /**
   * @brief Returns a symbol with this configuration's tape suffix, for a market-data request.
   * @param symbol The six-digit symbol.
   * @return The suffixed symbol.
   */
  std::string tape_symbol(std::string_view symbol) const {
    return std::string(symbol) + tape_suffix(market_tape_);
  }
};

/** @brief A Kiwoom REST access token (`au10001`'s `token`) and its expiry (`expires_dt`). */
struct KiwoomRest : public network::AuthBody<KiwoomRest> {
  /** @brief Kiwoom's `token_type` (`bearer` in its example). */
  std::string token_type_;

  /**
   * @brief The field Kiwoom returns the bearer token under.
   * @return `"token"`.
   */
  static constexpr std::string_view token_field() noexcept { return "token"; }

  /**
   * @brief Reads the expiry (`expires_dt`, the only one Kiwoom states) and `token_type`; called by
   *   `from_json`. A response with no readable expiry makes `from_json` throw.
   * @param doc The response.
   */
  void from_json_impl(const json::object& doc) {
    if (const auto expiry = doc.if_contains("expires_dt"); expiry && expiry->is_string()) {
      expires_at_ = parse_compact_expiry(std::string_view(expiry->as_string()));
    }
    if (const auto type = doc.if_contains("token_type"); type && type->is_string()) {
      token_type_ = type->as_string().c_str();
    }
  }
};

} // namespace kiwoom
} // namespace algotradelib
