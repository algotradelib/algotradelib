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

/**
 * Which venue's book a KIS domestic market-data call reads.
 *
 * KIS-private on purpose. A tape is Korean market structure -- two venues trading
 * one listing, plus the consolidated feed over them -- and broker-neutral `core/`
 * has nothing to normalize it to. See `KisConfig::market_tape_`.
 */
enum class MarketTape {
  /** The consolidated KRX+NXT tape (`UN`). */
  CONSOLIDATED,
  /** KRX alone (`J`). */
  KRX,
  /** NXT alone (`NX`). */
  NXT,
};

/** Returns the `FID_COND_MRKT_DIV_CODE` value for `tape`. */
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

/** KIS application credentials and live/demo endpoint selection. */
struct KisConfig : public network::ClientCredentials<KisConfig> {
  // std::string grant_type = "client_credentials"; // length = 18
  // std::string app_key_; // length = 36
  // std::string app_secret_; // length = 180
  /**
   * Whether to use KIS's 모의투자 endpoints.
   *
   * The loaders require it: `from_env` refuses unless `KIS_DEMO` is `1` or `0`, and `from_json`
   * refuses unless `"demo"` is `true` or `false`. See `network::required_demo_flag`. A config built in
   * code keeps this default, so such a program states the field in its own source.
   */
  bool is_demo_ = false;

  /**
   * Which book KIS's domestic market-data calls read.
   *
   * KRX-listed equities trade on two venues since Nextrade opened (2025-03-04),
   * and `FID_COND_MRKT_DIV_CODE` selects which one a quote, a bar, the depth, the
   * price limits or the recent prints describe: `J` is KRX alone, `NX` is NXT
   * alone, `UN` is the consolidated tape.
   *
   * **It is one setting for every market-data verb, and that is the point.** Five
   * of the seven verbs used to send `J` unconditionally while the quote derived its
   * code from the instrument, so a caller's last price came from the consolidated
   * tape and their bars from KRX -- and `Context::last_price`, which the exposure
   * ceiling is measured against, is written from both.
   *
   * **It is deliberately not read from `InstrumentId::venue_`.** That field is an
   * identity: `validate_order` refuses an order whose instrument names anything but
   * the combined venue, because a holding carries no venue and a mismatch makes
   * `Portfolio::working_quantity` read zero. A tape is a question about data, not
   * about which instrument this is, so it lives here where changing it cannot
   * invalidate an order.
   *
   * Consolidated by default, which is what a Korean equity's price means when
   * nobody says otherwise, and what Toss reports -- its market data takes no tape
   * parameter at all and its trading hours are stated on the 통합 (KRX+NXT) basis.
   * So the default makes the two brokers answer the same question.
   */
  MarketTape market_tape_ = MarketTape::CONSOLIDATED;

  /** Reads KIS credentials and environment selection from process variables. */
  void from_env_impl() {
    const char* key = std::getenv("KIS_APP_KEY");
    const char* secret = std::getenv("KIS_APP_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add KIS_APP_KEY and KIS_APP_SECRET environment variables.");
    }
    // Read before anything is assigned, so a refused load leaves this config as it was.
    const bool demo = network::required_demo_flag("KIS_DEMO");
    key_ = key;
    secret_ = secret;
    is_demo_ = demo;
  }

  /**
   * Reads credentials from a JSON file.
   *
   * Expected format: `{ "app_key": "...", "app_secret": "...", "demo": false }`. `"demo"` is
   * required and must be `true` or `false`.
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
    const bool demo = network::required_demo_flag(doc, filename);
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
    is_demo_ = demo;
  }

  /** The JSON field KIS expects the application key under. */
  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  /** The JSON field KIS expects the application secret under. */
  static constexpr std::string_view secret_field() noexcept { return "appsecret"; }

  /** The REST host for the selected environment. */
  const char* rest_host() const noexcept {
    return is_demo_ ? "openapivts.koreainvestment.com" : "openapi.koreainvestment.com";
  }

  /** The REST port for the selected environment. */
  const char* rest_port() const noexcept { return is_demo_ ? "29443" : "9443"; }

  /** The WebSocket host, which is the same for both environments. */
  const char* ws_host() const noexcept { return "ops.koreainvestment.com"; }

  /** The WebSocket port for the selected environment. */
  const char* ws_port() const noexcept { return is_demo_ ? "31000" : "21000"; }

  /** Returns the approval-key request body, which spells the secret `secretkey` rather than `appsecret`. */
  std::string ws_to_jsonbody() const {
    return R"({"grant_type":"client_credentials","appkey":")" + key_ + R"(","secretkey":")" +
      secret_ + R"("})";
  }
};

/** Parsed KIS REST OAuth token and absolute expiry time. */
struct KisRest : public network::AuthBody<KisRest> {
  // std::string access_token_; // length = 350
  // // std::string token_type; // length = 20, Bearer
  // int64_t expires_in_; // 10-digits
  /** KIS's own stated expiry, which overrides the relative `expires_in` when present. */
  std::chrono::system_clock::time_point access_token_expired_; // length = 50, format: "2022-08-30 08:10:10"

  KisRest() {}

  /** The field KIS returns the bearer token under. */
  static constexpr std::string_view token_field() noexcept { return "access_token"; }

  /** Reads KIS's absolute expiry out of the token response. */
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

  /** Returns whether the token is present and not yet expired. */
  bool valid() const noexcept { return network::AuthBody<KisRest>::valid(); }
};

/** Parsed KIS WebSocket approval key and refresh timestamp. */
struct KisWebsocket {
  /** The approval key KIS issued for a WebSocket session. */
  std::string approval_key_; // length = 286
  /** When it was issued, which is what the refresh window is measured from. */
  std::chrono::system_clock::time_point last_auth_time_;

  KisWebsocket() {}

  /** Reads the approval key out of the response, throwing when it is absent. */
  void from_json(
    const json::object& doc
  ) {
    if (!doc.contains("approval_key")) {
      throw std::runtime_error("WS approval key authentication failed");
    }
    approval_key_ = doc.at("approval_key").as_string().c_str();
    last_auth_time_ = std::chrono::system_clock::now();
  }

  /** Returns whether the key is present and still inside KIS's refresh window. */
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
