#pragma once

#include <algotradelib/network>

#include <boost/json.hpp>

#include <charconv>
#include <chrono>
#include <optional>
#include <string>

namespace algotradelib::broker_detail {

inline std::optional<std::chrono::seconds> retry_after(const network::HttpResponse& response) {
  const auto value = response.base().find("Retry-After");
  if (value == response.base().end()) {
    return std::nullopt;
  }
  const auto text = value->value();
  std::int64_t seconds{0};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || seconds < 0) {
    return std::nullopt;
  }
  return std::chrono::seconds(seconds);
}

inline BrokerError http_error(const network::HttpResponse& response) {
  BrokerError error;
  error.http_status = response.result_int();
  error.category = response.result() == http::status::too_many_requests ?
    ErrorCategory::RATE_LIMIT :
    ErrorCategory::HTTP;
  error.message = response.body();
  error.retry_after = retry_after(response);
  return error;
}

inline std::optional<json::object> parse_object(std::string_view body, BrokerError& error) {
  boost::system::error_code ec;
  auto value = json::parse(body, ec);
  if (ec || !value.is_object()) {
    error.category = ErrorCategory::PROTOCOL;
    error.broker_code = "invalid_json";
    error.message = ec ? ec.message() : "JSON response is not an object";
    return std::nullopt;
  }
  return value.as_object();
}

inline std::string json_string(const json::object& object, std::string_view key) {
  const auto value = object.if_contains(key);
  if (!value || !value->is_string()) {
    throw std::runtime_error("missing string field: " + std::string(key));
  }
  return std::string(value->as_string());
}

inline bool valid_symbol(std::string_view symbol) {
  if (symbol.empty()) {
    return false;
  }
  for (const char ch : symbol) {
    const bool valid = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '-';
    if (!valid) {
      return false;
    }
  }
  return true;
}

} // namespace algotradelib::broker_detail
