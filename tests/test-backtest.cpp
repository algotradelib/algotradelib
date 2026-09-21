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

// ---------------------------------------------------------------------------
// The replay ledger: the portfolio nothing else can produce
// ---------------------------------------------------------------------------

namespace {

Currency krw() { return Currency{"KRW"}; }

/** An execution of `quantity` at `price`, with optional commission. */
backtest::Execution filled(
  OrderSide side,
  const char* quantity,
  const char* price,
  const char* commission = "0"
) {
  backtest::Execution execution;
  execution.fill_.instrument_ = samsung();
  execution.fill_.side_ = side;
  execution.fill_.quantity_ = Decimal64::parse(quantity);
  execution.fill_.price_ = Decimal64::parse(price);
  execution.commission_ = Decimal64::parse(commission);
  execution.slippage_ = Decimal64::parse("0");
  return execution;
}

backtest::ReplayLedger opening(const char* cash = "10000000") {
  return backtest::ReplayLedger(Decimal64::parse(cash), krw());
}

} // namespace

TEST_CASE("a buy moves the position and pays for it out of cash") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));

  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(ledger.average_price_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("70000")));
  // 10,000,000 less 700,000.
  REQUIRE(ledger.cash_in(krw()) == std::optional<Decimal64>(Decimal64::parse("9300000")));
  // Nothing closed, so nothing is realized.
  REQUIRE(ledger.realized_in(krw()) == Decimal64::parse("0"));
}

TEST_CASE("commission leaves cash on both sides of a trade") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000", "1000"), krw()));
  REQUIRE(ledger.cash_in(krw()) == std::optional<Decimal64>(Decimal64::parse("9299000")));

  // Selling brings cash in, and commission still goes out.
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "10", "71000", "1000"), krw()));
  // 9,299,000 + 710,000 - 1,000.
  REQUIRE(ledger.cash_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10008000")));
}

TEST_CASE("adding to a position blends the average by magnitude") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "80000"), krw()));

  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("20")));
  REQUIRE(ledger.average_price_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("75000")));
}

TEST_CASE("reducing a position leaves the average alone") {
  // Selling half a position does not change what the remaining half cost.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "20", "70000"), krw()));
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "10", "90000"), krw()));

  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(ledger.average_price_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("70000")));
  // Ten shares closed at 20,000 above the average.
  REQUIRE(ledger.realized_in(krw()) == Decimal64::parse("200000"));
}

TEST_CASE("a position crossing through flat closes one leg and opens the other") {
  // **The case that catches a wrong implementation.** Selling 150 from a long of 100 is not "a short
  // of 50 at the sale price". It closes 100, realizing against the old average, and opens 50 short at
  // the new price. One weighted average would carry the closed leg's cost basis into the new position
  // and misstate every later figure.
  auto ledger = opening("100000000");
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "100", "70000"), krw()));
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "150", "80000"), krw()));

  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("-50")));
  // The new short's basis is the sale price, with nothing of the old long's 70,000 in it.
  REQUIRE(ledger.average_price_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("80000")));
  // Only the 100 that closed realized, at 10,000 each.
  REQUIRE(ledger.realized_in(krw()) == Decimal64::parse("1000000"));
}

TEST_CASE("closing a short banks the opposite sign, profiting when the price fell") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "10", "80000"), krw()));
  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("-10")));

  // Buying it back lower is a profit, not a loss, which a sign error would get backwards.
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  REQUIRE(ledger.realized_in(krw()) == Decimal64::parse("100000"));
  REQUIRE(ledger.quantity_of(samsung())->coefficient() == 0);
}

TEST_CASE("a position returning to flat has no average price to report") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "10", "70000"), krw()));

  REQUIRE(ledger.quantity_of(samsung())->coefficient() == 0);
  // Flat, so there is no basis -- reported as absent rather than as a stale 70,000.
  REQUIRE_FALSE(ledger.average_price_of(samsung()));
}

TEST_CASE("equity is cash plus positions at their last mark") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  // Cash 9,300,000 and ten shares marked at the fill price.
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10000000")));

  // Marking up the position raises equity without any trade.
  ledger.mark(samsung(), Decimal64::parse("80000"));
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10100000")));
}

TEST_CASE("the snapshot a strategy sees carries positions and cash, and no resting orders") {
  // A replay fills or discards an order within one bar, so nothing rests. An engine modelling orders
  // that live across bars would fill working_ in, and reconcile would then do the same
  // projected-position arithmetic it does live.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));

  const auto as_of = utc(2026, 3, 10, 6, 30);
  const auto snapshot = ledger.snapshot<backtest::BacktestTag>(as_of);
  REQUIRE(snapshot.as_of_ == as_of);
  REQUIRE(snapshot.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(snapshot.cash_in(krw()) == std::optional<Decimal64>(Decimal64::parse("9300000")));
  REQUIRE(snapshot.working_.empty());
}

TEST_CASE("every execution is kept, so a report can attribute a result to its trades") {
  auto ledger = opening();
  ledger.apply(filled(OrderSide::BUY, "10", "70000", "500"), krw());
  ledger.apply(filled(OrderSide::SELL, "10", "71000", "500"), krw());

  REQUIRE(ledger.executions().size() == 2);
  REQUIRE(ledger.executions()[0].fill_.side_ == OrderSide::BUY);
  REQUIRE(ledger.executions()[1].commission_ == Decimal64::parse("500"));
}

TEST_CASE("cash in another currency is neither converted nor counted") {
  // The same rule Portfolio keeps: a total spanning currencies needs an exchange rate this library
  // does not carry, and inventing one would misprice every figure derived from it.
  auto ledger = opening();
  REQUIRE_FALSE(ledger.cash_in(Currency{"USD"}));
  REQUIRE(ledger.equity_in(Currency{"USD"}) == std::optional<Decimal64>(Decimal64::parse("0")));
}
