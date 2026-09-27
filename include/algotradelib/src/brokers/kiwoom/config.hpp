#pragma once

#include "./network.hpp"
#include <fstream>
#include <boost/json.hpp>

namespace algotradelib {
namespace kiwoom {

/** Kiwoom application credentials and live/demo endpoint selection. */
struct KiwoomConfig : public network::ClientCredentials<KiwoomConfig> {
  bool is_demo_ = false;

  /** Reads KIS credentials and environment selection from process variables. */
  void from_env_impl() {
    const char* key = std::getenv("KIWOOM_APP_KEY");
    const char* secret = std::getenv("KIWOOM_SECRET_KEY");
    if (!key || !secret) {
      throw std::runtime_error("Add KIWOOM_APP_KEY and KIWOOM_SECRET_KEY environment variables.");
    }
    key_ = key;
    secret_ = secret;
		const char* demo = std::getenv("KIWOOM_DEMO");
    is_demo_ = demo && std::string_view(demo) == "1";
  }

	/**
   * Reads credentials from a JSON file.
   *
   * Expected format: `{ "app_key": "...", "app_secret": "...", "demo": false }`.
   */
  void from_json_impl(
    const std::string& filename
  ) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Kiwoom credential file: " + filename);
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
    const auto& doc = jv.as_object();
    if (!doc.contains("appkey") || !doc.contains("secretkey")) {
      throw std::runtime_error("Credential file should contain appkey and secretkey");
    }
    key_ = doc.at("appkey").as_string().c_str();
    secret_ = doc.at("secretkey").as_string().c_str();
		is_demo_ = doc.contains("demo") && doc.at("demo").as_bool();
  }

	/** The JSON field Toss expects the client id under. */
  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  /** The JSON field Toss expects the client secret under. */
  static constexpr std::string_view secret_field() noexcept { return "secretkey"; }

	/** The REST host for the selected environment. */
  const char* rest_host() const noexcept {
    return is_demo_ ? "mockapi.kiwoom.com" : "api.kiwoom.com";
  }

  /** The WebSocket host, which is the same for both environments. */
  const char* ws_host() const noexcept { return "ops.koreainvestment.com"; }
};

/** Parsed Kiwoom Securities REST OAuth token. */
struct KiwoomRest : public network::AuthBody<KiwoomRest> {

  /** Nothing to read: Kiwoom's token response carries no field beyond the shared base's. */
  void from_json_impl(const json::object& doc) {
		// Kiwoom has "token" not "access_token" -> should be here not in AuthBody
	}
};

} // namespace kiwoom
} // namespace algotradelib
