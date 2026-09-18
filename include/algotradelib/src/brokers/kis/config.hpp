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
struct KisRest;
struct KisWebsocket;

static inline std::chrono::system_clock::time_point parse_expiry(
  const char* s) {
  std::tm tm{};
  if (std::sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
        &tm.tm_min, &tm.tm_sec) != 6) {
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
 * @brief Token (Request)
 *
 */
struct KisConfig : public network::ClientCredentials<KisConfig> {
  // std::string grant_type = "client_credentials"; // length = 18
  // std::string app_key_; // length = 36
  // std::string app_secret_; // length = 180
  bool is_demo_ = false;

  /**
   * @brief Read from the environment
   *
   */
  void from_env_impl() {
    const char* key = std::getenv("KIS_APP_KEY");
    const char* secret = std::getenv("KIS_APP_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add KIS_APP_KEY and KIS_APP_SECRET environment variables.");
    }
    key_ = key;
    secret_ = secret;
    const char* demo = std::getenv("KIS_DEMO");
    is_demo_ = demo && std::string_view(demo) == "1";
  }

  /**
   * @brief Read from json file
   * Format: { "app_key": "...", "app_secret": "...", "is_demo": false }
   *
   * @param filename
   */
  void from_json_impl(
    const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open KIS credential file: " + filename);
    }
    const std::string content(
      std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{});
    boost::system::error_code ec;
    const auto jv = json::parse(content, ec);
    if (ec) {
      throw std::runtime_error(
        "Failed to parse KIS credentials file: " + filename + " — " + ec.message());
    }
    const auto& doc = jv.as_object();
    if (!doc.contains("app_key") || !doc.contains("app_secret")) {
      throw std::runtime_error("Credential file should contain app_key and app_secret");
    }
    key_ = doc.at("app_key").as_string().c_str();
    secret_ = doc.at("app_secret").as_string().c_str();
    is_demo_ = doc.contains("demo") && doc.at("demo").as_bool();
  }

  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  static constexpr std::string_view secret_field() noexcept { return "appsecret"; }

  const char* rest_host() const noexcept {
    return is_demo_ ? "openapivts.koreainvestment.com" : "openapi.koreainvestment.com";
  }

  const char* rest_port() const noexcept { return is_demo_ ? "29443" : "9443"; }

  const char* ws_host() const noexcept { return "ops.koreainvestment.com"; }

  const char* ws_port() const noexcept { return is_demo_ ? "31000" : "21000"; }

  std::string ws_to_jsonbody() const {
    return R"({"grant_type":"client_credentials","appkey":")" + key_ + R"(","secretkey":")" +
           secret_ + R"("})";
  }
};

/**
 * @brief Token (Response)
 *
 */
struct KisRest : public network::AuthBody<KisRest> {
  // std::string access_token_; // length = 350
  // // std::string token_type; // length = 20, Bearer
  // int64_t expires_in_; // 10-digits
  std::chrono::system_clock::time_point
    access_token_expired_; // length = 50, format: "2022-08-30 08:10:10"

  KisRest() {}

  void from_json_impl(
    const json::object& doc) {
    // if (!doc.contains("access_token")) {
    // 	throw std::runtime_error("Token authentication failed");
    // }
    // access_token_ = doc.at("access_token").as_string().c_str();
    // expires_in_ = doc.contains("expires_in")
    //   ? doc.at("expires_in").as_int64()
    //   : 86400;
    access_token_expired_ =
      doc.contains("access_token_token_expired")
        ? parse_expiry(doc.at("access_token_token_expired").as_string().c_str())
        : expires_at_;
    if (access_token_expired_ != std::chrono::system_clock::time_point{}) {
      expires_at_ = access_token_expired_;
    }
  }

  bool valid() const noexcept { return network::AuthBody<KisRest>::valid(); }
};

/**
 * @brief Websocket key
 *
 */
struct KisWebsocket {
  std::string approval_key_; // length = 286
  std::chrono::system_clock::time_point last_auth_time_;

  KisWebsocket() {}

  void from_json(
    const json::object& doc) {
    if (!doc.contains("approval_key")) {
      throw std::runtime_error("WS approval key authentication failed");
    }
    approval_key_ = doc.at("approval_key").as_string().c_str();
    last_auth_time_ = std::chrono::system_clock::now();
  }

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
