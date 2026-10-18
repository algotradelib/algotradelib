#pragma once

#include "../strategy/event.hpp"

#include "../math/scalar.hpp"

#include <algotradelib/core>

#include <chrono>
#include <utility>
#include <optional>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief Bar-interval arithmetic and the replay clock.
 *
 * Nothing here assumes a bar is a day: period counts come from the interval, the session window
 * and a trading-day count the caller states.
 */

/**
 * @brief Returns the fixed duration of an interval (`bar_interval_length`).
 * @param interval The interval.
 * @return The length of an intraday interval; `std::nullopt` for a day or longer, which has no fixed
 *   trading duration.
 */
inline std::optional<std::chrono::seconds> interval_duration(BarInterval interval) {
  return bar_interval_length(interval);
}

/**
 * @brief Returns how many bars of an interval a year contains, for `analytics::annualize_ratio`.
 *
 * Daily bars: `trading_days`. Intraday bars: the bars a session holds, times `trading_days`. A session
 * that is not a whole number of bars ends with a shorter one, which counts: KRX's 390-minute session holds
 * 390 one-minute bars (95,550 a year over 245 days) and 7 hourly ones, the last of them 30 minutes.
 *
 * @param interval The bar interval.
 * @param session The session window (used for intraday bars).
 * @param trading_days Trading days per year, such as about 245 for KRX or 252 for US markets.
 * @return The count, or `std::nullopt` for weekly, monthly or yearly bars, a non-positive
 *   `trading_days`, or (for intraday bars) a session whose close is not after its open.
 */
inline std::optional<math::Scalar> periods_per_year(
  BarInterval interval,
  const strategy::SessionWindow& session,
  math::Scalar trading_days
) {
  if (!(trading_days > math::Scalar{0})) {
    return std::nullopt;
  }
  if (interval == BarInterval::ONE_DAY) {
    return trading_days;
  }
  const auto duration = interval_duration(interval);
  if (!duration) {
    return std::nullopt;
  }
  const auto session_seconds =
    std::chrono::duration_cast<std::chrono::seconds>(session.close_ - session.open_).count();
  if (session_seconds <= 0 || duration->count() <= 0) {
    return std::nullopt;
  }
  // Rounded up: the session's last bar may be shorter than the others.
  const auto per_day = (session_seconds + duration->count() - 1) / duration->count();
  return static_cast<math::Scalar>(per_day) * trading_days;
}

/**
 * @brief A backtest's clock: the current instant and market phase, moved by the replayed events.
 *
 * It never reads the wall clock. "Now" is the period end of the last bar (the earliest moment it
 * could be known), or the instant of a quote.
 */
class ReplayClock {
public:
  /**
   * @brief Constructs a clock.
   * @param session The session window phases are derived from.
   */
  explicit ReplayClock(strategy::SessionWindow session = strategy::SessionWindow::krx())
  : session_(std::move(session)) {}

  /**
   * @brief Returns the session window.
   * @return The window phases are derived from.
   */
  const strategy::SessionWindow& session() const noexcept { return session_; }

  /**
   * @brief Returns the instant the replay has reached.
   * @return The current instant; the epoch before the first event.
   */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Returns the market phase at the current instant.
   * @return The phase; `UNKNOWN` before the first event.
   */
  strategy::MarketPhase phase() const noexcept { return phase_; }

  /**
   * @brief Moves the clock to a bar's period end and recomputes the phase.
   *
   * @param event The bar.
   * @param trading_day Whether the bar's date trades, as the calendar said; empty gives `UNKNOWN`,
   *   in which the default risk limits refuse to open positions.
   * @return `true` if the phase changed (so the caller fires `on_phase`).
   */
  bool advance(const strategy::BarEvent& event, std::optional<bool> trading_day) {
    return advance_to(event.period_end_, trading_day);
  }

  /**
   * @brief Moves the clock to an instant and recomputes the phase, for an event that is not a bar
   *   (such as a quote).
   *
   * The clock goes wherever it is told, even backwards; feeding events in time order is the
   * caller's job (see `Backtester::feed`).
   *
   * @param at The instant.
   * @param trading_day Whether that date trades; empty gives `UNKNOWN`.
   * @return `true` if the phase changed.
   */
  bool advance_to(
    std::chrono::system_clock::time_point at,
    std::optional<bool> trading_day
  ) {
    now_ = at;
    const auto next = session_.phase(now_, trading_day);
    const bool changed = next != phase_;
    phase_ = next;
    return changed;
  }

  /**
   * @brief Tests whether a bar follows the previous one with no gap.
   *
   * @param previous The earlier bar.
   * @param event The later bar.
   * @return `false` if both are minute bars more than a minute apart (such as across sessions);
   *   `true` otherwise, including for daily and longer bars, where gaps are not decided here.
   */
  static bool contiguous(
    const strategy::BarEvent& previous,
    const strategy::BarEvent& event
  ) {
    const auto duration = interval_duration(event.interval_);
    if (!duration || previous.interval_ != event.interval_) {
      // Undecidable for intervals with no fixed length; reported as contiguous.
      return true;
    }
    return event.period_end_ - previous.period_end_ <= *duration;
  }

private:
  strategy::SessionWindow session_;
  std::chrono::system_clock::time_point now_{};
  strategy::MarketPhase phase_{strategy::MarketPhase::UNKNOWN};
};

} // namespace backtest
} // namespace algotradelib
