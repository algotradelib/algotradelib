#include <algotradelib/backtest>
#include <algotradelib/vanilla>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

TEST_CASE("an intraday interval annualizes on the whole bars a session holds") {
  // 390 minutes: 78 five-minute bars, 26 of fifteen, 13 of thirty, and 7 hourly ones, the last of them 30 minutes.
  const auto session = strategy::SessionWindow::krx();
  const std::pair<BarInterval, double> per_day[] = {
    {BarInterval::FIVE_MINUTES, 78.0},
    {BarInterval::FIFTEEN_MINUTES, 26.0},
    {BarInterval::THIRTY_MINUTES, 13.0},
    {BarInterval::ONE_HOUR, 7.0},
  };
  for (const auto& [interval, bars] : per_day) {
    INFO(bar_interval_name(interval));
    const auto count = backtest::periods_per_year(interval, session, 245.0);
    REQUIRE(count);
    CHECK(*count == Catch::Approx(bars * 245.0));
  }
  CHECK(backtest::interval_duration(BarInterval::FIVE_MINUTES) == std::chrono::seconds(300));
  CHECK(backtest::interval_duration(BarInterval::ONE_HOUR) == std::chrono::seconds(3600));
  CHECK_FALSE(backtest::interval_duration(BarInterval::ONE_DAY));
}

TEST_CASE("an open-stamped intraday bar ends one interval after its start") {
  Bar bar;
  bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(1000);
  const std::pair<BarInterval, std::chrono::minutes> lengths[] = {
    {BarInterval::FIVE_MINUTES, std::chrono::minutes(5)},
    {BarInterval::FIFTEEN_MINUTES, std::chrono::minutes(15)},
    {BarInterval::THIRTY_MINUTES, std::chrono::minutes(30)},
    {BarInterval::ONE_HOUR, std::chrono::minutes(60)},
  };
  for (const auto& [interval, length] : lengths) {
    INFO(bar_interval_name(interval));
    CHECK(strategy::bar_period_end(bar, interval, BarStamp::OPEN) == bar.timestamp_ + length);
    CHECK(strategy::bar_period_end(bar, interval, BarStamp::CLOSE) == bar.timestamp_);
  }
  CHECK(bar_interval_name(BarInterval::FIVE_MINUTES) == "5m");
  CHECK(bar_interval_name(BarInterval::FIFTEEN_MINUTES) == "15m");
  CHECK(bar_interval_name(BarInterval::THIRTY_MINUTES) == "30m");
  CHECK(bar_interval_name(BarInterval::ONE_HOUR) == "1h");
}

TEST_CASE("a calendar interval has no derivable period count, and says so") {
  // A month has no fixed length and a week's divisor depends on the market's week, so answering would
  // be inventing a constant. Nothing is better than a plausible wrong number here.
  const auto session = strategy::SessionWindow::krx();
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_WEEK, session, 245.0));
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_MONTH, session, 245.0));
  REQUIRE_FALSE(backtest::periods_per_year(BarInterval::ONE_YEAR, session, 245.0));
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

TEST_CASE("a ledger past sixteen instruments finds each lot as a scan of its positions would") {
  // Past sixteen lots the ledger finds a lot by an index instead of a scan; every answer must stay the same.
  // Each symbol is listed at all four venues, which makes four instruments, so a lookup that matched on the
  // symbol alone would find another venue's lot.
  const auto id = [](int k) {
    return InstrumentId{Country::KR, all_venues[static_cast<std::size_t>(k % 4)], "S" + std::to_string(k / 4)};
  };
  const auto buy = [&id](int k, const std::string& quantity, const char* price) {
    auto execution = filled(OrderSide::BUY, quantity.c_str(), price);
    execution.fill_.instrument_ = id(k);
    return execution;
  };
  auto ledger = opening("9000000000000000000");
  for (int k = 0; k <= 40; ++k) {
    REQUIRE(ledger.apply(buy(k, std::to_string(k + 1), "1000"), krw()));
    REQUIRE(ledger.positions().size() == static_cast<std::size_t>(k + 1));
    // Every lot opened so far, before and after the index exists, in the order it was opened.
    for (int j = 0; j <= k; ++j) {
      INFO("lot " << j << " of " << k + 1);
      REQUIRE(ledger.positions()[static_cast<std::size_t>(j)].instrument_ == id(j));
      REQUIRE(ledger.quantity_of(id(j)) == std::optional<Decimal64>(Decimal64::parse(std::to_string(j + 1))));
    }
    REQUIRE_FALSE(ledger.quantity_of(id(k + 1)));
  }

  // A fill adds to the lot it names and opens nothing.
  REQUIRE(ledger.apply(buy(3, "4", "2000"), krw()));
  CHECK(ledger.positions().size() == 41);
  CHECK(ledger.quantity_of(id(3)) == std::optional<Decimal64>(Decimal64::parse("8")));
  CHECK(ledger.average_price_of(id(3)) == std::optional<Decimal64>(Decimal64::parse("1500")));
  auto sell = buy(7, "8", "1000");
  sell.fill_.side_ = OrderSide::SELL;
  REQUIRE(ledger.apply(sell, krw()));
  CHECK(ledger.quantity_of(id(7)) == std::optional<Decimal64>(Decimal64::parse("0")));
  CHECK_FALSE(ledger.average_price_of(id(7)));

  // A mark reaches its own lot only: not the same symbol at another venue.
  const auto twin_price = ledger.positions()[37].last_price_;
  REQUIRE(ledger.mark(id(38), Decimal64::parse("777")));
  CHECK(ledger.positions()[38].last_price_ == Decimal64::parse("777"));
  CHECK(ledger.positions()[38].market_value_.amount_ == Decimal64::parse("30303"));
  CHECK(ledger.positions()[37].last_price_ == twin_price);
  // An instrument never held is not an error and changes nothing.
  CHECK(ledger.mark(id(41), Decimal64::parse("1")));
  CHECK(ledger.positions().size() == 41);
  // A value that does not fit leaves both fields as they were and says so.
  const auto before = ledger.positions()[30];
  CHECK_FALSE(ledger.mark(id(30), Decimal64::parse("900000000000000000")));
  CHECK(ledger.positions()[30].last_price_ == before.last_price_);
  CHECK(ledger.positions()[30].market_value_.amount_ == before.market_value_.amount_);

  // A copy keeps finding its lots, and a lot it opens is its own.
  auto copy = ledger;
  REQUIRE(copy.apply(buy(41, "2", "1000"), krw()));
  REQUIRE(copy.apply(buy(12, "1", "1000"), krw()));
  CHECK(copy.quantity_of(id(41)) == std::optional<Decimal64>(Decimal64::parse("2")));
  CHECK(copy.quantity_of(id(12)) == std::optional<Decimal64>(Decimal64::parse("14")));
  CHECK_FALSE(ledger.quantity_of(id(41)));
  CHECK(ledger.quantity_of(id(12)) == std::optional<Decimal64>(Decimal64::parse("13")));
}

TEST_CASE("a ledger moved past sixteen instruments leaves its source empty and itself unchanged") {
  const auto id = [](int k) {
    return InstrumentId{Country::KR, all_venues[static_cast<std::size_t>(k % 4)], "S" + std::to_string(k / 4)};
  };
  const auto fill = [&id](OrderSide side, int k) {
    auto execution = filled(side, "1", "1000");
    execution.fill_.instrument_ = id(k);
    return execution;
  };
  const auto holding_twenty = [&fill] {
    auto ledger = opening("9000000000000000000");
    for (int k = 0; k < 20; ++k) {
      REQUIRE(ledger.apply(fill(OrderSide::BUY, k), krw()));
    }
    return ledger;
  };
  // The source of a move holds no lot, answers as an empty ledger, and opens lots again past sixteen.
  const auto check_source = [&id, &fill](backtest::ReplayLedger& source) {
    CHECK(source.positions().empty());
    CHECK_FALSE(source.quantity_of(id(0)));
    CHECK_FALSE(source.average_price_of(id(19)));
    CHECK(source.mark(id(5), Decimal64::parse("1")));
    for (int k = 100; k < 120; ++k) {
      REQUIRE(source.apply(fill(OrderSide::SELL, k), krw()));
    }
    CHECK(source.positions().size() == 20);
    for (int k = 100; k < 120; ++k) {
      CHECK(source.quantity_of(id(k)) == std::optional<Decimal64>(Decimal64::parse("-1")));
    }
    CHECK_FALSE(source.quantity_of(id(0)));
  };
  const auto check_destination = [&id](const backtest::ReplayLedger& destination) {
    CHECK(destination.positions().size() == 20);
    for (int k = 0; k < 20; ++k) {
      CHECK(destination.quantity_of(id(k)) == std::optional<Decimal64>(Decimal64::parse("1")));
    }
    CHECK_FALSE(destination.quantity_of(id(100)));
  };

  SECTION("by construction") {
    auto source = holding_twenty();
    const backtest::ReplayLedger destination(std::move(source));
    check_source(source);
    check_destination(destination);
  }
  SECTION("by assignment") {
    auto source = holding_twenty();
    auto destination = opening();
    destination = std::move(source);
    check_source(source);
    check_destination(destination);
  }
  SECTION("into itself") {
    // A ledger moved into itself is unchanged: every lot is still found where it is listed, and it keeps
    // opening and finding lots past sixteen.
    auto ledger = holding_twenty();
    auto& alias = ledger;
    ledger = std::move(alias);
    check_destination(ledger);
    CHECK(ledger.mark(id(19), Decimal64::parse("2000")));
    CHECK(ledger.positions()[19].last_price_ == Decimal64::parse("2000"));
    for (int k = 100; k < 120; ++k) {
      REQUIRE(ledger.apply(fill(OrderSide::BUY, k), krw()));
    }
    CHECK(ledger.positions().size() == 40);
    for (int k = 0; k < 20; ++k) {
      CHECK(ledger.quantity_of(id(k)) == std::optional<Decimal64>(Decimal64::parse("1")));
      CHECK(ledger.quantity_of(id(100 + k)) == std::optional<Decimal64>(Decimal64::parse("1")));
    }
  }
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

TEST_CASE("tick slippage walks the grid one step at a time and across band boundaries") {
  const auto ticks = [](int count, const char* price, OrderSide side) {
    auto model = backtest::SlippageModel::of_ticks(count);
    const auto slipped = model.apply(Decimal64::parse(price), side, krx_grid());
    REQUIRE(slipped);
    return *slipped;
  };
  // Inside one band: whole ticks of that band.
  CHECK(ticks(2, "70000", OrderSide::BUY).first == Decimal64::parse("70200"));
  CHECK(ticks(2, "70000", OrderSide::SELL).first == Decimal64::parse("69800"));
  // A buy from 4,990: 4,995, 5,000, then 5,010; never 5,005, which is off the grid.
  const auto bought = ticks(3, "4990", OrderSide::BUY);
  CHECK(bought.first == Decimal64::parse("5010"));
  CHECK(bought.second == Decimal64::parse("20"));
  // A sell from 5,010: 5,000, 4,995, then 4,990; not 4,980, which skips two grid prices.
  const auto sold = ticks(3, "5010", OrderSide::SELL);
  CHECK(sold.first == Decimal64::parse("4990"));
  CHECK(sold.second == Decimal64::parse("20"));
  // One tick down from a band's first price is the band below's tick.
  CHECK(ticks(1, "5000", OrderSide::SELL).first == Decimal64::parse("4995"));
  CHECK(ticks(1, "2000", OrderSide::SELL).first == Decimal64::parse("1999"));
  // A sell never steps to zero or below.
  const auto floor = ticks(3, "2", OrderSide::SELL);
  CHECK(floor.first == Decimal64::parse("1"));
  CHECK(floor.second == Decimal64::parse("1"));
  CHECK(ticks(3, "1", OrderSide::SELL).second == Decimal64::parse("0"));
  // From a price off the grid each step still takes the tick where it starts.
  CHECK(ticks(2, "4998", OrderSide::BUY).first == Decimal64::parse("5013"));
  // An unknown grid contributes no tick slippage.
  const auto unknown = backtest::SlippageModel::of_ticks(3).apply(Decimal64::parse("4990"), OrderSide::BUY, PriceRule{});
  REQUIRE(unknown);
  CHECK(unknown->second == Decimal64::parse("0"));
  // Basis points and ticks together: the ticks are walked from the price before slippage.
  auto both = backtest::SlippageModel::of_ticks(3);
  both.basis_points_ = Decimal64::parse("10");
  const auto combined = both.apply(Decimal64::parse("4990"), OrderSide::BUY, krx_grid());
  REQUIRE(combined);
  CHECK(combined->second == Decimal64::parse("24.99"));
}

// ---------------------------------------------------------------------------
// Filling, and the lookahead discipline that is structural rather than a rule
// ---------------------------------------------------------------------------

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
 * A replay that sells what it does not hold: the sellable check is turned off where the replay is configured,
 * which is how a short is simulated (the ledger then assumes the borrow).
 */
backtest::ReplayConfig short_replay_config() {
  auto config = replay_config();
  config.limits_.enforce_sellable_quantity_ = false;
  return config;
}

/**
 * The same replay, with `permitted_phases_` left at its default.
 *
 * Separate from `replay_config` because that one switches the limit off and every other test here
 * wants it off -- the point of those tests is fill timing, not the guard. The two tests using this
 * one are about the default itself, which no replay could once run: the flatten a shipped vanilla
 * produces was refused by it.
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

/**
 * The close of the one-minute bar ending `minute` minutes past 10:00 KST on 2026-03-10: a single
 * session of intraday bars, across which a DAY order rests as it does live.
 */
Clock::time_point session_minute(int minute) { return utc(2026, 3, 10, 1, minute); }

} // namespace

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a weekly bar's period start is not derivable, and the engine says so instead of guessing") {
  // **The one shape this engine cannot check.** A close-stamped weekly bar ends on a Friday and began on a
  // Monday, and nothing in the event says when. Three versions of this code inferred it from a previous bar
  // -- of the instrument, then of the instrument and interval -- and each was wrong in turn, because `feed`'s
  // contract requires timestamp order and nothing about stamps, anchors or overlap. So the fill is allowed on
  // the rule that governed every fill before this check, and **counted**: the report says the check did not
  // happen rather than claiming one that did not.
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
#endif

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
  // **The other half of the causal rule.** It compares two instants, and only one of them was
  // once checked. `bar_period_end` adds a
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
  // And the engine says it could not prove that, rather than claiming it checked -- once, for the
  // one order on the one bar it was considered against.
  CHECK(engine.result().unchecked_fill_causality_ == 1);
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

#if defined(ATL_HAS_EIGEN)
TEST_CASE("the report says it cannot know which book the bars came from") {
  // A KRX-listed equity trades on two venues and KIS can report either or the consolidated tape over
  // both. Nothing in a bar carries that provenance and `summarize` is handed neither the config nor
  // the bar source, so the caveat says unverified rather than naming a tape -- naming one would be a
  // claim this code cannot support, and a wrong one would hide the very mismatch it exists to show.
  STATIC_REQUIRE(backtest::ReportCaveats::bar_tape_unverified);
}
#endif

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

TEST_CASE("a replay that fills a buy its cash cannot pay for says it borrowed", "[overdraft]") {
  // Default limits: nothing refuses the buy, so the ledger goes below zero. Counted, not refused.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("500000");
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  engine.feed(bar_at(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000"));
  engine.feed(bar_at(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000"));
  engine.finish();

  const auto& result = engine.result();
  REQUIRE(result.executions_.size() == 1);
  // The buy fills against the second bar, whose equity point is the first taken overdrawn.
  CHECK(result.overdrawn_bars_ == 1);
  CHECK(result.first_overdrawn_ == result.equity_.back().at_);
  REQUIRE(result.lowest_cash_.has_value());
  // Five hundred thousand less ten shares at seventy thousand.
  CHECK(result.lowest_cash_->amount_ == Decimal64::parse("-200000"));
  CHECK(result.lowest_cash_at_ == result.executions_[0].fill_.at_);

#if defined(ATL_HAS_EIGEN)
  const auto report = backtest::summarize(result, engine.fills());
  CHECK(report.caveats_.overdrawn_bars_ == 1);
  CHECK(report.caveats_.first_overdrawn_ == result.first_overdrawn_);
  CHECK(report.caveats_.lowest_cash_at_ == result.lowest_cash_at_);
  REQUIRE(report.caveats_.lowest_cash_.has_value());
  CHECK(report.caveats_.lowest_cash_->amount_ == Decimal64::parse("-200000"));
#endif
}

TEST_CASE("a replay whose cash covers its buys reports no borrowing", "[overdraft]") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config());
  engine.feed(bar_at(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000"));
  engine.feed(bar_at(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000"));
  engine.finish();
  REQUIRE(engine.result().executions_.size() == 1);
  CHECK(engine.result().overdrawn_bars_ == 0);
  CHECK_FALSE(engine.result().first_overdrawn_.has_value());
  CHECK_FALSE(engine.result().lowest_cash_.has_value());
}

TEST_CASE("the buying-power check still lets a gap up overdraw", "[overdraft]") {
  // The check values a market buy at the reference price, the last close, without commission; the
  // fill is at the next bar's open. Cash for ten at 70,000 exactly, and the next bar opens higher.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("700000");
  config.limits_.enforce_buying_power_ = true;
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "72000", "72000"));
  engine.finish();
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(engine.result().lowest_cash_.has_value());
  // No commission in this config: the gap alone, 700,000 less ten at 72,000.
  CHECK(engine.result().lowest_cash_->amount_ == Decimal64::parse("-20000"));
  CHECK(engine.result().overdrawn_bars_ == 1);
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("no return is measured from equity at or below zero, and the ratios are undefined") {
  // Buying power is not enforced by default, so a replay can owe more than it holds. From a negative
  // total the ratio's sign reverses: the fall from -100,000 to -200,000 read as +100%.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("100000");
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "50000"));
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "50000", "40000"));
  engine.feed(moving_bar(utc(2026, 3, 16, 6, 30), BarInterval::ONE_DAY, "40000", "45000"));
  engine.finish();

  const auto& curve = engine.result().equity_;
  REQUIRE(engine.result().executions_.size() == 1);
  REQUIRE(curve.size() == 5);
  REQUIRE(curve[2].equity_ == Decimal64::parse("-100000"));
  REQUIRE(curve[3].equity_ == Decimal64::parse("-200000"));
  // From 100,000 to -100,000 is a return; from a negative total there is none.
  REQUIRE(curve[2].period_return_ == Catch::Approx(-2.0));
  CHECK_FALSE(math::observed(curve[3].period_return_));
  CHECK_FALSE(math::observed(curve[4].period_return_));

  const auto report = backtest::summarize(engine.result(), engine.fills(), 252.0);
  CHECK(math::is_undefined(report.sharpe_));
  CHECK(math::is_undefined(report.sortino_));
  CHECK(math::is_undefined(report.annualized_sharpe_));
  // The figures that are still defined stay.
  CHECK(report.total_return_ == Catch::Approx(-2.5));
  CHECK(math::observed(report.max_drawdown_));
}

TEST_CASE("a replay that opens with negative equity has no total return and no turnover") {
  // A replay may start with negative cash; -100,000 to -50,000 is not a 50% loss.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("-100000");
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "75000"));
  engine.finish();

  REQUIRE(engine.result().equity_.front().equity_ == Decimal64::parse("-100000"));
  REQUIRE(engine.result().equity_.back().equity_ == Decimal64::parse("-50000"));
  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(math::is_undefined(report.total_return_));
  CHECK(math::is_undefined(report.total_turnover_));
  CHECK(math::is_undefined(report.sharpe_));
}

TEST_CASE("equity that reaches zero only at the last point leaves every return defined") {
  auto config = replay_config();
  config.cash_ = Decimal64::parse("100000");
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, std::move(config));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "65000"));
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "65000", "60000"));
  engine.finish();

  REQUIRE(engine.result().equity_.back().equity_ == Decimal64::parse("0"));
  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK(math::observed(report.sharpe_));
  CHECK(math::observed(report.sortino_));
}
#endif

// ---------------------------------------------------------------------------
// The report, and the caveats that travel with the figures
// ---------------------------------------------------------------------------

#if defined(ATL_HAS_EIGEN)

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

#endif

// ---------------------------------------------------------------------------
// Every vanilla strategy, replayed at both intervals
// ---------------------------------------------------------------------------

namespace {

#if defined(ATL_HAS_EIGEN)
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
#endif

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

#if defined(ATL_HAS_EIGEN)

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

#endif

TEST_CASE("TimedLiquidation flattens a replay under the default limits") {
  // **The first place in this repository that runs the default `permitted_phases_` where the phase is real.** Every other replay either
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

TEST_CASE("a stated tax is charged on a sale only, and leaves the account", "[sell-tax]") {
  // The Korean securities transaction tax is charged on sales; a replay could not say so, because a
  // commission rate has no side. The rate is the caller's, never the library's.
  const auto fills =
    backtest::NextBarFillModel().with_sell_tax(backtest::SellTax{Decimal64::parse("0.002"), Country::KR});
  backtest::Backtester<BuysThenSells> engine(BuysThenSells{}, replay_config(), fills);
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();

  const auto& executions = engine.result().executions_;
  REQUIRE(executions.size() == 2);
  REQUIRE(executions[0].fill_.side_ == OrderSide::BUY);
  CHECK(executions[0].tax_ == Decimal64::parse("0"));
  REQUIRE(executions[1].fill_.side_ == OrderSide::SELL);
  // Ten at seventy thousand is seven hundred thousand, and 0.2 % of it is 1,400.
  CHECK(executions[1].tax_ == Decimal64::parse("1400"));
  // Bought and sold at the same price with no commission: the tax is all the account lost.
  CHECK(engine.ledger().cash_in(Currency{"KRW"}) == std::optional<Decimal64>(Decimal64::parse("99998600")));

#if defined(ATL_HAS_EIGEN)
  const auto report = backtest::summarize(engine.result(), engine.fills());
  REQUIRE(report.tax_paid_.has_value());
  CHECK(*report.tax_paid_ == Decimal64::parse("1400"));
  REQUIRE(report.caveats_.sell_tax_.has_value());
  CHECK(report.caveats_.sell_tax_->rate_ == Decimal64::parse("0.002"));
#endif
}

TEST_CASE("no stated tax charges none and reports no figure", "[sell-tax]") {
  backtest::Backtester<BuysThenSells> engine(BuysThenSells{}, replay_config());
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();
  REQUIRE(engine.result().executions_.size() == 2);
  CHECK(engine.result().executions_[1].tax_ == Decimal64::parse("0"));
  CHECK(engine.ledger().cash_in(Currency{"KRW"}) == std::optional<Decimal64>(Decimal64::parse("100000000")));
#if defined(ATL_HAS_EIGEN)
  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK_FALSE(report.tax_paid_.has_value());
  CHECK_FALSE(report.caveats_.sell_tax_.has_value());
#endif
}

namespace {

/** Short ten from flat, then back to flat: a sale that opens a short, and a buy that covers it. */
struct ShortsThenCovers {
  std::size_t bars_{0};
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = bars_ >= 3 ? Decimal64::parse("0") : Decimal64::parse("-10");
    ctx.submit(target);
  }
};

backtest::NextBarFillModel taxed(Country country) {
  return backtest::NextBarFillModel().with_sell_tax(backtest::SellTax{Decimal64::parse("0.002"), country});
}

} // namespace

TEST_CASE("a replay refuses a short unless its configuration turns the sellable check off", "[short]") {
  // The default limits check a sell against what is held, in a replay too: the ledger reports the whole
  // holding as sellable, so an exit passes and a sale from nothing does not. Simulating a short is a
  // choice made where the replay is configured (`short_replay_config`); the ledger then assumes the borrow.
  const auto run = [](backtest::ReplayConfig config) {
    backtest::Backtester<ShortsThenCovers> engine(ShortsThenCovers{}, std::move(config));
    for (int day = 10; day < 16; ++day) {
      engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    }
    engine.finish();
    return engine.result();
  };

  const auto cash_account = run(replay_config());
  CHECK(cash_account.executions_.empty());
  REQUIRE_FALSE(cash_account.refused_.empty());
  CHECK(cash_account.refused_.front().reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);

  const auto shorting = run(short_replay_config());
  REQUIRE(shorting.executions_.size() == 2);
  CHECK(shorting.executions_[0].fill_.side_ == OrderSide::SELL);
  CHECK(shorting.executions_[1].fill_.side_ == OrderSide::BUY);
  CHECK(shorting.refused_.empty());
}

TEST_CASE("a sale opening a short pays the tax, and the buy covering it does not", "[sell-tax]") {
  backtest::Backtester<ShortsThenCovers> engine(ShortsThenCovers{}, short_replay_config(), taxed(Country::KR));
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();
  const auto& executions = engine.result().executions_;
  REQUIRE(executions.size() == 2);
  REQUIRE(executions[0].fill_.side_ == OrderSide::SELL);
  CHECK(executions[0].tax_ == Decimal64::parse("1400"));
  REQUIRE(executions[1].fill_.side_ == OrderSide::BUY);
  CHECK(executions[1].tax_ == Decimal64::parse("0"));
}

TEST_CASE("a tax stated for one market is not charged on another market's sales", "[sell-tax]") {
  backtest::Backtester<BuysThenSells> engine(BuysThenSells{}, replay_config(), taxed(Country::US));
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  }
  engine.finish();
  REQUIRE(engine.result().executions_.size() == 2);
  CHECK(engine.result().executions_[1].tax_ == Decimal64::parse("0"));
}

TEST_CASE("the tax is truncated to whole units", "[sell-tax]") {
  backtest::Backtester<BuysThenSells> engine(BuysThenSells{}, replay_config(), taxed(Country::KR));
  for (int day = 10; day < 16; ++day) {
    engine.feed(moving_bar(utc(2026, 3, day, 6, 30), BarInterval::ONE_DAY, "70001", "70001"));
  }
  engine.finish();
  REQUIRE(engine.result().executions_.size() == 2);
  // Ten at 70,001 is 700,010, and 0.2 % of it is 1,400.02: charged 1,400.
  CHECK(engine.result().executions_[1].tax_ == Decimal64::parse("1400"));
}

TEST_CASE("a sell tax rate outside zero to one is refused", "[sell-tax]") {
  const backtest::NextBarFillModel model;
  CHECK_THROWS_AS(model.with_sell_tax(backtest::SellTax{Decimal64::parse("-0.001"), Country::KR}),
                  std::invalid_argument);
  CHECK_THROWS_AS(model.with_sell_tax(backtest::SellTax{Decimal64::parse("1"), Country::KR}),
                  std::invalid_argument);
  CHECK_NOTHROW(model.with_sell_tax(backtest::SellTax{Decimal64::parse("0"), Country::KR}));
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a stated tax with nothing executed reports no tax figure", "[sell-tax]") {
  backtest::Backtester<HoldsTen> engine(HoldsTen{}, replay_config(), taxed(Country::KR));
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.finish();
  REQUIRE(engine.result().executions_.empty());
  const auto report = backtest::summarize(engine.result(), engine.fills());
  CHECK_FALSE(report.tax_paid_.has_value());
  CHECK(report.caveats_.sell_tax_.has_value());
}
#endif

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

TEST_CASE("a flat commission model keeps the origin of the schedule it was built from") {
  CommissionSchedule published;
  published.rates_.push_back(rate_row(Country::KR, Decimal64::parse("0.00015"), std::nullopt, std::nullopt));
  const auto broker = backtest::commission_model_from(published, Country::KR, Date{2026, 9, 23});
  REQUIRE(broker);
  CHECK(broker.value().from_broker_);

  // A rate the caller typed from a fee page is reported as the caller's, never as one a broker published.
  CommissionSchedule stated = published;
  stated.origin_ = CommissionOrigin::CALLER_STATED;
  stated.tariff_ = "online account";
  const auto caller = backtest::commission_model_from(stated, Country::KR, Date{2026, 9, 23});
  REQUIRE(caller);
  CHECK_FALSE(caller.value().from_broker_);
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

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a DAY order on daily bars lasts one session, not until it fills") {
  // Decided at the 10th's close, the limit trades in the 11th's session and
  // expires at its close, before the strategy is shown the 11th's bar -- as a broker expires it. It
  // was good-till-cancelled: still working on the 12th, and on any later day the market reached it.
  backtest::Backtester<RestsOneLimit> engine(RestsOneLimit{}, replay_config());
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "45000", "70000"));
  engine.finish();

  const auto& seen = engine.strategy().working_seen_;
  REQUIRE(seen == std::vector<std::size_t>{0, 0, 0, 0});
  // The 13th's low of 45,000 would have filled it at 50,000; it had expired two days before.
  REQUIRE(engine.result().executions_.empty());
  REQUIRE(engine.result().unfilled_ == 0);
  REQUIRE(engine.result().expired_day_orders_ == 1);
  REQUIRE(backtest::summarize(engine.result(), engine.fills()).caveats_.expired_day_orders_ == 1);
}
#endif

namespace {

/** Rests one limit buy at 69,000 from its first quote, and nothing from bars. */
struct LimitFromQuote {
  bool placed_{false};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_quote(Ctx& ctx, const strategy::QuoteEvent& quote) {
    if (placed_) {
      return;
    }
    placed_ = true;
    ctx.submit(strategy::OrderIntent::limit(
      quote.quote_.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
      *strategy::Price::checked(Decimal64::parse("69000"), krx_grid())
    ));
  }
};

} // namespace

TEST_CASE("a limit from a quote inside a daily bar's session gets the next session, and only it") {
  // Decided at 10:00 on the 11th, the order cannot fill against the 11th's bar, whose range includes
  // the hours before it. It trades in the 12th's session -- one session, shifted, as a market order
  // decided there fills at the 12th's open -- and expires with it. The alternative, expiring it at
  // the 11th's close, would let no quote-placed limit ever fill on daily bars.
  SECTION("reached in that session: filled") {
    backtest::Backtester<LimitFromQuote> engine(LimitFromQuote{}, replay_config());
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70000"));
    engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "68000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "68500", "70000"));
    engine.finish();

    REQUIRE(engine.result().executions_.size() == 1);
    // Against the 12th's bar, at the limit.
    CHECK(engine.result().executions_[0].fill_.price_ == Decimal64::parse("69000"));
    CHECK(engine.result().executions_[0].fill_.at_ > utc(2026, 3, 11, 6, 30));
    CHECK(engine.result().expired_day_orders_ == 0);
  }

  SECTION("reached only in a later one: expired") {
    backtest::Backtester<LimitFromQuote> engine(LimitFromQuote{}, replay_config());
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
    engine.feed(quote_at(utc(2026, 3, 11, 1, 0), "70000"));
    engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "68000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "69500", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "68000", "70000"));
    engine.finish();

    REQUIRE(engine.result().executions_.empty());
    CHECK(engine.result().expired_day_orders_ == 1);
  }
}

TEST_CASE("a DAY order on open-stamped daily bars lasts one session too") {
  // The stamp every shipped adapter uses: Seoul midnight, with a nominal period end. The decision at
  // the 10th's nominal end is the 11th's start, so the order trades in the 11th's session and
  // expires with it.
  backtest::Backtester<RestsOneLimit> engine(RestsOneLimit{}, replay_config());
  engine.feed(open_stamped(utc(2026, 3, 9, 15, 0), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(open_stamped(utc(2026, 3, 10, 15, 0), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(open_stamped(utc(2026, 3, 11, 15, 0), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(open_stamped(utc(2026, 3, 12, 15, 0), BarInterval::ONE_DAY, "45000", "70000"));
  engine.finish();

  REQUIRE(engine.strategy().working_seen_ == std::vector<std::size_t>{0, 0, 0, 0});
  REQUIRE(engine.result().executions_.empty());
  REQUIRE(engine.result().expired_day_orders_ == 1);
}

TEST_CASE("a DAY order on minute bars rests through its session and expires at the close") {
  // Both ends of the session rule on intraday bars: later bars of the same session leave it
  // working, the bar ending at the close withdraws it, and a missing last bar is caught by the
  // next session's first.
  SECTION("the bar ending at the close") {
    backtest::Backtester<RestsOneLimit> engine(RestsOneLimit{}, replay_config());
    engine.feed(moving_bar(session_minute(1), BarInterval::ONE_MINUTE, "70000", "70000"));
    engine.feed(moving_bar(session_minute(2), BarInterval::ONE_MINUTE, "70000", "70000"));
    // 15:29 and 15:30 KST: the second ends at the close.
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 29), BarInterval::ONE_MINUTE, "70000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_MINUTE, "70000", "70000"));
    engine.feed(moving_bar(utc(2026, 3, 11, 0, 1), BarInterval::ONE_MINUTE, "45000", "70000"));
    engine.finish();

    REQUIRE(engine.strategy().working_seen_ == std::vector<std::size_t>{0, 1, 1, 0, 0});
    REQUIRE(engine.result().executions_.empty());
    REQUIRE(engine.result().expired_day_orders_ == 1);
  }

  SECTION("no bar at the close: the next session's first bar") {
    backtest::Backtester<RestsOneLimit> engine(RestsOneLimit{}, replay_config());
    engine.feed(moving_bar(session_minute(1), BarInterval::ONE_MINUTE, "70000", "70000"));
    engine.feed(moving_bar(session_minute(2), BarInterval::ONE_MINUTE, "70000", "70000"));
    // 09:01 KST the next day, reaching the limit: withdrawn before it is filled.
    engine.feed(moving_bar(utc(2026, 3, 11, 0, 1), BarInterval::ONE_MINUTE, "45000", "70000"));
    engine.finish();

    REQUIRE(engine.strategy().working_seen_ == std::vector<std::size_t>{0, 1, 0});
    REQUIRE(engine.result().executions_.empty());
    REQUIRE(engine.result().expired_day_orders_ == 1);
  }
}

// ---------------------------------------------------------------------------
// The guards that read `working_` fire in a replay
//
// Every case here is a rule the library already enforces live. Each one fails
// against the build before `Backtester` filled the field in.
// ---------------------------------------------------------------------------

namespace {

/** A limit that the market in these tests never reaches, so an order at it rests. */
strategy::Price far_below() {
  return *strategy::Price::checked(Decimal64::parse("50000"), krx_grid());
}

/** A flat bar for `instrument` at `close`, for interleaving two instruments in one replay. */
strategy::BarEvent bar_for(
  InstrumentId instrument,
  Clock::time_point at,
  const char* close,
  BarInterval interval = BarInterval::ONE_DAY
) {
  Bar bar;
  bar.timestamp_ = at;
  bar.open_ = Decimal64::parse(close);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(std::move(instrument), interval, BarStamp::CLOSE, bar);
}

/** A second KRX instrument, so a replay can interleave two of them. */
InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

Instrument hynix_reference() {
  auto reference = samsung_reference();
  reference.id_ = hynix();
  return reference;
}

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

/**
 * Replays `bars` flat one-minute bars at 70,000 through `engine`, all in one session: an order these
 * tests rest stays working across them, as a DAY order does live. Across daily bars it would expire.
 */
template <typename Engine>
void replay_flat_bars(Engine& engine, int bars) {
  for (int minute = 1; minute <= bars; ++minute) {
    engine.feed(
      moving_bar(session_minute(minute), BarInterval::ONE_MINUTE, "70000", "70000")
    );
  }
  engine.finish();
}

/** Replays `bars` flat daily bars at 70,000 through `engine`, one session each. */
template <typename Engine>
void replay_flat_days(Engine& engine, int bars) {
  for (int day = 0; day < bars; ++day) {
    engine.feed(
      moving_bar(utc(2026, 3, 10 + day, 6, 30), BarInterval::ONE_DAY, "70000", "70000")
    );
  }
  engine.finish();
}

} // namespace

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

  SECTION("on, across daily bars: yesterday's buy has expired and commits nothing") {
    auto config = replay_config();
    config.cash_ = Decimal64::parse("1000000");
    config.limits_.enforce_buying_power_ = true;

    backtest::Backtester<RecordsBuyingPower> engine(RecordsBuyingPower{}, std::move(config));
    replay_flat_days(engine, 3);

    REQUIRE(engine.strategy().seen_ == std::vector<std::optional<Decimal64>>(3, million));
    REQUIRE(engine.result().expired_day_orders_ == 2);
    REQUIRE(engine.result().unfilled_ == 1);
  }
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
  // and its reference resolves. The queue was once empty for the length of the loop,
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

} // namespace

#if defined(ATL_HAS_EIGEN)
TEST_CASE("one replay spanning a rate change charges each rate on its own dates") {
  // Deliberately one replay with one policy. Constructing two frozen models at two dates already
  // worked before per-execution rate selection, so a test doing that would prove nothing.
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
#endif

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

#if defined(ATL_HAS_EIGEN)
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
#endif

/** Records the book it decides on each bar; buys two on the first bar and one at a limit on the fourth. */
struct ReadsTheBookEachBar {
  struct Seen {
    std::optional<Decimal64> cash_;
    std::optional<Decimal64> buying_power_;
    std::optional<Decimal64> quantity_;
    std::optional<Decimal64> mark_;
    std::optional<Decimal64> market_value_;
  };
  std::vector<Seen> seen_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto& book = ctx.portfolio();
    Seen seen;
    seen.cash_ = book.cash_in(Currency{"KRW"});
    seen.buying_power_ = book.buying_power_in(Currency{"KRW"});
    seen.quantity_ = book.quantity_of(bar.instrument_);
    for (const auto& position : book.positions_) {
      if (position.instrument_ == bar.instrument_) {
        seen.mark_ = position.last_price_;
        seen.market_value_ = position.market_value_.amount_;
      }
    }
    seen_.push_back(seen);
    if (seen_.size() == 1) {
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("2"))
        )
      );
    } else if (seen_.size() == 4) {
      const auto reference = ctx.instrument(bar.instrument_);
      if (!reference) {
        return;
      }
      const auto limit = strategy::Price::checked(Decimal64::parse("1000000"), reference->price_rule_);
      if (!limit) {
        return;
      }
      ctx.submit(
        strategy::OrderIntent::limit(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")), *limit
        )
      );
    }
  }
};

TEST_CASE("a strategy on an unvalued bar reads exact figures rather than the failed total", "[backtest][caveats]") {
  // The replay goes on past a bar whose equity total will not
  // fit, so `on_bar` runs there. What it reads is per lot and per currency -- cash, buying power (here
  // equal to cash: the buying-power check is off, so no resting buy is committed against it), the
  // quantity, the lot's own mark and value -- and the one portfolio-wide sum the guard takes, the gross
  // exposure, is its own arithmetic without cash. None of them is `equity_in`'s sum.
  auto config = replay_config();
  config.cash_ = Decimal64::parse("9223372036854000000");
  config.limits_.max_gross_exposure_ = Decimal64::parse("10000000");
  backtest::Backtester<ReadsTheBookEachBar> engine(ReadsTheBookEachBar{}, config);
  engine.feed(moving_bar(utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(moving_bar(utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("2")));

  engine.feed(moving_bar(utc(2026, 3, 13, 6, 30), BarInterval::ONE_DAY, "70000", "1000000"));
  REQUIRE_FALSE(engine.result().halted_);
  REQUIRE(engine.result().unvalued_bars_ == 1);
  REQUIRE_FALSE(engine.ledger().equity_in(Currency{"KRW"}));

  // The strategy ran on that bar, after the gap was counted, and read exact figures.
  REQUIRE(engine.strategy().seen_.size() == 4);
  const auto& unvalued = engine.strategy().seen_.back();
  CHECK(unvalued.cash_ == engine.ledger().cash_in(Currency{"KRW"}));
  CHECK(unvalued.buying_power_ == unvalued.cash_);
  CHECK(unvalued.quantity_ == std::optional<Decimal64>(Decimal64::parse("2")));
  CHECK(unvalued.mark_ == std::optional<Decimal64>(Decimal64::parse("1000000")));
  CHECK(unvalued.market_value_ == std::optional<Decimal64>(Decimal64::parse("2000000")));

  // And the order it placed there passed the exposure ceiling -- (2 + 1) x 1,000,000 -- and filled.
  engine.feed(moving_bar(utc(2026, 3, 14, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  CHECK(engine.result().refused_.empty());
  CHECK(engine.result().rejected_.empty());
  CHECK(engine.ledger().quantity_of(samsung()) == std::optional<Decimal64>(Decimal64::parse("3")));
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
  // asserted** -- today it is the position valuation, not the cash total. What this test pins is
  // the engine's response to `apply` refusing: a typed halt rather than a fill that quietly
  // disappears.
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

#if defined(ATL_HAS_EIGEN)
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
#endif

TEST_CASE("a dated rate for another market's calendar is refused, not charged a day early") {
  // A replay derives one local date from its single `SessionWindow`, so a US
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

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a cost total that will not sum is stated as incomplete rather than short") {
  // No bound holds here: two ledger-accepted executions, each carrying an
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
  // this one reaches `summarize` the way a replay does -- through the fill model,
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
  // The last cost figure once summed across currencies: one won
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

TEST_CASE("a replay whose every trade is outside its curve reports no figures and counts them") {
  // The curve is in `currency_` only. A won replay of a dollar stock that rose by half used to report a
  // total return of zero: the curve held the opening cash and nothing else, which read as a flat result.
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference(), apple_reference()};
  config.limits_.permitted_phases_ = std::nullopt;
  const auto apple = apple_reference().id_;

  backtest::Backtester<HoldsTen> abroad(HoldsTen{}, config);
  abroad.feed(bar_for(apple, utc(2026, 3, 10, 6, 31), "200"));
  abroad.feed(bar_for(apple, utc(2026, 3, 11, 6, 31), "250"));
  abroad.feed(bar_for(apple, utc(2026, 3, 12, 6, 31), "300"));
  abroad.finish();
  const auto report = backtest::summarize(abroad.result(), abroad.fills(), 252.0);
  REQUIRE(report.executions_ == 1);
  CHECK(report.caveats_.other_currency_executions_ == 1);
  CHECK_FALSE(math::observed(report.total_return_));
  CHECK_FALSE(math::observed(report.sharpe_));
  CHECK_FALSE(math::observed(report.annualized_sharpe_));
  CHECK_FALSE(math::observed(report.sortino_));
  CHECK_FALSE(math::observed(report.max_drawdown_));
  CHECK_FALSE(math::observed(report.total_turnover_));

  // With a trade in the curve's currency too, the figures are the curve's, and the count says what they omit.
  backtest::Backtester<HoldsTen> mixed(HoldsTen{}, config);
  mixed.feed(bar_for(samsung(), utc(2026, 3, 10, 6, 30), "70000"));
  mixed.feed(bar_for(apple, utc(2026, 3, 10, 6, 31), "200"));
  mixed.feed(bar_for(samsung(), utc(2026, 3, 11, 6, 30), "77000"));
  mixed.feed(bar_for(apple, utc(2026, 3, 11, 6, 31), "250"));
  mixed.feed(bar_for(samsung(), utc(2026, 3, 12, 6, 30), "84000"));
  mixed.feed(bar_for(apple, utc(2026, 3, 12, 6, 31), "300"));
  mixed.finish();
  const auto both = backtest::summarize(mixed.result(), mixed.fills());
  REQUIRE(both.executions_ == 2);
  CHECK(both.caveats_.other_currency_executions_ == 1);
  CHECK(math::observed(both.total_return_));
  CHECK(both.total_return_ > 0);
  CHECK_FALSE(math::observed(both.total_turnover_));

  // In its own currency the same dollar replay has its figures and nothing to count.
  config.currency_ = Currency::usd();
  config.cash_ = Decimal64::parse("100000");
  backtest::Backtester<HoldsTen> home(HoldsTen{}, config);
  home.feed(bar_for(apple, utc(2026, 3, 10, 6, 31), "200"));
  home.feed(bar_for(apple, utc(2026, 3, 11, 6, 31), "250"));
  home.feed(bar_for(apple, utc(2026, 3, 12, 6, 31), "300"));
  home.finish();
  const auto at_home = backtest::summarize(home.result(), home.fills());
  CHECK(at_home.caveats_.other_currency_executions_ == 0);
  CHECK(at_home.total_return_ > 0);
}
#endif

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
  // The stop sells ten from a flat book, so this replay shorts.
  backtest::Backtester<PlacesAStop> engine(PlacesAStop{}, short_replay_config());
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

TEST_CASE("a stop released inside the bar whose limit was not marketable stays working", "[trigger]") {
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
  // The parity: the working row a replay shows is a stop row with its trigger,
  // so the target rule refuses exactly as it would against a live listing.
  // Minute bars of one session: across daily bars the stop would have expired before bar 3.
  backtest::Backtester<StopsThenTargetsFlat> engine(StopsThenTargetsFlat{}, replay_config());
  engine.feed(moving_bar(session_minute(1), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(session_minute(2), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(session_minute(3), BarInterval::ONE_MINUTE, "70000", "70000"));

  // Bar 3's flatten withdraws the stop and sells nothing beside it; bar 4 restates it against a book
  // with no stop, and bar 5 fills the sale -- the sequence a live run takes.
  engine.feed(moving_bar(session_minute(4), BarInterval::ONE_MINUTE, "70000", "70000"));
  engine.feed(moving_bar(session_minute(5), BarInterval::ONE_MINUTE, "70000", "70000"));
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

  // Credited the bar's 69,000 low with the limit far below: ten basis points would record 68,931,
  // below every price the bar showed, so it is held at the low.
  const auto out_of_bar = slipping.decide(
    sell_stop("69500", "68000"), ohlc("70000", "70500", "69000", "69200"), krx_grid(), fill_date()
  );
  REQUIRE(out_of_bar.execution_);
  CHECK(out_of_bar.execution_->fill_.price_ == Decimal64::parse("69000"));

  // Where the bar leaves room, slippage still applies: opened at 69,200 through the trigger and
  // credited the open under `AT_OPEN_IF_BETTER`, it slips to 69,130.8, above the 69,000 low.
  const backtest::NextBarFillModel roomy_model(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::of_basis_points(Decimal64::parse("10")),
    backtest::LimitFillPrice::AT_OPEN_IF_BETTER
  );
  const auto roomy = roomy_model.decide(
    sell_stop("69500", "68000"), ohlc("69200", "69400", "69000", "69100"), krx_grid(), fill_date()
  );
  REQUIRE(roomy.execution_);
  CHECK(roomy.execution_->fill_.price_ == Decimal64::parse("69130.8"));
}

namespace {

/** Places one sell stop at a stated trigger and limit on the first bar. */
struct PlacesStopAt {
  const char* trigger_;
  const char* limit_;
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
      *strategy::Price::checked(Decimal64::parse(trigger_), PriceRule::krx_equity()),
      *strategy::Price::checked(Decimal64::parse(limit_), PriceRule::krx_equity())
    ));
  }
};

strategy::BarEvent stop_bar(
  int day,
  const char* o,
  const char* h,
  const char* l,
  const char* c,
  BarInterval interval = BarInterval::ONE_DAY
) {
  Bar bar;
  // A daily bar of the `day`th, or the one-minute bar ending `day` minutes past 10:00 on the 10th.
  bar.timestamp_ = interval == BarInterval::ONE_DAY ? utc(2026, 3, day, 6, 30) : session_minute(day);
  bar.open_ = Decimal64::parse(o);
  bar.high_ = Decimal64::parse(h);
  bar.low_ = Decimal64::parse(l);
  bar.close_ = Decimal64::parse(c);
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(samsung(), interval, BarStamp::CLOSE, bar);
}

} // namespace

TEST_CASE("a stop released on a daily bar and not filled expires with that session", "[trigger]") {
  // The same bars as daily ones. Released on the 11th and unfilled at its close, the stop -- now a
  // limit -- expires with the session; the 12th's high would have filled it, and live it was gone.
  backtest::Backtester<PlacesStopAt> engine(PlacesStopAt{"69500", "69800"}, short_replay_config());
  engine.feed(stop_bar(10, "70000", "70000", "70000", "70000"));
  engine.feed(stop_bar(11, "70000", "70500", "69000", "69200"));
  engine.feed(stop_bar(12, "69900", "70000", "69700", "69950"));
  engine.finish();

  REQUIRE(engine.result().executions_.empty());
  CHECK(engine.result().stop_releases_ == 1);
  CHECK(engine.result().expired_day_orders_ == 1);
  CHECK(engine.result().unfilled_ == 0);
}

TEST_CASE("negative slippage never lifts a stop fill past its bar", "[trigger]") {
  // `SlippageModel` accepts a negative figure, which moves a fill
  // in the order's favour; past the bar's opposite extreme that is a price the bar never showed.
  const backtest::NextBarFillModel generous(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::of_basis_points(Decimal64::parse("-1000"))
  );
  const auto sell = generous.decide(
    sell_stop("69500", "68000"), ohlc("69200", "69400", "69000", "69100"), krx_grid(), fill_date()
  );
  REQUIRE(sell.execution_);
  CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69400"));

  const auto buy = generous.decide(
    buy_stop("70500", "72000"), ohlc("70800", "71000", "70600", "70900"), krx_grid(), fill_date()
  );
  REQUIRE(buy.execution_);
  CHECK(buy.execution_->fill_.price_ == Decimal64::parse("70600"));
}

TEST_CASE("favourable slippage cannot credit a price the bar reached before the release", "[trigger]") {
  // `70,000 -> 70,500 -> 69,000 -> 69,200` is a valid path for this
  // bar: its high came before the 69,500 trigger, and after the release the price never returned
  // above 69,500. Negative slippage used to lift the fill to that high.
  const backtest::NextBarFillModel generous(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::of_basis_points(Decimal64::parse("-1000"))
  );
  const auto sell = generous.decide(
    sell_stop("69500", "68000"), ohlc("70000", "70500", "69000", "69200"), krx_grid(), fill_date()
  );
  REQUIRE(sell.execution_);
  CHECK(sell.intrabar_release_fill_);
  CHECK(sell.execution_->fill_.price_ == Decimal64::parse("69000"));

  const auto buy = generous.decide(
    buy_stop("70500", "72000"), ohlc("70000", "71000", "69500", "70800"), krx_grid(), fill_date()
  );
  REQUIRE(buy.execution_);
  CHECK(buy.intrabar_release_fill_);
  CHECK(buy.execution_->fill_.price_ == Decimal64::parse("71000"));
}

namespace {

/**
 * Holds ten behind a stop and answers order updates: the first update it sees for the stop states
 * flat, and a second, for another order, buys the instrument back instead.
 */
struct ChangesItsMindInOneBatch {
  int bars_{0};
  int updates_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    if (bars_ == 1) {
      strategy::TargetPosition entry;
      entry.instrument_ = bar.instrument_;
      entry.target_quantity_ = Decimal64::parse("10");
      ctx.submit(entry);
    } else if (bars_ == 2) {
      ctx.submit(strategy::OrderIntent::stop_limit(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("10")),
        *strategy::Price::checked(Decimal64::parse("66500"), PriceRule::krx_equity()),
        *strategy::Price::checked(Decimal64::parse("66000"), PriceRule::krx_equity())
      ));
      ctx.submit(strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")),
        *strategy::Price::checked(Decimal64::parse("60000"), PriceRule::krx_equity())
      ));
    }
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot& order) {
    ++updates_;
    if (order.type_ == OrderType::STOP_LIMIT) {
      strategy::TargetPosition flat;
      flat.instrument_ = order.instrument_;
      flat.target_quantity_ = Decimal64::parse("0");
      ctx.submit(flat);
    } else if (order.type_ == OrderType::LIMIT && order.side_ == OrderSide::BUY) {
      ctx.submit(strategy::OrderIntent::limit(
        order.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")),
        *strategy::Price::checked(Decimal64::parse("60000"), PriceRule::krx_equity())
      ));
    }
  }
};

} // namespace

TEST_CASE("a replay releases a deferred target only after the whole update batch", "[trigger]") {
  // The stop's update makes the strategy state flat, which withdraws
  // the stop and defers flat; a later update in the same batch answers with an `OrderIntent` for the
  // instrument, which supersedes the fallback. Released after the first update, the sell was already
  // queued and both would trade.
  backtest::Backtester<ChangesItsMindInOneBatch> engine(ChangesItsMindInOneBatch{}, replay_config());
  engine.feed(stop_bar(10, "70000", "70000", "70000", "70000"));
  engine.feed(stop_bar(11, "70000", "70000", "70000", "70000"));
  engine.feed(stop_bar(12, "70000", "70000", "70000", "70000"));
  engine.feed(stop_bar(13, "70000", "70000", "70000", "70000"));
  engine.finish();

  for (const auto& execution : engine.result().executions_) {
    INFO("execution " << execution.fill_.tag_);
    CHECK(execution.fill_.side_ == OrderSide::BUY);
  }
}

TEST_CASE("a limit fill held at the bar's far extreme is counted as bounded", "[limit-bounds]") {
  // Negative slippage from a better open reaches past the bar: held at the low (a buy) or the high (a
  // sell), which the counter must report like any other bound.
  const backtest::NextBarFillModel generous(
    backtest::CommissionModel::none(),
    backtest::SlippageModel::of_basis_points(Decimal64::parse("-2000")),
    backtest::LimitFillPrice::AT_OPEN_IF_BETTER
  );
  const auto buy = generous.decide(
    order_of(OrderSide::BUY, "10", OrderType::LIMIT, "72000"),
    ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()
  );
  REQUIRE(buy.execution_);
  CHECK(buy.execution_->fill_.price_ == Decimal64::parse("69000"));
  CHECK(buy.bounded_);

  const auto sell = generous.decide(
    order_of(OrderSide::SELL, "10", OrderType::LIMIT, "68000"),
    ohlc("70000", "71000", "69000", "70500"), krx_grid(), fill_date()
  );
  REQUIRE(sell.execution_);
  CHECK(sell.execution_->fill_.price_ == Decimal64::parse("71000"));
  CHECK(sell.bounded_);
}

// ---------------------------------------------------------------------------------------------
// OrderUpdateWatch
// ---------------------------------------------------------------------------------------------

namespace {

using WatchRow = WorkingOrder<algotradelib::runner::DryTag>;

/** A listed row: the reference's two parts, a status and a filled quantity. */
WatchRow watch_row(
  const std::string& primary, const std::string& secondary, OrderStatus status, const char* filled = "0"
) {
  WatchRow row;
  row.ref_ = OrderRef<algotradelib::runner::DryTag>{primary, secondary};
  row.order_.order_id_ = primary;
  row.order_.status_ = status;
  row.order_.quantity_ = Decimal64::parse("10");
  row.order_.execution_.filled_quantity_ = Decimal64::parse(filled);
  return row;
}

/** The order ids of a batch of updates, in the order reported. */
std::vector<std::string> ids_of(const std::vector<OrderSnapshot>& updates) {
  std::vector<std::string> ids;
  for (const auto& update : updates) {
    ids.push_back(update.order_id_);
  }
  return ids;
}

/**
 * The watch's rule written the long way, one scan of the listing per row: what `OrderUpdateWatch`
 * must report, whatever it keeps to find a reference quickly.
 */
class ScanningWatch {
public:
  std::vector<OrderSnapshot> changed(const std::vector<WatchRow>& rows) {
    struct Entry {
      OrderRef<algotradelib::runner::DryTag> ref_;
      OrderSnapshot last_;
      bool contradicted_{false};
    };
    std::vector<Entry> next;
    for (const auto& row : rows) {
      if (row.reserved_) {
        continue;
      }
      if (row.ref_.primary_.empty() && row.ref_.secondary_.empty()) {
        ++untracked_;
        continue;
      }
      Entry* already = nullptr;
      for (auto& entry : next) {
        if (entry.ref_ == row.ref_) {
          already = &entry;
          break;
        }
      }
      if (already == nullptr) {
        next.push_back(Entry{row.ref_, row.order_, false});
      } else if (!algotradelib::runner::order_update_detail::same_observable_state(already->last_, row.order_)) {
        already->contradicted_ = true;
      }
    }
    std::vector<OrderSnapshot> updates;
    std::vector<std::pair<OrderRef<algotradelib::runner::DryTag>, OrderSnapshot>> kept;
    for (const auto& entry : next) {
      if (entry.contradicted_) {
        ++contradicted_;
        continue;
      }
      const OrderSnapshot* held = nullptr;
      for (const auto& old : tracked_) {
        if (old.first == entry.ref_) {
          held = &old.second;
          break;
        }
      }
      if (held == nullptr || !algotradelib::runner::order_update_detail::same_observable_state(*held, entry.last_)) {
        updates.push_back(entry.last_);
      }
      kept.emplace_back(entry.ref_, entry.last_);
    }
    tracked_ = std::move(kept);
    return updates;
  }

  std::size_t tracked_orders() const noexcept { return tracked_.size(); }
  std::size_t untracked_rows() const noexcept { return untracked_; }
  std::size_t contradicted_rows() const noexcept { return contradicted_; }

private:
  std::vector<std::pair<OrderRef<algotradelib::runner::DryTag>, OrderSnapshot>> tracked_;
  std::size_t untracked_{0};
  std::size_t contradicted_{0};
};

} // namespace

TEST_CASE("an order-update watch reports the rows that changed, in the listing's order", "[order-updates]") {
  algotradelib::runner::OrderUpdateWatch<algotradelib::runner::DryTag> watch;

  // New rows are reported in the order listed; a second part distinguishes two orders.
  CHECK(
    ids_of(watch.changed({
      watch_row("c", "", OrderStatus::PENDING),
      watch_row("a", "1", OrderStatus::PENDING),
      watch_row("a", "2", OrderStatus::PENDING),
    })) == std::vector<std::string>{"c", "a", "a"}
  );
  CHECK(watch.tracked_orders() == 3);

  // Nothing moved; then one fills in part and one leaves.
  CHECK(watch.changed({
    watch_row("c", "", OrderStatus::PENDING),
    watch_row("a", "1", OrderStatus::PENDING),
    watch_row("a", "2", OrderStatus::PENDING),
  }).empty());
  const auto moved = watch.changed({
    watch_row("a", "2", OrderStatus::PARTIAL_FILLED, "4"),
    watch_row("c", "", OrderStatus::PENDING),
  });
  REQUIRE(moved.size() == 1);
  CHECK(moved.front().status_ == OrderStatus::PARTIAL_FILLED);
  CHECK(watch.tracked_orders() == 2);

  // An order that left and returns is new again; a quantity written at another scale is no change.
  auto rescaled = watch_row("c", "", OrderStatus::PENDING);
  rescaled.order_.quantity_ = Decimal64(100, 1);
  CHECK(
    ids_of(watch.changed({
      rescaled,
      watch_row("a", "1", OrderStatus::PENDING),
      watch_row("a", "2", OrderStatus::PARTIAL_FILLED, "4"),
    })) == std::vector<std::string>{"a"}
  );

  // A baseline taken silently reports nothing, and is what the next listing is compared with.
  algotradelib::runner::OrderUpdateWatch<algotradelib::runner::DryTag> adopted;
  adopted.adopt({watch_row("x", "", OrderStatus::PENDING), watch_row("y", "", OrderStatus::PENDING)});
  CHECK(adopted.tracked_orders() == 2);
  CHECK(
    ids_of(adopted.changed({
      watch_row("x", "", OrderStatus::PENDING),
      watch_row("y", "", OrderStatus::PARTIAL_FILLED, "1"),
      watch_row("z", "", OrderStatus::PENDING),
    })) == std::vector<std::string>{"y", "z"}
  );
}

TEST_CASE("an order listed twice is one order, and two disagreeing readings are none", "[order-updates]") {
  algotradelib::runner::OrderUpdateWatch<algotradelib::runner::DryTag> watch;
  auto reserved = watch_row("r", "", OrderStatus::PENDING);
  reserved.reserved_ = true;

  // `b` is stated twice and disagrees, between two orders that are stated once; `d` twice and agrees.
  const auto first = watch.changed({
    watch_row("a", "", OrderStatus::PENDING),
    watch_row("b", "", OrderStatus::PENDING),
    watch_row("c", "", OrderStatus::PENDING),
    watch_row("d", "", OrderStatus::PENDING),
    watch_row("b", "", OrderStatus::PARTIAL_FILLED, "3"),
    watch_row("d", "", OrderStatus::PENDING),
    watch_row("", "", OrderStatus::PENDING),
    reserved,
  });
  CHECK(ids_of(first) == std::vector<std::string>{"a", "c", "d"});
  CHECK(watch.contradicted_rows() == 1);
  CHECK(watch.untracked_rows() == 1);
  CHECK(watch.tracked_orders() == 3);

  // The contradicted order kept no baseline, so it is new once it is stated once; the orders listed
  // after it are still compared with their own last reading, not with a neighbour's.
  const auto second = watch.changed({
    watch_row("a", "", OrderStatus::PENDING),
    watch_row("b", "", OrderStatus::PENDING),
    watch_row("c", "", OrderStatus::PENDING),
    watch_row("d", "", OrderStatus::PARTIAL_FILLED, "2"),
  });
  CHECK(ids_of(second) == std::vector<std::string>{"b", "d"});
  CHECK(watch.tracked_orders() == 4);
  CHECK(watch.contradicted_rows() == 1);
}

TEST_CASE("an order-update watch reports what a row-by-row scan of the listings reports", "[order-updates]") {
  // Listings drawn from a small pool of references, so orders repeat, leave, return and are stated
  // twice. A fixed sequence: the same every run.
  std::uint32_t state = 20261013;
  const auto draw = [&state](std::uint32_t below) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % below;
  };
  const std::array<OrderStatus, 3> statuses{OrderStatus::PENDING, OrderStatus::PARTIAL_FILLED, OrderStatus::UNKNOWN};
  const std::array<const char*, 3> fills{"0", "3", "7"};

  algotradelib::runner::OrderUpdateWatch<algotradelib::runner::DryTag> watch;
  ScanningWatch scanning;
  for (int listing = 0; listing < 400; ++listing) {
    std::vector<WatchRow> rows;
    const auto count = draw(40);
    for (std::uint32_t at = 0; at < count; ++at) {
      const auto primary = draw(25) == 0 ? std::string() : "o" + std::to_string(draw(12));
      const auto secondary = draw(4) == 0 ? "s" + std::to_string(draw(2)) : std::string();
      // Most rows keep the reading the reference had last; some move.
      const bool moves = draw(5) == 0;
      auto row = watch_row(
        primary, secondary, moves ? statuses[draw(3)] : OrderStatus::PENDING, moves ? fills[draw(3)] : "0"
      );
      row.reserved_ = draw(20) == 0;
      rows.push_back(row);
    }
    const auto expected = scanning.changed(rows);
    const auto reported = watch.changed(rows);
    INFO("listing " << listing << " of " << rows.size() << " rows");
    REQUIRE(reported.size() == expected.size());
    for (std::size_t at = 0; at < reported.size(); ++at) {
      CHECK(reported[at].order_id_ == expected[at].order_id_);
      CHECK(algotradelib::runner::order_update_detail::same_observable_state(reported[at], expected[at]));
    }
    REQUIRE(watch.tracked_orders() == scanning.tracked_orders());
    REQUIRE(watch.untracked_rows() == scanning.untracked_rows());
    REQUIRE(watch.contradicted_rows() == scanning.contradicted_rows());
  }
  // The sequence did exercise the rules it is there for.
  CHECK(scanning.contradicted_rows() > 20);
  CHECK(scanning.untracked_rows() > 20);
}

// ---------------------------------------------------------------------------
// on_bars: one call per period end and interval, with every instrument's bar
// ---------------------------------------------------------------------------

namespace {

/** A bar of `id` stamped at `at`, opening at `open` and closing at `close`. */
strategy::BarEvent slice_bar(
  const InstrumentId& id,
  Clock::time_point at,
  BarInterval interval,
  const char* open,
  const char* close,
  BarStamp stamp = BarStamp::CLOSE
) {
  Bar bar;
  bar.timestamp_ = at;
  bar.open_ = Decimal64::parse(open);
  const auto o = Decimal64::parse(open);
  const auto c = Decimal64::parse(close);
  bar.high_ = decimal_less(o, c) ? c : o;
  bar.low_ = decimal_less(o, c) ? o : c;
  bar.close_ = c;
  bar.volume_ = Decimal64::parse("1000");
  return strategy::make_bar_event(id, interval, stamp, bar);
}

backtest::ReplayConfig pair_config() {
  auto config = replay_config();
  config.instruments_ = {samsung_reference(), hynix_reference()};
  return config;
}

/** Records every slice it is given, and buys ten of `buy_` on the slice numbered `buy_on_`. */
struct RecordsSlices {
  struct Seen {
    Clock::time_point end_;
    BarInterval interval_{BarInterval::ONE_DAY};
    std::vector<std::string> symbols_;
    Clock::time_point now_;
    bool cached_to_its_bar_{true};
    std::size_t working_{0};
    std::optional<Decimal64> first_price_;
  };
  std::vector<Seen> seen_;
  std::vector<std::string> events_;
  std::optional<InstrumentId> buy_;
  std::size_t buy_on_{0};
  std::vector<Decimal64> fill_prices_;
  std::optional<std::size_t> slices_at_stop_;

  template <class Ctx>
  void on_stop(Ctx&) {
    slices_at_stop_ = seen_.size();
  }

  template <class Ctx>
  void on_bars(Ctx& ctx, const strategy::BarSlice& slice) {
    Seen seen;
    seen.first_price_ = ctx.last_price(slice.bars_.front().instrument_);
    seen.end_ = slice.period_end_;
    seen.interval_ = slice.interval_;
    seen.now_ = ctx.now();
    seen.working_ = ctx.portfolio().working_.size();
    for (const auto& event : slice.bars_) {
      seen.symbols_.push_back(event.instrument_.symbol_);
#if defined(ATL_HAS_EIGEN)
      const auto* series = ctx.bars(event.instrument_, slice.interval_);
      seen.cached_to_its_bar_ = seen.cached_to_its_bar_ && series != nullptr &&
        series->newest_instant() == event.bar_.timestamp_;
#endif
    }
    if (buy_ && seen_.size() == buy_on_) {
      ctx.submit(strategy::OrderIntent::market(
        *buy_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      ).with_tag("slice"));
    }
    seen_.push_back(std::move(seen));
    events_.push_back("bars");
  }

  template <class Ctx>
  void on_quote(Ctx&, const strategy::QuoteEvent&) {
    events_.push_back("quote");
  }

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill& fill) {
    fill_prices_.push_back(fill.price_);
  }
};

strategy::QuoteEvent quote_at(const InstrumentId& id, Clock::time_point at, const char* price) {
  strategy::QuoteEvent event;
  event.quote_.instrument_ = id;
  event.quote_.last_price_ = Decimal64::parse(price);
  event.quote_.received_at_ = at;
  return event;
}

} // namespace

TEST_CASE("on_bars receives each period once with every instrument's bar") {
  backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
  const auto d1 = utc(2026, 3, 10, 6, 30);
  const auto d2 = utc(2026, 3, 11, 6, 30);
  const auto d3 = utc(2026, 3, 12, 6, 30);
  engine.feed(slice_bar(samsung(), d1, BarInterval::ONE_DAY, "100", "101"));
  engine.feed(slice_bar(hynix(), d1, BarInterval::ONE_DAY, "200", "201"));
  // Nothing is delivered while the period may still receive bars.
  CHECK(engine.strategy().seen_.empty());
  engine.feed(slice_bar(samsung(), d2, BarInterval::ONE_DAY, "102", "103"));
  // A later period delivered the first, with both instruments.
  REQUIRE(engine.strategy().seen_.size() == 1);
  // The second period has no bar for one instrument, so that instrument is absent.
  engine.feed(slice_bar(samsung(), d3, BarInterval::ONE_DAY, "104", "105"));
  engine.feed(slice_bar(hynix(), d3, BarInterval::ONE_DAY, "204", "205"));
  engine.finish();

  const auto& seen = engine.strategy().seen_;
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].end_ == d1);
  CHECK(seen[0].symbols_ == std::vector<std::string>{"005930", "000660"});
  CHECK(seen[1].end_ == d2);
  CHECK(seen[1].symbols_ == std::vector<std::string>{"005930"});
  CHECK(seen[2].end_ == d3);
  CHECK(seen[2].symbols_ == std::vector<std::string>{"005930", "000660"});
  for (const auto& one : seen) {
    // The clock stands at the period, and each instrument's cached series ends at its bar.
    CHECK(one.now_ == one.end_);
    CHECK(one.cached_to_its_bar_);
  }
}

TEST_CASE("an order decided in on_bars fills on a later period and never on a bar of its own") {
  RecordsSlices strategy;
  strategy.buy_ = hynix();
  backtest::Backtester<RecordsSlices> engine(std::move(strategy), pair_config());
  // The order for the second instrument is decided after that instrument's bar of the same period was
  // settled, so the first price it can meet is the next period's open.
  engine.feed(slice_bar(samsung(), utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "100", "101"));
  engine.feed(slice_bar(hynix(), utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "200", "210"));
  engine.feed(slice_bar(samsung(), utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "102", "103"));
  engine.feed(slice_bar(hynix(), utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "220", "230"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("220"));
}

TEST_CASE("the last period reaches on_bars at finish before on_stop counts what is left") {
  RecordsSlices strategy;
  strategy.buy_ = samsung();
  strategy.buy_on_ = 1;
  backtest::Backtester<RecordsSlices> engine(std::move(strategy), pair_config());
  engine.feed(slice_bar(samsung(), utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "100", "101"));
  engine.feed(slice_bar(samsung(), utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "102", "103"));
  REQUIRE(engine.strategy().seen_.size() == 1);
  engine.finish();
  // Delivered once, before on_stop, and the order it placed is the one still queued.
  CHECK(engine.strategy().seen_.size() == 2);
  CHECK(engine.strategy().slices_at_stop_ == std::optional<std::size_t>(2));
  CHECK(engine.result().unfilled_ == 1);
}

TEST_CASE("intervals ending at one instant give one slice each in the order first fed") {
  const auto t = utc(2026, 3, 13, 6, 30);
  const auto run = [&](bool minute_first) {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    for (const auto& id : {samsung(), hynix()}) {
      if (minute_first) {
        engine.feed(slice_bar(id, t, BarInterval::ONE_MINUTE, "100", "101"));
        engine.feed(slice_bar(id, t, BarInterval::ONE_DAY, "99", "101"));
      } else {
        engine.feed(slice_bar(id, t, BarInterval::ONE_DAY, "99", "101"));
        engine.feed(slice_bar(id, t, BarInterval::ONE_MINUTE, "100", "101"));
      }
    }
    engine.finish();
    return engine.strategy().seen_;
  };
  for (const bool minute_first : {true, false}) {
    const auto seen = run(minute_first);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].interval_ == (minute_first ? BarInterval::ONE_MINUTE : BarInterval::ONE_DAY));
    CHECK(seen[1].interval_ == (minute_first ? BarInterval::ONE_DAY : BarInterval::ONE_MINUTE));
    for (const auto& one : seen) {
      CHECK(one.symbols_ == std::vector<std::string>{"005930", "000660"});
      CHECK(one.cached_to_its_bar_);
    }
  }
}

TEST_CASE("a slice's order does not meet a same-instant bar of another interval fed after it") {
  // A close-stamped weekly bar has no start the replay can derive, so an order already queued when it
  // settled could fill at its open, days in the past. Every bar ending at the instant settles before
  // the period is delivered, so the minute slice's order waits for the next minute.
  RecordsSlices strategy;
  strategy.buy_ = samsung();
  backtest::Backtester<RecordsSlices> engine(std::move(strategy), replay_config());
  const auto t = utc(2026, 3, 13, 6, 30);
  engine.feed(slice_bar(samsung(), t, BarInterval::ONE_MINUTE, "100", "101"));
  engine.feed(slice_bar(samsung(), t, BarInterval::ONE_WEEK, "50", "101"));
  engine.feed(slice_bar(samsung(), t + std::chrono::minutes(1), BarInterval::ONE_MINUTE, "120", "121"));
  engine.finish();

  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("120"));
}

TEST_CASE("a quote after a period comes after its on_bars which sees the period's own state") {
  backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
  const auto t = utc(2026, 3, 10, 6, 30);
  engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
  engine.feed(slice_bar(hynix(), t, BarInterval::ONE_DAY, "200", "201"));
  engine.feed(quote_at(samsung(), t + std::chrono::minutes(30), "999"));
  engine.finish();

  CHECK(engine.strategy().events_ == std::vector<std::string>{"bars", "quote"});
  REQUIRE(engine.strategy().seen_.size() == 1);
  CHECK(engine.strategy().seen_[0].now_ == t);
  // The period's own close, not the quote's price.
  CHECK(engine.strategy().seen_[0].first_price_ == std::optional<Decimal64>(Decimal64::parse("101")));
  CHECK(engine.result().quotes_ == 1);
}

TEST_CASE("on_bars refuses a bar it could not show consistently before anything changes") {
  const auto t = utc(2026, 3, 10, 6, 30);

  SECTION("a bar for a period a same-instant quote already delivered") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
    engine.feed(quote_at(samsung(), t, "101"));
    REQUIRE(engine.strategy().seen_.size() == 1);
    const auto bars_before = engine.result().bars_;
    CHECK_THROWS_AS(
      engine.feed(slice_bar(hynix(), t, BarInterval::ONE_DAY, "200", "201")), std::invalid_argument
    );
    CHECK(engine.result().bars_ == bars_before);
    engine.finish();
    CHECK(engine.strategy().seen_.size() == 1);
  }

  SECTION("a bar earlier than the period being collected") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
    CHECK_THROWS_AS(
      engine.feed(slice_bar(hynix(), t - std::chrono::hours(24), BarInterval::ONE_DAY, "200", "201")),
      std::invalid_argument
    );
    CHECK(engine.result().bars_ == 1);
  }

  SECTION("a second bar of one instrument and interval for the same period end") {
    // An open-stamped daily bar ends a day after its stamp, so it can share a period end with a
    // close-stamped one of the same instrument.
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, replay_config());
    engine.feed(slice_bar(
      samsung(), t - std::chrono::hours(24), BarInterval::ONE_DAY, "100", "101", BarStamp::OPEN
    ));
    CHECK_THROWS_AS(
      engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101")), std::invalid_argument
    );
    CHECK(engine.result().bars_ == 1);
  }

  SECTION("a bar of another interval at an instant a quote already delivered") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, replay_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_MINUTE, "100", "101"));
    engine.feed(quote_at(samsung(), t, "101"));
    CHECK_THROWS_AS(
      engine.feed(slice_bar(samsung(), t, BarInterval::ONE_WEEK, "50", "101")), std::invalid_argument
    );
    CHECK(engine.result().bars_ == 1);
  }

  SECTION("a quote earlier than a period already delivered") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
    engine.feed(slice_bar(samsung(), t + std::chrono::hours(24), BarInterval::ONE_DAY, "102", "103"));
    REQUIRE(engine.strategy().seen_.size() == 1);
    engine.finish();
    CHECK_THROWS_AS(engine.feed(quote_at(samsung(), t, "100")), std::invalid_argument);
    CHECK(engine.result().quotes_ == 0);
  }

  SECTION("a bar for a period finish already delivered") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
    engine.finish();
    REQUIRE(engine.strategy().seen_.size() == 1);
    CHECK_THROWS_AS(
      engine.feed(slice_bar(hynix(), t, BarInterval::ONE_DAY, "200", "201")), std::invalid_argument
    );
    CHECK(engine.strategy().seen_.size() == 1);
  }

  SECTION("a quote earlier than the period being collected") {
    backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, pair_config());
    engine.feed(slice_bar(samsung(), t, BarInterval::ONE_DAY, "100", "101"));
    CHECK_THROWS_AS(
      engine.feed(quote_at(samsung(), t - std::chrono::minutes(1), "100")), std::invalid_argument
    );
    CHECK(engine.result().quotes_ == 0);
  }
}

TEST_CASE("a period of forty instruments reaches on_bars whole and still refuses a second bar") {
  // Past sixteen bars a period finds an id by a table instead of scanning its slice; every answer must stay
  // the same. Each symbol is listed at all four venues, which makes four instruments.
  auto config = replay_config();
  config.instruments_.clear();
  std::vector<InstrumentId> ids;
  for (int k = 0; k <= 40; ++k) {
    ids.push_back(
      InstrumentId{Country::KR, all_venues[static_cast<std::size_t>(k % 4)], "S" + std::to_string(k / 4)}
    );
    auto reference = samsung_reference();
    reference.id_ = ids.back();
    config.instruments_.push_back(reference);
  }
  const auto d1 = utc(2026, 3, 10, 6, 30);
  const auto d2 = utc(2026, 3, 11, 6, 30);
  backtest::Backtester<RecordsSlices> engine(RecordsSlices{}, config);
  for (const auto& id : ids) {
    engine.feed(slice_bar(id, d1, BarInterval::ONE_DAY, "100", "101"));
  }
  // Another interval at the same instant is another slice, with its own ids.
  engine.feed(slice_bar(ids[29], d1, BarInterval::ONE_HOUR, "100", "101"));
  // A second daily bar, for a name fed after the set was built and for one fed before.
  CHECK_THROWS_AS(engine.feed(slice_bar(ids[29], d1, BarInterval::ONE_DAY, "100", "101")), std::invalid_argument);
  CHECK_THROWS_AS(engine.feed(slice_bar(ids[3], d1, BarInterval::ONE_DAY, "100", "101")), std::invalid_argument);
  CHECK(engine.result().bars_ == 42);
  engine.feed(slice_bar(ids[0], d2, BarInterval::ONE_DAY, "102", "103"));
  engine.finish();

  const auto& seen = engine.strategy().seen_;
  REQUIRE(seen.size() == 3);
  REQUIRE(seen[0].symbols_.size() == 41);
  for (std::size_t k = 0; k < ids.size(); ++k) {
    CHECK(seen[0].symbols_[k] == ids[k].symbol_);
  }
  CHECK(seen[1].interval_ == BarInterval::ONE_HOUR);
  CHECK(seen[1].symbols_ == std::vector<std::string>{ids[29].symbol_});
  CHECK(seen[2].end_ == d2);
  CHECK(seen[2].symbols_ == std::vector<std::string>{ids[0].symbol_});
}

TEST_CASE("a slice with a bar stating its end is decided at that end and not at a nominal one") {
  // The first instrument's open-stamped daily bar ends nominally at `t`; the second's close-stamped bar
  // states `t`. The slice holds both, so its decision is exact whichever came first: an order may not
  // fill at the open of a bar that began before `t`.
  const auto t = utc(2026, 3, 10, 6, 30);
  for (const bool exact_first : {true, false}) {
    RecordsSlices strategy;
    strategy.buy_ = samsung();
    backtest::Backtester<RecordsSlices> engine(std::move(strategy), pair_config());
    const auto nominal = slice_bar(
      samsung(), t - std::chrono::hours(24), BarInterval::ONE_DAY, "100", "101", BarStamp::OPEN
    );
    const auto exact = slice_bar(hynix(), t, BarInterval::ONE_DAY, "200", "201");
    if (exact_first) {
      engine.feed(exact);
      engine.feed(nominal);
    } else {
      engine.feed(nominal);
      engine.feed(exact);
    }
    // Began an hour before the decision: not eligible.
    engine.feed(slice_bar(
      samsung(), t - std::chrono::hours(1), BarInterval::ONE_DAY, "150", "151", BarStamp::OPEN
    ));
    engine.feed(slice_bar(
      samsung(), t + std::chrono::hours(23), BarInterval::ONE_DAY, "170", "171", BarStamp::OPEN
    ));
    engine.finish();
    REQUIRE(engine.strategy().fill_prices_.size() == 1);
    CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("170"));
  }
}

TEST_CASE("an all-nominal monthly slice's order fills in the next month and not the one after") {
  // An open-stamped month's end is nominal (31 days), so February's runs five days into March; an
  // order decided on it must still meet March's bar.
  RecordsSlices strategy;
  strategy.buy_ = samsung();
  strategy.buy_on_ = 1;
  backtest::Backtester<RecordsSlices> engine(std::move(strategy), replay_config());
  engine.feed(slice_bar(samsung(), utc(2026, 1, 1, 0, 0), BarInterval::ONE_MONTH, "100", "101", BarStamp::OPEN));
  engine.feed(slice_bar(samsung(), utc(2026, 2, 1, 0, 0), BarInterval::ONE_MONTH, "110", "111", BarStamp::OPEN));
  engine.feed(slice_bar(samsung(), utc(2026, 3, 1, 0, 0), BarInterval::ONE_MONTH, "120", "121", BarStamp::OPEN));
  engine.feed(slice_bar(samsung(), utc(2026, 4, 1, 0, 0), BarInterval::ONE_MONTH, "130", "131", BarStamp::OPEN));
  engine.finish();
  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  CHECK(engine.strategy().fill_prices_[0] == Decimal64::parse("120"));
}

namespace {

/** Rests a limit buy from `on_phase`, withdraws it from `on_order_update`, and records what `on_bars` sees. */
struct WithdrawsBeforeTheSlice {
  bool placed_{false};
  bool withdrawn_{false};
  std::vector<std::size_t> working_seen_;

  template <class Ctx>
  void on_phase(Ctx& ctx, const strategy::MarketPhase&) {
    if (placed_) {
      return;
    }
    placed_ = true;
    ctx.submit(strategy::OrderIntent::limit(
      samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")), far_below()
    ).with_tag("rests"));
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot&) {
    if (withdrawn_) {
      return;
    }
    for (const auto& working : ctx.portfolio().working_) {
      withdrawn_ = true;
      ctx.cancel(working.ref_, "withdraw");
    }
  }

  template <class Ctx>
  void on_bars(Ctx& ctx, const strategy::BarSlice&) {
    working_seen_.push_back(ctx.portfolio().working_.size());
  }
};

} // namespace

TEST_CASE("on_bars sees the queue an order update left and not the one before it") {
  backtest::Backtester<WithdrawsBeforeTheSlice> engine(WithdrawsBeforeTheSlice{}, replay_config());
  engine.feed(slice_bar(samsung(), utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "100", "101"));
  engine.finish();
  REQUIRE(engine.strategy().placed_);
  REQUIRE(engine.strategy().withdrawn_);
  REQUIRE(engine.strategy().working_seen_.size() == 1);
  CHECK(engine.strategy().working_seen_[0] == 0);
  CHECK(engine.result().unfilled_ == 0);
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a halt drops the period being collected and finish does not deliver it") {
  RecordsSlices strategy;
  strategy.buy_ = samsung();
  backtest::Backtester<RecordsSlices> engine(std::move(strategy), pair_config());
  engine.feed(slice_bar(samsung(), utc(2026, 3, 10, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  engine.feed(slice_bar(samsung(), utc(2026, 3, 11, 6, 30), BarInterval::ONE_DAY, "70000", "70000"));
  REQUIRE(engine.strategy().fill_prices_.size() == 1);
  // The third period's first bar is collected; the second halts the replay at its mark.
  engine.feed(slice_bar(hynix(), utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "200", "201"));
  engine.feed(slice_bar(
    samsung(), utc(2026, 3, 12, 6, 30), BarInterval::ONE_DAY, "70000", "9223372036854775807"
  ));
  REQUIRE(engine.result().halted_);
  const auto delivered = engine.strategy().seen_.size();
  engine.finish();
  CHECK(engine.strategy().seen_.size() == delivered);
}
#endif

namespace {

/** Holds ten of the first instrument while it closes above the second: the pair rule, by single bars. */
struct PairRuleByBar {
  std::optional<std::pair<Clock::time_point, Decimal64>> first_;
  std::optional<std::pair<Clock::time_point, Decimal64>> second_;
  std::optional<Clock::time_point> decided_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (bar.instrument_ == samsung()) {
      first_ = std::make_pair(bar.period_end_, bar.bar_.close_);
    } else if (bar.instrument_ == hynix()) {
      second_ = std::make_pair(bar.period_end_, bar.bar_.close_);
    }
    if (!first_ || !second_ || !(first_->first == second_->first) || decided_ == first_->first) {
      return;
    }
    decided_ = first_->first;
    decide(ctx, first_->second, second_->second);
  }

  template <class Ctx>
  static void decide(Ctx& ctx, const Decimal64& first, const Decimal64& second) {
    strategy::TargetPosition target;
    target.instrument_ = samsung();
    target.tag_ = "pair";
    target.target_quantity_ = decimal_less(second, first) ? Decimal64::parse("10") : Decimal64::parse("0");
    ctx.submit(target);
  }
};

/** The same rule with `on_bars`: no bookkeeping. */
struct PairRuleBySlice {
  template <class Ctx>
  void on_bars(Ctx& ctx, const strategy::BarSlice& slice) {
    const auto* first = slice.find(samsung());
    const auto* second = slice.find(hynix());
    if (first == nullptr || second == nullptr) {
      return;
    }
    PairRuleByBar::decide(ctx, first->bar_.close_, second->bar_.close_);
  }
};

template <typename Strategy>
std::vector<backtest::Execution> pair_rule_executions() {
  backtest::Backtester<Strategy> engine(Strategy{}, pair_config());
  const std::array<std::array<const char*, 2>, 6> closes{{
    {"100", "90"}, {"95", "99"}, {"110", "100"}, {"108", "109"}, {"120", "100"}, {"121", "100"}
  }};
  for (std::size_t day = 0; day < closes.size(); ++day) {
    const auto at = utc(2026, 3, 9 + static_cast<int>(day), 6, 30);
    engine.feed(slice_bar(samsung(), at, BarInterval::ONE_DAY, closes[day][0], closes[day][0]));
    engine.feed(slice_bar(hynix(), at, BarInterval::ONE_DAY, closes[day][1], closes[day][1]));
  }
  engine.finish();
  return engine.result().executions_;
}

} // namespace

TEST_CASE("the same pair rule trades identically through on_bar and on_bars") {
  const auto by_bar = pair_rule_executions<PairRuleByBar>();
  const auto by_slice = pair_rule_executions<PairRuleBySlice>();
  REQUIRE(by_bar.size() >= 2);
  REQUIRE(by_bar.size() == by_slice.size());
  for (std::size_t i = 0; i < by_bar.size(); ++i) {
    CHECK(by_bar[i].fill_.instrument_ == by_slice[i].fill_.instrument_);
    CHECK(by_bar[i].fill_.side_ == by_slice[i].fill_.side_);
    CHECK(by_bar[i].fill_.quantity_ == by_slice[i].fill_.quantity_);
    CHECK(by_bar[i].fill_.price_ == by_slice[i].fill_.price_);
    CHECK(by_bar[i].fill_.at_ == by_slice[i].fill_.at_);
  }
}

TEST_CASE("a replay models limit and market orders under every condition and stops only as day orders") {
  const auto order = [](OrderType type, TimeInForce condition, bool priced, bool triggered) {
    strategy::PlannedOrder planned;
    planned.type_ = type;
    planned.time_in_force_ = condition;
    planned.session_ = OrderSession::REGULAR;
    if (priced) {
      planned.price_ = Decimal64::parse("100");
    }
    if (triggered) {
      planned.trigger_price_ = Decimal64::parse("99");
    }
    return planned;
  };
  const TimeInForce conditions[] = {
    TimeInForce::UNKNOWN, TimeInForce::DAY, TimeInForce::IMMEDIATE_OR_CANCEL, TimeInForce::FILL_OR_KILL,
  };
  for (const auto condition : conditions) {
    INFO(static_cast<int>(condition));
    // Limit and market orders under any condition, with or without a price, but never with a trigger.
    CHECK(ReplayExecutor::can_model(order(OrderType::LIMIT, condition, true, false)));
    CHECK(ReplayExecutor::can_model(order(OrderType::LIMIT, condition, false, false)));
    CHECK(ReplayExecutor::can_model(order(OrderType::MARKET, condition, false, false)));
    CHECK_FALSE(ReplayExecutor::can_model(order(OrderType::LIMIT, condition, true, true)));
    CHECK_FALSE(ReplayExecutor::can_model(order(OrderType::MARKET, condition, false, true)));
    // A stop-limit needs both prices, and only a day one is modelled.
    CHECK(ReplayExecutor::can_model(order(OrderType::STOP_LIMIT, condition, true, true)) ==
      (condition == TimeInForce::DAY));
    CHECK_FALSE(ReplayExecutor::can_model(order(OrderType::STOP_LIMIT, condition, true, false)));
    CHECK_FALSE(ReplayExecutor::can_model(order(OrderType::STOP_LIMIT, condition, false, true)));
    for (const auto type : {
           OrderType::UNKNOWN, OrderType::CONDITIONAL_LIMIT, OrderType::BEST_LIMIT, OrderType::PRIMARY_LIMIT,
           OrderType::MID_PRICE, OrderType::CLOSING_PRICE,
         }) {
      CHECK_FALSE(ReplayExecutor::can_model(order(type, condition, true, false)));
    }
  }
  auto auction = order(OrderType::LIMIT, TimeInForce::DAY, true, false);
  auction.session_ = OrderSession::CLOSING_AUCTION;
  CHECK_FALSE(ReplayExecutor::can_model(auction));
}

// ---------------------------------------------------------------------------
// A strategy without on_order_update: the replay skips the updates, and nothing else changes
// ---------------------------------------------------------------------------

namespace {

/** Every field of a replay's result, as text, so two runs can be compared whole. */
std::string result_digest(const backtest::ReplayResult& r) {
  std::string out;
  const auto when = [](Clock::time_point at) { return std::to_string(at.time_since_epoch().count()); };
  const auto maybe_when = [&](const std::optional<Clock::time_point>& at) { return at ? when(*at) : std::string("-"); };
  const auto maybe = [](const std::optional<Decimal64>& value) { return value ? value->str() : std::string("-"); };
  const auto id = [](const InstrumentId& i) { return std::to_string(static_cast<int>(i.venue_)) + "/" + i.symbol_; };
  out += "bars " + std::to_string(r.bars_) + " gaps " + std::to_string(r.gaps_) + " unfilled " +
    std::to_string(r.unfilled_) + " aborted " + std::to_string(r.aborted_) + " quotes " + std::to_string(r.quotes_) +
    " unchecked " + std::to_string(r.unchecked_fill_causality_) + " unvalued " + std::to_string(r.unvalued_bars_) +
    " " + maybe_when(r.first_unvalued_) + " releases " + std::to_string(r.stop_releases_) + " intrabar " +
    std::to_string(r.stop_fills_intrabar_) + " bounded " + std::to_string(r.limit_fills_bounded_) + " overdrawn " +
    std::to_string(r.overdrawn_bars_) + " " + maybe_when(r.first_overdrawn_) + " lowest " +
    (r.lowest_cash_ ? r.lowest_cash_->amount_.str() : std::string("-")) + " " + maybe_when(r.lowest_cash_at_) +
    " expired " + std::to_string(r.expired_day_orders_) + " halted " + (r.halted_ ? r.halted_->message_ : "-") + "\n";
  for (const auto& point : r.equity_) {
    out += "E " + when(point.at_) + " " + point.equity_.str() + " " + std::to_string(point.period_return_) + "\n";
  }
  for (const auto& x : r.executions_) {
    const auto& f = x.fill_;
    out += "X " + f.order_id_ + " " + id(f.instrument_) + " " + std::to_string(static_cast<int>(f.side_)) + " " +
      f.quantity_.str() + " " + f.price_.str() + " " + f.tag_ + " " + when(f.at_) + " " + x.commission_.str() + " " +
      maybe(x.institution_fee_) + " " + x.slippage_.str() + " " + x.tax_.str() + " " + x.applied_.rate_.str() + "\n";
  }
  for (const auto& j : r.rejected_) {
    out += "J " + std::to_string(static_cast<int>(j.reason_)) + " " + id(j.instrument_) + " " + j.tag_ + "\n";
  }
  for (const auto& g : r.refused_) {
    out += "G " + std::to_string(static_cast<int>(g.reason_)) + " " + id(g.instrument_) + " " + g.tag_ + " " +
      maybe(g.limit_) + " " + maybe(g.attempted_) + "\n";
  }
  return out;
}

/**
 * Places, at random from a fixed seed, what makes a replay publish updates: limits that rest and fill, stop-limits,
 * targets (a flat one waits behind a resting stop), market orders, and cancellations of its own working orders.
 * Declares no `on_order_update`.
 */
struct Wanders {
  std::uint32_t state_{20261020};

  std::uint32_t draw(std::uint32_t below) {
    state_ = state_ * 1664525u + 1013904223u;
    return (state_ >> 8) % below;
  }

  static strategy::Price at(long price) {
    return *strategy::Price::checked(Decimal64(price, 0), PriceRule::krx_equity());
  }

  template <class Ctx>
  void act(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto close = bar.bar_.close_.coefficient() / static_cast<long>(std::pow(10, bar.bar_.close_.scale()));
    const auto some = strategy::SizeSpec::quantity(Decimal64(static_cast<long>(1 + draw(5)), 0));
    const auto& id = bar.instrument_;
    switch (draw(10)) {
      case 0: {
        strategy::TargetPosition target;
        target.instrument_ = id;
        target.target_quantity_ = Decimal64(static_cast<long>(draw(30)), 0);
        target.tag_ = "target";
        ctx.submit(target);
        break;
      }
      case 1:
        ctx.submit(strategy::OrderIntent::limit(id, OrderSide::BUY, some, at(close - 100 * draw(12))).with_tag("bid"));
        break;
      case 2:
        ctx.submit(strategy::OrderIntent::limit(id, OrderSide::SELL, some, at(close + 100 * draw(12))).with_tag("ask"));
        break;
      case 3:
        ctx.submit(strategy::OrderIntent::stop_limit(id, OrderSide::SELL, some, at(close - 200), at(close - 300)).with_tag("stop"));
        break;
      case 4:
        for (const auto& working : ctx.portfolio().working_) {
          if (working.order_.instrument_ == id && draw(2) == 0) {
            ctx.cancel(working.ref_, "withdraw");
            break;
          }
        }
        break;
      case 5:
        ctx.submit(strategy::OrderIntent::market(id, OrderSide::BUY, some).with_tag("buy"));
        break;
      case 6: {
        strategy::TargetPosition flat;
        flat.instrument_ = id;
        flat.target_quantity_ = Decimal64(0, 0);
        flat.tag_ = "flat";
        ctx.submit(flat);
        break;
      }
      default:
        break;
    }
  }

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    act(ctx, bar);
  }
};

/** How many updates the hearing strategies below were told about, so the test knows the full path ran. */
std::size_t updates_heard = 0;

/** The same strategy, told about every order update and answering none. */
struct WandersHearing : Wanders {
  template <class Ctx>
  void on_order_update(Ctx&, const OrderSnapshot&) {
    ++updates_heard;
  }
};

/** The same choices made once a period, over every instrument's bar. */
struct WandersBySlice {
  Wanders inner_;

  template <class Ctx>
  void on_bars(Ctx& ctx, const strategy::BarSlice& slice) {
    for (const auto& bar : slice.bars_) {
      inner_.act(ctx, bar);
    }
  }
};

struct WandersBySliceHearing : WandersBySlice {
  template <class Ctx>
  void on_order_update(Ctx&, const OrderSnapshot&) {
    ++updates_heard;
  }
};

static_assert(!strategy::strategy_detail::declares_on_order_update<Wanders>);
static_assert(strategy::strategy_detail::declares_on_order_update<WandersHearing>);
static_assert(!strategy::strategy_detail::declares_on_order_update<WandersBySlice>);
static_assert(strategy::strategy_detail::declares_on_order_update<WandersBySliceHearing>);

/** Three sessions of one-minute bars of both instruments, prices walking on the KRX grid. */
template <class Strategy>
backtest::ReplayResult wander(std::uint32_t seed) {
  backtest::Backtester<Strategy> engine(Strategy{}, pair_config());
  std::uint32_t walk = seed;
  const auto step = [&walk](std::uint32_t below) {
    walk = walk * 22695477u + 1u;
    return (walk >> 8) % below;
  };
  std::array<long, 2> price{70000, 120000};
  for (int day = 0; day < 3; ++day) {
    for (int minute = 1; minute <= 40; ++minute) {
      for (std::size_t k = 0; k < 2; ++k) {
        const long open = price[k];
        price[k] += 100 * (static_cast<long>(step(5)) - 2);
        const long close = price[k];
        Bar bar;
        bar.timestamp_ = utc(2026, 3, 10 + day, 1, minute);
        bar.open_ = Decimal64(open, 0);
        bar.close_ = Decimal64(close, 0);
        bar.high_ = Decimal64(std::max(open, close) + 100 * static_cast<long>(step(3)), 0);
        bar.low_ = Decimal64(std::min(open, close) - 100 * static_cast<long>(step(3)), 0);
        bar.volume_ = Decimal64::parse("1000");
        engine.feed(strategy::make_bar_event(k == 0 ? samsung() : hynix(), BarInterval::ONE_MINUTE, BarStamp::CLOSE, bar));
      }
    }
  }
  engine.finish();
  return engine.result();
}

} // namespace

TEST_CASE("a replay skips order updates for a strategy that has no handler and changes no result", "[order-updates]") {
  // The type without `on_order_update` takes the skip; the one adding an empty handler takes the full path. Same
  // decisions, same bars: every field must agree, including deferred targets released behind stops.
  updates_heard = 0;
  std::size_t executions = 0;
  std::size_t behind_stops = 0;
  std::size_t refused = 0;
  std::size_t expired = 0;
  for (std::uint32_t seed = 1; seed <= 12; ++seed) {
    INFO("seed " << seed);
    const auto skipped = wander<Wanders>(seed);
    CHECK(result_digest(skipped) == result_digest(wander<WandersHearing>(seed)));
    CHECK(result_digest(wander<WandersBySlice>(seed)) == result_digest(wander<WandersBySliceHearing>(seed)));
    executions += skipped.executions_.size();
    refused += skipped.refused_.size();
    expired += skipped.expired_day_orders_;
    for (const auto& rejection : skipped.rejected_) {
      behind_stops += rejection.reason_ == strategy::RejectReason::STOP_ORDER_RESTING ? 1 : 0;
    }
  }
  // The runs did what the comparison is about: orders filled, rested and expired, targets waited behind
  // stops, the guard refused some, and the hearing types were told about updates.
  CHECK(executions > 200);
  CHECK(behind_stops > 10);
  CHECK(refused > 10);
  CHECK(expired > 5);
  CHECK(updates_heard > 500);
}
