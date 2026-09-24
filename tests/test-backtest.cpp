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
    model.decide(order_of(OrderSide::BUY, "10"), ohlc("70000", "71000", "69000", "70500"), krx_grid());

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
  );
  REQUIRE(reached.execution_);
  // At the limit, not at the low: see the conservative default below.
  REQUIRE(reached.execution_->fill_.price_ == Decimal64::parse("69500"));

  // A bar that never touched the price is a bar in which the order sat unfilled, however close.
  const auto missed = model.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "68000"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  );
  REQUIRE_FALSE(missed.execution_);
  // And it stays queued: a resting order does not expire because one bar passed it by, and discarding
  // it would silently turn every limit into a fill-or-kill.
  REQUIRE(missed.still_working_);
}

TEST_CASE("a sell limit fills only when the bar's high reaches it") {
  const backtest::NextBarFillModel model;
  const auto reached = model.decide(
    order_of(OrderSide::SELL, "10", OrderType::LIMIT, "70800"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  );
  REQUIRE(reached.execution_);
  REQUIRE(reached.execution_->fill_.price_ == Decimal64::parse("70800"));

  const auto missed = model.decide(
    order_of(OrderSide::SELL, "10", OrderType::LIMIT, "72000"),
    ohlc("70000", "71000", "69000", "70500"),
    krx_grid()
  );
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
  );
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
  );
  REQUIRE(improved.execution_);
  REQUIRE(improved.execution_->fill_.price_ == Decimal64::parse("69000"));
}

TEST_CASE("a fill is stamped at the bar it filled against, not at the signal's bar") {
  const backtest::NextBarFillModel model;
  const auto next = ohlc("70000", "71000", "69000", "70500");
  const auto decision = model.decide(order_of(OrderSide::BUY, "10"), next, krx_grid());
  REQUIRE(decision.execution_);
  REQUIRE(decision.execution_->fill_.at_ == next.period_end_);
}

TEST_CASE("costs reach the execution, so a report can separate them from the strategy") {
  const backtest::NextBarFillModel model(
    backtest::CommissionModel::of(Decimal64::parse("0.00015")),
    backtest::SlippageModel::of_ticks(1)
  );
  const auto decision =
    model.decide(order_of(OrderSide::BUY, "10"), ohlc("70000", "71000", "69000", "70500"), krx_grid());

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

  const auto decision = model.decide(order_of(OrderSide::BUY, "10"), next, krx_grid());
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
  REQUIRE(from_broker.caveats_.commission_from_broker_);
  REQUIRE(from_broker.caveats_.commission_rate_ == Decimal64::parse("0.00015"));

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
  REQUIRE_FALSE(from_caller.caveats_.commission_from_broker_);
  REQUIRE(from_caller.caveats_.commission_rate_ == from_broker.caveats_.commission_rate_);
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
  REQUIRE(report.commission_paid_.coefficient() > 0);
  REQUIRE(report.slippage_paid_.coefficient() > 0);

  // The assumptions travel with the figures, as fields rather than as documentation a reader has to
  // remember applied to their run.
  REQUIRE(report.caveats_.limit_fill_ == backtest::LimitFillPrice::AT_LIMIT);
  REQUIRE(report.caveats_.commission_rate_ == Decimal64::parse("0.00015"));
  REQUIRE(report.caveats_.commission_minimum_ == Decimal64::parse("100"));
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
  REQUIRE(report.caveats_.commission_rate_.coefficient() == 0);
  REQUIRE(report.caveats_.slippage_ticks_ == 0);
  REQUIRE(report.commission_paid_.coefficient() == 0);
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

TEST_CASE("an order the bar fills is gone from the portfolio that bar installs") {
  // A filled order is removed before the ledger moves and before anything is shown a portfolio, so
  // no view ever carries it as still working. Without that a strategy would see its own filled order
  // resting and size the next one against it.
  backtest::Backtester<BuysOnce> engine(BuysOnce{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "69000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "71000"));
  engine.finish();

  REQUIRE(engine.result().executions_.size() == 1);
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

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
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

  // The resting sell is gone and only the buy side is left outstanding.
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
    ctx.submit(
      strategy::OrderIntent::limit(
        hynix(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")), far_below()
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
  // Two things at once: an order accepted during a bar is not eligible against that bar, and an
  // order for another instrument waits for that instrument's bar rather than the next bar of any
  // kind. Both are what makes an interleaved replay behave.
  auto config = replay_config();
  config.instruments_ = {samsung_reference(), hynix_reference()};

  backtest::Backtester<PlacesTheOtherOnFill> engine(PlacesTheOtherOnFill{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // The seed fills here, and `on_fill` places a hynix limit far below the market.
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // A samsung bar does not touch a hynix order.
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  // And hynix's own bar never reaches 50,000, so it rests rather than filling.
  engine.feed(bar_for(hynix(), utc(2026, 3, 12, 6, 31), "70000"));
  engine.finish();

  REQUIRE(engine.strategy().placed_on_fill_);
  // Exactly the seed filled: the hynix order never traded.
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.result().executions_[0].fill_.instrument_ == samsung());
  REQUIRE(engine.result().unfilled_ == 1);
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
