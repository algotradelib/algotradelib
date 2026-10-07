#pragma once

#include "./config.hpp"
#include <mutex>

namespace algotradelib {
namespace tossinvest {

class TossAuth;

/**
 * @brief Obtains and caches a Toss access token with blocking requests; thread-safe.
 *
 * `TossClient` manages its own token; this is for code that calls Toss directly.
 */
class TossAuth {
public:
  /**
   * @brief Constructs the wrapper.
   * @param ioc The I/O context the requests run on; borrowed, and must outlive the wrapper.
   * @param ctx The TLS context; borrowed, and must outlive the wrapper.
   * @param config The credentials (copied).
   */
  TossAuth(net::io_context& ioc, ssl::context& ctx, const TossConfig& config)
  : ioc_(ioc), ctx_(ctx), req_(config) {}

  /** @brief Destroys the wrapper. */
  virtual ~TossAuth() = default;

  /**
   * @brief Returns an access token, requesting a new one if none is held or it expires within five
   *   minutes.
   * @return The token.
   * @throws std::runtime_error if the token request fails or its response cannot be read.
   * @throws boost::system::system_error on a transport failure.
   */
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
  TossConfig req_;
  TossRest rest_resp_;
  std::mutex mtx_;

  void refresh() {
    const std::string formbody = req_.to_form_urlencoded_body();
    TossHttpClient rest(ioc_, ctx_, "openapi.tossinvest.com", "443");
    auto response = rest.post("/oauth2/token", formbody, {{"Content-Type", "application/x-www-form-urlencoded"}});
    if (response.result_int() < 200 || response.result_int() >= 300) {
      throw std::runtime_error(
        "Token authentication failed with HTTP status " + std::to_string(response.result_int())
      );
    }
    const std::string resp = std::move(response.body());
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
