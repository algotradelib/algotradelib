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
 * @file
 * @brief The events a runner delivers to a strategy's handlers, and the market phase.
 *
 * Each event is complete when the handler receives it; a handler decides and returns without
 * requesting more data.
 */

/**
 * @brief Where the clock stands relative to the trading session.
 *
 * Whether a date trades comes from the broker's calendar; the intraday open and close come from
 * the configured `SessionWindow`.
 */
enum class MarketPhase {
  /** @brief The calendar does not cover this date; nothing is known about it. */
  UNKNOWN,
  /** @brief A date the market does not trade. */
  CLOSED,
  /** @brief A trading day, before the session opens. */
  PRE_OPEN,
  /** @brief Inside the session. */
  OPEN,
  /** @brief A trading day, after the session closed. */
  POST_CLOSE,
};

/**
 * @brief Returns a lowercase name for a phase, such as `"pre_open"`, for logs and display.
 * @param phase The phase.
 * @return The name.
 */
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
 * @brief The intraday session of a market, as the operator states it.
 *
 * Times are minutes from local midnight, with a fixed UTC offset. The default is the KRX regular
 * session, 09:00-15:30 KST.
 *
 * @note The offset is fixed: a market that observes daylight saving needs a different window
 *   each season.
 */
struct SessionWindow {
  /** @brief The session open, in minutes from local midnight. */
  std::chrono::minutes open_{std::chrono::hours(9)};
  /** @brief The session close, in minutes from local midnight. */
  std::chrono::minutes close_{std::chrono::hours(15) + std::chrono::minutes(30)};
  /** @brief The market's offset from UTC. */
  std::chrono::minutes utc_offset_{std::chrono::hours(9)};
  /** @brief A description of where the window came from, for reports. */
  std::string source_{"KRX regular session, 09:00-15:30 KST (operator-stated)"};

  /**
   * @brief Returns the KRX regular session, 09:00-15:30 KST.
   * @return The default window.
   */
  static SessionWindow krx() { return SessionWindow{}; }

  /**
   * @brief Returns the market phase at an instant.
   *
   * @param now The instant.
   * @param trading_day Whether `now`'s local date (see `local_date`) is a trading day, as
   *   `TradingCalendar::is_trading_day` answered it.
   * @return `UNKNOWN` if `trading_day` is empty, `CLOSED` if it is `false`, and otherwise
   *   `PRE_OPEN`, `OPEN` or `POST_CLOSE` by local time. The open is inclusive, the close exclusive.
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
    const auto local = now + utc_offset_;
    const auto since_midnight =
      std::chrono::duration_cast<std::chrono::minutes>(local - local_midnight(now));
    if (since_midnight < open_) {
      return MarketPhase::PRE_OPEN;
    }
    if (close_ <= since_midnight) {
      return MarketPhase::POST_CLOSE;
    }
    return MarketPhase::OPEN;
  }

  /**
   * @brief Returns the market's local date at an instant.
   *
   * Ask the `TradingCalendar` about this date, not the machine's own: in another time zone they
   * differ for part of each day.
   *
   * @param now The instant.
   * @return The date in the market's time zone.
   */
  Date local_date(std::chrono::system_clock::time_point now) const {
    using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
    return date_from_epoch_days(
      std::chrono::duration_cast<Days>(local_midnight(now).time_since_epoch()).count()
    );
  }

  /**
   * @brief Returns the instant the session opens on the local date of an instant.
   *
   * @param now Any instant on the date in question.
   * @return The open of `local_date(now)`, whether or not that date trades.
   */
  std::chrono::system_clock::time_point opens_at(
    std::chrono::system_clock::time_point now
  ) const {
    return local_midnight(now) - utc_offset_ + open_;
  }

private:
  // The market's local midnight before `now`, as a UTC-shifted instant. floor rounds toward
  // negative infinity, so instants before the epoch work too. phase() and local_date() share it so
  // they always agree on the day.
  std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> local_midnight(
    std::chrono::system_clock::time_point now
  ) const {
    using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
    return std::chrono::time_point_cast<std::chrono::seconds>(
      std::chrono::floor<Days>(now + utc_offset_)
    );
  }
};

/**
 * @brief One completed bar, delivered to `on_bar`.
 *
 * A runner delivers only closed bars. `interval_` and `stamp_` say which period the bar covers;
 * `period_end_` is the instant that period ended, the earliest moment the bar could be known.
 */
struct BarEvent {
  /** @brief The instrument the bar is for. */
  InstrumentId instrument_;
  /** @brief The period length the bar covers. */
  BarInterval interval_{BarInterval::ONE_DAY};
  /** @brief Whether `bar_.timestamp_` marks the period's open or close. */
  BarStamp stamp_{BarStamp::CLOSE};
  /**
   * @brief The bar.
   *
   * @note Live, `live::SessionSource` holds back a daily or minute bar whose period has not
   *   ended, such as the broker's row for the current session. Weekly, monthly and yearly bars
   *   stamped at their open are delivered as read and may still be forming.
   */
  Bar bar_;
  /** @brief When the bar's period ended: the earliest moment the bar could be known. */
  std::chrono::system_clock::time_point period_end_{};
};

/**
 * @brief Computes the instant a bar's period ended.
 *
 * @param bar The bar.
 * @param interval The period length the bar covers.
 * @param stamp Whether `bar.timestamp_` marks the period's open or close.
 * @return `bar.timestamp_` for a `CLOSE`-stamped bar; for an `OPEN`-stamped bar, the timestamp
 *   plus one interval. A month counts as 31 days and a year as 366, so for those intervals the
 *   result is an upper bound, not the exact end.
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
      // Nominal: a month has no fixed length. Documented on the function.
      return bar.timestamp_ + std::chrono::hours(24 * 31);
    case BarInterval::ONE_YEAR:
      return bar.timestamp_ + std::chrono::hours(24 * 366);
  }
  return bar.timestamp_;
}

/**
 * @brief Makes a bar event, computing its `period_end_` with `bar_period_end`.
 *
 * @param instrument The instrument the bar is for.
 * @param interval The period length the bar covers.
 * @param stamp Whether the bar's timestamp marks the period's open or close.
 * @param bar The bar.
 * @return The event.
 */
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

/** @brief A quote the runner has just read, delivered to `on_quote`. */
struct QuoteEvent {
  /** @brief The quote, as the broker reported it. */
  QuoteSnapshot quote_;
};

/**
 * @brief One execution of an order the strategy placed, delivered to `on_fill`.
 *
 * Each execution is its own event, so a partial fill arrives as several; `quantity_` is this
 * execution's quantity, and the order's running total is in the portfolio. The order is named by
 * the broker's order id as a string; look it up in `ctx.portfolio()` to amend or cancel it.
 *
 * @warning A backtest delivers fills, but the shipped live loop never does: no broker inquiry
 *   produces a `Fill`. A strategy that reacts to fills in `on_fill` (for example, to set a stop)
 *   does nothing live. Live, executions show up through `on_order_update` and the portfolio.
 */
struct Fill {
  /** @brief The broker's order id. */
  std::string order_id_;
  /** @brief The instrument traded. */
  InstrumentId instrument_;
  /** @brief The side of the order. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The quantity of this execution, not the order's running total. */
  Decimal64 quantity_;
  /** @brief The execution price. */
  Decimal64 price_;
  /** @brief The tag of the intent that produced the order. */
  std::string tag_;
  /** @brief When the execution happened. */
  std::chrono::system_clock::time_point at_{};
};

} // namespace strategy
} // namespace algotradelib
