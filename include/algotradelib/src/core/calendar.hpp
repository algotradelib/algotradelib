#pragma once

#include "./types.hpp"

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

namespace algotradelib {

/**
 * Whether one calendar date is a day the market trades.
 *
 * Only that one question is normalized. Brokers report several related but
 * distinct answers — KIS separately reports a banking business day, a securities
 * business day, a market-open day, and a settlement day — and collapsing them
 * would silently pick one meaning for all four. The one a caller placing an order
 * needs is whether the market opens, which is what `open_` carries and what KIS
 * itself directs callers to use.
 */
struct TradingDay {
  /**
   * The calendar date, in the broker's own locale.
   *
   * Toss builds it with `parse_calendar_date`, so the caveat on `Date::year_` applies and matters
   * more here than anywhere else it does: `is_trading_day` is what decides whether a strategy may
   * place an order, and a row whose date silently parsed to a different day answers for that day
   * instead. An unparseable date fails the calendar request rather than producing a row.
   */
  Date date_{};
  /** Whether the market opens on it. Not a banking or settlement day; see above. */
  bool open_{false};
};

/**
 * The trading days a broker reported, ascending by date.
 *
 * A calendar covers a finite span, so every lookup distinguishes "closed" from
 * "not covered". That distinction is the point: treating an uncovered date as
 * closed would stop a strategy for a day that is in fact open, and treating it as
 * open would let one place orders into a holiday.
 *
 * Intraday session windows are deliberately absent. Only Toss publishes them, so
 * they stay reachable through that broker's client rather than becoming a field
 * that is permanently empty for the other.
 */
struct TradingCalendar {
  /** The market this calendar is for. */
  Country country_{Country::KR};
  /** The days the broker reported, ascending. A date outside this span is uncovered, not closed. */
  std::vector<TradingDay> days_;
  /** When the calendar arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;

  /** Returns whether `date` is a trading day, or nothing when uncovered. */
  std::optional<bool> is_trading_day(const Date& date) const {
    for (const auto& day : days_) {
      if (day.date_ == date) {
        return day.open_;
      }
    }
    return std::nullopt;
  }

  /** Returns the first trading day after `date` within the covered span. */
  std::optional<Date> next_trading_day(const Date& date) const {
    for (const auto& day : days_) {
      if (day.open_ && date < day.date_) {
        return day.date_;
      }
    }
    return std::nullopt;
  }

  /** Returns the last trading day before `date` within the covered span. */
  std::optional<Date> previous_trading_day(const Date& date) const {
    std::optional<Date> found;
    for (const auto& day : days_) {
      if (!(day.date_ < date)) {
        break;
      }
      if (day.open_) {
        found = day.date_;
      }
    }
    return found;
  }

  /** Returns the covered span, or nothing when the calendar is empty. */
  std::optional<std::pair<Date, Date>> covered() const {
    if (days_.empty()) {
      return std::nullopt;
    }
    return std::make_pair(days_.front().date_, days_.back().date_);
  }
};

} // namespace algotradelib
