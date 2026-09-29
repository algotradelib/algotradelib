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
 * Identifies a country supported by the compiled library.
 *
 * The set is intentionally closed and allocation-free. Supporting a new
 * country requires adding its normalized value together with broker mapping
 * and tests; unknown wire values must not silently enter strategy code.
 */
enum class Country {
  KR,
  US,
};

/**
 * Selects a normalized execution or quotation venue supported by the library.
 *
 * Broker wire codes remain in adapters. New normalized venues are added with
 * their broker mappings and tests.
 */
enum class Venue {
  UNSPECIFIED,
  KRX,
  NXT,
  COMBINED,
};

/**
 * Returns a stable name for `venue`, for a message a person reads.
 *
 * **`runner::venue_name` deliberately keeps its own copy.** That one is the journal's on-disk
 * spelling, and a recorded format must not move because a display name was reworded; they agree
 * today and the journal's round-trip tests are what hold it to its own.
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

/** Broker-neutral identity for a financial instrument. */
struct InstrumentId {
  /** The listing country, which selects the symbol convention and the price grid. */
  Country country_{Country::KR};
  /** The identity venue, which participates in equality; routing is `OrderRequest::venue_`. */
  Venue venue_{Venue::COMBINED};
  /** The broker's own symbol, unpadded and as the venue spells it (`"005930"`). */
  std::string symbol_;

  /** Compares country, venue and symbol; two instruments differing in venue are different. */
  friend bool operator==(const InstrumentId& lhs, const InstrumentId& rhs) {
    return lhs.country_ == rhs.country_ && lhs.venue_ == rhs.venue_ && lhs.symbol_ == rhs.symbol_;
  }

  /**
   * Orders identities so they can key a map. The ordering itself carries no meaning.
   *
   * It compares **exactly what `operator==` compares**, which is the whole reason it is declared
   * beside it: a container keyed on less than all three fields silently merges two instruments that
   * are not equal. `runner::Runner`'s price bands were such a container -- keyed on `symbol_` alone,
   * so a KRX identity and an NXT identity for the same stock overwrote each other's band.
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
 * Fixed-capacity uppercase code, stored without allocating.
 *
 * Some broker vocabularies are open sets: a market segment, a venue, or an
 * exchange identifier may take a value this library has not seen. A closed
 * `enum class` would have to either reject it or collapse it into a sentinel, and
 * both lose information the broker actually sent.
 *
 * `Code` preserves such a value exactly, in place, so an unrecognized code can be
 * carried through normalization, compared, and displayed without a heap
 * allocation and without pretending to be understood. Where a value *is*
 * understood, a normalized enum stays the right representation; this is the
 * companion for the rest.
 */
template <std::size_t N>
class Code {
public:
  /** Constructs an empty code. */
  Code() = default;

  /**
   * Constructs a code from up to `N` uppercase ASCII characters or digits.
   *
   * @throws std::invalid_argument if `text` is too long or contains anything
   * other than `A`-`Z`, `0`-`9`, or `_`.
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
   * Returns a code holding `text`, or nothing when it cannot be stored.
   *
   * Normalization paths use this rather than the throwing constructor. An
   * unfamiliar code is exactly the case this type exists for, so encountering one
   * must not throw out of a response parse and fail an entire listing over a
   * single odd value.
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

  /** Returns the code as a view over its own storage. */
  std::string_view view() const noexcept {
    return std::string_view(characters_.data(), size_);
  }

  /** Returns the code as a string. */
  std::string str() const { return std::string(view()); }

  /** Returns whether the code carries no value. */
  bool empty() const noexcept { return size_ == 0; }

  /** Returns how many characters the code holds. */
  std::size_t size() const noexcept { return size_; }

  /** Returns the greatest number of characters a code of this width can hold. */
  static constexpr std::size_t capacity() noexcept { return N; }

  /** Compares the stored characters. */
  friend bool operator==(const Code& lhs, const Code& rhs) { return lhs.view() == rhs.view(); }

  /** The negation of `operator==`. */
  friend bool operator!=(const Code& lhs, const Code& rhs) { return !(lhs == rhs); }

private:
  std::array<char, N> characters_{};
  std::uint8_t size_{0};
};

/** Validated ISO 4217-style three-letter currency code. */
class Currency {
public:
  /** Constructs Korean won. */
  Currency() : code_{'K', 'R', 'W'} {}

  /**
   * Constructs a currency from a three-character uppercase code.
   *
   * @throws std::invalid_argument if `code` is not three uppercase ASCII
   * characters.
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

  /** Returns Korean won. */
  static Currency krw() { return Currency("KRW"); }

  /** Returns United States dollars. */
  static Currency usd() { return Currency("USD"); }

  /** Returns the three-letter currency code. */
  std::string str() const { return std::string(code_.begin(), code_.end()); }

  /** Compares two currencies by their ISO 4217 code. */
  friend bool operator==(const Currency& lhs, const Currency& rhs) {
    return lhs.code_ == rhs.code_;
  }

private:
  std::array<char, 3> code_{};
};

/** Exact signed fixed-point decimal represented by a coefficient and scale. */
class Decimal64 {
public:
  /** Constructs zero. */
  Decimal64() = default;

  /** Constructs `coefficient * 10^-scale` without normalization. */
  Decimal64(std::int64_t coefficient, std::uint8_t scale)
  : coefficient_(coefficient), scale_(scale) {}

  /**
   * Parses and normalizes a decimal string without binary floating point.
   *
   * @throws std::invalid_argument for malformed input.
   * @throws std::out_of_range when the coefficient or scale cannot be stored.
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

  /** Returns the signed integer coefficient. */
  std::int64_t coefficient() const noexcept { return coefficient_; }

  /** Returns the count of fractional decimal digits. */
  std::uint8_t scale() const noexcept { return scale_; }

  /** Returns a normalized decimal string. */
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
   * Compares the stored representation: the coefficient **and** the scale.
   *
   * **On the parse path that is the same as comparing the number**, because `parse` normalizes:
   * it strips trailing zeros, so `parse("1.50")` and `parse("1.5")` are both `15` at scale `1` and
   * compare equal. Two broker strings for the same figure agree whatever their trailing zeros, which
   * is what reconciliation needs.
   *
   * **It differs for a value built at an explicit scale**, which is not normalized: the
   * two-argument constructor and `analytics::to_decimal(value, scale)`. `Decimal64(150, 2)` and
   * `Decimal64(15, 1)` are not equal, and neither are `to_decimal(1.5, 3)` -- which is `1500` at
   * scale `3` -- and `parse("1.5")`. Compare those with `decimal_subtract`, or on `str()` at a
   * known scale, as `analytics/conversion.hpp` already says.
   */
  friend bool operator==(const Decimal64& lhs, const Decimal64& rhs) {
    return lhs.coefficient_ == rhs.coefficient_ && lhs.scale_ == rhs.scale_;
  }

private:
  std::int64_t coefficient_{0};
  std::uint8_t scale_{0};
};

/**
 * Returns `value` with trailing fractional zeros removed.
 *
 * `Decimal64` equality compares the coefficient and the scale, so `1.50` and
 * `1.5` are distinct values. Normalizing an arithmetic result keeps it comparable
 * to the same number parsed from a broker's string.
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
 * Returns the exact sum, or nothing when it does not fit.
 *
 * The result is optional rather than wrapping or throwing. These operations sit on
 * paths that run per quote and per bar, where a throw is the wrong cost and a
 * silent wrap on a price is never acceptable; an empty result says "this number
 * cannot be represented" in the same vocabulary the rest of the library already
 * uses for absent values.
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

/** Returns the exact difference, or nothing when it does not fit. */
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
 * Returns exactly half of `value`, or nothing when it does not fit.
 *
 * Halving an odd coefficient needs one more decimal place, so it is taken rather
 * than rounded away: rounding belongs to whoever is about to act on the number,
 * not to the arithmetic that produced it.
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
 * Returns the exact product, or nothing when it does not fit.
 *
 * The scales add, which is what makes the product exact: two prices at scale 2 multiply to a
 * value at scale 4, and truncating that here would be rounding on behalf of a caller who has
 * not yet said where they want it. `decimal_normalized` then removes only trailing zeros, which
 * changes no value.
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
 * Returns `lhs / rhs` truncated toward zero at `scale`, or nothing when it does not fit.
 *
 * The result scale is stated by the caller because a quotient has no natural one: a division
 * that does not terminate has to stop somewhere, and only the caller knows where. Truncation
 * rather than rounding, for the same reason: a quotient this library rounded would be a value
 * the caller did not choose. `decimal_divide(x, y, 0)` is therefore "how many whole `y` fit in
 * `x`", which is what sizing an order in shares needs.
 *
 * Division by zero returns nothing rather than throwing, matching how the rest of this
 * arithmetic reports a value it cannot produce.
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
 * Returns the largest multiple of `step` not exceeding `value`, or nothing when it does not fit.
 *
 * Toward negative infinity, not toward zero, so the guarantee holds on both sides of zero: the
 * result is always less than or equal to the input. A caller rounding a quantity relies on that
 * — rounding a sell quantity toward zero would sell less than asked while rounding a negative
 * target toward zero would hold more than asked, and only one of those is a shortfall.
 *
 * `step` must be positive; a zero or negative step has no multiples to choose between.
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

/** Returns whether `lhs` is numerically less than `rhs`, comparing exactly. */
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

/** Broad category for transport and broker-operation failures. */
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
 * Structured error returned by transport and broker operations.
 *
 * `retry_after_` and `rate_limit_` are populated only for
 * `ErrorCategory::RATE_LIMIT`, and only where the broker reported them.
 * `rate_limit_` is the allowance the broker says it is enforcing, in requests per
 * second, which is a better correction than any guess a client could make. A
 * broker that reports neither leaves both empty rather than having a number
 * invented for it.
 */
struct BrokerError {
  /** What kind of failure this is, and the only field a caller should branch on. */
  ErrorCategory category_{ErrorCategory::PROTOCOL};
  /**
   * Whether a request actually went out before this error was produced.
   *
   * **False only when a client refused before any I/O**, which is a normal thing for a client to do
   * -- an order id too long for the broker's format, a time in force it does not support -- and is
   * indistinguishable from a broker's own refusal by `category_` alone: both are
   * `INVALID_ARGUMENT`. The difference matters twice over. A request that never went was not
   * counted by the broker, so counting it against a rate window refuses a later order that would
   * have been allowed; and a report saying the *broker* refused an instruction it never received
   * sends a reader to the wrong place.
   *
   * Defaults to true, which is the safe direction on both counts: a client that forgets to say it
   * refused locally over-counts its budget and over-attributes the refusal, and neither loses an
   * order or hides a failure.
   */
  bool reached_broker_{true};
  /** The HTTP status, or `0` when the failure happened before a response existed. */
  int http_status_{0};
  /** The broker's own error code, verbatim (KIS's `EGW00201`, Toss's error identifier). */
  std::string broker_code_;
  /** The broker's correlation identifier for this request, where it published one. */
  std::string request_id_;
  /** A human-readable description, for a log or an operator; never parsed. */
  std::string message_;
  /**
   * How long the broker asked the caller to wait, where it said.
   *
   * Empty means either that the broker sent no `Retry-After`, or that it sent one
   * `broker_detail::integer_header` could not read -- malformed, or negative. A caller deciding
   * when to retry cannot tell the two apart and should back off on its own rather than retry at once.
   */
  std::optional<std::chrono::seconds> retry_after_;
  /**
   * The allowance the broker states it is enforcing, in requests per second, where it published one.
   *
   * Read through `broker_detail::integer_header` and then `reported_rate_limit`, so empty covers an
   * absent header and one that could not be used: malformed, negative, zero, or above `INT_MAX`.
   * A 429 can therefore carry `X-RateLimit-Limit` and still leave this empty. KIS states no
   * allowance at all, with either of its two metering layers, so this is empty on every KIS refusal.
   */
  std::optional<int> rate_limit_;
};

/** Value-or-error result with no exception required on the success path. */
template <typename T>
class Result {
public:
  /** Constructs a successful result. */
  Result(T value) : value_(std::move(value)) {}

  /** Constructs a failed result. */
  Result(BrokerError error) : value_(std::move(error)) {}

  /** Returns whether this result contains a value. */
  bool has_value() const noexcept { return std::holds_alternative<T>(value_); }

  /** Returns whether this result contains a value. */
  explicit operator bool() const noexcept { return has_value(); }

  /** Returns the contained value. */
  T& value() { return std::get<T>(value_); }

  /** Returns the contained value. */
  const T& value() const { return std::get<T>(value_); }

  /** Returns the contained error. */
  BrokerError& error() { return std::get<BrokerError>(value_); }

  /** Returns the contained error. */
  const BrokerError& error() const { return std::get<BrokerError>(value_); }

private:
  std::variant<T, BrokerError> value_;
};

/**
 * Best bid and offer, with the size resting at each.
 *
 * Every member is optional because a quote may arrive before either side has a
 * resting order, and because a broker may publish a price without a size. An
 * absent value means the broker did not state one, never zero.
 */
struct TopOfBook {
  /** The best bid the broker published. Empty means it stated none, never zero. */
  std::optional<Decimal64> bid_price_;
  /** The size resting at `bid_price_`, where the broker published one. */
  std::optional<Decimal64> bid_quantity_;
  /** The best offer the broker published. Empty means it stated none, never zero. */
  std::optional<Decimal64> ask_price_;
  /** The size resting at `ask_price_`, where the broker published one. */
  std::optional<Decimal64> ask_quantity_;
};

/**
 * Normalized point-in-time market quote.
 *
 * `top_of_book_` and `volume_` are optional additions: the members that existed
 * before keep their meaning, so a caller written against the earlier shape is
 * unaffected.
 */
struct QuoteSnapshot {
  /** What was quoted. */
  InstrumentId instrument_;
  /** The last traded price, exact as the broker reported it. */
  Decimal64 last_price_;
  /** The currency `last_price_` is denominated in. */
  Currency currency_;
  /** When the exchange says the quote was true, where the broker published a timestamp. */
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
  /** When this process received the answer. A local clock, and the weaker of the two claims. */
  std::chrono::system_clock::time_point received_at_;

  /**
   * Best bid and offer alongside the quote -- **and no adapter fills this today**.
   *
   * Neither the KIS nor the Toss quote parser writes it, so it is `std::nullopt` on every quote
   * either broker returns; the only values that ever appear here are ones a caller or a test
   * constructed. It is not a capability gate away, it is simply unwired.
   *
   * The book is available, through `session.depth()` -> `DepthSnapshot`, which both adapters do
   * populate and both declare with `BrokerCaps::has_depth`. Use that for a spread or a midpoint.
   */
  std::optional<TopOfBook> top_of_book_;
  /** Cumulative traded volume for the session, where the broker published it. */
  std::optional<Decimal64> volume_;
};

/** Exact monetary amount paired with its currency. */
struct Money {
  /** The exact amount. */
  Decimal64 amount_;
  /** The currency it is denominated in; amounts in different currencies are never added. */
  Currency currency_;

  /** Compares amount and currency. Equal amounts in different currencies are not equal. */
  friend bool operator==(const Money& lhs, const Money& rhs) {
    return lhs.amount_ == rhs.amount_ && lhs.currency_ == rhs.currency_;
  }
};

/**
 * What a sellable figure is evidence of.
 *
 * `SellableSource` above says **where** the figure came from, which is a fact about this library's
 * plumbing and is correctly a compile-time constant. This says **what it means**, which is the
 * question `RiskGuard` actually asks of it, and it is a run-time value because one broker's answer
 * is a caller's assertion about an account rather than a property of the adapter.
 *
 * **Why a type and not a comment.** `HoldingPosition::sellable_quantity_` documents in prose that
 * whether the figure is already net of resting sell orders "is not stated by either broker and is
 * not assumed here" -- and then the guard reads the number and trusts it completely. A third party
 * writing an adapter fills that field from a vendor field whose scope they may never have
 * established, and nothing tells them. Both axes below therefore default to `UNESTABLISHED`, which
 * refuses: a safety rule has to fail closed for code this library has never seen.
 */
struct SellableBasis {
  /** Whether the figure covers the whole holding it will be measured against. */
  enum class Coverage {
    /** Nobody has established what the figure covers. The guard refuses. */
    UNESTABLISHED,
    /** It covers every share held of the instrument, whatever venue or lot they sit in. */
    WHOLE_HOLDING,
    /**
     * It covers only part of the holding.
     *
     * The guard refuses this too, and the value exists so the refusal can say *which* gap it is: a
     * figure covering one venue's slice cannot bound a sell of the whole position, and reporting
     * that as "unestablished" would lose the distinction between nobody having looked and somebody
     * having looked and found a partial answer.
     */
    PARTIAL,
  };

  /**
   * How the guard must treat resting sell orders against this figure.
   *
   * **A treatment, not a claim about the broker**, and that distinction is load-bearing. An earlier
   * revision of this type named these `GROSS` and `NET` -- assertions about what a vendor reports.
   * No broker publishes that fact, so no adapter could honestly declare one, so every guarded sell
   * refused at two of three brokers. A policy this library applies is always statable; what nobody
   * may state is a measurement nobody made, and `verified_` is where that goes.
   */
  enum class RestingTreatment {
    /** No treatment stated. The guard refuses. */
    UNESTABLISHED,
    /**
     * Deduct every resting sell.
     *
     * Declarable by any adapter without establishing anything, because it errs in the only safe
     * direction: if the broker had already deducted them the result over-refuses, which costs a
     * trade, where the other error sells the same shares twice.
     *
     * **There is deliberately no second value, and the reason is worth keeping.** A broker that
     * reports a figure already net of resting orders is double-counted by this rule, which refuses an
     * honest exit -- a real defect, and one this axis was originally shaped to express. It cannot be
     * expressed safely with what a broker publishes. Such a figure accounts for the orders the broker
     * saw *when it computed the figure*, so a guard trusting it must still deduct anything
     * acknowledged since -- and nothing can identify those orders. `OrderSnapshot::ordered_at_` is
     * optional and no shipped adapter fills it, because KIS's working-order inquiry publishes no
     * timestamp at all; and a working-order poll replaces this library's own reservation rows with the
     * broker's, losing the one marker that distinguished them. So the only implementable rule is this
     * one, and an enumerator whose sole safe behaviour duplicates another is not offered.
     * `plan/todo/netted-sellable-figures.md` carries the work.
     */
    DEDUCT,
  };

  /** What the figure covers. Unestablished by default, which refuses. */
  Coverage coverage_ = Coverage::UNESTABLISHED;
  /** How the guard must treat resting sells against it. Unestablished by default, which refuses. */
  RestingTreatment resting_ = RestingTreatment::UNESTABLISHED;
  /**
   * False when this basis is a stated treatment rather than a measured broker fact.
   *
   * Nothing in this library sets it true. It is here so that "we apply the conservative rule" stays
   * distinguishable, in the type, from "somebody read a real response and confirmed this" -- the
   * distinction `issues/pre-trade-checks-sellable-deduction.md` records in prose because no type
   * could hold it.
   */
  bool verified_ = false;

  /** Whether the guard may measure a sell against a figure carrying this basis at all. */
  constexpr bool is_enforceable() const noexcept {
    return coverage_ == Coverage::WHOLE_HOLDING && resting_ != RestingTreatment::UNESTABLISHED;
  }
};

/** Returns a stable lowercase name for `coverage`, for logs and refusal detail. */
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

/** Returns a stable lowercase name for `treatment`, for logs and refusal detail. */
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

/** One broker-neutral position in an account holdings snapshot. */
struct HoldingPosition {
  /** What is held. A broker reports a holding with no venue, so this carries the identity venue. */
  InstrumentId instrument_;
  /** The instrument's display name as the broker spells it. */
  std::string name_;
  /**
   * The quantity in this row.
   *
   * A broker may report one instrument across several rows, so this is not necessarily the whole
   * position. Size a holding with `Portfolio::net_quantity_of`, never from one row.
   */
  Decimal64 quantity_;
  /**
   * How much of it the broker says may be sold now.
   *
   * Empty means the broker did not state one, which is not the same as zero. Whether the figure is
   * already net of resting sell orders is not stated by either broker and is not assumed here.
   */
  std::optional<Decimal64> sellable_quantity_;
  /**
   * What *this row's* `sellable_quantity_` is evidence of, when it did not come from the holdings read.
   *
   * Empty means the figure arrived with the snapshot and `HoldingsSnapshot::sellable_basis_` describes
   * it, which is the ordinary case. It is filled only where a figure reached this row by another route
   * -- a per-instrument inquiry, which answers for the one instrument asked about and says nothing
   * about any other row.
   *
   * **Why a row needs its own at all.** Promoting an inquiry's basis to the whole portfolio lets one
   * instrument's declaration certify another's: answer A by inquiry, and a populated but undeclared
   * figure sitting on B is suddenly enforceable, so a wrong-sized B sell reaches the wire. Provenance
   * has to stay with the figure it describes.
   */
  std::optional<SellableBasis> sellable_basis_;
  /** The broker's own average cost for the position. */
  Decimal64 average_purchase_price_;
  /** The mark the broker valued the position at. */
  Decimal64 last_price_;
  /** Cost basis, as the broker computed it. */
  Money purchase_value_;
  /** Current value, as the broker computed it. */
  Money market_value_;
  /** Unrealized profit and loss on the position, as the broker reported it. */
  Money unrealized_profit_loss_;
  /** The broker's own return figure, as a fraction rather than a percentage. */
  Decimal64 unrealized_return_;
};

/** Gross holdings totals for one currency without implicit conversion. */
struct HoldingsTotals {
  /** The currency these totals are in. There is one row per currency and no conversion. */
  Currency currency_;
  /**
   * The broker's own cost-basis total for `currency_`.
   *
   * Read from the broker's summary row, **not** summed from `positions_`. The two need not agree:
   * a paged holdings inquiry can return part of the positions beside an account-wide summary.
   */
  Decimal64 purchase_value_;
  /** The broker's own market-value total for `currency_`. Read from its summary, as above. */
  Decimal64 market_value_;
  /** The broker's own unrealized profit and loss for `currency_`. Read from its summary, as above. */
  Decimal64 unrealized_profit_loss_;
};


/** Point-in-time normalized positions and per-currency totals. */
struct HoldingsSnapshot {
  /** Every position row the broker reported; one instrument may appear in more than one. */
  std::vector<HoldingPosition> positions_;
  /** One totals row per currency held. */
  std::vector<HoldingsTotals> totals_;
  /** When this process received the answer, which is what a staleness gate measures against. */
  std::chrono::system_clock::time_point received_at_;
  /**
   * What `HoldingPosition::sellable_quantity_` on these rows is evidence of.
   *
   * Describes **every** populated row in this snapshot, which is why it sits here rather than on
   * each row: one read has one basis, and a per-row basis would invite an adapter to vary it
   * without evidence. An adapter whose response would need two different bases to describe it must
   * fail the snapshot rather than pick one.
   *
   * Defaults to unestablished, so an adapter that fills a sellable quantity and states nothing has
   * its sells refused rather than measured against a figure nobody has characterized.
   */
  SellableBasis sellable_basis_;
};

/**
 * Where this library learns what may be sold of a position.
 *
 * Named for the **source** rather than for a capability, deliberately. A `has_*` constant on
 * `BrokerCaps` answers whether the *broker* implements an operation, and by that question KIS's
 * answer is "yes, and this library does not use it": 매도가능수량조회 exists and is documented, and
 * the figure it returns is already on every holdings snapshot, so spending a request per instrument
 * to fetch it again -- on an endpoint a demo account cannot call -- would buy nothing.
 *
 * Saying where the figure comes from states what is true without implying a gap in anyone's API,
 * and it gates the runner with one `if constexpr`.
 */
enum class SellableSource {
  /** It arrives on the holdings snapshot; filling it costs no request at all. */
  HOLDINGS,
  /** The port has a per-instrument inquiry, spent only where the snapshot is silent. */
  INQUIRY,
  /** This library has no way to learn it for this broker. */
  NONE,
};

/** Returns a stable lowercase name for `source`, for logs and structured reporting. */
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
 * What the broker says may be spent, in one currency.
 *
 * Deliberately not cash, and the distinction is why this type exists rather than a `Money` in the
 * portfolio's cash list. The distinction is **ledger-modelled cash versus broker-reported buying
 * power**: `Portfolio::cash_` is what this library's own accounting says, and in a replay that
 * accounting credits a sale on the fill (`backtest::ReplayLedger::apply`) because it models no
 * settlement schedule at all. A broker's buying power is the broker's own answer, which prices in
 * unsettled proceeds on its own terms and excludes whatever it is already reserving against resting
 * orders. Carrying that answer in the field meant for the library's accounting would make one
 * number mean two things depending on who last wrote it.
 *
 * `cash_` is the credit-free figure both brokers publish — Toss's `cashBuyingPower`, KIS's
 * `nrcvb_buy_amt`. A credit-inclusive figure is that broker's own and stays reachable through its
 * client: this library has no model of credit, and a number without the model is how a strategy
 * discovers leverage by accident.
 *
 * `received_at_` is the local clock, not the broker's, because neither broker timestamps the
 * answer. It is therefore when the figure *arrived* rather than when it was true, which is the
 * weaker of the two claims and is the one this field makes. A replay fills it with the snapshot
 * time for the same reason — that is when the modelled figure was read — and a caller must not read
 * it as evidence that a broker answered at that moment.
 */
struct BuyingPower {
  /** The spendable amount the broker stated, with its currency. */
  Money cash_;
  /** When the figure arrived, on this machine's clock; see the note above on what that claims. */
  std::chrono::system_clock::time_point received_at_{};
};

/**
 * Calendar date without a time of day or zone.
 *
 * Settlement and inquiry-range values are calendar dates in the broker's own
 * locale. Converting them to a time point would attach a zone the broker never
 * stated, so they are kept as plain dates.
 */
struct Date {
  /**
   * The four-digit year.
   *
   * **A `Date` parsed from a broker string is checked digit by digit.** `std::stoi` skips leading
   * whitespace, accepts a sign, takes the digits that follow and ignores the rest, so it returns `2`
   * for `"2x"`, `" 2"` and `"+2"` alike, and a fixed-width field split into components and handed to
   * it becomes a real date that is not the one on the wire. No adapter reads a date that way any
   * more: KIS and Kiwoom share `broker_detail::compact_date` for `YYYYMMDD`, and Toss's
   * `parse_calendar_date` checks `YYYY-MM-DD` the same way, so a malformed value is unreadable rather
   * than different. Where it lands then depends on the field -- `StatedDate::State::UNREADABLE` for
   * an instrument's dates, a dropped and counted row for a calendar day.
   */
  int year_{0};
  /** The month, `1`-`12`. Zero means unset. */
  unsigned month_{0};
  /** The day of the month, `1`-`31`. Zero means unset. */
  unsigned day_{0};

  /** Compares two calendar dates for component-wise equality. */
  friend bool operator==(const Date& lhs, const Date& rhs) {
    return lhs.year_ == rhs.year_ && lhs.month_ == rhs.month_ && lhs.day_ == rhs.day_;
  }

  /** Orders calendar dates chronologically. */
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
 * Returns the Gregorian date `days` after 1970-01-01, counting negative days backwards.
 *
 * Pure arithmetic on a day count, deliberately: `std::chrono`'s calendar types are C++20 and this
 * library promises C++17, and the alternatives are worse than the eleven lines below. `gmtime_r` is
 * not portable to Windows without a second spelling and both take a `time_t`, which is not
 * guaranteed to be wide enough; `localtime` would attach the *process's* zone to a date that belongs
 * to a market. This is Howard Hinnant's `civil_from_days`, whose derivation and proof are published
 * with it, and it is exact for every representable day rather than for a range somebody tested.
 *
 * The result is not range-checked against `is_valid_date`: every day count maps to a real date, and
 * a caller handing this a day count outside year 1..9999 has a bug this function cannot diagnose.
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

/** Returns whether `date` is a valid Gregorian calendar date. */
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

/** Which kind of venue a `VenueKey` names. */
enum class VenueKeyKind {
  /** Applies to any venue. Every rate row that existed before venue-aware costing is this. */
  ANY,
  /** A `Venue` this library normalizes. */
  NORMALIZED,
  /** A code the broker named that this library does not normalize, preserved verbatim. */
  RAW,
};

/**
 * Which venue a commission rate applies to, as a key that cannot be ambiguous.
 *
 * **The three kinds are mutually exclusive, and that is the whole point.** An earlier design carried a
 * `Venue` beside a preserved code and let `Venue::UNSPECIFIED` mean "no venue stated", which made an
 * unnormalized row indistinguishable from a venue-neutral one -- so a rate published for one unknown
 * market could be charged on any venue at all. "Nobody said" is `ANY`; "a venue I know" is
 * `NORMALIZED`; "a venue I do not know, spelled as the broker spelled it" is `RAW`. A `RAW` key never
 * matches a `NORMALIZED` one even when the text looks the same, because the library has not established
 * that they mean the same market.
 *
 * `Venue::UNSPECIFIED` is refused as a normalized key for the same reason: it is the absence of a
 * statement, and the absence of a statement is `ANY`.
 *
 * **`Venue::COMBINED` is storable and is refused as an *execution* assertion**, and the two halves of
 * that belong in different places. A broker that publishes a consolidated rate must be representable,
 * so the key holds it. But `COMBINED` is what `normalized_venue` maps KIS's `SOR` to -- consolidated
 * *routing*, not a venue an order matched on -- so `backtest::CommissionPolicy` rejects it as the venue
 * a fill is asserted to have happened at. Costing against it would be a guess wearing a normalized name.
 */
class VenueKey {
public:
  /** Constructs the neutral key, which applies to any venue. */
  VenueKey() = default;

  /** Returns the neutral key: this rate applies whatever venue a fill happened at. */
  static VenueKey any() noexcept { return VenueKey(); }

  /**
   * Returns a key naming a venue this library normalizes, or nothing for `UNSPECIFIED`.
   *
   * `UNSPECIFIED` is refused rather than accepted and treated as neutral, because a caller who has a
   * `Venue` variable holding it has not stated a venue and should say so with `any()`.
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

  /** Returns a key preserving a venue code this library does not normalize, or nothing if it will not fit. */
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

  /** Returns which kind of venue this key names. */
  VenueKeyKind kind() const noexcept { return kind_; }

  /** Returns whether this key applies to any venue. */
  bool is_any() const noexcept { return kind_ == VenueKeyKind::ANY; }

  /** Returns the normalized venue; meaningful only when `kind()` is `NORMALIZED`. */
  Venue venue() const noexcept { return venue_; }

  /** Returns the preserved code; empty unless `kind()` is `RAW`. */
  const Code<8>& raw_code() const noexcept { return raw_; }

  /**
   * Returns whether this key is the one `asserted` names.
   *
   * Exact in both kind and value. A `RAW` key never matches a `NORMALIZED` one, and neither matches
   * `ANY`: falling back to a neutral rate is `CommissionSchedule::rate_for`'s second pass, deliberately
   * not something this function does, so that "an exact rate exists" and "a neutral rate exists" can
   * never be confused at the point of comparison.
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

  /** Compares kind and value. */
  friend bool operator==(const VenueKey& lhs, const VenueKey& rhs) noexcept { return lhs.matches(rhs); }

  /** The negation of `operator==`. */
  friend bool operator!=(const VenueKey& lhs, const VenueKey& rhs) noexcept { return !(lhs == rhs); }

private:
  VenueKeyKind kind_{VenueKeyKind::ANY};
  Venue venue_{Venue::UNSPECIFIED};
  Code<8> raw_;
};

/** Returns a stable lowercase name for `kind`, for a report to state what a rate was keyed on. */
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

/** Where a commission schedule's numbers came from. */
enum class CommissionOrigin {
  /** The broker published them through an inquiry this library called. */
  BROKER_PUBLISHED,
  /** A caller stated them, from a published fee page or their own agreement. */
  CALLER_STATED,
};

/** Returns a stable lowercase name for `origin`, for a report to state where a rate came from. */
inline constexpr std::string_view commission_origin_name(CommissionOrigin origin) noexcept {
  switch (origin) {
    case CommissionOrigin::BROKER_PUBLISHED:
      return "broker_published";
    case CommissionOrigin::CALLER_STATED:
      return "caller_stated";
  }
  return "unknown";
}

/** Whether a dated row covers a date, when that is knowable at all. */
enum class RateApplicability {
  /** A bound this library could read puts the date outside the row's window. */
  NO,
  /** Every bound was readable, and they cover the date. */
  YES,
  /**
   * No readable bound excludes the date, and a bound that could not be read might have.
   *
   * Deliberately not folded into `NO`: the row may well be the one in force, so a caller who reaches
   * for another row here charges a rate on the strength of something nobody established.
   */
  UNREADABLE,
};

/**
 * One commission rate a broker charges, and the dates it applies on.
 *
 * A rate is a fraction of traded value rather than a percentage, matching
 * `backtest::CommissionModel::rate_`: `0.00015` is the order of magnitude a Korean online
 * brokerage charges.
 *
 * Both bounds are optional. Absent usually means open-ended in that direction, which is how Toss
 * publishes them — a `null` end date is a rate with no scheduled change. Absent is deliberately not
 * "today": a schedule that started before the account did is a normal thing to be told, and
 * inventing a start for it would make a date comparison say something the broker did not.
 *
 * **Absent is not the only way a bound can be empty, and the two are held apart.** A bound that was
 * present and could not be read leaves the optional empty *and* sets its `_unreadable_` flag, so
 * `applies_on` answers a definite no rather than treating the row as unbounded. Before that
 * distinction existed, a damaged `endDate` made an expired rate read as current and a damaged
 * `startDate` made a future one read as already in force — an unreadable value downgraded to the
 * most permissive reading available, which is the shape Decision 6 exists to forbid.
 *
 * There is no per-order floor and no transaction tax here, because neither broker publishes one.
 */
struct CommissionRate {
  /** The market this rate applies to, when the library normalizes it; see `other_` when it does not. */
  Country country_{Country::KR};
  /**
   * The market the broker named, when this library does not normalize it.
   *
   * Empty on every row whose market is one of the normalized countries, and `country_` means
   * nothing when this is set. Toss's specification requires a client to accept unknown values of
   * its market enum, and this library's answer to an unrecognized wire value is to preserve it
   * rather than drop it — the same shape `InstrumentId::other_` uses, for the same reason: a
   * schedule that silently omitted a market would be a successful, incomplete answer with no way
   * for a caller to see what was missing.
   *
   * A code too long or too odd for `Code<16>` cannot be preserved, and such a row is skipped rather
   * than reported under a market it does not belong to.
   */
  Code<16> other_;
  /**
   * Which venue this rate applies to.
   *
   * `VenueKey::any()` on every row a broker publishes today, and on every row that existed before
   * venue-aware costing — which is what makes the venue dimension additive rather than a migration.
   * Neither broker publishes a per-venue rate: KRX and NXT charge differently and a caller states
   * that from the published fee page, keyed on their own account's channel.
   */
  VenueKey venue_;
  /** The broker's own commission, as a fraction of traded value: `0.00015`, not `0.015`. */
  Decimal64 rate_;
  /**
   * The second charged component, when the rate's source distinguishes one -- and **empty when
   * nobody said**.
   *
   * Korean equity trading carries 유관기관제비용 beside the broker's commission: an institution fee,
   * charged by different parties (the exchange, the depository), differing by venue independently of
   * the commission. A schedule carrying one combined number is wrong for whichever component it omits.
   *
   * **Three states, because "no separate charge" and "nobody said" are different answers.** Empty
   * means the source stated one rate and nothing about its composition -- which is every row Toss
   * publishes, since its schedule is a single `commissionRate`. A present zero is a stated zero. A
   * present positive value is a second, non-overlapping charge. A report reading an empty one states
   * its commission total as *inclusive* and its institution total as absent, rather than claiming a
   * known zero for something nobody established.
   */
  std::optional<Decimal64> institution_rate_;
  /**
   * The first date the rate applies on. Empty is open-ended, and deliberately not "today".
   *
   * **Empty means open-ended only when `from_unreadable_` is false.** A bound the source stated and
   * this library could not read is empty here too, which is why the flag exists: the two facts are
   * different and one optional cannot carry both.
   */
  std::optional<Date> from_;
  /** The last date the rate applies on. Empty is no scheduled change, subject to `until_unreadable_`. */
  std::optional<Date> until_;
  /**
   * Whether `from_` is empty because the source stated a start this library could not read.
   *
   * False on every row whose start was absent or parsed, which is every row a caller states by hand
   * and every row a broker publishes readably. True means the broker named a first date in a form
   * nothing here can hold -- a wrong length, a wrong separator, a digit that is not one, a day that
   * is not a real calendar day, or a JSON value that is not a string at all.
   *
   * The row is kept rather than dropped, because the rate itself is intact and one bad bound must
   * not lose a schedule's other rows. What changes is what the row is allowed to answer:
   * `applies_on` returns `RateApplicability::UNREADABLE` for any date the readable bounds do not
   * already exclude, so the row neither applies nor quietly steps aside.
   */
  bool from_unreadable_{false};
  /** @copydoc from_unreadable_ */
  bool until_unreadable_{false};

  /**
   * Returns whether this row names a market this library normalizes.
   *
   * An unnormalized row carries its market in `other_`, so an empty `other_` means `country_` is
   * the answer. A row naming no market at all must therefore never be built: it would read as a
   * normalized row for `country_`'s default, which is to say a rate for Korea the broker never
   * published. Parsers skip such a row and count it in `dropped_`.
   */
  bool normalized() const noexcept { return other_.empty(); }

  /**
   * Returns whether this rate is the one in force on `date`, or that the row cannot say.
   *
   * **Not a `bool`, and the loud break is the point.** This predicate used to return one, meaning
   * "in force", and every caller wrote `if (rate.applies_on(day))`. Adding a third answer under that
   * signature would have made an unreadable window read as `false` at every one of those sites --
   * safe where `false` means "do nothing", and a wrong charge where it means "use the other row
   * instead", which is what a fallback path does. An `enum class` does not convert to `bool`, so
   * every existing caller stops compiling and is made to decide which of the two it is.
   *
   * **Damage the date makes immaterial reports `NO`, not `UNREADABLE`.** A row with a readable start
   * of 2026-01-01 and an unreadable end is a plain no on 2025-06-01: the readable bound already
   * excluded the date and no reading of the other could have changed that. Reporting doubt there
   * would be doubt this library does not have, and a refusal a caller cannot act on is one they
   * learn to route around.
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
   * Returns whether a bound this library **could** read puts `date` outside this row's window.
   *
   * The half of the question that does not depend on damage. It stays public because this is an
   * aggregate and a private section would stop a caller brace-initializing one.
   */
  bool excluded_on(const Date& date) const {
    return (from_ && date < *from_) || (until_ && *until_ < date);
  }
};

/**
 * The commission rates a broker published for one account, or a caller stated for their own.
 *
 * Only one broker publishes any, so a caller reaches the published kind through a capability gate
 * rather than assuming it. An empty schedule from a broker that *does* publish them is a real answer —
 * the account has no stated rates — and is not the same as the broker having no such inquiry.
 */
struct CommissionSchedule {
  /** Every rate row the broker published for the account. Empty is a real answer. */
  std::vector<CommissionRate> rates_;
  /** When the schedule arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_{};
  /** Whether a broker published these numbers or a caller stated them. */
  CommissionOrigin origin_{CommissionOrigin::BROKER_PUBLISHED};
  /**
   * Which tariff these numbers are, in the source's own words.
   *
   * **Required on a caller-stated schedule, and a cost model refuses one without it.** A published
   * fee page is a matrix of channel by venue, and the channel -- 온라인 영업점 개설 계좌,
   * 온라인 뱅키스 계좌, 영업점매매 -- is a property of the account that no API states and that varies
   * by an order of magnitude. Nothing here can select it, so a caller names the row they read, the
   * report prints it, and omitting it is an error rather than a wrong number.
   *
   * A broker-published schedule carries the broker's own name and needs nothing from the caller.
   */
  std::string tariff_;

  /**
   * How many rows the broker sent that could not be represented at all.
   *
   * Nonzero means this schedule is **incomplete**: the broker named a market in a form nothing here
   * can store -- a code too long for `Code<16>`, or no code at all -- and the row was skipped
   * rather than reported under a market it does not belong to. A caller comparing a broker's own
   * statement against this one needs to know the difference exists; a successful-looking schedule
   * that had quietly lost rows is the failure this counter exists to prevent.
   *
   * It is a count rather than the rows themselves because what was lost is precisely the part that
   * could not be held.
   */
  std::size_t dropped_{0};

  /**
   * How many rows carry a date bound the source stated and this library could not read.
   *
   * Nonzero means this schedule is **partly undated**: the rows are all here and their rates are
   * intact, but at least one row's window is unknown, so a question whose answer that row could have
   * changed is refused rather than answered from the rows that happen to be readable.
   *
   * It counts rows, not bounds -- a row with both bounds damaged counts once -- because what a
   * caller does about it is per row: refetch the schedule, or state the rates by hand. Separate from
   * `dropped_`, which counts rows that could not be **held** at all; these are held and cannot be
   * **dated**.
   */
  std::size_t unreadable_bounds_{0};

  /** What a lookup found, and whether an unreadable window is why it found nothing. */
  struct RateSelection {
    /** The rate in force, when one definitely is. */
    std::optional<CommissionRate> rate_;
    /**
     * Whether a row whose window could not be read is why no rate was returned.
     *
     * Never true together with a rate. A caller that falls back on an empty `rate_` -- to a stated
     * tariff, to another schedule, to a zero -- must read this first: an absent rate means the
     * schedule covers no such date, and an unreadable one means it may well cover it and nobody can
     * tell. Falling back on the second charges a number the broker did not publish.
     */
    bool bounds_unreadable_{false};
  };

  /**
   * Returns the rate in force for `country` at `venue` on `date`, and why none is when none is.
   *
   * **Selection is two-pass, and that is what makes the venue dimension additive.** A row keyed to
   * the venue `asserted` names wins; only when no such row exists does a venue-neutral row answer.
   * So a schedule of neutral rows -- which is every schedule a broker publishes -- returns exactly
   * what it returned before venues existed, including one that appends venue rows *after* a neutral
   * row, which a plain first-match rule would have made unreachable.
   *
   * **A venue-specific schedule asked without an assertion refuses.** `VenueKey::any()` matches only
   * neutral rows, so a schedule carrying KRX and NXT rows and no neutral one answers nothing rather
   * than picking a venue nobody named. That is Decision 6: a caller who did not say where a fill
   * happened is told, not guessed at.
   *
   * Within one pass the first matching row wins, as it always has. A broker publishing two
   * overlapping rows for one country, venue and date is describing something this library cannot
   * resolve, and picking the larger or the smaller would be inventing a policy nobody stated.
   *
   * **It returns a `RateSelection` rather than an `std::optional`, and that break is deliberate.**
   * An empty optional said "the schedule covers no such date", and a row whose window could not be
   * read is a different fact wearing the same emptiness. Callers fall back on emptiness, so the
   * type has to make them look.
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
   * Returns whether any rate for `country` carries a dated bound.
   *
   * Asked by a cost model before it prices a market whose trading day it cannot compute: if no rate
   * for that market is dated, every candidate is the same whichever calendar named the day, and if
   * one is, the day decides something and a date from elsewhere cannot decide it. Deliberately not
   * "does the row that would be selected carry a bound" -- the row that would be selected *on the
   * wrong date* says nothing about the row that would have won on the right one.
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
   * Returns the rate in force for `country` on `date` with no venue asserted.
   *
   * The pre-venue spelling, kept so a caller who never named a venue keeps compiling and keeps its
   * answer. It asks for `VenueKey::any()`, so it reads a neutral row and **refuses** a schedule that
   * only carries venue-specific ones rather than choosing one of them.
   */
  RateSelection rate_for(Country country, const Date& date) const {
    return rate_for(country, VenueKey::any(), date);
  }
};

/** Direction of an order. */
enum class OrderSide {
  BUY,
  SELL,
};

/**
 * How an order's execution price is decided: its price *basis*.
 *
 * This is one of three independent facts about an order, and it is worth
 * saying why they are three fields rather than one. A broker names an order
 * with a single code -- KIS's `ORD_DVSN` has twenty-eight of them -- but that
 * code is a product: `16` is a best-limit order, filled-or-killed, in the
 * regular session, and `45` is a best-limit order, immediate-or-cancel, in the
 * after-market. Flattening the product would mean six spellings of "best
 * limit", and every consumer that wanted to ask "is this a best-limit order"
 * would have to enumerate all six. So the basis lives here, the execution
 * condition lives in `TimeInForce`, and the session lives in `OrderSession`.
 *
 * Only combinations a broker actually publishes are accepted on a request; the
 * adapter holds the table and rejects anything else before any network I/O,
 * rather than approximating it with a nearby value. On a read path an
 * unrecognized combination surfaces as `UNKNOWN` with the broker's own code
 * preserved beside it, because an order placed through another channel must
 * still be reportable.
 *
 * **Whether a basis carries a price is a property of the basis**, and callers
 * and adapters both need it. `LIMIT`, `CONDITIONAL_LIMIT` and `STOP_LIMIT` are
 * priced by the caller. `MARKET` is not, and neither are `BEST_LIMIT`,
 * `PRIMARY_LIMIT`, `MID_PRICE` or `CLOSING_PRICE`: each of those names a rule
 * for reading a price off the book or the session rather than a number, which
 * is the whole point of asking for one. `carries_caller_price` states it once so
 * no adapter has to re-derive it.
 */
enum class OrderType {
  UNKNOWN,
  /** 지정가 -- fills at the stated price or better. */
  LIMIT,
  /** 시장가 -- fills at whatever the book offers. */
  MARKET,
  /** 조건부지정가 -- a limit that becomes a market order at the close. */
  CONDITIONAL_LIMIT,
  /** 최유리지정가 -- priced at the best quote on the opposite side. */
  BEST_LIMIT,
  /** 최우선지정가 -- priced at the best quote on the order's own side. */
  PRIMARY_LIMIT,
  /** 중간가 -- priced at the midpoint of the spread. */
  MID_PRICE,
  /** 스톱지정가 -- a limit released when a trigger price is reached. */
  STOP_LIMIT,
  /** 시간외종가 -- priced at a session's closing price rather than quoted. */
  CLOSING_PRICE,
};

/**
 * Returns whether `type` is priced by the caller rather than by the market.
 *
 * A basis that is not priced by the caller must be sent with no price at all:
 * KIS's `ORD_UNPR` is documented as "시장가 등 주문시 "0"으로 입력", and a
 * best-limit or mid-price order takes its price from the book at the moment it
 * is worked. A basis that *is* priced by the caller and is sent without one
 * becomes an order at zero, which is why this is a function rather than a
 * comparison against `LIMIT` repeated in each adapter -- that comparison was
 * wrong for `CONDITIONAL_LIMIT`, and silently: 조건부지정가 is a limit
 * instruction until the close, and it was being transmitted with `ORD_UNPR=0`.
 */
inline constexpr bool carries_caller_price(OrderType type) noexcept {
  return type == OrderType::LIMIT || type == OrderType::CONDITIONAL_LIMIT ||
    type == OrderType::STOP_LIMIT;
}

/**
 * Returns whether `type` is released by a caller-stated trigger price.
 *
 * Only `STOP_LIMIT`, which needs **both** numbers: the trigger decides when the
 * order is released and `carries_caller_price` still says it is a limit once it
 * is. Every other basis takes no trigger, and one stated on such a basis is
 * refused rather than dropped (Decision 6), because an adapter has nowhere to
 * put it and the order sent would be a plain limit the caller did not describe.
 *
 * Neither KIS nor Kiwoom documents the trigger's direction, its tick grid or a
 * relation to the limit, so none of those is checked here or in an adapter; the
 * broker is the authority and its refusal reaches the caller like any other.
 */
inline constexpr bool carries_trigger_price(OrderType type) noexcept {
  return type == OrderType::STOP_LIMIT;
}

/**
 * How long an order lives and what it does with an unfilled remainder.
 *
 * This is the execution condition and nothing else, which is what FIX means by
 * the name. A trading session is a different fact and lives in `OrderSession`:
 * an at-the-close order is a `DAY` order in the `CLOSING_AUCTION` session, not
 * a validity condition of its own. That separation is load-bearing -- Toss
 * spells both on one wire field (`timeInForce` carries `DAY`, `CLS` and `OPG`)
 * while KIS spells both inside `ORD_DVSN`, and a library that mirrored either
 * wire shape would put the same fact in different fields depending on which
 * broker reported it.
 *
 * `UNKNOWN` appears only on a read path, for a condition this library does not
 * normalize. It is never accepted on a request.
 */
enum class TimeInForce {
  UNKNOWN,
  /** Rests until the session ends; an unfilled remainder is cancelled then. */
  DAY,
  /** Fills what it can immediately and cancels the remainder. */
  IMMEDIATE_OR_CANCEL,
  /** Fills entirely and immediately, or not at all. */
  FILL_OR_KILL,
};

/**
 * The trading session an order is submitted into.
 *
 * Both brokers have these and neither puts them where the other does, which is
 * the whole reason this is its own field: Toss reports the opening and closing
 * auctions through `timeInForce` (`OPG`, `CLS`) and KIS reports its pre-market,
 * after-hours and after-market sessions inside `ORD_DVSN`. Normalizing them
 * into one field is Decision 2b applied to two spellings of one fact.
 *
 * A value a broker does not offer is rejected on a request, naming the broker
 * and the session, before any network I/O.
 */
enum class OrderSession {
  UNKNOWN,
  /** The continuous regular session. */
  REGULAR,
  /** The opening single-price auction (시가단일가; Toss `OPG`). */
  OPENING_AUCTION,
  /** The closing auction (Toss `CLS`, a US limit-on-close). */
  CLOSING_AUCTION,
  /** Before the open, at the previous session's close (장전 시간외). */
  PRE_MARKET,
  /** After the close, at that session's close (장후 시간외). */
  AFTER_HOURS,
  /** The after-hours single-price auction (시간외 단일가). */
  AFTER_HOURS_AUCTION,
  /** KRX's after-market session (애프터마켓). */
  AFTER_MARKET,
  /** NXT's GTP session. */
  GTP,
};

/**
 * Lifecycle state of an order.
 *
 * `CANCEL_REJECTED`, `REPLACE_REJECTED`, and `REPLACED` describe the fate of a
 * modify or cancel instruction rather than the original order, matching how
 * brokers record those instructions as separate rows.
 *
 * `UNKNOWN` carries a lifecycle state this library does not recognize. Both
 * broker specifications require clients to tolerate unknown status codes, so a
 * single unrecognized order must not fail an entire order listing.
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
 * Broker-neutral instruction to open a new order.
 *
 * `price_` must be present for `OrderType::LIMIT` and absent for
 * `OrderType::MARKET`; adapters reject any other combination before performing
 * network I/O.
 *
 * `client_order_id_` is an idempotency key where the broker supports one. It is
 * never generated automatically, because a generated key would make a retry
 * look safe on brokers that provide no such guarantee.
 */
struct OrderRequest {
  /** What to trade. Its `venue_` is identity; routing is this struct's own `venue_`. */
  InstrumentId instrument_;
  /** Buy or sell. */
  OrderSide side_{OrderSide::BUY};
  /** The price basis: limit, market, best-limit, conditional-limit, and so on. */
  OrderType type_{OrderType::LIMIT};
  /** The execution condition: day, immediate-or-cancel, fill-or-kill. */
  TimeInForce time_in_force_{TimeInForce::DAY};
  /** The session to trade in: regular, an auction, pre-market, after-hours. */
  OrderSession session_{OrderSession::REGULAR};
  // `UNKNOWN` in `type_`, `time_in_force_` or `session_` is rejected before
  // network I/O, and so is any combination of the three the broker's own code
  // table does not contain for the requested venue.
  /**
   * The venue to route this order to, where the broker offers a choice.
   *
   * **Not `instrument_.venue_`, for the reason `OrderSnapshot::venue_` is not
   * either.** An `InstrumentId` names an instrument and compares its venue, and
   * every consumer that asks "what have I already got resting in this" matches
   * a working order's `instrument_` against the instrument a caller names --
   * `Portfolio::working_quantity`, `strategy::reconcile`, every ceiling in
   * `RiskGuard`, and recovery's `row_corresponds`. A broker reports a holding
   * without a venue, so an order whose identity carried the venue it was routed
   * to would stop matching its own holding, and the resting quantity would read
   * zero: no sellable deduction, no working-order component in the position
   * ceiling, and a reconciliation that plans a second order beside the one
   * already out.
   *
   * So routing lives here and identity stays in `instrument_`, and the request
   * has the same shape as the snapshot it will be read back as. `UNSPECIFIED`
   * sends no venue at all and takes the broker's documented default.
   *
   * An adapter refuses an `instrument_.venue_` that is not the identity venue
   * it reports for that instrument, rather than routing on it, because a caller
   * who sets it there means routing and would otherwise get it silently.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** How much to trade, in the instrument's own units. */
  Decimal64 quantity_;
  /** The limit price. Required for `OrderType::LIMIT`, and refused for `OrderType::MARKET`. */
  std::optional<Decimal64> price_;
  /**
   * The price that releases a stop order, for a basis `carries_trigger_price` names.
   *
   * Required for `OrderType::STOP_LIMIT` beside `price_`, and refused on every other basis. A
   * broker whose `BrokerCaps::has_trigger_price` is false never receives one: `BrokerSession`
   * refuses the request before the adapter, because an adapter that does not read this field
   * would send the order without its releasing condition.
   */
  std::optional<Decimal64> trigger_price_;
  /**
   * An idempotency key, where the broker offers one.
   *
   * Never generated by this library, so its absence stays a visible caller decision. Toss honours
   * one for ten minutes; KIS has none for individual customers and **refuses** a request carrying
   * this field, before authentication and before any socket write.
   */
  std::optional<std::string> client_order_id_;
  /** Acknowledges the broker's high-value-order confirmation, where the broker requires one. */
  bool confirm_high_value_{false};
};

/**
 * Realized execution totals for one order.
 *
 * Monetary members are optional because a broker need not report them, **but a present one is not
 * evidence of a fill and the two adapters differ**. Toss leaves one empty when the broker sent an
 * explicit JSON `null`; a field missing from the response altogether is a protocol error there, not
 * an empty optional. KIS sets `filled_amount_` on every row of its open-order and history listings,
 * from `tot_ccld_amt`, so an unfilled KIS order carries a present `Money{0, KRW}`, and never sets
 * `average_filled_price_` on an open-order row at all.
 *
 * Decide whether an order filled from `filled_quantity_`, which is always present and is zero
 * exactly when nothing has filled. Never from `filled_amount_.has_value()`.
 */
struct OrderExecution {
  /**
   * How much has filled so far. Zero on an unfilled order, and never absent.
   *
   * **This is the field to test a fill against**, for the reason the struct comment gives.
   */
  Decimal64 filled_quantity_;
  /**
   * The volume-weighted fill price, **only where the broker supplies it** -- not whenever a fill
   * exists.
   *
   * KIS's open-order listing never sets it, including on a row it marks `PARTIAL_FILLED`, and its
   * history sets it only for a positive `avg_prvs`. So an empty value says nothing about whether
   * the order filled; `filled_quantity_` does.
   */
  std::optional<Decimal64> average_filled_price_;
  /**
   * The cash the fills came to, as the broker computed it.
   *
   * Present on every KIS listing row, including an unfilled one, where it is zero. Absent at Toss
   * when the broker reported none. See the note above: this is not a fill indicator.
   */
  std::optional<Money> filled_amount_;
  /** Commission charged so far, where the broker reports it per order. Absence as the struct comment describes. */
  std::optional<Money> commission_;
  /** Transaction tax charged so far, where the broker reports it per order. Absence as the struct comment describes. */
  std::optional<Money> tax_;
  /** When the most recent fill happened, where the broker timestamps it. */
  std::optional<std::chrono::system_clock::time_point> filled_at_;
  /** The settlement date the broker stated, kept as a plain date in the broker's own locale. */
  std::optional<Date> settlement_date_;
};

/** Point-in-time normalized state of one order. */
struct OrderSnapshot {
  /** The broker's own order number, as a listing spells it. */
  std::string order_id_;
  /** What the order is for. Carries the identity venue, never the routing one. */
  InstrumentId instrument_;
  /** The instrument's display name as the broker spells it. */
  std::string name_;
  /** Buy or sell. */
  OrderSide side_{OrderSide::BUY};
  /** The price basis this library read from the broker's code. `UNKNOWN` if it did not recognize it. */
  OrderType type_{OrderType::UNKNOWN};
  /** The execution condition read from the broker's code. `UNKNOWN` if it did not recognize it. */
  TimeInForce time_in_force_{TimeInForce::UNKNOWN};
  /** The session read from the broker's code. `UNKNOWN` if it did not recognize it. */
  OrderSession session_{OrderSession::UNKNOWN};
  /**
   * The venue the broker reported this order was routed to, where it reports one.
   *
   * **Not `instrument_.venue_`, and the difference is load-bearing.** An
   * `InstrumentId` names an instrument, and two `InstrumentId`s are equal only
   * when their venues match — which is what `Portfolio::working_quantity`,
   * `strategy::reconcile` and every ceiling in `RiskGuard` key on when they ask
   * "what do I already have resting in this". A holding is reported without a
   * venue by both brokers, so an order row that carried a *different* venue in
   * its `instrument_` would stop matching the holding it belongs to, and the
   * resting quantity would silently read as zero: no sellable deduction, no
   * working-order component in the position ceiling, and a reconciliation that
   * plans a second order beside the one already out.
   *
   * So the routing fact lives here instead. It is what a later amendment must
   * address, and it is `UNSPECIFIED` where the broker reports none.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /**
   * The broker's own order-shape code, preserved verbatim.
   *
   * The three normalized fields above say what this library understood. This
   * says what the broker sent, whether or not the table recognized it, so a
   * code added after this release reaches a caller instead of being discarded
   * behind an `UNKNOWN`.
   *
   * **It preserves the codes a broker can currently send, not every string.**
   * `Code` accepts `A`-`Z`, `0`-`9` and `_` and at most eight of them; KIS's
   * codes are two digits and Toss's are three uppercase letters, so both fit
   * with room to spare. A future code carrying a lowercase letter, a hyphen or
   * a dot would not be stored, and this would be empty rather than wrong --
   * which is the same outcome as the broker reporting no code at all. The
   * alternative was a `std::string`, and an allocation on every row of every
   * listing to carry a value that has never needed one is the wrong trade; the
   * alternative of throwing is worse still, because both specifications require
   * a client to tolerate an unknown code and one odd row must not fail a whole
   * listing.
   *
   * Empty therefore means one of three things -- the broker reported no code, it
   * reported one outside that alphabet, or it reported a valid uppercase code
   * longer than eight characters, which `Code<8>::try_make` also rejects -- and
   * the normalized fields say `UNKNOWN` in all three.
   */
  Code<8> shape_code_;
  /** Where the order stands. `UNKNOWN` where the broker publishes no status column. */
  OrderStatus status_{OrderStatus::UNKNOWN};
  /** The limit price, where the order has one. */
  std::optional<Decimal64> price_;
  /**
   * The trigger price of a stop order, where the broker reports one.
   *
   * Set whenever the broker's value is a positive number, **whatever basis the row reads as**:
   * both brokers report zero on an ordinary order, so a positive trigger on a row that is not
   * `STOP_LIMIT` is the broker contradicting itself, and discarding it would hide that. Recorded,
   * it makes a replacement of that order refuse (a trigger on a basis that takes none) while its
   * cancellation still goes through.
   *
   * **Empty on a stop row means the broker did not say**, not that the order has no trigger: a
   * replacement then has none to keep and is refused, and a cancellation is unaffected.
   */
  std::optional<Decimal64> trigger_price_;
  /** The quantity ordered. What remains is this less `execution_.filled_quantity_`. */
  Decimal64 quantity_;
  /**
   * The cash amount an amount-based order committed, where the broker reports
   * one.
   *
   * An amount-based order states money and lets the fill decide the quantity,
   * which is the reverse of every other order here. Present only where a broker
   * offers one -- Toss, for a US market order -- and `std::nullopt` otherwise.
   * This library does not place one yet
   * (`plan/todo/us-and-conditional-orders.md`); it reports one placed through
   * another channel, because a snapshot that dropped the amount would state a
   * quantity and be unable to say what the order actually committed.
   */
  std::optional<Decimal64> order_amount_;
  /** The currency the prices and amounts on this row are in. */
  Currency currency_;
  /** When the broker recorded the order, where it timestamps one. */
  std::optional<std::chrono::system_clock::time_point> ordered_at_;
  /** When the order was cancelled, where the broker reports it. */
  std::optional<std::chrono::system_clock::time_point> canceled_at_;
  /**
   * What has filled, and what it cost. Always present as a value.
   *
   * An unfilled order has `filled_quantity_` of zero; its monetary members may be absent *or* a
   * reported zero, depending on the broker. Read `OrderExecution`'s own comment before testing any
   * of them, and test a fill with `filled_quantity_`.
   */
  OrderExecution execution_;
};

/**
 * One page of normalized orders.
 *
 * `next_cursor_` is empty when the broker reported no further page. Callers
 * pass it back unchanged; its content is broker-specific and opaque.
 */
struct OrderPage {
  /** This page's rows, in the order the broker returned them. */
  std::vector<OrderSnapshot> orders_;
  /** Pass back unchanged to continue. Empty means the broker reported no further page. */
  std::optional<std::string> next_cursor_;
  /** When this page arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * Broker acknowledgement that an order instruction was accepted.
 *
 * An acknowledgement means the broker recorded the instruction, not that the
 * order executed. The resulting state must be read back through an order
 * inquiry.
 */
struct OrderAck {
  /** The order number the broker assigned. */
  std::string order_id_;
  /** The idempotency key echoed back, where the broker supports and echoes one. */
  std::optional<std::string> client_order_id_;
  /** When the acknowledgement arrived. Acceptance, not execution. */
  std::chrono::system_clock::time_point accepted_at_;
};

/**
 * Compile-time identity of the broker that issued an order reference.
 *
 * A broker declares a tag type carrying `static constexpr std::string_view name`
 * and names it in its `BrokerTraits` specialization. The tag is a template
 * parameter rather than a stored field, so the broker costs nothing at run time
 * and a reference from one broker is a different *type* from another's: handing a
 * reference to the wrong session fails to compile instead of failing a check.
 *
 * Both of a broker's clients — asynchronous and blocking — share one tag, so a
 * reference obtained through either is usable with the other.
 */

/**
 * Opaque order identity issued by broker `Broker`.
 *
 * Two parts are stored because addressing an order is not uniformly reducible to
 * one string: KIS needs a branch code together with an order number, while Toss
 * needs one identifier. `primary_` carries the order number or identifier and
 * `secondary_` carries the additional part when a broker has one.
 *
 * The parts are opaque. Strategy code stores, compares, and hashes a reference
 * but never parses it, so a broker may change its internal composition without
 * breaking a caller.
 */
template <typename Broker>
struct OrderRef {
  /** The order number or identifier. Opaque: stored, compared and hashed, never parsed. */
  std::string primary_;
  /** The additional part a broker needs, such as KIS's branch code. Empty where it needs none. */
  std::string secondary_;

  /** Returns the issuing broker's normalized name, known at compile time. */
  static constexpr std::string_view broker() noexcept { return Broker::name; }

  /** Returns a stable human-readable form for logs and order attribution. */
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

  /** Compares two references of the same broker. */
  friend bool operator==(const OrderRef& lhs, const OrderRef& rhs) {
    return lhs.primary_ == rhs.primary_ && lhs.secondary_ == rhs.secondary_;
  }

  /** Compares two references of the same broker for inequality. */
  friend bool operator!=(const OrderRef& lhs, const OrderRef& rhs) { return !(lhs == rhs); }
};

/**
 * Broker acknowledgement of an accepted order together with its reference.
 *
 * Acceptance means the broker recorded the instruction, not that the order
 * executed. `ref_` is what a later modification or cancellation must address.
 */
template <typename Broker>
struct PlacedOrder {
  /** What the broker said when it accepted the instruction. */
  OrderAck ack_;
  /** The reference a later modification or cancellation must address. */
  OrderRef<Broker> ref_;
};

/**
 * The price shape of an order: its type and, for a limit order, its price.
 *
 * A shape is carried explicitly because some brokers require the shape to be
 * restated on a modification or a cancellation. KIS's `order-rvsecncl` re-sends
 * `ORD_DVSN` and `ORD_UNPR` for both, and rejects a limit shape with no price.
 * Toss requires a shape for a modification and not for a cancellation. Stating
 * the shape keeps the port from inventing a value the broker will act on.
 *
 * All three normalized facts are carried, not just the basis. KIS composes one
 * `ORD_DVSN` from the three, so a shape that named only the basis would make an
 * amendment re-send a *different* order: an immediate-or-cancel order amended
 * to a new price would arrive as a plain day order at that price, which is a
 * silent downgrade of a condition the caller stated (Decision 6).
 *
 * The price basis and the price come first, and the condition and session
 * after them, because C++17 has no designated initializers and
 * `OrderShape{OrderType::LIMIT, price}` is both the common case and what every
 * existing caller writes.
 *
 * The two-argument form compiles cleanly under `-Wmissing-field-initializers`
 * because `OrderShape` has a constructor; as a plain aggregate it did not, and
 * the safe form would have been one a consumer with that flag could not write.
 * That is what the constructor is for.
 */
struct OrderShape {
  /**
   * Constructs a shape, stating only as much as the caller means.
   *
   * **A constructor rather than plain aggregate initialization**, so that
   * `OrderShape{OrderType::LIMIT, price}` -- the form this design depends on
   * being the natural one -- is a call and not a brace with two members left
   * out. Left as an aggregate it tripped `-Wmissing-field-initializers`, which
   * this library's own warnings configuration promotes to an error, so the safe
   * form would have been the one a careful consumer could not write.
   */
  OrderShape() = default;
  /** @copydoc OrderShape::OrderShape() */
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

  /** The price basis this shape states. */
  OrderType type_{OrderType::LIMIT};
  /** The price, for a basis that takes one. */
  std::optional<Decimal64> price_;
  /**
   * The trigger price, for a basis `carries_trigger_price` names.
   *
   * **On an amendment, unset means "leave it as it is"**, like the condition and session below,
   * so `replace_with(order, OrderShape{STOP_LIMIT, limit})` moves a stop's limit and keeps its
   * trigger. `amended_trigger_price` is the one place that resolves it, and it resolves from
   * `AmendInstruction::resting_` only when **both** shapes are `STOP_LIMIT`: a change of basis
   * into a stop states its own trigger or is refused, and an unknown resting shape is never
   * guessed at. A set value is a deliberate trigger, and on a basis that takes none it is refused
   * rather than dropped.
   *
   * On an `OrderRequest`, which has no resting order, there is nothing to resolve from, and a
   * stop-limit request without one is refused.
   */
  std::optional<Decimal64> trigger_price_;
  /**
   * The execution condition and the session, **unset when the caller did not
   * state one**.
   *
   * Optional rather than defaulted, and that is the whole point. These were
   * plain values with `DAY` and `REGULAR` defaults, so `OrderShape{LIMIT,
   * price}` -- the form every caller writes to reprice an order -- silently
   * *stated* a day order in the regular session. Against a resting
   * closing-auction order that is a request to move it, and at Toss it is a
   * request the modify endpoint cannot carry, so success was reported for a move
   * that did not happen. Defaulting them the other way instead, from the resting
   * order, only swapped that for silently discarding a deliberate change.
   *
   * Unset means "leave it as it is", and an adapter resolves it from
   * `AmendInstruction::resting_`. A set value is a deliberate change, honoured
   * where the broker can make one and refused where it cannot. Neither is a
   * guess.
   */
  std::optional<TimeInForce> time_in_force_;
  /** @copydoc OrderShape::time_in_force_ */
  std::optional<OrderSession> session_;
};

/**
 * Broker-neutral instruction for a modification or a cancellation.
 *
 * `all_remaining_` states whether the whole remaining quantity is affected or
 * only `quantity_`. It is never inferred, because guessing changes how much of
 * a live order is withdrawn.
 *
 * `quantity_` is optional in this shared value because brokers disagree about
 * needing it: KIS validates a positive whole quantity even for a full
 * cancellation, and Toss cancels in full with no quantity at all. A broker that
 * requires one rejects an instruction that omits it rather than substituting a
 * value. Building the instruction with `cancel_all()`, `cancel_quantity()`, or
 * `replace_with()` fills whatever the bound broker needs from a `WorkingOrder`.
 */
struct AmendInstruction {
  /** The shape the order should have after the amendment. */
  OrderShape shape_;
  /**
   * The venue the order lives at, where the broker's amendment endpoint takes
   * one.
   *
   * KIS's `order-rvsecncl` publishes a different set of order codes per venue,
   * exactly as its placement endpoint does, so an amendment needs the venue for
   * the same reason a placement does. `UNSPECIFIED` omits the field and takes
   * the broker's documented default; the helpers below fill it from the working
   * order rather than leaving a caller to remember.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /** How much is affected. Required by a broker that validates one; never substituted. */
  std::optional<Decimal64> quantity_;
  /** Whether the whole remaining quantity is affected rather than `quantity_`. Never inferred. */
  bool all_remaining_{false};
  /** Acknowledges the broker's high-value confirmation, where the amendment endpoint requires one. */
  bool confirm_high_value_{false};
  /**
   * The shape the order currently has, or nothing when the caller could not say.
   *
   * An adapter needs it to tell a *change* from a supported *value*. Toss can
   * place an order in the closing auction and cannot move one -- its
   * `OrderModifyRequest` has no validity field at all -- so checking that a
   * requested session is one Toss supports proves nothing: the question is
   * whether it differs from what the order already is.
   *
   * **Optional, so "unknown" is representable.** It was a plain `OrderShape`,
   * whose defaults are `DAY`/`REGULAR`, so a hand-built instruction naming
   * `DAY`/`REGULAR` and leaving this alone compared equal and passed -- and if
   * the referenced order was really `CLS`, the POST carried no session field and
   * success again reported a move that did not occur. A comment saying a
   * hand-built instruction "states no original" was not represented in the
   * type; now it is.
   *
   * `replace_with`, `reprice`, `cancel_all` and `cancel_quantity` fill it from
   * the working order. An adapter that cannot verify a change it cannot
   * transmit refuses before I/O rather than assuming it is a no-op.
   */
  std::optional<OrderShape> resting_;
};

/**
 * Returns the trigger price an amendment leaves the order with.
 *
 * The stated one where `instruction.shape_` sets it. Otherwise the resting order's, **but only
 * when the instruction keeps a `STOP_LIMIT` a `STOP_LIMIT`** -- the case in which "unset" can only
 * mean "unchanged". Anything else yields nothing: a basis change into a stop has no trigger to
 * inherit, a basis change out of one must not carry it along, and a resting shape the caller could
 * not state is not a trigger this library may assume. Every adapter that transmits a trigger on an
 * amendment reads it through here, so the rule is one rule.
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
 * One order that is still open to modification or cancellation.
 *
 * `ref_` is the reference a modification or cancellation must address. A broker
 * may also report which earlier order this one superseded — KIS's `orgn_odno` —
 * but that is lineage, not an address: amending it would address an order that is
 * no longer live. Lineage stays reachable through the concrete client and is not
 * normalized here.
 *
 * `amendable_quantity_` is optional because a broker reports it only where it
 * publishes one. An empty value means the broker did not state a quantity, not
 * that the quantity is zero.
 */
template <typename Broker>
struct WorkingOrder {
  /** The order's current normalized state. */
  OrderSnapshot order_;
  /** The reference a modification or cancellation must address. */
  OrderRef<Broker> ref_;
  /** How much the broker says may still be amended. Empty means it stated none, not zero. */
  std::optional<Decimal64> amendable_quantity_;
  /**
   * Whether this row is a runner's own reservation rather than something the broker reported.
   *
   * False on everything a listing produced, which is every row an adapter builds. A runner sets it
   * on the rows it adds for orders it has just sent, so that a limit sizing against "what is
   * outstanding" counts them while anything **inferring what the broker does** ignores them --
   * `strategy::SellableConventionProbe` reads a broker's behaviour from these rows, and a
   * reservation is this library's own arithmetic, not evidence about the broker.
   *
   * A poll replaces the listing wholesale, so a reservation never outlives the picture it was added
   * to.
   *
   * **It is not a "do not touch" marker.** A reserved row carries the reference the broker issued
   * and the order's own shape, so it can be cancelled like any other -- an order this runner sent a
   * moment ago is as real as one a listing reported, and refusing to withdraw it would be the
   * defect, not the safeguard. The flag says only where the row came from.
   */
  bool reserved_{false};
};

/** Point-in-time set of orders open to modification or cancellation. */
template <typename Broker>
struct WorkingOrders {
  /** Every order open to amendment at the moment of the listing. */
  std::vector<WorkingOrder<Broker>> orders_;
  /** When the listing arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * Filter for one order-history inquiry.
 *
 * A broker that cannot express a stated filter rejects the query with
 * `ErrorCategory::INVALID_ARGUMENT` before any network I/O. A filter is never
 * dropped silently, because a listing that ignored it would answer a different
 * question from the one asked.
 */
struct OrderHistoryQuery {
  /** The first date to include. Required by a broker whose history inquiry takes a range. */
  std::optional<Date> from_;
  /** The last date to include. */
  std::optional<Date> to_;
  /** Restrict to one symbol, where the broker can express that filter. */
  std::optional<std::string> symbol_;
  /** A cursor from a previous page's `next_cursor_`, passed back unchanged. */
  std::optional<std::string> cursor_;
  /** The maximum number of rows to return, where the broker honours one. */
  std::optional<int> limit_;
};

/** Returns the shape a working order currently has. */
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
 * Returns an instruction cancelling the whole remaining quantity of `order`.
 *
 * The order's own shape and reported amendable quantity are carried through, so
 * a broker that requires either receives it without the caller restating it.
 */
template <typename Broker>
inline AmendInstruction cancel_all(const WorkingOrder<Broker>& order) {
  AmendInstruction instruction;
  // A cancellation states no shape of its own, so every part of the order's
  // shape is copied. Copying the basis alone would have a broker re-read the
  // order as a day order in the regular session, which is a different order
  // from the one being withdrawn.
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

/** Returns an instruction cancelling exactly `quantity` of `order`. */
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
 * Returns an instruction moving `order` to `shape`.
 *
 * Omitting `quantity` amends the whole remaining quantity; supplying one amends
 * exactly that quantity.
 *
 * **`shape` states what to change. What it leaves unset stays as it is.**
 *
 * `OrderShape`'s condition and session are `std::optional`, so
 * `OrderShape{OrderType::LIMIT, price}` states a new price and says nothing
 * about either -- which is what a caller writing that form means. An adapter
 * resolves the unset parts from `resting_`, recorded here from `order`.
 *
 * Setting one is a deliberate change. KIS re-sends `ORD_DVSN` on an amendment
 * and can make one; Toss's modify request has no validity field, so it refuses
 * one rather than reporting a success that did not happen. Both are decided by
 * the adapter, because which of them is possible is a fact about the broker.
 *
 * This has been wrong twice in opposite directions. Taking the shape entirely as
 * written made the common form silently request a move; copying the condition and
 * session from the resting order silently discarded a deliberate change. Neither
 * was a guess the caller could see. An unset optional is.
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
  // The caller's shape is left as written. An earlier version overwrote the
  // condition and session from the resting order, which stopped the *common*
  // form lying but silently discarded an explicit change -- including a KIS
  // caller's, whose old code still compiled. Recording the original instead
  // lets each adapter honour a change it can make and refuse one it cannot.
  instruction.venue_ = order.order_.venue_;
  instruction.all_remaining_ = !quantity.has_value();
  instruction.quantity_ = quantity ? quantity : order.amendable_quantity_;
  return instruction;
}

/**
 * Returns an instruction moving `order` to a new price and changing nothing else.
 *
 * A name for the common amendment. `replace_with(order, OrderShape{basis, price})`
 * says the same thing -- `OrderShape`'s condition and session are optional, so
 * that form already leaves both alone -- and this states the intent outright
 * rather than by omission.
 *
 * It was once the only safe way to reprice, because the condition and session
 * were defaulted values rather than optionals and the plain form silently
 * requested a move to the regular session. Making them optional fixed that at
 * the type; this stayed because "reprice" is what the caller is doing.
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
 * Formats an instant as an ISO 8601 timestamp in UTC.
 *
 * Emitted with a `Z` designator rather than a numeric offset, so the value cannot
 * be read against the wrong zone. Brokers that accept an ISO 8601 bound accept
 * this form; a broker that wants a local calendar date converts from it in its own
 * adapter, where the zone it means is known.
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

/** Parses an ISO 8601 timestamp with a UTC designator or numeric offset. */
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
 * Hashes an opaque order reference so callers can key an order book on it.
 *
 * The broker is part of the type rather than the value, so only the two parts
 * are hashed; two brokers' references cannot collide because they cannot share a
 * container.
 */
template <typename Broker>
struct hash<algotradelib::OrderRef<Broker>> {
  /** Combines both reference parts. */
  std::size_t operator()(const algotradelib::OrderRef<Broker>& ref) const noexcept {
    std::size_t seed = hash<std::string>{}(ref.primary_);
    seed ^= hash<std::string>{}(ref.secondary_) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

} // namespace std
