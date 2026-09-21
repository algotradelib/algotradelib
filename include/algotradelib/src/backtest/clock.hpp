#pragma once

#include "../strategy/event.hpp"

#include "../math/scalar.hpp"

#include <algotradelib/core>

#include <chrono>
#include <utility>
#include <cstdint>
#include <optional>
#include <ratio>

namespace algotradelib {
namespace backtest {

/**
 * Turning a bar interval into the numbers a backtest needs, without assuming a bar is a day.
 *
 * **The rule this whole engine is built on.** Neither supported broker makes intraday history easy —
 * KIS serves minute bars one trading day at a time — but that is a limitation of two adapters, not a
 * property of backtesting. A broker that serves minute history, or a caller with their own bar files,
 * must be able to run an intraday backtest without the engine being rewritten. So nothing here
 * multiplies by 245 or divides by 24 hours; everything is derived from the interval, the session, and
 * a trading-day count the caller states.
 */

/** Returns the nominal duration of one `interval`, or nothing for a calendar interval. */
inline std::optional<std::chrono::seconds> interval_duration(BarInterval interval) {
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return std::chrono::seconds(60);
    case BarInterval::ONE_DAY:
    case BarInterval::ONE_WEEK:
    case BarInterval::ONE_MONTH:
    case BarInterval::ONE_YEAR:
      // Nothing, deliberately. A day is not 24 hours of *trading*, and a month has no fixed length at
      // all. A caller wanting a period count for these states the trading-day count instead, which is
      // a calendar fact rather than an arithmetic one.
      return std::nullopt;
  }
  return std::nullopt;
}

/**
 * Returns how many bars of `interval` a year contains, given a session and a trading-day count.
 *
 * What `analytics::annualize_ratio` needs, and the reason that function takes the period count rather
 * than defaulting to one: the answer is `245` for daily bars on KRX and `95,550` for minute bars over
 * the same year, and no default serves both.
 *
 * `trading_days` is a calendar fact the caller supplies — about 245 for KRX, 252 for US markets — and
 * deliberately has no default here, because a library guessing a market's calendar is how a Sharpe
 * ratio ends up silently scaled by the wrong constant.
 *
 * Returns nothing when the interval does not divide the session evenly enough to answer: a weekly bar
 * over a five-day week is `trading_days / 5`, which this does not attempt, because the right divisor
 * depends on the market's week and not on arithmetic.
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
  const auto per_day = static_cast<math::Scalar>(session_seconds) /
    static_cast<math::Scalar>(duration->count());
  return per_day * trading_days;
}

/**
 * Where a backtest's clock stands, derived from the bars it has replayed.
 *
 * Holds no wall-clock time at all. A backtest's "now" is the instant of the last bar's period end —
 * the earliest moment that bar could legitimately have been known — and reading a real clock would
 * make a replay depend on when it was run.
 */
class ReplayClock {
public:
  /** Constructs a clock reading the session `session`. */
  explicit ReplayClock(strategy::SessionWindow session = strategy::SessionWindow::krx())
  : session_(std::move(session)) {}

  /** Returns the session window phases are derived against. */
  const strategy::SessionWindow& session() const noexcept { return session_; }

  /** Returns the instant the replay has reached. */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /** Returns the phase at the current instant. */
  strategy::MarketPhase phase() const noexcept { return phase_; }

  /**
   * Advances to `event`'s period end and recomputes the phase.
   *
   * Returns whether the phase changed, so a caller knows to fire `on_phase`. A daily replay sees one
   * transition per bar at most; a minute replay sees the real intraday sequence. Neither is special
   * cased, which is the whole point.
   *
   * `trading_day` is what the calendar said about the bar's date, including its emptiness: an
   * uncovered date yields `UNKNOWN`, which the risk guards refuse to trade in by default.
   */
  bool advance(const strategy::BarEvent& event, std::optional<bool> trading_day) {
    now_ = event.period_end_;
    const auto next = session_.phase(now_, trading_day);
    const bool changed = next != phase_;
    phase_ = next;
    return changed;
  }

  /**
   * Returns whether `event` follows `previous` without an intervening gap.
   *
   * A minute replay crossing a session boundary has hours between two consecutive bars, and that gap
   * is not a bar: nothing happened in it that a strategy should be told about. Reported rather than
   * hidden so the engine can decline to interpolate, which is the only honest thing to do with a
   * period no data covers.
   */
  static bool contiguous(
    const strategy::BarEvent& previous,
    const strategy::BarEvent& event
  ) {
    const auto duration = interval_duration(event.interval_);
    if (!duration || previous.interval_ != event.interval_) {
      // Calendar intervals have no fixed length, so contiguity is not decidable here. Saying "yes"
      // would let a gap pass unremarked; saying "no" would flag every daily bar. Undecidable is
      // reported as contiguous and left to the calendar, which is the component that knows.
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
