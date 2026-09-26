#pragma once

#include "./market_data.hpp"
#include "./types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace algotradelib {

/**
 * Listing venue of an instrument, at market-segment granularity.
 *
 * The segments here are the ones a supported broker names. `other_` carries a
 * value outside that set exactly as the broker sent it, where it fits, because a
 * market this library has not normalized is still a market a caller may need to
 * see. Preservation is best-effort rather than total -- see the caveat below and
 * on `other_` itself.
 *
 * A code that will not fit, or that is not an uppercase identifier, leaves both
 * members unset and reads as `UNKNOWN`. That loses the raw value, which is worse
 * than preserving it and better than throwing out of a response parse and failing
 * an entire listing; the capacity is set well above every documented value.
 */
struct MarketSegment {
  /** Segments this library recognizes by name. */
  enum class Known {
    UNKNOWN,
    KOSPI,
    KOSDAQ,
    NYSE,
    NASDAQ,
    AMEX,
  };

  /** The normalized segment, or `Known::UNKNOWN` when the broker's code was not recognized. */
  Known known_{Known::UNKNOWN};
  /**
   * The broker's own code, preserved verbatim when `known_` is `UNKNOWN`.
   *
   * **Preservation is best-effort, not a guarantee.** `market_segment()` stores an unrecognized
   * code only if `Code<12>::try_make` accepts it, so one too long or outside its alphabet leaves
   * both this and `known_` empty and is genuinely lost. An empty `other_` beside
   * `Known::UNKNOWN` therefore means either "the broker named no segment" or "it named one this
   * field cannot hold".
   */
  Code<12> other_;

  /** Returns a display form: the normalized name, or the preserved raw code. */
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

  /** Compares the normalized segment and the preserved code together. */
  friend bool operator==(const MarketSegment& lhs, const MarketSegment& rhs) {
    return lhs.known_ == rhs.known_ && lhs.other_ == rhs.other_;
  }

  /** The negation of `operator==`. */
  friend bool operator!=(const MarketSegment& lhs, const MarketSegment& rhs) {
    return !(lhs == rhs);
  }
};

/**
 * Returns the segment named by a broker's code, preserving an unknown one where it fits.
 *
 * A code `Code<12>::try_make` rejects -- too long, or not an uppercase identifier -- leaves both
 * `known_` and `other_` empty, so it is not recoverable from the result.
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
 * What kind of security an instrument is.
 *
 * Adopted from Toss's enumeration, which is the more expressive of the two; KIS's
 * product codes derive into it. `UNKNOWN` appears only on a read path, for a type
 * this library does not normalize.
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

/** Whether an instrument is listed, awaiting listing, or delisted. */
enum class InstrumentStatus {
  UNKNOWN,
  SCHEDULED,
  ACTIVE,
  DELISTED,
};

/**
 * One step of an exchange's price grid.
 *
 * A tick applies from `from_` up to the next band's `from_`, so the bands are
 * stated by their lower bound alone.
 */
struct TickBand {
  /** The lower bound this band applies from, inclusive. */
  Decimal64 from_;
  /** The tick size within the band. */
  Decimal64 tick_;
};

/**
 * The price grid an instrument's orders must sit on.
 *
 * **This table is owned by the library, not by a broker.** Neither KIS nor Toss
 * publishes tick size anywhere in its API, so there is nothing to normalize from
 * and the grid has to be stated here. That makes it the one piece of market
 * structure in this library that can silently go stale when an exchange revises
 * its rules, which is why it is data a caller can replace rather than logic
 * compiled in, and why `source_` records what it came from.
 *
 * `kind_` distinguishes the two reasons a rule can have no bands, which behave
 * differently and must not be confused: a market that genuinely imposes no grid, and
 * an instrument this library has no table for. The second refuses to round rather
 * than passing a price through unchanged, because a caller that cannot round cannot
 * safely send a computed price.
 */
struct PriceRule {
  /** Why a rule has no bands, which is not always the same reason. */
  enum class Kind {
    /** The bands below describe the market's grid. */
    STATED,
    /** The market quotes in free decimals and imposes no grid. */
    UNCONSTRAINED,
    /**
     * The library has no grid for this instrument and will not guess one.
     *
     * KRX prices exchange-traded funds and notes on a different grid from cash
     * equities, and neither broker publishes either, so an ETF gets this rather than
     * the equity table. Attaching the wrong grid would be worse than attaching none:
     * it would round a price onto a ladder the exchange does not use and the order
     * would be rejected, with the library having confidently produced the price.
     */
    UNKNOWN,
  };

  /** Which of the three states this rule is in. Defaults to `UNKNOWN`, so an unset rule rounds nothing. */
  Kind kind_{Kind::UNKNOWN};
  /** The bands, ascending by `from_`. Empty unless `kind_` is `STATED`. */
  std::vector<TickBand> bands_;
  /** Where the table came from and when, so a stale grid is visible rather than assumed. */
  std::string source_;

  /** Returns the tick that applies at `price`, or nothing when there is no grid. */
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
   * Returns whether `price` sits on the grid, or nothing when that is unknown.
   *
   * An unknown grid answers neither yes nor no. Answering yes would let an off-grid
   * price through a check that appeared to have passed.
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
   * Returns `price` moved onto the grid, in the direction that cannot cross it.
   *
   * A buy rounds **down** and a sell rounds **up**, so rounding never improves a
   * price into territory the caller did not ask for: a buy limit that rounded up
   * would be willing to pay more than instructed.
   *
   * Returns nothing when the grid is unknown. A caller that cannot round cannot
   * safely send a computed price, and saying so is the only honest answer.
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

  /** Returns the KRX cash-equity grid revised in January 2023. */
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

  /** Returns a rule imposing no grid, for a market quoted in free decimals. */
  static PriceRule unconstrained() {
    PriceRule rule;
    rule.kind_ = Kind::UNCONSTRAINED;
    rule.source_ = "no exchange price grid";
    return rule;
  }

  /**
   * Returns a rule stating that this library has no grid for the instrument.
   *
   * `reason` records why, so a caller reading `source_` learns what is missing rather
   * than only that something is.
   */
  static PriceRule unknown(std::string reason) {
    PriceRule rule;
    rule.kind_ = Kind::UNKNOWN;
    rule.source_ = std::move(reason);
    return rule;
  }

private:
  /** Returns `price` modulo `tick`, computed exactly. */
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
 * Returns the price grid this library has for an instrument of `type` in `country`.
 *
 * Both adapters choose through this, so the decision has one definition. Cash
 * equities and the share-like types listed on KRX get the equity ladder; funds,
 * notes, warrants, and anything unrecognized get an unknown grid, because KRX prices
 * them differently and neither broker publishes the table.
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
 * Static reference data for one instrument.
 *
 * Everything a broker may omit is optional. `price_rule_` is the library's own grid
 * rather than the broker's, because no broker publishes one — and it depends on what
 * the instrument *is*, not only on where it trades: KRX prices funds and notes on a
 * different ladder from cash equities, so those arrive with an unknown grid rather
 * than the equity one.
 */
struct Instrument {
  /** The instrument this row describes. */
  InstrumentId id_;
  /** The local-language name as the broker spells it. */
  std::string name_;
  /** The English name, where the broker publishes one. Empty otherwise. */
  std::string english_name_;
  /** The ISIN, where the broker publishes one. Empty otherwise. */
  std::string isin_;
  /** The market segment: normalized where recognized, and otherwise preserved where `Code<12>` can hold it. */
  MarketSegment segment_;
  /** What kind of security it is, which is what selects the price grid. */
  SecurityType type_{SecurityType::UNKNOWN};
  /** Listed, awaiting listing, or delisted. */
  InstrumentStatus status_{InstrumentStatus::UNKNOWN};
  /** The currency it trades in. */
  Currency currency_;
  /** Whether this is the common share rather than a preferred or derived line. */
  bool common_share_{true};
  /**
   * The listing date, where the broker publishes one.
   *
   * Empty also covers a date that was present and unreadable -- a published date can look absent.
   * KIS's `compact_date_field` wants eight characters forming a real day and Toss's
   * `optional_calendar_date` wants `YYYY-MM-DD`; a wrong length, a wrong separator or an impossible
   * day yields empty rather than failing the row.
   *
   * Empty is not the only way a malformed date goes wrong, though. See the caveat on `Date::year_`:
   * a numeric prefix with trailing junk parses to a different real date, and a part with no leading
   * digit throws and fails the request.
   */
  std::optional<Date> listed_on_;
  /** The delisting date, where one exists and the broker publishes it. Empty as for `listed_on_`. */
  std::optional<Date> delisted_on_;
  /**
   * Shares outstanding, where the broker publishes the figure.
   *
   * The KIS adapter reads it through `positive_decimal`, so a blank *or a literal zero* both
   * arrive as empty. Empty therefore means "not stated", never "zero shares".
   */
  std::optional<Decimal64> shares_outstanding_;
  /** The order quantity increment, where the broker publishes one. */
  std::optional<Decimal64> lot_size_;
  /** The library's own price grid for this instrument; `UNKNOWN` where it has no table. */
  PriceRule price_rule_;
};

} // namespace algotradelib
