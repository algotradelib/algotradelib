#pragma once

#include "./types.hpp"

#include <chrono>
#include <cstddef>
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
   * Toss builds it with `parse_calendar_date`, which requires every component to be digits -- so a
   * value carrying a numeric prefix is rejected rather than silently becoming a different, valid
   * day. That mattered more here than anywhere else the caveat on `Date::year_` applies:
   * `is_trading_day` is what decides whether a strategy may place an order, and a row answering for
   * the wrong day answers that question wrongly. KIS's `bass_dt` is read the same way. **An
   * unreadable date produces no row** and is counted in `TradingCalendar::unreadable_days_`.
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
  /**
   * Rows the broker sent whose date could not be read, which therefore cover no day.
   *
   * Such a row is dropped rather than failing the calendar, so one damaged row costs its own day and
   * no other. The day it was for is then *uncovered* -- `is_trading_day` answers nothing -- which is
   * the conservative answer, but **it can sit inside the covered span**: `next_trading_day` and
   * `previous_trading_day` step over it, so where this is nonzero their answer is not proof that no
   * trading day lies between. `runner::PollingLoop` names this count when it cannot establish the
   * phase; a damaged row for another day is visible only here.
   */
  std::size_t unreadable_days_{0};

  /** Returns whether `date` is a trading day, or nothing when uncovered. */
  std::optional<bool> is_trading_day(const Date& date) const {
    for (const auto& day : days_) {
      if (day.date_ == date) {
        return day.open_;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns the first trading day after `date` within the covered span.
   *
   * Among the days that were read: see `unreadable_days_` for the day this can step over.
   */
  std::optional<Date> next_trading_day(const Date& date) const {
    for (const auto& day : days_) {
      if (day.open_ && date < day.date_) {
        return day.date_;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns the last trading day before `date` within the covered span.
   *
   * Among the days that were read: see `unreadable_days_` for the day this can step over.
   */
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
