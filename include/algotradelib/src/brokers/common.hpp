#pragma once

#include <algotradelib/network>

#include <boost/json.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace algotradelib::broker_detail {

/** Reads a whole-number header, or nothing when it is absent or malformed. */
inline std::optional<std::int64_t> integer_header(
  const network::HttpResponse& response,
  std::string_view name
) {
  const auto value = response.base().find(name);
  if (value == response.base().end()) {
    return std::nullopt;
  }
  const auto text = value->value();
  std::int64_t number{0};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < 0) {
    return std::nullopt;
  }
  return number;
}

inline std::optional<std::chrono::seconds> retry_after(const network::HttpResponse& response) {
  const auto seconds = integer_header(response, "Retry-After");
  if (!seconds) {
    return std::nullopt;
  }
  return std::chrono::seconds(*seconds);
}

/**
 * Reads the allowance a broker says it is enforcing, in requests per second.
 *
 * Toss documents `X-RateLimit-Limit` as the burst capacity per second on its
 * rate-limit response. It is treated as observed-when-present and never
 * required, because the header is documented on the refusal and not promised on
 * a successful response.
 */
inline std::optional<int> reported_rate_limit(const network::HttpResponse& response) {
  const auto limit = integer_header(response, "X-RateLimit-Limit");
  if (!limit || *limit < 1 || *limit > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(*limit);
}

inline BrokerError http_error(const network::HttpResponse& response) {
  BrokerError error;
  error.http_status_ = response.result_int();
  error.category_ = response.result() == http::status::too_many_requests ?
    ErrorCategory::RATE_LIMIT :
    ErrorCategory::HTTP;
  error.message_ = response.body();
  if (error.category_ == ErrorCategory::RATE_LIMIT) {
    error.retry_after_ = retry_after(response);
    error.rate_limit_ = reported_rate_limit(response);
  }
  return error;
}

inline std::optional<json::object> parse_object(std::string_view body, BrokerError& error) {
  boost::system::error_code ec;
  auto value = json::parse(body, ec);
  if (ec || !value.is_object()) {
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = "invalid_json";
    error.message_ = ec ? ec.message() : "JSON response is not an object";
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

inline std::int64_t json_int64(const json::object& object, std::string_view key) {
  const auto value = object.if_contains(key);
  if (!value) {
    throw std::runtime_error("missing integer field: " + std::string(key));
  }
  if (value->is_int64()) {
    return value->as_int64();
  }
  if (
    value->is_uint64() &&
    value->as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
  ) {
    return static_cast<std::int64_t>(value->as_uint64());
  }
  throw std::runtime_error("invalid integer field: " + std::string(key));
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

inline std::string query_encode(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
      (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
    if (unreserved) {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('%');
      result.push_back(hex[ch >> 4]);
      result.push_back(hex[ch & 0x0F]);
    }
  }
  return result;
}

inline bool decimal_digits(std::string_view value, std::size_t length) {
  if (value.size() != length) {
    return false;
  }
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

inline Decimal64 percent_to_ratio(std::string_view value) {
  const auto percent = Decimal64::parse(value);
  if (percent.scale() > std::numeric_limits<std::uint8_t>::max() - 2) {
    throw std::out_of_range("percentage scale exceeds Decimal64 range");
  }

  auto coefficient = percent.coefficient();
  auto scale = static_cast<std::uint8_t>(percent.scale() + 2);
  while (scale > 0 && coefficient % 10 == 0) {
    coefficient /= 10;
    --scale;
  }
  return Decimal64(coefficient, scale);
}

} // namespace algotradelib::broker_detail
