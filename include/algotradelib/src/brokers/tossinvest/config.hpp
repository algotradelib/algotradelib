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

/** Toss Securities OAuth client credentials. */
struct TossConfig : public network::ClientCredentials<TossConfig> {
  // std::string grant_type = "client_credentials"; // length = 18
  // std::string client_id_;
  // std::string client_secret_;

  /** Reads Toss client credentials from process variables. */
  void from_env_impl() {
    const char* key = std::getenv("TOSS_CLIENT_ID");
    const char* secret = std::getenv("TOSS_CLIENT_SECRET");
    if (!key || !secret) {
      throw std::runtime_error("Add TOSS_CLIENT_ID and TOSS_CLIENT_SECRET environment variables.");
    }
    key_ = key;
    secret_ = secret;
  }

  /** Reads Toss client credentials from a JSON file. */
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

  /** The JSON field Toss expects the client id under. */
  static constexpr std::string_view key_field() noexcept { return "client_id"; }

  /** The JSON field Toss expects the client secret under. */
  static constexpr std::string_view secret_field() noexcept { return "client_secret"; }
};

/** Parsed Toss Securities REST OAuth token. */
struct TossRest : public network::AuthBody<TossRest> {
  /** The field Toss returns the bearer token under. */
  static constexpr std::string_view token_field() noexcept { return "access_token"; }

  /** Nothing to read: Toss's token response carries no field beyond the shared base's. */
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
