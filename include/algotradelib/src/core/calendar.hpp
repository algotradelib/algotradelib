#pragma once

#include "./types.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace algotradelib {

/**
 * @brief One calendar date and whether the market opens on it.
 *
 * `open_` means the exchange trades that day. It is not a banking or settlement
 * business day, which some brokers report separately.
 */
struct TradingDay {
  /**
   * @brief The calendar date, as the broker reports it.
   *
   * A broker row whose date cannot be read produces no `TradingDay`; it is counted in
   * `TradingCalendar::unreadable_days_` instead.
   */
  Date date_{};
  /** @brief Whether the market opens on `date_`. */
  bool open_{false};
};

/**
 * @brief The trading days a broker reported for one market, ascending by date.
 *
 * A calendar covers a finite span. Every lookup tells "closed" apart from "not
 * covered": a date outside the span, or one whose row could not be read, has no
 * answer rather than a guessed one.
 *
 * Intraday session hours are not part of this type; a broker that publishes them
 * exposes them through its own client.
 */
struct TradingCalendar {
  /** @brief The market this calendar is for. */
  Country country_{Country::KR};
  /** @brief The days the broker reported, ascending. A date outside them is uncovered, not closed. */
  std::vector<TradingDay> days_;
  /** @brief When the calendar was received, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
  /**
   * @brief How many rows from the broker had a date that could not be read.
   *
   * Each such row is dropped, and the day it described is uncovered: `is_trading_day`
   * returns nothing for it.
   *
   * @warning The dropped day can fall inside the covered span. When this count is
   * nonzero, `next_trading_day` and `previous_trading_day` may skip over a trading day.
   */
  std::size_t unreadable_days_{0};

  /**
   * @brief Looks up whether the market opens on a date.
   *
   * @param date The date to look up.
   * @return `true` if the market opens, `false` if it is closed, or `std::nullopt` if
   *   the calendar does not cover `date`.
   */
  std::optional<bool> is_trading_day(const Date& date) const {
    for (const auto& day : days_) {
      if (day.date_ == date) {
        return day.open_;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Finds the first trading day after a date.
   *
   * @param date The date to search from; it is not itself a candidate.
   * @return The first open day after `date`, or `std::nullopt` if none is covered.
   * @see unreadable_days_ for a day this search may skip.
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
   * @brief Finds the last trading day before a date.
   *
   * @param date The date to search from; it is not itself a candidate.
   * @return The last open day before `date`, or `std::nullopt` if none is covered.
   * @see unreadable_days_ for a day this search may skip.
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

  /**
   * @brief Returns the first and last dates the calendar covers.
   *
   * @return The pair `(first, last)`, or `std::nullopt` if the calendar is empty.
   */
  std::optional<std::pair<Date, Date>> covered() const {
    if (days_.empty()) {
      return std::nullopt;
    }
    return std::make_pair(days_.front().date_, days_.back().date_);
  }
};

} // namespace algotradelib
