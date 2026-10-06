#pragma once

#include "./market_data.hpp"
#include "./types.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace algotradelib {

/**
 * @brief The market segment an instrument is listed on, such as KOSPI or NASDAQ.
 *
 * A code the library does not recognize is kept in `other_` when `Code<12>` can hold it.
 */
struct MarketSegment {
  /** @brief The segments the library recognizes. */
  enum class Known {
    UNKNOWN,
    KOSPI,
    KOSDAQ,
    NYSE,
    NASDAQ,
    AMEX,
  };

  /** @brief The segment, or `UNKNOWN` if the broker's code was not recognized. */
  Known known_{Known::UNKNOWN};
  /**
   * @brief The broker's code, when `known_` is `UNKNOWN` and the code fits `Code<12>`.
   *
   * Empty beside `UNKNOWN` means the broker named no segment, or named one too long or with
   * characters `Code` cannot hold.
   */
  Code<12> other_;

  /**
   * @brief Returns the segment's name for display.
   * @return The normalized name, the preserved code, or `"UNKNOWN"`.
   */
  std::string str() const {
    switch (known_) {
      case Known::KOSPI:
        return "KOSPI";
      case Known::KOSDAQ:
        return "KOSDAQ";
      case Known::NYSE:
        return "NYSE";
      case Known::NASDAQ:
        return "NASDAQ";
      case Known::AMEX:
        return "AMEX";
      case Known::UNKNOWN:
        break;
    }
    return other_.empty() ? std::string("UNKNOWN") : other_.str();
  }

  /**
   * @brief Compares the normalized segment and the preserved code.
   * @param lhs The first segment.
   * @param rhs The second segment.
   * @return `true` if both are equal.
   */
  friend bool operator==(const MarketSegment& lhs, const MarketSegment& rhs) {
    return lhs.known_ == rhs.known_ && lhs.other_ == rhs.other_;
  }

  /**
   * @brief Compares two segments for inequality.
   * @param lhs The first segment.
   * @param rhs The second segment.
   * @return `true` if they differ.
   */
  friend bool operator!=(const MarketSegment& lhs, const MarketSegment& rhs) {
    return !(lhs == rhs);
  }
};

/**
 * @brief Maps a broker's segment code to a `MarketSegment`.
 * @param code The code, such as `"KOSDAQ"`.
 * @return The recognized segment, or `UNKNOWN` with the code kept in `other_` if it fits
 *   (otherwise both are empty).
 */
inline MarketSegment market_segment(std::string_view code) {
  MarketSegment segment;
  if (code == "KOSPI") {
    segment.known_ = MarketSegment::Known::KOSPI;
  } else if (code == "KOSDAQ") {
    segment.known_ = MarketSegment::Known::KOSDAQ;
  } else if (code == "NYSE") {
    segment.known_ = MarketSegment::Known::NYSE;
  } else if (code == "NASDAQ") {
    segment.known_ = MarketSegment::Known::NASDAQ;
  } else if (code == "AMEX") {
    segment.known_ = MarketSegment::Known::AMEX;
  } else if (const auto preserved = Code<12>::try_make(code)) {
    segment.other_ = *preserved;
  }
  return segment;
}

/**
 * @brief What kind of security an instrument is.
 *
 * `UNKNOWN` appears only on reads, for a type the library does not normalize.
 */
enum class SecurityType {
  UNKNOWN,
  STOCK,
  FOREIGN_STOCK,
  DEPOSITARY_RECEIPT,
  INFRASTRUCTURE_FUND,
  REIT,
  ETF,
  FOREIGN_ETF,
  ETN,
  STOCK_WARRANT,
};

/** @brief Whether an instrument is listed, awaiting listing, or delisted. */
enum class InstrumentStatus {
  UNKNOWN,
  SCHEDULED,
  ACTIVE,
  DELISTED,
};

/**
 * @brief One band of a price grid: the tick size from a price upward, until the next band.
 */
struct TickBand {
  /** @brief The price the band starts at, inclusive. */
  Decimal64 from_;
  /** @brief The tick size within the band. */
  Decimal64 tick_;
};

/**
 * @brief The price grid (tick sizes) an instrument's order prices must sit on.
 *
 * No broker publishes tick sizes, so the library maintains these tables (`source_` names the
 * table and its revision); replace a rule if the exchange revises its grid. A rule of kind
 * `UNKNOWN` refuses to check or round a price. A custom table needs positive ticks and bands
 * sorted by `from_`.
 */
struct PriceRule {
  /** @brief Whether the rule states a grid, imposes none, or does not know it. */
  enum class Kind {
    /** @brief `bands_` describe the market's grid. */
    STATED,
    /** @brief The market imposes no grid (free decimals). */
    UNCONSTRAINED,
    /**
     * @brief The library has no grid for this instrument (for example a KRX ETF or ETN) and will
     *   not guess one.
     */
    UNKNOWN,
  };

  /** @brief The kind of rule; `UNKNOWN` by default, so an unset rule refuses to round. */
  Kind kind_{Kind::UNKNOWN};
  /** @brief The bands, ascending by `from_`; empty unless `kind_` is `STATED`. */
  std::vector<TickBand> bands_;
  /** @brief Where the table came from and its revision, or why there is none. */
  std::string source_;

  /**
   * @brief Returns the tick size at a price.
   * @param price The price.
   * @return The tick of the band containing `price`, or `std::nullopt` if there are no bands or
   *   `price` is below the first.
   */
  std::optional<Decimal64> tick_at(const Decimal64& price) const {
    std::optional<Decimal64> tick;
    for (const auto& band : bands_) {
      if (decimal_less(price, band.from_)) {
        break;
      }
      tick = band.tick_;
    }
    return tick;
  }

  /**
   * @brief Tests whether a price is on the grid.
   * @param price The price.
   * @return `std::nullopt` if the grid is unknown (kind `UNKNOWN`); `true` if `price` is on its
   *   band's tick, or there is no band at `price` (an unconstrained rule); otherwise `false`, which
   *   also covers a remainder that does not fit a `Decimal64`.
   */
  std::optional<bool> on_tick(const Decimal64& price) const {
    if (kind_ == Kind::UNKNOWN) {
      return std::nullopt;
    }
    const auto tick = tick_at(price);
    if (!tick) {
      return true;
    }
    const auto rest = remainder(price, *tick);
    return rest.has_value() && rest->coefficient() == 0;
  }

  /**
   * @brief Rounds a price to its band's tick in the order's favour: a buy rounds down, a sell up.
   *
   * The tick is the one at `price`; a rounded price that crosses into the next band is not
   * re-rounded. That is correct for the KRX table; with a custom table whose band boundaries are not
   * multiples of both ticks, check the result with `on_tick`.
   *
   * @param price The price.
   * @param side The side of the order.
   * @return The rounded price (unchanged if already on the grid or there is no band at `price`),
   *   or `std::nullopt` if the grid is unknown or the arithmetic does not fit.
   */
  std::optional<Decimal64> round_to_tick(const Decimal64& price, OrderSide side) const {
    if (kind_ == Kind::UNKNOWN) {
      return std::nullopt;
    }
    const auto tick = tick_at(price);
    if (!tick) {
      return price;
    }
    const auto rest = remainder(price, *tick);
    if (!rest) {
      return std::nullopt;
    }
    if (rest->coefficient() == 0) {
      return decimal_normalized(price);
    }
    const auto floor_price = decimal_subtract(price, *rest);
    if (!floor_price) {
      return std::nullopt;
    }
    if (side == OrderSide::BUY) {
      return floor_price;
    }
    return decimal_add(*floor_price, *tick);
  }

  /**
   * @brief Returns the KRX cash-equity tick table (January 2023 revision).
   * @return The rule.
   */
  static PriceRule krx_equity() {
    PriceRule rule;
    rule.kind_ = Kind::STATED;
    rule.source_ = "KRX cash equity tick table, 2023-01 revision (library-maintained)";
    rule.bands_ = {
      TickBand{Decimal64::parse("0"), Decimal64::parse("1")},
      TickBand{Decimal64::parse("2000"), Decimal64::parse("5")},
      TickBand{Decimal64::parse("5000"), Decimal64::parse("10")},
      TickBand{Decimal64::parse("20000"), Decimal64::parse("50")},
      TickBand{Decimal64::parse("50000"), Decimal64::parse("100")},
      TickBand{Decimal64::parse("200000"), Decimal64::parse("500")},
      TickBand{Decimal64::parse("500000"), Decimal64::parse("1000")},
    };
    return rule;
  }

  /**
   * @brief Returns a rule with no grid, for a market quoted in free decimals.
   * @return The rule, of kind `UNCONSTRAINED`.
   */
  static PriceRule unconstrained() {
    PriceRule rule;
    rule.kind_ = Kind::UNCONSTRAINED;
    rule.source_ = "no exchange price grid";
    return rule;
  }

  /**
   * @brief Returns a rule that knows no grid and refuses to round.
   * @param reason Why there is no grid; stored in `source_`.
   * @return The rule, of kind `UNKNOWN`.
   */
  static PriceRule unknown(std::string reason) {
    PriceRule rule;
    rule.kind_ = Kind::UNKNOWN;
    rule.source_ = std::move(reason);
    return rule;
  }

private:
  // `price` modulo `tick`, exactly.
  static std::optional<Decimal64> remainder(const Decimal64& price, const Decimal64& tick) {
    std::int64_t left = 0;
    std::int64_t right = 0;
    std::uint8_t scale = 0;
    if (!decimal_detail::align(price, tick, left, right, scale) || right == 0) {
      return std::nullopt;
    }
    auto rest = left % right;
    if (rest < 0) {
      rest += right < 0 ? -right : right;
    }
    return Decimal64(rest, scale);
  }
};

/**
 * @brief Returns the library's price grid for a kind of security in a country.
 *
 * KRX stocks, foreign stocks, depositary receipts, infrastructure funds and REITs get the equity
 * table; ETFs, ETNs, warrants and unknown types get an `UNKNOWN` rule; other countries are
 * unconstrained.
 *
 * @param country The listing country.
 * @param type The security type.
 * @return The rule.
 */
inline PriceRule price_rule_for(Country country, SecurityType type) {
  if (country != Country::KR) {
    return PriceRule::unconstrained();
  }
  switch (type) {
    case SecurityType::STOCK:
    case SecurityType::FOREIGN_STOCK:
    case SecurityType::DEPOSITARY_RECEIPT:
    case SecurityType::INFRASTRUCTURE_FUND:
    case SecurityType::REIT:
      return PriceRule::krx_equity();
    case SecurityType::ETF:
    case SecurityType::FOREIGN_ETF:
    case SecurityType::ETN:
      return PriceRule::unknown(
        "KRX prices funds and notes on a ladder this library does not carry"
      );
    case SecurityType::STOCK_WARRANT:
    case SecurityType::UNKNOWN:
      break;
  }
  return PriceRule::unknown("no price grid is known for this security type");
}

/**
 * @brief A date a broker stated, did not state, or stated in a form that could not be read.
 *
 * Name the state before reading the date:
 *
 * @code
 * switch (instrument.delisted_on_.state()) {
 *   case StatedDate::State::STATED:     use(instrument.delisted_on_.date()); break;
 *   case StatedDate::State::NOT_STATED: ...; break;  // the broker said nothing
 *   case StatedDate::State::UNREADABLE: ...; break;  // the broker sent something unreadable
 * }
 * @endcode
 *
 * It does not convert to `bool` or to `std::optional<Date>`, so "no date" and "unreadable date"
 * cannot be confused.
 */
class StatedDate {
public:
  /** @brief What the broker's field said. */
  enum class State {
    /** @brief No date: absent, `null`, or the broker's empty spelling. */
    NOT_STATED,
    /** @brief A date that was read. */
    STATED,
    /** @brief Something that is not a readable calendar date. */
    UNREADABLE,
  };

  /** @brief Constructs a date the broker did not state. */
  StatedDate() = default;

  /**
   * @brief Makes a stated date.
   * @param date The date.
   * @return A value in state `STATED`.
   */
  static StatedDate stated(Date date) {
    StatedDate result;
    result.state_ = State::STATED;
    result.date_ = date;
    return result;
  }

  /**
   * @brief Makes an unreadable date.
   * @return A value in state `UNREADABLE`.
   */
  static StatedDate unreadable() {
    StatedDate result;
    result.state_ = State::UNREADABLE;
    return result;
  }

  /**
   * @brief Returns what the broker's field said.
   * @return The state.
   */
  State state() const noexcept { return state_; }

  /**
   * @brief Returns the date.
   * @return The date.
   * @throws std::logic_error unless `state()` is `STATED`.
   */
  const Date& date() const {
    if (state_ != State::STATED) {
      throw std::logic_error("StatedDate::date() read a date that was not stated");
    }
    return date_;
  }

  /**
   * @brief Compares two values: the same state, and the same date when stated.
   * @param lhs The first value.
   * @param rhs The second value.
   * @return `true` if they say the same thing.
   */
  friend bool operator==(const StatedDate& lhs, const StatedDate& rhs) {
    return lhs.state_ == rhs.state_ && (lhs.state_ != State::STATED || lhs.date_ == rhs.date_);
  }

  /**
   * @brief Compares two values for inequality.
   * @param lhs The first value.
   * @param rhs The second value.
   * @return `true` if they differ.
   */
  friend bool operator!=(const StatedDate& lhs, const StatedDate& rhs) { return !(lhs == rhs); }

private:
  State state_{State::NOT_STATED};
  Date date_{};
};

/** @brief What an instrument's listing and delisting dates say about one day. */
enum class DateExclusion {
  /** @brief A readable date rules the day out: listed after it, or delisted on or before it. */
  EXCLUDED,
  /** @brief No date rules it out and neither is unreadable. This does not mean "listed". */
  NOT_EXCLUDED,
  /** @brief No readable date rules it out, but an unreadable date might. */
  UNREADABLE,
};

/**
 * @brief Reference data for one instrument.
 *
 * Fields a broker may omit are optional or empty. `price_rule_` is the library's grid, chosen by
 * `price_rule_for` from the security type.
 */
struct Instrument {
  /** @brief The instrument. */
  InstrumentId id_;
  /** @brief The local-language name, as the broker spells it. */
  std::string name_;
  /** @brief The English name, or empty. */
  std::string english_name_;
  /** @brief The ISIN, or empty. */
  std::string isin_;
  /** @brief The market segment. */
  MarketSegment segment_;
  /** @brief The kind of security, which selects the price grid. */
  SecurityType type_{SecurityType::UNKNOWN};
  /** @brief Listed, awaiting listing, or delisted. */
  InstrumentStatus status_{InstrumentStatus::UNKNOWN};
  /** @brief The currency it trades in. */
  Currency currency_;
  /** @brief Whether this is the common share, not a preferred or derived line. */
  bool common_share_{true};
  /**
   * @brief The listing date.
   *
   * Toss sends `null` for none; KIS sends `""` (and probably `"00000000"`); Kiwoom sends `""`.
   */
  StatedDate listed_on_;
  /** @brief The delisting date; always `NOT_STATED` from Kiwoom, which publishes none. */
  StatedDate delisted_on_;
  /** @brief Shares outstanding, if published; KIS's blank or zero is empty, never zero shares. */
  std::optional<Decimal64> shares_outstanding_;
  /** @brief The order quantity increment, if the broker publishes one. */
  std::optional<Decimal64> lot_size_;
  /** @brief The price grid; kind `UNKNOWN` where the library has no table. */
  PriceRule price_rule_;

  /**
   * @brief Tests whether the listing and delisting dates rule a day out.
   *
   * @param day The day.
   * @return `EXCLUDED` if a readable date rules `day` out (listed after it, or delisted on or
   *   before it); otherwise `UNREADABLE` if either date is unreadable; otherwise `NOT_EXCLUDED`.
   * @note `NOT_EXCLUDED` does not mean the instrument trades on `day`; check `status_` too.
   */
  DateExclusion dates_exclude(Date day) const {
    using State = StatedDate::State;
    if (listed_on_.state() == State::STATED && day < listed_on_.date()) {
      return DateExclusion::EXCLUDED;
    }
    if (delisted_on_.state() == State::STATED && !(day < delisted_on_.date())) {
      return DateExclusion::EXCLUDED;
    }
    if (listed_on_.state() == State::UNREADABLE || delisted_on_.state() == State::UNREADABLE) {
      return DateExclusion::UNREADABLE;
    }
    return DateExclusion::NOT_EXCLUDED;
  }
};

} // namespace algotradelib
