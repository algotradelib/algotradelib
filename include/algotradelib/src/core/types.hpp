#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
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
#include <vector>

namespace algotradelib {

/**
 * @brief A country (market) the library supports.
 *
 * Adding a country requires its broker mappings; an unknown wire value is never mapped onto one
 * of these.
 */
enum class Country {
  KR,
  US,
};

/**
 * @brief A trading venue: an exchange, or the combined view across exchanges.
 *
 * `UNSPECIFIED` leaves routing to the broker's default. Broker wire codes stay in the adapters.
 */
enum class Venue {
  UNSPECIFIED,
  KRX,
  NXT,
  COMBINED,
};

/**
 * @brief Returns a display name for a venue, such as `"KRX"`.
 * @param venue The venue.
 * @return The name.
 */
inline constexpr std::string_view venue_name(Venue venue) noexcept {
  switch (venue) {
    case Venue::UNSPECIFIED:
      return "unspecified";
    case Venue::KRX:
      return "KRX";
    case Venue::NXT:
      return "NXT";
    case Venue::COMBINED:
      return "COMBINED";
  }
  return "unspecified";
}

/** @brief Identifies an instrument: its country, venue and symbol. */
struct InstrumentId {
  /** @brief The listing country, which selects the symbol convention and the price grid. */
  Country country_{Country::KR};
  /**
   * @brief The venue that is part of the identity (two venues are two instruments).
   *
   * Where an order is routed is `OrderRequest::venue_`, not this.
   */
  Venue venue_{Venue::COMBINED};
  /** @brief The symbol as the venue spells it, such as `"005930"`. */
  std::string symbol_;

  /**
   * @brief Compares country, venue and symbol.
   * @param lhs The first identity.
   * @param rhs The second identity.
   * @return `true` if all three are equal.
   */
  friend bool operator==(const InstrumentId& lhs, const InstrumentId& rhs) {
    return lhs.country_ == rhs.country_ && lhs.venue_ == rhs.venue_ && lhs.symbol_ == rhs.symbol_;
  }

  /**
   * @brief Orders identities by country, venue, then symbol, so they can key a map.
   *
   * The order has no meaning beyond that, and it is consistent with `operator==`.
   *
   * @param lhs The first identity.
   * @param rhs The second identity.
   * @return `true` if `lhs` sorts before `rhs`.
   */
  friend bool operator<(const InstrumentId& lhs, const InstrumentId& rhs) {
    if (lhs.country_ != rhs.country_) {
      return lhs.country_ < rhs.country_;
    }
    if (lhs.venue_ != rhs.venue_) {
      return lhs.venue_ < rhs.venue_;
    }
    return lhs.symbol_ < rhs.symbol_;
  }
};

/**
 * @brief A short uppercase code (letters, digits, `_`), stored inline without allocation.
 *
 * The length is stored in one byte, so `N` must be at most 255.
 *
 * Holds a broker code the library may not recognize (a market segment, an exchange id) exactly as
 * sent, so it can be carried, compared and displayed.
 *
 * @tparam N The maximum number of characters.
 */
template <std::size_t N>
class Code {
public:
  /** @brief Constructs an empty code. */
  Code() = default;

  /**
   * @brief Constructs a code.
   *
   * @param text Up to `N` characters, each `A`-`Z`, `0`-`9` or `_`.
   * @throws std::invalid_argument if `text` is longer than `N` or has any other character.
   */
  explicit Code(std::string_view text) {
    if (text.size() > N) {
      throw std::invalid_argument("code is longer than its capacity");
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char ch = text[i];
      const bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
      if (!valid) {
        throw std::invalid_argument("code must use uppercase ASCII, digits, or underscore");
      }
      characters_[i] = ch;
    }
    size_ = static_cast<std::uint8_t>(text.size());
  }

  /**
   * @brief Makes a code without throwing.
   *
   * @param text Up to `N` characters, each `A`-`Z`, `0`-`9` or `_`.
   * @return The code, or `std::nullopt` if `text` is too long or has any other character.
   */
  static std::optional<Code> try_make(std::string_view text) {
    if (text.size() > N) {
      return std::nullopt;
    }
    Code code;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char ch = text[i];
      const bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
      if (!valid) {
        return std::nullopt;
      }
      code.characters_[i] = ch;
    }
    code.size_ = static_cast<std::uint8_t>(text.size());
    return code;
  }

  /**
   * @brief Returns the code's characters.
   * @return A view of the code's own storage, valid while the code is alive.
   */
  std::string_view view() const noexcept {
    return std::string_view(characters_.data(), size_);
  }

  /**
   * @brief Returns the code as a string.
   * @return A copy of the characters.
   */
  std::string str() const { return std::string(view()); }

  /**
   * @brief Tests whether the code is empty.
   * @return `true` if it has no characters.
   */
  bool empty() const noexcept { return size_ == 0; }

  /**
   * @brief Returns the number of characters.
   * @return The length, at most `N`.
   */
  std::size_t size() const noexcept { return size_; }

  /**
   * @brief Returns the maximum number of characters.
   * @return `N`.
   */
  static constexpr std::size_t capacity() noexcept { return N; }

  /**
   * @brief Compares two codes' characters.
   * @param lhs The first code.
   * @param rhs The second code.
   * @return `true` if they are equal.
   */
  friend bool operator==(const Code& lhs, const Code& rhs) { return lhs.view() == rhs.view(); }

  /**
   * @brief Compares two codes for inequality.
   * @param lhs The first code.
   * @param rhs The second code.
   * @return `true` if they differ.
   */
  friend bool operator!=(const Code& lhs, const Code& rhs) { return !(lhs == rhs); }

private:
  std::array<char, N> characters_{};
  std::uint8_t size_{0};
};

/** @brief A three-letter currency code, such as `KRW` or `USD` (ISO 4217 style). */
class Currency {
public:
  /** @brief Constructs Korean won. */
  Currency() : code_{'K', 'R', 'W'} {}

  /**
   * @brief Constructs a currency from its code.
   *
   * @param code Three uppercase ASCII letters.
   * @throws std::invalid_argument if `code` is not three uppercase ASCII letters.
   */
  explicit Currency(std::string_view code) {
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

  /**
   * @brief Returns Korean won.
   * @return `KRW`.
   */
  static Currency krw() { return Currency("KRW"); }

  /**
   * @brief Returns United States dollars.
   * @return `USD`.
   */
  static Currency usd() { return Currency("USD"); }

  /**
   * @brief Returns the code.
   * @return The three letters.
   */
  std::string str() const { return std::string(code_.begin(), code_.end()); }

  /**
   * @brief Compares two currencies.
   * @param lhs The first currency.
   * @param rhs The second currency.
   * @return `true` if the codes are equal.
   */
  friend bool operator==(const Currency& lhs, const Currency& rhs) {
    return lhs.code_ == rhs.code_;
  }

private:
  std::array<char, 3> code_{};
};

/**
 * @brief An exact signed decimal: an `int64` coefficient times `10^-scale`.
 *
 * Prices, quantities and money are `Decimal64` wherever they come from or go to a broker. The
 * arithmetic is the `decimal_*` free functions, which return `std::nullopt` when a coefficient,
 * scale or intermediate value does not fit (a result whose final value would fit can still fail).
 */
class Decimal64 {
public:
  /** @brief Constructs zero. */
  Decimal64() = default;

  /**
   * @brief Constructs `coefficient * 10^-scale`, keeping the scale as given.
   * @param coefficient The integer coefficient.
   * @param scale The number of fractional digits.
   */
  Decimal64(std::int64_t coefficient, std::uint8_t scale)
  : coefficient_(coefficient), scale_(scale) {}

  /**
   * @brief Parses a decimal string exactly (no binary floating point).
   *
   * Accepts an optional sign, digits and at most one `.`. Trailing fractional zeros are removed,
   * so `parse("1.50") == parse("1.5")`.
   *
   * @param input The text, such as `"-12.345"`.
   * @return The value.
   * @throws std::invalid_argument if `input` is empty, has no digits, has more than one `.`, or
   *   contains any other character.
   * @throws std::out_of_range if the digits as written (before trailing zeros are removed) or the
   *   scale do not fit.
   */
  static Decimal64 parse(std::string_view input) {
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

  /**
   * @brief Returns the coefficient.
   * @return The signed integer coefficient.
   */
  std::int64_t coefficient() const noexcept { return coefficient_; }

  /**
   * @brief Returns the scale.
   * @return The number of fractional digits.
   */
  std::uint8_t scale() const noexcept { return scale_; }

  /**
   * @brief Formats the value as a decimal string.
   * @return The text, with exactly `scale()` fractional digits (no exponent).
   */
  std::string str() const {
    const bool negative = coefficient_ < 0;
    std::uint64_t magnitude = negative ?
      static_cast<std::uint64_t>(-(coefficient_ + 1)) + 1ULL :
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

  /**
   * @brief Compares the coefficient **and** the scale.
   *
   * Values from `parse` and the `decimal_*` functions are normalized, so for them this compares
   * the numbers. A value built with an explicit scale is not: `Decimal64(150, 2)` does not equal
   * `Decimal64(15, 1)`. To compare numbers at any scale, use `decimal_less` both ways or
   * `decimal_normalized` first.
   *
   * @param lhs The first value.
   * @param rhs The second value.
   * @return `true` if coefficient and scale are both equal.
   */
  friend bool operator==(const Decimal64& lhs, const Decimal64& rhs) {
    return lhs.coefficient_ == rhs.coefficient_ && lhs.scale_ == rhs.scale_;
  }

private:
  std::int64_t coefficient_{0};
  std::uint8_t scale_{0};
};

/**
 * @brief Removes trailing fractional zeros, so equal numbers compare equal with `==`.
 * @param value The value.
 * @return The same number at the smallest scale that represents it.
 */
inline Decimal64 decimal_normalized(const Decimal64& value) {
  auto coefficient = value.coefficient();
  auto scale = value.scale();
  while (scale > 0 && coefficient % 10 == 0) {
    coefficient /= 10;
    --scale;
  }
  return Decimal64(coefficient, scale);
}

namespace decimal_detail {

/**
 * Rescales both values to their common scale.
 *
 * Returns false when the rescaling would not fit, which is the only way exact
 * fixed-width arithmetic can fail.
 */
inline bool align(
  const Decimal64& lhs,
  const Decimal64& rhs,
  std::int64_t& left,
  std::int64_t& right,
  std::uint8_t& scale
) {
  scale = lhs.scale() > rhs.scale() ? lhs.scale() : rhs.scale();
  left = lhs.coefficient();
  right = rhs.coefficient();
  for (std::uint8_t step = lhs.scale(); step < scale; ++step) {
    if (left > std::numeric_limits<std::int64_t>::max() / 10 ||
        left < std::numeric_limits<std::int64_t>::min() / 10) {
      return false;
    }
    left *= 10;
  }
  for (std::uint8_t step = rhs.scale(); step < scale; ++step) {
    if (right > std::numeric_limits<std::int64_t>::max() / 10 ||
        right < std::numeric_limits<std::int64_t>::min() / 10) {
      return false;
    }
    right *= 10;
  }
  return true;
}

} // namespace decimal_detail

/**
 * @brief Adds two decimals exactly.
 * @param lhs The first addend.
 * @param rhs The second addend.
 * @return The normalized sum, or `std::nullopt` if it, or either operand rescaled to the larger
 *   scale, does not fit.
 */
inline std::optional<Decimal64> decimal_add(const Decimal64& lhs, const Decimal64& rhs) {
  std::int64_t left = 0;
  std::int64_t right = 0;
  std::uint8_t scale = 0;
  if (!decimal_detail::align(lhs, rhs, left, right, scale)) {
    return std::nullopt;
  }
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return std::nullopt;
  }
  return decimal_normalized(Decimal64(left + right, scale));
}

/**
 * @brief Subtracts two decimals exactly.
 * @param lhs The minuend.
 * @param rhs The subtrahend.
 * @return The normalized difference, or `std::nullopt` if it, or either operand rescaled to the
 *   larger scale, does not fit.
 */
inline std::optional<Decimal64> decimal_subtract(const Decimal64& lhs, const Decimal64& rhs) {
  std::int64_t left = 0;
  std::int64_t right = 0;
  std::uint8_t scale = 0;
  if (!decimal_detail::align(lhs, rhs, left, right, scale)) {
    return std::nullopt;
  }
  if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
      (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
    return std::nullopt;
  }
  return decimal_normalized(Decimal64(left - right, scale));
}

/**
 * @brief Halves a decimal exactly, adding a decimal place when needed.
 * @param value The value.
 * @return The normalized half, or `std::nullopt` if it does not fit.
 */
inline std::optional<Decimal64> decimal_halve(const Decimal64& value) {
  const auto coefficient = value.coefficient();
  if (coefficient % 2 == 0) {
    return decimal_normalized(Decimal64(coefficient / 2, value.scale()));
  }
  if (value.scale() == std::numeric_limits<std::uint8_t>::max() ||
      coefficient > std::numeric_limits<std::int64_t>::max() / 5 ||
      coefficient < std::numeric_limits<std::int64_t>::min() / 5) {
    return std::nullopt;
  }
  return decimal_normalized(
    Decimal64(coefficient * 5, static_cast<std::uint8_t>(value.scale() + 1))
  );
}

/**
 * @brief Multiplies two decimals exactly; the scales add.
 * @param lhs The first factor.
 * @param rhs The second factor.
 * @return The normalized product, or `std::nullopt` if the product of the coefficients or the sum of
 *   the scales does not fit (checked before trailing zeros are removed).
 */
inline std::optional<Decimal64> decimal_multiply(const Decimal64& lhs, const Decimal64& rhs) {
  const auto left = lhs.coefficient();
  const auto right = rhs.coefficient();
  const int scale = static_cast<int>(lhs.scale()) + static_cast<int>(rhs.scale());
  if (scale > std::numeric_limits<std::uint8_t>::max()) {
    return std::nullopt;
  }
  if (left != 0 && right != 0) {
    // Checked before multiplying rather than after, because signed overflow is undefined and a
    // wrapped price is exactly the failure this type exists to prevent.
    if (left > 0 ? (right > 0 ? left > std::numeric_limits<std::int64_t>::max() / right
                              : right < std::numeric_limits<std::int64_t>::min() / left)
                 : (right > 0 ? left < std::numeric_limits<std::int64_t>::min() / right
                              : left < std::numeric_limits<std::int64_t>::max() / right)) {
      return std::nullopt;
    }
  }
  return decimal_normalized(Decimal64(left * right, static_cast<std::uint8_t>(scale)));
}

/**
 * @brief Divides two decimals, truncating toward zero at a given scale.
 *
 * `decimal_divide(x, y, 0)` is the number of whole `y` in `x`, as when sizing an order in shares.
 *
 * @param lhs The dividend.
 * @param rhs The divisor.
 * @param scale The number of fractional digits to keep.
 * @return The normalized quotient, or `std::nullopt` if `rhs` is zero, or the rescaled operands or
 *   the dividend multiplied by `10^scale` do not fit.
 */
inline std::optional<Decimal64> decimal_divide(
  const Decimal64& lhs,
  const Decimal64& rhs,
  std::uint8_t scale
) {
  std::int64_t left = 0;
  std::int64_t right = 0;
  std::uint8_t common = 0;
  if (!decimal_detail::align(lhs, rhs, left, right, common) || right == 0) {
    return std::nullopt;
  }
  // Aligning cancels the scales, so `left / right` is the plain ratio. Each requested decimal
  // place is one more factor of ten taken before the division.
  for (std::uint8_t step = 0; step < scale; ++step) {
    if (left > std::numeric_limits<std::int64_t>::max() / 10 ||
        left < std::numeric_limits<std::int64_t>::min() / 10) {
      return std::nullopt;
    }
    left *= 10;
  }
  if (left == std::numeric_limits<std::int64_t>::min() && right == -1) {
    return std::nullopt;
  }
  return decimal_normalized(Decimal64(left / right, scale));
}

/**
 * @brief Rounds down to a multiple of a step (toward negative infinity).
 *
 * The result is never greater than `value`, on either side of zero.
 *
 * @param value The value.
 * @param step The step, such as a lot size; must be positive.
 * @return The largest multiple of `step` not above `value`, or `std::nullopt` if `step` is not
 *   positive or the operands rescaled to a common scale do not fit.
 * @warning A negative `value` whose rounded result is below the smallest `int64` coefficient is
 *   undefined behaviour (unchecked overflow), such as `Decimal64(INT64_MIN, 0)` with step 3.
 */
inline std::optional<Decimal64> decimal_floor_to_multiple(
  const Decimal64& value,
  const Decimal64& step
) {
  std::int64_t left = 0;
  std::int64_t right = 0;
  std::uint8_t scale = 0;
  if (!decimal_detail::align(value, step, left, right, scale) || right <= 0) {
    return std::nullopt;
  }
  auto rest = left % right;
  if (rest < 0) {
    rest += right;
  }
  return decimal_normalized(Decimal64(left - rest, scale));
}

/**
 * @brief Compares two decimals numerically, whatever their scales.
 * @param lhs The first value.
 * @param rhs The second value.
 * @return `true` if `lhs < rhs`.
 * @warning Wrong for two negative values when rescaling one to the other's scale overflows `int64`
 *   (for example `-9e18` against `-0.01`). Values of ordinary prices and quantities are unaffected.
 */
inline bool decimal_less(const Decimal64& lhs, const Decimal64& rhs) {
  std::int64_t left = 0;
  std::int64_t right = 0;
  std::uint8_t scale = 0;
  if (decimal_detail::align(lhs, rhs, left, right, scale)) {
    return left < right;
  }
  // Rescaling overflowed, so the values differ by more than any shared scale can
  // express; the one with the larger magnitude and its sign decides.
  const bool left_negative = lhs.coefficient() < 0;
  return left_negative != (rhs.coefficient() < 0) ? left_negative : lhs.scale() > rhs.scale();
}

/** @brief The kind of failure a `BrokerError` reports. */
enum class ErrorCategory {
  CANCELLED,
  TRANSPORT,
  HTTP,
  AUTHENTICATION,
  RATE_LIMIT,
  PROTOCOL,
  INVALID_ARGUMENT,
};

/**
 * @brief The error a broker or transport operation returns.
 *
 * Branch on `category_`; the other fields are for diagnosis and logs.
 */
struct BrokerError {
  /** @brief The kind of failure; the field to branch on. */
  ErrorCategory category_{ErrorCategory::PROTOCOL};
  /**
   * @brief Whether a request was sent before this error.
   *
   * `false` only when the client refused before any I/O (an order id too long, an unsupported time
   * in force); such a refusal is not counted against a rate limit and was not the broker's.
   * Defaults to `true`.
   */
  bool reached_broker_{true};
  /** @brief The HTTP status, or `0` if the failure happened before a response. */
  int http_status_{0};
  /** @brief The broker's own error code, verbatim (for example KIS's `EGW00201`). */
  std::string broker_code_;
  /** @brief The broker's request id for this call, if it gave one. */
  std::string request_id_;
  /** @brief A human-readable description, for logs; do not parse it. */
  std::string message_;
  /**
   * @brief How long the broker asked the caller to wait (`Retry-After`), for a rate-limit error.
   *
   * Empty if the broker sent none or an unreadable one; back off on your own then rather than
   * retrying at once.
   */
  std::optional<std::chrono::seconds> retry_after_;
  /**
   * @brief The rate limit the broker says it enforces, in requests per second, for a rate-limit error.
   *
   * Empty if the broker stated none or an unusable one. KIS never states one.
   */
  std::optional<int> rate_limit_;
};

/**
 * @brief Either a value or a `BrokerError`.
 *
 * Test it with `has_value()` (or `if (result)`) before calling `value()` or `error()`; the wrong
 * accessor throws `std::bad_variant_access`.
 *
 * @tparam T The value type.
 */
template <typename T>
class Result {
public:
  /**
   * @brief Constructs a successful result.
   * @param value The value.
   */
  Result(T value) : value_(std::move(value)) {}

  /**
   * @brief Constructs a failed result.
   * @param error The error.
   */
  Result(BrokerError error) : value_(std::move(error)) {}

  /**
   * @brief Tests whether the result holds a value.
   * @return `true` for success.
   */
  bool has_value() const noexcept { return std::holds_alternative<T>(value_); }

  /**
   * @brief Tests whether the result holds a value.
   * @return `true` for success.
   */
  explicit operator bool() const noexcept { return has_value(); }

  /**
   * @brief Returns the value.
   * @return The value.
   * @throws std::bad_variant_access if the result holds an error.
   */
  T& value() { return std::get<T>(value_); }

  /**
   * @brief Returns the value.
   * @return The value.
   * @throws std::bad_variant_access if the result holds an error.
   */
  const T& value() const { return std::get<T>(value_); }

  /**
   * @brief Returns the error.
   * @return The error.
   * @throws std::bad_variant_access if the result holds a value.
   */
  BrokerError& error() { return std::get<BrokerError>(value_); }

  /**
   * @brief Returns the error.
   * @return The error.
   * @throws std::bad_variant_access if the result holds a value.
   */
  const BrokerError& error() const { return std::get<BrokerError>(value_); }

private:
  std::variant<T, BrokerError> value_;
};

/**
 * @brief The best bid and offer, with the size at each. An absent field was not stated, never zero.
 */
struct TopOfBook {
  /** @brief The best bid price. */
  std::optional<Decimal64> bid_price_;
  /** @brief The size at the best bid. */
  std::optional<Decimal64> bid_quantity_;
  /** @brief The best offer price. */
  std::optional<Decimal64> ask_price_;
  /** @brief The size at the best offer. */
  std::optional<Decimal64> ask_quantity_;
};

/** @brief A quote: the last traded price of an instrument at a point in time. */
struct QuoteSnapshot {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The last traded price, as the broker reported it. */
  Decimal64 last_price_;
  /** @brief The currency of `last_price_`. */
  Currency currency_;
  /** @brief When the exchange says the quote was true, if the broker gave a timestamp. */
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
  /** @brief When this process received the quote, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;

  /**
   * @brief The best bid and offer.
   *
   * @warning No shipped adapter fills this: it is empty on every quote from KIS, Toss or Kiwoom.
   *   For the spread or midpoint, read the order book with the session's `depth()`.
   */
  std::optional<TopOfBook> top_of_book_;
  /** @brief The session's cumulative traded volume, if the broker gave it. */
  std::optional<Decimal64> volume_;
};

/** @brief An exact amount of money in one currency. */
struct Money {
  /** @brief The amount. */
  Decimal64 amount_;
  /** @brief The currency; the library never adds amounts in different currencies. */
  Currency currency_;

  /**
   * @brief Compares amount (with `Decimal64::operator==`) and currency.
   * @param lhs The first amount.
   * @param rhs The second amount.
   * @return `true` if both are equal.
   */
  friend bool operator==(const Money& lhs, const Money& rhs) {
    return lhs.amount_ == rhs.amount_ && lhs.currency_ == rhs.currency_;
  }
};

/**
 * @brief What a sellable quantity covers, and how `RiskGuard` must treat resting sells against it.
 *
 * Both parts default to `UNESTABLISHED`, and `RiskGuard` refuses a checked sell against a figure
 * whose basis is not enforceable. An adapter that fills `sellable_quantity_` must state this too.
 */
struct SellableBasis {
  /** @brief Whether the figure covers the whole holding. */
  enum class Coverage {
    /** @brief Not established; `RiskGuard` refuses. */
    UNESTABLISHED,
    /** @brief Every share held of the instrument, whatever venue or lot. */
    WHOLE_HOLDING,
    /**
     * @brief Only part of the holding, such as one venue's shares; `RiskGuard` refuses, reporting
     *   `SELLABLE_BASIS_UNESTABLISHED`.
     */
    PARTIAL,
  };

  /** @brief How `RiskGuard` treats resting sell orders against the figure. */
  enum class RestingTreatment {
    /** @brief Not stated; `RiskGuard` refuses. */
    UNESTABLISHED,
    /**
     * @brief Subtract every resting sell from the figure.
     *
     * Safe whatever the broker does: if the broker already deducted them, sells are over-refused,
     * never doubled.
     */
    DEDUCT,
  };

  /** @brief What the figure covers; unestablished by default. */
  Coverage coverage_ = Coverage::UNESTABLISHED;
  /** @brief How resting sells are treated; unestablished by default. */
  RestingTreatment resting_ = RestingTreatment::UNESTABLISHED;
  /**
   * @brief Whether the basis was confirmed from a real broker response, rather than stated as a policy.
   *
   * Nothing in the library sets it to `true` today.
   */
  bool verified_ = false;

  /**
   * @brief Tests whether `RiskGuard` may check a sell against a figure with this basis.
   * @return `true` if the coverage is `WHOLE_HOLDING` and the treatment is stated.
   */
  constexpr bool is_enforceable() const noexcept {
    return coverage_ == Coverage::WHOLE_HOLDING && resting_ != RestingTreatment::UNESTABLISHED;
  }
};

/**
 * @brief Returns a lowercase name for a coverage, such as `"whole_holding"`, for logs.
 * @param coverage The coverage.
 * @return The name.
 */
inline constexpr std::string_view sellable_coverage_name(SellableBasis::Coverage coverage
) noexcept {
  switch (coverage) {
    case SellableBasis::Coverage::UNESTABLISHED:
      return "unestablished";
    case SellableBasis::Coverage::WHOLE_HOLDING:
      return "whole_holding";
    case SellableBasis::Coverage::PARTIAL:
      return "partial";
  }
  return "unestablished";
}

/**
 * @brief Returns a lowercase name for a treatment, such as `"deduct"`, for logs.
 * @param treatment The treatment.
 * @return The name.
 */
inline constexpr std::string_view sellable_resting_name(SellableBasis::RestingTreatment treatment
) noexcept {
  switch (treatment) {
    case SellableBasis::RestingTreatment::UNESTABLISHED:
      return "unestablished";
    case SellableBasis::RestingTreatment::DEDUCT:
      return "deduct";
  }
  return "unestablished";
}

/** @brief One position row in a holdings snapshot. */
struct HoldingPosition {
  /** @brief The instrument held (brokers report holdings without a trading venue). */
  InstrumentId instrument_;
  /** @brief The instrument's name as the broker spells it. */
  std::string name_;
  /**
   * @brief The quantity in this row.
   *
   * One instrument may span several rows; size a holding with `Portfolio::net_quantity_of`.
   */
  Decimal64 quantity_;
  /**
   * @brief How much the broker says may be sold now.
   *
   * Empty if the broker did not say, which is not zero. What it covers is the row's
   * `sellable_basis_`, or else the snapshot's `HoldingsSnapshot::sellable_basis_`.
   */
  std::optional<Decimal64> sellable_quantity_;
  /**
   * @brief What this row's `sellable_quantity_` covers, when the figure came from a per-instrument
   *   inquiry rather than the holdings read.
   *
   * Empty in the ordinary case, where `HoldingsSnapshot::sellable_basis_` applies.
   */
  std::optional<SellableBasis> sellable_basis_;
  /** @brief The broker's average purchase price. */
  Decimal64 average_purchase_price_;
  /** @brief The price the broker valued the position at. */
  Decimal64 last_price_;
  /** @brief The cost basis, as the broker computed it. */
  Money purchase_value_;
  /** @brief The market value, as the broker computed it. */
  Money market_value_;
  /** @brief The unrealized profit or loss, as the broker reported it. */
  Money unrealized_profit_loss_;
  /** @brief The broker's return figure, as a fraction (`0.05` is 5%). */
  Decimal64 unrealized_return_;
};

/** @brief The broker's holdings totals for one currency. */
struct HoldingsTotals {
  /** @brief The currency; there is one totals row per currency, never converted. */
  Currency currency_;
  /**
   * @brief The broker's total cost basis in `currency_`.
   *
   * From the broker's summary, not summed from the positions; the two can differ when the
   * positions were paged.
   */
  Decimal64 purchase_value_;
  /** @brief The broker's total market value in `currency_`, from its summary. */
  Decimal64 market_value_;
  /** @brief The broker's total unrealized profit or loss in `currency_`, from its summary. */
  Decimal64 unrealized_profit_loss_;
};


/** @brief An account's positions and per-currency totals at one point in time. */
struct HoldingsSnapshot {
  /** @brief Every position row; one instrument may appear in more than one. */
  std::vector<HoldingPosition> positions_;
  /** @brief One totals row per currency held. */
  std::vector<HoldingsTotals> totals_;
  /** @brief When this process received the snapshot (brokers do not timestamp holdings). */
  std::chrono::system_clock::time_point received_at_;
  /**
   * @brief What the rows' `sellable_quantity_` figures cover (unless a row carries its own).
   *
   * Defaults to unestablished, so an adapter that fills sellable quantities without stating this
   * has its sells refused.
   */
  SellableBasis sellable_basis_;
};

/** @brief Where the library gets a broker's sellable quantity. */
enum class SellableSource {
  /** @brief On the holdings snapshot, at no extra request. */
  HOLDINGS,
  /** @brief From a per-instrument inquiry, made only where the snapshot has no figure. */
  INQUIRY,
  /** @brief Not available for this broker. */
  NONE,
};

/**
 * @brief Returns a lowercase name for a source, such as `"inquiry"`, for logs.
 * @param source The source.
 * @return The name.
 */
inline constexpr std::string_view sellable_source_name(SellableSource source) noexcept {
  switch (source) {
    case SellableSource::HOLDINGS:
      return "holdings";
    case SellableSource::INQUIRY:
      return "inquiry";
    case SellableSource::NONE:
      return "none";
  }
  return "none";
}

/**
 * @brief What the broker says may be spent, in one currency.
 *
 * The broker's own figure (Toss `cashBuyingPower`, KIS `nrcvb_buy_amt`): it counts unsettled
 * proceeds on the broker's terms, excludes amounts reserved for resting orders, and includes no
 * credit. It is not the library's own `Portfolio::cash_`. In a backtest the ledger fills it with
 * modelled cash.
 */
struct BuyingPower {
  /** @brief The amount that may be spent. */
  Money cash_;
  /**
   * @brief When the figure was received, on this machine's clock (brokers do not timestamp it).
   *
   * In a backtest, the snapshot time.
   */
  std::chrono::system_clock::time_point received_at_{};
};

/**
 * @brief A calendar date with no time of day or time zone.
 *
 * Settlement dates, calendar days and inquiry ranges are dates in the broker's local calendar.
 */
struct Date {
  /** @brief The year. */
  int year_{0};
  /** @brief The month, `1`-`12`; zero means unset. */
  unsigned month_{0};
  /** @brief The day of the month, `1`-`31`; zero means unset. */
  unsigned day_{0};

  /**
   * @brief Compares two dates.
   * @param lhs The first date.
   * @param rhs The second date.
   * @return `true` if year, month and day are equal.
   */
  friend bool operator==(const Date& lhs, const Date& rhs) {
    return lhs.year_ == rhs.year_ && lhs.month_ == rhs.month_ && lhs.day_ == rhs.day_;
  }

  /**
   * @brief Orders dates chronologically.
   * @param lhs The first date.
   * @param rhs The second date.
   * @return `true` if `lhs` is earlier.
   */
  friend bool operator<(const Date& lhs, const Date& rhs) {
    if (lhs.year_ != rhs.year_) {
      return lhs.year_ < rhs.year_;
    }
    if (lhs.month_ != rhs.month_) {
      return lhs.month_ < rhs.month_;
    }
    return lhs.day_ < rhs.day_;
  }
};

/**
 * @brief Converts a day count since 1970-01-01 to a Gregorian date.
 *
 * Howard Hinnant's `civil_from_days`, exact for any day count whose year fits an `int` (far beyond
 * years 1 to 9999). The result is not checked against `is_valid_date`'s year range.
 *
 * @param days Days after 1970-01-01; negative counts go backwards.
 * @return The date.
 */
inline Date date_from_epoch_days(std::int64_t days) {
  // Shifts the era so that the year starts in March, which is what removes the leap-day special
  // case from everything below: 1970-01-01 is 719468 days after 0000-03-01.
  const std::int64_t shifted = days + 719468;
  const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const std::int64_t day_of_era = shifted - era * 146097;
  const std::int64_t year_of_era =
    (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const std::int64_t year = year_of_era + era * 400;
  const std::int64_t day_of_year =
    day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
  const std::int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
  const std::int64_t month = shifted_month + (shifted_month < 10 ? 3 : -9);
  return Date{
    static_cast<int>(year + (month <= 2 ? 1 : 0)),
    static_cast<unsigned>(month),
    static_cast<unsigned>(day),
  };
}

/**
 * @brief Tests whether a date exists in the Gregorian calendar, in years 1 to 9999.
 * @param date The date.
 * @return `true` if valid.
 */
inline bool is_valid_date(const Date& date) {
  if (date.year_ < 1 || date.year_ > 9999 || date.month_ < 1 || date.month_ > 12) {
    return false;
  }
  static constexpr unsigned days_per_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
  };
  unsigned days = days_per_month[date.month_ - 1];
  const bool leap = date.year_ % 4 == 0 && (date.year_ % 100 != 0 || date.year_ % 400 == 0);
  if (date.month_ == 2 && leap) {
    ++days;
  }
  return date.day_ >= 1 && date.day_ <= days;
}

/** @brief Which kind of venue a `VenueKey` names. */
enum class VenueKeyKind {
  /** @brief Any venue (a venue-neutral rate). */
  ANY,
  /** @brief A `Venue` the library normalizes. */
  NORMALIZED,
  /** @brief A broker venue code the library does not normalize, kept verbatim. */
  RAW,
};

/**
 * @brief Which venue a commission rate applies to: any venue, a normalized `Venue`, or a raw code.
 *
 * The three kinds never match each other: a `RAW` key never equals a `NORMALIZED` one even if the
 * text looks the same. A backtest refuses `Venue::COMBINED` as the venue a fill happened at, since
 * it names consolidated routing (KIS `SOR`), not where an order matched.
 */
class VenueKey {
public:
  /** @brief Constructs the key for any venue. */
  VenueKey() = default;

  /**
   * @brief Returns the key for any venue.
   * @return A key of kind `ANY`.
   */
  static VenueKey any() noexcept { return VenueKey(); }

  /**
   * @brief Makes a key for a normalized venue.
   * @param venue The venue; `UNSPECIFIED` is not a venue (use `any()`).
   * @return The key, or `std::nullopt` for `Venue::UNSPECIFIED`.
   */
  static std::optional<VenueKey> normalized(Venue venue) {
    if (venue == Venue::UNSPECIFIED) {
      return std::nullopt;
    }
    VenueKey key;
    key.kind_ = VenueKeyKind::NORMALIZED;
    key.venue_ = venue;
    return key;
  }

  /**
   * @brief Makes a key for a venue code the library does not normalize.
   * @param code The broker's code: 1 to 8 characters, each `A`-`Z`, `0`-`9` or `_`.
   * @return The key, or `std::nullopt` if `code` is empty or cannot be stored.
   */
  static std::optional<VenueKey> raw(std::string_view code) {
    auto stored = Code<8>::try_make(code);
    if (!stored || stored->empty()) {
      return std::nullopt;
    }
    VenueKey key;
    key.kind_ = VenueKeyKind::RAW;
    key.raw_ = *stored;
    return key;
  }

  /**
   * @brief Returns the key's kind.
   * @return `ANY`, `NORMALIZED` or `RAW`.
   */
  VenueKeyKind kind() const noexcept { return kind_; }

  /**
   * @brief Tests whether the key is for any venue.
   * @return `true` for kind `ANY`.
   */
  bool is_any() const noexcept { return kind_ == VenueKeyKind::ANY; }

  /**
   * @brief Returns the normalized venue.
   * @return The venue for kind `NORMALIZED`; `UNSPECIFIED` otherwise.
   */
  Venue venue() const noexcept { return venue_; }

  /**
   * @brief Returns the raw venue code.
   * @return The code for kind `RAW`; empty otherwise.
   */
  const Code<8>& raw_code() const noexcept { return raw_; }

  /**
   * @brief Tests whether two keys name the same venue, exactly in kind and value.
   *
   * No fallback: an `ANY` key matches only `ANY`. Falling back to a neutral rate is done by
   * `CommissionSchedule::rate_for`.
   *
   * @param asserted The key to compare with.
   * @return `true` if the kinds and values are equal.
   */
  bool matches(const VenueKey& asserted) const noexcept {
    if (kind_ != asserted.kind_) {
      return false;
    }
    switch (kind_) {
      case VenueKeyKind::ANY:
        return true;
      case VenueKeyKind::NORMALIZED:
        return venue_ == asserted.venue_;
      case VenueKeyKind::RAW:
        return raw_ == asserted.raw_;
    }
    return false;
  }

  /**
   * @brief Compares kind and value; same as `matches`.
   * @param lhs The first key.
   * @param rhs The second key.
   * @return `true` if they match.
   */
  friend bool operator==(const VenueKey& lhs, const VenueKey& rhs) noexcept { return lhs.matches(rhs); }

  /**
   * @brief Compares two keys for inequality.
   * @param lhs The first key.
   * @param rhs The second key.
   * @return `true` if they do not match.
   */
  friend bool operator!=(const VenueKey& lhs, const VenueKey& rhs) noexcept { return !(lhs == rhs); }

private:
  VenueKeyKind kind_{VenueKeyKind::ANY};
  Venue venue_{Venue::UNSPECIFIED};
  Code<8> raw_;
};

/**
 * @brief Returns a lowercase name for a key kind, such as `"raw"`, for reports.
 * @param kind The kind.
 * @return The name.
 */
inline constexpr std::string_view venue_key_kind_name(VenueKeyKind kind) noexcept {
  switch (kind) {
    case VenueKeyKind::ANY:
      return "any";
    case VenueKeyKind::NORMALIZED:
      return "normalized";
    case VenueKeyKind::RAW:
      return "raw";
  }
  return "unknown";
}

/** @brief Where a commission schedule's numbers came from. */
enum class CommissionOrigin {
  /** @brief The broker published them, through an inquiry the library made. */
  BROKER_PUBLISHED,
  /** @brief The caller stated them, from a fee page or their own agreement. */
  CALLER_STATED,
};

/**
 * @brief Returns a lowercase name for an origin, such as `"caller_stated"`, for reports.
 * @param origin The origin.
 * @return The name.
 */
inline constexpr std::string_view commission_origin_name(CommissionOrigin origin) noexcept {
  switch (origin) {
    case CommissionOrigin::BROKER_PUBLISHED:
      return "broker_published";
    case CommissionOrigin::CALLER_STATED:
      return "caller_stated";
  }
  return "unknown";
}

/** @brief Whether a commission rate applies on a date. */
enum class RateApplicability {
  /** @brief A readable date bound puts the date outside the rate's window. */
  NO,
  /** @brief Every bound was readable, and they cover the date. */
  YES,
  /**
   * @brief Unknown: no readable bound excludes the date, but a bound could not be read.
   *
   * Do not fall back to another rate on this answer; the unreadable row may be the one in force.
   */
  UNREADABLE,
};

/**
 * @brief One commission rate, the market and venue it applies to, and the dates it applies on.
 *
 * Rates are fractions of traded value (`0.00015`, not `0.015`). An absent date bound is open-ended
 * in that direction; a bound that was present but unreadable sets `from_unreadable_` or
 * `until_unreadable_`, and then `applies_on` answers `UNREADABLE` rather than treating the row as
 * open-ended. No per-order minimum or transaction tax is modelled here; a backtest states a sale
 * tax with `backtest::NextBarFillModel::with_sell_tax`.
 */
struct CommissionRate {
  /** @brief The market the rate applies to; meaningless when `other_` is set. */
  Country country_{Country::KR};
  /**
   * @brief The broker's market code, when the library does not normalize that market.
   *
   * Empty for a normalized market. A broker row whose market code cannot be stored here is skipped
   * and counted in `CommissionSchedule::dropped_`.
   */
  Code<16> other_;
  /**
   * @brief The venue the rate applies to.
   *
   * `VenueKey::any()` on every rate a broker publishes; no broker publishes per-venue rates, so a
   * caller states KRX and NXT rates from the fee page.
   */
  VenueKey venue_;
  /** @brief The broker's commission, as a fraction of traded value (`0.00015`, not `0.015`). */
  Decimal64 rate_;
  /**
   * @brief The institution fee (유관기관제비용), as a fraction of traded value, if stated separately.
   *
   * Empty means the source gave one combined rate and said nothing about its parts (every Toss
   * row); a report then treats `rate_` as inclusive. A present zero is a stated zero; a positive
   * value is charged in addition to `rate_`.
   */
  std::optional<Decimal64> institution_rate_;
  /**
   * @brief The first date the rate applies on; empty is open-ended unless `from_unreadable_` is set.
   */
  std::optional<Date> from_;
  /** @brief The last date the rate applies on; empty is open-ended unless `until_unreadable_` is set. */
  std::optional<Date> until_;
  /**
   * @brief Whether the source stated a start date that could not be read (so `from_` is empty).
   *
   * The row is kept, but `applies_on` answers `UNREADABLE` for any date the readable bounds do not
   * exclude.
   */
  bool from_unreadable_{false};
  /**
   * @brief Whether the source stated an end date that could not be read (so `until_` is empty).
   *
   * The row is kept, but `applies_on` answers `UNREADABLE` for any date the readable bounds do not
   * exclude.
   */
  bool until_unreadable_{false};

  /**
   * @brief Tests whether the rate's market is a normalized `Country`.
   * @return `true` if `other_` is empty, so `country_` names the market.
   */
  bool normalized() const noexcept { return other_.empty(); }

  /**
   * @brief Tests whether the rate applies on a date.
   *
   * @param date The date.
   * @return `NO` if a readable bound excludes the date (even when the other bound is unreadable),
   *   `UNREADABLE` if not excluded but a bound could not be read, and `YES` otherwise.
   */
  RateApplicability applies_on(const Date& date) const {
    if (excluded_on(date)) {
      return RateApplicability::NO;
    }
    if (from_unreadable_ || until_unreadable_) {
      return RateApplicability::UNREADABLE;
    }
    return RateApplicability::YES;
  }

  /**
   * @brief Tests whether a readable bound puts a date outside the rate's window.
   * @param date The date.
   * @return `true` if `date` is before `from_` or after `until_`.
   */
  bool excluded_on(const Date& date) const {
    return (from_ && date < *from_) || (until_ && *until_ < date);
  }
};

/**
 * @brief The commission rates a broker published for an account, or a caller stated.
 *
 * An empty schedule from a broker that publishes rates is a real answer: the account has none.
 */
struct CommissionSchedule {
  /** @brief The rate rows. */
  std::vector<CommissionRate> rates_;
  /** @brief When the schedule was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_{};
  /** @brief Whether a broker published the rates or a caller stated them. */
  CommissionOrigin origin_{CommissionOrigin::BROKER_PUBLISHED};
  /**
   * @brief Which tariff the rates are, in the source's own words.
   *
   * **Required for a caller-stated schedule**: a backtest's cost model refuses one without it,
   * because fee pages differ by account channel (온라인 영업점 개설 계좌, 온라인 뱅키스 계좌,
   * 영업점매매), which no API states. A broker-published schedule carries the broker's own name.
   */
  std::string tariff_;

  /**
   * @brief How many broker rows were skipped because their market code could not be stored.
   *
   * Nonzero means the schedule is incomplete.
   */
  std::size_t dropped_{0};

  /**
   * @brief How many rows have a date bound that could not be read.
   *
   * Nonzero means some rows' windows are unknown, so a lookup they could affect returns no rate
   * (`RateSelection::bounds_unreadable_`). Unlike `dropped_`, these rows are kept.
   */
  std::size_t unreadable_bounds_{0};

  /** @brief The result of `rate_for`: a rate, or why there is none. */
  struct RateSelection {
    /** @brief The rate in force, if one definitely is. */
    std::optional<CommissionRate> rate_;
    /**
     * @brief Whether an unreadable date window is why no rate was returned.
     *
     * Never `true` together with a rate. Check it before falling back to another rate: `true` means
     * the schedule may cover the date, and nobody can tell.
     */
    bool bounds_unreadable_{false};
  };

  /**
   * @brief Finds the rate in force for a market and venue on a date.
   *
   * First the rows keyed to `asserted`'s venue are searched, then the venue-neutral ones; within
   * each pass the first applicable row wins. If a row reached before any match has an unreadable
   * window, the search stops and reports `bounds_unreadable_`. Asking with `VenueKey::any()` reads
   * neutral rows only, so a schedule with only KRX and NXT rows answers nothing.
   *
   * @param country The market.
   * @param asserted The venue the fill happened at, or `VenueKey::any()`.
   * @param date The trading date.
   * @return The rate, or none with `bounds_unreadable_` saying whether damage is why.
   */
  RateSelection rate_for(
    Country country,
    const VenueKey& asserted,
    const Date& date
  ) const {
    // A row for a market this library does not normalize can never be the answer to a question asked
    // in terms of `Country`, and it is kept so a caller can see that the broker sent it.
    const auto in_market = [&](const CommissionRate& rate) {
      return rate.normalized() && rate.country_ == country;
    };
    // **Each pass stops at the first row that decides it, and an unreadable row decides it too.**
    // Within a pass the first match has always won, so a damaged row *after* a definite match could
    // never have been the answer and is not a reason to refuse -- scanning the whole pass first would
    // throw away an answer this library knows. A damaged row reached *before* any match is the
    // opposite case: it may have been the match, so nothing later in the pass can be trusted to be
    // the winner.
    const auto pass = [&](bool venue_specific) -> std::optional<RateSelection> {
      for (const auto& rate : rates_) {
        if (!in_market(rate)) {
          continue;
        }
        if (!(venue_specific ? rate.venue_.matches(asserted) : rate.venue_.is_any())) {
          continue;
        }
        switch (rate.applies_on(date)) {
          case RateApplicability::NO:
            continue;
          case RateApplicability::YES: {
            RateSelection found;
            found.rate_ = rate;
            return found;
          }
          case RateApplicability::UNREADABLE: {
            RateSelection refused;
            refused.bounds_unreadable_ = true;
            return refused;
          }
        }
      }
      return std::nullopt;
    };
    // An undecided pass one falls through; a *refused* pass one does not. Pass two's precondition is
    // "no row for the asserted venue applies", and that is precisely what stopped being known.
    if (const auto venue_row = pass(true)) {
      return *venue_row;
    }
    if (const auto neutral_row = pass(false)) {
      return *neutral_row;
    }
    return RateSelection{};
  }

  /**
   * @brief Tests whether any rate for a market has a date bound, readable or not.
   * @param country The market.
   * @return `true` if a rate for `country` has `from_`, `until_`, or an unreadable bound.
   */
  bool any_dated_rate_for(Country country) const {
    for (const auto& rate : rates_) {
      // A bound that could not be read is still a bound the source stated. Asking only whether the
      // optionals are filled would read a row dated *solely* by damage as undated, and hand the
      // caller the very "every candidate is the same whichever calendar named the day" assurance
      // that the damage makes impossible.
      const bool dated = rate.from_ || rate.until_ || rate.from_unreadable_ || rate.until_unreadable_;
      if (rate.normalized() && rate.country_ == country && dated) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Finds the venue-neutral rate in force for a market on a date.
   *
   * Same as `rate_for(country, VenueKey::any(), date)`.
   *
   * @param country The market.
   * @param date The trading date.
   * @return The rate, or none with `bounds_unreadable_` saying whether damage is why.
   */
  RateSelection rate_for(Country country, const Date& date) const {
    return rate_for(country, VenueKey::any(), date);
  }
};

/** @brief The side of an order. */
enum class OrderSide {
  BUY,
  SELL,
};

/**
 * @brief How an order's price is decided (its price basis).
 *
 * The execution condition is `TimeInForce` and the session is `OrderSession`; a broker's single
 * order code (such as KIS `ORD_DVSN`) maps to all three. A request is accepted only for
 * combinations the broker publishes; an adapter refuses others before any I/O. On a read, an
 * unrecognized combination is `UNKNOWN`. `carries_caller_price` says which bases take a price.
 */
enum class OrderType {
  UNKNOWN,
  /** @brief 지정가: fills at the stated price or better. */
  LIMIT,
  /** @brief 시장가: fills at whatever the book offers. */
  MARKET,
  /** @brief 조건부지정가: a limit that becomes a market order at the close. */
  CONDITIONAL_LIMIT,
  /** @brief 최유리지정가: priced at the best quote on the opposite side. */
  BEST_LIMIT,
  /** @brief 최우선지정가: priced at the best quote on the order's own side. */
  PRIMARY_LIMIT,
  /** @brief 중간가: priced at the midpoint of the spread. */
  MID_PRICE,
  /** @brief 스톱지정가: a limit released when a trigger price is reached. */
  STOP_LIMIT,
  /** @brief 시간외종가: priced at a session's closing price. */
  CLOSING_PRICE,
};

/**
 * @brief Tests whether an order type takes a price from the caller.
 *
 * A type that does not is sent without a price; a type that does must have one.
 *
 * @param type The order type.
 * @return `true` for `LIMIT`, `CONDITIONAL_LIMIT` and `STOP_LIMIT`.
 */
inline constexpr bool carries_caller_price(OrderType type) noexcept {
  return type == OrderType::LIMIT || type == OrderType::CONDITIONAL_LIMIT ||
    type == OrderType::STOP_LIMIT;
}

/**
 * @brief Tests whether an order type takes a trigger price.
 *
 * A trigger stated on any other type is refused, not dropped. The trigger's direction, tick grid
 * and relation to the limit are the broker's rules and are not checked by the library.
 *
 * @param type The order type.
 * @return `true` for `STOP_LIMIT` only.
 */
inline constexpr bool carries_trigger_price(OrderType type) noexcept {
  return type == OrderType::STOP_LIMIT;
}

/**
 * @brief How long an order stays valid and what happens to an unfilled remainder.
 *
 * The session is a separate field, `OrderSession`: an at-the-close order is `DAY` in the
 * `CLOSING_AUCTION` session. `UNKNOWN` appears only on reads, never on requests.
 */
enum class TimeInForce {
  UNKNOWN,
  /** @brief Rests until the session ends; an unfilled remainder is cancelled then. */
  DAY,
  /** @brief Fills what it can immediately and cancels the remainder. */
  IMMEDIATE_OR_CANCEL,
  /** @brief Fills entirely and immediately, or not at all. */
  FILL_OR_KILL,
};

/**
 * @brief The trading session an order is submitted into.
 *
 * A session the broker does not offer is refused on a request before any I/O.
 */
enum class OrderSession {
  UNKNOWN,
  /** @brief The continuous regular session. */
  REGULAR,
  /** @brief The opening single-price auction (시가단일가; Toss `OPG`). */
  OPENING_AUCTION,
  /** @brief The closing auction (Toss `CLS`, a US limit-on-close). */
  CLOSING_AUCTION,
  /** @brief Before the open, at the previous session's close (장전 시간외). */
  PRE_MARKET,
  /** @brief After the close, at that session's close (장후 시간외). */
  AFTER_HOURS,
  /** @brief The after-hours single-price auction (시간외 단일가). */
  AFTER_HOURS_AUCTION,
  /** @brief KRX's after-market session (애프터마켓). */
  AFTER_MARKET,
  /** @brief NXT's GTP session. */
  GTP,
};

/**
 * @brief Where an order stands in its lifecycle.
 *
 * `CANCEL_REJECTED`, `REPLACE_REJECTED` and `REPLACED` describe a cancel or modify instruction,
 * which brokers list as rows of their own. `UNKNOWN` is a status the library does not recognize;
 * one such row does not fail a listing.
 */
enum class OrderStatus {
  UNKNOWN,
  PENDING,
  PENDING_CANCEL,
  PENDING_REPLACE,
  PARTIAL_FILLED,
  FILLED,
  CANCELED,
  REJECTED,
  CANCEL_REJECTED,
  REPLACE_REJECTED,
  REPLACED,
};

/**
 * @brief An order to place, in broker-neutral form.
 *
 * `price_` must be set exactly when `carries_caller_price(type_)`, and `trigger_price_` exactly
 * for `STOP_LIMIT`. An `UNKNOWN` type, time in force or session, or a combination the broker does
 * not offer at the requested venue, is refused before any I/O.
 */
struct OrderRequest {
  /** @brief The instrument; its `venue_` is identity, and routing is this struct's `venue_`. */
  InstrumentId instrument_;
  /** @brief Buy or sell. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The price basis: limit, market, best-limit, and so on. */
  OrderType type_{OrderType::LIMIT};
  /** @brief How long the order stays valid. */
  TimeInForce time_in_force_{TimeInForce::DAY};
  /** @brief The trading session. */
  OrderSession session_{OrderSession::REGULAR};
  /**
   * @brief Where to route the order, if the broker offers a choice.
   *
   * Set routing here, not in `instrument_.venue_` (an adapter refuses an identity venue other than
   * the one it reports). `UNSPECIFIED` takes the broker's default:
   * - KIS routes `UNSPECIFIED` to KRX; `COMBINED` is sent as `SOR`, which a KIS **demo** account
   *   refuses;
   * - Kiwoom requires a venue and refuses `UNSPECIFIED` before any request;
   * - Toss offers no choice and refuses any venue but `UNSPECIFIED`.
   *
   * A live runner sets it from `live::SessionExecutor::default_route`.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** @brief The quantity, in the instrument's units (shares). */
  Decimal64 quantity_;
  /** @brief The price; required when `carries_caller_price(type_)`, refused otherwise. */
  std::optional<Decimal64> price_;
  /**
   * @brief The trigger price; required for `STOP_LIMIT`, refused otherwise.
   *
   * A broker whose `BrokerCaps::has_trigger_price` is false never receives one: `BrokerSession`
   * refuses the request first.
   */
  std::optional<Decimal64> trigger_price_;
  /**
   * @brief An idempotency key, if the broker offers one; never generated by the library.
   *
   * Toss honours one for ten minutes. KIS has none for individual customers and refuses a request
   * that carries one, before any I/O.
   */
  std::optional<std::string> client_order_id_;
  /** @brief Confirms a high-value order, where the broker asks for that confirmation. */
  bool confirm_high_value_{false};
};

/**
 * @brief What has filled on one order, and what it cost.
 *
 * @warning Test whether an order filled with `filled_quantity_`, never with whether an amount is
 *   present: KIS reports a zero `filled_amount_` on unfilled orders, and Toss leaves amounts empty
 *   when the broker sent `null`.
 *
 * At Toss, an amount sent as `null` is empty, but a missing field fails the listing as a protocol
 * error.
 */
struct OrderExecution {
  /** @brief The quantity filled so far; zero if nothing has filled. Always present. */
  Decimal64 filled_quantity_;
  /**
   * @brief The average fill price, only where the broker supplies it.
   *
   * KIS's open-order listing never sets it, even on a partly filled order.
   */
  std::optional<Decimal64> average_filled_price_;
  /**
   * @brief The value of the fills, as the broker computed it.
   *
   * Present (zero) on every KIS row, filled or not; empty at Toss when not reported.
   */
  std::optional<Money> filled_amount_;
  /** @brief Commission charged so far, if the broker reports it per order. */
  std::optional<Money> commission_;
  /** @brief Transaction tax charged so far, if the broker reports it per order. */
  std::optional<Money> tax_;
  /** @brief When the most recent fill happened, if the broker timestamps it. */
  std::optional<std::chrono::system_clock::time_point> filled_at_;
  /** @brief The settlement date the broker stated. */
  std::optional<Date> settlement_date_;
};

/** @brief The state of one order, as a broker listing reported it. */
struct OrderSnapshot {
  /** @brief The broker's order number. */
  std::string order_id_;
  /** @brief The instrument (identity venue; the routing venue is `venue_`). */
  InstrumentId instrument_;
  /** @brief The instrument's name as the broker spells it. */
  std::string name_;
  /** @brief Buy or sell. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The price basis; `UNKNOWN` if the broker's code was not recognized. */
  OrderType type_{OrderType::UNKNOWN};
  /** @brief The time in force; `UNKNOWN` if the broker's code was not recognized. */
  TimeInForce time_in_force_{TimeInForce::UNKNOWN};
  /** @brief The session; `UNKNOWN` if the broker's code was not recognized. */
  OrderSession session_{OrderSession::UNKNOWN};
  /**
   * @brief Where the broker says the order was routed; `UNSPECIFIED` if it does not say.
   *
   * An amendment must address this venue. It is kept apart from `instrument_` so the order still
   * matches its holding, which brokers report without a venue.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /**
   * @brief The broker's own order-type code, verbatim (KIS two digits, Toss three letters).
   *
   * Kept even when the normalized fields read `UNKNOWN`. Empty if the broker sent none, or a code
   * longer than eight characters or with characters other than `A`-`Z`, `0`-`9`, `_`.
   */
  Code<8> shape_code_;
  /** @brief The status; `UNKNOWN` where the broker publishes none. */
  OrderStatus status_{OrderStatus::UNKNOWN};
  /** @brief The limit price, if the order has one. */
  std::optional<Decimal64> price_;
  /**
   * @brief The trigger price, whenever the broker reports a positive one.
   *
   * Kept even on a row whose type is not `STOP_LIMIT` (a broker contradiction); replacing such an
   * order is then refused, while cancelling it works. Empty on a stop row means the broker did not
   * say, and replacing it is refused.
   */
  std::optional<Decimal64> trigger_price_;
  /** @brief The quantity ordered; the remainder is this minus `execution_.filled_quantity_`. */
  Decimal64 quantity_;
  /**
   * @brief The money an amount-based order committed (Toss US market orders), if any.
   *
   * The library does not place such orders, but reports ones placed elsewhere.
   */
  std::optional<Decimal64> order_amount_;
  /** @brief The currency of the row's prices and amounts. */
  Currency currency_;
  /** @brief When the broker recorded the order, if it gives a time. KIS working orders have none. */
  std::optional<std::chrono::system_clock::time_point> ordered_at_;
  /** @brief When the order was cancelled, if the broker reports it. */
  std::optional<std::chrono::system_clock::time_point> canceled_at_;
  /** @brief What has filled and what it cost; see `OrderExecution` before testing a fill. */
  OrderExecution execution_;
};

/** @brief One page of an order listing. */
struct OrderPage {
  /** @brief The rows, in the broker's order. */
  std::vector<OrderSnapshot> orders_;
  /** @brief The cursor for the next page, to pass back unchanged; empty if there is none. */
  std::optional<std::string> next_cursor_;
  /** @brief When the page was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * @brief The broker's acknowledgement that it accepted an instruction.
 *
 * Accepted is not executed: read the order back to learn what happened.
 */
struct OrderAck {
  /** @brief The order number the broker assigned. */
  std::string order_id_;
  /** @brief The idempotency key, if the broker echoed one. */
  std::optional<std::string> client_order_id_;
  /** @brief When the acknowledgement was received. */
  std::chrono::system_clock::time_point accepted_at_;
};

/**
 * @brief A broker's reference to one order, used to amend or cancel it.
 *
 * The broker is a type parameter, so a reference from one broker cannot be passed to another's
 * session (it does not compile); a broker's asynchronous and blocking clients share one tag. Treat
 * the parts as opaque: store, compare and hash them, but do not parse them.
 *
 * @tparam Broker The broker's tag type, with `static constexpr std::string_view name`.
 */
template <typename Broker>
struct OrderRef {
  /** @brief The order number or id. */
  std::string primary_;
  /** @brief A second part some brokers need, such as KIS's branch code; empty otherwise. */
  std::string secondary_;

  /**
   * @brief Returns the issuing broker's name.
   * @return `Broker::name`.
   */
  static constexpr std::string_view broker() noexcept { return Broker::name; }

  /**
   * @brief Formats the reference for logs, as `broker:primary[:secondary]`.
   * @return The text.
   */
  std::string str() const {
    std::string text(broker());
    text += ':';
    text += primary_;
    if (!secondary_.empty()) {
      text += ':';
      text += secondary_;
    }
    return text;
  }

  /**
   * @brief Compares two references.
   * @param lhs The first reference.
   * @param rhs The second reference.
   * @return `true` if both parts are equal.
   */
  friend bool operator==(const OrderRef& lhs, const OrderRef& rhs) {
    return lhs.primary_ == rhs.primary_ && lhs.secondary_ == rhs.secondary_;
  }

  /**
   * @brief Compares two references for inequality.
   * @param lhs The first reference.
   * @param rhs The second reference.
   * @return `true` if either part differs.
   */
  friend bool operator!=(const OrderRef& lhs, const OrderRef& rhs) { return !(lhs == rhs); }
};

/**
 * @brief An accepted order: the acknowledgement and the reference to address it with.
 *
 * Accepted is not executed.
 *
 * @tparam Broker The broker tag.
 */
template <typename Broker>
struct PlacedOrder {
  /** @brief The acknowledgement. */
  OrderAck ack_;
  /** @brief The reference to amend or cancel the order with. */
  OrderRef<Broker> ref_;
};

/**
 * @brief An order's type, price, trigger, time in force and session, as an amendment states them.
 *
 * In an amendment, an unset time in force or session keeps the order's own. An unset trigger keeps
 * the resting trigger only when both the new and the resting type are `STOP_LIMIT`; turning a stop
 * into a plain `LIMIT` drops its trigger. To change only the price, use `reprice`.
 */
struct OrderShape {
  /** @brief Constructs a limit shape with nothing else stated. */
  OrderShape() = default;
  /**
   * @brief Constructs a shape, stating only what the caller means to set.
   * @param type The price basis.
   * @param price The price, for a basis that takes one.
   * @param time_in_force The time in force, or unset to keep the resting order's.
   * @param session The session, or unset to keep the resting order's.
   * @param trigger_price The trigger; unset keeps a resting stop's trigger only if `type` is also
   *   `STOP_LIMIT`.
   */
  OrderShape(
    OrderType type,
    std::optional<Decimal64> price,
    std::optional<TimeInForce> time_in_force = std::nullopt,
    std::optional<OrderSession> session = std::nullopt,
    std::optional<Decimal64> trigger_price = std::nullopt
  )
  : type_(type),
    price_(std::move(price)),
    trigger_price_(std::move(trigger_price)),
    time_in_force_(time_in_force),
    session_(session) {}

  /** @brief The price basis. */
  OrderType type_{OrderType::LIMIT};
  /** @brief The price, for a basis that takes one. */
  std::optional<Decimal64> price_;
  /**
   * @brief The trigger price, for `STOP_LIMIT`.
   *
   * On an amendment, unset keeps the resting stop's trigger, but only when both the new and the
   * resting shape are `STOP_LIMIT` (see `amended_trigger_price`); changing into a stop needs a
   * trigger stated. A trigger set on a type that takes none is refused. On an `OrderRequest`, a
   * stop-limit without one is refused.
   */
  std::optional<Decimal64> trigger_price_;
  /**
   * @brief The time in force; unset means "keep the resting order's".
   *
   * A set value is a deliberate change: made where the broker can make it, refused where it cannot
   * (Toss cannot change it on a modify).
   */
  std::optional<TimeInForce> time_in_force_;
  /**
   * @brief The session; unset means "keep the resting order's".
   *
   * A set value is a deliberate change: made where the broker can make it, refused where it cannot
   * (Toss cannot move an order between sessions).
   */
  std::optional<OrderSession> session_;
};

/**
 * @brief A modification or cancellation of a working order.
 *
 * Build one from a `WorkingOrder` with `cancel_all`, `cancel_quantity`, `replace_with` or `reprice`,
 * which fill in what the bound broker needs. `all_remaining_` is never inferred. A broker that
 * needs a quantity (KIS, even to cancel everything) refuses an instruction without one.
 */
struct AmendInstruction {
  /** @brief The shape the order should have afterwards. */
  OrderShape shape_;
  /**
   * @brief The venue the order rests at, for a broker whose amendment needs it (KIS).
   *
   * The helper functions copy it from the working order; `UNSPECIFIED` takes the broker's default.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** @brief The quantity affected; never substituted if a broker needs one and it is missing. */
  std::optional<Decimal64> quantity_;
  /** @brief Whether the whole remaining quantity is affected, rather than `quantity_`. */
  bool all_remaining_{false};
  /** @brief Confirms a high-value amendment, where the broker asks for that confirmation. */
  bool confirm_high_value_{false};
  /**
   * @brief The shape the order has now, or unset if unknown.
   *
   * An adapter compares it with `shape_` to tell a change from a keep. One that cannot verify a
   * change it cannot send refuses before any I/O. The helper functions fill it in.
   */
  std::optional<OrderShape> resting_;
};

/**
 * @brief Returns the trigger price an amendment leaves the order with.
 *
 * @param instruction The amendment.
 * @return The trigger `shape_` states; otherwise the resting order's trigger if both shapes are
 *   `STOP_LIMIT`; otherwise `std::nullopt`.
 */
inline std::optional<Decimal64> amended_trigger_price(const AmendInstruction& instruction) {
  if (instruction.shape_.trigger_price_) {
    return instruction.shape_.trigger_price_;
  }
  if (
    carries_trigger_price(instruction.shape_.type_) && instruction.resting_ &&
    carries_trigger_price(instruction.resting_->type_)
  ) {
    return instruction.resting_->trigger_price_;
  }
  return std::nullopt;
}

/**
 * @brief An order still open to modification or cancellation.
 *
 * @tparam Broker The broker tag.
 */
template <typename Broker>
struct WorkingOrder {
  /** @brief The order's state. */
  OrderSnapshot order_;
  /** @brief The reference to amend or cancel the order with. */
  OrderRef<Broker> ref_;
  /** @brief How much the broker says may still be amended. Empty means it stated none, not zero. */
  std::optional<Decimal64> amendable_quantity_;
  /**
   * @brief Whether this row is a runner's record of an order it just sent, rather than a row the
   *   broker listed.
   *
   * Such a row counts toward limits and can be cancelled like any other; only code that infers the
   * broker's behaviour ignores it. The next listing replaces it.
   */
  bool reserved_{false};
};

/**
 * @brief The orders open to modification or cancellation at one point in time.
 * @tparam Broker The broker tag.
 */
template <typename Broker>
struct WorkingOrders {
  /** @brief The open orders. */
  std::vector<WorkingOrder<Broker>> orders_;
  /** @brief When the listing was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * @brief The filter for an order-history inquiry.
 *
 * A filter the broker cannot apply is refused with `ErrorCategory::INVALID_ARGUMENT` before any
 * I/O, never silently dropped.
 */
struct OrderHistoryQuery {
  /** @brief The first date to include; required where `BrokerCaps::history_requires_range`. */
  std::optional<Date> from_;
  /** @brief The last date to include. */
  std::optional<Date> to_;
  /** @brief Only this symbol, where the broker supports the filter. */
  std::optional<std::string> symbol_;
  /** @brief A previous page's `next_cursor_`, passed back unchanged. */
  std::optional<std::string> cursor_;
  /** @brief The most rows to return, where the broker supports a limit. */
  std::optional<int> limit_;
};

/**
 * @brief Returns a working order's current shape.
 * @tparam Broker The broker tag.
 * @param order The working order.
 * @return Its type, price, trigger, time in force and session.
 */
template <typename Broker>
inline OrderShape resting_shape(const WorkingOrder<Broker>& order) {
  OrderShape shape;
  shape.type_ = order.order_.type_;
  shape.price_ = order.order_.price_;
  shape.trigger_price_ = order.order_.trigger_price_;
  shape.time_in_force_ = order.order_.time_in_force_;
  shape.session_ = order.order_.session_;
  return shape;
}

/**
 * @brief Makes an instruction cancelling all that remains of an order.
 * @tparam Broker The broker tag.
 * @param order The working order.
 * @return The instruction, carrying the order's shape, venue and amendable quantity.
 */
template <typename Broker>
inline AmendInstruction cancel_all(const WorkingOrder<Broker>& order) {
  AmendInstruction instruction;
  // A cancellation copies the whole shape, so a broker that re-reads it sees the same order.
  instruction.shape_.type_ = order.order_.type_;
  instruction.shape_.time_in_force_ = order.order_.time_in_force_;
  instruction.shape_.session_ = order.order_.session_;
  instruction.shape_.price_ = order.order_.price_;
  instruction.shape_.trigger_price_ = order.order_.trigger_price_;
  instruction.resting_ = instruction.shape_;
  instruction.venue_ = order.order_.venue_;
  instruction.quantity_ = order.amendable_quantity_;
  instruction.all_remaining_ = true;
  return instruction;
}

/**
 * @brief Makes an instruction cancelling part of an order.
 * @tparam Broker The broker tag.
 * @param order The working order.
 * @param quantity The quantity to cancel; check `BrokerCaps::has_partial_cancel` first.
 * @return The instruction.
 */
template <typename Broker>
inline AmendInstruction cancel_quantity(const WorkingOrder<Broker>& order, Decimal64 quantity) {
  AmendInstruction instruction;
  instruction.shape_.type_ = order.order_.type_;
  instruction.shape_.time_in_force_ = order.order_.time_in_force_;
  instruction.shape_.session_ = order.order_.session_;
  instruction.shape_.price_ = order.order_.price_;
  instruction.shape_.trigger_price_ = order.order_.trigger_price_;
  instruction.resting_ = instruction.shape_;
  instruction.venue_ = order.order_.venue_;
  instruction.quantity_ = quantity;
  instruction.all_remaining_ = false;
  return instruction;
}

/**
 * @brief Makes an instruction changing a working order to a new shape.
 *
 * An unset time in force or session stays as the order has it; an unset trigger stays only if both
 * `shape` and the order are `STOP_LIMIT` (see `amended_trigger_price`). A set value is a deliberate
 * change, made where the broker can and refused where it cannot. To change only the price, use
 * `reprice`.
 *
 * @tparam Broker The broker tag.
 * @param order The working order.
 * @param shape The new shape, such as `OrderShape{OrderType::LIMIT, price}`.
 * @param quantity The quantity to amend, or unset for all that remains.
 * @return The instruction.
 */
template <typename Broker>
inline AmendInstruction replace_with(
  const WorkingOrder<Broker>& order,
  OrderShape shape,
  std::optional<Decimal64> quantity = std::nullopt
) {
  AmendInstruction instruction;
  instruction.shape_ = std::move(shape);
  instruction.resting_ = resting_shape(order);
  // The caller's shape is kept as written; `resting_` lets each adapter tell a change from a keep.
  instruction.venue_ = order.order_.venue_;
  instruction.all_remaining_ = !quantity.has_value();
  instruction.quantity_ = quantity ? quantity : order.amendable_quantity_;
  return instruction;
}

/**
 * @brief Makes an instruction changing only a working order's price.
 * @tparam Broker The broker tag.
 * @param order The working order.
 * @param price The new price.
 * @param quantity The quantity to amend, or unset for all that remains.
 * @return The instruction.
 */
template <typename Broker>
inline AmendInstruction reprice(
  const WorkingOrder<Broker>& order,
  Decimal64 price,
  std::optional<Decimal64> quantity = std::nullopt
) {
  auto shape = resting_shape(order);
  shape.price_ = std::move(price);
  return replace_with(order, std::move(shape), std::move(quantity));
}

/**
 * @brief Formats an instant as an ISO 8601 timestamp in UTC, such as `2026-10-06T00:30:00Z`.
 * @param value The instant; fractions of a second are dropped.
 * @return The text.
 */
inline std::string format_iso8601_utc(std::chrono::system_clock::time_point value) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  char buffer[32];
  const auto written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buffer, written);
}

/**
 * @brief Parses an ISO 8601 timestamp with a `Z` or a `+hh:mm`/`-hh:mm` offset.
 *
 * Fractional seconds are truncated to the millisecond.
 *
 * @param value The text, such as `2026-10-06T09:30:00.250+09:00`.
 * @return The instant, or `std::nullopt` if the date-time part does not parse, the date or time is
 *   invalid, or there is no zone designator.
 * @warning Not a strict validator: it does not check the offset's range, trailing text after the
 *   zone, or characters after the third fractional digit (`+99:99` and `Zgarbage` are accepted).
 */
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
  if (!is_valid_date(Date{year, static_cast<unsigned>(month), static_cast<unsigned>(day)}) ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
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

namespace std {

/**
 * @brief Hashes an order reference, so it can key an unordered container.
 * @tparam Broker The broker tag.
 */
template <typename Broker>
struct hash<algotradelib::OrderRef<Broker>> {
  /**
   * @brief Hashes both parts of the reference.
   * @param ref The reference.
   * @return The hash.
   */
  std::size_t operator()(const algotradelib::OrderRef<Broker>& ref) const noexcept {
    std::size_t seed = hash<std::string>{}(ref.primary_);
    seed ^= hash<std::string>{}(ref.secondary_) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

} // namespace std
