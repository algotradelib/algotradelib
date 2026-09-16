#pragma once

#include "./config.hpp"
// #include "./network.hpp"
// #include <chrono>
// #include <mutex>

namespace algotradelib {
namespace kis {

class KisAuth;

/**
 * @brief KIS OAuth Authentication
 * 
 */
class KisAuth {
public:
	KisAuth(net::io_context& ioc, ssl::context& ctx, const KisConfig& config)
	: ioc_(ioc), ctx_(ctx), req_(config) {}
	
	virtual ~KisAuth() = default;
	
	const std::string& accessToken() {
		if (!rest_resp_.valid()) {
			refresh();
		}
		return rest_resp_.access_token_;
	}

	const std::string& accessWebsocket() {
		std::lock_guard<std::mutex> lock(mtx_);
		if (!ws_resp_.valid()) {
			refreshWebsocket();
		}
		return ws_resp_.approval_key_;
	}

private:
	net::io_context& ioc_;
  ssl::context& ctx_;
	KisConfig req_;
	KisRest rest_resp_;
	KisWebsocket ws_resp_;
	std::mutex mtx_;

	void refresh() {
		const std::string jsonbody = req_.to_jsonbody();
		KisHttpClient rest(ioc_, ctx_, req_.rest_host(), req_.rest_port());
		const std::string resp = std::move(rest.post(
			"/oauth2/tokenP",
			jsonbody,
      {{"content-type", "application/json"}}
		).body());
		boost::system::error_code ec;
    const auto jv = json::parse(resp, ec);
    // const auto* doc = (!ec && jv.is_object()) ? &jv.as_object() : nullptr;
    // if (!doc || !doc->contains("access_token")) {
		// 	throw std::runtime_error("Token authentication failed: " + resp);
		// }
		// rest_resp_.access_token_ = doc->at("access_token").as_string().c_str();
		// rest_resp_.expires_in_ = doc->contains("expires_in") ? doc->at("expires_in").as_int64() : 86400;
		// rest_resp_.access_token_expired_ = doc->contains("access_token_token_expired")
		// 	? parse_expiry(doc->at("access_token_token_expired").as_string().c_str())
		// 	: std::chrono::system_clock::now() + std::chrono::seconds(86400);
		if (ec || !jv.is_object()) {
			throw std::runtime_error("Token authentication failed: " + resp);
		}
		rest_resp_.from_json(jv.as_object());
	}

	void refreshWebsocket() {
		const std::string jsonbody = req_.ws_to_jsonbody();
		KisHttpClient rest(ioc_, ctx_, req_.rest_host(), req_.rest_port());
		// _base_headers_ws = {
		// 		"content-type": "utf-8",
		// }
		const std::string resp = std::move(rest.post(
			"/oauth2/Approval",
			jsonbody,
      {{"content-type", "utf-8"}}
		).body());
		boost::system::error_code ec;
    const auto jv = json::parse(resp, ec);
    // const auto* doc = (!ec && jv.is_object()) ? &jv.as_object() : nullptr;
    // if (!doc || !doc->contains("approval_key")) {
		// 	throw std::runtime_error("WS approval key authentication failed: " + resp);
		// }
		// ws_resp_.approval_key = doc->at("approval_key").as_string().c_str();
		// ws_resp_.last_auth_time = std::chrono::system_clock::now();
		if (ec || !jv.is_object()) {
			throw std::runtime_error("Token authentication failed: " + resp);
		}
		ws_resp_.from_json(jv.as_object());
	}
};

} // namespace kis
} // namespace algotradelib