#include <algotradelib/analytics>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>

using namespace algotradelib;
using analytics::Scalar;
using analytics::missing;
using analytics::observed;
using analytics::round_scalar_to_tick;
using analytics::to_decimal;
using analytics::to_scalar;

TEST_CASE("an exact decimal converts to a scalar without loss at market scales") {
  // Every price and quantity a caller can place has a coefficient inside 53 bits,
  // which `double` holds exactly. That is why this direction is safe in practice even
  // though it is lossy in principle.
  REQUIRE(to_scalar(Decimal64::parse("70000")) == 70000.0);
  REQUIRE(to_scalar(Decimal64::parse("70000.5")) == 70000.5);
  REQUIRE(to_scalar(Decimal64::parse("-0.25")) == -0.25);
  REQUIRE(to_scalar(Decimal64::parse("0")) == 0.0);
  REQUIRE(to_scalar(Decimal64::parse("185.75")) == 185.75);

  // A round trip through the scalar returns the same decimal at the same scale.
  for (const char* text : {"70000", "70000.5", "185.75", "-0.25", "1.125"}) {
    const auto original = Decimal64::parse(text);
    const auto back = to_decimal(to_scalar(original), original.scale());
    REQUIRE(back.has_value());
    REQUIRE(*back == original);
  }
}

TEST_CASE("the return conversion rounds half away from zero") {
  REQUIRE(to_decimal(0.5, 0)->str() == "1");
  REQUIRE(to_decimal(1.5, 0)->str() == "2");
  REQUIRE(to_decimal(2.5, 0)->str() == "3");
  REQUIRE(to_decimal(-0.5, 0)->str() == "-1");
  REQUIRE(to_decimal(-1.5, 0)->str() == "-2");

  // Not banker's rounding: 2.5 goes to 3, not to 2. A series of conversions is not
  // biased toward even values.
  REQUIRE(to_decimal(2.5, 0)->str() != "2");

  REQUIRE(to_decimal(70000.449, 1)->str() == "70000.4");
  REQUIRE(to_decimal(70000.450, 1)->str() == "70000.5");
}

TEST_CASE("a value that is not a number has no decimal form") {
  // An indicator's warm-up region is `missing()`, and a strategy that has not warmed
  // up must not be able to turn that into a price.
  REQUIRE_FALSE(to_decimal(missing(), 0).has_value());
  REQUIRE_FALSE(to_decimal(std::numeric_limits<Scalar>::infinity(), 0).has_value());
  REQUIRE_FALSE(to_decimal(-std::numeric_limits<Scalar>::infinity(), 2).has_value());

  REQUIRE_FALSE(observed(missing()));
  REQUIRE(observed(0.0));
  REQUIRE(observed(-1.5));
}

TEST_CASE("a value too large for an exact decimal is refused, not wrapped") {
  const Scalar huge = 1e30;
  REQUIRE_FALSE(to_decimal(huge, 0).has_value());
  REQUIRE_FALSE(to_decimal(-huge, 0).has_value());

  // Scaling is what overflows first for a large value at a fine scale.
  REQUIRE_FALSE(to_decimal(1e18, 4).has_value());
  REQUIRE(to_decimal(1e15, 0).has_value());
}

TEST_CASE("the route from a computed value to an order price is narrow") {
  const auto rule = PriceRule::krx_equity();

  // A buy rounds down onto the grid, a sell rounds up: rounding never improves a price
  // past the instruction.
  REQUIRE(round_scalar_to_tick(70123.7, rule, OrderSide::BUY)->str() == "70100");
  REQUIRE(round_scalar_to_tick(70123.7, rule, OrderSide::SELL)->str() == "70200");

  // A value already on the grid survives unchanged.
  REQUIRE(round_scalar_to_tick(70100.0, rule, OrderSide::BUY)->str() == "70100");

  // The warm-up region cannot become a price.
  REQUIRE_FALSE(round_scalar_to_tick(missing(), rule, OrderSide::BUY).has_value());

  // Nor can a value the library has no grid for. An ETF is the real case: KRX prices
  // funds on a ladder this library does not carry, so a computed price cannot be sent.
  const auto fund = price_rule_for(Country::KR, SecurityType::ETF);
  REQUIRE_FALSE(round_scalar_to_tick(34567.8, fund, OrderSide::BUY).has_value());

  // A market quoting in free decimals rounds to the requested scale and no further.
  const auto free = PriceRule::unconstrained();
  REQUIRE(round_scalar_to_tick(185.7049, free, OrderSide::BUY, 2)->str() == "185.70");
  REQUIRE(round_scalar_to_tick(185.7051, free, OrderSide::BUY, 2)->str() == "185.71");
}

TEST_CASE("a midpoint computed exactly and one computed in scalars agree at market scale") {
  // The two layers answer the same question in different arithmetic, and the boundary
  // is only trustworthy if they agree where a caller can act on it.
  DepthSnapshot book;
  book.bids_.push_back(DepthLevel{Decimal64::parse("70000"), Decimal64::parse("10")});
  book.asks_.push_back(DepthLevel{Decimal64::parse("70001"), Decimal64::parse("5")});

  const auto exact = book.mid();
  REQUIRE(exact.has_value());
  REQUIRE(exact->str() == "70000.5");

  const Scalar computed =
    (to_scalar(book.bids_.front().price_) + to_scalar(book.asks_.front().price_)) / 2.0;
  const auto converted = to_decimal(computed, 1);
  REQUIRE(converted.has_value());
  REQUIRE(*converted == *exact);
}

TEST_CASE("the returned decimal keeps the scale it was asked for") {
  // The scale parameter is the caller stating the precision the market quotes in, so it
  // is honoured rather than normalized away. That differs on purpose from the exact
  // arithmetic in core, which normalizes because its results are for comparing.
  const auto padded = to_decimal(1.5, 3);
  REQUIRE(padded.has_value());
  REQUIRE(padded->str() == "1.500");
  REQUIRE(padded->scale() == 3);

  // Numerically equal to the same value parsed, but not `==`, because equality compares
  // the scale too. This is the trap the documentation names.
  const auto parsed = Decimal64::parse("1.5");
  REQUIRE_FALSE(*padded == parsed);
  REQUIRE(decimal_subtract(*padded, parsed)->coefficient() == 0);

  // Asking for the scale the value actually has gives a value that does compare equal.
  REQUIRE(*to_decimal(1.5, 1) == parsed);
}
