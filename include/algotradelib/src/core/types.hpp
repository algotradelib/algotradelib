#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace algotradelib {

enum class Country {
  KR,
  US,
};

enum class Venue {
  UNSPECIFIED,
  KRX,
  NXT,
  COMBINED,
};

struct InstrumentId {
  Country country{Country::KR};
  Venue venue{Venue::COMBINED};
  std::string symbol;

  friend bool operator==(
    const InstrumentId& lhs,
    const InstrumentId& rhs
  ) {
    return lhs.country == rhs.country && lhs.venue == rhs.venue && lhs.symbol == rhs.symbol;
  }
};

class Currency {
public:
  Currency() : code_{'K', 'R', 'W'} {}

  explicit Currency(
    std::string_view code
  ) {
    if (code.size() != code_.size()) {
      throw std::invalid_argument("currency code must contain three characters");
    }
    for (std::size_t i = 0; i < code_.size(); ++i) {
      const char ch = code[i];
      if (ch < 'A' || ch > 'Z') {
        throw std::invalid_argument("currency code must use uppercase ASCII characters");
      }
      code_[i] = ch;
    }
  }

  static Currency krw() { return Currency("KRW"); }
  static Currency usd() { return Currency("USD"); }

  std::string str() const { return std::string(code_.begin(), code_.end()); }

  friend bool operator==(
    const Currency& lhs,
    const Currency& rhs
  ) {
    return lhs.code_ == rhs.code_;
  }

private:
  std::array<char, 3> code_{};
};

class Decimal64 {
public:
  Decimal64() = default;
  Decimal64(
    std::int64_t coefficient,
    std::uint8_t scale
  )
  : coefficient_(coefficient), scale_(scale) {}

  static Decimal64 parse(
    std::string_view input
  ) {
    if (input.empty()) {
      throw std::invalid_argument("decimal value is empty");
    }

    bool negative = false;
    std::size_t index = 0;
    if (input.front() == '+' || input.front() == '-') {
      negative = input.front() == '-';
      index = 1;
    }
    if (index == input.size()) {
      throw std::invalid_argument("decimal value has no digits");
    }

    std::uint64_t magnitude = 0;
    std::uint8_t scale = 0;
    bool decimal_point = false;
    bool has_digit = false;
    const std::uint64_t limit = negative ?
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL :
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    for (; index < input.size(); ++index) {
      const char ch = input[index];
      if (ch == '.') {
        if (decimal_point) {
          throw std::invalid_argument("decimal value contains multiple points");
        }
        decimal_point = true;
        continue;
      }
      if (ch < '0' || ch > '9') {
        throw std::invalid_argument("decimal value contains an invalid character");
      }
      has_digit = true;
      const auto digit = static_cast<std::uint64_t>(ch - '0');
      if (magnitude > (limit - digit) / 10ULL) {
        throw std::out_of_range("decimal value exceeds Decimal64 range");
      }
      magnitude = magnitude * 10ULL + digit;
      if (decimal_point) {
        if (scale == std::numeric_limits<std::uint8_t>::max()) {
          throw std::out_of_range("decimal scale exceeds Decimal64 range");
        }
        ++scale;
      }
    }
    if (!has_digit) {
      throw std::invalid_argument("decimal value has no digits");
    }

    while (scale > 0 && magnitude % 10ULL == 0) {
      magnitude /= 10ULL;
      --scale;
    }

    std::int64_t coefficient;
    if (
      negative &&
      magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL
    ) {
      coefficient = std::numeric_limits<std::int64_t>::min();
    } else {
      coefficient = static_cast<std::int64_t>(magnitude);
      if (negative) {
        coefficient = -coefficient;
      }
    }
    return Decimal64(coefficient, scale);
  }

  std::int64_t coefficient() const noexcept { return coefficient_; }
  std::uint8_t scale() const noexcept { return scale_; }

  std::string str() const {
    const bool negative = coefficient_ < 0;
    std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(coefficient_ + 1)) + 1ULL :
                                         static_cast<std::uint64_t>(coefficient_);
    std::string digits = std::to_string(magnitude);
    if (scale_ > 0) {
      if (digits.size() <= scale_) {
        digits.insert(0, static_cast<std::size_t>(scale_) - digits.size() + 1, '0');
      }
      digits.insert(digits.size() - scale_, 1, '.');
    }
    if (negative) {
      digits.insert(digits.begin(), '-');
    }
    return digits;
  }

  friend bool operator==(
    const Decimal64& lhs,
    const Decimal64& rhs
  ) {
    return lhs.coefficient_ == rhs.coefficient_ && lhs.scale_ == rhs.scale_;
  }

private:
  std::int64_t coefficient_{0};
  std::uint8_t scale_{0};
};

enum class ErrorCategory {
  CANCELLED,
  TRANSPORT,
  HTTP,
  AUTHENTICATION,
  RATE_LIMIT,
  PROTOCOL,
  INVALID_ARGUMENT,
};

struct BrokerError {
  ErrorCategory category{ErrorCategory::PROTOCOL};
  int http_status{0};
  std::string broker_code;
  std::string request_id;
  std::string message;
  std::optional<std::chrono::seconds> retry_after;
};

template <typename T>
class Result {
public:
  Result(
    T value
  )
  : value_(std::move(value)) {}
  Result(
    BrokerError error
  )
  : value_(std::move(error)) {}

  bool has_value() const noexcept { return std::holds_alternative<T>(value_); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() { return std::get<T>(value_); }
  const T& value() const { return std::get<T>(value_); }
  BrokerError& error() { return std::get<BrokerError>(value_); }
  const BrokerError& error() const { return std::get<BrokerError>(value_); }

private:
  std::variant<T, BrokerError> value_;
};

struct QuoteSnapshot {
  InstrumentId instrument;
  Decimal64 last_price;
  Currency currency;
  std::optional<std::chrono::system_clock::time_point> exchange_time;
  std::chrono::system_clock::time_point received_at;
};

inline std::optional<std::chrono::system_clock::time_point> parse_iso8601_timestamp(
  std::string_view value
) {
  int year, month, day, hour, minute, second;
  if (
    std::sscanf(
      std::string(value).c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second
    ) != 6
  ) {
    return std::nullopt;
  }

  const auto timezone_position = value.find_first_of("Z+-", 19);
  if (timezone_position == std::string_view::npos) {
    return std::nullopt;
  }

  std::tm time{};
  time.tm_year = year - 1900;
  time.tm_mon = month - 1;
  time.tm_mday = day;
  time.tm_hour = hour;
  time.tm_min = minute;
  time.tm_sec = second;
  time.tm_isdst = 0;
#ifdef _WIN32
  std::time_t utc = _mkgmtime(&time);
#else
  std::time_t utc = timegm(&time);
#endif
  if (utc == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }

  if (value[timezone_position] != 'Z') {
    int offset_hour, offset_minute;
    if (
      std::sscanf(
        std::string(value.substr(timezone_position + 1)).c_str(),
        "%d:%d",
        &offset_hour,
        &offset_minute
      ) != 2
    ) {
      return std::nullopt;
    }
    const int offset = offset_hour * 3600 + offset_minute * 60;
    utc += value[timezone_position] == '+' ? -offset : offset;
  }

  auto result = std::chrono::system_clock::from_time_t(utc);
  const auto dot = value.find('.', 19);
  if (dot != std::string_view::npos && dot < timezone_position) {
    std::uint32_t milliseconds = 0;
    std::uint32_t factor = 100;
    for (std::size_t index = dot + 1; index < timezone_position && factor > 0; ++index) {
      if (value[index] < '0' || value[index] > '9') {
        return std::nullopt;
      }
      milliseconds += static_cast<std::uint32_t>(value[index] - '0') * factor;
      factor /= 10;
    }
    result += std::chrono::milliseconds(milliseconds);
  }
  return result;
}

} // namespace algotradelib
