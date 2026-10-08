#include <algotradelib/core>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;

namespace {

/** Returns the instrument used throughout these tests. */
InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** Returns a depth snapshot with one level on each side. */
DepthSnapshot book(const char* bid, const char* ask) {
  DepthSnapshot snapshot;
  snapshot.instrument_ = samsung();
  if (bid) {
    snapshot.bids_.push_back(DepthLevel{Decimal64::parse(bid), Decimal64::parse("10")});
  }
  if (ask) {
    snapshot.asks_.push_back(DepthLevel{Decimal64::parse(ask), Decimal64::parse("7")});
  }
  return snapshot;
}

} // namespace

TEST_CASE("a fixed-capacity code preserves an unfamiliar value without allocating") {
  const Code<12> segment("KR_ETC");
  REQUIRE(segment.view() == "KR_ETC");
  REQUIRE(segment.str() == "KR_ETC");
  REQUIRE(segment.size() == 6);
  REQUIRE_FALSE(segment.empty());
  REQUIRE(Code<12>::capacity() == 12);

  const Code<12> same("KR_ETC");
  const Code<12> other("US_ETC");
  REQUIRE(segment == same);
  REQUIRE(segment != other);

  const Code<4> blank;
  REQUIRE(blank.empty());
  REQUIRE(blank.view().empty());

  REQUIRE_THROWS_AS(Code<4>("TOOLONG"), std::invalid_argument);
  REQUIRE_THROWS_AS(Code<8>("kospi"), std::invalid_argument);
  REQUIRE_THROWS_AS(Code<8>("KO SPI"), std::invalid_argument);
}

TEST_CASE("decimal addition and subtraction align scales exactly") {
  const auto sum = decimal_add(Decimal64::parse("70000"), Decimal64::parse("0.25"));
  REQUIRE(sum.has_value());
  REQUIRE(sum->str() == "70000.25");

  const auto difference = decimal_subtract(Decimal64::parse("70100"), Decimal64::parse("70000"));
  REQUIRE(difference.has_value());
  REQUIRE(difference->str() == "100");

  const auto negative = decimal_subtract(Decimal64::parse("1.5"), Decimal64::parse("2"));
  REQUIRE(negative.has_value());
  REQUIRE(negative->str() == "-0.5");

  // A result is normalized, so it compares equal to the same number parsed.
  const auto trailing = decimal_add(Decimal64::parse("1.25"), Decimal64::parse("0.25"));
  REQUIRE(trailing.has_value());
  REQUIRE(*trailing == Decimal64::parse("1.5"));
  REQUIRE(trailing->str() == "1.5");
}

TEST_CASE("decimal arithmetic reports overflow instead of wrapping") {
  const Decimal64 huge(std::numeric_limits<std::int64_t>::max(), 0);
  REQUIRE_FALSE(decimal_add(huge, Decimal64::parse("1")).has_value());

  const Decimal64 tiny(std::numeric_limits<std::int64_t>::min(), 0);
  REQUIRE_FALSE(decimal_subtract(tiny, Decimal64::parse("1")).has_value());

  // Aligning scales can overflow even when the sum would not.
  REQUIRE_FALSE(decimal_add(huge, Decimal64::parse("0.0001")).has_value());
}

TEST_CASE("a depth snapshot reports the best levels, the spread, and the midpoint") {
  const auto full = book("70000", "70100");
  REQUIRE(full.best_bid().has_value());
  REQUIRE(full.best_bid()->price_ == Decimal64::parse("70000"));
  REQUIRE(full.best_ask()->quantity_ == Decimal64::parse("7"));
  REQUIRE(full.spread()->str() == "100");
  REQUIRE(full.mid()->str() == "70050");

  // An odd sum gains a decimal place rather than being rounded to the tick grid,
  // which is a decision for whoever places the order.
  const auto odd = book("70000", "70001");
  REQUIRE(odd.mid()->str() == "70000.5");
}

TEST_CASE("an empty side of the book yields no price rather than zero") {
  REQUIRE_FALSE(book(nullptr, "70100").best_bid().has_value());
  REQUIRE_FALSE(book("70000", nullptr).best_ask().has_value());
  REQUIRE_FALSE(book(nullptr, "70100").spread().has_value());
  REQUIRE_FALSE(book("70000", nullptr).mid().has_value());
  REQUIRE_FALSE(DepthSnapshot{}.spread().has_value());
}

TEST_CASE("the KRX grid selects the right tick at every band boundary") {
  const auto rule = PriceRule::krx_equity();
  REQUIRE_FALSE(rule.source_.empty());
  REQUIRE(rule.kind_ == PriceRule::Kind::STATED);

  struct Case {
    const char* price;
    const char* tick;
  };
  // The boundary is inclusive at the bottom of each band, so 2000 pays the higher
  // tick and 1999 the lower one.
  const Case cases[] = {
    {"1", "1"},         {"1999", "1"},
    {"2000", "5"},      {"4999", "5"},
    {"5000", "10"},     {"19999", "10"},
    {"20000", "50"},    {"49999", "50"},
    {"50000", "100"},   {"199999", "100"},
    {"200000", "500"},  {"499999", "500"},
    {"500000", "1000"}, {"1000000", "1000"},
  };
  for (const auto& entry : cases) {
    const auto tick = rule.tick_at(Decimal64::parse(entry.price));
    REQUIRE(tick.has_value());
    REQUIRE(tick->str() == entry.tick);
  }
}

TEST_CASE("rounding to the grid never improves a price past the instruction") {
  const auto rule = PriceRule::krx_equity();

  // A buy rounds down: rounding up would pay more than the caller instructed.
  const auto buy = rule.round_to_tick(Decimal64::parse("70123"), OrderSide::BUY);
  REQUIRE(buy->str() == "70100");

  // A sell rounds up, for the same reason in the other direction.
  const auto sell = rule.round_to_tick(Decimal64::parse("70123"), OrderSide::SELL);
  REQUIRE(sell->str() == "70200");

  // A price already on the grid is returned unchanged, either way.
  REQUIRE(rule.on_tick(Decimal64::parse("70100")) == true);
  REQUIRE(rule.round_to_tick(Decimal64::parse("70100"), OrderSide::BUY)->str() == "70100");
  REQUIRE(rule.round_to_tick(Decimal64::parse("70100"), OrderSide::SELL)->str() == "70100");
  REQUIRE(rule.on_tick(Decimal64::parse("70123")) == false);

  // Rounding crosses a band boundary correctly.
  REQUIRE(rule.round_to_tick(Decimal64::parse("1999.5"), OrderSide::BUY)->str() == "1999");
  REQUIRE(rule.round_to_tick(Decimal64::parse("2003"), OrderSide::SELL)->str() == "2005");
}

TEST_CASE("the grid an instrument gets depends on what it is, not only where") {
  // KRX prices funds and notes on a different ladder from cash equities, and neither
  // broker publishes either, so attaching the equity table to an ETF would round a
  // price onto a ladder the exchange does not use.
  REQUIRE(price_rule_for(Country::KR, SecurityType::STOCK).kind_ == PriceRule::Kind::STATED);
  REQUIRE(price_rule_for(Country::KR, SecurityType::REIT).kind_ == PriceRule::Kind::STATED);
  REQUIRE(
    price_rule_for(Country::KR, SecurityType::DEPOSITARY_RECEIPT).kind_ ==
    PriceRule::Kind::STATED
  );

  REQUIRE(price_rule_for(Country::KR, SecurityType::ETF).kind_ == PriceRule::Kind::UNKNOWN);
  REQUIRE(price_rule_for(Country::KR, SecurityType::ETN).kind_ == PriceRule::Kind::UNKNOWN);
  REQUIRE(
    price_rule_for(Country::KR, SecurityType::UNKNOWN).kind_ == PriceRule::Kind::UNKNOWN
  );

  // A market quoted in free decimals is unconstrained, which is a different answer.
  REQUIRE(
    price_rule_for(Country::US, SecurityType::STOCK).kind_ == PriceRule::Kind::UNCONSTRAINED
  );
}

TEST_CASE("a market segment normalizes what it knows and preserves what it does not") {
  REQUIRE(market_segment("KOSPI").known_ == MarketSegment::Known::KOSPI);
  REQUIRE(market_segment("NASDAQ").known_ == MarketSegment::Known::NASDAQ);
  REQUIRE(market_segment("KOSPI").str() == "KOSPI");

  // An unrecognized segment keeps the broker's own code rather than being
  // collapsed into a sentinel that discards it.
  const auto other = market_segment("KR_ETC");
  REQUIRE(other.known_ == MarketSegment::Known::UNKNOWN);
  REQUIRE(other.other_.view() == "KR_ETC");
  REQUIRE(other.str() == "KR_ETC");
  REQUIRE(other != market_segment("US_ETC"));

  // A value that cannot be stored reads as unknown instead of throwing.
  const auto unstorable = market_segment("A_VERY_LONG_SEGMENT_NAME");
  REQUIRE(unstorable.known_ == MarketSegment::Known::UNKNOWN);
  REQUIRE(unstorable.other_.empty());
  REQUIRE(unstorable.str() == "UNKNOWN");
}
