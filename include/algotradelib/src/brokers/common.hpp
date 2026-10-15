#pragma once

/**
 * @brief Defined as 1 once a broker adapter header is included.
 *
 * The library's tests use it to check that the strategy headers include no broker.
 */
#define ATL_BROKER_ADAPTER_PRESENT 1

#include <algotradelib/network>

#include <boost/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace algotradelib::broker_detail {

// Reads a whole-number header, or nothing when it is absent or malformed. Which headers carry meaning
// is a broker's own contract, so each adapter reads the ones its documentation promises; only the
// parsing lives here.
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

// The most of a reply body an error message carries. A broker's error envelope (a code, a message, a
// request id) fits easily; whatever else arrives is not copied whole into logs and journals.
inline constexpr std::size_t reply_excerpt_limit = 512;

// Returns `body` whole if it fits in `limit` bytes; otherwise its first `limit` bytes, cut back so no
// UTF-8 character is split (KIS and Kiwoom write Korean), and how many bytes were left out.
inline std::string reply_excerpt(std::string_view body, std::size_t limit = reply_excerpt_limit) {
  if (body.size() <= limit) {
    return std::string(body);
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(body[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  return std::string(body.substr(0, cut)) + "... (" + std::to_string(body.size() - cut) + " more bytes)";
}

inline BrokerError http_error(const network::HttpResponse& response) {
  BrokerError error;
  error.http_status_ = response.result_int();
  error.category_ = response.result() == http::status::too_many_requests ?
    ErrorCategory::RATE_LIMIT :
    ErrorCategory::HTTP;
  error.message_ = reply_excerpt(response.body());
  return error;
}

// Reads a response's body as a JSON object. A body that is not one is `PROTOCOL` `invalid_json`, with the
// response's HTTP status kept and the parser's message, never the body.
inline std::optional<json::object> parse_object(const network::HttpResponse& response, BrokerError& error) {
  boost::system::error_code ec;
  auto value = json::parse(response.body(), ec);
  if (ec || !value.is_object()) {
    error.category_ = ErrorCategory::PROTOCOL;
    error.http_status_ = response.result_int();
    error.broker_code_ = "invalid_json";
    error.message_ = ec ? ec.message() : "JSON response is not an object";
    return std::nullopt;
  }
  return std::move(value.as_object());
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

// Reads `YYYYMMDD` as a calendar date, or nothing when it is not one. Every character is checked as
// a digit before any arithmetic, and the result must be a real date: `std::stoi` per component would
// read `"2026092x"` as 2026-09-02, and unchecked digit arithmetic would read `"2026090:"` as
// 2026-09-10, since `':'` follows `'9'`.
inline std::optional<Date> compact_date(std::string_view text) {
  if (!decimal_digits(text, 8)) {
    return std::nullopt;
  }
  const auto number = [text](std::size_t offset, std::size_t length) {
    int value = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
      value = value * 10 + (text[index] - '0');
    }
    return value;
  };
  Date date{
    number(0, 4), static_cast<unsigned>(number(4, 2)), static_cast<unsigned>(number(6, 2))
  };
  return is_valid_date(date) ? std::optional<Date>(date) : std::nullopt;
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
