#include <algotradelib/analytics>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace algotradelib;
using analytics::Scalar;
using analytics::missing;
using analytics::observed;
using analytics::round_scalar_to_tick;
using analytics::to_decimal;
using analytics::to_scalar;
using analytics::bar_field_index;

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

namespace {

using analytics::Alignment;
using analytics::BarField;
using analytics::Panel;
using analytics::SeriesFrame;
using analytics::TimeIndex;

/** Returns an instant `day` days after a fixed origin, so tests read by number. */
TimeIndex::Instant day_of(int day) {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * day);
}

/** Returns a one-day bar closing at `close`, stamped on `day`. */
Bar day_bar(int day, const char* close) {
  Bar bar;
  bar.timestamp_ = day_of(day);
  bar.open_ = Decimal64::parse(close);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000");
  return bar;
}

/** Returns a series for `symbol` whose bars fall on the listed days. */
BarSeries series_for(const char* symbol, std::vector<std::pair<int, const char*>> closes) {
  BarSeries series;
  series.instrument_ = InstrumentId{Country::KR, Venue::KRX, symbol};
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::OPEN;
  series.currency_ = Currency::krw();
  for (const auto& entry : closes) {
    series.bars_.push_back(day_bar(entry.first, entry.second));
  }
  return series;
}

} // namespace

TEST_CASE("a time index refuses anything but strictly ascending instants") {
  REQUIRE_NOTHROW(TimeIndex(std::vector<TimeIndex::Instant>{day_of(1), day_of(2), day_of(5)}));
  REQUIRE_NOTHROW(TimeIndex(std::vector<TimeIndex::Instant>{}));

  // Out of order, and repeated, both make every lookup ambiguous.
  REQUIRE_THROWS_AS(
    TimeIndex(std::vector<TimeIndex::Instant>{day_of(2), day_of(1)}), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
    TimeIndex(std::vector<TimeIndex::Instant>{day_of(1), day_of(1)}), std::invalid_argument
  );
}

TEST_CASE("an as-of lookup never reaches forward") {
  const TimeIndex index(std::vector<TimeIndex::Instant>{day_of(1), day_of(3), day_of(7)});

  REQUIRE(index.asof(day_of(1)) == 0);
  REQUIRE(index.asof(day_of(2)) == 0);
  REQUIRE(index.asof(day_of(3)) == 1);
  REQUIRE(index.asof(day_of(100)) == 2);

  // Before the whole index there is no observation to report, and reporting the first
  // one would be the worst kind of lookahead.
  REQUIRE_FALSE(index.asof(day_of(0)).has_value());

  // An exact lookup is not an as-of lookup.
  REQUIRE(index.find(day_of(3)) == 1);
  REQUIRE_FALSE(index.find(day_of(2)).has_value());
}

TEST_CASE("bars become a frame once, and an absent value stays unobserved") {
  auto series = series_for("005930", {{1, "70000"}, {2, "71000"}, {3, "72000"}});
  // A day on which nothing traded has no open.
  series.bars_[1].open_.reset();

  const auto frame = analytics::from_bars(series);
  REQUIRE(frame.rows() == 3);
  REQUIRE(frame.columns() == analytics::bar_field_count);
  REQUIRE(frame.instrument().symbol_ == "005930");

  REQUIRE(frame.close()(0) == 70000.0);
  REQUIRE(frame.close()(2) == 72000.0);
  REQUIRE(observed(frame.open()(0)));
  REQUIRE_FALSE(observed(frame.open()(1)));

  // A field no broker reported is unobserved for every row rather than zero.
  REQUIRE_FALSE(observed(frame.at(0, BarField::TRADED_VALUE)));

  REQUIRE(frame.time().at(1) == day_of(2));
  REQUIRE(frame.time().asof(day_of(2)) == 1);
}

TEST_CASE("a frame column references the frame's own storage rather than a copy") {
  const auto frame = analytics::from_bars(series_for("005930", {{1, "1"}, {2, "2"}, {3, "3"}}));

  // Two views of the same column address the same memory, which is what makes reading a
  // series free. Column-major storage with time on the rows is what allows it.
  REQUIRE(frame.close().data() == frame.column(BarField::CLOSE).data());
  REQUIRE(frame.close().data() == frame.values().col(bar_field_index(BarField::CLOSE)).data());

  // And the column is contiguous, so an indicator walks it without a stride.
  REQUIRE(frame.close().innerStride() == 1);
}

TEST_CASE("an out-of-order series is refused rather than indexed") {
  BarSeries backwards;
  backwards.instrument_ = InstrumentId{Country::KR, Venue::KRX, "005930"};
  backwards.bars_.push_back(day_bar(3, "72000"));
  backwards.bars_.push_back(day_bar(1, "70000"));
  REQUIRE_FALSE(backwards.ascending());
  REQUIRE_THROWS_AS(analytics::from_bars(backwards), std::invalid_argument);
}

TEST_CASE("panel alignment must be stated, and each choice answers differently") {
  // Two instruments that traded on overlapping but different days: the second did not
  // trade on day 2 and the first did not trade on day 4.
  const std::vector<BarSeries> universe = {
    series_for("005930", {{1, "100"}, {2, "110"}, {3, "120"}}),
    series_for("000660", {{1, "200"}, {3, "220"}, {4, "230"}}),
  };

  SECTION("the union leaves gaps unobserved and invents nothing") {
    const auto panel = Panel::from_bars(universe, BarField::CLOSE, Alignment::UNION_UNOBSERVED);
    REQUIRE(panel.rows() == 4);
    REQUIRE(panel.columns() == 2);
    REQUIRE(panel.alignment() == Alignment::UNION_UNOBSERVED);

    REQUIRE(panel.at(1, 0) == 110.0);
    REQUIRE_FALSE(observed(panel.at(1, 1)));  // second instrument did not trade
    REQUIRE_FALSE(observed(panel.at(3, 0)));  // first instrument did not trade
    REQUIRE(panel.at(3, 1) == 230.0);
  }

  SECTION("the intersection keeps only complete rows") {
    const auto panel = Panel::from_bars(universe, BarField::CLOSE, Alignment::INTERSECTION);
    REQUIRE(panel.rows() == 2);
    REQUIRE(panel.time().at(0) == day_of(1));
    REQUIRE(panel.time().at(1) == day_of(3));

    // Every value is observed, which is what a cross-sectional statistic needs.
    for (std::size_t row = 0; row < panel.rows(); ++row) {
      for (std::size_t column = 0; column < panel.columns(); ++column) {
        REQUIRE(observed(panel.at(row, column)));
      }
    }
  }

  SECTION("a forward fill carries the last observation and never fills before the first") {
    const auto panel = Panel::from_bars(universe, BarField::CLOSE, Alignment::FORWARD_FILL);
    REQUIRE(panel.rows() == 4);

    // Day 2: the second instrument's day-1 price is carried forward. It is a real price
    // that is no longer current, which is why this alignment is the easiest to misuse.
    REQUIRE(panel.at(1, 1) == 200.0);

    // Day 4: the first instrument's day-3 price is carried forward.
    REQUIRE(panel.at(3, 0) == 120.0);
  }

  SECTION("a filled gap makes a return read as zero and then jump") {
    // The reason forward fill is documented as the easiest to misuse, asserted rather
    // than described: nothing traded on day 2, so the filled return is zero, and the
    // move that did happen lands entirely on day 3.
    const auto panel = Panel::from_bars(universe, BarField::CLOSE, Alignment::FORWARD_FILL);
    const auto second = panel.column(1);
    REQUIRE((second(1) - second(0)) == 0.0);
    REQUIRE((second(2) - second(1)) == 20.0);
  }

  SECTION("a forward fill leaves an instrument's pre-history unobserved") {
    const std::vector<BarSeries> staggered = {
      series_for("005930", {{1, "100"}, {2, "110"}}),
      series_for("000660", {{2, "200"}}),
    };
    const auto panel = Panel::from_bars(staggered, BarField::CLOSE, Alignment::FORWARD_FILL);
    REQUIRE(panel.rows() == 2);

    // There is nothing to carry backwards, so day 1 stays unobserved.
    REQUIRE_FALSE(observed(panel.at(0, 1)));
    REQUIRE(panel.at(1, 1) == 200.0);
  }
}

TEST_CASE("a panel locates an instrument by identity and reports its own shape") {
  const std::vector<BarSeries> universe = {
    series_for("005930", {{1, "100"}}),
    series_for("000660", {{1, "200"}}),
  };
  const auto panel = Panel::from_bars(universe, BarField::VOLUME, Alignment::INTERSECTION);

  REQUIRE(panel.field() == BarField::VOLUME);
  REQUIRE(panel.find(InstrumentId{Country::KR, Venue::KRX, "000660"}) == 1);

  // The identity has to match, not just the symbol: a different venue is a different
  // instrument, exactly as the market-data adapters treat it.
  REQUIRE_FALSE(panel.find(InstrumentId{Country::KR, Venue::COMBINED, "000660"}).has_value());

  REQUIRE(Panel::from_bars({}, BarField::CLOSE, Alignment::INTERSECTION).rows() == 0);
  REQUIRE(analytics::alignment_name(Alignment::FORWARD_FILL) == "forward_fill");
  REQUIRE(analytics::bar_field_name(BarField::CLOSE) == "close");
}
