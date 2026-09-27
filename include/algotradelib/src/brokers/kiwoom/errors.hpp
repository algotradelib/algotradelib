#pragma once

#include "./network.hpp"

#include "../../core/types.hpp"

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace algotradelib::kiwoom {

/**
 * Which pacing layer a Kiwoom `return_code` reports as exhausted.
 *
 * Kiwoom documents three separate 유량 ceilings — per API, account-wide, and per API group — and
 * they are not interchangeable: backing off the wrong one leaves the exhausted one exhausted.
 */
enum class KiwoomRateLimitLayer {
  /** Not a rate-limit code. */
  NONE,
  /** `1700`: this API's own allowance. */
  ENDPOINT,
  /** `1701`: the total allowance across every API. */
  TOTAL,
  /** `1702`: the allowance of the API group this endpoint belongs to. */
  GROUP,
};

/** Returns the pacing layer `code` reports, or `NONE` when it reports none. */
inline KiwoomRateLimitLayer kiwoom_rate_limit_layer(std::string_view code) noexcept {
  if (code == "1700") {
    return KiwoomRateLimitLayer::ENDPOINT;
  }
  if (code == "1701") {
    return KiwoomRateLimitLayer::TOTAL;
  }
  if (code == "1702") {
    return KiwoomRateLimitLayer::GROUP;
  }
  return KiwoomRateLimitLayer::NONE;
}

/**
 * Returns the `ErrorCategory` one of Kiwoom's documented `return_code` values belongs to.
 *
 * Driven from the whole of the specification's `errorCodeList` -- thirty-seven entries -- rather
 * than from the handful an adapter notices first, because the ones it does not notice are the
 * expensive ones. **The entire `80xx` family names the key, the token, the terminal or the
 * registered IP**, and two of its members arrive on *ordinary* endpoints rather than on the token
 * call: `8005` 토큰이 유효하지 않습니다 and `8010` 토큰을 발급받은 IP와 요청 IP가 다릅니다. Reported
 * as an ordinary protocol failure, an expired token would never be refreshed -- the refresh path keys on
 * `AUTHENTICATION` -- and every later request would fail identically until the process restarted.
 *
 * `8104` (모의투자에서 지원하지 않는 API) and `8200` (법인 고객 미지원) are the exceptions in that
 * family, and they are `INVALID_ARGUMENT` rather than `AUTHENTICATION` because they are
 * **availability, not authentication**: the credential is fine and this endpoint is not offered to
 * it. Decision 2 settled exactly this distinction for KIS's demo accounts -- capability is compile
 * time, availability is run time -- so one binary must not claim different things for a live and a
 * demo key.
 *
 * An undocumented code is `PROTOCOL` with Kiwoom's own code and message preserved, which is the
 * category the KIS adapter gives a declared failure it does not otherwise classify.
 */
inline ErrorCategory kiwoom_error_category(std::string_view code) noexcept {
  // Header-level authentication faults: the authorization field is missing or malformed.
  if (code == "1512" || code == "1513" || code == "1514" || code == "1515" || code == "1516") {
    return ErrorCategory::AUTHENTICATION;
  }
  // Availability, not authentication. Checked before the 80xx sweep below, which it sits inside.
  if (code == "8104" || code == "8200") {
    return ErrorCategory::INVALID_ARGUMENT;
  }
  // The key, the token, the terminal, or the registered IP.
  if (code.size() == 4 && code.front() == '8') {
    return ErrorCategory::AUTHENTICATION;
  }
  if (kiwoom_rate_limit_layer(code) != KiwoomRateLimitLayer::NONE) {
    return ErrorCategory::RATE_LIMIT;
  }
  // 재귀 호출이 발생하여 API 호출을 제한합니다: a throttle, stated as one.
  if (code == "1687") {
    return ErrorCategory::RATE_LIMIT;
  }
  if (code == "1501" || code == "1504" || code == "1505" || code == "1511" || code == "1517" ||
      code == "1901" || code == "1902" || code == "1903") {
    return ErrorCategory::INVALID_ARGUMENT;
  }
  return ErrorCategory::PROTOCOL;
}

/**
 * Returns the failure a Kiwoom response body declares, or nothing when it declares success.
 *
 * **Kiwoom states a business failure in the body of an HTTP 200.** Every response carries
 * `return_code` — zero on success — and `return_msg`, so a 2xx status is not evidence that the
 * request did what was asked. Checking it in one place is what keeps a parser from reading a
 * refusal as data.
 *
 * The classification itself is `kiwoom_error_category`, which is driven from the whole of the
 * specification's `errorCodeList`. An undocumented code is `PROTOCOL` -- the category the KIS
 * adapter gives a declared failure it does not otherwise classify -- and `ErrorCategory` has no
 * "broker" member to fall back to.
 *
 * `reached_broker_` stays true for every classified failure: Kiwoom answered. The token path is
 * the one exception and marks its own failures, for the reason both other adapters record -- an
 * unmarked authentication failure reads to a runner as the broker refusing an order it never
 * received, spending rate-limit budget on nothing.
 */
inline std::optional<BrokerError> kiwoom_declared_error(const json::object& document) {
  const auto code = document.if_contains("return_code");
  if (!code) {
    return std::nullopt;
  }
  std::string text;
  if (code->is_int64()) {
    text = std::to_string(code->as_int64());
  } else if (code->is_uint64()) {
    text = std::to_string(code->as_uint64());
  } else if (code->is_string()) {
    text = std::string(code->as_string());
  } else {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = "invalid_return_code";
    error.message_ = "return_code is neither a number nor a string";
    return error;
  }
  if (text == "0") {
    return std::nullopt;
  }

  BrokerError error;
  error.broker_code_ = text;
  error.reached_broker_ = true;
  if (const auto message = document.if_contains("return_msg"); message && message->is_string()) {
    error.message_ = std::string(message->as_string());
  } else {
    error.message_ = "Kiwoom declared return_code " + text;
  }
  error.category_ = kiwoom_error_category(text);
  return error;
}

} // namespace algotradelib::kiwoom
