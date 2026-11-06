#pragma once

#include <boost/json.hpp>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include "../../network/oauth.hpp"

namespace algotradelib {
namespace tossinvest {

struct TossConfig;
struct TossRest;
struct TossWebsocket;

/**
 * @brief Toss Securities credentials (client id and secret). Toss has no demo environment.
 *
 * Load with `from_env()` (`TOSS_CLIENT_ID`, `TOSS_CLIENT_SECRET`), `from_json(filename)` or
 * `from_json_text(text, origin)`.
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
   *   called by `from_json(filename)`. Nothing changes if it throws.
   * @param filename The file.
   * @throws std::runtime_error if the file cannot be read, is not a JSON object, or lacks either
   *   field or has one that is not a string.
   */
  void from_json_impl(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Toss Invest credential file: " + filename);
    }
    const std::string content(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{});
    from_json_text_impl(content, "file " + filename);
  }

  /**
   * @brief Reads the credentials from JSON text in the file's format; called by `from_json_text`.
   *   Nothing changes if it throws.
   * @param text The JSON document.
   * @param origin What error messages call the source (`"file creds.json"`); not a secret.
   * @throws std::runtime_error if the text cannot be parsed, is not a JSON object, or lacks either field
   *   or has one that is not a string.
   */
  void from_json_text_impl(std::string_view text, const std::string& origin) {
    boost::system::error_code ec;
    const auto jv = json::parse(text, ec);
    if (ec || !jv.is_object()) {
      throw std::runtime_error(
        "Failed to parse Toss Invest credentials " + origin +
        (ec ? " — " + ec.message() : std::string(" — not a JSON object"))
      );
    }
    const auto& doc = jv.as_object();
    const json::value* key = doc.if_contains("client_id");
    const json::value* secret = doc.if_contains("client_secret");
    if (!key || !secret || !key->is_string() || !secret->is_string()) {
      throw std::runtime_error(
        "Toss Invest credential " + origin + " should contain client_id and client_secret as strings"
      );
    }
    // Both are checked before either is assigned, so a refused document leaves these credentials as they
    // were.
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
  }

  /**
   * @brief Returns the token request's field for the client id.
   * @return `"client_id"`.
   */
  static constexpr std::string_view key_field() noexcept {
    return "client_id";
  }

  /**
   * @brief Returns the token request's field for the client secret.
   * @return `"client_secret"`.
   */
  static constexpr std::string_view secret_field() noexcept {
    return "client_secret";
  }
};

/** @brief A Toss REST access token and its expiry. */
struct TossRest : public network::AuthBody<TossRest> {
  /**
   * @brief Returns the token response's field for the token.
   * @return `"access_token"`.
   */
  static constexpr std::string_view token_field() noexcept {
    return "access_token";
  }

  /** @brief Reads nothing more from a token response (the argument is unused; Toss states `expires_in`); called by `from_json`. */
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
