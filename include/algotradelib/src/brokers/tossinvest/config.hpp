#pragma once

#include "./network.hpp"
#include <chrono>
#include <ctime>
#include <boost/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace algotradelib {
namespace tossinvest {

struct TossConfig;
struct TossRest;
struct TossWebsocket;

/**
 * @brief Toss Securities credentials (client id and secret). Toss has no demo environment.
 *
 * Load with `from_env()` (`TOSS_CLIENT_ID`, `TOSS_CLIENT_SECRET`) or `from_json(filename)`.
 */
struct TossConfig : public network::ClientCredentials<TossConfig> {
  // std::string grant_type = "client_credentials"; // length = 18
  // std::string client_id_;
  // std::string client_secret_;

  /**
   * @brief Reads the credentials from `TOSS_CLIENT_ID` and `TOSS_CLIENT_SECRET`; called by
   *   `from_env()`.
   * @throws std::runtime_error if either is not set.
   */
  void from_env_impl() {
    const char* key = std::getenv("TOSS_CLIENT_ID");
    const char* secret = std::getenv("TOSS_CLIENT_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add TOSS_CLIENT_ID and TOSS_CLIENT_SECRET environment variables.");
    }
    key_ = key;
    secret_ = secret;
  }

  /**
   * @brief Reads the credentials from a JSON file, `{ "client_id": "...", "client_secret": "..." }`;
   *   called by `from_json(filename)`.
   * @param filename The file.
   * @throws std::runtime_error if the file cannot be read, is not a JSON object, or lacks either
   *   field or has one that is not a string.
   */
  void from_json_impl(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Toss Invest credential file: " + filename);
    }
    const std::string content(
      std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{}
    );
    boost::system::error_code ec;
    const auto jv = json::parse(content, ec);
    if (ec) {
      throw std::runtime_error(
        "Failed to parse Toss Invest credentials file: " + filename + " — " + ec.message()
      );
    }
    const auto& doc = jv.as_object();
    if (!doc.contains("client_id") || !doc.contains("client_secret")) {
      throw std::runtime_error("Credential file should contain client_id and client_secret");
    }
    key_ = doc.at("client_id").as_string().c_str();
    secret_ = doc.at("client_secret").as_string().c_str();
  }

  /**
   * @brief Returns the token request's field for the client id.
   * @return `"client_id"`.
   */
  static constexpr std::string_view key_field() noexcept { return "client_id"; }

  /**
   * @brief Returns the token request's field for the client secret.
   * @return `"client_secret"`.
   */
  static constexpr std::string_view secret_field() noexcept { return "client_secret"; }
};

/** @brief A Toss REST access token and its expiry. */
struct TossRest : public network::AuthBody<TossRest> {
  /**
   * @brief Returns the token response's field for the token.
   * @return `"access_token"`.
   */
  static constexpr std::string_view token_field() noexcept { return "access_token"; }

  /** @brief Reads nothing more from a token response (Toss states `expires_in`); called by `from_json`. */
  void from_json_impl(const json::object&) {}

  // bool valid() const noexcept {
  // 	const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
  // 		std::chrono::system_clock::now().time_since_epoch()
  // 	).count();
  // 	return !access_token_.empty() && (expires_in_ - now) > 300; // 5min
  // }
};

} // namespace tossinvest
} // namespace algotradelib
