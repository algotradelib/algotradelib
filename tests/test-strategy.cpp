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
