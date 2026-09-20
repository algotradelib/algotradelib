#pragma once

#include "./types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {

/**
 * Aggregation period of one bar.
 *
 * The set is closed and holds only periods a supported broker actually publishes.
 * An interval a broker does not publish is refused rather than produced by
 * aggregating a shorter one: resampling silently answers a different question from
 * the one asked, and a caller who wants it should ask for it deliberately.
 *
 * The enumerators are spelled `ONE_DAY` rather than `DAY_1` because POSIX
 * `<langinfo.h>` defines `DAY_1` through `DAY_7` and `MON_1` through `MON_12` as
 * macros, which a header cannot defend against once it is included.
 */
enum class BarInterval {
  ONE_MINUTE,
  ONE_DAY,
  ONE_WEEK,
  ONE_MONTH,
  ONE_YEAR,
};

/** Returns a stable lowercase name for `interval`, for logs and display. */
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
  }
  return "unknown";
}

/**
 * Which edge of its period a bar's timestamp marks.
 *
 * Brokers disagree, and the disagreement is invisible until a backtest reads a
 * bar it could not have known yet. Toss stamps a minute bar at its **close** and
 * aggregates `[t - 1m, t)`, while stamping a daily bar at local midnight — its
 * **open**. KIS reports a daily bar by its trading date.
 *
 * Carrying the convention in the value rather than in a comment is deliberate:
 * an off-by-one-bar lookahead is the single most common defect in backtesting
 * code, and it cannot be diagnosed from a timestamp alone.
 */
enum class BarStamp {
  /** `timestamp_` is the first instant of the period. */
  OPEN,
  /** `timestamp_` is the instant the period ended, exclusive of the next. */
  CLOSE,
};

/**
 * One aggregated period of trading.
 *
 * Prices and volume stay exact decimals: a bar is a broker-reported value that a
 * caller may compare, display, or send back, so it is not converted to floating
 * point here. The analytics layer converts at its own documented boundary.
 *
 * `open_` is optional because a trading day can produce no trade at all, which
 * KIS reports with `mod_yn`. An absent open means the broker stated none, never
 * zero, and never the previous close.
 */
struct Bar {
  std::chrono::system_clock::time_point timestamp_;
  BarStamp stamp_{BarStamp::CLOSE};
  BarInterval interval_{BarInterval::ONE_DAY};
  std::optional<Decimal64> open_;
  Decimal64 high_;
  Decimal64 low_;
  Decimal64 close_;
  Decimal64 volume_;
  std::optional<Decimal64> traded_value_;
  Currency currency_;
};

/**
 * Bars for one instrument and interval, oldest first.
 *
 * Both brokers return newest first; the adapter reverses once, at the boundary, so
 * that everything above it can assume ascending time. `adjusted_` records whether
 * the prices account for corporate actions, because the two brokers spell that
 * request differently and a caller cannot otherwise tell what it received.
 */
struct BarSeries {
  InstrumentId instrument_;
  BarInterval interval_{BarInterval::ONE_DAY};
  bool adjusted_{true};
  std::vector<Bar> bars_;
  std::optional<std::string> next_cursor_;
  std::chrono::system_clock::time_point received_at_;

  /** Returns whether the bars are in ascending time order with no duplicates. */
  bool ascending() const {
    for (std::size_t index = 1; index < bars_.size(); ++index) {
      if (!(bars_[index - 1].timestamp_ < bars_[index].timestamp_)) {
        return false;
      }
    }
    return true;
  }
};

/** One resting price level and the quantity available at it. */
struct DepthLevel {
  Decimal64 price_;
  Decimal64 quantity_;
};

/**
 * Resting orders on both sides of one instrument's book.
 *
 * `bids_` descend by price and `asks_` ascend, so index zero is the best level on
 * either side. A side may be empty, which is why every accessor returns an
 * optional rather than a zero a caller could mistake for a real price.
 *
 * Adapters establish that ordering themselves rather than trusting the broker to
 * have sent it. Toss's specification contradicts itself — it documents `asks` as
 * ascending and its own worked example lists them descending — and reading the best
 * offer off the wrong end of the book would make the spread negative and the
 * midpoint meaningless. Sorting a book of ten levels costs nothing next to being
 * wrong about which price is best.
 */
struct DepthSnapshot {
  InstrumentId instrument_;
  std::vector<DepthLevel> bids_;
  std::vector<DepthLevel> asks_;
  Currency currency_;
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
  std::chrono::system_clock::time_point received_at_;

  /** Returns the highest bid, or nothing when no bid rests. */
  std::optional<DepthLevel> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional<DepthLevel>(bids_.front());
  }

  /** Returns the lowest offer, or nothing when no offer rests. */
  std::optional<DepthLevel> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional<DepthLevel>(asks_.front());
  }

  /**
   * Returns the difference between the best offer and the best bid.
   *
   * Both sides must rest for a spread to exist. The subtraction is exact: the two
   * prices are rescaled to their common scale rather than converted to a double.
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
   * Returns the midpoint of the best bid and offer, when both rest.
   *
   * A midpoint halves an odd sum, so it needs one more decimal place than its
   * inputs. That place is added rather than rounded away, because rounding a
   * midpoint to the tick grid is a decision for whoever is about to place an
   * order, not for the value that reports it.
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
 * Puts a book's levels in the order `DepthSnapshot` promises.
 *
 * Best first on both sides: bids descending, asks ascending.
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
 * One executed trade as the broker reported it.
 *
 * `aggressor_` is optional because only some brokers say which side initiated.
 */
struct TradePrint {
  Decimal64 price_;
  Decimal64 quantity_;
  std::optional<OrderSide> aggressor_;
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
};

/**
 * Highest and lowest price an instrument may trade at today.
 *
 * Both are optional because markets without a price limit — United States
 * equities among them — report none, and an absent limit must not become a bound
 * of zero.
 */
struct PriceLimits {
  std::optional<Decimal64> upper_;
  std::optional<Decimal64> lower_;
  Currency currency_;
  std::optional<std::chrono::system_clock::time_point> exchange_time_;
};

/**
 * What bars to fetch.
 *
 * `count_` is how many bars the caller wants, not how many one request returns.
 * Every broker caps a page well below a useful window — 200 at Toss, 100 for KIS
 * daily bars, 30 or 120 for KIS minute bars — so satisfying `count_` is the port's
 * job, bounded by the same paging guards the holdings and order slices established.
 *
 * `before_` bounds the newest bar returned, so a caller can page backwards through
 * history by passing the oldest timestamp it has already seen. It is inclusive,
 * matching Toss's `before`; the KIS adapter walks its date and time inputs to the
 * same effect.
 *
 * `adjusted_` asks for prices that account for corporate actions. It is spelled
 * once here because the brokers disagree about how to ask: Toss takes a boolean,
 * and KIS takes `FID_ORG_ADJ_PRC` where `0` means adjusted and `1` means raw.
 */
struct BarQuery {
  InstrumentId instrument_;
  BarInterval interval_{BarInterval::ONE_DAY};
  int count_{100};
  std::optional<std::chrono::system_clock::time_point> before_;
  bool adjusted_{true};
};

} // namespace algotradelib
