#pragma once

#include "./network.hpp"
#include <string>
#include <chrono>
#include <ctime>
#include <boost/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace algotradelib {
namespace kis {

// https://apiportal.koreainvestment.com/apiservice-apiservice?/oauth2/tokenP
// Real-time(websocket):
// https://apiportal.koreainvestment.com/apiservice-apiservice?/oauth2/Approval
// https://github.com/koreainvestment/open-trading-api/blob/main/examples_user/auth/auth_examples.py
// https://github.com/koreainvestment/open-trading-api/blob/main/examples_user/auth/auth_functions.py
struct KisConfig;

/** @brief Which venue's prices KIS market-data requests read (see `KisConfig::market_tape_`). */
enum class MarketTape {
  /** @brief KRX and NXT combined (`UN`). */
  CONSOLIDATED,
  /** @brief KRX alone (`J`). */
  KRX,
  /** @brief NXT alone (`NX`). */
  NXT,
};

/**
 * @brief Returns KIS's `FID_COND_MRKT_DIV_CODE` for a tape.
 * @param tape The tape.
 * @return `"UN"`, `"J"` or `"NX"`.
 */
inline constexpr const char* market_division_code(MarketTape tape) noexcept {
  switch (tape) {
    case MarketTape::KRX:
      return "J";
    case MarketTape::NXT:
      return "NX";
    case MarketTape::CONSOLIDATED:
      return "UN";
  }
  return "UN";
}
struct KisRest;
struct KisWebsocket;

/**
 * @brief Reads KIS's token expiry, `YYYY-MM-DD HH:MM:SS` in Korean time.
 * @param s The text.
 * @return The instant, or the epoch if the text does not have that form.
 */
static inline std::chrono::system_clock::time_point parse_expiry(const char* s) {
  std::tm tm{};
  if (
    std::sscanf(
      s,
      "%d-%d-%d %d:%d:%d",
      &tm.tm_year,
      &tm.tm_mon,
      &tm.tm_mday,
      &tm.tm_hour,
      &tm.tm_min,
      &tm.tm_sec
    ) != 6
  ) {
    return {};
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = 0;
#ifdef _WIN32
  const std::time_t utc = _mkgmtime(&tm) - 9 * 3600;
#else
  const std::time_t utc = timegm(&tm) - 9 * 3600;
#endif
  return std::chrono::system_clock::from_time_t(utc);
}

/**
 * @brief KIS credentials (app key and secret), demo or real account, and the market-data tape.
 *
 * Load with `from_env()` (`KIS_APP_KEY`, `KIS_APP_SECRET`, `KIS_DEMO`) or `from_json(filename)`.
 */
struct KisConfig : public network::ClientCredentials<KisConfig> {
  // std::string grant_type = "client_credentials"; // length = 18
  // std::string app_key_; // length = 36
  // std::string app_secret_; // length = 180
  /**
   * @brief Whether to use KIS's demo (모의투자) endpoints; **`true` unless stated otherwise**.
   *
   * Real trading must be stated: `KIS_DEMO=0`, `"demo": false` in the credential file, or
   * `is_demo_ = false`. Real-account keys sent to the demo endpoints are refused at the first token
   * request, so a forgotten setting fails rather than trades.
   */
  bool is_demo_ = true;

  /**
   * @brief Which venue's prices every KIS market-data request reads (quotes, bars, depth, price
   *   bands, trades); KRX and NXT combined by default, as Toss reports.
   *
   * It does not change the instrument's identity or where orders go.
   */
  MarketTape market_tape_ = MarketTape::CONSOLIDATED;

  /**
   * @brief Reads the credentials from `KIS_APP_KEY` and `KIS_APP_SECRET`, and the environment from
   *   `KIS_DEMO` (see `demo_from_env`); called by `from_env()`. Nothing changes if it throws.
   * @throws std::runtime_error if a key variable is missing or `KIS_DEMO` is invalid.
   */
  void from_env_impl() {
    const char* key = std::getenv("KIS_APP_KEY");
    const char* secret = std::getenv("KIS_APP_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add KIS_APP_KEY and KIS_APP_SECRET environment variables.");
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
   * Format: `{ "app_key": "...", "app_secret": "...", "demo": false }`; without `"demo"` the config
   * is demo.
   *
   * @param filename The file.
   * @throws std::runtime_error if the file cannot be read or parsed, a key is missing or not a
   *   string, or `"demo"` is not a boolean.
   */
  void from_json_impl(
    const std::string& filename
  ) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open KIS credential file: " + filename);
    }
    const std::string content(
      std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{}
    );
    boost::system::error_code ec;
    const auto jv = json::parse(content, ec);
    if (ec) {
      throw std::runtime_error(
        "Failed to parse KIS credentials file: " + filename + " — " + ec.message()
      );
    }
    if (!jv.is_object()) {
      throw std::runtime_error("Failed to parse KIS credentials file: " + filename + " — not a JSON object");
    }
    const auto& doc = jv.as_object();
    const json::value* key = doc.if_contains("app_key");
    const json::value* secret = doc.if_contains("app_secret");
    if (!key || !secret || !key->is_string() || !secret->is_string()) {
      throw std::runtime_error("Credential file should contain app_key and app_secret as strings");
    }
    // Everything is read and checked before anything is assigned, so a refused file leaves this
    // config -- and the endpoint it already selected -- exactly as it was.
    const bool demo = demo_from_json(doc, filename);
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
    is_demo_ = demo;
  }

  /**
   * @brief Reads `KIS_DEMO`.
   * @return `true` (demo) if unset or `1`, `false` (real) if `0`.
   * @throws std::runtime_error for any other value.
   */
  static bool demo_from_env() {
    const char* demo = std::getenv("KIS_DEMO");
    if (!demo || std::string_view(demo) == "1") {
      return true;
    }
    if (std::string_view(demo) == "0") {
      return false;
    }
    throw std::runtime_error(
      std::string("KIS_DEMO is \"") + demo + "\"; set it to 1 for the demo endpoints or 0 to trade "
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
        "KIS credential file " + filename + " has a \"demo\" that is not true or false; write "
        "true for the demo endpoints or false to trade live, or leave it out for demo."
      );
    }
    return demo->get_bool();
  }

  /**
   * @brief Returns the token request's field for the app key.
   * @return `"appkey"`.
   */
  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  /**
   * @brief Returns the token request's field for the app secret.
   * @return `"appsecret"`.
   */
  static constexpr std::string_view secret_field() noexcept { return "appsecret"; }

  /**
   * @brief Returns the REST host for the environment.
   * @return The host name.
   */
  const char* rest_host() const noexcept {
    return is_demo_ ? "openapivts.koreainvestment.com" : "openapi.koreainvestment.com";
  }

  /**
   * @brief Returns the REST port for the environment.
   * @return `"29443"` (demo) or `"9443"`.
   */
  const char* rest_port() const noexcept { return is_demo_ ? "29443" : "9443"; }

  /**
   * @brief Returns the WebSocket host (the same for both environments).
   * @return The host name.
   */
  const char* ws_host() const noexcept { return "ops.koreainvestment.com"; }

  /**
   * @brief Returns the WebSocket port for the environment.
   * @return `"31000"` (demo) or `"21000"`.
   */
  const char* ws_port() const noexcept { return is_demo_ ? "31000" : "21000"; }

  /**
   * @brief Builds the WebSocket approval-key request body (the secret under `secretkey`).
   * @return The JSON body.
   */
  std::string ws_to_jsonbody() const {
    return R"({"grant_type":"client_credentials","appkey":")" + key_ + R"(","secretkey":")" +
      secret_ + R"("})";
  }
};

/** @brief A KIS REST access token and its expiry. */
struct KisRest : public network::AuthBody<KisRest> {
  // std::string access_token_; // length = 350
  // // std::string token_type; // length = 20, Bearer
  // int64_t expires_in_; // 10-digits
  /** @brief KIS's stated expiry (`access_token_token_expired`), which takes precedence over `expires_in`. */
  std::chrono::system_clock::time_point access_token_expired_; // length = 50, format: "2022-08-30 08:10:10"

  /** @brief Constructs an empty token. */
  KisRest() {}

  /**
   * @brief Returns the token response's field for the token.
   * @return `"access_token"`.
   */
  static constexpr std::string_view token_field() noexcept { return "access_token"; }

  /**
   * @brief Reads KIS's stated expiry from a token response; called by `from_json`.
   * @param doc The response.
   */
  void from_json_impl(
    const json::object& doc
  ) {
    // if (!doc.contains("access_token")) {
    // 	throw std::runtime_error("Token authentication failed");
    // }
    // access_token_ = doc.at("access_token").as_string().c_str();
    // expires_in_ = doc.contains("expires_in")
    //   ? doc.at("expires_in").as_int64()
    //   : 86400;
    access_token_expired_ = doc.contains("access_token_token_expired") ?
      parse_expiry(doc.at("access_token_token_expired").as_string().c_str()) :
      expires_at_;
    if (access_token_expired_ != std::chrono::system_clock::time_point{}) {
      expires_at_ = access_token_expired_;
    }
  }

  /**
   * @brief Tests whether the token is set and stays valid for at least five more minutes.
   * @return `true` if so.
   */
  bool valid() const noexcept { return network::AuthBody<KisRest>::valid(); }
};

/** @brief A KIS WebSocket approval key and when it was issued. */
struct KisWebsocket {
  /** @brief The approval key. */
  std::string approval_key_; // length = 286
  /** @brief When it was received. */
  std::chrono::system_clock::time_point last_auth_time_;

  /** @brief Constructs an empty key. */
  KisWebsocket() {}

  /**
   * @brief Reads an approval-key response.
   * @param doc The response.
   * @throws std::runtime_error if it has no `approval_key`.
   */
  void from_json(
    const json::object& doc
  ) {
    if (!doc.contains("approval_key")) {
      throw std::runtime_error("WS approval key authentication failed");
    }
    approval_key_ = doc.at("approval_key").as_string().c_str();
    last_auth_time_ = std::chrono::system_clock::now();
  }

  /**
   * @brief Tests whether the key is set and was received less than a day ago.
   * @return `true` if so.
   */
  bool valid() const noexcept {
    // https://github.com/koreainvestment/open-trading-api/blob/33e0e1e65cd1c8c8b639531483ec0b327087bab1/examples_user/kis_auth.py#L507
    const auto now = std::chrono::system_clock::now();
    const bool never_authed = last_auth_time_ == std::chrono::system_clock::time_point{};
    const auto elapsed =
      std::chrono::duration_cast<std::chrono::seconds>(now - last_auth_time_).count();
    return !approval_key_.empty() && !never_authed && elapsed < 86400;
  }
};

} // namespace kis
} // namespace algotradelib
