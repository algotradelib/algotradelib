#include <algotradelib/backtest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace backtest = algotradelib::backtest;

namespace {

using Clock = std::chrono::system_clock;

/** Builds a UTC instant without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) +
    std::chrono::minutes(minute);
}

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** A bar closing at `close`, stamped at `at`. */
strategy::BarEvent bar_at(Clock::time_point at, BarInterval interval, const char* close) {
  Bar bar;
  bar.timestamp_ = at;
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), interval, BarStamp::CLOSE, bar);
}

} // namespace

// ---------------------------------------------------------------------------
// Periods per year, the number an annualized figure needs
// ---------------------------------------------------------------------------

TEST_CASE("a daily interval annualizes on the trading-day count the caller states") {
  const auto session = strategy::SessionWindow::krx();
  // 245 for KRX, 252 for US markets. No default, because a library guessing a market's calendar is
  // how a Sharpe ratio ends up silently scaled by the wrong constant.
  REQUIRE(backtest::periods_per_year(BarInterval::ONE_DAY, session, 245.0) == Catch::Approx(245.0));
  REQUIRE(backtest::periods_per_year(BarInterval::ONE_DAY, session, 252.0) == Catch::Approx(252.0));
}

TEST_CASE("a minute interval annualizes on the session length, not on a day") {
  // The KRX regular session is 09:00-15:30, which is 390 minutes. Over 245 trading days that is
  // 95,550 minute bars -- and this is exactly why annualize_ratio takes the period count rather than
  // defaulting to one: no single default serves both this and 245.
  const auto session = strategy::SessionWindow::krx();
  const auto minutes = backtest::periods_per_year(BarInterval::ONE_MINUTE, session, 245.0);
  REQUIRE(minutes);
  REQUIRE(*minutes == Catch::Approx(390.0 * 245.0));

  // A shorter session gives proportionally fewer, derived rather than assumed.
  strategy::SessionWindow half;
  half.open_ = std::chrono::hours(9);
  half.close_ = std::chrono::hours(12);
  const auto shortened = backtest::periods_per_year(BarInterval::ONE_MINUTE, half, 245.0);
  REQUIRE(shortened);
  REQUIRE(*shortened == Catch::Approx(180.0 * 245.0));
}

TEST_CASE("a calendar interval has no derivable period count, and says so") {
  // A month has no fixed length and a week's divisor depends on the market's week, so answering would
  // be inventing a constant. Nothing is better than a plausible wrong number here.
  const auto session = strategy::SessionWindow::krx();
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_WEEK, session, 245.0));
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_MONTH, session, 245.0));
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_YEAR, session, 245.0));
}

TEST_CASE("a non-positive trading-day count is refused rather than scaled by") {
  const auto session = strategy::SessionWindow::krx();
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_DAY, session, 0.0));
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_DAY, session, -245.0));
}

TEST_CASE("only a minute interval has a fixed duration") {
  REQUIRE(backtest::interval_duration(BarInterval::ONE_MINUTE) == std::chrono::seconds(60));
  // A trading day is not 24 hours of trading, so a day has no fixed duration for this purpose.
  REQUIRE_FALSE(backtest::interval_duration(BarInterval::ONE_DAY));
  REQUIRE_FALSE(backtest::interval_duration(BarInterval::ONE_MONTH));
}

// ---------------------------------------------------------------------------
// The replay clock, at two intervals
// ---------------------------------------------------------------------------

TEST_CASE("the clock reads a bar's period end, never a wall clock") {
  // A replay's "now" is the earliest instant the bar could legitimately have been known. Reading a
  // real clock would make a replay depend on when it was run.
  backtest::ReplayClock clock;
  const auto when = utc(2026, 3, 10, 3, 0);
  clock.advance(bar_at(when, BarInterval::ONE_MINUTE, "70000"), true);
  REQUIRE(clock.now() == when);
}

TEST_CASE("a daily replay sees at most one phase transition per bar") {
  backtest::ReplayClock clock;
  // 03:00 UTC is 12:00 KST, inside the session.
  REQUIRE(clock.advance(bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_DAY, "70000"), true));
  REQUIRE(clock.phase() == strategy::MarketPhase::OPEN);

  // The next day at the same time is the same phase, so nothing changed.
  REQUIRE_FALSE(clock.advance(bar_at(utc(2026, 3, 11, 3, 0), BarInterval::ONE_DAY, "70100"), true));
  REQUIRE(clock.phase() == strategy::MarketPhase::OPEN);

  // A holiday is closed, which is a change.
  REQUIRE(clock.advance(bar_at(utc(2026, 3, 12, 3, 0), BarInterval::ONE_DAY, "70200"), false));
  REQUIRE(clock.phase() == strategy::MarketPhase::CLOSED);
}

TEST_CASE("a minute replay sees the real intraday sequence, with no special casing") {
  // The same clock, the same code, a different interval. KST is UTC+9, so the session runs
  // 00:00-06:30 UTC.
  backtest::ReplayClock clock;

  clock.advance(bar_at(utc(2026, 3, 9, 23, 30), BarInterval::ONE_MINUTE, "70000"), true);
  REQUIRE(clock.phase() == strategy::MarketPhase::PRE_OPEN);

  REQUIRE(clock.advance(bar_at(utc(2026, 3, 10, 0, 1), BarInterval::ONE_MINUTE, "70000"), true));
  REQUIRE(clock.phase() == strategy::MarketPhase::OPEN);

  REQUIRE_FALSE(clock.advance(bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70100"), true));
  REQUIRE(clock.phase() == strategy::MarketPhase::OPEN);

  REQUIRE(clock.advance(bar_at(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70200"), true));
  REQUIRE(clock.phase() == strategy::MarketPhase::POST_CLOSE);
}

TEST_CASE("an uncovered calendar date reaches the clock as unknown") {
  // Carried through so the risk guards can refuse it, which they do by default. Assuming a date is
  // open would let a replay trade into a holiday; assuming closed would skip a day that traded.
  backtest::ReplayClock clock;
  clock.advance(bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_DAY, "70000"), std::nullopt);
  REQUIRE(clock.phase() == strategy::MarketPhase::UNKNOWN);
}

TEST_CASE("a gap between sessions is reported rather than interpolated") {
  // A minute replay crossing a session boundary has hours between two consecutive bars, and that gap
  // is not a bar: nothing happened in it that a strategy should be told about.
  const auto close = bar_at(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000");
  const auto next_minute = bar_at(utc(2026, 3, 10, 6, 31), BarInterval::ONE_MINUTE, "70010");
  const auto next_session = bar_at(utc(2026, 3, 11, 0, 1), BarInterval::ONE_MINUTE, "70100");

  REQUIRE(backtest::ReplayClock::contiguous(close, next_minute));
  REQUIRE_FALSE(backtest::ReplayClock::contiguous(close, next_session));
}

TEST_CASE("contiguity of a calendar interval is left to the calendar that knows") {
  // Saying "not contiguous" would flag every daily bar; saying nothing would let a real gap pass. The
  // honest answer is that this function cannot decide, so it defers.
  const auto monday = bar_at(utc(2026, 3, 9, 6, 30), BarInterval::ONE_DAY, "70000");
  const auto friday = bar_at(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70100");
  REQUIRE(backtest::ReplayClock::contiguous(monday, friday));
}
