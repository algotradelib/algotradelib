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

/**
 * @brief Which order instruction a reply answers.
 *
 * Fixed where the request is built and carried to the parser with it, never recovered from the
 * reply. Everything that is not an order request is `UNCLASSIFIED`.
 */
enum class KiwoomOrderOperation {
  /** @brief Not an order request, or one this client does not classify. */
  UNCLASSIFIED,
  /** @brief A placement (`kt10000`, `kt10001`). */
  PLACE,
  /** @brief An amendment (`kt10002`). */
  AMEND,
  /** @brief A cancellation (`kt10003`). */
  CANCEL,
};

/**
 * @brief Reads the code inside an order refusal's `return_msg`, which Kiwoom writes as
 *   `[2000](CODE:text)` under the generic `return_code` 20.
 *
 * The whole message must have that shape, from its first character to its last: the literal
 * `[2000](`, a code of one or more of `A`-`Z` and `0`-`9`, a colon, some text, and a closing
 * parenthesis as the last character. A code that only appears somewhere in a message is not read.
 *
 * @param message Kiwoom's `return_msg`.
 * @return The code, a view into `message`, or an empty view if the message has another shape.
 */
inline std::string_view kiwoom_refusal_code(std::string_view message) noexcept {
  constexpr std::string_view prefix = "[2000](";
  if (message.size() < prefix.size() || message.substr(0, prefix.size()) != prefix || message.back() != ')') {
    return {};
  }
  const auto inner = message.substr(prefix.size(), message.size() - prefix.size() - 1);
  const auto colon = inner.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == inner.size()) {
    return {};
  }
  const auto code = inner.substr(0, colon);
  for (const char character : code) {
    if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))) {
      return {};
    }
  }
  return code;
}

/**
 * @brief Reads the refusal a cash-order reply states, for the replies this client recognizes as one.
 *
 * From the demo server: a placement answered `800033` (not enough to sell), `RC4057` (the market has
 * not opened) or `RC4058` (the market has closed), and a cancellation answered `RC4033` (nothing left to amend or cancel). From the real
 * server: a placement answered `571445` (no securities balance) and a cancellation answered `506550`
 * (no quantity left to cancel). Such a reply is `INVALID_ARGUMENT` with `reached_broker_` set, which
 * a runner reads as a rejection; any other reply with `return_code` 20 stays `PROTOCOL`, an unknown
 * outcome.
 *
 * Each code is recognized from the server it was seen from only: a demo code from the real server,
 * or a real code from the demo server, is not. The reply must be HTTP 200 with `return_code` 20 (a
 * JSON integer, or the string `"20"`), a `return_msg` of the shape `kiwoom_refusal_code` reads, and
 * none of what an acknowledgement carries: `ord_no` absent or empty, and `cncl_qty` and `mdfy_qty` absent, empty or
 * all zeros. A reply acknowledging a new order is never read as a refusal (the original order's
 * number, echoed in `base_orig_ord_no`, is not an acknowledgement). A refused cancellation says only
 * that the cancellation did nothing, not what became of the order it named.
 *
 * @param http_status The reply's HTTP status.
 * @param document The reply body.
 * @param operation The instruction the reply answers.
 * @param demo Whether the request went to the demo server.
 * @return The error (`broker_code_` the inner code, `message_` Kiwoom's `return_msg` followed by the
 *   outer code and the rule that matched), or `std::nullopt` if the reply is not one of them.
 */
inline std::optional<BrokerError> kiwoom_recognized_refusal(
  int http_status,
  const boost::json::object& document,
  KiwoomOrderOperation operation,
  bool demo
) {
  if (http_status != 200) {
    return std::nullopt;
  }
  const auto code = document.if_contains("return_code");
  const bool generic = code && (
    (code->is_int64() && code->as_int64() == 20) || (code->is_uint64() && code->as_uint64() == 20) ||
    (code->is_string() && code->as_string() == "20")
  );
  const auto message = document.if_contains("return_msg");
  if (!generic || !message || !message->is_string()) {
    return std::nullopt;
  }
  const std::string stated(kiwoom_refusal_code(std::string_view(message->as_string())));
  const char* name = "";
  const bool placement = operation == KiwoomOrderOperation::PLACE &&
    (demo ? (stated == "800033" || stated == "RC4057" || stated == "RC4058") : stated == "571445");
  const bool cancellation = operation == KiwoomOrderOperation::CANCEL &&
    (demo ? stated == "RC4033" : stated == "506550");
  if (placement) {
    name = "placement";
  } else if (cancellation) {
    name = "cancellation";
  } else {
    return std::nullopt;
  }
  if (const auto number = document.if_contains("ord_no");
      number && !(number->is_string() && number->as_string().empty())) {
    return std::nullopt;
  }
  for (const char* field : {"cncl_qty", "mdfy_qty"}) {
    const auto quantity = document.if_contains(field);
    if (!quantity) {
      continue;
    }
    if (!quantity->is_string() ||
        std::string_view(quantity->as_string()).find_first_not_of('0') != std::string_view::npos) {
      return std::nullopt;
    }
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = true;
  error.http_status_ = http_status;
  error.broker_code_ = stated;
  error.message_ = std::string(message->as_string()) + " (return_code 20; recognized refusal: Kiwoom " +
    (demo ? "demo" : "real") + ", cash " + name + ", " + stated + ")";
  return error;
}

} // namespace algotradelib::kiwoom
