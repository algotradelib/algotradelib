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
 * The segments here are the ones a supported broker names. `other_` carries any
 * value outside that set exactly as the broker sent it, because a market this
 * library has not normalized is still a market a caller may need to see.
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

  Known known_{Known::UNKNOWN};
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

  friend bool operator==(const MarketSegment& lhs, const MarketSegment& rhs) {
    return lhs.known_ == rhs.known_ && lhs.other_ == rhs.other_;
  }

  friend bool operator!=(const MarketSegment& lhs, const MarketSegment& rhs) {
    return !(lhs == rhs);
  }
};

/** Returns the segment named by a broker's code, preserving an unknown one. */
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
  Decimal64 from_;
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
 * An empty `bands_` means "no constraint", which is the correct description of a
 * market quoted in free decimals.
 */
struct PriceRule {
  std::vector<TickBand> bands_;
  std::string source_;

  /** Returns the tick that applies at `price`, or nothing when unconstrained. */
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

  /** Returns whether `price` already sits on the grid. */
  bool on_tick(const Decimal64& price) const {
    const auto tick = tick_at(price);
    if (!tick) {
      return true;
    }
    return remainder(price, *tick).has_value() &&
      remainder(price, *tick)->coefficient() == 0;
  }

  /**
   * Returns `price` moved onto the grid, in the direction that cannot cross it.
   *
   * A buy rounds **down** and a sell rounds **up**, so rounding never improves a
   * price into territory the caller did not ask for: a buy limit that rounded up
   * would be willing to pay more than instructed.
   */
  std::optional<Decimal64> round_to_tick(const Decimal64& price, OrderSide side) const {
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
    rule.source_ = "no exchange price grid";
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
 * Static reference data for one instrument.
 *
 * Everything a broker may omit is optional. `price_rule_` is the library's own
 * grid rather than the broker's, because no broker publishes one.
 */
struct Instrument {
  InstrumentId id_;
  std::string name_;
  std::string english_name_;
  std::string isin_;
  MarketSegment segment_;
  SecurityType type_{SecurityType::UNKNOWN};
  InstrumentStatus status_{InstrumentStatus::UNKNOWN};
  Currency currency_;
  bool common_share_{true};
  std::optional<Date> listed_on_;
  std::optional<Date> delisted_on_;
  std::optional<Decimal64> shares_outstanding_;
  std::optional<Decimal64> lot_size_;
  PriceRule price_rule_;
};

} // namespace algotradelib
