#pragma once

#include "../../network/oauth.hpp"

#include <boost/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {
namespace alpaca {

// https://docs.alpaca.markets/docs/authentication
// Trading: https://paper-api.alpaca.markets (paper), https://api.alpaca.markets (live)
// Market data: https://data.alpaca.markets (both)

/**
 * @brief Alpaca API keys (key id and secret key), paper or live account.
 *
 * Load with `from_env()` (`ALPACA_KEY`, `ALPACA_SECRET`, `ALPACA_DEMO`), `from_json(filename)` or
 * `from_json_text(text, origin)`.
 * Alpaca has no token: both keys go on every request as headers, so nothing is requested or stored before a call.
 * Paper and live keys differ, and a key sent to the other environment's host is refused (HTTP 401) at its first
 * request, so a forgotten setting fails rather than trades.
 */
struct AlpacaConfig : public network::ClientCredentials<AlpacaConfig> {
  /**
   * @brief Whether to use Alpaca's paper trading host; **`true` unless stated otherwise**.
   *
   * Live trading must be stated: `ALPACA_DEMO=0`, `"demo": false` in the credential file, or `is_demo_ = false`.
   * Live is the same code path as paper and differs only in the trading host; it is supported, but the library's
   * own checks ran on paper only.
   */
  bool is_demo_ = true;

  /**
   * @brief Reads the keys from `ALPACA_KEY` and `ALPACA_SECRET`, and the environment from `ALPACA_DEMO` (see
   *   `demo_from_env`); called by `from_env()`. Nothing changes if it throws.
   * @throws std::runtime_error if a key variable is missing or `ALPACA_DEMO` is invalid.
   */
  void from_env_impl() {
    const char* key = std::getenv("ALPACA_KEY");
    const char* secret = std::getenv("ALPACA_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add ALPACA_KEY and ALPACA_SECRET environment variables.");
    }
    // Read before anything is assigned, so a refused load leaves this config as it was.
    const bool demo = demo_from_env();
    key_ = key;
    secret_ = secret;
    is_demo_ = demo;
  }

  /**
   * @brief Reads the keys from a JSON file; called by `from_json(filename)`. Nothing changes if it throws.
   *
   * Format: `{ "key_id": "...", "secret_key": "...", "demo": false }`; without `"demo"` the config is paper.
   *
   * @param filename The file.
   * @throws std::runtime_error if the file cannot be read or parsed, a key is missing or not a string, or `"demo"`
   *   is not a boolean.
   */
  void from_json_impl(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Alpaca credential file: " + filename);
    }
    const std::string content(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{});
    from_json_text_impl(content, "file " + filename);
  }

  /**
   * @brief Reads the keys from JSON text in the file's format; called by `from_json_text`. Nothing changes if it
   *   throws.
   * @param text The JSON document.
   * @param origin What error messages call the source (`"file creds.json"`); not a secret.
   * @throws std::runtime_error if the text cannot be parsed, a key is missing or not a string, or `"demo"` is not a
   *   boolean.
   */
  void from_json_text_impl(std::string_view text, const std::string& origin) {
    boost::system::error_code ec;
    const auto jv = json::parse(text, ec);
    if (ec || !jv.is_object()) {
      throw std::runtime_error("Failed to parse Alpaca credential " + origin + ": not a JSON object");
    }
    const auto& doc = jv.as_object();
    const json::value* key = doc.if_contains("key_id");
    const json::value* secret = doc.if_contains("secret_key");
    if (!key || !secret || !key->is_string() || !secret->is_string()) {
      throw std::runtime_error("Alpaca credential " + origin + " should contain key_id and secret_key as strings");
    }
    // Everything is read and checked before anything is assigned.
    const bool demo = demo_from_json(doc, origin);
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
    is_demo_ = demo;
  }

  /**
   * @brief Reads `ALPACA_DEMO`.
   * @return `true` (paper) if unset or `1`, `false` (live) if `0`.
   * @throws std::runtime_error for any other value.
   */
  static bool demo_from_env() {
    const char* demo = std::getenv("ALPACA_DEMO");
    if (!demo || std::string_view(demo) == "1") {
      return true;
    }
    if (std::string_view(demo) == "0") {
      return false;
    }
    throw std::runtime_error(
      std::string("ALPACA_DEMO is \"") + demo + "\"; set it to 1 for paper trading or 0 to trade live, or leave it " +
      "unset for paper."
    );
  }

  /**
   * @brief Reads a credential document's `"demo"`.
   * @param doc The document.
   * @param origin What the error message calls the source.
   * @return `true` if absent, otherwise its value.
   * @throws std::runtime_error if it is not a boolean.
   */
  static bool demo_from_json(const json::object& doc, const std::string& origin) {
    const json::value* demo = doc.if_contains("demo");
    if (!demo) {
      return true;
    }
    if (!demo->is_bool()) {
      throw std::runtime_error(
        "Alpaca credential " + origin +
        " has a \"demo\" that is not true or false; write true for paper trading or false to trade live, or leave "
        "it out for paper."
      );
    }
    return demo->get_bool();
  }

  /**
   * @brief The credential file's field for the key id; Alpaca has no token request behind it.
   * @return `"key_id"`.
   */
  static constexpr std::string_view key_field() noexcept {
    return "key_id";
  }

  /**
   * @brief The credential file's field for the secret key; Alpaca has no token request behind it.
   * @return `"secret_key"`.
   */
  static constexpr std::string_view secret_field() noexcept {
    return "secret_key";
  }

  /**
   * @brief The trading host for the selected environment.
   * @return `"paper-api.alpaca.markets"` (paper) or `"api.alpaca.markets"` (live).
   */
  const char* trading_host() const noexcept {
    return is_demo_ ? "paper-api.alpaca.markets" : "api.alpaca.markets";
  }

  /**
   * @brief The market-data host, the same for both environments.
   * @return `"data.alpaca.markets"`.
   */
  const char* data_host() const noexcept {
    return "data.alpaca.markets";
  }

  /**
   * @brief The HTTPS port, the same for every host.
   * @return `"443"`.
   */
  const char* port() const noexcept {
    return "443";
  }
};

} // namespace alpaca
} // namespace algotradelib
