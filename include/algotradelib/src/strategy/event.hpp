#pragma once

#include <algotradelib/core>

#include <chrono>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string>

namespace algotradelib {
namespace strategy {

/**
 * What a strategy is told about, and when.
 *
 * Every event is a value a runner has already finished assembling. A handler receives one,
 * decides, and returns; it never asks for more data, because anything it could ask for would be
 * a blocking request inside a handler. What the runner has is reachable through the context.
 */

/**
 * Where the clock stands relative to a trading session.
 *
 * **The day comes from the broker; the intraday boundaries come from configuration.** That split
 * is deliberate and follows `market-data-core`: both brokers publish which dates the market
 * trades, so `CLOSED` on a holiday is a broker-reported fact. Only Toss publishes intraday
 * session windows, and they are not normalized, so deriving `OPEN` from a broker would give one
 * answer for one broker and nothing for the other. Instead the operator states the window — for
 * KRX it is a published, stable fact, not something to discover per request.
 *
 * `UNKNOWN` is a distinct state rather than a synonym for `CLOSED`, for the same reason
 * `TradingCalendar` distinguishes them: treating an uncovered date as closed stops a strategy on
 * a day that is in fact open, and treating it as open lets one place orders into a holiday.
 */
enum class MarketPhase {
  /** The calendar does not cover this date, so nothing is known about it. */
  UNKNOWN,
  /** A date the market does not trade at all. */
  CLOSED,
  /** A trading day, before the configured session opens. */
  PRE_OPEN,
  /** Inside the configured session. */
  OPEN,
  /** A trading day, after the configured session closed. */
  POST_CLOSE,
};

/** Returns a stable lowercase name for `phase`, for logs and display. */
inline constexpr std::string_view market_phase_name(MarketPhase phase) noexcept {
  switch (phase) {
    case MarketPhase::UNKNOWN:
      return "unknown";
    case MarketPhase::CLOSED:
      return "closed";
    case MarketPhase::PRE_OPEN:
      return "pre_open";
    case MarketPhase::OPEN:
      return "open";
    case MarketPhase::POST_CLOSE:
      return "post_close";
  }
  return "unknown";
}

/**
 * The intraday window a market trades in, as the operator states it.
 *
 * Held as minutes since local midnight plus the market's offset from UTC, because that is what
 * the exchange publishes. Not read from a broker: see `MarketPhase` for why.
 *
 * The offset is fixed rather than a time zone. Both markets this library targets observe no
 * daylight saving, and a fixed offset that is wrong is obvious immediately, whereas a zone
 * database this library carried would be wrong quietly and a year late.
 */
struct SessionWindow {
  /** Minutes from local midnight to the session open. */
  std::chrono::minutes open_{std::chrono::hours(9)};
  /** Minutes from local midnight to the session close. */
  std::chrono::minutes close_{std::chrono::hours(15) + std::chrono::minutes(30)};
  /** The market's fixed offset from UTC. */
  std::chrono::minutes utc_offset_{std::chrono::hours(9)};
  /** Where the window came from, so a reader learns who stated it. */
  std::string source_{"KRX regular session, 09:00-15:30 KST (operator-stated)"};

  /** Returns the KRX regular session. */
  static SessionWindow krx() { return SessionWindow{}; }

  /**
   * Returns the phase at `now`, given whether its date is a trading day.
   *
   * `trading_day` is what `TradingCalendar::is_trading_day` answered, including its emptiness:
   * an uncovered date yields `UNKNOWN` rather than a guess.
   */
  MarketPhase phase(
    std::chrono::system_clock::time_point now,
    std::optional<bool> trading_day
  ) const {
    if (!trading_day) {
      return MarketPhase::UNKNOWN;
    }
    if (!*trading_day) {
      return MarketPhase::CLOSED;
    }
    // `std::chrono::days` is C++20; this is the same duration, spelled for C++17. `floor`
    // rounds toward negative infinity, so the arithmetic is right for instants before the
    // epoch as well as after it.
    using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
    const auto local = now + utc_offset_;
    const auto midnight = std::chrono::floor<Days>(local);
    const auto since_midnight =
      std::chrono::duration_cast<std::chrono::minutes>(local - midnight);
    if (since_midnight < open_) {
      return MarketPhase::PRE_OPEN;
    }
    if (close_ <= since_midnight) {
      return MarketPhase::POST_CLOSE;
    }
    return MarketPhase::OPEN;
  }
};

/**
 * One completed bar, with everything needed to know which period it describes.
 *
 * A runner delivers only **closed** bars. A handler that saw a forming bar would be reading a
 * high and a low that can still change, which is a lookahead defect that shows up as a backtest
 * that cannot be reproduced live.
 *
 * `interval_` and `stamp_` are carried because a bar does not carry them — they live on
 * `BarSeries`, being constant across a series — and a strategy needs both to know what
 * `bar_.timestamp_` means. `period_end_` is that arithmetic already done: the instant the bar's
 * period finished, which is the earliest moment the strategy could legitimately have known the
 * bar. Deriving it needs the interval and the stamp convention together, so getting it wrong is
 * easy, which is why it is computed once here rather than in every strategy.
 */
struct BarEvent {
  InstrumentId instrument_;
  BarInterval interval_{BarInterval::ONE_DAY};
  BarStamp stamp_{BarStamp::CLOSE};
  Bar bar_;
  std::chrono::system_clock::time_point period_end_{};
};

/**
 * Returns `bar`'s period end, given how its timestamp is stamped.
 *
 * A `CLOSE`-stamped bar already carries it. An `OPEN`-stamped bar ends one interval later, which
 * for a calendar interval — a month, a year — is not a fixed duration, so those are advanced by
 * calendar arithmetic rather than by adding a nominal number of days.
 */
inline std::chrono::system_clock::time_point bar_period_end(
  const Bar& bar,
  BarInterval interval,
  BarStamp stamp
) {
  if (stamp == BarStamp::CLOSE) {
    return bar.timestamp_;
  }
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return bar.timestamp_ + std::chrono::minutes(1);
    case BarInterval::ONE_DAY:
      return bar.timestamp_ + std::chrono::hours(24);
    case BarInterval::ONE_WEEK:
      return bar.timestamp_ + std::chrono::hours(24 * 7);
    case BarInterval::ONE_MONTH:
      // Nominal, and deliberately so: a month has no fixed length, and the alternative is a
      // civil-calendar dependency for a value only used to order events that are already
      // ordered. Documented rather than hidden, because a caller comparing a monthly bar's
      // period end to a precise instant would be relying on more than this can promise.
      return bar.timestamp_ + std::chrono::hours(24 * 31);
    case BarInterval::ONE_YEAR:
      return bar.timestamp_ + std::chrono::hours(24 * 366);
  }
  return bar.timestamp_;
}

/** Returns a bar event with `period_end_` derived from the interval and stamp convention. */
inline BarEvent make_bar_event(
  InstrumentId instrument,
  BarInterval interval,
  BarStamp stamp,
  Bar bar
) {
  BarEvent event;
  event.instrument_ = std::move(instrument);
  event.interval_ = interval;
  event.stamp_ = stamp;
  event.period_end_ = bar_period_end(bar, interval, stamp);
  event.bar_ = std::move(bar);
  return event;
}

/** One quote the runner has just read. */
struct QuoteEvent {
  QuoteSnapshot quote_;
};

/**
 * One execution against an order this strategy placed.
 *
 * Broker-neutral on purpose, unlike `CancelIntent`. A fill is identified by the broker's own
 * order id as a string rather than by a typed `OrderRef<Broker>`, so `on_fill`'s signature is
 * writable without naming a broker or reaching for `Ctx::Fill`. A strategy that needs to act on
 * the order — amend or cancel it — looks it up in `ctx.portfolio()`, where the typed reference
 * lives.
 *
 * `tag_` is the label the strategy put on the intent that produced the order, carried back so a
 * fill can be attributed to the decision that caused it.
 *
 * `quantity_` is the quantity of *this* execution, not the order's running total. A partial fill
 * arrives as one event per execution, and summing them is the strategy's business; the running
 * total is in the portfolio.
 */
struct Fill {
  std::string order_id_;
  InstrumentId instrument_;
  OrderSide side_{OrderSide::BUY};
  Decimal64 quantity_;
  Decimal64 price_;
  std::string tag_;
  std::chrono::system_clock::time_point at_{};
};

} // namespace strategy
} // namespace algotradelib
