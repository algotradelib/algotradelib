#pragma once

#include "./config.hpp"

namespace algotradelib {
namespace tossinvest {

class TossAuth;

class TossAuth {
public:
	TossAuth(net::io_context& ioc, ssl::context& ctx, const TossConfig& config)
	: ioc_(ioc), ctx_(ctx), req_(config) {}

	virtual ~TossAuth() = default;
	
	const std::string& accessToken() {
		if (rest_resp_.access_token_.empty()) {
			refresh();
		}
		return rest_resp_.access_token_;
	}

private:
	net::io_context& ioc_;
  ssl::context& ctx_;
	TossConfig req_;
	TossRest rest_resp_;

	void refresh() {
		const std::string jsonbody = req_.to_jsonbody();
		TossHttpClient rest(ioc_, ctx_, "openapi.tossinvest.com", "443");
		const std::string resp = std::move(rest.post(
			"/oauth2/token",
			jsonbody,
      {{"Content-Type", "application/x-www-form-urlencoded"}}
		).body());
		boost::system::error_code ec;
    const auto jv = json::parse(resp, ec);
		if (ec || !jv.is_object()) {
			throw std::runtime_error("Token authentication failed: " + resp);
		}
		rest_resp_.from_json(jv.as_object());
	}
};

} // namespace tossinvest
} // namespace algotradelib