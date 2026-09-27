#pragma once

#include "./config.hpp"
#include <mutex>

namespace algotradelib {
namespace kiwoom {

class KiwoomAuth;

/** Blocking, mutex-protected compatibility wrapper for Kiwoom authentication. */
class KiwoomAuth {
public:
  /** Constructs an authentication wrapper with copied credentials. */
  KiwoomAuth(net::io_context& ioc, ssl::context& ctx, const KiwoomConfig& config)
  : ioc_(ioc), ctx_(ctx), req_(config) {}

  virtual ~KiwoomAuth() = default;

  /** Returns a cached REST token, refreshing it synchronously when required. */
  std::string accessToken() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!rest_resp_.valid()) {
      refresh();
    }
    return rest_resp_.access_token_;
  }

private:
  net::io_context& ioc_;
  ssl::context& ctx_;
  KiwoomConfig req_;
  KiwoomRest rest_resp_;
  std::mutex mtx_;

  void refresh() {
    const std::string jsonbody = req_.to_jsonbody();
    KiwoomHttpClient rest(ioc_, ctx_, req_.rest_host(), "443");
		auto response = rest.post("/oauth2/token", jsonbody, {{"Content-Type", "application/json;charset=UTF-8"}});
    if (response.result_int() < 200 || response.result_int() >= 300) {
      throw std::runtime_error(
        "Token authentication failed with HTTP status " + std::to_string(response.result_int())
      );
    }
    const std::string resp = std::move(response.body());
    boost::system::error_code ec;
    const auto jv = json::parse(resp, ec);
    if (ec || !jv.is_object()) {
      throw std::runtime_error("Kiwoom authentication failed: " + resp);
    }
    rest_resp_.from_json(jv.as_object());
  }
};

} // namespace kiwoom
} // namespace algotradelib
