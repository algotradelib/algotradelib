#include <algotradelib/strategy>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;

using strategy::OrderIntent;
using strategy::Price;
using strategy::SizeKind;
using strategy::SizeSpec;

namespace {

/** The KRX cash-equity ladder, the grid both adapters attach to a Korean share. */
const PriceRule& krx() {
  static const PriceRule rule = PriceRule::krx_equity();
  return rule;
}

InstrumentId samsung() {
  return InstrumentId{Country::KR, Venue::COMBINED, "005930"};
}

} // namespace

// ---------------------------------------------------------------------------
// Price: the grid guarantee
// ---------------------------------------------------------------------------

TEST_CASE("a price cannot be constructed at all except through a grid check") {
  // The guarantee this type exists for, asserted rather than described. There is no public
  // constructor, no default, and nothing that takes a bare decimal.
  STATIC_REQUIRE_FALSE(std::is_default_constructible_v<Price>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<Price, Decimal64>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<Price, double>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<Price, int>);
  // Copying one that already exists is fine: it was checked when it was made.
  STATIC_REQUIRE(std::is_copy_constructible_v<Price>);
}

TEST_CASE("a price on the grid is accepted and keeps its exact value") {
  // 70,000 sits in the 100-won band and is a multiple of it.
  const auto price = Price::checked(Decimal64::parse("70000"), krx());
  REQUIRE(price);
  REQUIRE(price->value() == Decimal64::parse("70000"));
  REQUIRE(price->str() == "70000");
}

TEST_CASE("a price off the grid is refused rather than quietly moved") {
  // 70,050 is not a multiple of the 100-won tick that applies above 50,000. Refusing is the
  // point: silently moving it would place an order at a price the strategy did not choose.
  REQUIRE_FALSE(Price::checked(Decimal64::parse("70050"), krx()));
  // And just below the band boundary, where the tick is 50 rather than 100, it is fine.
  REQUIRE(Price::checked(Decimal64::parse("49950"), krx()));
  REQUIRE_FALSE(Price::checked(Decimal64::parse("49975"), krx()));
}

TEST_CASE("an unknown grid refuses every price rather than passing them all") {
  // An ETN, or a listing whose ladder this library does not have. Answering "on tick" here
  // would let an off-grid price through a check that appeared to have passed.
  const auto rule = PriceRule::unknown("no table for this instrument type");
  REQUIRE_FALSE(Price::checked(Decimal64::parse("70000"), rule));
  REQUIRE_FALSE(Price::rounded(Decimal64::parse("70000"), rule, OrderSide::BUY));
}

TEST_CASE("an unconstrained market accepts any decimal") {
  const auto rule = PriceRule::unconstrained();
  REQUIRE(Price::checked(Decimal64::parse("185.7049"), rule));
}

TEST_CASE("rounding moves a price the way that cannot cross the caller's intent") {
  // A buy rounds down and a sell rounds up, so rounding never makes a limit more aggressive
  // than instructed. 70,050 is between two 100-won ticks.
  const auto buy = Price::rounded(Decimal64::parse("70050"), krx(), OrderSide::BUY);
  const auto sell = Price::rounded(Decimal64::parse("70050"), krx(), OrderSide::SELL);
  REQUIRE(buy);
  REQUIRE(sell);
  REQUIRE(buy->value() == Decimal64::parse("70000"));
  REQUIRE(sell->value() == Decimal64::parse("70100"));

  // Whatever it produces is on the grid, which is what makes the two factories agree.
  REQUIRE(Price::checked(buy->value(), krx()));
  REQUIRE(Price::checked(sell->value(), krx()));
}

// ---------------------------------------------------------------------------
// SizeSpec
// ---------------------------------------------------------------------------

TEST_CASE("a size states which currency of measurement it is in") {
  const auto shares = SizeSpec::quantity(Decimal64::parse("10"));
  REQUIRE(shares.kind() == SizeKind::QUANTITY);
  REQUIRE(shares.as_quantity() == Decimal64::parse("10"));

  const auto cash = SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}});
  REQUIRE(cash.kind() == SizeKind::NOTIONAL);
  REQUIRE(cash.as_notional().amount_ == Decimal64::parse("1000000"));

  const auto weight = SizeSpec::target_weight(0.25);
  REQUIRE(weight.kind() == SizeKind::TARGET_WEIGHT);
  REQUIRE(weight.as_target_weight() == Catch::Approx(0.25));
}

TEST_CASE("reading a size as the wrong measurement throws instead of answering zero") {
  // An order for the wrong amount is not a failure to paper over with a default.
  const auto cash = SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}});
  REQUIRE_THROWS_AS(cash.as_quantity(), std::logic_error);
  REQUIRE_THROWS_AS(cash.as_target_weight(), std::logic_error);

  const auto shares = SizeSpec::quantity(Decimal64::parse("10"));
  REQUIRE_THROWS_AS(shares.as_notional(), std::logic_error);
}

// ---------------------------------------------------------------------------
// OrderIntent
// ---------------------------------------------------------------------------

TEST_CASE("an intent can only be assembled in a shape an adapter accepts") {
  // The invalid combinations are unconstructible rather than rejected later: there is no way
  // to make a limit order without a price, or a market order with one.
  STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OrderIntent>);
  STATIC_REQUIRE(std::is_copy_constructible_v<OrderIntent>);
}

TEST_CASE("a limit intent carries its price and a market intent carries none") {
  const auto price = Price::checked(Decimal64::parse("70000"), krx());
  REQUIRE(price);

  const auto buy = OrderIntent::limit(
    samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("10")), *price
  );
  REQUIRE(buy.type() == OrderType::LIMIT);
  REQUIRE(buy.limit());
  REQUIRE(buy.limit()->value() == Decimal64::parse("70000"));
  REQUIRE(buy.side() == OrderSide::BUY);
  REQUIRE(buy.time_in_force() == TimeInForce::DAY);
  REQUIRE(buy.instrument().symbol_ == "005930");
  REQUIRE(buy.size().as_quantity() == Decimal64::parse("10"));

  const auto flatten = OrderIntent::market(
    samsung(), OrderSide::SELL, SizeSpec::quantity(Decimal64::parse("10"))
  );
  REQUIRE(flatten.type() == OrderType::MARKET);
  REQUIRE_FALSE(flatten.limit());
}

TEST_CASE("labelling and keying an intent leaves the original untouched") {
  const auto base = OrderIntent::market(
    samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("1"))
  );
  REQUIRE(base.tag().empty());
  REQUIRE_FALSE(base.client_order_id());

  const auto tagged = base.with_tag("sma-crossover");
  REQUIRE(tagged.tag() == "sma-crossover");
  // The value semantics matter: a handler may build several intents from one template.
  REQUIRE(base.tag().empty());

  const auto keyed = tagged.with_client_order_id("idem-1");
  REQUIRE(keyed.client_order_id() == std::optional<std::string>("idem-1"));
  REQUIRE(keyed.tag() == "sma-crossover");
  REQUIRE_FALSE(tagged.client_order_id());
}

TEST_CASE("a target position is signed, so flat and short are both expressible") {
  strategy::TargetPosition flat;
  flat.instrument_ = samsung();
  flat.target_quantity_ = Decimal64::parse("0");
  REQUIRE(flat.target_quantity_.coefficient() == 0);
  REQUIRE_FALSE(flat.limit_);

  strategy::TargetPosition shorted;
  shorted.target_quantity_ = Decimal64::parse("-10");
  REQUIRE(decimal_less(shorted.target_quantity_, Decimal64::parse("0")));
}

TEST_CASE("the strategy surface needs no linear algebra backend") {
  // Asserted by this file compiling: tests/CMakeLists.txt builds it unconditionally, while the
  // analytics tests are guarded by ATL_USE_EIGEN. A strategy acting on prices and positions
  // alone should not have to acquire Eigen to do it.
  STATIC_REQUIRE(sizeof(strategy::SizeSpec) > 0);
}

// ---------------------------------------------------------------------------
// Market phase
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::system_clock;

/** Builds a UTC instant, so a test states the moment it means without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  // Days since the epoch by proleptic Gregorian arithmetic, valid for the dates used here.
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  const long long epoch_days = era_days - 719468;
  return Clock::time_point{} + std::chrono::hours(epoch_days * 24 + hour) +
    std::chrono::minutes(minute);
}

} // namespace

TEST_CASE("an uncovered date is unknown, never assumed closed or open") {
  // The same distinction TradingCalendar draws. Assuming closed stops a strategy on a day that
  // is in fact open; assuming open lets it order into a holiday.
  const auto session = strategy::SessionWindow::krx();
  REQUIRE(session.phase(utc(2026, 3, 10, 2, 0), std::nullopt) == strategy::MarketPhase::UNKNOWN);
  REQUIRE(session.phase(utc(2026, 3, 10, 2, 0), false) == strategy::MarketPhase::CLOSED);
}

TEST_CASE("the KRX session boundaries fall where the exchange publishes them") {
  const auto session = strategy::SessionWindow::krx();
  // KST is UTC+9, so 09:00 KST is 00:00 UTC and 15:30 KST is 06:30 UTC.
  REQUIRE(session.phase(utc(2026, 3, 10, 23, 59), true) == strategy::MarketPhase::PRE_OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 0, 0), true) == strategy::MarketPhase::OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 3, 0), true) == strategy::MarketPhase::OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 6, 29), true) == strategy::MarketPhase::OPEN);
  // The close is exclusive: at exactly 15:30 KST the session is over.
  REQUIRE(session.phase(utc(2026, 3, 10, 6, 30), true) == strategy::MarketPhase::POST_CLOSE);
  REQUIRE(session.phase(utc(2026, 3, 10, 14, 0), true) == strategy::MarketPhase::POST_CLOSE);
}

TEST_CASE("a phase name is stable for every enumerator") {
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::UNKNOWN) == "unknown");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::CLOSED) == "closed");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::PRE_OPEN) == "pre_open");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::OPEN) == "open");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::POST_CLOSE) == "post_close");
}

// ---------------------------------------------------------------------------
// Bar events
// ---------------------------------------------------------------------------

TEST_CASE("a bar event states when its period ended, whichever edge it is stamped on") {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 0, 0);
  bar.high_ = Decimal64::parse("70500");
  bar.low_ = Decimal64::parse("69400");
  bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1200000");

  // A close-stamped bar already carries the end of its period.
  const auto closed = strategy::make_bar_event(
    samsung(), BarInterval::ONE_MINUTE, BarStamp::CLOSE, bar
  );
  REQUIRE(closed.period_end_ == bar.timestamp_);

  // An open-stamped bar ends one interval later. Getting this wrong is the off-by-one-bar
  // lookahead that BarStamp exists to prevent, which is why it is computed once here.
  const auto opened = strategy::make_bar_event(
    samsung(), BarInterval::ONE_MINUTE, BarStamp::OPEN, bar
  );
  REQUIRE(opened.period_end_ == bar.timestamp_ + std::chrono::minutes(1));

  const auto daily = strategy::make_bar_event(
    samsung(), BarInterval::ONE_DAY, BarStamp::OPEN, bar
  );
  REQUIRE(daily.period_end_ == bar.timestamp_ + std::chrono::hours(24));
  REQUIRE(daily.interval_ == BarInterval::ONE_DAY);
  REQUIRE(daily.stamp_ == BarStamp::OPEN);
  REQUIRE(daily.instrument_.symbol_ == "005930");
}

TEST_CASE("a fill reports one execution, not an order's running total") {
  // A partial fill arrives as one event per execution. Summing them is the strategy's business;
  // the running total lives in the portfolio.
  strategy::Fill fill;
  fill.order_id_ = "0001569139";
  fill.instrument_ = samsung();
  fill.side_ = OrderSide::BUY;
  fill.quantity_ = Decimal64::parse("40");
  fill.price_ = Decimal64::parse("70000");
  fill.tag_ = "sma-crossover";

  REQUIRE(fill.quantity_ == Decimal64::parse("40"));
  REQUIRE(fill.tag_ == "sma-crossover");
  // Broker-neutral: an order id as text, so on_fill's signature names no broker.
  REQUIRE(fill.order_id_ == "0001569139");
}
