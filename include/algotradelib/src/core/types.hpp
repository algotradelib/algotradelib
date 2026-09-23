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

/** Broker-neutral identity for a financial instrument. */
struct InstrumentId {
  Country country_{Country::KR};
  Venue venue_{Venue::COMBINED};
  std::string symbol_;

  friend bool operator==(const InstrumentId& lhs, const InstrumentId& rhs) {
    return lhs.country_ == rhs.country_ && lhs.venue_ == rhs.venue_ && lhs.symbol_ == rhs.symbol_;
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

  friend bool operator==(const Code& lhs, const Code& rhs) { return lhs.view() == rhs.view(); }

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
  ErrorCategory category_{ErrorCategory::PROTOCOL};
  int http_status_{0};
  std::string broker_code_;
  std::string request_id_;
  std::string message_;
  std::optional<std::chrono::seconds> retry_after_;
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
  std::optional<Decimal64> bid_price_;
  std::optional<Decimal64> bid_quantity_;
  std::optional<Decimal64> ask_price_;
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
  InstrumentId instrument_;
  Decimal64 last_price_;
  Currency currency_;
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
  std::chrono::system_clock::time_point received_at_;
  std::optional<TopOfBook> top_of_book_;
  std::optional<Decimal64> volume_;
};

/** Exact monetary amount paired with its currency. */
struct Money {
  Decimal64 amount_;
  Currency currency_;

  friend bool operator==(const Money& lhs, const Money& rhs) {
    return lhs.amount_ == rhs.amount_ && lhs.currency_ == rhs.currency_;
  }
};

/** One broker-neutral position in an account holdings snapshot. */
struct HoldingPosition {
  InstrumentId instrument_;
  std::string name_;
  Decimal64 quantity_;
  std::optional<Decimal64> sellable_quantity_;
  Decimal64 average_purchase_price_;
  Decimal64 last_price_;
  Money purchase_value_;
  Money market_value_;
  Money unrealized_profit_loss_;
  Decimal64 unrealized_return_;
};

/** Gross holdings totals for one currency without implicit conversion. */
struct HoldingsTotals {
  Currency currency_;
  Decimal64 purchase_value_;
  Decimal64 market_value_;
  Decimal64 unrealized_profit_loss_;
};

/** Point-in-time normalized positions and per-currency totals. */
struct HoldingsSnapshot {
  std::vector<HoldingPosition> positions_;
  std::vector<HoldingsTotals> totals_;
  std::chrono::system_clock::time_point received_at_;
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
  Money cash_;
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
  int year_{0};
  unsigned month_{0};
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

/**
 * One commission rate a broker charges, and the dates it applies on.
 *
 * A rate is a fraction of traded value rather than a percentage, matching
 * `backtest::CommissionModel::rate_`: `0.00015` is the order of magnitude a Korean online
 * brokerage charges.
 *
 * Both bounds are optional and absent means open-ended in that direction, which is how Toss
 * publishes them — a `null` end date is a rate with no scheduled change. Absent is deliberately not
 * "today": a schedule that started before the account did is a normal thing to be told, and
 * inventing a start for it would make a date comparison say something the broker did not.
 *
 * There is no per-order floor and no transaction tax here, because neither broker publishes one.
 */
struct CommissionRate {
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
  Decimal64 rate_;
  std::optional<Date> from_;
  std::optional<Date> until_;

  /**
   * Returns whether this row names a market this library normalizes.
   *
   * An unnormalized row carries its market in `other_`, so an empty `other_` means `country_` is
   * the answer. A row naming no market at all must therefore never be built: it would read as a
   * normalized row for `country_`'s default, which is to say a rate for Korea the broker never
   * published. Parsers skip such a row and count it in `dropped_`.
   */
  bool normalized() const noexcept { return other_.empty(); }

  /** Returns whether this rate is the one in force on `date`. */
  bool applies_on(const Date& date) const {
    if (from_ && date < *from_) {
      return false;
    }
    if (until_ && *until_ < date) {
      return false;
    }
    return true;
  }
};

/**
 * The commission rates a broker published for one account.
 *
 * Only one broker publishes any, so a caller reaches this through a capability gate rather than
 * assuming it. An empty schedule from a broker that *does* publish them is a real answer — the
 * account has no stated rates — and is not the same as the broker having no such inquiry.
 */
struct CommissionSchedule {
  std::vector<CommissionRate> rates_;
  std::chrono::system_clock::time_point received_at_{};

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
   * Returns the rate in force for `country` on `date`, or nothing when none is.
   *
   * The first matching row wins. A broker publishing two overlapping rows for one country and date
   * is describing something this library cannot resolve, and picking the larger or the smaller
   * would be inventing a policy the broker did not state.
   */
  std::optional<CommissionRate> rate_for(Country country, const Date& date) const {
    for (const auto& rate : rates_) {
      // A row for a market this library does not normalize can never be the answer to a question
      // asked in terms of `Country`, and it is kept so a caller can see that the broker sent it.
      if (rate.normalized() && rate.country_ == country && rate.applies_on(date)) {
        return rate;
      }
    }
    return std::nullopt;
  }
};

/** Direction of an order. */
enum class OrderSide {
  BUY,
  SELL,
};

/**
 * Normalized price instruction for an order.
 *
 * Conditional, immediate-or-cancel, fill-or-kill, stop, mid-price, and
 * after-market variants exist at KIS but are not normalized by this library
 * yet. On a request path adapters reject them rather than approximating them
 * with a nearby value. On a read path they surface as `UNKNOWN`, because an
 * order placed through another channel must still be reportable.
 */
enum class OrderType {
  UNKNOWN,
  LIMIT,
  MARKET,
};

/**
 * Validity condition attached to an order.
 *
 * `UNKNOWN` appears only on a read path, for a validity condition this
 * library does not normalize. It is never accepted on a request.
 */
enum class TimeInForce {
  UNKNOWN,
  DAY,
  AT_THE_CLOSE,
  AT_THE_OPEN,
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
  InstrumentId instrument_;
  OrderSide side_{OrderSide::BUY};
  OrderType type_{OrderType::LIMIT};
  TimeInForce time_in_force_{TimeInForce::DAY};
  // `UNKNOWN` in `type_` or `time_in_force_` is rejected before network I/O.
  Decimal64 quantity_;
  std::optional<Decimal64> price_;
  std::optional<std::string> client_order_id_;
  bool confirm_high_value_{false};
};

/**
 * Realized execution totals for one order.
 *
 * Monetary members are optional because a broker reports them only once the
 * corresponding amount exists; an unfilled order has a filled quantity of zero
 * and no amounts at all.
 */
struct OrderExecution {
  Decimal64 filled_quantity_;
  std::optional<Decimal64> average_filled_price_;
  std::optional<Money> filled_amount_;
  std::optional<Money> commission_;
  std::optional<Money> tax_;
  std::optional<std::chrono::system_clock::time_point> filled_at_;
  std::optional<Date> settlement_date_;
};

/** Point-in-time normalized state of one order. */
struct OrderSnapshot {
  std::string order_id_;
  InstrumentId instrument_;
  std::string name_;
  OrderSide side_{OrderSide::BUY};
  OrderType type_{OrderType::UNKNOWN};
  TimeInForce time_in_force_{TimeInForce::UNKNOWN};
  OrderStatus status_{OrderStatus::UNKNOWN};
  std::optional<Decimal64> price_;
  Decimal64 quantity_;
  Currency currency_;
  std::optional<std::chrono::system_clock::time_point> ordered_at_;
  std::optional<std::chrono::system_clock::time_point> canceled_at_;
  OrderExecution execution_;
};

/**
 * One page of normalized orders.
 *
 * `next_cursor_` is empty when the broker reported no further page. Callers
 * pass it back unchanged; its content is broker-specific and opaque.
 */
struct OrderPage {
  std::vector<OrderSnapshot> orders_;
  std::optional<std::string> next_cursor_;
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
  std::string order_id_;
  std::optional<std::string> client_order_id_;
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
  std::string primary_;
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
  OrderAck ack_;
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
 */
struct OrderShape {
  OrderType type_{OrderType::LIMIT};
  std::optional<Decimal64> price_;
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
  OrderShape shape_;
  std::optional<Decimal64> quantity_;
  bool all_remaining_{false};
  bool confirm_high_value_{false};
};

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
  OrderSnapshot order_;
  OrderRef<Broker> ref_;
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
  std::vector<WorkingOrder<Broker>> orders_;
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
  std::optional<Date> from_;
  std::optional<Date> to_;
  std::optional<std::string> symbol_;
  std::optional<std::string> cursor_;
  std::optional<int> limit_;
};

/**
 * Returns an instruction cancelling the whole remaining quantity of `order`.
 *
 * The order's own shape and reported amendable quantity are carried through, so
 * a broker that requires either receives it without the caller restating it.
 */
template <typename Broker>
inline AmendInstruction cancel_all(const WorkingOrder<Broker>& order) {
  AmendInstruction instruction;
  instruction.shape_.type_ = order.order_.type_;
  instruction.shape_.price_ = order.order_.price_;
  instruction.quantity_ = order.amendable_quantity_;
  instruction.all_remaining_ = true;
  return instruction;
}

/** Returns an instruction cancelling exactly `quantity` of `order`. */
template <typename Broker>
inline AmendInstruction cancel_quantity(const WorkingOrder<Broker>& order, Decimal64 quantity) {
  AmendInstruction instruction;
  instruction.shape_.type_ = order.order_.type_;
  instruction.shape_.price_ = order.order_.price_;
  instruction.quantity_ = quantity;
  instruction.all_remaining_ = false;
  return instruction;
}

/**
 * Returns an instruction moving `order` to `shape`.
 *
 * Omitting `quantity` amends the whole remaining quantity; supplying one amends
 * exactly that quantity.
 */
template <typename Broker>
inline AmendInstruction replace_with(
  const WorkingOrder<Broker>& order,
  OrderShape shape,
  std::optional<Decimal64> quantity = std::nullopt
) {
  AmendInstruction instruction;
  instruction.shape_ = std::move(shape);
  instruction.all_remaining_ = !quantity.has_value();
  instruction.quantity_ = quantity ? quantity : order.amendable_quantity_;
  return instruction;
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
