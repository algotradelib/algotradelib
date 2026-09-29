#include <algotradelib/backtest>
#include <algotradelib/vanilla>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace backtest = algotradelib::backtest;
namespace vanilla = algotradelib::strategy::vanilla;

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

/** An execution in `instrument`, so a ledger can hold two currencies at once. */
backtest::Execution filled_in(
  const InstrumentId& instrument,
  OrderSide side,
  const char* quantity,
  const char* price
) {
  auto execution = filled(side, quantity, price);
  execution.fill_.instrument_ = instrument;
  return execution;
}

InstrumentId apple() { return InstrumentId{Country::US, Venue::COMBINED, "AAPL"}; }

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

TEST_CASE("the ledger supplies positions and cash; working orders are not its to know") {
  // The empty field is correct *here*: the ledger has never seen an order, and the queue belongs to
  // the executor. `Backtester` fills it in when it installs a portfolio, which is where the engine
  // test below checks it. The claim this test used to make -- that nothing rests in a replay -- was
  // false, and is what left the guards reading an empty field in a backtest and a full one live.
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

TEST_CASE("a fill whose arithmetic will not fit leaves the ledger untouched") {
  // `Backtester` reads false as "the order did not fill", so a partly applied ledger means a replay
  // carries on holding the effects of a fill it counted as never happening. Every fallible step used
  // to run against the ledger itself: find_or_open pushed a lot, add_realized banked, and
  // weighted_average blended, all before a later step could fail.
  // **The failure has to come after the lot is opened**, which is the whole point: an execution whose
  // very first multiplication overflows returns false before touching anything, in any version. Cash
  // at the maximum an exact decimal holds, and a sale whose proceeds will not fit on top of it, fails
  // in `adjust_cash` -- which the old code reached only after opening the lot, setting its average,
  // its quantity and its mark.
  auto ledger = opening("9223372036854775807");

  const auto positions_before = ledger.positions().size();
  const auto cash_before = ledger.cash_in(krw());
  const auto executions_before = ledger.executions().size();

  REQUIRE_FALSE(ledger.apply(filled(OrderSide::SELL, "10", "70000"), krw()));

  REQUIRE(ledger.positions().size() == positions_before);
  REQUIRE(ledger.cash_in(krw()) == cash_before);
  REQUIRE(ledger.executions().size() == executions_before);
  // No short position was opened by a fill the ledger reported as not having happened.
  REQUIRE_FALSE(ledger.quantity_of(samsung()));
}

TEST_CASE("realized profit is not banked by a fill that then fails") {
  // The other half of the same promise. `add_realized` ran before the steps that can still fail, so
  // a closing trade could bank its profit and then report that it never happened.
  auto ledger = opening("9223372036854775000");
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  const auto realized_before = ledger.realized_in(krw());
  const auto quantity_before = ledger.quantity_of(samsung());

  // Closing at a profit, with proceeds that will not fit on top of the cash already held.
  REQUIRE_FALSE(ledger.apply(filled(OrderSide::SELL, "10", "80000"), krw()));

  REQUIRE(ledger.realized_in(krw()) == realized_before);
  REQUIRE(ledger.quantity_of(samsung()) == quantity_before);
}

TEST_CASE("a mark that will not fit reports itself and leaves the lot coherent") {
  // **Both halves of this were got wrong once each.** Advancing the price and keeping the old
  // market value handed a strategy reading positions() a stale number presented as current, with no
  // error anywhere. Skipping the row and saying nothing silently declined to apply a price the
  // caller asked for. The lot now moves as a whole or not at all, and the caller is told which.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "2", "70000"), krw()));
  const auto& lot = ledger.positions().front();
  REQUIRE(lot.market_value_.amount_ == Decimal64::parse("140000"));

  REQUIRE_FALSE(ledger.mark(samsung(), Decimal64::parse("9223372036854775807")));

  // Price and value still agree, and still describe the mark actually in force.
  REQUIRE(ledger.positions().front().last_price_ == Decimal64::parse("70000"));
  REQUIRE(ledger.positions().front().market_value_.amount_ == Decimal64::parse("140000"));
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10000000")));

  // And an ordinary mark afterwards still lands, so nothing was left wedged.
  REQUIRE(ledger.mark(samsung(), Decimal64::parse("80000")));
  REQUIRE(ledger.positions().front().market_value_.amount_ == Decimal64::parse("160000"));
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10020000")));
}

TEST_CASE("an instrument does not change currency underneath an open position") {
  // Backtester resolves the currency from fixed reference data, so it cannot reach this; the ledger
  // is public and a direct caller can. The second fill used to relabel the whole lot, so a position
  // bought in won reported as dollars while the cash stayed split across both.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "1", "70000"), krw()));
  REQUIRE_FALSE(ledger.apply(filled(OrderSide::BUY, "1", "70000"), Currency{"USD"}));

  REQUIRE(ledger.quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("1")));
  REQUIRE(ledger.positions().front().market_value_.currency_ == krw());
  REQUIRE_FALSE(ledger.cash_in(Currency{"USD"}));

  // Once flat there is no position to mislabel, so the lot may be re-denominated.
  REQUIRE(ledger.apply(filled(OrderSide::SELL, "1", "70000"), krw()));
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "1", "200"), Currency{"USD"}));
  REQUIRE(ledger.positions().front().market_value_.currency_ == Currency{"USD"});
}

TEST_CASE("a position in another currency is not counted either") {
  // **The case the test above could not catch.** `opening()` holds no positions, so it asked only
  // whether cash was filtered, and passed however the position loop behaved. With a holding, the
  // previous code returned the KRW position's value as USD equity -- an implicit conversion at a
  // rate of one, in the figure `Backtester` turns into the equity curve.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));

  REQUIRE(ledger.equity_in(Currency{"USD"}) == std::optional<Decimal64>(Decimal64::parse("0")));
  // And the KRW answer is unchanged: 9,300,000 of cash plus 700,000 held.
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10000000")));
}

TEST_CASE("two currencies are totalled apart, each seeing only its own positions") {
  auto ledger = opening();
  REQUIRE(ledger.apply(filled(OrderSide::BUY, "10", "70000"), krw()));
  REQUIRE(ledger.apply(filled_in(apple(), OrderSide::BUY, "5", "200"), Currency{"USD"}));

  // KRW: 9,300,000 cash plus 700,000 of Samsung. The Apple position contributes nothing.
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10000000")));
  // USD: no opening cash, so the account is 1,000 dollars short, plus 1,000 of Apple.
  REQUIRE(ledger.cash_in(Currency{"USD"}) == std::optional<Decimal64>(Decimal64::parse("-1000")));
  REQUIRE(ledger.equity_in(Currency{"USD"}) == std::optional<Decimal64>(Decimal64::parse("0")));

  // **And a positive USD figure, because the line above is zero and a filter that dropped every
  // position would also reach zero from the other side** -- it would report the -1,000 of cash
  // alone. Marking Apple up moves USD equity and must leave KRW untouched.
  ledger.mark(apple(), Decimal64::parse("300"));
  REQUIRE(ledger.equity_in(Currency{"USD"}) == std::optional<Decimal64>(Decimal64::parse("500")));
  REQUIRE(ledger.equity_in(krw()) == std::optional<Decimal64>(Decimal64::parse("10000000")));
}

TEST_CASE("a position states the currency it is denominated in, at its latest mark") {
  // `Currency` default-constructs to KRW, so an unwritten `market_value_` does not read as absent --
  // it reads as Korean won. Recording it on the fill is what lets anything sum per currency at all,
  // and it is what the runner's exposure ceiling resolves a replay position's currency from.
  auto ledger = opening();
  REQUIRE(ledger.apply(filled_in(apple(), OrderSide::BUY, "5", "200"), Currency{"USD"}));

  const auto& lot = ledger.positions().front();
  REQUIRE(lot.market_value_.currency_ == Currency{"USD"});
  REQUIRE(lot.market_value_.amount_ == Decimal64::parse("1000"));

  // A mark re-values without a trade, and keeps the currency the fill recorded.
  ledger.mark(apple(), Decimal64::parse("220"));
  REQUIRE(ledger.positions().front().market_value_.amount_ == Decimal64::parse("1100"));
  REQUIRE(ledger.positions().front().market_value_.currency_ == Currency{"USD"});
}

// ---------------------------------------------------------------------------
// Costs: stated models, never defaults that flatter
// ---------------------------------------------------------------------------

namespace {

const PriceRule& krx_grid() {
  static const PriceRule rule = PriceRule::krx_equity();
  return rule;
}

strategy::PlannedOrder order_of(
  OrderSide side,
  const char* quantity,
  OrderType type = OrderType::MARKET,
  const char* limit = nullptr
) {
  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = side;
  order.type_ = type;
  order.quantity_ = Decimal64::parse(quantity);
  if (limit != nullptr) {
    order.price_ = Decimal64::parse(limit);
  }
  order.tag_ = "test";
  return order;
}

/** A bar with a stated open, high, low and close. */
strategy::BarEvent ohlc(const char* o, const char* h, const char* l, const char* c) {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 6, 30);
  bar.open_ = Decimal64::parse(o);
  bar.high_ = Decimal64::parse(h);
  bar.low_ = Decimal64::parse(l);
  bar.close_ = Decimal64::parse(c);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);
}

/**
 * The day every fill-model case that is not about dates asks on.
 *
 * `ohlc` stamps its bar 2026-03-10, and a flat commission charges the same number on every date, so
 * the value matters only to the scheduled cases below -- which state their own dates rather than
 * relying on this one.
 */
Date fill_date() { return Date{2026, 3, 10}; }

} // namespace

TEST_CASE("commission charges a rate and honours a per-order floor") {
  const auto model = backtest::CommissionModel::of(Decimal64::parse("0.00015"));
  // Ten shares at 70,000 is 700,000, and 0.015% of that is 105.
  REQUIRE(model.on(Decimal64::parse("10"), Decimal64::parse("70000")) ==
          std::optional<Decimal64>(Decimal64::parse("105")));

  // The floor is what makes a small trade uneconomic, and it is why a notional trade tolerance exists.
  const auto floored =
    backtest::CommissionModel::of(Decimal64::parse("0.00015"), Decimal64::parse("1000"));
  REQUIRE(floored.on(Decimal64::parse("10"), Decimal64::parse("70000")) ==
          std::optional<Decimal64>(Decimal64::parse("1000")));
  // A large enough trade passes the floor and pays the rate.
  REQUIRE(floored.on(Decimal64::parse("1000"), Decimal64::parse("70000")) ==
          std::optional<Decimal64>(Decimal64::parse("10500")));
}

TEST_CASE("charging nothing is a model, so a report can say it was applied") {
  const auto model = backtest::CommissionModel::none();
  REQUIRE(model.on(Decimal64::parse("10"), Decimal64::parse("70000")) ==
          std::optional<Decimal64>(Decimal64::parse("0")));
}

TEST_CASE("slippage always moves against the order, never for it") {
  // Slippage that helped would not be slippage, and a model cutting both ways would be right on
  // average and flattering in exactly the cases a strategy trades most.
  const auto model = backtest::SlippageModel::of_basis_points(Decimal64::parse("10"));

  const auto buying = model.apply(Decimal64::parse("70000"), OrderSide::BUY, krx_grid());
  REQUIRE(buying);
  // Ten basis points of 70,000 is 70, paid.
  REQUIRE(buying->first == Decimal64::parse("70070"));
  REQUIRE(buying->second == Decimal64::parse("70"));

  const auto selling = model.apply(Decimal64::parse("70000"), OrderSide::SELL, krx_grid());
  REQUIRE(selling);
  REQUIRE(selling->first == Decimal64::parse("69930"));
  REQUIRE(selling->second == Decimal64::parse("70"));
}

TEST_CASE("a tick of slippage means a tick at that price level, not an invented step") {
  // The KRX grid is 100 won above 50,000 and 50 between 20,000 and 50,000, so one tick is a different
  // amount at each -- which is the reason this goes through the instrument's own rule.
  const auto model = backtest::SlippageModel::of_ticks(1);

  const auto high = model.apply(Decimal64::parse("70000"), OrderSide::BUY, krx_grid());
  REQUIRE(high);
  REQUIRE(high->second == Decimal64::parse("100"));

  const auto low = model.apply(Decimal64::parse("30000"), OrderSide::BUY, krx_grid());
  REQUIRE(low);
  REQUIRE(low->second == Decimal64::parse("50"));
}

TEST_CASE("an unknown grid contributes no tick slippage rather than a guessed step") {
  // The same refusal PriceRule::UNKNOWN makes everywhere else: a guessed tick is worse than none.
  const auto model = backtest::SlippageModel::of_ticks(1);
  const auto rule = PriceRule::unknown("no table for this type");
  const auto applied = model.apply(Decimal64::parse("70000"), OrderSide::BUY, rule);
  REQUIRE(applied);
  REQUIRE(applied->second == Decimal64::parse("0"));
}

// ---------------------------------------------------------------------------
// Filling, and the lookahead discipline that is structural rather than a rule
// ---------------------------------------------------------------------------

TEST_CASE("a market order fills at the next bar's open") {
  // "The next bar's open" is the same sentence for a minute bar and a daily one, which is why the rule
  // is expressed that way rather than as "the next day's open".
  const backtest::NextBarFillModel model;
  const auto decision =
    model.decide(order_of(OrderSide::BUY, "10"), ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date());

  REQUIRE(decision.execution_);
  REQUIRE(decision.execution_->fill_.price_ == Decimal64::parse("70000"));
  REQUIRE(decision.execution_->fill_.quantity_ == Decimal64::parse("10"));
  REQUIRE(decision.execution_->fill_.tag_ == "test");
  REQUIRE_FALSE(decision.still_working_);
}

TEST_CASE("a buy limit fills only when the bar's low reaches it") {
  const backtest::NextBarFillModel model;
  const auto reached = model.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "69500"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  , fill_date());
  REQUIRE(reached.execution_);
  // At the limit, not at the low: see the conservative default below.
  REQUIRE(reached.execution_->fill_.price_ == Decimal64::parse("69500"));

  // A bar that never touched the price is a bar in which the order sat unfilled, however close.
  const auto missed = model.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "68000"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  , fill_date());
  REQUIRE_FALSE(missed.execution_);
  // And it stays queued: a resting order does not expire because one bar passed it by, and discarding
  // it would silently turn every limit into a fill-or-kill.
  REQUIRE(missed.still_working_);
}

TEST_CASE("an immediate-or-cancel order does not rest when the bar misses it") {
  const backtest::NextBarFillModel model;
  // The identical limit that rests as a day order above.
  auto order = order_of(OrderSide::BUY, "10", OrderType::LIMIT, "68000");
  order.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  const auto missed =
    model.decide(order, ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date());
  REQUIRE_FALSE(missed.execution_);
  // Resting is a property of the condition, not of the basis. This read
  // `type_ == LIMIT`, which was the same answer while every order was a day
  // order: an immediate-or-cancel order that this bar did not fill would have
  // stayed queued and could fill on a later one, which is the condition the
  // caller stated being ignored.
  REQUIRE_FALSE(missed.still_working_);

  order.time_in_force_ = TimeInForce::FILL_OR_KILL;
  REQUIRE_FALSE(
    model.decide(order, ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()).still_working_
  );

  // And the day order it was cloned from still rests, so the assertion above is
  // about the condition rather than about the fixture.
  order.time_in_force_ = TimeInForce::DAY;
  REQUIRE(
    model.decide(order, ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()).still_working_
  );
}

TEST_CASE("a best-limit day order rests exactly as a plain limit does") {
  const backtest::NextBarFillModel model;
  auto order = order_of(OrderSide::BUY, "10", OrderType::BEST_LIMIT);
  order.time_in_force_ = TimeInForce::DAY;
  // Whether the engine will *accept* this order is a separate question, asked
  // in the executor test below. This pins that the fill model's resting rule
  // keys on the condition and not on the basis being exactly LIMIT.
  const auto missed =
    model.decide(order_of(OrderSide::BUY, "10", OrderType::LIMIT, "68000"),
                 ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date());
  REQUIRE(missed.still_working_);
}

TEST_CASE("a sell limit fills only when the bar's high reaches it") {
  const backtest::NextBarFillModel model;
  const auto reached = model.decide(
    order_of(OrderSide::SELL, "10", OrderType::LIMIT, "70800"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  , fill_date());
  REQUIRE(reached.execution_);
  REQUIRE(reached.execution_->fill_.price_ == Decimal64::parse("70800"));

  const auto missed = model.decide(
    order_of(OrderSide::SELL, "10", OrderType::LIMIT, "72000"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  , fill_date());
  REQUIRE_FALSE(missed.execution_);
  REQUIRE(missed.still_working_);
}

TEST_CASE("the conservative default refuses a price improvement it cannot justify") {
  // A bar opening at 69,000 through a buy limit of 70,000 would often fill better in reality, but
  // claiming it assumes the order was in the book before the auction *and* that volume existed at that
  // level -- neither of which a bar can tell us. Refusing understates a result; claiming overstates one.
  const backtest::NextBarFillModel conservative;
  const auto held = conservative.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "70000"),
    ohlc("69000", "70000", "68500", "69500"),
    krx_grid()
  , fill_date());
  REQUIRE(held.execution_);
  REQUIRE(held.execution_->fill_.price_ == Decimal64::parse("70000"));

  // The optimistic variant exists, opt-in, because that assumption is the caller's to make.
  const backtest::NextBarFillModel optimistic(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::none(),
    backtest::LimitFillPrice::AT_OPEN_IF_BETTER
  );
  const auto improved = optimistic.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "70000"),
    ohlc("69000", "70000", "68500", "69500"),
    krx_grid()
  , fill_date());
  REQUIRE(improved.execution_);
  REQUIRE(improved.execution_->fill_.price_ == Decimal64::parse("69000"));
}

TEST_CASE("a fill is stamped at the bar it filled against, not at the signal's bar") {
  const backtest::NextBarFillModel model;
  const auto next = ohlc("70000", "71000", "69000", "70500");
  const auto decision = model.decide(order_of(OrderSide::BUY, "10"), next, krx_grid(), fill_date());
  REQUIRE(decision.execution_);
  REQUIRE(decision.execution_->fill_.at_ == next.period_end_);
}

TEST_CASE("costs reach the execution, so a report can separate them from the strategy") {
  const backtest::NextBarFillModel model(
    backtest::CommissionModel::of(Decimal64::parse("0.00015")),
    backtest::SlippageModel::of_ticks(1)
  );
  const auto decision =
    model.decide(order_of(OrderSide::BUY, "10"), ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date());

  REQUIRE(decision.execution_);
  // One tick above 70,000 is 70,100, and the slippage is recorded rather than folded away.
  REQUIRE(decision.execution_->fill_.price_ == Decimal64::parse("70100"));
  REQUIRE(decision.execution_->slippage_ == Decimal64::parse("100"));
  // Commission on 701,000 at 0.015% is 105, rounded to whole won.
  REQUIRE(decision.execution_->commission_ == Decimal64::parse("105"));
}

TEST_CASE("a bar with no open falls back to its close rather than refusing to fill") {
  // A trading day on which nothing traded at the auction reports no open. Refusing to fill would
  // silently skip the bar; using the close is the only price the bar actually states.
  const backtest::NextBarFillModel model;
  auto next = ohlc("70000", "71000", "69000", "70500");
  next.bar_.open_ = std::nullopt;

  const auto decision = model.decide(order_of(OrderSide::BUY, "10"), next, krx_grid(), fill_date());
  REQUIRE(decision.execution_);
  REQUIRE(decision.execution_->fill_.price_ == Decimal64::parse("70500"));
}

TEST_CASE("the fill model states what it assumed, for a report to carry") {
  const backtest::NextBarFillModel model;
  REQUIRE(model.limit_price() == backtest::LimitFillPrice::AT_LIMIT);
  REQUIRE(backtest::limit_fill_price_name(model.limit_price()) == "at_limit");
  REQUIRE(backtest::limit_fill_price_name(backtest::LimitFillPrice::AT_OPEN_IF_BETTER) ==
          "at_open_if_better");
}

// ---------------------------------------------------------------------------
// The engine, and the discipline it exists to keep
// ---------------------------------------------------------------------------

namespace {

Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/** Builds one commission row without depending on the order of the struct's fields. */
CommissionRate rate_row(
  Country country,
  Decimal64 rate,
  std::optional<Date> from,
  std::optional<Date> until
) {
  CommissionRate row;
  row.country_ = country;
  row.rate_ = std::move(rate);
  row.from_ = std::move(from);
  row.until_ = std::move(until);
  return row;
}

backtest::ReplayConfig replay_config() {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference()};
  // Any phase, so a minute replay's pre-open and post-close bars are not refused by the guard while
  // the point of the test is the fill timing.
  config.limits_.permitted_phases_ = std::nullopt;
  return config;
}

/**
 * The same replay, with `permitted_phases_` left at its default.
 *
 * Separate from `replay_config` because that one switches the limit off and every other test here
 * wants it off -- the point of those tests is fill timing, not the guard. The two tests using this
 * one are about the default itself, which until `ai/phase-limit-exits` no replay could run: the
 * flatten a shipped vanilla produces was refused by it.
 */
backtest::ReplayConfig replay_config_with_default_phases() {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference()};
  return config;
}

/** Buys ten on the first bar it sees, and records every fill it is told about. */
struct BuysOnce {
  bool submitted_{false};
  std::vector<Decimal64> fill_prices_;
  std::vector<Decimal64> close_when_submitted_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    close_when_submitted_.push_back(bar.bar_.close_);
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("buys-once")
    );
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill& fill) {
    fill_prices_.push_back(fill.price_);
  }
};

/** Holds a fixed target, so the position moves as the ledger changes. */
struct HoldsTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    target.tag_ = "holds-ten";
    ctx.submit(target);
  }
};

/** Buys ten, holds it, then asks to be flat, so a replay has a real sale to check. */
struct BuysThenSells {
  std::size_t bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = bars_ >= 3 ? Decimal64::parse("0") : Decimal64::parse("10");
    target.tag_ = "buys-then-sells";
    ctx.submit(target);
  }
};

/** Records every phase transition it is told about. */
struct RecordsPhases {
  std::vector<strategy::MarketPhase> seen_;
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_phase(Ctx&, strategy::MarketPhase phase) { seen_.push_back(phase); }
};

/** Counts every handler it can define, which is how the audit measured the parity gap. */
struct CountsEverything {
  std::size_t bars_{0};
  std::size_t quotes_{0};
  std::size_t fills_{0};
  std::size_t updates_{0};

  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) { ++bars_; }
  template <class Ctx> void on_quote(Ctx&, const strategy::QuoteEvent&) { ++quotes_; }
  template <class Ctx> void on_fill(Ctx&, const strategy::Fill&) { ++fills_; }
  template <class Ctx> void on_order_update(Ctx&, const OrderSnapshot&) { ++updates_; }
};

/** Buys once from `on_quote`, recording what the context said while it decided. */
struct BuysOnQuote {
  bool submitted_{false};
  std::vector<Decimal64> fill_prices_;
  std::optional<Decimal64> price_when_submitted_;
  Clock::time_point now_when_submitted_{};
  std::size_t positions_when_submitted_{0};

  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_quote(Ctx& ctx, const strategy::QuoteEvent& quote) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    price_when_submitted_ = ctx.last_price(quote.quote_.instrument_);
    now_when_submitted_ = ctx.now();
    positions_when_submitted_ = ctx.portfolio().positions_.size();
    ctx.submit(
      strategy::OrderIntent::market(
        quote.quote_.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("buys-on-quote")
    );
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill& fill) {
    fill_prices_.push_back(fill.price_);
  }
};

/** Buys once from `on_quote` and counts the order updates it is told about. */
struct QuoteBuyerCountingUpdates {
  bool submitted_{false};
  std::size_t updates_{0};

  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_quote(Ctx& ctx, const strategy::QuoteEvent& quote) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    const auto reference = ctx.instrument(quote.quote_.instrument_);
    if (!reference) {
      return;
    }
    // **A limit the bars never reach**, so the order is still resting when the next bar publishes.
    // A market order placed here would fill against that same bar and be gone before the
    // observation point -- which is not this test's subject, and is the limitation slice 28 already
    // documented: an order placed and completed between two listings appears in neither.
    const auto limit =
      strategy::Price::checked(Decimal64::parse("10000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    ctx.submit(
      strategy::OrderIntent::limit(
        quote.quote_.instrument_,
        OrderSide::BUY,
        strategy::SizeSpec::quantity(Decimal64::parse("10")),
        *limit
      )
    );
  }

  template <class Ctx> void on_order_update(Ctx&, const OrderSnapshot&) { ++updates_; }
};

/** Places a resting order on its first quote, and records what it is shown working on every quote. */
struct QuoteWatchesPortfolio {
  bool submitted_{false};
  std::vector<std::size_t> working_seen_;

  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_quote(Ctx& ctx, const strategy::QuoteEvent& quote) {
    working_seen_.push_back(ctx.portfolio().working_.size());
    if (submitted_) {
      return;
    }
    const auto reference = ctx.instrument(quote.quote_.instrument_);
    if (!reference) {
      return;
    }
    const auto limit =
      strategy::Price::checked(Decimal64::parse("10000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::limit(
        quote.quote_.instrument_,
        OrderSide::BUY,
        strategy::SizeSpec::quantity(Decimal64::parse("10")),
        *limit
      )
    );
  }
};

/** Records the price and the instant it was shown when a phase change was announced. */
struct WatchesPhase {
  std::vector<std::optional<Decimal64>> price_at_phase_;
  std::vector<strategy::MarketPhase> phases_;

  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_quote(Ctx&, const strategy::QuoteEvent&) {}

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    phases_.push_back(phase);
    price_at_phase_.push_back(ctx.last_price(samsung()));
  }
};

/** Buys the instrument it is shown, once, on its first bar, and records what filled. */
struct BuysOnFirstBar {
  bool submitted_{false};
  std::vector<Decimal64> fill_prices_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      )
    );
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill& fill) { fill_prices_.push_back(fill.price_); }
};

/** A quote for the replayed instrument at `price`. */
strategy::QuoteEvent quote_at(Clock::time_point at, const char* price) {
  strategy::QuoteEvent event;
  event.quote_.instrument_ = samsung();
  event.quote_.last_price_ = Decimal64::parse(price);
  event.quote_.currency_ = Currency{"KRW"};
  event.quote_.received_at_ = at;
  event.quote_.exchange_time_ = at;
  return event;
}

/** A bar of the given interval at `at`, with a distinct open and close. */
strategy::BarEvent moving_bar(
  Clock::time_point at,
  BarInterval interval,
  const char* open,
  const char* close
) {
  Bar bar;
  bar.timestamp_ = at;
  bar.open_ = Decimal64::parse(open);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(open);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), interval, BarStamp::CLOSE, bar);
}

} // namespace

TEST_CASE("on_quote fires in a replay, which it never did before") {
  // `issues/live-parity-audit.md` finding 2, measured by its own probe: the same strategy driven
  // through both paths saw one quote live and none in a replay, because `QuoteEvent` appeared
  // nowhere in `backtest/`. A supported live cadence with no counterpart means a strategy written
  // on this handler runs against real money having been evaluated against nothing.
  backtest::Backtester<CountsEverything> engine(CountsEverything{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  CHECK(engine.strategy().bars_ == 2);
  CHECK(engine.strategy().quotes_ == 1);
  // Counted apart from the bars, because a per-bar statistic is about bars.
  CHECK(engine.result().bars_ == 2);
  CHECK(engine.result().quotes_ == 1);
}

TEST_CASE("a replayed quote sets the price and the clock, exactly as a live one does") {
  // **Parity here is structural, and this test exists because a first draft got it backwards.**
  // Reading `live::SessionSource::poll_quotes` alone says a quote only dispatches; the work is one
  // level down, in `Runner::feed(const QuoteEvent&)`, which sets `last_price` from the quote and
  // advances the decision clock to its `received_at_` -- and **both modes call that same
  // function**. So a replay that "helpfully" left the price and the clock alone would have been the
  // one diverging. The engine adds two things around it and no more: it refuses after a halt, and
  // it counts.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  const auto first_bar = utc(2026, 3, 10, 6, 30);
  const auto quote_time = utc(2026, 3, 10, 7, 0);
  engine.feed(moving_bar(first_bar, BarInterval::ONE_DAY, "69000", "70000"));
  const auto equity_after_bar = engine.result().equity_.size();
  const auto equity_before_quote = engine.ledger().equity_in(Currency{"KRW"});
  engine.feed(quote_at(quote_time, "70500"));

  // The quote's price, not the bar's close: a market order sized from `on_quote` is valued at what
  // the strategy was just shown.
  CHECK(
    engine.strategy().price_when_submitted_ == std::optional<Decimal64>(Decimal64::parse("70500"))
  );
  // And the quote's instant, which is what `RiskGuard`'s rate window measures from.
  CHECK(engine.strategy().now_when_submitted_ == quote_time);
  // What the replay does **not** do: no mark, so no equity point. That half is the engine's own,
  // because live has no ledger to disagree with -- and it is asserted on the **ledger** as well as
  // on the curve, because an empty portfolio makes a missing equity point prove very little. A mark
  // that moved the book without recording a point would pass the second check and fail the first.
  CHECK(engine.ledger().equity_in(Currency{"KRW"}) == equity_before_quote);
  CHECK(engine.result().equity_.size() == equity_after_bar);
}

TEST_CASE("an order placed from on_quote fills against the next bar, not the quote") {
  // The consequence a strategy author has to know, and the reason `ReportCaveats::quotes_` exists.
  // `NextBarFillModel` is unchanged by this slice: a quote neither fills nor marks, so the order
  // rests until a bar arrives and executes at that bar's price -- a whole bar's movement away from
  // the quote the strategy reacted to. Live, the same order reaches the broker at once.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));
  // Nothing has filled yet: a quote is not a bar.
  CHECK(engine.result().executions_.empty());

  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // The next bar's open, which is what the fill model credits a market order with -- and 9,500 away
  // from the quote that produced the order.
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("80000"));
}

TEST_CASE("the report states how many quotes a replay saw, and says so only when it saw one") {
  // Absence versus zero, on a field a reader uses to decide whether the next-bar caveat applied to
  // their run. A bars-only replay is every replay written before this slice and must not carry a
  // caveat about a mechanism it never used.
  SECTION("a replay with quotes") {
    backtest::Backtester<CountsEverything> engine(CountsEverything{}, replay_config());
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
    engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));
    engine.feed(quote_at(utc(2026, 3, 10, 7, 30), "70600"));
    engine.finish();

    const auto report = backtest::summarize(engine.result(), engine.fills());
    CHECK(report.caveats_.quotes_ == 2);
  }

  SECTION("a replay of bars alone") {
    backtest::Backtester<CountsEverything> engine(CountsEverything{}, replay_config());
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
    engine.finish();

    const auto report = backtest::summarize(engine.result(), engine.fills());
    CHECK(report.caveats_.quotes_ == 0);
  }
}

TEST_CASE("a quote fed before the first bar is accepted, counted, and not a special case") {
  // Stated rather than forbidden: live, a quote poll can and does run before the first bar arrives,
  // so refusing here would be a rule the live path does not have. And nothing about it is unset --
  // the clock and the phase both come from the quote itself, which is the whole of why it needs no
  // special handling. (An earlier version of this test fed the bar *before* the quote it was
  // supposed to precede, and asserted a clock claim that was never true.)
  backtest::Backtester<CountsEverything> engine(CountsEverything{}, replay_config());
  engine.feed(quote_at(utc(2026, 3, 10, 1, 0), "70500"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.finish();

  CHECK(engine.strategy().quotes_ == 1);
  CHECK(engine.result().quotes_ == 1);
  CHECK(engine.result().bars_ == 1);
}

TEST_CASE("a phase announced by a quote is announced after that quote's price is installed") {
  // `Runner::feed(QuoteEvent)` installs the price, and it runs *after* the announcement -- so
  // announcing first left `on_phase` deciding on the previous bar's close, which for a market order
  // is the value it is sized and guarded at. The bar path has always installed its close before
  // announcing; this is the same order.
  backtest::Backtester<WatchesPhase> engine(WatchesPhase{}, replay_config());
  // 06:30 UTC is 15:30 KST: the close, so the phase after this bar is POST_CLOSE.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  // 01:00 UTC next day is 10:00 KST, inside the session: a transition to OPEN.
  engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70500"));

  REQUIRE(engine.strategy().phases_.size() == 2);
  CHECK(engine.strategy().phases_[1] == strategy::MarketPhase::OPEN);
  // The quote's price, not the previous bar's close.
  CHECK(engine.strategy().price_at_phase_[1] == std::optional<Decimal64>(Decimal64::parse("70500")));
}

TEST_CASE("a weekly bar's period start is not derivable, and the engine says so instead of guessing") {
  // **The one shape this engine cannot check, and the third answer it took four review rounds to
  // arrive at.** A close-stamped weekly bar ends on a Friday and began on a Monday, and nothing in
  // the event says when. Three versions of this code inferred it from a previous bar -- of the
  // instrument, then of the instrument and interval -- and a reviewer falsified each in turn, because
  // `feed`'s contract requires timestamp order and nothing about stamps, anchors or overlap. So the
  // fill is allowed on the rule that governed every fill before this branch, and **counted**: the
  // report says the check did not happen rather than claiming one that did not.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 6, 6, 30), BarInterval::ONE_WEEK, "60000", "61000"));
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_WEEK, "69000", "70000"));
  // The Tuesday inside the following week.
  engine.feed(quote_at(utc(2026, 3, 17, 1, 0), "70500"));
  engine.feed(moving_bar(utc(2026, 3, 20, 6, 30), BarInterval::ONE_WEEK, "75000", "76000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // That week's own bar filled it, at that week's open, which may be a price from before the quote.
  // Unchecked, not endorsed -- and the figure below is the whole of what the engine will say.
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("75000"));
  CHECK(engine.result().unchecked_fill_causality_ > 0);

  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(report.caveats_.unchecked_fill_causality_ == engine.result().unchecked_fill_causality_);
}

TEST_CASE("a daily replay checks every fill, and says so with a zero") {
  // The other half of that figure, and the reason it is worth reading: every bar shape this library
  // produces -- intraday, open-stamped, and close-stamped daily -- states or derives its period
  // start, so the causal comparison is exact and this stays at zero. Absence versus zero again: a
  // reader seeing zero is being told the check ran on everything, not that nothing was checked.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70500"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "75000", "76000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // Deferred past the session it fell inside, and filled at the next one's open.
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("80000"));
  CHECK(engine.result().unchecked_fill_causality_ == 0);
}

TEST_CASE("an order decided at the epoch is dated, not undated") {
  // The sentinel that was also a value. A replay's instants come from its data, and 1970 is a
  // perfectly reachable one -- so an order decided then took the *undated* branch, which is the
  // branch that skips the causal check entirely rather than recording that it was skipped.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  strategy::QuoteEvent at_epoch;
  at_epoch.quote_.instrument_ = samsung();
  at_epoch.quote_.last_price_ = Decimal64::parse("70000");
  at_epoch.quote_.currency_ = Currency{"KRW"};
  at_epoch.quote_.received_at_ = Clock::time_point{};
  engine.feed(at_epoch);
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_WEEK, "60000", "61000"));

  // Dated: the weekly bar cannot check it, so the engine records that it could not -- which an
  // undated order would not have done.
  CHECK(engine.result().unchecked_fill_causality_ > 0);
}

/** An open-stamped bar of `interval` at `at`, which is how every shipped adapter stamps one. */
strategy::BarEvent open_stamped(
  Clock::time_point at,
  BarInterval interval,
  const char* open,
  const char* close
) {
  Bar bar;
  bar.timestamp_ = at;
  bar.open_ = Decimal64::parse(open);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(open);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), interval, BarStamp::OPEN, bar);
}

TEST_CASE("a nominal period end does not defer a fill by a whole month") {
  // **The defect a senior review found after eight rounds of the other one.** The causal rule
  // compares two instants and this branch had checked only one of them. `bar_period_end` adds a
  // nominal 31 days to an open-stamped monthly bar, and every shipped adapter stamps every
  // non-minute bar `OPEN` -- so February's period end lands on 5 March, an order decided on
  // February's bar looked as though it had been decided inside March's period, and it was deferred
  // to April. Bars only, no quote, in the shape the library itself fetches.
  backtest::Backtester<BuysOnFirstBar> engine(BuysOnFirstBar{}, replay_config());
  engine.feed(open_stamped(utc(2026, 2, 2, 0, 0), BarInterval::ONE_MONTH, "60000", "61000"));
  engine.feed(open_stamped(utc(2026, 3, 2, 0, 0), BarInterval::ONE_MONTH, "70000", "71000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // March's open, which is the month after the decision -- not April's.
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("70000"));
  // And the engine says it could not prove that, rather than claiming it checked.
  CHECK(engine.result().unchecked_fill_causality_ > 0);
}

TEST_CASE("an open-stamped week whose successor starts exactly a week later is checked") {
  // The other side of the same figure. Seven days after a Monday-stamped weekly bar is the next
  // Monday, so the nominal end and the next period's start coincide: the comparison proves
  // causality and nothing is counted. A shortened week would not, and would be counted -- which is
  // the honest difference between "checked" and "assumed".
  backtest::Backtester<BuysOnFirstBar> engine(BuysOnFirstBar{}, replay_config());
  engine.feed(open_stamped(utc(2026, 3, 2, 0, 0), BarInterval::ONE_WEEK, "60000", "61000"));
  engine.feed(open_stamped(utc(2026, 3, 9, 0, 0), BarInterval::ONE_WEEK, "70000", "71000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("70000"));
  CHECK(engine.result().unchecked_fill_causality_ == 0);
}

TEST_CASE("an open-stamped bar states its own period start") {
  // The case that needs no derivation at all and that the first version of this rule got wrong:
  // `BarStamp::OPEN` means the timestamp *is* the first instant of the period, while the period end
  // is 24 hours later -- so deriving the start from the period end's date put it a day late, and a
  // quote inside the day passed a check it should have failed.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  Bar first;
  first.timestamp_ = utc(2026, 3, 10, 0, 0);
  first.open_ = Decimal64::parse("69000");
  first.high_ = Decimal64::parse("70000");
  first.low_ = Decimal64::parse("69000");
  first.close_ = Decimal64::parse("70000");
  first.volume_ = Decimal64::parse("1000");
  engine.feed(strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::OPEN, first));

  // 01:00 UTC on the 11th is 10:00 KST, an hour into the period the next open-stamped bar covers.
  engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70500"));

  Bar second;
  second.timestamp_ = utc(2026, 3, 11, 0, 0);
  second.open_ = Decimal64::parse("75000");
  second.high_ = Decimal64::parse("76000");
  second.low_ = Decimal64::parse("75000");
  second.close_ = Decimal64::parse("76000");
  second.volume_ = Decimal64::parse("1000");
  engine.feed(strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::OPEN, second));

  // Its period began at 09:00 KST, an hour before the decision, so it cannot fill the order.
  CHECK(engine.result().executions_.empty());
}

TEST_CASE("an order placed from on_quote is announced in that same call") {
  // **Parity is a property of the loop, not of the function with the matching name.** A quote poll
  // publishes no order update, which is what an earlier version of this branch reasoned from -- but
  // `WORKING_ORDERS` follows `QUOTES` in `all_poll_kinds`, so live the order can be listed and
  // announced moments after the quote that placed it. Publishing here is the closer parity, and it
  // also announces an order that would otherwise fill and be gone before any later observation
  // point.
  backtest::Backtester<QuoteBuyerCountingUpdates> engine(
    QuoteBuyerCountingUpdates{}, replay_config()
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));

  // Announced by the quote that placed it, before any bar and before `finish` -- both of which
  // would also announce it, which is why this is asserted here and not at the end of the run.
  CHECK(engine.strategy().updates_ == 1);
}

TEST_CASE("an order decided part-way through a bar's period does not fill against that bar") {
  // **The mirror image of lookahead, and just as false.** `NextBarFillModel` prices a market order
  // at a bar's open. A quote arrives *inside* a period without opening a new one, so the bar-counter
  // rule alone made an order decided at 10:00 eligible for that day's bar -- whose open was 09:00.
  // The replay would have credited a price from an hour before the decision.
  backtest::Backtester<BuysOnQuote> engine(BuysOnQuote{}, replay_config());
  // Bars stamped at the KRX close, 15:30 KST.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  // 01:00 UTC on the 11th is 10:00 KST -- an hour into the session the next bar covers.
  engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70500"));

  // That session's own bar cannot fill it: its period began at 09:00, before the decision.
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "75000", "76000"));
  CHECK(engine.result().executions_.empty());

  // The session after it can, and does, at its open.
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("80000"));
}

TEST_CASE("a quote derives the phase at its own instant, which the default guard then reads") {
  // Live, `PollingLoop` re-derives the phase before each due poll, including the quote poll. A
  // replay that moved the phase only on a bar showed `on_quote` the phase as it stood at the
  // previous close -- and `RiskLimits::permitted_phases_` defaults to `{OPEN}`, so a strategy that
  // trades correctly live was refused in a replay. These two sections are the same quote at two
  // times of day, under the **default** limits rather than the permissive ones the other cases use.
  SECTION("inside the session, the order goes out") {
    backtest::Backtester<BuysOnQuote> engine(
      BuysOnQuote{}, replay_config_with_default_phases()
    );
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
    // 01:00 UTC is 10:00 KST, an hour into the session.
    engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70500"));

    CHECK(engine.result().refused_.empty());
    CHECK(engine.result().rejected_.empty());
    // One order outstanding, which is what "it went out" means to a replay.
    engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
    engine.finish();
    CHECK(engine.result().executions_.size() == 1);
  }

  SECTION("after the close, the operator's own limit refuses it") {
    backtest::Backtester<BuysOnQuote> engine(
      BuysOnQuote{}, replay_config_with_default_phases()
    );
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
    // 07:00 UTC is 16:00 KST, half an hour after the close.
    engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));
    engine.finish();

    REQUIRE(engine.result().refused_.size() == 1);
    CHECK(engine.result().refused_[0].reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED);
    CHECK(engine.result().executions_.empty());
  }
}

TEST_CASE("a quote sees what the quote before it did, not the last bar's picture") {
  // Every dispatch site in this engine installs the portfolio first, because the engine holds a
  // fresher answer than the one it last pushed. **Between two quotes is where that matters and
  // nowhere else is:** after a bar, the bar's own install and its order-update pass have already
  // refreshed the context, so a quote arriving next would see a current picture either way. An order
  // placed *from a quote* has no such pass behind it -- the next quote's install is the only thing
  // that puts it in front of the strategy.
  backtest::Backtester<QuoteWatchesPortfolio> engine(QuoteWatchesPortfolio{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(quote_at(utc(2026, 3, 10, 7, 0), "70500"));
  engine.feed(quote_at(utc(2026, 3, 10, 7, 30), "70600"));
  engine.finish();

  REQUIRE(engine.strategy().working_seen_.size() == 2);
  // Nothing working when the first quote arrived, and the order it placed working at the second.
  CHECK(engine.strategy().working_seen_[0] == 0);
  CHECK(engine.strategy().working_seen_[1] == 1);
}

TEST_CASE("the report says it cannot know which book the bars came from") {
  // A KRX-listed equity trades on two venues and KIS can report either or the consolidated tape over
  // both. Nothing in a bar carries that provenance and `summarize` is handed neither the config nor
  // the bar source, so the caveat says unverified rather than naming a tape -- naming one would be a
  // claim this code cannot support, and a wrong one would hide the very mismatch it exists to show.
  STATIC_REQUIRE(backtest::ReportCaveats::bar_tape_unverified);
}

TEST_CASE("an order is never filled from the bar that produced it") {
  // **The defect a backtester exists to prevent.** The strategy submits on the first bar, whose close
  // is 70,000. If the engine filled from that bar it would fill at 70,000 or 70,500; filling from the
  // next bar gives that bar's open, 80,000. The difference is the whole point.
  backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());

  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.strategy().close_when_submitted_.size() == 1);
  REQUIRE(engine.strategy().close_when_submitted_[0] == Decimal64::parse("70000"));
  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // The next bar's open, not anything from the signal's own bar.
  REQUIRE(engine.strategy().fill_prices_[0] == Decimal64::parse("80000"));
}

TEST_CASE("an order submitted on the last bar never fills, and is counted") {
  // There is no next bar to fill against, and inventing one would be inventing data. Counting it is how
  // a reader learns a result excluded a trade the strategy asked for.
  backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.empty());
  REQUIRE(engine.result().unfilled_ == 1);
  REQUIRE(engine.result().executions_.empty());
}

TEST_CASE("the strategy is told about its own fills, and the ledger moves") {
  backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(engine.ledger().average_price_of(samsung()) ==
          std::optional<Decimal64>(Decimal64::parse("80000")));
  // 100,000,000 less ten at 80,000.
  REQUIRE(engine.ledger().cash_in(Currency{"KRW"}) ==
          std::optional<Decimal64>(Decimal64::parse("99200000")));
}

TEST_CASE("a target already reached stops asking, because the ledger tells it so") {
  // The reason the engine must maintain the portfolio: without it, a target-stating strategy repeats its
  // target forever and the position never moves, which looks like a strategy doing nothing.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();

  // One fill, not five: once the position matched, reconciliation produced nothing.
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
}

TEST_CASE("equity is recorded per bar and the curve carries period returns") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "77000"));
  engine.finish();

  const auto& curve = engine.result().equity_;
  REQUIRE(curve.size() == 3);
  // The first point has no predecessor, so no return exists for it -- absent rather than zero.
  REQUIRE_FALSE(math::observed(curve[0].period_return_));
  REQUIRE(math::observed(curve[2].period_return_));
  // Ten shares marking up 7,000 each on a 100,000,000 book is 70,000, or 0.07%.
  REQUIRE(curve[2].period_return_ == Catch::Approx(0.0007).epsilon(0.01));
}

TEST_CASE("a guard refusal is collected rather than lost in a per-bar report") {
  auto config = replay_config();
  config.limits_.max_order_notional_ = Decimal64::parse("100000");
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));

  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  REQUIRE_FALSE(engine.result().refused_.empty());
  REQUIRE(engine.result().refused_[0].reason_ == strategy::GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  REQUIRE(engine.result().executions_.empty());
}

TEST_CASE("an instrument with no reference data is rejected, not guessed at") {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.limits_.permitted_phases_ = std::nullopt;
  // Deliberately no instruments.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));

  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();
  REQUIRE_FALSE(engine.result().rejected_.empty());
  REQUIRE(engine.result().rejected_[0].reason_ == strategy::RejectReason::NO_REFERENCE_DATA);
}

TEST_CASE("a gap between sessions is counted rather than interpolated across") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 29), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  // The next session's first minute, hours later.
  engine.feed(moving_bar(utc(2026, 3, 11, 0, 1), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.result().gaps_ == 1);
  REQUIRE(engine.result().bars_ == 3);
}

// ---------------------------------------------------------------------------
// The same strategy, the same assertions, at two intervals
// ---------------------------------------------------------------------------

TEST_CASE("the engine replays minute bars exactly as it replays daily ones") {
  // Interval-independence as a property under test rather than an intention. The same strategy, the
  // same assertions, and no engine change -- which is what makes the engine usable with a broker that
  // serves intraday history even though neither current one does easily.
  const auto run_at = [](BarInterval interval, Clock::time_point start, auto step) {
    backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());
    engine.feed(moving_bar(start, interval, "69000", "70000"));
    engine.feed(moving_bar(start + step, interval, "80000", "81000"));
    engine.finish();
    return engine;
  };

  const auto daily = run_at(BarInterval::ONE_DAY, utc(2026, 3, 10, 6, 30), std::chrono::hours(24));
  const auto minutes =
    run_at(BarInterval::ONE_MINUTE, utc(2026, 3, 10, 3, 0), std::chrono::minutes(1));

  // Identical decisions, identical fills, identical ledger.
  REQUIRE(daily.strategy().fill_prices_ == minutes.strategy().fill_prices_);
  REQUIRE(daily.strategy().fill_prices_[0] == Decimal64::parse("80000"));
  REQUIRE(daily.ledger().quantity_of(samsung()) == minutes.ledger().quantity_of(samsung()));
  REQUIRE(daily.ledger().cash_in(Currency{"KRW"}) == minutes.ledger().cash_in(Currency{"KRW"}));
  REQUIRE(daily.result().executions_.size() == minutes.result().executions_.size());
}

TEST_CASE("a minute replay sees intraday phases while a daily one does not") {
  // The one place the two intervals *should* differ, and it comes from the session window rather than
  // from any special casing in the engine.
  backtest::Backtester<RecordsPhases> intraday(RecordsPhases{}, replay_config());
  intraday.feed(moving_bar(utc(2026, 3, 9, 23, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  intraday.feed(moving_bar(utc(2026, 3, 10, 0, 1), BarInterval::ONE_MINUTE, "70000", "70000"));
  intraday.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  intraday.finish();

  const auto& seen = intraday.strategy().seen_;
  REQUIRE(seen.size() == 3);
  REQUIRE(seen[0] == strategy::MarketPhase::PRE_OPEN);
  REQUIRE(seen[1] == strategy::MarketPhase::OPEN);
  REQUIRE(seen[2] == strategy::MarketPhase::POST_CLOSE);
}

TEST_CASE("costs reach the replay's executions, so a report can separate them out") {
  backtest::Backtester<BuysOnce> engine(
    BuysOnce{},
    replay_config(),
    backtest::NextBarFillModel(
      backtest::CommissionModel::of(Decimal64::parse("0.00015")),
      backtest::SlippageModel::of_ticks(1)
    )
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.result().executions_.size() == 1);
  const auto& execution = engine.result().executions_[0];
  // One tick above 80,000 on the KRX grid is 80,100.
  REQUIRE(execution.fill_.price_ == Decimal64::parse("80100"));
  REQUIRE(execution.slippage_ == Decimal64::parse("100"));
  REQUIRE(execution.commission_.coefficient() > 0);
  // And the model still reports what it assumed.
  REQUIRE(engine.fills().limit_price() == backtest::LimitFillPrice::AT_LIMIT);
}

// ---------------------------------------------------------------------------
// The report, and the caveats that travel with the figures
// ---------------------------------------------------------------------------

#if defined(ATL_HAS_EIGEN)

TEST_CASE("a replay says whether its commission came from the broker or from the caller") {
  // Found in review: the caveat existed and nothing ever set it, so a replay seeded from a broker's
  // published schedule still reported the rate as the caller's. The provenance now travels on the
  // model, because the model is what reaches the report -- a flag the caller had to set separately
  // would be false on exactly the runs where it mattered.
  CommissionSchedule schedule;
  schedule.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.00015"), std::nullopt, std::nullopt)
  );
  const auto seeded = backtest::commission_model_from(schedule, Country::KR, Date{2026, 9, 23});
  REQUIRE(seeded);

  backtest::Backtester<HoldsTen> published(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(seeded.value())
  );
  published.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  published.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  published.finish();

  const auto from_broker = backtest::summarize(published.result(), published.fills());
  REQUIRE(from_broker.caveats_.applied_rates_.size() == 1);
  REQUIRE(from_broker.caveats_.applied_rates_.front().origin_ == CommissionOrigin::BROKER_PUBLISHED);
  REQUIRE(from_broker.caveats_.applied_rates_.front().rate_ == Decimal64::parse("0.00015"));

  // The same rate chosen by the caller reads as the caller's, which is the distinction: the figures
  // are identical and they mean different things.
  backtest::Backtester<HoldsTen> chosen(
    HoldsTen{},
    replay_config(),
    backtest::NextBarFillModel(backtest::CommissionModel::of(Decimal64::parse("0.00015")))
  );
  chosen.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  chosen.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  chosen.finish();

  const auto from_caller = backtest::summarize(chosen.result(), chosen.fills());
  REQUIRE(from_caller.caveats_.applied_rates_.size() == 1);
  REQUIRE(from_caller.caveats_.applied_rates_.front().origin_ == CommissionOrigin::CALLER_STATED);
  REQUIRE(from_caller.caveats_.applied_rates_.front().rate_ ==
          from_broker.caveats_.applied_rates_.front().rate_);
}

TEST_CASE("a report summarizes a replay and states what the model assumed") {
  backtest::Backtester<HoldsTen> engine(
    HoldsTen{},
    replay_config(),
    backtest::NextBarFillModel(
      backtest::CommissionModel::of(Decimal64::parse("0.00015"), Decimal64::parse("100")),
      backtest::SlippageModel::of_ticks(1)
    )
  );
  for (int day = 10; day < 20; ++day) {
    const auto close = std::to_string(70000 + 500 * (day - 10));
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, close.c_str(), close.c_str()));
  }
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.bars_ == 10);
  REQUIRE(report.executions_ == 1);
  REQUIRE(report.commission_paid_);
  REQUIRE(report.commission_paid_->coefficient() > 0);
  REQUIRE(report.slippage_paid_);
  REQUIRE(report.slippage_paid_->coefficient() > 0);

  // The assumptions travel with the figures, as fields rather than as documentation a reader has to
  // remember applied to their run.
  REQUIRE(report.caveats_.limit_fill_ == backtest::LimitFillPrice::AT_LIMIT);
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  REQUIRE(report.caveats_.applied_rates_.front().rate_ == Decimal64::parse("0.00015"));
  REQUIRE(report.caveats_.applied_rates_.front().minimum_ == Decimal64::parse("100"));
  REQUIRE(report.caveats_.applied_rates_.front().executions_ == 1);
  REQUIRE(report.caveats_.slippage_ticks_ == 1);
  REQUIRE(backtest::ReportCaveats::volume_ignored);
}

TEST_CASE("a zero-cost run says so rather than leaving it to be assumed") {
  // Charging nothing is a model, and a reader must be able to see that it was the one applied.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  // The zero rate is *stated*: an entry exists carrying zero, so a reader sees that nothing was
  // charged rather than an empty table that could also mean nothing is known.
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  REQUIRE(report.caveats_.applied_rates_.front().rate_.coefficient() == 0);
  REQUIRE(report.caveats_.slippage_ticks_ == 0);
  REQUIRE(report.commission_paid_);
  REQUIRE(report.commission_paid_->coefficient() == 0);
}

TEST_CASE("an annualized figure is unobserved unless a period count was supplied") {
  // The reason analytics::annualize_ratio takes the count rather than assuming: 245 for daily bars and
  // 95,550 for minute bars over the same year. Absent leaves it unobserved rather than scaled by a guess.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  for (int day = 10; day < 20; ++day) {
    const auto close = std::to_string(70000 + 500 * (day - 10));
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, close.c_str(), close.c_str()));
  }
  engine.finish();

  const auto without = backtest::summarize(engine.result(), engine.fills());
  REQUIRE_FALSE(math::observed(without.annualized_sharpe_));

  const auto periods =
    backtest::periods_per_year(BarInterval::ONE_DAY, strategy::SessionWindow::krx(), 245.0);
  REQUIRE(periods);
  const auto with = backtest::summarize(engine.result(), engine.fills(), *periods);
  if (math::observed(with.sharpe_)) {
    REQUIRE(math::observed(with.annualized_sharpe_));
    // The square root of the period count, not the count -- confusing them overstates a daily Sharpe
    // about sixteenfold.
    REQUIRE(with.annualized_sharpe_ ==
            Catch::Approx(with.sharpe_ * std::sqrt(245.0)).epsilon(1e-9));
  }
}

TEST_CASE("a replay with nothing to measure leaves every figure unobserved, not zero") {
  // Zero would read as a flat result; absent reads as an absent one, which is what it is.
  backtest::ReplayResult empty;
  const backtest::NextBarFillModel fills;
  const auto report = backtest::summarize(empty, fills);

  REQUIRE_FALSE(math::observed(report.total_return_));
  REQUIRE_FALSE(math::observed(report.sharpe_));
  REQUIRE_FALSE(math::observed(report.sortino_));
  REQUIRE_FALSE(math::observed(report.max_drawdown_));
  REQUIRE_FALSE(math::observed(report.total_turnover_));
}

TEST_CASE("a Sortino ratio over a run that never lost is undefined, not infinite") {
  // Carried straight through from analytics, and distinguishable from a warm-up by is_undefined.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  for (int day = 10; day < 20; ++day) {
    const auto close = std::to_string(70000 + 500 * (day - 10));
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, close.c_str(), close.c_str()));
  }
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  // A rising run with a single entry never fell below zero, so there is no downside to divide by.
  REQUIRE_FALSE(math::observed(report.sortino_));
  REQUIRE(math::is_undefined(report.sortino_));
}

TEST_CASE("a multi-instrument replay is flagged for survivorship bias") {
  // The engine cannot fix it -- broker reference data lists instruments that exist now, so a universe
  // assembled from it excludes everything delisted in the period. So it says so instead.
  const auto other = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  auto config = replay_config();
  auto second = samsung_reference();
  second.id_ = other;
  config.instruments_.push_back(second);

  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  // Interleaved, as a cross-sectional replay would be.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  auto bar = moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "50000", "50000");
  bar.instrument_ = other;
  engine.feed(bar);
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  auto next = moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "50000", "50000");
  next.instrument_ = other;
  engine.feed(next);
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 2);
  REQUIRE(report.caveats_.multiple_instruments_);
}

TEST_CASE("a single-instrument replay is not flagged, because the bias does not apply") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE_FALSE(report.caveats_.multiple_instruments_);
}

TEST_CASE("gaps and unfilled orders reach the report rather than staying in the engine") {
  backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 29), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 0, 1), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.caveats_.gaps_ == 1);
  // Submitted on bar one, filled against bar two, so nothing is left over here.
  REQUIRE(report.caveats_.unfilled_ == 0);
  REQUIRE(report.executions_ == 1);
}

TEST_CASE("turnover is measured from what was actually traded") {
  // From the executions rather than from a weight matrix, because a replay knows what it traded.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  // Ten at 70,000 is 700,000 against a 100,000,000 book: 0.7%.
  REQUIRE(report.total_turnover_ == Catch::Approx(0.007).epsilon(0.01));
}

#endif

// ---------------------------------------------------------------------------
// Every vanilla strategy, replayed at both intervals
// ---------------------------------------------------------------------------

namespace {

/** A price path that rises, stalls, falls and recovers, so a trend strategy has both sides. */
const std::vector<int>& path() {
  static const std::vector<int> closes = {
    60000, 60500, 61200, 61000, 62000, 63000, 64500, 65000, 66200, 67000,
    68500, 69000, 70200, 71000, 72500, 73000, 72800, 72000, 71000, 69500,
    68000, 66500, 65000, 63500, 62000, 61000, 60000, 59000, 58500, 58000,
    58500, 59500, 61000, 62500, 64000, 65500, 67000, 68500, 70000, 71500,
  };
  return closes;
}

/**
 * Replays `path()` through `strategy` at `interval` and returns the finished engine.
 *
 * The same function serves both intervals, which is what makes the pair of assertions below a test of
 * the engine rather than of two code paths.
 */
template <typename Strategy>
backtest::Backtester<Strategy> replay(Strategy strategy, BarInterval interval) {
  auto config = replay_config();
  config.series_capacity_ = 64;
  backtest::Backtester<Strategy> engine(std::move(strategy), std::move(config));

  // Minute bars start inside the session and step by a minute; daily bars step by a day. Nothing else
  // differs, and the engine is told nothing about which it is getting.
  const auto start = interval == BarInterval::ONE_MINUTE ?
    utc(2026, 3, 10, 0, 1) :
    utc(2026, 3, 10, 6, 30);
  for (std::size_t at = 0; at < path().size(); ++at) {
    const auto step = interval == BarInterval::ONE_MINUTE ?
      std::chrono::minutes(static_cast<int>(at)) :
      std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours(24 * static_cast<int>(at)));
    const auto close = std::to_string(path()[at]);
    engine.feed(moving_bar(start + step, interval, close.c_str(), close.c_str()));
  }
  engine.finish();
  return engine;
}

/**
 * A signal strategy and a liquidation policy composed, which is how the plan intended them to be used.
 *
 * At namespace scope because a local class cannot hold a template member -- a trap this file has now
 * hit five times, which is why the vignette records it.
 */
struct BuyThenLiquidate {
  vanilla::BuyAndHold buying_;
  vanilla::TimedLiquidation flattening_;
  bool flattened_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    // **`flattened_` is load-bearing, and composing these two without it does not work.**
    // `BuyAndHold::on_bar` restates a target of 100 on every bar, and `on_bar` runs after
    // `on_phase` within one `feed`. So the bar that liquidates is also a bar that restates the
    // buy, and reconciliation withdraws the flatten as an order opposing the target -- correctly,
    // because that is what the composed strategy asked for.
    //
    // This test passed without the flag only because `backtest::ReplayLedger::snapshot` reported no
    // working orders, so reconciliation could not see the flatten to withdraw it. A live run of the
    // same pair would never have gone flat. The replay was not modelling the strategy; it was
    // hiding a conflict inside it.
    if (flattened_) {
      return;
    }
    buying_.on_bar(ctx, bar);
  }
  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    flattening_.on_phase(ctx, phase);
    for (const auto allowed : flattening_.params_.hold_through_) {
      if (allowed == phase) {
        return;
      }
    }
    flattened_ = true;
  }
};

/** Asserts a strategy behaves identically at both intervals. */
template <typename Strategy>
void behaves_the_same_at_both_intervals(Strategy strategy) {
  const auto daily = replay(strategy, BarInterval::ONE_DAY);
  const auto minutes = replay(strategy, BarInterval::ONE_MINUTE);

  REQUIRE(daily.result().executions_.size() == minutes.result().executions_.size());
  REQUIRE(daily.ledger().quantity_of(samsung()) == minutes.ledger().quantity_of(samsung()));
  REQUIRE(daily.ledger().cash_in(Currency{"KRW"}) == minutes.ledger().cash_in(Currency{"KRW"}));
  REQUIRE(daily.ledger().realized_in(Currency{"KRW"}) ==
          minutes.ledger().realized_in(Currency{"KRW"}));
  // And neither ran into an instruction it could not build.
  REQUIRE(daily.result().rejected_.empty());
  REQUIRE(minutes.result().rejected_.empty());
}

} // namespace

TEST_CASE("BuyAndHold replays identically at both intervals") {
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = samsung();
  holding.params_.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(holding);

  // And it traded exactly once, which is the property it exists to demonstrate.
  const auto engine = replay(holding, BarInterval::ONE_DAY);
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("100")));
}

#if defined(ATL_HAS_EIGEN)

TEST_CASE("SmaCrossover replays identically at both intervals and trades both ways") {
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 3;
  params.slow_ = 8;
  params.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(vanilla::SmaCrossover{params});

  const auto engine = replay(vanilla::SmaCrossover{params}, BarInterval::ONE_DAY);
  // The path rises, falls and recovers, so a level-stated crossover enters, exits and re-enters.
  REQUIRE(engine.result().executions_.size() >= 3);
  REQUIRE(engine.ledger().realized_in(Currency{"KRW"}).coefficient() != 0);
}

TEST_CASE("EmaCrossover replays identically at both intervals") {
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 3;
  params.slow_ = 8;
  params.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(vanilla::EmaCrossover{params});
}

TEST_CASE("DonchianBreakout replays identically at both intervals") {
  vanilla::DonchianBreakout breaking;
  breaking.params_.instrument_ = samsung();
  breaking.params_.lookback_ = 5;
  breaking.params_.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(breaking);

  const auto engine = replay(breaking, BarInterval::ONE_DAY);
  REQUIRE(engine.result().executions_.size() >= 2);
}

TEST_CASE("ZScoreReversion replays identically at both intervals") {
  vanilla::ZScoreReversion reverting;
  reverting.params_.instrument_ = samsung();
  reverting.params_.window_ = 8;
  reverting.params_.entry_ = 1.0;
  reverting.params_.exit_ = 0.2;
  reverting.params_.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(reverting);
}

TEST_CASE("RsiThreshold replays identically at both intervals") {
  vanilla::RsiThreshold indexed;
  indexed.params_.instrument_ = samsung();
  indexed.params_.period_ = 5;
  indexed.params_.oversold_ = 35.0;
  indexed.params_.overbought_ = 65.0;
  indexed.params_.quantity_ = Decimal64::parse("100");
  behaves_the_same_at_both_intervals(indexed);
}

TEST_CASE("VolatilityTargetSizing replays identically at both intervals") {
  vanilla::VolatilityTargetSizing sizing;
  sizing.params_.instrument_ = samsung();
  sizing.params_.window_ = 8;
  sizing.params_.risk_budget_ = Decimal64::parse("200000");
  sizing.params_.max_quantity_ = Decimal64::parse("500");
  behaves_the_same_at_both_intervals(sizing);

  // It sized something finite rather than clamping to the maximum on a flat window.
  const auto engine = replay(sizing, BarInterval::ONE_DAY);
  const auto held = engine.ledger().quantity_of(samsung());
  if (held && held->coefficient() != 0) {
    REQUIRE_FALSE(decimal_less(sizing.params_.max_quantity_, *held));
  }
}

TEST_CASE("every vanilla strategy finishes a replay without an unbuildable instruction") {
  // A blunter assertion across the set: whatever each decided, reconciliation could always express it
  // given reference data and a price. A rejection here would mean the engine failed to supply
  // something the runner needs, not that a strategy misbehaved.
  vanilla::MovingAverageCrossover::Params crossing;
  crossing.instrument_ = samsung();
  crossing.fast_ = 3;
  crossing.slow_ = 8;
  crossing.quantity_ = Decimal64::parse("100");

  REQUIRE(replay(vanilla::SmaCrossover{crossing}, BarInterval::ONE_DAY).result().rejected_.empty());
  REQUIRE(replay(vanilla::EmaCrossover{crossing}, BarInterval::ONE_MINUTE).result().rejected_.empty());
}

#endif

TEST_CASE("TimedLiquidation flattens a position when a replay leaves the session") {
  // Only meaningful at an interval that crosses a session boundary, which is the clearest case for why
  // intraday replay had to work rather than be deferred.
  auto config = replay_config();
  config.limits_.permitted_phases_ =
    std::vector<strategy::MarketPhase>{strategy::MarketPhase::OPEN,
                                      strategy::MarketPhase::POST_CLOSE};

  BuyThenLiquidate combined;
  combined.buying_.params_.instrument_ = samsung();
  combined.buying_.params_.quantity_ = Decimal64::parse("100");

  backtest::Backtester<BuyThenLiquidate> engine(combined, std::move(config));
  // Inside the session, then past the close.
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 1), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 31), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  // Bought inside the session and sold back out after the close, so it ends flat.
  REQUIRE(engine.result().executions_.size() == 2);
  REQUIRE(engine.result().executions_[1].fill_.side_ == OrderSide::SELL);
  REQUIRE(engine.ledger().quantity_of(samsung())->coefficient() == 0);
}

TEST_CASE("TimedLiquidation flattens a replay under the default limits") {
  // **The acceptance criterion of `ai/phase-limit-exits`, and the first place in this repository
  // that runs the default `permitted_phases_` where the phase is real.** Every other replay either
  // switches the limit off or widens it -- including the test above, which has to add `POST_CLOSE`
  // for this same vanilla to place anything at all. That was the evidence the default was not
  // usable: a shipped strategy could not do its job without the operator disabling a risk limit.
  //
  // Nothing here sets `permitted_phases_`. The flatten is a sell of the whole holding, so it closes
  // a position and the phase limit does not stand in front of it; the buy inside the session is
  // permitted because the session is open. Both halves of the default are doing their job.
  auto config = replay_config_with_default_phases();
  // The default, spelled out: trading only while the market is open.
  REQUIRE(config.limits_.permitted_phases_);
  REQUIRE(config.limits_.permitted_phases_->size() == 1);
  REQUIRE(config.limits_.permitted_phases_->front() == strategy::MarketPhase::OPEN);

  BuyThenLiquidate combined;
  combined.buying_.params_.instrument_ = samsung();
  combined.buying_.params_.quantity_ = Decimal64::parse("100");

  backtest::Backtester<BuyThenLiquidate> engine(combined, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 1), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 31), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.result().refused_.empty());
  REQUIRE(engine.result().executions_.size() == 2);
  REQUIRE(engine.result().executions_[1].fill_.side_ == OrderSide::SELL);
  REQUIRE(engine.ledger().quantity_of(samsung())->coefficient() == 0);
}

TEST_CASE("a replay still refuses an opening order after the close") {
  // The other half of the default, in the same engine. If the exception were wider than "closes a
  // position" the test above would pass for the wrong reason, and a replay would quietly permit
  // trading outside the session -- which is the whole thing `permitted_phases_` is for.
  auto config = replay_config_with_default_phases();

  backtest::Backtester<vanilla::BuyAndHold> engine(
    vanilla::BuyAndHold{{samsung(), Decimal64::parse("100")}}, std::move(config)
  );
  // Past the close on its very first bar, so the only order it ever plans is an opening buy there.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 31), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.result().executions_.empty());
  REQUIRE_FALSE(engine.result().refused_.empty());
  REQUIRE(engine.result().refused_[0].reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED);
}

TEST_CASE("a replay sells what it holds with the sellable check enabled") {
  // The check refuses a sale the broker has not said is sellable, and a replay has no broker. Until
  // the ledger answered, every sale a replay planned was refused -- so the check could not be run
  // over a replay at all, and the acceptance criterion that claimed it could was not reachable.
  auto config = replay_config();
  config.limits_.enforce_sellable_quantity_ = true;

  backtest::Backtester<BuysThenSells> engine(BuysThenSells{}, config);
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();

  // Bought ten, then sold ten, and nothing was refused on the way.
  REQUIRE(engine.result().refused_.empty());
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("0")));
  REQUIRE(engine.result().executions_.size() == 2);
  REQUIRE(engine.result().executions_[1].fill_.side_ == OrderSide::SELL);
}

TEST_CASE("a replay reports every held share as sellable, and says so") {
  // The modelling assumption itself, pinned where a reader of the ledger would look for it: the
  // snapshot answers the question the guard asks, and answers it with the whole holding.
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto portfolio =
    engine.ledger().template snapshot<backtest::BacktestTag>(utc(2026, 3, 11, 6, 30));
  REQUIRE(portfolio.positions_.size() == 1);
  REQUIRE(portfolio.positions_[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(portfolio.sellable_quantity_of(samsung()) == Decimal64::parse("10"));

#if defined(ATL_HAS_EIGEN)
  // And the report carries the assumption, so a reader who never opens the ledger still learns it.
  // The report needs the analytics backend, so a build without one states the model nowhere -- the
  // ledger still models it, which is why the assumption is pinned above as well as here.
  REQUIRE(backtest::ReportCaveats::sellable_is_whole_holding);
#endif
}

TEST_CASE("a replay buys within its modelled buying power and is refused above it") {
  // The check reads `buying_power_`, which live comes from the broker and in a replay comes from
  // this ledger's own cash. Without the ledger answering, every buy a replay plans is refused.
  auto config = replay_config();
  config.limits_.enforce_buying_power_ = true;

  // Ten at 70,000 is 700,000, so a million funds the first buy and nothing after it.
  config.cash_ = Decimal64::parse("1000000");

  backtest::Backtester<HoldsTen> engine(HoldsTen{}, config);
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.result().refused_.empty());
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));

  // The same strategy against a budget that cannot afford it is refused, by the broker's answer
  // rather than by a ceiling the caller configured.
  auto poor = replay_config();
  poor.limits_.enforce_buying_power_ = true;
  poor.cash_ = Decimal64::parse("100000");

  backtest::Backtester<HoldsTen> broke(HoldsTen{}, poor);
  broke.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  broke.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  broke.finish();

  REQUIRE_FALSE(broke.result().refused_.empty());
  REQUIRE(
    broke.result().refused_[0].reason_ == strategy::GuardReason::INSUFFICIENT_BUYING_POWER
  );
  REQUIRE(broke.result().executions_.empty());

#if defined(ATL_HAS_EIGEN)
  // And the report says the figure was modelled, so nobody reads a replay's pass as a broker's.
  REQUIRE(backtest::ReportCaveats::buying_power_is_ledger_cash);
#endif
}

TEST_CASE("a replay's buying power is its cash, stamped with the snapshot time") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto when = utc(2026, 3, 10, 6, 30);
  const auto portfolio = engine.ledger().template snapshot<backtest::BacktestTag>(when);
  REQUIRE(portfolio.buying_power_.size() == 1);
  REQUIRE(portfolio.buying_power_in(Currency{"KRW"}) == portfolio.cash_in(Currency{"KRW"}));
  // The time the modelled figure was read, which is all a replay can honestly stamp it with.
  REQUIRE(portfolio.buying_power_[0].received_at_ == when);
}

TEST_CASE("a cost model seeded from a broker's schedule charges what the broker charges") {
  CommissionSchedule schedule;
  schedule.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.00015"), Date{2026, 1, 1}, std::nullopt)
  );
  schedule.rates_.push_back(
    rate_row(Country::US, Decimal64::parse("0.001"), std::nullopt, Date{2026, 6, 30})
  );

  const auto domestic = backtest::commission_model_from(schedule, Country::KR, Date{2026, 9, 23});
  REQUIRE(domestic);
  REQUIRE(domestic.value().rate_ == Decimal64::parse("0.00015"));
  // No floor is invented: Toss publishes a rate and nothing else, and an unstated floor is unknown
  // rather than zero -- which this documents rather than pretends to model.
  REQUIRE(domestic.value().minimum_ == Decimal64::parse("0"));

  // Ten million won at 0.00015 is 1,500, rounded to whole currency units.
  const auto charged =
    domestic.value().on(Decimal64::parse("100"), Decimal64::parse("100000"));
  REQUIRE(charged);
  REQUIRE(*charged == Decimal64::parse("1500"));

  // A date outside every window is an error, not a free trade: a zero-cost run is the most
  // flattering wrong answer a backtest can give.
  const auto before = backtest::commission_model_from(schedule, Country::KR, Date{2025, 12, 31});
  REQUIRE_FALSE(before);
  REQUIRE(before.error().category_ == ErrorCategory::INVALID_ARGUMENT);

  const auto expired = backtest::commission_model_from(schedule, Country::US, Date{2026, 9, 23});
  REQUIRE_FALSE(expired);

  // And a market the schedule never mentions is refused rather than defaulted to another's rate.
  const CommissionSchedule empty;
  REQUIRE_FALSE(backtest::commission_model_from(empty, Country::KR, Date{2026, 9, 23}));
}

TEST_CASE("a rate whose window could not be read is refused as unknown, not as missing") {
  CommissionSchedule schedule;
  auto damaged = rate_row(Country::KR, Decimal64::parse("0.00015"), Date{2024, 1, 1}, std::nullopt);
  damaged.until_unreadable_ = true;
  schedule.rates_.push_back(damaged);

  // Before the two were held apart, this row read as open-ended and was charged: an expired rate
  // billed for years because the bound that ended it arrived unreadable.
  const auto unknown = backtest::commission_model_from(schedule, Country::KR, Date{2026, 9, 23});
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK_FALSE(unknown.error().reached_broker_);
  // The code is its own, because the two silences want different things of an operator: a missing
  // row wants a rate stated or a wider schedule, and an unreadable window wants the schedule
  // refetched. One code for both sends half of them hunting for a row that is sitting right there.
  CHECK(unknown.error().broker_code_ == "rate_bounds_unreadable");

  // A date the readable bound already excludes is a plain missing row, not an unknown one.
  const auto before = backtest::commission_model_from(schedule, Country::KR, Date{2023, 12, 31});
  REQUIRE_FALSE(before);
  CHECK(before.error().broker_code_ == "no_rate_for_date");

  // And a whole schedule of readable rows is unchanged.
  CommissionSchedule sound;
  sound.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.00015"), std::nullopt, std::nullopt)
  );
  REQUIRE(backtest::commission_model_from(sound, Country::KR, Date{2026, 9, 23}));
}

// ---------------------------------------------------------------------------
// Order identity in a replay: what rests, and which one a withdrawal names
// ---------------------------------------------------------------------------

namespace {

using ReplayExecutor = backtest::QueueingExecutor<backtest::BacktestTag>;

/** A resting buy of `quantity` at `limit`, as the executor would be handed one. */
strategy::PlannedOrder resting_buy(const char* quantity, const char* limit) {
  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = Decimal64::parse(quantity);
  order.price_ = Decimal64::parse(limit);
  order.tag_ = "resting";
  return order;
}

/** The withdrawal a runner sends for `ref`, which is all `cancel` reads. */
WorkingOrder<backtest::BacktestTag> withdrawal_of(const OrderRef<backtest::BacktestTag>& ref) {
  WorkingOrder<backtest::BacktestTag> working;
  working.ref_ = ref;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  return working;
}

} // namespace

TEST_CASE("the replay refuses an order shape it cannot price instead of inventing one") {
  ReplayExecutor executor;

  SECTION("a basis a bar cannot price is refused, not filled at the open") {
    for (const auto type :
         {OrderType::BEST_LIMIT, OrderType::PRIMARY_LIMIT, OrderType::MID_PRICE,
          OrderType::CONDITIONAL_LIMIT, OrderType::STOP_LIMIT, OrderType::CLOSING_PRICE}) {
      auto order = resting_buy("10", "60000");
      order.type_ = type;
      INFO("basis " << static_cast<int>(type));
      // `reference_price` returns the bar's open for every basis that is not
      // LIMIT, so before this each of these filled at the open -- a price the
      // order would never have had. A backtest that invents a fill price is
      // worse than one that declines to model the order.
      REQUIRE(executor.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
      REQUIRE(executor.queued().empty());
    }
  }

  SECTION("a session a bar series does not represent is refused") {
    for (const auto session :
         {OrderSession::PRE_MARKET, OrderSession::AFTER_HOURS, OrderSession::AFTER_MARKET,
          OrderSession::AFTER_HOURS_AUCTION, OrderSession::GTP, OrderSession::OPENING_AUCTION,
          OrderSession::CLOSING_AUCTION}) {
      auto order = resting_buy("10", "60000");
      order.session_ = session;
      REQUIRE(executor.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
    }
    REQUIRE(executor.queued().empty());
  }

  SECTION("a trigger on a shape it can price is refused, not ignored") {
    // Priced as the plain limit it would otherwise be, the trigger would simply vanish.
    auto order = resting_buy("10", "60000");
    order.trigger_price_ = Decimal64::parse("59500");
    REQUIRE(executor.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
    REQUIRE(executor.queued().empty());
  }

  SECTION("the shapes it can price are still accepted") {
    REQUIRE(executor.place(resting_buy("10", "60000")) == runner::SendOutcome::ACCEPTED);
    auto market = resting_buy("10", "60000");
    market.type_ = OrderType::MARKET;
    market.price_.reset();
    REQUIRE(executor.place(market) == runner::SendOutcome::ACCEPTED);
    // An immediate-or-cancel limit is priceable; only its resting differs.
    auto immediate = resting_buy("10", "60000");
    immediate.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
    REQUIRE(executor.place(immediate) == runner::SendOutcome::ACCEPTED);
    REQUIRE(executor.queued().size() == 3);
  }
}

TEST_CASE("a withdrawal removes the order it names, not one that merely looks like it") {
  // **Two resting buys of one instrument are two orders, and attributes cannot tell them apart.**
  // Matching on instrument and side withdraws whichever is first and reports success, so a strategy
  // cancelling the second silently loses the first and is told its request was honoured.
  ReplayExecutor executor;
  REQUIRE(executor.place(resting_buy("10", "60000")) == runner::SendOutcome::ACCEPTED);
  const auto first = *executor.last_ref();
  REQUIRE(executor.place(resting_buy("20", "61000")) == runner::SendOutcome::ACCEPTED);
  const auto second = *executor.last_ref();

  REQUIRE_FALSE(first == second);
  REQUIRE(executor.queued().size() == 2);

  REQUIRE(executor.cancel(withdrawal_of(second)) == runner::SendOutcome::ACCEPTED);
  REQUIRE(executor.queued().size() == 1);
  // The one that survived is the one that was not named.
  REQUIRE(executor.queued()[0].ref_ == first);
  REQUIRE(executor.queued()[0].order_.quantity_ == Decimal64::parse("10"));
}

TEST_CASE("a withdrawal for an order that is not there is refused, not accepted") {
  // `ACCEPTED` means the broker took the request, and `Runner` reads it as a withdrawal that went
  // out. An order that already filled is gone, so claiming to have withdrawn it would report a
  // withdrawal that never happened -- which is what `rejected_cancel_` exists to distinguish.
  ReplayExecutor executor;
  executor.place(resting_buy("10", "60000"));
  const auto ref = *executor.last_ref();

  REQUIRE(executor.cancel(withdrawal_of(ref)) == runner::SendOutcome::ACCEPTED);
  REQUIRE(executor.cancel(withdrawal_of(ref)) == runner::SendOutcome::REJECTED);
  REQUIRE(executor.queued().empty());
}

TEST_CASE("an order accepted during a bar is not eligible to fill against that bar") {
  // The no-lookahead rule, as a property of the order rather than of which list it is in. `open_bar`
  // is what moves the boundary, so an order placed after it carries the new bar's stamp.
  ReplayExecutor executor;
  executor.open_bar();
  executor.place(resting_buy("10", "60000"));
  const auto during_first = executor.queued()[0].accepted_on_;
  REQUIRE(during_first == executor.bar());

  executor.open_bar();
  REQUIRE(during_first < executor.bar());
  executor.place(resting_buy("10", "60000"));
  REQUIRE(executor.queued()[1].accepted_on_ == executor.bar());
}

namespace {

/** Rests one far-from-market limit buy on its first bar, and never touches it again. */
struct RestsOneLimit {
  bool placed_{false};
  std::vector<std::size_t> working_seen_;
  std::vector<std::string> refs_seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    working_seen_.push_back(ctx.portfolio().working_.size());
    for (const auto& working : ctx.portfolio().working_) {
      refs_seen_.push_back(working.ref_.primary_);
    }
    if (placed_) {
      return;
    }
    placed_ = true;
    const auto limit = strategy::Price::checked(Decimal64::parse("50000"), krx_grid());
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        *limit
      ).with_tag("rests")
    );
  }
};

} // namespace

TEST_CASE("an order resting from an earlier bar is in the portfolio the strategy decides on") {
  // The defect this branch exists for. The order is placed on bar 1, rests through bars 2 and 3
  // because the market never reaches 50,000, and was invisible on both -- so reconciliation, the
  // position and exposure ceilings and the sellable deduction all read an empty field in a replay
  // and a full one live.
  backtest::Backtester<RestsOneLimit> engine(RestsOneLimit{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto& seen = engine.strategy().working_seen_;
  REQUIRE(seen.size() == 3);
  // Nothing outstanding on the bar it was placed on -- the portfolio is installed before `on_bar`.
  REQUIRE(seen[0] == 0);
  // And outstanding on the next bar **and the one after**, not merely on the first miss.
  REQUIRE(seen[1] == 1);
  REQUIRE(seen[2] == 1);

  // The same order throughout, which is what makes it one resting order rather than a new one each
  // bar: reconciliation matches on the reference, and a reference that moved would make a single
  // resting order look like a fresh order every bar.
  REQUIRE(engine.strategy().refs_seen_.size() == 2);
  REQUIRE(engine.strategy().refs_seen_[0] == engine.strategy().refs_seen_[1]);
  REQUIRE_FALSE(engine.strategy().refs_seen_[0].empty());

  // It never filled, so it is still outstanding at the end.
  REQUIRE(engine.result().unfilled_ == 1);
  REQUIRE(engine.result().executions_.empty());
}

namespace {

/** Buys ten once, and records how much was outstanding when its own fill arrived. */
struct WatchesItsOwnFill {
  bool submitted_{false};
  std::size_t working_at_fill_{99};
  std::size_t fills_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("watches")
    );
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const strategy::Fill&) {
    ++fills_;
    working_at_fill_ = ctx.portfolio().working_.size();
  }
};

} // namespace

TEST_CASE("an order the bar fills is gone from the portfolio that bar installs") {
  // A filled order is removed before the ledger moves and before anything is shown a portfolio, so
  // no view ever carries it as still working. Without that a strategy would see its own filled order
  // resting in the very callback announcing the fill, and size the next one against it.
  backtest::Backtester<WatchesItsOwnFill> engine(WatchesItsOwnFill{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "71000"));
  engine.finish();

  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.strategy().fills_ == 1);
  // The order that just filled is not in the portfolio its own fill callback reads.
  REQUIRE(engine.strategy().working_at_fill_ == 0);
  REQUIRE(engine.result().unfilled_ == 0);
}

// ---------------------------------------------------------------------------
// The acceptance slice: the guards that read `working_` now fire in a replay
//
// Every case here is a rule the library already enforces live. Each one fails
// against the build before `Backtester` filled the field in.
// ---------------------------------------------------------------------------

namespace {

/** A limit that the market in these tests never reaches, so an order at it rests. */
strategy::Price far_below() {
  return *strategy::Price::checked(Decimal64::parse("50000"), krx_grid());
}

/** A limit above the market, so a buy at it fills and a sell at it rests. */
strategy::Price far_above() {
  return *strategy::Price::checked(Decimal64::parse("90000"), krx_grid());
}

/** Restates one limit target every bar, which is the ordinary shape reconciliation exists for. */
struct RestatesLimitTarget {
  Decimal64 quantity_{Decimal64::parse("10")};
  strategy::Price limit_{far_below()};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = quantity_;
    target.limit_ = limit_;
    target.tag_ = "restates";
    ctx.submit(target);
  }
};

/** Rests a sell above the market, then restates a long target that opposes it. */
struct SellsHighThenBuysBack {
  std::size_t bars_{0};
  /** The sides outstanding at the start of each bar, so a test can say which order survived. */
  std::vector<std::vector<OrderSide>> sides_seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    std::vector<OrderSide> sides;
    for (const auto& working : ctx.portfolio().working_) {
      sides.push_back(working.order_.side_);
    }
    sides_seen_.push_back(std::move(sides));
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    // A resting sell first, then a target that wants the position back.
    target.target_quantity_ = bars_ == 1 ? Decimal64::parse("-10") : Decimal64::parse("10");
    target.limit_ = bars_ == 1 ? far_above() : far_below();
    target.tag_ = "opposes";
    ctx.submit(target);
  }
};

/** Places one resting limit order per bar, never restating a target. */
struct AddsARestingOrderEveryBar {
  OrderSide side_{OrderSide::BUY};
  Decimal64 quantity_{Decimal64::parse("10")};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, side_, strategy::SizeSpec::quantity(quantity_),
        side_ == OrderSide::BUY ? far_below() : far_above()
      ).with_tag("adds")
    );
  }
};

/**
 * Buys ten at market, then rests a sell of ten above the market on every later bar.
 *
 * The holding makes the sellable figure ten, so the first resting sell uses it up and every later
 * one has to be refused for the quantity already committed rather than for an empty position.
 */
struct BuysThenRestsSells {
  std::size_t bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    if (bars_ == 1) {
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
        ).with_tag("stock-up")
      );
      return;
    }
    if (bars_ == 2) {
      // The buy fills against this bar; leave the position alone for one bar before selling.
      return;
    }
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        far_above()
      ).with_tag("rests-a-sell")
    );
  }
};

/** A flat bar for `instrument` at `close`, for interleaving two instruments in one replay. */
strategy::BarEvent bar_for(InstrumentId instrument, Clock::time_point at, const char* close) {
  Bar bar;
  bar.timestamp_ = at;
  bar.open_ = Decimal64::parse(close);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(
    std::move(instrument), BarInterval::ONE_DAY, BarStamp::CLOSE, bar
  );
}

/** A second KRX instrument, so a replay can interleave two of them. */
InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

Instrument hynix_reference() {
  auto reference = samsung_reference();
  reference.id_ = hynix();
  return reference;
}

/** Rests one limit buy in each of two instruments, on the bar it first sees each. */
struct RestsABuyInEachInstrument {
  std::vector<InstrumentId> placed_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    for (const auto& already : placed_) {
      if (already == bar.instrument_) {
        return;
      }
    }
    placed_.push_back(bar.instrument_);
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        far_below()
      ).with_tag("one-each")
    );
  }
};

/** Rests a limit buy every bar, recording the buying power it was shown before placing it. */
struct RecordsBuyingPower {
  std::vector<std::optional<Decimal64>> seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    seen_.push_back(ctx.portfolio().buying_power_in(Currency{"KRW"}));
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        far_below()
      ).with_tag("records")
    );
  }
};

/** Counts how many times a guard refused for `reason`. */
std::size_t refusals_for(const backtest::ReplayResult& result, strategy::GuardReason reason) {
  std::size_t count = 0;
  for (const auto& refusal : result.refused_) {
    if (refusal.reason_ == reason) {
      ++count;
    }
  }
  return count;
}

/** Replays `bars` flat bars at 70,000 through `engine`. */
template <typename Engine>
void replay_flat_bars(Engine& engine, int bars) {
  for (int day = 0; day < bars; ++day) {
    engine.feed(
      moving_bar(utc(2026, 3, 10 + day, 6, 30), BarInterval::ONE_DAY, "70000", "70000")
    );
  }
  engine.finish();
}

} // namespace

TEST_CASE("reconciliation nets a restated target against the order already resting") {
  // The ordinary shape: a strategy states the same target every bar. Live it nets against its own
  // resting order and sends nothing more. In a replay it saw no resting order, so it re-sent the
  // whole target every bar -- three bars, three orders, thirty shares working for a target of ten.
  backtest::Backtester<RestatesLimitTarget> engine(RestatesLimitTarget{}, replay_config());
  replay_flat_bars(engine, 3);

  REQUIRE(engine.result().executions_.empty());
  REQUIRE(engine.result().unfilled_ == 1);
}

TEST_CASE("reconciliation withdraws a resting order that opposes the new target") {
  // Step 2 of `reconcile`: an order working the *wrong way* is withdrawn however small the drift,
  // because leaving it resting works against the target rather than merely failing to reach it.
  // With no rows there was nothing to oppose, so a replay left the sell resting while it bought.
  backtest::Backtester<SellsHighThenBuysBack> engine(SellsHighThenBuysBack{}, replay_config());
  replay_flat_bars(engine, 3);

  const auto& seen = engine.strategy().sides_seen_;
  REQUIRE(seen.size() == 3);
  // Bar 1 starts with nothing; bar 2 starts with the sell that bar 1 rested.
  REQUIRE(seen[0].empty());
  REQUIRE(seen[1] == std::vector<OrderSide>{OrderSide::SELL});
  // Bar 3 starts with the buy alone: the opposing sell was withdrawn on bar 2, rather than left
  // resting while the replay bought against it.
  REQUIRE(seen[2] == std::vector<OrderSide>{OrderSide::BUY});

  REQUIRE(engine.result().unfilled_ == 1);
  REQUIRE(engine.result().executions_.empty());
}

TEST_CASE("the sellable check counts the sell already resting") {
  // `ai/pre-trade-checks` refuses a sell whose quantity plus what is already resting on that side
  // exceeds what the broker says is sellable. Ten are held and a replay reports all ten sellable,
  // so the first resting sell of ten is allowed and the second must be refused -- and was not,
  // because `working_quantity` summed an empty list.
  //
  // The position is real, which is the point: a strategy with nothing held is refused by this check
  // whatever the resting quantity is, so it would pass this test without the fix and prove nothing.
  auto config = replay_config();
  config.limits_.enforce_sellable_quantity_ = true;

  backtest::Backtester<BuysThenRestsSells> engine(BuysThenRestsSells{}, std::move(config));
  replay_flat_bars(engine, 5);

  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::QUANTITY_NOT_SELLABLE) > 0);
}

TEST_CASE("the position ceiling counts what is resting, not only what is held") {
  // `max_position_quantity_` measures the position that would exist if the order and everything
  // outstanding filled. Ten a bar against a ceiling of fifteen breaches on the second bar; with no
  // rows the projection reduced to the held quantity, which is zero until something fills.
  auto config = replay_config();
  config.limits_.max_position_quantity_ = Decimal64::parse("15");

  backtest::Backtester<AddsARestingOrderEveryBar> engine(
    AddsARestingOrderEveryBar{}, std::move(config)
  );
  replay_flat_bars(engine, 3);

  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::POSITION_LIMIT_EXCEEDED) > 0);
}

TEST_CASE("the exposure ceiling counts a resting buy") {
  // `ai/guard-inputs` merged the projection max(|held + working_buys|, |held - working_sells|). In a
  // replay both working sums were empty on every bar, so the ceiling it exists to make real reduced
  // to |held| -- the defect that branch closed live, still open here.
  auto config = replay_config();
  // Ten at 70,000 is 700,000 a bar, so a ceiling of one million is breached by the second.
  config.limits_.max_gross_exposure_ = Decimal64::parse("1000000");

  backtest::Backtester<AddsARestingOrderEveryBar> engine(
    AddsARestingOrderEveryBar{}, std::move(config)
  );
  replay_flat_bars(engine, 3);

  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::GROSS_EXPOSURE_EXCEEDED) > 0);
}

/** Buys hynix from samsung's bar, so the decision is made inside another instrument's period. */
struct BuysTheOtherName {
  bool submitted_{false};
  std::vector<Decimal64> fill_prices_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_ || !(bar.instrument_ == samsung())) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        hynix(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      )
    );
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill& fill) { fill_prices_.push_back(fill.price_); }
};

TEST_CASE("an order for another instrument waits for a bar that began after it was decided") {
  // **The one existing behaviour the causal fill rule changes, and it changes it for the better.**
  // A strategy deciding on samsung's 15:30 close and ordering hynix used to fill against hynix's
  // *same-day* bar -- whose open was 09:00, six and a half hours before the decision. The bar
  // counter could not see it, because the two bars are different bars of the same replay. Only a
  // single well-formed series was ever safe.
  auto config = replay_config();
  config.instruments_ = {samsung_reference(), hynix_reference()};

  backtest::Backtester<BuysTheOtherName> engine(BuysTheOtherName{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(bar_for(hynix(), utc(2026, 3, 10, 6, 31), "50000"));
  // Same day, and its period began at 09:00: it cannot fill an order decided at 15:30.
  CHECK(engine.result().executions_.empty());

  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "71000"));
  engine.feed(bar_for(hynix(), utc(2026, 3, 11, 6, 31), "60000"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // The next day's hynix bar, at its open.
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("60000"));
}

TEST_CASE("buying power is spent by an order that is still resting") {
  // **Rows alone do not close this one.** `enforce_buying_power_` reads the scalar, and
  // `ReplayLedger::snapshot` builds it from ledger cash that has not paid for anything outstanding.
  // Each resting buy is ten at its limit of 50,000, so 500,000 of cash. Against 900,000 the first
  // fits and the second cannot, and the second passed anyway because the first had spent nothing
  // the snapshot knew about.
  //
  // **Two different instruments**, which is the case a per-instrument check cannot catch: nothing
  // here is a position ceiling or a same-instrument projection, only shared money.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("900000");
  config.instruments_ = {samsung_reference(), hynix_reference()};
  config.limits_.enforce_buying_power_ = true;

  backtest::Backtester<RestsABuyInEachInstrument> engine(
    RestsABuyInEachInstrument{}, std::move(config)
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(bar_for(hynix(), utc(2026, 3, 10, 6, 31), "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(bar_for(hynix(), utc(2026, 3, 11, 6, 31), "70000"));
  engine.finish();

  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::INSUFFICIENT_BUYING_POWER) > 0);
  // The samsung buy is the one that got the money; the hynix one is outstanding only if it passed.
  REQUIRE(engine.result().unfilled_ == 1);
}

TEST_CASE("an outstanding buy moves buying power only when the check is on") {
  // `buying_power_in()` is a figure a strategy may size its next order from, so the commitment is
  // scoped to the limit exactly as `Runner::reserve` scopes its own (`runner/runner.hpp:700-705`).
  // Debiting it for a caller who never enabled the check would change how their strategy sizes, and
  // would make `ReportCaveats::buying_power_is_ledger_cash` untrue for every replay that has an
  // order resting.
  const auto million = std::optional<Decimal64>(Decimal64::parse("1000000"));

  SECTION("off: the figure stays the ledger's cash") {
    auto config = replay_config();
    config.cash_ = Decimal64::parse("1000000");
    REQUIRE_FALSE(config.limits_.enforce_buying_power_);

    backtest::Backtester<RecordsBuyingPower> engine(RecordsBuyingPower{}, std::move(config));
    replay_flat_bars(engine, 3);

    // Three bars, and a resting buy of 500,000 outstanding for the last two of them.
    REQUIRE(engine.strategy().seen_.size() == 3);
    REQUIRE(engine.strategy().seen_[0] == million);
    REQUIRE(engine.strategy().seen_[1] == million);
    REQUIRE(engine.strategy().seen_[2] == million);
    REQUIRE(engine.result().unfilled_ == 3);
  }

  SECTION("on: the figure is cash less what is committed") {
    auto config = replay_config();
    config.cash_ = Decimal64::parse("1000000");
    config.limits_.enforce_buying_power_ = true;

    backtest::Backtester<RecordsBuyingPower> engine(RecordsBuyingPower{}, std::move(config));
    replay_flat_bars(engine, 3);

    REQUIRE(engine.strategy().seen_.size() == 3);
    REQUIRE(engine.strategy().seen_[0] == million);
    // Ten at the limit of 50,000 is 500,000, committed by the order resting from the first bar.
    REQUIRE(engine.strategy().seen_[1] == std::optional<Decimal64>(Decimal64::parse("500000")));
    REQUIRE(engine.strategy().seen_[2] == std::optional<Decimal64>(Decimal64::parse("0")));
  }

}

namespace {

/** Rests a limit buy on every bar, and withdraws everything outstanding when the run ends. */
struct WithdrawsOnStop {
  std::size_t outstanding_at_stop_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        far_below()
      ).with_tag("rests-until-stop")
    );
  }

  template <class Ctx>
  void on_stop(Ctx& ctx) {
    outstanding_at_stop_ = ctx.portfolio().working_.size();
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "shutting down");
    }
  }
};

} // namespace

TEST_CASE("a shutdown handler sees the orders placed on the final bar") {
  // `Runner::stop` calls `on_stop` and settles what it asks for, so a handler withdrawing what is
  // outstanding decides on whatever portfolio was installed last. The order placed during the final
  // `on_bar` is the one most likely to be missed, and it is also the one still resting.
  //
  // No limit is enabled here on purpose: `persist_reservation` writes nothing when none needs it, so
  // a replay that relied on a reservation to carry that order would show `on_stop` an empty book.
  backtest::Backtester<WithdrawsOnStop> engine(WithdrawsOnStop{}, replay_config());
  replay_flat_bars(engine, 2);

  // Both bars placed an order and neither could fill, so both are outstanding when the run ends.
  REQUIRE(engine.strategy().outstanding_at_stop_ == 2);
  REQUIRE(engine.result().unfilled_ == 0);
}

namespace {

/**
 * Rests two orders in one instrument, then from the first one's fill withdraws the second.
 *
 * The reentrancy case: `settle_queued_against` collects the eligible references before it fills
 * anything, so the second order's reference is already in that list when `on_fill` removes it.
 */
struct WithdrawsTheOtherOnFill {
  bool placed_{false};
  bool withdrew_{false};
  std::size_t working_at_fill_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (placed_) {
      return;
    }
    placed_ = true;
    // The first fills against the next bar's open; the second rests, far from the market.
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("fills")
    );
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        far_below()
      ).with_tag("rests")
    );
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const strategy::Fill&) {
    if (withdrew_) {
      return;
    }
    withdrew_ = true;
    working_at_fill_ = ctx.portfolio().working_.size();
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "one is enough");
    }
  }
};

/** Buys ten at market the first time the phase changes, so the order is priced by `on_phase`. */
struct BuysOnPhaseChange {
  bool submitted_{false};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    // Only the close, not the open: the first bar also fires a transition, and buying there would
    // price the order from the bar this test wants to leave behind.
    if (submitted_ || phase != strategy::MarketPhase::POST_CLOSE) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("on-phase")
    );
  }
};

/** Places an order for the *other* instrument from a fill callback. */
struct PlacesTheOtherOnFill {
  bool seeded_{false};
  bool placed_on_fill_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (seeded_ || !(bar.instrument_ == samsung())) {
      return;
    }
    seeded_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("seed")
    );
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const strategy::Fill&) {
    if (placed_on_fill_) {
      return;
    }
    placed_on_fill_ = true;
    // **A market order, so it fills as soon as it is eligible.** A limit the market never reaches
    // stays unfilled whether the engine waits correctly or checks it on the wrong bar, which would
    // make this test pass without establishing either timing claim.
    ctx.submit(
      strategy::OrderIntent::market(
        hynix(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("from-a-fill")
    );
  }
};

} // namespace

TEST_CASE("a fill callback can withdraw an order the same pass had not reached") {
  // The order the loop has not filled yet is outstanding, so it is in the portfolio `on_fill` reads
  // and its reference resolves. Before this branch the queue was empty for the length of the loop,
  // so `on_fill` saw nothing to withdraw and could not have reached it if it had.
  backtest::Backtester<WithdrawsTheOtherOnFill> engine(
    WithdrawsTheOtherOnFill{}, replay_config()
  );
  replay_flat_bars(engine, 3);

  // The market order filled and the resting limit was still outstanding when `on_fill` ran.
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.strategy().working_at_fill_ == 1);
  // And the withdrawal reached it, so nothing is left over at the end.
  REQUIRE(engine.result().unfilled_ == 0);
}

TEST_CASE("an order placed from a fill waits for its own instrument's next bar") {
  // Two claims at once, and the order is a *market* order so that each is observable: an order
  // accepted during a bar is not eligible against that bar, and an order for another instrument
  // waits for that instrument's bar rather than the next bar of any kind.
  auto config = replay_config();
  config.instruments_ = {samsung_reference(), hynix_reference()};

  backtest::Backtester<PlacesTheOtherOnFill> engine(PlacesTheOtherOnFill{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));

  // The seed fills against this bar, and `on_fill` places a hynix market buy during it.
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.strategy().placed_on_fill_);
  REQUIRE(engine.result().executions_.size() == 1);

  // A later samsung bar does not touch it: a market order fills against the next bar *of its own
  // instrument*, and this is not one.
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.result().executions_.size() == 1);

  // Hynix's own bar fills it, at that bar's open.
  engine.feed(bar_for(hynix(), utc(2026, 3, 12, 6, 31), "70000"));
  engine.finish();

  REQUIRE(engine.result().executions_.size() == 2);
  REQUIRE(engine.result().executions_[0].fill_.instrument_ == samsung());
  REQUIRE(engine.result().executions_[1].fill_.instrument_ == hynix());
  REQUIRE(engine.result().unfilled_ == 0);
}

TEST_CASE("a phase decision is priced from the bar that caused it") {
  // `Runner::settle` values a market order at `context_.last_price`. Installing a portfolio marked
  // to this bar while leaving the price at the previous bar's makes the view incoherent rather than
  // fresh -- and worse than uniformly stale, because it looks current. With a ceiling of one
  // million, ten shares are 700,000 at the old close of 70,000 and 1,000,000 at the new one, so the
  // stale price admits an order the ceiling exists to refuse.
  auto config = replay_config();
  config.limits_.max_gross_exposure_ = Decimal64::parse("900000");
  config.limits_.permitted_phases_ =
    std::vector<strategy::MarketPhase>{strategy::MarketPhase::OPEN,
                                       strategy::MarketPhase::POST_CLOSE};

  backtest::Backtester<BuysOnPhaseChange> engine(BuysOnPhaseChange{}, std::move(config));
  // Inside the session at 70,000, then a bar closing at 100,000 that crosses the close.
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "100000"));
  engine.finish();

  REQUIRE(engine.strategy().submitted_);
  // Ten at the bar's own close is 1,000,000, over the 900,000 ceiling. Priced at the previous
  // close it is 700,000 and passes.
  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::GROSS_EXPOSURE_EXCEEDED) > 0);
}

namespace {

/** Rests a limit from `on_phase`, then records what `on_bar` of the same pass is shown. */
struct RestsFromPhaseThenLooks {
  bool placed_{false};
  std::vector<std::size_t> working_at_bar_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    working_at_bar_.push_back(ctx.portfolio().working_.size());
  }

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    if (placed_ || phase != strategy::MarketPhase::POST_CLOSE) {
      return;
    }
    placed_ = true;
    ctx.submit(
      strategy::OrderIntent::limit(
        samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")), far_below()
      ).with_tag("from-a-phase")
    );
  }
};

} // namespace

TEST_CASE("an order placed from a phase change survives the install before the bar") {
  // `feed` installs a portfolio twice: once before `on_phase` and once before `on_bar`. The second
  // install replaces whatever the first left, including `Runner::persist_reservation`'s row for the
  // order `on_phase` just placed -- so it has to carry that order itself, or the strategy sees an
  // empty book one line after asking for something.
  //
  // No limit is enabled, which is the case that matters: `persist_reservation` writes nothing when
  // none needs it, so nothing else could have carried the order.
  auto config = replay_config();
  config.limits_.permitted_phases_ =
    std::vector<strategy::MarketPhase>{strategy::MarketPhase::OPEN,
                                       strategy::MarketPhase::POST_CLOSE};

  backtest::Backtester<RestsFromPhaseThenLooks> engine(
    RestsFromPhaseThenLooks{}, std::move(config)
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70000", "70000"));
  // Crosses the close, so `on_phase` fires and places before this bar's `on_bar`.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.strategy().placed_);
  REQUIRE(engine.strategy().working_at_bar_.size() == 2);
  REQUIRE(engine.strategy().working_at_bar_[0] == 0);
  // Placed by `on_phase` moments earlier, and still there for `on_bar` to reconcile against.
  REQUIRE(engine.strategy().working_at_bar_[1] == 1);
}

namespace {

/** Places one market order from `on_phase` and another from `on_bar`, so both share an instant. */
struct BuysOnPhaseAndOnBar {
  bool on_phase_done_{false};
  bool on_bar_done_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (on_bar_done_ || !on_phase_done_) {
      return;
    }
    on_bar_done_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      ).with_tag("from-bar")
    );
  }

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    if (on_phase_done_ || phase != strategy::MarketPhase::POST_CLOSE) {
      return;
    }
    on_phase_done_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      ).with_tag("from-phase")
    );
  }
};

} // namespace

TEST_CASE("an order rate window counts a phase order against the bar that caused it") {
  // `Runner::feed(MarketPhase)` does not advance the context's time; only `feed(BarEvent)` does, and
  // it runs later. So `on_phase` decided with this bar's marks and close beside the *previous* bar's
  // instant, and `RiskGuard` files a send at that instant.
  //
  // `recent_orders` counts sends within `rate_window_` of now, so across a gap longer than the
  // window the phase order is filed under the old instant and the bar order does not see it: two
  // orders through a one-order window at what the replay calls the same moment.
  auto config = replay_config();
  config.limits_.max_orders_per_window_ = 1;
  config.limits_.rate_window_ = std::chrono::seconds(60);
  config.limits_.permitted_phases_ =
    std::vector<strategy::MarketPhase>{strategy::MarketPhase::OPEN,
                                       strategy::MarketPhase::POST_CLOSE};

  backtest::Backtester<BuysOnPhaseAndOnBar> engine(BuysOnPhaseAndOnBar{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 3, 0), BarInterval::ONE_MINUTE, "70000", "70000"));
  // Three and a half hours later, which is far outside the sixty-second window.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.strategy().on_phase_done_);
  REQUIRE(engine.strategy().on_bar_done_);
  // Both were decided at this bar's period end, so the second has to meet the one-order window.
  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::ORDER_RATE_EXCEEDED) == 1);
}

namespace {

/** Buys ten, then adds five from the fill callback, then one more from the bar. */
struct AddsFromFillThenFromBar {
  bool seeded_{false};
  bool added_on_fill_{false};
  bool added_on_bar_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (!seeded_) {
      seeded_ = true;
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
        ).with_tag("seed")
      );
      return;
    }
    if (!added_on_fill_ || added_on_bar_) {
      return;
    }
    added_on_bar_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      ).with_tag("one-more")
    );
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const strategy::Fill& fill) {
    if (added_on_fill_) {
      return;
    }
    added_on_fill_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        fill.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("5"))
      ).with_tag("from-a-fill")
    );
  }
};

} // namespace

TEST_CASE("an order placed from a fill reaches the next guard check") {
  // Slice 3 promised this and the earlier tests did not establish it: they showed the order was
  // *visible*, not that an enabled guard counts it.
  //
  // Ten are held when `on_fill` runs, so its buy of five projects to the ceiling of fifteen exactly
  // and is allowed. The buy of one that follows on the same bar projects to sixteen and must be
  // refused -- and would project eleven, and pass, if the `on_fill` order were not counted.
  //
  // **It does not test that the order is counted only once, and cannot.** Duplicating every row in
  // `install_portfolio` leaves this case green, because once-ness here is not arithmetic that could
  // double: `install_portfolio` builds each view from a fresh ledger snapshot and *replaces* the
  // context portfolio, so a reservation and an engine row for the same order never accumulate.
  // That is Decision C's argument, and it is structural rather than asserted.
  auto config = replay_config();
  config.limits_.max_position_quantity_ = Decimal64::parse("15");

  backtest::Backtester<AddsFromFillThenFromBar> engine(
    AddsFromFillThenFromBar{}, std::move(config)
  );
  replay_flat_bars(engine, 4);

  REQUIRE(engine.strategy().added_on_fill_);
  REQUIRE(engine.strategy().added_on_bar_);

  // Counted once: the buy of five was permitted and filled, alongside the seed of ten.
  REQUIRE(engine.result().executions_.size() == 2);
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("15")));
  // Counted at all: the buy of one on the same bar was refused against the same ceiling.
  REQUIRE(refusals_for(engine.result(), strategy::GuardReason::POSITION_LIMIT_EXCEEDED) == 1);
}

/** Records what the context could tell it about a date it has no bar for. */
struct AsksTheCalendar {
  std::optional<bool> tomorrow_trades_;
  bool asked_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    asked_ = true;
    tomorrow_trades_ = ctx.calendar().is_trading_day(algotradelib::Date{2026, 3, 11});
  }
};

TEST_CASE("a replayed strategy can read the calendar it was configured with") {
  // `Context::set_calendar` had no caller anywhere in the library, so `ctx.calendar()` -- published to
  // every strategy -- was permanently empty in both worlds. The live loop now fills it from a polled
  // calendar; leaving the replay empty would make a strategy that reads it behave differently in a
  // backtest from live, which is the divergence this slice exists to remove.
  auto config = replay_config();
  config.calendar_.country_ = algotradelib::Country::KR;
  config.calendar_.days_.push_back(algotradelib::TradingDay{algotradelib::Date{2026, 3, 11}, false});

  backtest::Backtester<AsksTheCalendar> engine(AsksTheCalendar{}, config);
  engine.feed(bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_DAY, "70000"));

  REQUIRE(engine.strategy().asked_);
  REQUIRE(engine.strategy().tomorrow_trades_.has_value());
  CHECK_FALSE(*engine.strategy().tomorrow_trades_);
}

TEST_CASE("a replay with no configured calendar answers nothing, rather than guessing") {
  // Empty is the honest answer, and it is distinct from "closed": a replay's trading-day question is
  // answered per bar by `feed`'s own argument, because a bar exists and so its date traded. A
  // strategy asking about a date it has no bar for gets `std::nullopt`.
  backtest::Backtester<AsksTheCalendar> engine(AsksTheCalendar{}, replay_config());
  engine.feed(bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_DAY, "70000"));

  REQUIRE(engine.strategy().asked_);
  CHECK_FALSE(engine.strategy().tomorrow_trades_.has_value());
}

/** Records the phase it was told and what the calendar said about the same date. */
struct ComparesPhaseWithCalendar {
  std::optional<strategy::MarketPhase> phase_;
  std::optional<bool> calendar_says_;

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    phase_ = phase;
    calendar_says_ =
      ctx.calendar().is_trading_day(strategy::SessionWindow::krx().local_date(ctx.now()));
  }
};

TEST_CASE("a replay believes feed's own trading-day argument, not the configured calendar") {
  // **Raised by Codex round 1, and pinned rather than fixed.** A configured calendar can say a bar's
  // date is closed while `feed`'s argument says it traded, and the replay believes the argument -- so
  // the same calendar would give `CLOSED` live and `OPEN` here.
  //
  // That is deliberate and it is the narrower of two evils. `ReplayConfig::calendar_` exists so a
  // strategy can *read* a calendar, which it could not before; deriving the replay's phase from it
  // instead would turn every date a supplied calendar does not cover into `UNKNOWN` and refuse every
  // order in replays that work today. Making the replay validate the two against each other is a
  // change with its own report caveat, and it is a decision for whoever wants it. This test exists so
  // that decision is taken deliberately and not discovered.
  auto config = replay_config();
  config.limits_.permitted_phases_ = std::nullopt;
  const auto bar = bar_at(utc(2026, 3, 10, 3, 0), BarInterval::ONE_DAY, "70000");
  const auto date = strategy::SessionWindow::krx().local_date(bar.period_end_);
  config.calendar_.days_.push_back(algotradelib::TradingDay{date, false});

  backtest::Backtester<ComparesPhaseWithCalendar> engine(ComparesPhaseWithCalendar{}, config);
  engine.feed(bar, /*trading_day=*/true);

  REQUIRE(engine.strategy().phase_.has_value());
  // The argument won: the replay says the session is open on a date its own calendar calls closed.
  CHECK(*engine.strategy().phase_ == strategy::MarketPhase::OPEN);
  REQUIRE(engine.strategy().calendar_says_.has_value());
  CHECK_FALSE(*engine.strategy().calendar_says_);
}

namespace {

/** Builds a caller-stated schedule naming its tariff, as a cost policy requires. */
CommissionSchedule operator_tariff(std::vector<CommissionRate> rows) {
  CommissionSchedule schedule;
  schedule.origin_ = CommissionOrigin::CALLER_STATED;
  schedule.tariff_ = "online, branch-opened account";
  schedule.rates_ = std::move(rows);
  return schedule;
}

/** One venue-keyed row with both components stated. */
CommissionRate venue_rate(
  Venue venue,
  const char* rate,
  const char* institution,
  std::optional<Date> from = std::nullopt,
  std::optional<Date> until = std::nullopt
) {
  CommissionRate row;
  row.country_ = Country::KR;
  row.venue_ = *VenueKey::normalized(venue);
  row.rate_ = Decimal64::parse(rate);
  row.institution_rate_ = Decimal64::parse(institution);
  row.from_ = std::move(from);
  row.until_ = std::move(until);
  return row;
}

/** Runs a two-bar replay costed by `policy` and returns the engine, so a caller can read the ledger. */
backtest::CommissionPolicy policy_for(Venue venue, std::vector<CommissionRate> rows) {
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff(std::move(rows)), *VenueKey::normalized(venue), Country::KR
  );
  REQUIRE(policy);
  return std::move(policy.value());
}

} // namespace

TEST_CASE("a KRX fill and an NXT fill of the same size are charged differently") {
  // The whole point of the slice, asserted end to end rather than on the schedule alone. The rates
  // are hand-picked round numbers rather than the operator's published table, which is still
  // disputed -- see issues/venue-aware-costs-rate-components.md. What is asserted is that the venue
  // reaches the arithmetic, not that any particular percentage is the right one.
  const std::vector<CommissionRate> rows{
    venue_rate(Venue::KRX, "0.002", "0.0001"),
    venue_rate(Venue::NXT, "0.001", "0.00005"),
  };

  const auto charge_at = [&](Venue venue) {
    backtest::Backtester<HoldsTen> engine(
      HoldsTen{}, replay_config(), backtest::NextBarFillModel(policy_for(venue, rows))
    );
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.finish();
    return backtest::summarize(engine.result(), engine.fills());
  };

  const auto krx = charge_at(Venue::KRX);
  const auto nxt = charge_at(Venue::NXT);
  REQUIRE(krx.executions_ == 1);
  REQUIRE(nxt.executions_ == 1);

  // Ten shares at 70,000 is 700,000. 0.2% is 1,400 and 0.1% is 700; the institution components are
  // 70 and 35. Computed by hand so the test states the arithmetic rather than echoing the code.
  CHECK(krx.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(nxt.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("700")));
  CHECK(krx.institution_fees_paid_ == std::optional<Decimal64>(Decimal64::parse("70")));
  CHECK(nxt.institution_fees_paid_ == std::optional<Decimal64>(Decimal64::parse("35")));
  CHECK_FALSE(krx.commission_paid_ == nxt.commission_paid_);
}

TEST_CASE("each cost component moves the ledger's cash, not only the report") {
  // A version of this that checked the two report totals alone would pass while every equity figure
  // and every realized profit was too high by the institution fee, with nothing in the run saying so.
  const auto cash_after = [](const char* institution) {
    backtest::Backtester<HoldsTen> engine(
      HoldsTen{},
      replay_config(),
      backtest::NextBarFillModel(
        policy_for(Venue::KRX, {venue_rate(Venue::KRX, "0.002", institution)})
      )
    );
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.finish();
    const auto cash = engine.ledger().cash_in(Currency{"KRW"});
    REQUIRE(cash);
    return *cash;
  };

  // 100,000,000 less 700,000 of stock, less 1,400 commission, less the institution component: 70 at
  // 0.01% and nothing at all when the component is zero.
  CHECK(cash_after("0.0001") == Decimal64::parse("99298530"));
  CHECK(cash_after("0") == Decimal64::parse("99298600"));
  // The difference is exactly the institution fee, which is the claim: it left the account.
  CHECK(decimal_subtract(cash_after("0"), cash_after("0.0001")) ==
        std::optional<Decimal64>(Decimal64::parse("70")));
}

TEST_CASE("one replay spanning a rate change charges each rate on its own dates") {
  // Deliberately one replay with one policy. Constructing two frozen models at two dates already
  // worked before this branch, so a test doing that would pass against `main` and prove nothing.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({
      venue_rate(Venue::KRX, "0.002", "0", std::nullopt, Date{2026, 3, 11}),
      venue_rate(Venue::KRX, "0.001", "0", Date{2026, 3, 12}, std::nullopt),
    }),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<BuysThenSells> engine(
    BuysThenSells{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  // Buys on the 11th at the old rate, sells on the 13th at the new one.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 2);
  // Two rates, each charging one execution -- not one rate charging both, which is what a frozen
  // model produces.
  REQUIRE(report.caveats_.applied_rates_.size() == 2);
  CHECK(report.caveats_.applied_rates_[0].rate_ == Decimal64::parse("0.002"));
  CHECK(report.caveats_.applied_rates_[0].executions_ == 1);
  CHECK(report.caveats_.applied_rates_[1].rate_ == Decimal64::parse("0.001"));
  CHECK(report.caveats_.applied_rates_[1].executions_ == 1);
  // 1,400 on the buy and 700 on the sale.
  CHECK(report.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("2100")));
}

TEST_CASE("a rate window is bounded by the market-local date, not the timestamp") {
  // A bar stamped just after UTC midnight is the *previous* day in Seoul, so a window ending that
  // day still covers it. Reading the instant instead would charge the next window's rate.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({
      venue_rate(Venue::KRX, "0.002", "0", std::nullopt, Date{2026, 3, 11}),
      venue_rate(Venue::KRX, "0.001", "0", Date{2026, 3, 12}, std::nullopt),
    }),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<HoldsTen> engine(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  // 2026-03-11 06:30 UTC is 15:30 in Seoul on the 11th; 2026-03-11 23:00 UTC is 08:00 on the 12th.
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 1);
  CHECK(report.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("1400")));
}

TEST_CASE("COMBINED cannot be asserted as the venue a fill happened at") {
  // It is what KIS's `SOR` normalizes to: consolidated routing, not a venue an order matched on.
  // A rate row may still be keyed to it, which the core tests assert.
  const auto refused = backtest::CommissionPolicy::scheduled(
    operator_tariff({venue_rate(Venue::KRX, "0.002", "0")}),
    *VenueKey::normalized(Venue::COMBINED), Country::KR
  );
  REQUIRE_FALSE(refused);
  CHECK(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK(refused.error().broker_code_ == "commission_venue_not_an_execution");
  CHECK_FALSE(refused.error().reached_broker_);
}

TEST_CASE("a caller-stated schedule that does not name its tariff is refused") {
  // The published table is channel by venue and the channel is a property of the account -- 뱅키스
  // against a branch-opened account is an order of magnitude. Nothing here can select it, so
  // omitting it is an error rather than a plausible default.
  CommissionSchedule unnamed;
  unnamed.origin_ = CommissionOrigin::CALLER_STATED;
  unnamed.rates_.push_back(venue_rate(Venue::KRX, "0.002", "0"));

  const auto refused = backtest::CommissionPolicy::scheduled(
    unnamed, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "commission_tariff_unnamed");

  // A broker-published schedule needs nothing from the caller, because the broker is the source.
  CommissionSchedule published;
  published.origin_ = CommissionOrigin::BROKER_PUBLISHED;
  published.rates_.push_back(venue_rate(Venue::KRX, "0.002", "0"));
  CHECK(backtest::CommissionPolicy::scheduled(
    published, *VenueKey::normalized(Venue::KRX), Country::KR
  ));
}

namespace {

/**
 * Places two market buys every bar and records whether it was ever stopped.
 *
 * Two, because one order cannot show that a halt governs the *rest of the bar*: the refused order
 * is withdrawn either way, and only a second eligible order behind it can demonstrate that nothing
 * filled after the refusal.
 */
/** Buys one share at a time, so a huge per-execution commission can be charged twice over. */
struct BuysOneEachBar {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      ).with_tag("buys-one")
    );
  }
};

/** Buys a quantity whose notional cannot fit an exact decimal, to reach the ledger's refusal. */
struct BuysHuge {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY,
        strategy::SizeSpec::quantity(Decimal64::parse("1300000000000"))
      ).with_tag("buys-huge")
    );
  }
};

/** Buys two shares on its first bar and never trades again. */
struct BuysOnceThenHolds {
  bool bought_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (bought_) {
      return;
    }
    bought_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("2"))
      )
    );
  }
};

struct PlacesTwoEachBar {
  bool stopped_{false};
  std::size_t fills_{0};
  std::size_t bars_seen_{0};
  std::size_t phases_seen_{0};
  std::size_t quotes_seen_{0};

  template <class Ctx>
  void on_phase(Ctx&, strategy::MarketPhase) { ++phases_seen_; }

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_seen_;
    for (const char* tag : {"first", "second"}) {
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
        ).with_tag(tag)
      );
    }
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill&) { ++fills_; }

  /** Counts quotes, so a halt can be shown to suppress the handler and not merely the counter. */
  template <class Ctx>
  void on_quote(Ctx&, const strategy::QuoteEvent&) { ++quotes_seen_; }

  template <class Ctx>
  void on_stop(Ctx& ctx) {
    stopped_ = true;
    // If this ever runs after a halt, the order has nowhere honest to go: the replay has already
    // stopped being able to cost one.
    ctx.submit(
      strategy::OrderIntent::market(
        samsung(), OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      ).with_tag("stop-flatten")
    );
  }
};

} // namespace

TEST_CASE("a date the schedule does not cover halts the replay rather than dropping a trade") {
  // The defect this replaces: an uncostable fill returned a decision with no execution, which read
  // as an order that did not trade. The replay carried on, the strategy took a path it would not
  // have taken, and every figure derived from that path -- equity, return, drawdown, Sharpe,
  // turnover -- described a run that never happened, while the report looked complete.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({venue_rate(Venue::KRX, "0.002", "0", std::nullopt, Date{2026, 3, 11})}),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<PlacesTwoEachBar> engine(
    PlacesTwoEachBar{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // Both orders from the first bar fill here, inside the window.
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.result().executions_.size() == 2);
  // Outside the window: the first eligible order cannot be costed.
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));

  const auto& halted = engine.result();
  REQUIRE(halted.halted_);
  CHECK(halted.halted_->broker_code_ == "no_rate_for_date");
  // The refusal names the date rather than describing it, so a reader does not have to reproduce
  // the run to find out which day was uncovered.
  CHECK(halted.halted_->message_.find("2026-03-12") != std::string::npos);
  CHECK_FALSE(halted.halted_->reached_broker_);

  // **The halt governed the rest of the bar.** The second order eligible on the refusal bar did not
  // fill behind the first; it was cleared into `aborted_`.
  CHECK(halted.executions_.size() == 2);
  CHECK(engine.strategy().fills_ == 2);
  CHECK(halted.aborted_ == 1);

  // A further bar changes nothing at all: no bar counted, no fill, no callback.
  const auto bars_at_halt = halted.bars_;
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  CHECK(engine.result().bars_ == bars_at_halt);
  CHECK(engine.result().executions_.size() == 2);

  engine.finish();
  // **`on_stop` never ran**, so nothing was placed into a replay that cannot cost an order, and
  // `unfilled_` is not where the aborted order landed -- the two counts mean different things.
  CHECK_FALSE(engine.strategy().stopped_);
  CHECK(engine.result().unfilled_ == 0);
  CHECK(engine.result().aborted_ == 1);

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.halted_);
  CHECK(report.aborted_ == 1);
  CHECK(report.executions_ == 2);
}

TEST_CASE("a halted replay consumes no quote either") {
  // The same rule `feed(const BarEvent&)` opens with, and it has to hold for every way into the
  // engine: a strategy told about anything after the halt decides on a run that has stopped being
  // able to cost what it asks for, and `finish` will not stop it either.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({venue_rate(Venue::KRX, "0.002", "0", std::nullopt, Date{2026, 3, 11})}),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  // **A strategy with an `on_quote` to suppress**, because a strategy without one proves nothing
  // about whether the handler ran.
  backtest::Backtester<PlacesTwoEachBar> engine(
    PlacesTwoEachBar{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.result().halted_);

  const auto quotes_before = engine.result().quotes_;
  const auto handled_before = engine.strategy().quotes_seen_;
  engine.feed(quote_at(utc(2026, 3, 12, 7, 0), "70500"));

  // Not counted, and the handler did not run: the quote was refused entry, not merely ignored
  // downstream of something.
  CHECK(engine.result().quotes_ == quotes_before);
  CHECK(engine.strategy().quotes_seen_ == handled_before);
}

TEST_CASE("the refusal bar itself runs nothing after the refusal") {
  // Found in review, and it is the counterexample to the promised terminal transition. `feed`
  // returned only from settlement, so on the very bar whose fill could not be costed it still
  // marked the book, recorded an equity point, advanced the clock, called `on_phase` and called
  // `on_bar` -- and the strategy, deciding once more on a portfolio missing that trade, queued two
  // fresh orders which then survived in the executor, because every later `feed` is a no-op and
  // `finish` returns early.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({venue_rate(Venue::KRX, "0.002", "0", std::nullopt, Date{2026, 3, 11})}),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<PlacesTwoEachBar> engine(
    PlacesTwoEachBar{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));

  const auto bars_before = engine.strategy().bars_seen_;
  const auto phases_before = engine.strategy().phases_seen_;
  const auto equity_before = engine.result().equity_.size();
  REQUIRE(bars_before == 2);

  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.result().halted_);

  // Nothing downstream of settlement ran on the refusal bar.
  CHECK(engine.strategy().bars_seen_ == bars_before);
  CHECK(engine.strategy().phases_seen_ == phases_before);
  CHECK(engine.result().equity_.size() == equity_before);

  engine.finish();
  // And nothing was left queued: the two orders the halted bar would have placed were never
  // placed, and the one order already resting was cleared into `aborted_`.
  CHECK(engine.result().aborted_ == 1);
  CHECK(engine.result().unfilled_ == 0);
  CHECK_FALSE(engine.strategy().stopped_);
}

TEST_CASE("a mark that will not fit halts the replay before the strategy decides on it") {
  // The strategy buys two shares on every bar. The third bar opens at an ordinary price, so the two
  // orders from the second fill at it; its close cannot value the lot, so the book cannot be marked.
  // Continuing -- which this used to do, skipping only the equity point -- ran `on_bar` with the
  // bar's close as the last price beside a lot valued one bar ago, and the orders placed from that
  // view filled on the next bar.
  backtest::Backtester<PlacesTwoEachBar> engine(PlacesTwoEachBar{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.strategy().bars_seen_ == 2);
  const auto fills_before = engine.strategy().fills_;
  const auto equity_before = engine.result().equity_.size();
  const auto phases_before = engine.strategy().phases_seen_;

  engine.feed(moving_bar(
    utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "9223372036854775807"
  ));

  REQUIRE(engine.result().halted_);
  CHECK(engine.result().halted_->broker_code_ == "mark_not_representable");
  CHECK(engine.result().halted_->message_.find("005930") != std::string::npos);
  CHECK_FALSE(engine.result().halted_->reached_broker_);
  // The orders resting from the second bar filled at the third bar's open, before the mark...
  CHECK(engine.strategy().fills_ > fills_before);
  // ...and nothing decided on the unmarked book: no `on_phase`, no `on_bar`, so no order from it.
  CHECK(engine.strategy().bars_seen_ == 2);
  CHECK(engine.strategy().phases_seen_ == phases_before);
  CHECK(engine.result().equity_.size() == equity_before);
  // The lot is still coherent at its previous mark.
  for (const auto& lot : engine.ledger().positions()) {
    CHECK(lot.last_price_ == Decimal64::parse("70000"));
  }

  // A later ordinary bar changes nothing: the replay stopped.
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  CHECK(engine.strategy().bars_seen_ == 2);
  CHECK(engine.result().equity_.size() == equity_before);

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.halted_);
  CHECK(report.halted_->broker_code_ == "mark_not_representable");
}

TEST_CASE("an equity total that will not fit is counted, and no return spans the gap", "[backtest][caveats]") {
  // Every mark fits and the total of cash and lots does not: cash is within a million won of the
  // largest exact decimal, and the lot's appreciation pushes the sum past it. The bar gets no point,
  // and the next point must not carry a return measured across two bars.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("9223372036854000000");
  backtest::Backtester<BuysOnceThenHolds> engine(BuysOnceThenHolds{}, config);
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("2")));
  const auto points_before = engine.result().equity_.size();

  const auto unvalued_at = utc(2026, 3, 12, 6, 30);
  engine.feed(moving_bar(unvalued_at, BarInterval::ONE_DAY, "70000", "1000000"));
  REQUIRE_FALSE(engine.result().halted_);
  // The mark landed -- this is a total that does not fit, not a lot that does not.
  REQUIRE(engine.ledger().positions().front().last_price_ == Decimal64::parse("1000000"));
  REQUIRE_FALSE(engine.ledger().equity_in(Currency{"KRW"}));
  CHECK(engine.result().equity_.size() == points_before);
  CHECK(engine.result().unvalued_bars_ == 1);

  // A second in a row: counted, and the date kept is still the first one's -- a close-stamped
  // bar's period end is its own timestamp.
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "1000000"));
  CHECK(engine.result().unvalued_bars_ == 2);
  REQUIRE(engine.result().first_unvalued_);
  CHECK(*engine.result().first_unvalued_ == unvalued_at);

  engine.feed(moving_bar(utc(2026, 3, 14, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.result().equity_.size() == points_before + 1);
  // The point after the gap carries no return; the one before it did.
  CHECK_FALSE(math::observed(engine.result().equity_.back().period_return_));
  CHECK(math::observed(engine.result().equity_[points_before - 1].period_return_));

  engine.feed(moving_bar(utc(2026, 3, 16, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // And the one after that is an ordinary bar-to-bar return again.
  CHECK(math::observed(engine.result().equity_.back().period_return_));
  CHECK(engine.result().unvalued_bars_ == 2);

  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(report.caveats_.unvalued_bars_ == 2);
  CHECK(report.caveats_.first_unvalued_ == engine.result().first_unvalued_);
}

TEST_CASE("an ordinary replay reports no unvalued bars", "[backtest][caveats]") {
  backtest::Backtester<PlacesTwoEachBar> engine(PlacesTwoEachBar{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70500"));
  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(report.caveats_.unvalued_bars_ == 0);
  CHECK_FALSE(report.caveats_.first_unvalued_);
  CHECK_FALSE(engine.result().halted_);
}

namespace {

/** A bar stamped at Seoul midnight of its period, as every shipped adapter stamps a non-minute bar. */
strategy::BarEvent open_stamped(int year, int month, int day, BarInterval interval, const char* price) {
  Bar bar;
  // Seoul midnight is 15:00 UTC the day before.
  bar.timestamp_ = utc(year, month, day, 0, 0) - std::chrono::hours(9);
  bar.open_ = Decimal64::parse(price);
  bar.high_ = Decimal64::parse(price);
  bar.low_ = Decimal64::parse(price);
  bar.close_ = Decimal64::parse(price);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), interval, BarStamp::OPEN, bar);
}

/** A KRX schedule charging `before` through `last_old_day` and `after` from the next day. */
backtest::CommissionPolicy rate_change(Date last_old_day, Date first_new_day) {
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({
      venue_rate(Venue::KRX, "0.002", "0", std::nullopt, last_old_day),
      venue_rate(Venue::KRX, "0.001", "0", first_new_day, std::nullopt),
    }),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);
  return std::move(policy.value());
}

/** Places one buy limit at the bar's price, so an `AT_LIMIT` fill is priced at the limit, not the open. */
struct BuysOneLimit {
  bool placed_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (placed_) {
      return;
    }
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = strategy::Price::checked(Decimal64::parse("70000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    placed_ = true;
    ctx.submit(strategy::OrderIntent::limit(
      bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("2")), *limit
    ));
  }
};

} // namespace

TEST_CASE("a fill at an open-stamped day bar's open is costed on that day and not the next", "[backtest][costs]") {
  // The order from the 10th fills at the 11th's open. The 11th's period end is nominal -- its stamp
  // plus 24 hours, so the 12th -- and dating the charge from it charged the 12th's rate a day early.
  backtest::Backtester<BuysOnceThenHolds> engine(
    BuysOnceThenHolds{}, replay_config(),
    backtest::NextBarFillModel(rate_change(Date{2026, 3, 11}, Date{2026, 3, 12}))
  );
  engine.feed(open_stamped(2026, 3, 10, BarInterval::ONE_DAY, "70000"));
  engine.feed(open_stamped(2026, 3, 11, BarInterval::ONE_DAY, "70000"));
  engine.feed(open_stamped(2026, 3, 12, BarInterval::ONE_DAY, "70000"));
  engine.finish();

  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.result().executions_.size() == 1);
  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  // The 11th's rate: 2 x 70,000 x 0.2%.
  CHECK(report.caveats_.applied_rates_[0].rate_ == Decimal64::parse("0.002"));
  CHECK(report.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("280")));
}

TEST_CASE("a fill at an open-stamped month's open is costed in that month", "[backtest][costs]") {
  // February's bar is stamped on its first trading day, 2 February; its nominal end is 31 days later,
  // 5 March, after the rate changed on 1 March. The fill is priced at February's open and belongs to
  // February's rate.
  backtest::Backtester<BuysOnceThenHolds> engine(
    BuysOnceThenHolds{}, replay_config(),
    backtest::NextBarFillModel(rate_change(Date{2026, 2, 28}, Date{2026, 3, 1}))
  );
  engine.feed(open_stamped(2026, 1, 2, BarInterval::ONE_MONTH, "70000"));
  engine.feed(open_stamped(2026, 2, 2, BarInterval::ONE_MONTH, "70000"));
  engine.feed(open_stamped(2026, 3, 2, BarInterval::ONE_MONTH, "70000"));
  engine.finish();

  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.result().executions_.size() == 1);
  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  CHECK(report.caveats_.applied_rates_[0].rate_ == Decimal64::parse("0.002"));
  CHECK(report.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("280")));
}

TEST_CASE("a limit priced inside an open-stamped month keeps the period-end date", "[backtest][costs]") {
  // **The partial scope, pinned.** Under `AT_LIMIT` a limit is priced at the limit, somewhere inside
  // the month, and nothing says which day: this branch dates only a fill at the open. The limit keeps
  // the old dating -- the nominal 5 March, so the March rate -- until
  // `plan/todo/nominal-period-end-dates-a-fill.md` settles it; a change that widened or shrank the
  // correction would move this figure.
  backtest::Backtester<BuysOneLimit> engine(
    BuysOneLimit{}, replay_config(),
    backtest::NextBarFillModel(rate_change(Date{2026, 2, 28}, Date{2026, 3, 1}))
  );
  engine.feed(open_stamped(2026, 1, 2, BarInterval::ONE_MONTH, "70000"));
  engine.feed(open_stamped(2026, 2, 2, BarInterval::ONE_MONTH, "70000"));
  engine.feed(open_stamped(2026, 3, 2, BarInterval::ONE_MONTH, "70000"));
  engine.finish();

  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.result().executions_.size() == 1);
  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  CHECK(report.caveats_.applied_rates_[0].rate_ == Decimal64::parse("0.001"));
}

TEST_CASE("a fill the ledger cannot record halts the replay rather than disappearing") {
  // The same silently-missing-trade path the costing refusal removes, one boundary further in:
  // `ReplayLedger::apply` is atomic and returns false on arithmetic that will not fit, and the
  // engine used to `continue` -- so the model produced a fill, the books did not record it, and
  // the replay carried on from a portfolio missing a trade with nothing saying so.
  //
  // Driven through a quantity whose notional cannot be represented, so the cash arithmetic fails
  // rather than the costing.
  backtest::ReplayConfig config = replay_config();
  config.cash_ = Decimal64::parse("100000000");

  // A zero-rate model, so the commission arithmetic cannot be what fails: `CommissionModel::on`
  // computes the notional before the rate, so any notional that overflows is refused there first
  // and the ledger is never reached. **Which step inside `apply` gives way is deliberately not
  // asserted** -- an earlier version of this comment claimed the cash total and review showed it
  // is the position valuation. What this test pins is the engine's response to `apply` refusing:
  // a typed halt rather than a fill that quietly disappears.
  backtest::Backtester<BuysHuge> engine(
    BuysHuge{}, config, backtest::NextBarFillModel(backtest::CommissionModel::none())
  );
  for (int day = 10; day < 14; ++day) {
    engine.feed(
      moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000.50", "70000.50")
    );
  }
  engine.finish();

  REQUIRE(engine.result().halted_);
  CHECK(engine.result().halted_->broker_code_ == "execution_not_representable");
  CHECK_FALSE(engine.result().halted_->reached_broker_);
  // Four bars were fed and the replay stopped inside one of them, so it did not run to the end
  // with a trade quietly missing from its books.
  CHECK(engine.result().bars_ < 4);
}

TEST_CASE("a replay that can cost every fill still runs to the end and stops its strategy") {
  // The control for the case above: without it, a mutation that halted on *every* bar would leave
  // that test passing and this one is what notices.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({venue_rate(Venue::KRX, "0.002", "0")}),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<PlacesTwoEachBar> engine(
    PlacesTwoEachBar{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  CHECK_FALSE(engine.result().halted_);
  CHECK(engine.result().aborted_ == 0);
  CHECK(engine.strategy().stopped_);
  CHECK(engine.result().executions_.size() == 4);
}

TEST_CASE("the report states the rates it applied, not the rates the schedule holds") {
  // A table listing the schedule's rows would describe what was available. The two differ whenever
  // a schedule spans more dates or venues than the replay traded, which is the normal case.
  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({
      venue_rate(Venue::KRX, "0.002", "0"),
      // Eligible by country and venue, and never reached: this replay never trades after 2027.
      venue_rate(Venue::KRX, "0.009", "0", Date{2027, 1, 1}, std::nullopt),
    }),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<HoldsTen> engine(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.caveats_.applied_rates_.size() == 1);
  CHECK(report.caveats_.applied_rates_.front().rate_ == Decimal64::parse("0.002"));
  CHECK(report.caveats_.applied_rates_.front().executions_ == 1);
  // The venue is recorded as *asserted*, because a replay cannot observe where a fill executed.
  REQUIRE(report.caveats_.asserted_venue_);
  CHECK(report.caveats_.asserted_venue_->venue() == Venue::KRX);
  CHECK(report.caveats_.commission_tariff_ == "online, branch-opened account");
}

TEST_CASE("a rate stating one figure leaves the institution total absent rather than zero") {
  // Every rate Toss publishes is one of these: a single `commissionRate` that says nothing about
  // whether an institution charge sits inside it. A zero total would claim the charge was levied
  // and came to nothing, which nobody said.
  CommissionSchedule published;
  published.origin_ = CommissionOrigin::BROKER_PUBLISHED;
  published.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.002"), std::nullopt, std::nullopt)
  );
  auto policy =
    backtest::CommissionPolicy::scheduled(published, VenueKey::any(), Country::KR);
  REQUIRE(policy);

  backtest::Backtester<HoldsTen> engine(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(report.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK_FALSE(report.institution_fees_paid_);
  CHECK(report.caveats_.inclusive_commission_executions_ == 1);
}

TEST_CASE("an existing schedule with no venue on it charges exactly what it charged") {
  // The migration regression, and it matters because the operator consumes this library as a
  // submodule: a number that moved under them would move silently.
  CommissionSchedule published;
  published.origin_ = CommissionOrigin::BROKER_PUBLISHED;
  published.rates_.push_back(
    rate_row(Country::KR, Decimal64::parse("0.00015"), std::nullopt, std::nullopt)
  );

  // The pre-venue path: a frozen model from `commission_model_from`.
  const auto frozen = backtest::commission_model_from(published, Country::KR, Date{2026, 3, 10});
  REQUIRE(frozen);
  backtest::Backtester<HoldsTen> old_way(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(frozen.value())
  );
  old_way.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  old_way.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  old_way.finish();

  // The new path over the same schedule, with no venue asserted.
  auto policy = backtest::CommissionPolicy::scheduled(published, VenueKey::any(), Country::KR);
  REQUIRE(policy);
  backtest::Backtester<HoldsTen> new_way(
    HoldsTen{}, replay_config(), backtest::NextBarFillModel(std::move(policy.value()))
  );
  new_way.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  new_way.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  new_way.finish();

  const auto before = backtest::summarize(old_way.result(), old_way.fills());
  const auto after = backtest::summarize(new_way.result(), new_way.fills());
  CHECK(before.commission_paid_ == std::optional<Decimal64>(Decimal64::parse("105")));
  CHECK(after.commission_paid_ == before.commission_paid_);
  CHECK(old_way.ledger().cash_in(Currency{"KRW"}) == new_way.ledger().cash_in(Currency{"KRW"}));
  // And a model seeded from a broker's schedule still says so after passing through the policy,
  // rather than being relabelled as the caller's because it took the flat route.
  CHECK(before.caveats_.applied_rates_.front().origin_ == CommissionOrigin::BROKER_PUBLISHED);
  CHECK(after.caveats_.applied_rates_.front().origin_ == CommissionOrigin::BROKER_PUBLISHED);
}

namespace {

/** An American listing, so a replay can span two currencies. */
Instrument apple_reference() {
  Instrument reference;
  reference.id_ = InstrumentId{Country::US, Venue::UNSPECIFIED, "AAPL"};
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency::usd();
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

} // namespace

TEST_CASE("a replay spanning two currencies states each, and no scalar total at all") {
  // `Money` refuses to add unlike currencies everywhere else in this library, and a cost total is
  // not an exception: a scalar here could only have been produced by adding won to dollars. The
  // per-currency figures are the answer and the scalar is **absent**, which is distinguishable from
  // a charge of zero.
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference(), apple_reference()};
  config.limits_.permitted_phases_ = std::nullopt;

  auto policy = backtest::CommissionPolicy::scheduled(
    operator_tariff({
      venue_rate(Venue::KRX, "0.002", "0"),
      [] {
        // The American row, keyed to no venue: the US listing has none this library normalizes.
        // **Deliberately a different rate from the Korean row.** With both at 0.002 this row could
        // be deleted without changing the expected USD figure, so the test would have passed while
        // a US fill was charged the Korean tariff -- which is exactly the defect review found.
        CommissionRate row;
        row.country_ = Country::US;
        row.rate_ = Decimal64::parse("0.005");
        row.institution_rate_ = Decimal64::parse("0");
        return row;
      }(),
    }),
    *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  backtest::Backtester<HoldsTen> engine(
    HoldsTen{}, config, backtest::NextBarFillModel(std::move(policy.value()))
  );
  engine.feed(bar_for(samsung(), utc(2026, 3, 10, 6, 30), "70000"));
  engine.feed(bar_for(apple_reference().id_, utc(2026, 3, 10, 6, 31), "200"));
  engine.feed(bar_for(samsung(), utc(2026, 3, 11, 6, 30), "70000"));
  engine.feed(bar_for(apple_reference().id_, utc(2026, 3, 11, 6, 31), "200"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 2);
  // Two entries, each in its own currency, and nothing added across them.
  REQUIRE(report.commission_by_currency_.size() == 2);
  CHECK_FALSE(report.commission_paid_);
  CHECK_FALSE(report.institution_fees_paid_);

  const auto in = [&](const Currency& currency) -> std::optional<Decimal64> {
    for (const auto& amount : report.commission_by_currency_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  };
  // Ten at 70,000 is 700,000 and 0.2% of that is 1,400 won. Ten at 200 is 2,000 and the *American*
  // row's 0.5% is 10 dollars -- charging the Korean row's 0.2% would give 4, so this figure is what
  // says the country came from the instrument rather than from the policy.
  CHECK(in(Currency{"KRW"}) == std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(in(Currency::usd()) == std::optional<Decimal64>(Decimal64::parse("10")));

  // **Turnover is unobserved, not merely uncaveated.** It divides an execution's notional by the
  // equity curve's opening value, and the curve is one currency's -- so a dollar fill against a
  // won equity produced an ordinary-looking ratio that had added the two. A whole-portfolio
  // turnover across currencies needs an exchange rate this library does not carry.
  CHECK_FALSE(math::observed(report.total_turnover_));
  // **Missing, not undefined**, and the two are different claims: `undefined()` is a statistic
  // whose inputs were present and whose ratio does not exist, and this ratio's inputs were never
  // comparable. An assertion on `observed` alone would accept either.
  CHECK_FALSE(math::is_undefined(report.total_turnover_));
  // The rest of the performance figures are the accounting currency's and stay observed, which is
  // what makes the absence above mean something.
  CHECK(math::observed(report.total_return_));
  // And the venue assertion is global while the country is not: the Korean row is venue-keyed and
  // was reached by the assertion, the American row is venue-neutral and was reached by the
  // fallback, in one replay.
  REQUIRE(report.caveats_.applied_rates_.size() == 2);
}

TEST_CASE("a dated rate for another market's calendar is refused, not charged a day early") {
  // Found at round 4. A replay derives one local date from its single `SessionWindow`, so a US
  // execution in a KRX-sessioned replay can be stamped with the next day -- and a US rate window
  // changing at that boundary would be charged on the wrong side of it, or halt despite a row that
  // covers the real trading day. Refused only where it could matter: an open-ended row is the same
  // rate whichever calendar named the day.
  CommissionSchedule schedule;
  schedule.origin_ = CommissionOrigin::CALLER_STATED;
  schedule.tariff_ = "online, branch-opened account";
  schedule.rates_.push_back(venue_rate(Venue::KRX, "0.002", "0"));
  CommissionRate american;
  american.country_ = Country::US;
  american.rate_ = Decimal64::parse("0.005");
  american.from_ = Date{2026, 3, 1};
  american.until_ = Date{2026, 3, 31};
  schedule.rates_.push_back(american);

  auto policy = backtest::CommissionPolicy::scheduled(
    schedule, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  // The Korean fill is priced: its market is the one the dates were computed in.
  CHECK(policy.value().charge(
    Decimal64::parse("10"), Decimal64::parse("70000"), Date{2026, 3, 10}, Country::KR
  ));

  // The American one is refused, naming why.
  const auto refused = policy.value().charge(
    Decimal64::parse("10"), Decimal64::parse("200"), Date{2026, 3, 10}, Country::US
  );
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "commission_date_not_market_local");

  // An open-ended American row is charged, because no boundary can fall on the wrong day.
  schedule.rates_.back().from_.reset();
  schedule.rates_.back().until_.reset();
  auto undated = backtest::CommissionPolicy::scheduled(
    schedule, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(undated);
  const auto charged = undated.value().charge(
    Decimal64::parse("10"), Decimal64::parse("200"), Date{2026, 3, 10}, Country::US
  );
  REQUIRE(charged);
  CHECK(charged.value().broker_ == Decimal64::parse("10"));
}

TEST_CASE("a costing refusal names the unreadable window, and the foreign guard sees it") {
  CommissionSchedule schedule;
  schedule.origin_ = CommissionOrigin::CALLER_STATED;
  schedule.tariff_ = "online, branch-opened account";
  auto krx = venue_rate(Venue::KRX, "0.002", "0", Date{2026, 1, 1});
  krx.until_unreadable_ = true;
  schedule.rates_.push_back(krx);

  auto policy = backtest::CommissionPolicy::scheduled(
    schedule, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);
  const auto refused = policy.value().charge(
    Decimal64::parse("10"), Decimal64::parse("200"), Date{2026, 3, 10}, Country::KR
  );
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "rate_bounds_unreadable");
  // A refusal a reader has to reproduce to understand is one they work around, so the date and the
  // remedy are both in the message.
  CHECK(refused.error().message_.find("2026-03-10") != std::string::npos);
  CHECK(refused.error().message_.find("refetch") != std::string::npos);

  // And the foreign-market guard, which asks whether *any* row for the market is dated, sees a row
  // dated only by damage. Asking about the two optionals alone read it as undated and walked past.
  CommissionSchedule foreign;
  foreign.origin_ = CommissionOrigin::CALLER_STATED;
  foreign.tariff_ = "online, branch-opened account";
  foreign.rates_.push_back(venue_rate(Venue::KRX, "0.002", "0"));
  CommissionRate american;
  american.country_ = Country::US;
  american.rate_ = Decimal64::parse("0.01");
  american.until_unreadable_ = true;
  foreign.rates_.push_back(american);

  auto abroad = backtest::CommissionPolicy::scheduled(
    foreign, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(abroad);
  const auto not_local = abroad.value().charge(
    Decimal64::parse("10"), Decimal64::parse("200"), Date{2026, 3, 10}, Country::US
  );
  REQUIRE_FALSE(not_local);
  CHECK(not_local.error().broker_code_ == "commission_date_not_market_local");
}

TEST_CASE("a foreign market with any dated rate is refused, whichever row would have won") {
  // Round 5's counterexample, found with a probe. The earlier check asked whether the *selected*
  // row was dated -- but `rate_for` excludes a dated row that does not cover the date it was
  // given, so a US row dated through 11 March simply lost to an undated one when a Korean session
  // supplied 12 March, and the undated row was charged with no refusal at all. What matters is
  // that a dated row for the market exists, so the day decides something this replay cannot
  // compute.
  CommissionSchedule schedule;
  schedule.origin_ = CommissionOrigin::CALLER_STATED;
  schedule.tariff_ = "online, branch-opened account";
  schedule.rates_.push_back(venue_rate(Venue::KRX, "0.002", "0"));

  CommissionRate dated;
  dated.country_ = Country::US;
  dated.rate_ = Decimal64::parse("0.01");
  dated.until_ = Date{2026, 3, 11};
  schedule.rates_.push_back(dated);

  CommissionRate undated;
  undated.country_ = Country::US;
  undated.rate_ = Decimal64::parse("0.02");
  schedule.rates_.push_back(undated);

  auto policy = backtest::CommissionPolicy::scheduled(
    schedule, *VenueKey::normalized(Venue::KRX), Country::KR
  );
  REQUIRE(policy);

  // 12 March in Seoul is still 11 March in New York, where the 1% row is in force -- and the old
  // check charged the 2% row here without a word.
  const auto refused = policy.value().charge(
    Decimal64::parse("100"), Decimal64::parse("1"), Date{2026, 3, 12}, Country::US
  );
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "commission_date_not_market_local");

  // The home market is unaffected: its dates are the ones the replay computes.
  CHECK(policy.value().charge(
    Decimal64::parse("10"), Decimal64::parse("70000"), Date{2026, 3, 12}, Country::KR
  ));
}

TEST_CASE("a cost total that will not sum is stated as incomplete rather than short") {
  // Round 5 disproved the bound I had assumed: two ledger-accepted executions, each carrying an
  // individually representable commission, can have a sum that does not fit -- and `summarize`
  // left the previous total in place and reported it as the answer.
  backtest::ReplayConfig config = replay_config();
  config.cash_ = Decimal64::parse("5000000000000000000");

  backtest::ReplayResult result;
  const auto huge = Decimal64::parse("5000000000000000000");
  for (int at = 0; at < 2; ++at) {
    backtest::Execution execution;
    execution.fill_.instrument_ = samsung();
    execution.fill_.side_ = OrderSide::BUY;
    execution.fill_.quantity_ = Decimal64::parse("1");
    execution.fill_.price_ = Decimal64::parse("1");
    execution.commission_ = huge;
    execution.slippage_ = Decimal64::parse("0");
    execution.currency_ = Currency{"KRW"};
    result.executions_.push_back(execution);
  }

  const backtest::NextBarFillModel fills;
  const auto report = backtest::summarize(result, fills);
  CHECK(report.caveats_.cost_totals_incomplete_);
  // Nothing is stated rather than a figure short by an amount nobody can name.
  CHECK_FALSE(report.commission_paid_);
  CHECK_FALSE(report.institution_fees_paid_);
}

TEST_CASE("an unsummable cost total is reported through a real replay, not only a built result") {
  // The route rather than the arithmetic: the previous case builds a `ReplayResult` directly, and
  // review asked for one that reaches `summarize` the way a replay does -- through the fill model,
  // the ledger's acceptance, and the engine's own accounting. Two one-share fills, each charged a
  // commission that is representable on its own and not when added to the other.
  backtest::ReplayConfig config = replay_config();
  config.cash_ = Decimal64::parse("9000000000000000000");

  backtest::Backtester<BuysOneEachBar> engine(
    BuysOneEachBar{}, config,
    backtest::NextBarFillModel(backtest::CommissionModel::of(Decimal64::parse("5000000000000000000")))
  );
  for (int day = 10; day < 14; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "1", "1"));
  }
  engine.finish();

  // Each fill's own arithmetic fits -- 5e18 charged against 9e18 of cash, then against what is
  // left, which goes negative and stays representable -- so the ledger accepts them and the
  // replay runs to the end. Their sum is 1e19, which
  // does not fit, and that is what the report has to say rather than quietly state the first.
  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.result().executions_.size() >= 2);

  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(report.caveats_.cost_totals_incomplete_);
  CHECK_FALSE(report.commission_paid_);
  CHECK_FALSE(report.institution_fees_paid_);
}

TEST_CASE("slippage in two currencies is kept apart like every other cost") {
  // The last cost figure still summed across currencies, and review found it with a probe: one won
  // fill and one dollar fill produced a scalar denominated in neither, with no caveat set and both
  // shipped examples printing it plainly. `Execution` has carried its currency since the
  // mixed-currency commission fix; slippage simply had not been given it.
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference(), apple_reference()};
  config.limits_.permitted_phases_ = std::nullopt;

  backtest::Backtester<HoldsTen> engine(
    HoldsTen{},
    config,
    backtest::NextBarFillModel(
      backtest::CommissionModel::none(), backtest::SlippageModel::of_ticks(1)
    )
  );
  engine.feed(bar_for(samsung(), utc(2026, 3, 10, 6, 30), "70000"));
  engine.feed(bar_for(apple_reference().id_, utc(2026, 3, 10, 6, 31), "200"));
  engine.feed(bar_for(samsung(), utc(2026, 3, 11, 6, 30), "70000"));
  engine.feed(bar_for(apple_reference().id_, utc(2026, 3, 11, 6, 31), "200"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 2);
  REQUIRE(report.slippage_by_currency_.size() == 2);
  // Neither currency's figure is zero, so a scalar sum would have been a number that looked real
  // and belonged to no currency at all.
  for (const auto& given_up : report.slippage_by_currency_) {
    CHECK(given_up.amount_.coefficient() > 0);
  }
  CHECK_FALSE(report.slippage_paid_);

  // And a single-currency replay still states the scalar, so the absence means something.
  backtest::Backtester<HoldsTen> home(
    HoldsTen{},
    replay_config(),
    backtest::NextBarFillModel(
      backtest::CommissionModel::none(), backtest::SlippageModel::of_ticks(1)
    )
  );
  home.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  home.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  home.finish();
  const auto at_home = backtest::summarize(home.result(), home.fills());
  REQUIRE(at_home.slippage_paid_);
  CHECK(at_home.slippage_paid_->coefficient() > 0);
}

TEST_CASE("turnover is stated when every fill is in the equity curve's own currency") {
  // The control for the mixed-currency case: without it, withholding turnover unconditionally
  // would satisfy that assertion and remove the figure from every replay that has one.
  backtest::Backtester<HoldsTen> engine(
    HoldsTen{}, replay_config(),
    backtest::NextBarFillModel(backtest::CommissionModel::of(Decimal64::parse("0.00015")))
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.executions_ == 1);
  REQUIRE(math::observed(report.total_turnover_));
  CHECK(report.total_turnover_ > math::Scalar{0});
}

namespace {

/**
 * Buys ten on the first bar and records every order update, with the portfolio it saw at the time.
 *
 * `working_seen_` is the part that matters: a handler told about an order that cannot find that
 * order in its own portfolio has been handed a picture contradicting the event.
 */
struct WatchesItsOwnOrder {
  bool submitted_{false};
  std::vector<OrderSnapshot> updates_;
  std::vector<std::size_t> working_seen_;
  std::vector<std::string> ids_seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      )
    );
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot& order) {
    updates_.push_back(order);
    ids_seen_.push_back(order.order_id_);
    working_seen_.push_back(ctx.portfolio().working_.size());
  }
};

/** Places a second order the first time it is told about the first one. */
struct PlacesFromAnUpdate {
  bool submitted_{false};
  bool answered_{false};
  std::size_t updates_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")),
        far_below()
      )
    );
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot& order) {
    ++updates_;
    if (answered_) {
      return;
    }
    answered_ = true;
    ctx.submit(
      strategy::OrderIntent::limit(
        order.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("2")),
        far_below()
      )
    );
  }
};

/** Places an unfillable order from `on_stop`, and counts updates. */
struct PlacesOnStop {
  std::size_t updates_{0};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_stop(Ctx& ctx) {
    ctx.submit(
      strategy::OrderIntent::limit(
        samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")),
        far_below()
      )
    );
  }

  template <class Ctx>
  void on_order_update(Ctx&, const OrderSnapshot&) { ++updates_; }
};

} // namespace

TEST_CASE("an order placed on one bar and filled on the next is reported once") {
  // **The case the first design of this feature reported zero times.** Observing the queue at
  // `install_portfolio` misses it entirely: the last install of a bar precedes `on_bar`, and the
  // next bar's settlement withdraws the order before that bar's first install. A fixture letting
  // the order rest an extra bar would pass against that design, which is why this one does not.
  backtest::Backtester<WatchesItsOwnOrder> engine(WatchesItsOwnOrder{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  // One update: the order became resting. The fill is *not* a second one -- the row left the queue,
  // and a disappearance is never reported.
  CHECK(engine.strategy().updates_.size() == 1);
  REQUIRE(engine.strategy().updates_.size() >= 1);
  CHECK(engine.strategy().updates_[0].status_ == OrderStatus::PENDING);
  CHECK(engine.strategy().updates_[0].quantity_ == Decimal64::parse("10"));
}

TEST_CASE("a strategy told about its own order can find it in its portfolio") {
  // With every optional limit off, `persist_reservation` writes no working row, so nothing but the
  // install inside the publish puts the order in the view. Without it the handler is told about an
  // order it can neither see nor withdraw.
  backtest::Backtester<WatchesItsOwnOrder> engine(WatchesItsOwnOrder{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.strategy().working_seen_.size() == 1);
  CHECK(engine.strategy().working_seen_[0] == 1);
}

TEST_CASE("a replay's order update carries an identifier") {
  // A replay is this order's broker, so a row with no `order_id_` is an order the handler cannot
  // name -- which a live row never is.
  backtest::Backtester<WatchesItsOwnOrder> engine(WatchesItsOwnOrder{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  REQUIRE(engine.strategy().ids_seen_.size() == 1);
  CHECK_FALSE(engine.strategy().ids_seen_[0].empty());
  // **The mapping, not merely non-emptiness.** The id is the queue's own reference, which is what
  // lets a handler join the event to the row it can withdraw; a nonempty check passes for any
  // string at all.
  REQUIRE(engine.result().unfilled_ == 0);
  CHECK(engine.strategy().ids_seen_[0] == "1");
}

TEST_CASE("an order an update handler places is reported at the next boundary, not within it") {
  // Bounded rather than recursive: the second order enters the queue behind the observation that is
  // running, so it is seen next time. Two updates over the run, never a loop inside one.
  backtest::Backtester<PlacesFromAnUpdate> engine(PlacesFromAnUpdate{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  CHECK(engine.strategy().updates_ == 2);
}

TEST_CASE("a change made by on_stop is reported, because finish is the only boundary that can") {
  // An `on_bar` placement on the last bar is already observed by the end-of-`feed` call, so a
  // fixture built that way would pass without `finish` publishing at all.
  backtest::Backtester<PlacesOnStop> engine(PlacesOnStop{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  CHECK(engine.strategy().updates_ == 0);
  engine.finish();
  CHECK(engine.strategy().updates_ == 1);
}

namespace {

/** Places two resting orders, then withdraws the second the first time it is told about one. */
struct WithdrawsAnotherFromAnUpdate {
  bool submitted_{false};
  bool answered_{false};
  std::vector<std::string> told_about_;
  std::vector<std::size_t> working_seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (submitted_) {
      return;
    }
    submitted_ = true;
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")),
        far_below()
      ).with_tag("first")
    );
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("2")),
        far_below()
      ).with_tag("second")
    );
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot& order) {
    told_about_.push_back(order.order_id_);
    working_seen_.push_back(ctx.portfolio().working_.size());
    if (answered_) {
      return;
    }
    answered_ = true;
    // Withdraw every *other* resting order, which is the one this batch has already captured an
    // event for and has not yet dispatched.
    for (const auto& working : ctx.portfolio().working_) {
      if (working.order_.order_id_ != order.order_id_) {
        ctx.cancel(working.ref_, "withdraw-the-other");
      }
    }
  }
};

} // namespace

TEST_CASE("an update is not dispatched for an order a previous update handler withdrew") {
  // The batch is captured before the first dispatch, and the first handler can cancel an order the
  // batch already holds an event for. Announcing it then would contradict the portfolio installed
  // alongside it -- which correctly no longer lists it -- and would invite a strategy to act on an
  // order that no longer exists.
  backtest::Backtester<WithdrawsAnotherFromAnUpdate> engine(
    WithdrawsAnotherFromAnUpdate{}, replay_config()
  );
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "80000", "81000"));
  engine.finish();

  // Two orders were placed and one was withdrawn from the first update, so exactly one update is
  // dispatched: the second order's event describes an order that no longer exists by then.
  CHECK(engine.strategy().told_about_.size() == 1);

  // And what it was told about, it could see.
  REQUIRE(engine.strategy().working_seen_.size() == 1);
  CHECK(engine.strategy().working_seen_[0] == 2);
}

namespace {

/** Places one protective stop on the first bar, as a strategy protecting an entry would. */
struct PlacesAStop {
  bool sent_{false};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (sent_) {
      return;
    }
    sent_ = true;
    ctx.submit(strategy::OrderIntent::stop_limit(
      bar.instrument_,
      OrderSide::SELL,
      strategy::SizeSpec::quantity(Decimal64::parse("10")),
      *strategy::Price::checked(Decimal64::parse("69500"), PriceRule::krx_equity()),
      *strategy::Price::checked(Decimal64::parse("69000"), PriceRule::krx_equity())
    ).with_tag("stop"));
  }
};

} // namespace

TEST_CASE("a replay models the stop a strategy places", "[trigger]") {
  // Parity with a live run, where this order goes to the broker: the stop is queued, released by the
  // bar that falls through its trigger, filled at its limit, and counted as the intrabar assumption.
  backtest::Backtester<PlacesAStop> engine(PlacesAStop{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // A day that opens above the trigger and falls through it. (`moving_bar` makes its low the open,
  // which cannot describe a fall.)
  Bar falling;
  falling.timestamp_ = utc(2026, 3, 11, 6, 30);
  falling.open_ = Decimal64::parse("70000");
  falling.high_ = Decimal64::parse("70000");
  falling.low_ = Decimal64::parse("68000");
  falling.close_ = Decimal64::parse("68000");
  falling.volume_ = Decimal64::parse("1000");
  engine.feed(strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, falling));
  engine.finish();

  CHECK(engine.result().rejected_.empty());
  REQUIRE(engine.result().executions_.size() == 1);
  const auto& fill = engine.result().executions_[0].fill_;
  CHECK(fill.side_ == OrderSide::SELL);
  CHECK(fill.price_ == Decimal64::parse("69000"));
  CHECK(fill.tag_ == "stop");
  CHECK(engine.result().stop_releases_ == 1);
  CHECK(engine.result().stop_fills_intrabar_ == 1);
}

namespace {

/** A sell stop-limit, as `reconcile` plans one. */
strategy::PlannedOrder sell_stop(const char* trigger, const char* limit) {
  auto order = order_of(OrderSide::SELL, "10", OrderType::STOP_LIMIT, limit);
  order.trigger_price_ = Decimal64::parse(trigger);
  return order;
}

/** A buy stop-limit. */
strategy::PlannedOrder buy_stop(const char* trigger, const char* limit) {
  auto order = order_of(OrderSide::BUY, "10", OrderType::STOP_LIMIT, limit);
  order.trigger_price_ = Decimal64::parse(trigger);
  return order;
}

} // namespace

TEST_CASE("a dormant stop waits for a bar that reaches its trigger", "[trigger]") {
  const backtest::NextBarFillModel model;
  const auto sell = model.decide(
    sell_stop("68500", "68000"), ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()
  );
  CHECK_FALSE(sell.execution_);
  CHECK_FALSE(sell.released_);
  CHECK(sell.still_working_);

  const auto buy = model.decide(
    buy_stop("71500", "72000"), ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()
  );
  CHECK_FALSE(buy.execution_);
  CHECK_FALSE(buy.released_);
  CHECK(buy.still_working_);
}

TEST_CASE("a stop released inside the bar fills at its limit, never better", "[trigger]") {
  for (const auto policy : {backtest::LimitFillPrice::AT_LIMIT, backtest::LimitFillPrice::AT_OPEN_IF_BETTER}) {
    const backtest::NextBarFillModel model(
      backtest::CommissionModel::none(), backtest::SlippageModel::none(), policy
    );
    INFO("policy " << static_cast<int>(policy));
    // The open is above the trigger and the low below it: released inside the bar, where the price
    // was the trigger, at which a sell limited at 69,000 is marketable.
    const auto sell = model.decide(
      sell_stop("69500", "69000"), ohlc("70000", "70500", "68000", "68500"), krx_grid(), fill_date()
    );
    REQUIRE(sell.execution_);
    CHECK(sell.released_);
    CHECK(sell.intrabar_release_fill_);
    CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69000"));

    const auto buy = model.decide(
      buy_stop("70500", "71000"), ohlc("70000", "72000", "69500", "71500"), krx_grid(), fill_date()
    );
    REQUIRE(buy.execution_);
    CHECK(buy.intrabar_release_fill_);
    CHECK(buy.execution_->fill_.price_ == Decimal64::parse("71000"));
  }
}

TEST_CASE("a stop released inside the bar whose limit was not marketable rests", "[trigger]") {
  const backtest::NextBarFillModel model;
  // A sell stop limited *above* its trigger: at the release the price was 69,500 and the limit wants
  // 69,800. The bar's high came before or after -- it cannot say -- so it does not fill here.
  const auto released = model.decide(
    sell_stop("69500", "69800"), ohlc("70000", "70500", "68000", "68500"), krx_grid(), fill_date()
  );
  CHECK_FALSE(released.execution_);
  CHECK(released.released_);
  CHECK(released.still_working_);

  // The next bar treats it as the limit it has become.
  const auto later = model.decide(
    sell_stop("69500", "69800"), ohlc("69000", "70000", "68800", "69900"), krx_grid(), fill_date(),
    std::nullopt, true
  );
  REQUIRE(later.execution_);
  CHECK_FALSE(later.released_);
  CHECK_FALSE(later.intrabar_release_fill_);
  CHECK(later.execution_->fill_.price_ == Decimal64::parse("69800"));
}

TEST_CASE("a stop the open gapped through is priced as a limit against the whole bar", "[trigger]") {
  const backtest::NextBarFillModel at_limit;
  const backtest::NextBarFillModel open_if_better(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::none(),
    backtest::LimitFillPrice::AT_OPEN_IF_BETTER
  );

  SECTION("through the trigger and not the limit") {
    const auto bar = ohlc("69200", "69400", "68000", "68500");
    const auto limit = at_limit.decide(sell_stop("69500", "69000"), bar, krx_grid(), fill_date());
    REQUIRE(limit.execution_);
    CHECK(limit.released_);
    CHECK_FALSE(limit.intrabar_release_fill_);
    CHECK(limit.execution_->fill_.price_ == Decimal64::parse("69000"));
    const auto open = open_if_better.decide(sell_stop("69500", "69000"), bar, krx_grid(), fill_date());
    REQUIRE(open.execution_);
    CHECK(open.execution_->fill_.price_ == Decimal64::parse("69200"));
  }

  SECTION("through both, and the bar never came back to the limit") {
    const auto gapped = at_limit.decide(
      sell_stop("69500", "69000"), ohlc("68000", "68800", "67500", "68200"), krx_grid(), fill_date()
    );
    CHECK_FALSE(gapped.execution_);
    CHECK(gapped.released_);
    CHECK(gapped.still_working_);
  }

  SECTION("a buy stop gapped through") {
    const auto buy = at_limit.decide(
      buy_stop("70500", "71000"), ohlc("70800", "71500", "70600", "71200"), krx_grid(), fill_date()
    );
    REQUIRE(buy.execution_);
    CHECK_FALSE(buy.intrabar_release_fill_);
    CHECK(buy.execution_->fill_.price_ == Decimal64::parse("71000"));
  }
}

TEST_CASE("the replay queues only a stop it can release", "[trigger]") {
  ReplayExecutor executor;
  auto modelled = resting_buy("10", "60000");
  modelled.type_ = OrderType::STOP_LIMIT;
  modelled.trigger_price_ = Decimal64::parse("59500");
  REQUIRE(executor.place(modelled) == runner::SendOutcome::ACCEPTED);

  auto no_trigger = modelled;
  no_trigger.trigger_price_.reset();
  CHECK(executor.place(no_trigger) == runner::SendOutcome::REFUSED_LOCALLY);
  auto immediate = modelled;
  immediate.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  CHECK(executor.place(immediate) == runner::SendOutcome::REFUSED_LOCALLY);
  auto after_hours = modelled;
  after_hours.session_ = OrderSession::AFTER_HOURS;
  CHECK(executor.place(after_hours) == runner::SendOutcome::REFUSED_LOCALLY);
  CHECK(executor.queued().size() == 1);

  // Released and withdrawn by its reference like any queued order.
  const auto ref = executor.queued().front().ref_;
  CHECK(executor.release(ref));
  CHECK(executor.queued().front().released_);
  CHECK(executor.cancel(withdrawal_of(ref)) == runner::SendOutcome::ACCEPTED);
  CHECK(executor.queued().empty());
}

namespace {

/** Places a protective stop on the first bar and asks to be flat on every later one. */
struct StopsThenTargetsFlat {
  int bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (++bars_ == 1) {
      strategy::TargetPosition entry;
      entry.instrument_ = bar.instrument_;
      entry.target_quantity_ = Decimal64::parse("10");
      ctx.submit(entry);
      return;
    }
    if (bars_ == 2) {
      ctx.submit(strategy::OrderIntent::stop_limit(
        bar.instrument_,
        OrderSide::SELL,
        strategy::SizeSpec::quantity(Decimal64::parse("10")),
        *strategy::Price::checked(Decimal64::parse("66500"), PriceRule::krx_equity()),
        *strategy::Price::checked(Decimal64::parse("66000"), PriceRule::krx_equity())
      ).with_tag("stop"));
      return;
    }
    strategy::TargetPosition flat;
    flat.instrument_ = bar.instrument_;
    flat.target_quantity_ = Decimal64::parse("0");
    flat.tag_ = "flat";
    ctx.submit(flat);
  }
};

} // namespace

TEST_CASE("a queued stop is withdrawn before a flatten in a replay as it is live", "[trigger]") {
  // The parity this slice exists for: the working row a replay shows is a stop row with its trigger,
  // so the target rule refuses exactly as it would against a live listing.
  backtest::Backtester<StopsThenTargetsFlat> engine(StopsThenTargetsFlat{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));

  // Bar 3's flatten withdraws the stop and sells nothing beside it; bar 4 restates it against a book
  // with no stop, and bar 5 fills the sale -- the sequence a live run takes.
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 16, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();

  REQUIRE(engine.result().rejected_.size() == 1);
  CHECK(engine.result().rejected_[0].reason_ == strategy::RejectReason::STOP_ORDER_RESTING);
  REQUIRE(engine.result().executions_.size() == 2);
  CHECK(engine.result().executions_[0].fill_.side_ == OrderSide::BUY);
  CHECK(engine.result().executions_[1].fill_.side_ == OrderSide::SELL);
  CHECK(engine.result().executions_[1].fill_.tag_ == "flat");
  CHECK(engine.result().stop_releases_ == 0);
  CHECK(engine.result().unfilled_ == 0);
}

TEST_CASE("a stop released inside the bar is credited a price the bar traded", "[trigger]") {
  const backtest::NextBarFillModel model;

  SECTION("a limit below the bar's low is credited the low, not the limit") {
    // Released at 69,500 and marketable there, but the bar never traded below 69,000: crediting the
    // 68,000 limit would name a price nobody could have been filled at.
    const auto sell = model.decide(
      sell_stop("69500", "68000"), ohlc("70000", "70500", "69000", "69200"), krx_grid(), fill_date()
    );
    REQUIRE(sell.execution_);
    CHECK(sell.intrabar_release_fill_);
    CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69000"));

    const auto buy = model.decide(
      buy_stop("70500", "72000"), ohlc("70000", "71000", "69500", "70800"), krx_grid(), fill_date()
    );
    REQUIRE(buy.execution_);
    CHECK(buy.execution_->fill_.price_ == Decimal64::parse("71000"));
  }

  SECTION("a limit the close proves the price came back to fills on the same bar") {
    // Limited above the trigger, so not marketable at the release -- but the close is always last, and
    // at 69,900 it proves the price rose through the 69,800 limit after falling through 69,500.
    const auto sell = model.decide(
      sell_stop("69500", "69800"), ohlc("70000", "70500", "68000", "69900"), krx_grid(), fill_date()
    );
    REQUIRE(sell.execution_);
    CHECK(sell.intrabar_release_fill_);
    CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69800"));

    const auto buy = model.decide(
      buy_stop("70500", "70200"), ohlc("70000", "71500", "69800", "70100"), krx_grid(), fill_date()
    );
    REQUIRE(buy.execution_);
    CHECK(buy.execution_->fill_.price_ == Decimal64::parse("70200"));
  }

  SECTION("a bar with no open releases inside the bar, never at an open it did not state") {
    auto bar = ohlc("70000", "71000", "68000", "68000");
    bar.bar_.open_.reset();
    // Limited above the trigger with a close below it: whether 69,800 traded after the release is
    // unknowable, so it rests.
    const auto rests = model.decide(sell_stop("69500", "69800"), bar, krx_grid(), fill_date());
    CHECK_FALSE(rests.execution_);
    CHECK(rests.released_);
    CHECK(rests.still_working_);
  }
}

TEST_CASE("a stop fill is never credited through its limit, slippage included", "[trigger]") {
  const backtest::NextBarFillModel slipping(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::of_basis_points(Decimal64::parse("10"))
  );
  // Credited 69,000 before slippage; ten basis points would record 68,931, below the limit.
  const auto sell = slipping.decide(
    sell_stop("69500", "69000"), ohlc("70000", "70500", "68000", "68500"), krx_grid(), fill_date()
  );
  REQUIRE(sell.execution_);
  CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69000"));

  // A released stop credited above its limit still slips, down to the limit and no further.
  const auto roomy = slipping.decide(
    sell_stop("69500", "68000"), ohlc("70000", "70500", "69000", "69200"), krx_grid(), fill_date()
  );
  REQUIRE(roomy.execution_);
  CHECK(roomy.execution_->fill_.price_ == Decimal64::parse("68931"));
}

TEST_CASE("a stop released at the open is dated from the open whatever the price policy", "[trigger][costs]") {
  // The same rate change as the open-stamped costing tests: 0.2% through the 11th, 0.1% from the 12th.
  const backtest::NextBarFillModel model(rate_change(Date{2026, 3, 11}, Date{2026, 3, 12}));
  const auto open_day = std::optional<Date>(Date{2026, 3, 11});
  const Date nominal_end{2026, 3, 12};

  // Opened below the trigger and above the limit: released and marketable at the open, credited the
  // limit under `AT_LIMIT` -- and it traded at the open, on the 11th.
  const auto at_open = model.decide(
    sell_stop("69500", "69000"), ohlc("69200", "69400", "68000", "68500"), krx_grid(), nominal_end,
    open_day
  );
  REQUIRE(at_open.execution_);
  CHECK(at_open.execution_->fill_.price_ == Decimal64::parse("69000"));
  CHECK(at_open.execution_->applied_.rate_ == Decimal64::parse("0.002"));

  // Released inside the bar: no known day, so the period end's, as for any intrabar fill.
  const auto inside = model.decide(
    sell_stop("69500", "69000"), ohlc("70000", "70500", "68000", "68500"), krx_grid(), nominal_end,
    open_day
  );
  REQUIRE(inside.execution_);
  CHECK(inside.execution_->applied_.rate_ == Decimal64::parse("0.001"));
}
