#include <algotradelib/analytics>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace algotradelib;
namespace math = algotradelib::math;
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

TEST_CASE("the math module's scalar needs no linear algebra backend") {
  // `math/scalar.hpp` is deliberately backend-free, so the numeric vocabulary is
  // available to code that does no linear algebra at all.
  REQUIRE_FALSE(math::observed(math::missing()));
  REQUIRE(math::observed(math::Scalar(0)));
  REQUIRE(std::isnan(math::missing()));

  // The linear algebra vocabulary is the module's, not the analytics layer's.
  const auto blank = math::unobserved(3);
  REQUIRE(blank.size() == 3);
  for (math::Index position = 0; position < blank.size(); ++position) {
    REQUIRE_FALSE(math::observed(blank(position)));
  }

  const auto grid = math::unobserved(2, 4);
  REQUIRE(grid.rows() == 2);
  REQUIRE(grid.cols() == 4);
  REQUIRE_FALSE(math::observed(grid(1, 3)));
}

TEST_CASE("aligning a panel walks both indices and stops at their edges") {
  // The alignment walks two ascending indices together rather than searching per row, so
  // its boundaries are worth pinning: a target instant before the source's first
  // observation, and one after its last.
  const std::vector<BarSeries> staggered = {
    series_for("005930", {{1, "100"}, {2, "110"}, {3, "120"}, {4, "130"}}),
    series_for("000660", {{2, "200"}, {3, "210"}}),
  };

  SECTION("an exact alignment matches only where both reported") {
    const auto panel = Panel::from_bars(staggered, BarField::CLOSE, Alignment::UNION_UNOBSERVED);
    REQUIRE(panel.rows() == 4);

    // Before the second instrument's history, and after it.
    REQUIRE_FALSE(observed(panel.at(0, 1)));
    REQUIRE(panel.at(1, 1) == 200.0);
    REQUIRE(panel.at(2, 1) == 210.0);
    REQUIRE_FALSE(observed(panel.at(3, 1)));

    // The first instrument reported on every row.
    for (std::size_t row = 0; row < panel.rows(); ++row) {
      REQUIRE(observed(panel.at(row, 0)));
    }
  }

  SECTION("a carried alignment stops before the first and holds after the last") {
    const auto panel = Panel::from_bars(staggered, BarField::CLOSE, Alignment::FORWARD_FILL);

    // Nothing to carry backwards, so the row before the history stays unobserved.
    REQUIRE_FALSE(observed(panel.at(0, 1)));

    // After the last observation the value is held, which is the behaviour the
    // alignment promises and the reason it is the easiest to misuse.
    REQUIRE(panel.at(3, 1) == 210.0);
  }

  SECTION("a source entirely outside the target contributes nothing") {
    const std::vector<BarSeries> disjoint = {
      series_for("005930", {{1, "100"}, {2, "110"}}),
      series_for("000660", {{50, "900"}}),
    };
    const auto panel = Panel::from_bars(disjoint, BarField::CLOSE, Alignment::INTERSECTION);
    REQUIRE(panel.rows() == 0);
    REQUIRE(panel.columns() == 2);
  }
}

TEST_CASE("a rolling window keeps the last observations in one contiguous run") {
  analytics::RollingWindow window(4, 1);
  REQUIRE(window.capacity() == 4);
  REQUIRE(window.fields() == 1);
  REQUIRE(window.empty());
  REQUIRE_FALSE(window.full());

  for (int value = 1; value <= 4; ++value) {
    window.push(day_of(value), std::vector<math::Scalar>{double(value)});
  }
  REQUIRE(window.size() == 4);
  REQUIRE(window.full());

  const auto series = window.column(std::size_t{0});
  REQUIRE(series.size() == 4);
  REQUIRE(series(0) == 1.0);
  REQUIRE(series(3) == 4.0);

  // Oldest first, and one span with no stride, which is what a computation needs.
  REQUIRE(series.innerStride() == 1);
  REQUIRE(window.instant(0) == day_of(1));
  REQUIRE(window.newest_instant() == day_of(4));

  // Past capacity the oldest is dropped rather than the newest refused.
  window.push(day_of(5), std::vector<math::Scalar>{5.0});
  REQUIRE(window.size() == 4);
  REQUIRE(window.column(std::size_t{0})(0) == 2.0);
  REQUIRE(window.column(std::size_t{0})(3) == 5.0);
  REQUIRE(window.instant(0) == day_of(2));
}

TEST_CASE("a rolling window compacts at its amortized bound and never allocates for it") {
  constexpr std::size_t capacity = 8;
  analytics::RollingWindow window(capacity, 2);

  // Storage holds twice the capacity, so a compaction can only happen once per capacity
  // pushes. Asserting the count is what makes the amortized bound a fact rather than a
  // claim in a comment.
  constexpr std::size_t pushes = 800;
  for (std::size_t step = 0; step < pushes; ++step) {
    window.push(day_of(static_cast<int>(step)), std::vector<math::Scalar>{double(step), 0.0});
  }

  REQUIRE(window.size() == capacity);
  REQUIRE(window.compactions() <= pushes / capacity + 1);
  REQUIRE(window.compactions() >= pushes / (capacity * 2));

  // The contents survive every compaction: the window still holds exactly the last
  // `capacity` observations, oldest first.
  const auto series = window.column(std::size_t{0});
  REQUIRE(series.size() == static_cast<math::Index>(capacity));
  for (std::size_t position = 0; position < capacity; ++position) {
    REQUIRE(series(static_cast<math::Index>(position)) == double(pushes - capacity + position));
  }
  REQUIRE(series.innerStride() == 1);

  // And a view is still one contiguous run after all of that, which is the property the
  // compaction exists to preserve and a ring buffer would have lost.
  REQUIRE(window.at(0, 0) == double(pushes - capacity));
  REQUIRE(window.newest_instant() == day_of(static_cast<int>(pushes - 1)));
}

TEST_CASE("a rolling window refuses an observation that would break its ordering") {
  analytics::RollingWindow window(3, 1);
  window.push(day_of(5), std::vector<math::Scalar>{1.0});

  // Out of order, and repeated, both make a trailing computation over the window
  // meaningless, so neither is accepted.
  REQUIRE_THROWS_AS(
    window.push(day_of(4), std::vector<math::Scalar>{2.0}), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
    window.push(day_of(5), std::vector<math::Scalar>{2.0}), std::invalid_argument
  );
  REQUIRE_NOTHROW(window.push(day_of(6), std::vector<math::Scalar>{2.0}));

  // A row that does not carry every field is refused rather than padded.
  REQUIRE_THROWS_AS(
    window.push(day_of(7), std::vector<math::Scalar>{1.0, 2.0}), std::invalid_argument
  );

  REQUIRE_THROWS_AS(analytics::RollingWindow(0, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::RollingWindow(4, 0), std::invalid_argument);
}

TEST_CASE("a rolling window takes a bar the way a live loop receives one") {
  analytics::RollingWindow window(2);
  REQUIRE(window.fields() == analytics::bar_field_count);

  auto first = day_bar(1, "70000");
  auto second = day_bar(2, "71000");
  // A trading day on which nothing traded has no open, and that stays unobserved here too.
  second.open_.reset();

  window.push(first);
  window.push(second);

  REQUIRE(window.size() == 2);
  REQUIRE(window.column(BarField::CLOSE)(0) == 70000.0);
  REQUIRE(window.column(BarField::CLOSE)(1) == 71000.0);
  REQUIRE(observed(window.column(BarField::OPEN)(0)));
  REQUIRE_FALSE(observed(window.column(BarField::OPEN)(1)));

  // A field neither bar reported is unobserved rather than zero.
  REQUIRE_FALSE(observed(window.column(BarField::TRADED_VALUE)(0)));

  window.clear();
  REQUIRE(window.empty());
  REQUIRE(window.capacity() == 2);
}

TEST_CASE("a rolling window's storage is taken once and never grows") {
  // The claim is that a live loop does not allocate on the path that runs every tick.
  // Storage is one matrix taken at construction, so its base address never moves; the
  // only thing that moves is where the held run starts inside it, and that is bounded by
  // the capacity. If the buffer were regrowing, the observed addresses would wander by
  // arbitrary amounts instead of within that bound.
  constexpr std::size_t capacity = 16;
  analytics::RollingWindow window(capacity, 1);

  const char* lowest = nullptr;
  const char* highest = nullptr;
  for (std::size_t step = 0; step < 5000; ++step) {
    window.push(day_of(static_cast<int>(step)), std::vector<math::Scalar>{double(step)});
    const auto* address = reinterpret_cast<const char*>(window.column(std::size_t{0}).data());
    if (!lowest || address < lowest) {
      lowest = address;
    }
    if (!highest || address > highest) {
      highest = address;
    }
  }

  REQUIRE(lowest != nullptr);
  REQUIRE(highest != nullptr);

  // Every observed start sits inside one buffer of twice the capacity, so the span can be
  // at most `capacity` elements: the run's start never passes the compaction point.
  const auto span = static_cast<std::size_t>(highest - lowest);
  REQUIRE(span <= capacity * sizeof(math::Scalar));

  // And after five thousand pushes the window still holds exactly its capacity.
  REQUIRE(window.size() == capacity);
  REQUIRE(window.compactions() > 0);
}

// ---------------------------------------------------------------------------
// Returns and trailing statistics
// ---------------------------------------------------------------------------

namespace {

/** Builds a series from a literal list, so a fixture reads as the numbers it is. */
math::Vector series(std::initializer_list<Scalar> values) {
  math::Vector result(static_cast<math::Index>(values.size()));
  math::Index row = 0;
  for (const Scalar value : values) {
    result[row++] = value;
  }
  return result;
}

} // namespace

TEST_CASE("simple returns are unobserved wherever no return exists") {
  const math::Vector prices = series({100.0, 110.0, 99.0, 99.0, 108.9});
  const math::Vector result = analytics::returns(prices);

  REQUIRE(result.size() == prices.size());
  // Row 0 has no prior price. It is unobserved rather than zero, which would be a claim
  // the price did not move.
  REQUIRE_FALSE(observed(result[0]));
  REQUIRE(result[1] == Catch::Approx(0.1));
  REQUIRE(result[2] == Catch::Approx(-0.1));
  REQUIRE(result[3] == Catch::Approx(0.0));
  REQUIRE(result[4] == Catch::Approx(0.1));
}

TEST_CASE("a simple return refuses to divide by a zero prior price") {
  // An infinity here would survive every later reduction: they mask NaN, not inf.
  const math::Vector prices = series({0.0, 10.0, 11.0});
  const math::Vector result = analytics::returns(prices);

  REQUIRE_FALSE(observed(result[1]));
  REQUIRE(result[2] == Catch::Approx(0.1));
}

TEST_CASE("a return spanning an unobserved price is unobserved on both sides") {
  const math::Vector prices = series({100.0, missing(), 121.0});
  const math::Vector result = analytics::returns(prices);

  REQUIRE_FALSE(observed(result[1]));
  // And row 2, whose prior price is the absent one, does not reach back past it to
  // manufacture a two-period return.
  REQUIRE_FALSE(observed(result[2]));
}

TEST_CASE("log returns add where simple returns compound") {
  const math::Vector prices = series({100.0, 110.0, 99.0, 99.0, 108.9});
  const math::Vector result = analytics::log_returns(prices);

  REQUIRE_FALSE(observed(result[0]));
  REQUIRE(result[1] == Catch::Approx(0.09531017980432493));
  REQUIRE(result[2] == Catch::Approx(-0.10536051565782628));
  REQUIRE(result[3] == Catch::Approx(0.0));
  REQUIRE(result[4] == Catch::Approx(0.09531017980432493));

  // The property that makes them worth having: their sum is the total log return.
  const Scalar total = result[1] + result[2] + result[3] + result[4];
  REQUIRE(total == Catch::Approx(std::log(108.9 / 100.0)));
}

TEST_CASE("a log return refuses a non-positive price instead of returning a bare NaN") {
  const math::Vector prices = series({10.0, -1.0, 10.0});
  const math::Vector result = analytics::log_returns(prices);

  REQUIRE_FALSE(observed(result[1]));
  REQUIRE_FALSE(observed(result[2]));
}

TEST_CASE("cumulative returns compound to the total the price series implies") {
  const math::Vector prices = series({100.0, 110.0, 99.0, 99.0, 108.9});
  const math::Vector result = analytics::cumulative_returns(analytics::returns(prices));

  // No capital has been at risk before the first observed return.
  REQUIRE_FALSE(observed(result[0]));
  REQUIRE(result[1] == Catch::Approx(0.1));
  REQUIRE(result[2] == Catch::Approx(-0.01));
  REQUIRE(result[3] == Catch::Approx(-0.01));
  // Which is exactly the return of holding from the first price to the last.
  REQUIRE(result[4] == Catch::Approx(108.9 / 100.0 - 1.0));
}

TEST_CASE("trailing statistics report nothing until their window is full") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});

  const math::Vector sum = analytics::rolling_sum(values, 3);
  const math::Vector mean = analytics::rolling_mean(values, 3);
  const math::Vector smallest = analytics::rolling_min(values, 3);
  const math::Vector largest = analytics::rolling_max(values, 3);

  // The warm-up region is asserted explicitly. An indicator that answers from a
  // partially filled window is a lookahead bug that looks like a good backtest.
  for (math::Index row = 0; row < 2; ++row) {
    REQUIRE_FALSE(observed(sum[row]));
    REQUIRE_FALSE(observed(mean[row]));
    REQUIRE_FALSE(observed(smallest[row]));
    REQUIRE_FALSE(observed(largest[row]));
  }

  REQUIRE(sum[2] == Catch::Approx(6.0));
  REQUIRE(sum[3] == Catch::Approx(9.0));
  REQUIRE(sum[4] == Catch::Approx(12.0));
  REQUIRE(mean[2] == Catch::Approx(2.0));
  REQUIRE(mean[4] == Catch::Approx(4.0));
  REQUIRE(smallest[2] == Catch::Approx(1.0));
  REQUIRE(smallest[4] == Catch::Approx(3.0));
  REQUIRE(largest[2] == Catch::Approx(3.0));
  REQUIRE(largest[4] == Catch::Approx(5.0));
}

TEST_CASE("a shorter warm-up is available but has to be asked for") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});
  const math::Vector mean = analytics::rolling_mean(values, 3, 1);

  REQUIRE(mean[0] == Catch::Approx(1.0));
  REQUIRE(mean[1] == Catch::Approx(1.5));
  REQUIRE(mean[2] == Catch::Approx(2.0));
  REQUIRE(mean[4] == Catch::Approx(4.0));
}

TEST_CASE("a window skips an unobserved value rather than being blanked by it") {
  const math::Vector values = series({1.0, missing(), 3.0, 4.0, 5.0});

  // With the whole window demanded, the two windows containing the gap hold only two
  // observations and stay unobserved.
  const math::Vector strict = analytics::rolling_mean(values, 3);
  REQUIRE_FALSE(observed(strict[2]));
  REQUIRE_FALSE(observed(strict[3]));
  REQUIRE(strict[4] == Catch::Approx(4.0));

  // Relaxed to two observations, the gap is masked and the divisor is the count actually
  // seen -- two, not three.
  const math::Vector relaxed = analytics::rolling_mean(values, 3, 2);
  REQUIRE_FALSE(observed(relaxed[1]));
  REQUIRE(relaxed[2] == Catch::Approx(2.0));
  REQUIRE(relaxed[3] == Catch::Approx(3.5));
  REQUIRE(relaxed[4] == Catch::Approx(4.0));
}

TEST_CASE("a trailing deviation is the sample one and needs two observations") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});
  const math::Vector deviation = analytics::rolling_stdev(values, 3);

  REQUIRE_FALSE(observed(deviation[0]));
  REQUIRE_FALSE(observed(deviation[1]));
  REQUIRE(deviation[2] == Catch::Approx(1.0));
  REQUIRE(deviation[3] == Catch::Approx(1.0));
  REQUIRE(deviation[4] == Catch::Approx(1.0));

  // Even asked for a one-observation warm-up, a single point has no dispersion to
  // report: `count - 1` would be a division by zero.
  const math::Vector relaxed = analytics::rolling_stdev(values, 3, 1);
  REQUIRE_FALSE(observed(relaxed[0]));
  REQUIRE(relaxed[1] == Catch::Approx(std::sqrt(0.5)));
}

TEST_CASE("a trailing deviation survives the price levels that break the one-pass form") {
  // Three values one apart, around a level whose square exceeds what a double holds
  // exactly. The sample deviation is 1 by construction. The one-pass
  // `E[x^2] - E[x]^2` form returns a variance of 2 here -- a deviation of 1.414,
  // wrong by 41% with no warning -- because the two terms agree to more digits than a
  // double has. Two passes over the window is what buys the right answer.
  const math::Vector values = series({1.0e8, 1.0e8 + 1.0, 1.0e8 + 2.0});
  const math::Vector deviation = analytics::rolling_stdev(values, 3);

  REQUIRE(deviation[2] == 1.0);
}

TEST_CASE("a weighted average leans on its most recent row and renormalizes over gaps") {
  const math::Vector values = series({1.0, 2.0, 3.0});
  const math::Vector weighted = analytics::wma(values, 3);

  REQUIRE_FALSE(observed(weighted[0]));
  REQUIRE_FALSE(observed(weighted[1]));
  // (1*1 + 2*2 + 3*3) / (1 + 2 + 3)
  REQUIRE(weighted[2] == Catch::Approx(14.0 / 6.0));

  // Dropping a row drops its weight with it. Keeping the weight in the divisor would
  // bias every window containing a gap toward zero.
  const math::Vector gapped = series({1.0, missing(), 3.0});
  const math::Vector over_gap = analytics::wma(gapped, 3, 2);
  REQUIRE(over_gap[2] == Catch::Approx((1.0 * 1.0 + 3.0 * 3.0) / (1.0 + 3.0)));
}

TEST_CASE("the simple moving average is the trailing mean, and stays that way") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});
  const math::Vector average = analytics::sma(values, 3);
  const math::Vector mean = analytics::rolling_mean(values, 3);

  for (math::Index row = 0; row < values.size(); ++row) {
    REQUIRE(observed(average[row]) == observed(mean[row]));
    if (observed(mean[row])) {
      REQUIRE(average[row] == mean[row]);
    }
  }
}

TEST_CASE("an exponential average uses the span convention and seeds on its first value") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});

  // span 3 is alpha = 2 / (3 + 1) = 0.5, so each level is the midpoint of the previous
  // level and the new value.
  const math::Vector smoothed = analytics::ema(values, 3);
  REQUIRE(smoothed[0] == Catch::Approx(1.0));
  REQUIRE(smoothed[1] == Catch::Approx(1.5));
  REQUIRE(smoothed[2] == Catch::Approx(2.25));
  REQUIRE(smoothed[3] == Catch::Approx(3.125));
  REQUIRE(smoothed[4] == Catch::Approx(4.0625));

  // span 1 is alpha = 1, which is no smoothing at all. A degenerate span should give
  // the series back rather than divide by something.
  const math::Vector unsmoothed = analytics::ema(values, 1);
  for (math::Index row = 0; row < values.size(); ++row) {
    REQUIRE(unsmoothed[row] == values[row]);
  }
}

TEST_CASE("an exponential average carries its level across a gap without reporting one") {
  const math::Vector values = series({1.0, missing(), 3.0});
  const math::Vector smoothed = analytics::ema(values, 3);

  REQUIRE(smoothed[0] == Catch::Approx(1.0));
  // The row itself has no observation, so the indicator says nothing about it.
  REQUIRE_FALSE(observed(smoothed[1]));
  // But the level was not lost: 1 + 0.5 * (3 - 1).
  REQUIRE(smoothed[2] == Catch::Approx(2.0));
}

TEST_CASE("an exponential average can suppress the region its seed still dominates") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});
  const math::Vector smoothed = analytics::ema(values, 3, 3);

  REQUIRE_FALSE(observed(smoothed[0]));
  REQUIRE_FALSE(observed(smoothed[1]));
  REQUIRE(smoothed[2] == Catch::Approx(2.25));
  REQUIRE(smoothed[4] == Catch::Approx(4.0625));
}

TEST_CASE("an impossible window is refused rather than answered with nothing") {
  const math::Vector values = series({1.0, 2.0, 3.0});

  // Both of these would otherwise produce an all-unobserved column, which looks
  // identical to a series that simply had no data.
  REQUIRE_THROWS_AS(analytics::rolling_mean(values, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::rolling_mean(values, 3, 4), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::ema(values, 0), std::invalid_argument);
}

TEST_CASE("an indicator reads a frame column and a rolling window the same way") {
  // The point of one contiguous series per column: the same function serves a completed
  // frame and a live window, so a backtest and a live loop cannot diverge here.
  using namespace std::chrono_literals;
  const auto base = std::chrono::system_clock::time_point{} + 24h;

  analytics::RollingWindow window(3);
  const std::vector<Scalar> rows[] = {
    {0, 0, 0, 10.0, 0, 0},
    {0, 0, 0, 20.0, 0, 0},
    {0, 0, 0, 30.0, 0, 0},
  };
  for (std::size_t at = 0; at < 3; ++at) {
    window.push(base + std::chrono::hours(24 * static_cast<int>(at)), rows[at]);
  }

  const math::Vector from_window =
    analytics::sma(window.column(analytics::BarField::CLOSE), 3);
  REQUIRE(from_window[2] == Catch::Approx(20.0));

  const math::Vector direct = analytics::sma(series({10.0, 20.0, 30.0}), 3);
  REQUIRE(direct[2] == from_window[2]);
}
