#pragma once

#include "./config.hpp"
#include "./errors.hpp"
#include "./network.hpp"

#include <mutex>
#include <stdexcept>
#include <string>

namespace algotradelib {
namespace kiwoom {

class KiwoomAuth;

/**
 * @brief Obtains and caches a Kiwoom access token with blocking requests; thread-safe.
 *
 * `KiwoomClient` manages its own token; this is for code that calls Kiwoom directly.
 */
class KiwoomAuth {
public:
  /**
   * @brief Constructs the wrapper.
   * @param ioc The I/O context the requests run on; borrowed, and must outlive the wrapper.
   * @param ctx The TLS context; borrowed, and must outlive the wrapper.
   * @param config The credentials (copied).
   */
  KiwoomAuth(net::io_context& ioc, ssl::context& ctx, const KiwoomConfig& config)
  : ioc_(ioc), ctx_(ctx), req_(config) {}

  /** @brief Destroys the wrapper. */
  virtual ~KiwoomAuth() = default;

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
  KiwoomConfig req_;
  KiwoomRest rest_resp_;
  std::mutex mtx_;

  void refresh() {
    const std::string jsonbody = req_.to_jsonbody();
    KiwoomHttpClient rest(ioc_, ctx_, req_.rest_host(), req_.rest_port());
    auto response = rest.post(
      "/oauth2/token", jsonbody, {{"Content-Type", "application/json;charset=UTF-8"}}
    );
    if (response.result_int() < 200 || response.result_int() >= 300) {
      throw std::runtime_error(
        "Token authentication failed with HTTP status " + std::to_string(response.result_int())
      );
    }
    const std::string resp = std::move(response.body());
    boost::system::error_code ec;
    const auto jv = json::parse(resp, ec);
    if (ec || !jv.is_object()) {
      // Not the body: a token reply that did not parse may still hold the token.
      throw std::runtime_error(
        "Kiwoom authentication failed: the reply is not a JSON object (" + std::to_string(resp.size()) + " bytes)"
      );
    }
    // Kiwoom states a business failure in the body of an HTTP 200, so a successful status is not
    // a successful token. The classification lives in `kiwoom_declared_error` and is shared with
    // every other endpoint rather than restated here: `8020` (no appkey or secretkey), `8011` and
    // `8012` (a missing or wrong grant_type) all arrive this way.
    const auto& doc = jv.as_object();
    if (const auto declared = kiwoom_declared_error(doc)) {
      throw std::runtime_error(
        "Kiwoom authentication failed [" + declared->broker_code_ + "]: " + declared->message_
      );
    }
    rest_resp_.from_json(doc);
  }
};

} // namespace kiwoom
} // namespace algotradelib
