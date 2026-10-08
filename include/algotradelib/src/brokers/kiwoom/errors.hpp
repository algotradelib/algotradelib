#pragma once

#include "../../core/types.hpp"

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace algotradelib::kiwoom {

/**
 * @brief Which of Kiwoom's three rate limits a refusal reports (per API, account-wide, or per API
 *   group); backing off the wrong one does not help.
 */
enum class KiwoomRateLimitLayer {
  /** @brief Not a rate-limit code. */
  NONE,
  /** @brief `1700`: this API's own allowance. */
  ENDPOINT,
  /** @brief `1701`: the total allowance across every API. */
  TOTAL,
  /** @brief `1702`: the allowance of the API group this endpoint belongs to. */
  GROUP,
};

/**
 * @brief Classifies a Kiwoom return code as a rate-limit refusal.
 * @param code Kiwoom's `return_code`, as text.
 * @return The layer, or `NONE` if the code is not a rate-limit refusal.
 */
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
 * @brief Classifies a Kiwoom `return_code`.
 *
 * - `AUTHENTICATION`: `1512`-`1516`, and any four-digit code starting with `8` (key, token, terminal,
 *   registered IP) except `8104` and `8200`;
 * - `INVALID_ARGUMENT`: `8104` (not offered on demo), `8200` (not offered to corporate customers),
 *   `1501`, `1504`, `1505`, `1511`, `1517` and `1901`-`1903`;
 * - `RATE_LIMIT`: `1700`-`1702` and `1687`;
 * - `PROTOCOL`: any other code.
 *
 * The client does not discard a token Kiwoom refuses: requests may keep failing until five minutes
 * before the token's stated expiry.
 *
 * @param code The code, as text.
 * @return The category.
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
 * @brief Reads the failure a Kiwoom response declares in its body (Kiwoom reports business failures
 *   with HTTP 200 and a nonzero `return_code`).
 * @param document The response body.
 * @return The error (`reached_broker_` set, classified by `kiwoom_error_category`, Kiwoom's code and
 *   `return_msg` kept), a `PROTOCOL` error if `return_code` is neither a number nor a string, or
 *   `std::nullopt` if it is `0` or absent.
 */
inline std::optional<BrokerError> kiwoom_declared_error(const boost::json::object& document) {
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
