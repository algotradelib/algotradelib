#include <algotradelib/analytics>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
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

TEST_CASE("a value too large for an exact decimal is refused, not wrapped") {
  const Scalar huge = 1e30;
  REQUIRE_FALSE(to_decimal(huge, 0).has_value());
  REQUIRE_FALSE(to_decimal(-huge, 0).has_value());

  // Scaling is what overflows first for a large value at a fine scale.
  REQUIRE_FALSE(to_decimal(1e18, 4).has_value());
  REQUIRE(to_decimal(1e15, 0).has_value());

  // 2^63 is the double `INT64_MAX` converts to, and does not fit.
  REQUIRE_FALSE(to_decimal(9223372036854775808.0, 0).has_value());
  REQUIRE(to_decimal(-9223372036854775808.0, 0).has_value());
}

TEST_CASE("a scalar rounds half away from zero without rounding the addition first") {
  CHECK(to_decimal(2.5, 0)->coefficient() == 3);
  CHECK(to_decimal(-2.5, 0)->coefficient() == -3);
  // The largest double below one half: adding 0.5 rounds the sum up to exactly 1.
  CHECK(to_decimal(0.49999999999999994, 0)->coefficient() == 0);
  CHECK(to_decimal(-0.49999999999999994, 0)->coefficient() == 0);
  // An odd integer between 2^52 and 2^53 is exact, and adding 0.5 moved it to the next even one.
  CHECK(to_decimal(4503599627370497.0, 0)->coefficient() == 4503599627370497LL);
}

TEST_CASE("a decimal converts to the nearest double, as its text parses") {
  // Python's float and strtod give the nearest double; analytics on the same prices must start from
  // the same doubles. 16896.7559 is a split-adjusted close that four successive divisions put an ulp
  // low.
  for (const char* text : {"0.07", "0.29", "185.71", "16896.7559", "98765.43219", "1234567.891234",
                           "-0.1", "70000.5", "70000", "0.000001", "9007199254740.991"}) {
    REQUIRE(to_scalar(Decimal64::parse(text)) == std::strtod(text, nullptr));
  }
  // The edges of the promise: 53 bits of coefficient, either sign, and 22 places.
  const std::int64_t widest = (std::int64_t{1} << 53) - 1;
  for (const auto& value : {Decimal64(1, 22), Decimal64(-1, 22), Decimal64(widest, 22),
                            Decimal64(-widest, 22), Decimal64(7612685828371001, 15), Decimal64(widest, 0),
                            Decimal64(std::numeric_limits<std::int64_t>::min(), 0)}) {
    REQUIRE(to_scalar(value) == std::strtod(value.str().c_str(), nullptr));
  }
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

  // A market quoting in free decimals rounds to the requested scale, in the order's favour.
  const auto free = PriceRule::unconstrained();
  REQUIRE(round_scalar_to_tick(185.7049, free, OrderSide::BUY, 2)->str() == "185.70");
  REQUIRE(round_scalar_to_tick(185.7051, free, OrderSide::BUY, 2)->str() == "185.70");
  REQUIRE(round_scalar_to_tick(185.7049, free, OrderSide::SELL, 2)->str() == "185.71");
}

TEST_CASE("a computed price never crosses its value on the way to the grid") {
  const auto rule = PriceRule::krx_equity();
  // Rounding to the nearest won first made these 2000: above the buy, below the sell.
  REQUIRE(round_scalar_to_tick(1999.8, rule, OrderSide::BUY)->str() == "1999");
  REQUIRE(round_scalar_to_tick(2000.2, rule, OrderSide::SELL)->str() == "2005");

  // A decimal that went through a scalar comes back as itself, not one unit off, at any scale a
  // market quotes in.
  const auto free = PriceRule::unconstrained();
  for (const char* text : {"185.71", "0.29", "70100", "1234.57", "98765.43219", "1234567.891234"}) {
    const auto decimal = Decimal64::parse(text);
    const auto scalar = analytics::to_scalar(decimal);
    for (const auto side : {OrderSide::BUY, OrderSide::SELL}) {
      const auto price = round_scalar_to_tick(scalar, free, side, decimal.scale());
      REQUIRE(price);
      REQUIRE_FALSE(decimal_less(*price, decimal));
      REQUIRE_FALSE(decimal_less(decimal, *price));
    }
  }
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

TEST_CASE("a series buffer keeps the last observations in one contiguous run") {
  analytics::SeriesBuffer buffer(4, 1);
  REQUIRE(buffer.capacity() == 4);
  REQUIRE(buffer.fields() == 1);
  REQUIRE(buffer.empty());
  REQUIRE_FALSE(buffer.full());

  for (int value = 1; value <= 4; ++value) {
    buffer.push(day_of(value), std::vector<math::Scalar>{double(value)});
  }
  REQUIRE(buffer.size() == 4);
  REQUIRE(buffer.full());

  const auto series = buffer.column(std::size_t{0});
  REQUIRE(series.size() == 4);
  REQUIRE(series(0) == 1.0);
  REQUIRE(series(3) == 4.0);

  // Oldest first, and one span with no stride, which is what a computation needs.
  REQUIRE(series.innerStride() == 1);
  REQUIRE(buffer.instant(0) == day_of(1));
  REQUIRE(buffer.newest_instant() == day_of(4));

  // Past capacity the oldest is dropped rather than the newest refused.
  buffer.push(day_of(5), std::vector<math::Scalar>{5.0});
  REQUIRE(buffer.size() == 4);
  REQUIRE(buffer.column(std::size_t{0})(0) == 2.0);
  REQUIRE(buffer.column(std::size_t{0})(3) == 5.0);
  REQUIRE(buffer.instant(0) == day_of(2));
}

TEST_CASE("a series buffer compacts at its amortized bound and never allocates for it") {
  constexpr std::size_t capacity = 8;
  analytics::SeriesBuffer buffer(capacity, 2);

  // Storage holds twice the capacity, so a compaction can only happen once per capacity
  // pushes. Asserting the count is what makes the amortized bound a fact rather than a
  // claim in a comment.
  constexpr std::size_t pushes = 800;
  for (std::size_t step = 0; step < pushes; ++step) {
    buffer.push(day_of(static_cast<int>(step)), std::vector<math::Scalar>{double(step), 0.0});
  }

  REQUIRE(buffer.size() == capacity);
  REQUIRE(buffer.compactions() <= pushes / capacity + 1);
  REQUIRE(buffer.compactions() >= pushes / (capacity * 2));

  // The contents survive every compaction: the buffer still holds exactly the last
  // `capacity` observations, oldest first.
  const auto series = buffer.column(std::size_t{0});
  REQUIRE(series.size() == static_cast<math::Index>(capacity));
  for (std::size_t position = 0; position < capacity; ++position) {
    REQUIRE(series(static_cast<math::Index>(position)) == double(pushes - capacity + position));
  }
  REQUIRE(series.innerStride() == 1);

  // And a view is still one contiguous run after all of that, which is the property the
  // compaction exists to preserve and a ring buffer would have lost.
  REQUIRE(buffer.at(0, 0) == double(pushes - capacity));
  REQUIRE(buffer.newest_instant() == day_of(static_cast<int>(pushes - 1)));
}

TEST_CASE("a series buffer refuses an observation that would break its ordering") {
  analytics::SeriesBuffer buffer(3, 1);
  buffer.push(day_of(5), std::vector<math::Scalar>{1.0});

  // Out of order, and repeated, both make a trailing computation over the buffer
  // meaningless, so neither is accepted.
  REQUIRE_THROWS_AS(
    buffer.push(day_of(4), std::vector<math::Scalar>{2.0}), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
    buffer.push(day_of(5), std::vector<math::Scalar>{2.0}), std::invalid_argument
  );
  REQUIRE_NOTHROW(buffer.push(day_of(6), std::vector<math::Scalar>{2.0}));

  // A row that does not carry every field is refused rather than padded.
  REQUIRE_THROWS_AS(
    buffer.push(day_of(7), std::vector<math::Scalar>{1.0, 2.0}), std::invalid_argument
  );

  REQUIRE_THROWS_AS(analytics::SeriesBuffer(0, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::SeriesBuffer(4, 0), std::invalid_argument);
}

TEST_CASE("a series buffer takes a bar the way a live loop receives one") {
  analytics::SeriesBuffer buffer(2);
  REQUIRE(buffer.fields() == analytics::bar_field_count);

  auto first = day_bar(1, "70000");
  auto second = day_bar(2, "71000");
  // A trading day on which nothing traded has no open, and that stays unobserved here too.
  second.open_.reset();

  buffer.push(first);
  buffer.push(second);

  REQUIRE(buffer.size() == 2);
  REQUIRE(buffer.column(BarField::CLOSE)(0) == 70000.0);
  REQUIRE(buffer.column(BarField::CLOSE)(1) == 71000.0);
  REQUIRE(observed(buffer.column(BarField::OPEN)(0)));
  REQUIRE_FALSE(observed(buffer.column(BarField::OPEN)(1)));

  // A field neither bar reported is unobserved rather than zero.
  REQUIRE_FALSE(observed(buffer.column(BarField::TRADED_VALUE)(0)));

  buffer.clear();
  REQUIRE(buffer.empty());
  REQUIRE(buffer.capacity() == 2);
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

TEST_CASE("a simple return refuses a negative prior price") {
  // From a negative base the sign reverses: -10 to -20 would read as +100%.
  const math::Vector prices = series({-10.0, -20.0, 5.0, 6.0});
  const math::Vector result = analytics::returns(prices);

  REQUIRE_FALSE(observed(result[1]));
  REQUIRE_FALSE(observed(result[2]));
  REQUIRE(result[3] == Catch::Approx(0.2));
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

TEST_CASE("an impossible window is refused rather than answered with nothing") {
  const math::Vector values = series({1.0, 2.0, 3.0});

  // Both of these would otherwise produce an all-unobserved column, which looks
  // identical to a series that simply had no data.
  REQUIRE_THROWS_AS(analytics::rolling_mean(values, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::rolling_mean(values, 3, 4), std::invalid_argument);
}

namespace {

/** Whether `ema(view, smoothing)` is a well-formed call. */
template <typename Smoothing, typename = void>
struct ema_accepts : std::false_type {};
template <typename Smoothing>
struct ema_accepts<
  Smoothing,
  std::void_t<decltype(analytics::ema(std::declval<math::SeriesView>(), std::declval<Smoothing>()))>
> : std::true_type {};

/** Whether `ema_step(level, value, smoothing)` is a well-formed call. */
template <typename Smoothing, typename = void>
struct ema_step_accepts : std::false_type {};
template <typename Smoothing>
struct ema_step_accepts<
  Smoothing,
  std::void_t<decltype(analytics::ema_step(Scalar{}, Scalar{}, std::declval<Smoothing>()))>
> : std::true_type {};

} // namespace

// An integer where the smoothing factor goes is an old span. It must not compile as alpha = 12,
// which would only be refused when the call first runs -- possibly inside a live loop.
static_assert(ema_accepts<Scalar>::value, "ema takes a smoothing factor");
static_assert(ema_accepts<float>::value, "a float is promoted, not ambiguous");
static_assert(!ema_accepts<int>::value, "an int is a span: alpha_from_span");
static_assert(!ema_accepts<std::size_t>::value, "a size_t is a span: alpha_from_span");
static_assert(ema_step_accepts<Scalar>::value, "ema_step takes a smoothing factor");
static_assert(!ema_step_accepts<int>::value, "an int is a span: alpha_from_span");
static_assert(!ema_step_accepts<std::size_t>::value, "a size_t is a span: alpha_from_span");

TEST_CASE("an exponential average refuses a smoothing factor outside (0, 1]") {
  const math::Vector values = series({1.0, 2.0, 3.0});
  const math::Vector empty = series({});
  for (const Scalar alpha : {0.0, -0.5, 1.5, std::numeric_limits<Scalar>::quiet_NaN()}) {
    REQUIRE_THROWS_AS(analytics::ema(values, alpha), std::invalid_argument);
    // Before anything is read: an empty series refuses too.
    REQUIRE_THROWS_AS(analytics::ema(empty, alpha), std::invalid_argument);
    REQUIRE_THROWS_AS(analytics::ema_step(1.0, 2.0, alpha), std::invalid_argument);
  }
}

TEST_CASE("the exponential average keeps its arithmetic, digit for digit") {
  // `level + alpha * (value - level)`, in that order. The algebraically equal
  // `alpha * value + (1 - alpha) * level` rounds differently on values like these, and would move
  // every EMA, MACD and crossover line in the last place.
  const math::Vector values = series({54.689, 897.428});
  REQUIRE(analytics::ema(values, analytics::alpha_from_span(3))[1] == 476.05850000000004);
  REQUIRE(analytics::ema_step(54.689, 897.428, analytics::alpha_from_span(3)) == 476.05850000000004);
  // IEEE, unfused (the suite is built with -ffp-contract=off): a fused multiply-add gives 67.227042735042758.
  REQUIRE(analytics::macd(values).macd_[1] == 67.22704273504274);
}

// ---------------------------------------------------------------------------
// Named indicators
// ---------------------------------------------------------------------------

TEST_CASE("Bollinger bands straddle the trailing mean by the deviation asked for") {
  const math::Vector values = series({1.0, 2.0, 3.0, 4.0, 5.0});
  const auto bands = analytics::bollinger(values, 3, 2.0);

  REQUIRE(bands.middle_.size() == values.size());
  REQUIRE_FALSE(observed(bands.upper_[1]));
  // Window {1,2,3}: mean 2, sample deviation 1.
  REQUIRE(bands.middle_[2] == Catch::Approx(2.0));
  REQUIRE(bands.upper_[2] == Catch::Approx(4.0));
  REQUIRE(bands.lower_[2] == Catch::Approx(0.0));

  // A flat window's band genuinely has zero width, and nothing here divides by the
  // deviation, so all three lines collapse onto the mean and are reported. That is the
  // deliberate difference from `zscore`, which is undefined on the very same window.
  const math::Vector flat = series({7.0, 7.0, 7.0});
  const auto collapsed = analytics::bollinger(flat, 3);
  REQUIRE(collapsed.middle_[2] == Catch::Approx(7.0));
  REQUIRE(collapsed.upper_[2] == Catch::Approx(7.0));
  REQUIRE(collapsed.lower_[2] == Catch::Approx(7.0));
  REQUIRE_FALSE(observed(analytics::zscore(flat, 3)[2]));
}

TEST_CASE("the relative strength index matches Wilder's published series") {
  // Wilder's own fixture, as reproduced in every reference table for the indicator. The
  // point of testing against a published series rather than a second implementation of
  // our own is that it catches a wrong *convention*, not just a wrong loop.
  const math::Vector closes = series({
    44.34, 44.09, 44.15, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84, 46.08,
    45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41, 46.22, 45.64,
  });
  const math::Vector strength = analytics::rsi(closes, 14);

  // Row 0 has no change, and the smoothing needs fourteen changes before it has a seed.
  for (math::Index row = 0; row < 14; ++row) {
    REQUIRE_FALSE(observed(strength[row]));
  }
  REQUIRE(strength[14] == Catch::Approx(70.46413502109705).epsilon(1e-10));
  REQUIRE(strength[15] == Catch::Approx(66.24961855355505).epsilon(1e-10));
  REQUIRE(strength[16] == Catch::Approx(66.48094183471265).epsilon(1e-10));
  REQUIRE(strength[19] == Catch::Approx(57.91502067008556).epsilon(1e-10));
}

TEST_CASE("the relative strength index decides its degenerate cases instead of dividing") {
  const math::Vector rising = series({1.0, 2.0, 3.0, 4.0});
  REQUIRE(analytics::rsi(rising, 3)[3] == Catch::Approx(100.0));

  const math::Vector falling = series({4.0, 3.0, 2.0, 1.0});
  REQUIRE(analytics::rsi(falling, 3)[3] == Catch::Approx(0.0));

  // Neither strength nor weakness. Any other answer here would read as a signal.
  const math::Vector flat = series({2.0, 2.0, 2.0, 2.0});
  REQUIRE(analytics::rsi(flat, 3)[3] == Catch::Approx(50.0));
}

TEST_CASE("the average true range seeds on a simple mean then smooths Wilder's way") {
  const math::Vector highs = series({10.0, 11.0, 12.5, 11.5, 13.0});
  const math::Vector lows = series({9.0, 9.5, 11.0, 10.0, 11.5});
  const math::Vector closes = series({9.5, 10.5, 12.0, 10.5, 12.5});
  const math::Vector average = analytics::atr(highs, lows, closes, 3);

  REQUIRE_FALSE(observed(average[0]));
  REQUIRE_FALSE(observed(average[1]));
  // The seed is the simple mean of the first three true ranges, 1.0, 1.5 and 2.0.
  REQUIRE(average[2] == Catch::Approx(1.5));
  // Then (1.5 * 2 + 2.0) / 3.
  REQUIRE(average[3] == Catch::Approx(5.0 / 3.0));
  REQUIRE(average[4] == Catch::Approx(1.9444444444444446));
}

TEST_CASE("a trailing covariance is the sample one over paired rows") {
  const math::Vector left = series({1.0, 2.0, 3.0, 4.0});
  const math::Vector right = series({2.0, 4.0, 6.0, 8.0});
  const math::Vector covariance = analytics::rolling_cov(left, right, 3);

  REQUIRE_FALSE(observed(covariance[1]));
  REQUIRE(covariance[3] == Catch::Approx(2.0));
}

TEST_CASE("a trailing correlation is bounded because it uses one sample for all three parts") {
  const math::Vector left = series({1.0, 2.0, 3.0, 4.0});
  const math::Vector right = series({2.0, 1.0, 4.0, 3.0});
  const math::Vector correlation = analytics::rolling_corr(left, right, 3);

  REQUIRE(correlation[3] == Catch::Approx(0.6546536707079771));

  // A series perfectly tracking another correlates at exactly one, and the opposite sign
  // at exactly minus one. These are the values that a mismatched sample would overshoot.
  const math::Vector doubled = series({2.0, 4.0, 6.0, 8.0});
  const math::Vector negated = series({-1.0, -2.0, -3.0, -4.0});
  REQUIRE(analytics::rolling_corr(left, doubled, 3)[3] == 1.0);
  REQUIRE(analytics::rolling_corr(left, negated, 3)[3] == -1.0);
}

TEST_CASE("beta is the covariance over the market's own variance") {
  const math::Vector asset = series({1.0, 2.0, 3.0, 4.0});
  const math::Vector market = series({2.0, 1.0, 4.0, 3.0});

  REQUIRE(analytics::beta(asset, market, 3)[3] == Catch::Approx(0.42857142857142855));

  // A market that did not move gives nothing to attribute the asset's movement to, so
  // beta is unobserved rather than zero or an infinity.
  const math::Vector still = series({1.0, 1.0, 1.0, 1.0});
  REQUIRE_FALSE(observed(analytics::beta(asset, still, 3)[3]));

  // And an asset that is exactly the market has a beta of one, whatever the market did.
  REQUIRE(analytics::beta(market, market, 3)[3] == Catch::Approx(1.0));
}

// ---------------------------------------------------------------------------
// Decompositions, used directly
// ---------------------------------------------------------------------------

namespace {

/** Builds a matrix from a list of columns, so a fixture reads the way a frame is laid out. */
math::Matrix columns(std::initializer_list<std::initializer_list<Scalar>> values) {
  const auto width = static_cast<math::Index>(values.size());
  const auto height = static_cast<math::Index>(values.begin()->size());
  math::Matrix result(height, width);
  math::Index column = 0;
  for (const auto& entries : values) {
    math::Index row = 0;
    for (const Scalar value : entries) {
      result(row++, column) = value;
    }
    ++column;
  }
  return result;
}

} // namespace

TEST_CASE("the caller picks the decomposition, and each reports its own failure") {
  math::Matrix design(4, 2);
  design << 1.0, 1.0, 1.0, 2.0, 1.0, 3.0, 1.0, 4.0;
  const math::Vector target = series({5.0, 8.0, 11.0, 14.0});

  // Cholesky on the normal equations: allowed here only because the matrix is known
  // positive definite, and `info()` is what says so rather than a bool we invented.
  const math::Matrix normal = design.transpose() * design;
  const math::Llt chol(normal);
  REQUIRE(chol.info() == math::SUCCESS);
  const math::Vector via_chol = chol.solve(design.transpose() * target);
  REQUIRE(via_chol[1] == Catch::Approx(3.0));

  // The same answer through a pivoting QR and through an SVD, which is the point of
  // exposing all three: the choice is a trade-off the caller makes, not one we make.
  REQUIRE(math::ColPivQr(design).solve(target)[1] == Catch::Approx(3.0));
  REQUIRE(math::thin_svd(design).solve(target)[1] == Catch::Approx(3.0));

  // Cholesky refuses an indefinite matrix rather than returning plausible numbers.
  math::Matrix indefinite(2, 2);
  indefinite << 1.0, 2.0, 2.0, 1.0;
  REQUIRE(math::Llt(indefinite).info() != math::SUCCESS);
}

TEST_CASE("a rank-deficient design is reported by the decomposition itself") {
  // Two identical columns. The pivoting QR knows the rank; an SVD's singular values say
  // the same thing in a different language. Neither needed a wrapper to tell us.
  math::Matrix design(4, 3);
  design << 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 1.0, 3.0, 3.0, 1.0, 4.0, 4.0;

  REQUIRE(math::ColPivQr(design).rank() == 2);
  REQUIRE(math::FullPivQr(design).rank() == 2);

  const auto svd = math::thin_svd(design);
  REQUIRE(svd.singularValues().size() == 3);
  REQUIRE(svd.singularValues()[2] == Catch::Approx(0.0).margin(1e-12));
}

// ---------------------------------------------------------------------------
// Evaluation metrics
// ---------------------------------------------------------------------------

TEST_CASE("a drawdown over nothing observable reports nothing rather than zero") {
  const math::Vector levels = series({missing(), missing()});
  const auto worst = analytics::max_drawdown(levels);

  REQUIRE_FALSE(observed(worst.magnitude_));
  REQUIRE(worst.peak_row_ == -1);
  REQUIRE(worst.trough_row_ == -1);
}

// ---------------------------------------------------------------------------
// The two kinds of absence, and the vectorized reductions
// ---------------------------------------------------------------------------

TEST_CASE("a statistic that cannot exist says so, distinctly from having no data") {
  // A flat window: the data is there and the z-score does not exist for it.
  const math::Vector flat = series({5.0, 5.0, 5.0, 5.0});
  const math::Vector score = analytics::zscore(flat, 3);
  REQUIRE_FALSE(observed(score[0]));
  REQUIRE_FALSE(math::is_undefined(score[0]));   // warm-up: simply absent
  REQUIRE(math::is_undefined(score[2]));         // computed, and undefined

  // Returns that never varied have no risk-adjusted return; too few returns have no answer yet.
  REQUIRE(math::is_undefined(analytics::sharpe(series({0.01, 0.01, 0.01}))));
  REQUIRE_FALSE(math::is_undefined(analytics::sharpe(series({0.01}))));

  // Nothing fell short of the target, so there is no downside risk to divide by.
  REQUIRE(math::is_undefined(analytics::sortino(series({0.01, 0.02, 0.03}))));

  // Correlation with a constant, and beta against a market that did not move.
  const math::Vector moving = series({1.0, 2.0, 3.0, 4.0});
  const math::Vector still = series({3.0, 3.0, 3.0, 3.0});
  REQUIRE(math::is_undefined(analytics::rolling_corr(moving, still, 3)[3]));
  REQUIRE(math::is_undefined(analytics::beta(moving, still, 3)[3]));

  // An equity curve at zero has not declined by a percentage; it is gone.
  REQUIRE(math::is_undefined(analytics::max_drawdown(series({0.0, 0.0})).magnitude_));
}

TEST_CASE("equal values that are not binary fractions are still a constant window") {
  // The computed mean of {0.1, 0.1, 0.1} is an ulp away from 0.1, so a deviation about it is tiny but
  // not zero. A test for zero alone read it as dispersion: a z-score near -0.8, a Sharpe near 1e15.
  // (`rolling_stdev` and Bollinger report that tiny deviation as it is; nothing divides by it there.)
  const math::Vector tenth = series({0.1, 0.1, 0.1});
  REQUIRE(analytics::rolling_mean(tenth, 3)[2] != 0.1);  // the premise
  CHECK(math::is_undefined(analytics::zscore(tenth, 3)[2]));
  CHECK(math::is_undefined(analytics::sharpe(tenth)));

  // A decimal price level, with a gap in the window: the gap's row must not count as a value.
  const math::Vector level = series({12.34, missing(), 12.34, 12.34});
  REQUIRE(analytics::rolling_mean(level, 4, 2)[3] != 12.34);  // the premise
  CHECK(math::is_undefined(analytics::zscore(level, 4, 2)[3]));

  const math::Vector moving = series({0.01, 0.03, 0.02, 0.05});
  const math::Vector still = series({0.1, 0.1, 0.1, 0.1});
  CHECK(math::is_undefined(analytics::rolling_corr(moving, still, 3)[3]));
  CHECK(math::is_undefined(analytics::rolling_corr(still, moving, 3)[3]));
  CHECK(math::is_undefined(analytics::beta(moving, still, 3)[3]));
}

TEST_CASE("a dispersion whose sample variance underflows is not a constant window") {
  // The squared deviations sum to a positive subnormal, and dividing it by `count - 1` rounds to zero;
  // the values differ, so beta and correlation of the series with itself are still 1.
  const math::Vector tiny = series({-2e-162, 0.0, 0.0, 0.0, 2e-162});
  CHECK(analytics::beta(tiny, tiny, 5)[4] == 1.0);
  CHECK(analytics::rolling_corr(tiny, tiny, 5)[4] == Catch::Approx(1.0));
}

TEST_CASE("a correlation's denominator neither underflows nor overflows where its parts do not") {
  // Each squared sum is finite; their product is not (about 1e600 here). The root of each, multiplied,
  // is. (At the other end, subnormal dispersions lose precision in the sums themselves; only the
  // underflow of the product is avoided, as in the self-correlation above.)
  const math::Vector large = series({-2e150, 1e150, 0.0, 2e150});
  const math::Vector large_other = series({-1e150, 3e150, 0.0, 2e150});
  const math::Vector plain = series({-2.0, 1.0, 0.0, 2.0});
  const math::Vector plain_other = series({-1.0, 3.0, 0.0, 2.0});
  const auto expected = analytics::rolling_corr(plain, plain_other, 4)[3];
  REQUIRE(observed(expected));
  CHECK(analytics::rolling_corr(large, large_other, 4)[3] == Catch::Approx(expected));
  CHECK(analytics::rolling_corr(large, large, 4)[3] == Catch::Approx(1.0));
  // An ordinary self-correlation is exactly 1, even where the root of a squared sum, squared, is not
  // the sum (3 here): the product's root is taken, and the bound holds.
  const math::Vector lone = series({0.0, 0.0, 0.0, 2.0});
  CHECK(analytics::rolling_corr(lone, lone, 4)[3] == 1.0);
}

TEST_CASE("a window of values one ulp apart keeps its dispersion") {
  // Not every rounding-small deviation is a constant window: these values differ, so the statistics
  // exist (however large), and the exact check says so.
  const Scalar one = 1.0;
  const Scalar next = std::nextafter(one, 2.0);
  const math::Vector close = series({one, next, one});
  CHECK(analytics::rolling_stdev(close, 3)[2] > 0.0);
  CHECK(observed(analytics::zscore(close, 3)[2]));
  CHECK(observed(analytics::sharpe(close)));
  CHECK(observed(analytics::beta(series({0.01, 0.03, 0.02}), close, 3)[2]));
  CHECK(observed(analytics::rolling_corr(series({0.01, 0.03, 0.02}), close, 3)[2]));
}

TEST_CASE("the window overloads say what the three-argument form spells out") {
  const math::Vector values = series({1.0, missing(), 3.0, 4.0, 5.0});

  // Two arguments demands every row of the window. There is no zero-means-all sentinel any more.
  for (math::Index row = 0; row < values.size(); ++row) {
    const Scalar strict = analytics::rolling_mean(values, 3)[row];
    const Scalar spelt = analytics::rolling_mean(values, 3, 3)[row];
    REQUIRE(observed(strict) == observed(spelt));
    if (observed(strict)) {
      REQUIRE(strict == spelt);
    }
  }

  // And zero observations is now refused rather than meaning "all".
  REQUIRE_THROWS_AS(analytics::rolling_mean(values, 3, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::rolling_stdev(values, 3, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(analytics::ema(values, 0.5, 0), std::invalid_argument);
}

TEST_CASE("turnover names its convention, and both start policies are available") {
  const math::Matrix weights = columns({
    {1.0, 0.0},
    {0.0, 1.0},
  });

  const math::Vector one_way = analytics::one_way_turnover(weights);
  const math::Vector two_way = analytics::two_way_turnover(weights);

  // Row 1 rotates the whole book: one, one-way; two, two-way. Naming both is what stops a
  // silent factor of two reaching a cost estimate.
  REQUIRE(one_way[1] == Catch::Approx(1.0));
  REQUIRE(two_way[1] == Catch::Approx(2.0));
  for (math::Index row = 0; row < one_way.size(); ++row) {
    REQUIRE(two_way[row] == Catch::Approx(2.0 * one_way[row]));
  }

  // Building the first position from cash counts by default, and can be skipped when comparing
  // strategies whose histories start on different dates.
  REQUIRE(one_way[0] == Catch::Approx(0.5));
  const math::Vector skipped =
    analytics::one_way_turnover(weights, analytics::TurnoverStart::SKIP);
  REQUIRE_FALSE(observed(skipped[0]));
  REQUIRE(skipped[1] == Catch::Approx(1.0));
}
