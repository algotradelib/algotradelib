#pragma once

#include "./types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace algotradelib {

/**
 * @brief The period one bar covers.
 *
 * Only periods a broker publishes; an interval a broker does not publish is refused, not built by
 * aggregating shorter bars.
 */
enum class BarInterval {
  ONE_MINUTE,
  ONE_DAY,
  ONE_WEEK,
  ONE_MONTH,
  ONE_YEAR,
  /** @brief Five minutes. */
  FIVE_MINUTES,
  /** @brief Fifteen minutes. */
  FIFTEEN_MINUTES,
  /** @brief Thirty minutes. */
  THIRTY_MINUTES,
  /** @brief Sixty minutes. */
  ONE_HOUR,
};

/** @brief Every `BarInterval`, in declaration order. */
inline constexpr std::array<BarInterval, 9> all_bar_intervals{
  BarInterval::ONE_MINUTE,
  BarInterval::ONE_DAY,
  BarInterval::ONE_WEEK,
  BarInterval::ONE_MONTH,
  BarInterval::ONE_YEAR,
  BarInterval::FIVE_MINUTES,
  BarInterval::FIFTEEN_MINUTES,
  BarInterval::THIRTY_MINUTES,
  BarInterval::ONE_HOUR,
};

/**
 * @brief Returns a short name for an interval, such as `"1m"`, `"5m"`, `"1h"` or `"1d"`.
 * @param interval The interval.
 * @return The name; empty for a value that is not a `BarInterval`.
 */
inline constexpr std::string_view bar_interval_name(BarInterval interval) noexcept {
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return "1m";
    case BarInterval::ONE_DAY:
      return "1d";
    case BarInterval::ONE_WEEK:
      return "1w";
    case BarInterval::ONE_MONTH:
      return "1mo";
    case BarInterval::ONE_YEAR:
      return "1y";
    case BarInterval::FIVE_MINUTES:
      return "5m";
    case BarInterval::FIFTEEN_MINUTES:
      return "15m";
    case BarInterval::THIRTY_MINUTES:
      return "30m";
    case BarInterval::ONE_HOUR:
      return "1h";
  }
  return "";
}

/**
 * @brief Returns an interval's fixed length, for an intraday interval.
 * @param interval The interval.
 * @return 60, 300, 900, 1800 or 3600 seconds; `std::nullopt` for a day or longer, which has no fixed
 *   trading length (a trading day is not 24 hours, and a month has no fixed length).
 */
inline constexpr std::optional<std::chrono::seconds> bar_interval_length(BarInterval interval) noexcept {
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return std::chrono::seconds(60);
    case BarInterval::FIVE_MINUTES:
      return std::chrono::seconds(300);
    case BarInterval::FIFTEEN_MINUTES:
      return std::chrono::seconds(900);
    case BarInterval::THIRTY_MINUTES:
      return std::chrono::seconds(1800);
    case BarInterval::ONE_HOUR:
      return std::chrono::seconds(3600);
    case BarInterval::ONE_DAY:
    case BarInterval::ONE_WEEK:
    case BarInterval::ONE_MONTH:
    case BarInterval::ONE_YEAR:
      return std::nullopt;
  }
  return std::nullopt;
}

static_assert(
  lists_every_value(all_bar_intervals, [](BarInterval interval) { return !bar_interval_name(interval).empty(); }),
  "all_bar_intervals must list every BarInterval"
);

/**
 * @brief Which edge of its period a bar's timestamp marks.
 *
 * Brokers differ: Toss stamps a minute bar at its close (covering `[t - 1m, t)`) and a daily bar
 * at local midnight, its open; KIS and Kiwoom stamp a minute bar at the start of its minute and a
 * daily bar by its trading date, both opens. Reading an open-stamped bar as close-stamped looks
 * ahead by one bar.
 */
enum class BarStamp {
  /** @brief The timestamp is the first instant of the period. */
  OPEN,
  /** @brief The timestamp is the instant the period ended. */
  CLOSE,
};

/**
 * @brief One bar: the prices and volume of one period, as exact decimals.
 *
 * The interval, stamp convention and currency are on the `BarSeries`.
 */
struct Bar {
  /** @brief The instant marking the period; `BarSeries::stamp_` says which edge. */
  std::chrono::system_clock::time_point timestamp_;
  /** @brief The first trade's price; empty if nothing traded in the period (never zero or the previous close). */
  std::optional<Decimal64> open_;
  /** @brief The highest trade price. */
  Decimal64 high_;
  /** @brief The lowest trade price. */
  Decimal64 low_;
  /** @brief The last trade price. */
  Decimal64 close_;
  /** @brief The shares traded. */
  Decimal64 volume_;
  /** @brief The value traded, if the broker publishes it. */
  std::optional<Decimal64> traded_value_;
};

/**
 * @brief The bars of one instrument and interval, oldest first.
 *
 * Adapters reverse the brokers' newest-first order, so bars are ascending in time.
 */
struct BarSeries {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The period each bar covers. */
  BarInterval interval_{BarInterval::ONE_DAY};
  /** @brief Which edge of its period each bar's timestamp marks. */
  BarStamp stamp_{BarStamp::CLOSE};
  /** @brief The currency of the prices. */
  Currency currency_;
  /** @brief Whether the prices are adjusted for corporate actions. */
  bool adjusted_{true};
  /**
   * @brief The bars, oldest first.
   *
   * There can be fewer than `BarQuery::count_`: paging stops on an empty page or when the page
   * budget is spent. Use the size.
   */
  std::vector<Bar> bars_;
  /**
   * @brief A cursor to continue from, if the broker has more.
   *
   * A short series may still carry one; an empty or exhausted one carries none.
   */
  std::optional<std::string> next_cursor_;
  /** @brief When the series was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;

  /**
   * @brief Tests whether the bars are strictly ascending in time.
   * @return `true` if each timestamp is later than the one before.
   */
  bool ascending() const {
    for (std::size_t index = 1; index < bars_.size(); ++index) {
      if (!(bars_[index - 1].timestamp_ < bars_[index].timestamp_)) {
        return false;
      }
    }
    return true;
  }
};

/** @brief One price level of an order book. */
struct DepthLevel {
  /** @brief The price. */
  Decimal64 price_;
  /** @brief The quantity resting at it. */
  Decimal64 quantity_;
};

/**
 * @brief An instrument's order book: resting bids and offers.
 *
 * Index zero is the best level on each side (bids descending, asks ascending); adapters sort the
 * levels themselves. A side may be empty.
 */
struct DepthSnapshot {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The bids, descending by price. */
  std::vector<DepthLevel> bids_;
  /** @brief The offers, ascending by price. */
  std::vector<DepthLevel> asks_;
  /** @brief The currency of the prices. */
  Currency currency_;
  /** @brief When the exchange says the book was true, if the broker gave a time. */
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
  /** @brief When the book was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;

  /**
   * @brief Returns the best bid.
   * @return The highest bid level, or `std::nullopt` if no bid rests.
   */
  std::optional<DepthLevel> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional<DepthLevel>(bids_.front());
  }

  /**
   * @brief Returns the best offer.
   * @return The lowest offer level, or `std::nullopt` if no offer rests.
   */
  std::optional<DepthLevel> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional<DepthLevel>(asks_.front());
  }

  /**
   * @brief Returns the spread, best offer minus best bid, exactly.
   * @return The spread, or `std::nullopt` if a side is empty or the arithmetic does not fit.
   */
  std::optional<Decimal64> spread() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
      return std::nullopt;
    }
    return decimal_subtract(ask->price_, bid->price_);
  }

  /**
   * @brief Returns the midpoint of the best bid and offer, exactly (one more decimal place if needed).
   *
   * Not rounded to the tick grid; round it yourself before using it as an order price.
   *
   * @return The midpoint, or `std::nullopt` if a side is empty or the arithmetic does not fit.
   */
  std::optional<Decimal64> mid() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
      return std::nullopt;
    }
    const auto sum = decimal_add(bid->price_, ask->price_);
    if (!sum) {
      return std::nullopt;
    }
    return decimal_halve(*sum);
  }
};

/**
 * @brief Sorts a book's levels best first: bids descending, asks ascending.
 * @param book The book to sort in place.
 */
inline void order_depth(DepthSnapshot& book) {
  const auto by_price = [](const DepthLevel& lhs, const DepthLevel& rhs) {
    return decimal_less(lhs.price_, rhs.price_);
  };
  std::sort(book.asks_.begin(), book.asks_.end(), by_price);
  std::sort(book.bids_.begin(), book.bids_.end(), [&by_price](const auto& l, const auto& r) {
    return by_price(r, l);
  });
}

/**
 * @brief One executed trade, as the broker reported it.
 *
 * No broker publishes which side was the aggressor, so there is no such field.
 */
struct TradePrint {
  /** @brief The trade price. */
  Decimal64 price_;
  /** @brief The quantity traded. */
  Decimal64 quantity_;
  /** @brief The currency of `price_`. */
  Currency currency_;
  /** @brief When the exchange says it executed, if the broker gave a time. */
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
};

/** @brief The highest and lowest price an instrument may trade at today. */
struct PriceLimits {
  /**
   * @brief The highest price allowed today.
   *
   * Empty does not mean unlimited: it is empty for a market with no limit (US equities), but also
   * when KIS sent a blank or zero figure for a KRX stock, which always has a limit. Never a bound
   * of zero.
   */
  std::optional<Decimal64> upper_;
  /** @brief The lowest price allowed today; empty means the same as for `upper_`. */
  std::optional<Decimal64> lower_;
  /** @brief The currency of the bounds. */
  Currency currency_;
  /** @brief When the exchange set the bounds, if the broker gave a time. */
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
};

/**
 * @brief What bars to fetch.
 *
 * The client pages until `count_` bars are read (brokers cap a page at 30 to 200 bars), within its
 * paging limits.
 */
struct BarQuery {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The period of each bar; an interval the broker does not publish is refused. */
  BarInterval interval_{BarInterval::ONE_DAY};
  /** @brief How many bars to return in total, across pages. */
  int count_{100};
  /** @brief The newest bar time to return, inclusive; pass the oldest timestamp seen to page backwards. */
  std::optional<std::chrono::system_clock::time_point> before_;
  /** @brief Whether to ask for prices adjusted for corporate actions. */
  bool adjusted_{true};
};

} // namespace algotradelib
