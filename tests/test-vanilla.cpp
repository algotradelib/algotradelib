#include <algotradelib/backtest>
#include <algotradelib/runner>
#include <algotradelib/vanilla>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;
namespace vanilla = algotradelib::strategy::vanilla;

namespace {

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }
InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

Instrument reference_for(const InstrumentId& id) {
  Instrument reference;
  reference.id_ = id;
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/** A bar `index` days along, closing at `close`. */
strategy::BarEvent bar(std::size_t index, const char* close, const InstrumentId& id = samsung()) {
  Bar value;
  value.timestamp_ = std::chrono::system_clock::time_point{} +
    std::chrono::hours(500000 + 24 * static_cast<int>(index));
  value.high_ = Decimal64::parse(close);
  value.low_ = Decimal64::parse(close);
  value.close_ = Decimal64::parse(close);
  value.volume_ = Decimal64::parse("1000000");
  return strategy::make_bar_event(id, BarInterval::ONE_DAY, BarStamp::CLOSE, value);
}

/** Arranges a runner so only the warm-up can be what withholds an order. */
template <typename R>
void arrange(R& dry, const std::vector<InstrumentId>& universe = {samsung()}) {
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  for (const auto& id : universe) {
    dry.context().set_instrument(reference_for(id));
    dry.context().set_last_price(id, Decimal64::parse("70000"));
  }
  dry.set_portfolio_value(Decimal64::parse("100000000"));
}

/**
 * Feeds `count` bars at a steadily rising close and returns how many produced an order.
 *
 * Rising, so a trend strategy has a reason to act as soon as it is able. A flat series would make
 * "emitted nothing" ambiguous between a warm-up and an absent signal.
 */
template <typename R>
std::size_t first_order_bar(R& dry, std::size_t count) {
  std::size_t first = count;
  for (std::size_t at = 0; at < count; ++at) {
    const auto close = std::to_string(60000 + 200 * at);
    const auto report = dry.feed(bar(at, close.c_str()));
    if (!report.would_place_.empty() && first == count) {
      first = at;
    }
  }
  return first;
}

} // namespace

TEST_CASE("buy and hold targets its quantity and then asks for nothing more") {
  vanilla::BuyAndHold held;
  held.params_.instrument_ = samsung();
  held.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::BuyAndHold> dry(held);
  arrange(dry);

  const auto first = dry.feed(bar(0, "70000"));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE(first.would_place_[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(first.would_place_[0].tag_ == "buy-and-hold");

  // Once the position matches, reconciliation produces nothing -- which is how it "never trades
  // again" while remembering nothing at all.
  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(position);
  dry.context().set_portfolio(portfolio);

  REQUIRE(dry.feed(bar(1, "71000")).quiet());
  // And a bar for a different instrument is ignored entirely.
  REQUIRE(dry.feed(bar(2, "70000", hynix())).quiet());
}

TEST_CASE("timed liquidation flattens when the session leaves the phases it holds through") {
  vanilla::TimedLiquidation flatten;
  runner::DryRunner<vanilla::TimedLiquidation> dry(flatten);
  arrange(dry);

  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(position);
  dry.context().set_portfolio(portfolio);

  // Inside the session it holds.
  REQUIRE(dry.feed(strategy::MarketPhase::OPEN).quiet());

  // After the close it flattens, **under the default limits**, which is the whole reason this
  // vanilla is usable. `permitted_phases_` still defaults to
  // `{OPEN}` and still refuses everything that opens a position outside the session; a sell of the
  // whole holding closes one, so the limit does not stand in front of it. Before that branch this
  // strategy could not place the order it exists to place without the operator switching its own
  // phase limit off.
  const auto after = dry.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(after.refused_.empty());
  REQUIRE(after.would_place_.size() == 1);
  REQUIRE(after.would_place_[0].side_ == OrderSide::SELL);
  REQUIRE(after.would_place_[0].quantity_ == Decimal64::parse("10"));
  // And the runner says it sent under a phase the operator did not permit, because it did.
  REQUIRE(dry.sends_outside_permitted_phase() == 1);

  // Widening the limit reaches the same place by the other road, and **does not** count as a send
  // outside the permitted phases -- the phase was permitted outright, so no exception was used.
  // Without this half the counter could be incremented for any order in a non-`OPEN` phase and the
  // assertion above would still pass.
  strategy::RiskLimits limits;
  limits.permitted_phases_ =
    std::vector<strategy::MarketPhase>{strategy::MarketPhase::OPEN,
                                      strategy::MarketPhase::POST_CLOSE};
  runner::DryRunner<vanilla::TimedLiquidation> permitted(flatten, limits);
  arrange(permitted);
  permitted.context().set_portfolio(portfolio);
  const auto sent = permitted.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(sent.would_place_.size() == 1);
  REQUIRE(sent.would_place_[0].side_ == OrderSide::SELL);
  REQUIRE(sent.would_place_[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(permitted.sends_outside_permitted_phase() == 0);
}

TEST_CASE("timed liquidation flattens a holding the broker reports as two lots") {
  // **This was a real over-sell.** A broker reporting one instrument as
  // two lots is ordinary. This strategy submitted one zero target per *row*, and
  // `reconcile(TargetPosition)` sized each against `Portfolio::quantity_of`, which returns the
  // *first* row -- so rows of 60 and 40 produced two sells of 60, and wherever the phase permitted
  // them both went out: 120 sold against a holding of 100, leaving a short, from the one strategy
  // whose entire purpose is to end the day flat. One target per instrument and a net-of-all-rows
  // reconciliation are both needed; either alone still gets it wrong.
  vanilla::TimedLiquidation flatten;
  runner::DryRunner<vanilla::TimedLiquidation> dry(flatten);
  arrange(dry);

  strategy::Portfolio<runner::DryTag> portfolio;
  for (const char* lot : {"60", "40"}) {
    HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = Decimal64::parse(lot);
    portfolio.positions_.push_back(position);
  }
  dry.context().set_portfolio(portfolio);

  const auto after = dry.feed(strategy::MarketPhase::POST_CLOSE);

  // Exactly one order, for the whole net holding.
  REQUIRE(after.refused_.empty());
  REQUIRE(after.would_place_.size() == 1);
  REQUIRE(after.would_place_[0].side_ == OrderSide::SELL);
  REQUIRE(after.would_place_[0].quantity_ == Decimal64::parse("100"));
}

TEST_CASE("timed liquidation flattens on an uncovered calendar date too") {
  // An uncovered date is precisely when a position should not be carried on an assumption.
  vanilla::TimedLiquidation flatten;
  strategy::RiskLimits limits;
  limits.permitted_phases_ = std::nullopt;
  runner::DryRunner<vanilla::TimedLiquidation> dry(flatten, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(position);
  dry.context().set_portfolio(portfolio);

  REQUIRE(dry.feed(strategy::MarketPhase::UNKNOWN).would_place_.size() == 1);
}

#if defined(ATL_HAS_EIGEN)

TEST_CASE("a moving average crossover emits nothing until its slow window is full") {
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 3;
  params.slow_ = 8;
  params.quantity_ = Decimal64::parse("10");

  vanilla::SmaCrossover crossing(params);
  runner::DryRunner<vanilla::SmaCrossover> dry(crossing);
  arrange(dry);

  const auto first = first_order_bar(dry, 20);
  // The warm-up the strategy states, honoured to the bar: nothing before it, something at it.
  REQUIRE(first == crossing.warmup_bars() - 1);
}

TEST_CASE("an exponential crossover states and honours its own warm-up") {
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 3;
  params.slow_ = 8;
  params.quantity_ = Decimal64::parse("10");

  vanilla::EmaCrossover crossing(params);
  runner::DryRunner<vanilla::EmaCrossover> dry(crossing);
  arrange(dry);

  REQUIRE(first_order_bar(dry, 20) == crossing.warmup_bars() - 1);
}

namespace {

/** A column from plain values, for the line helpers. */
math::Vector column_of(std::initializer_list<math::Scalar> values) {
  math::Vector column(static_cast<math::Index>(values.size()));
  math::Index row = 0;
  for (const auto value : values) {
    column[row++] = value;
  }
  return column;
}

/** `count` closes on a path that rises, falls and rises again, so no two lines coincide. */
math::Vector wandering(std::size_t count) {
  math::Vector column(static_cast<math::Index>(count));
  for (math::Index row = 0; row < column.size(); ++row) {
    column[row] = 60000.0 + 900.0 * std::sin(0.4 * static_cast<double>(row)) + 35.0 * row;
  }
  return column;
}

} // namespace

TEST_CASE("the adjusted average is pandas' adjust=True") {
  // pandas 3.0.6: Series([100, 160, 150, NaN, 155]).ewm(alpha=0.5, adjust=True, ignore_na=True).mean()
  // is 100, 140, 145.71428571428572, (145.71..., carried), 150.66666666666666.
  const math::Vector values = column_of({100.0, 160.0, 150.0, math::missing(), 155.0});
  const math::Vector levels = vanilla::adjusted_ema(values, 0.5);

  REQUIRE(levels[0] == 100.0);
  REQUIRE(levels[1] == Catch::Approx(140.0).epsilon(1e-14));
  REQUIRE(levels[2] == Catch::Approx(145.71428571428572).epsilon(1e-14));
  // pandas reports the carried level on the missing row; the library's convention leaves it unobserved.
  REQUIRE_FALSE(math::observed(levels[3]));
  REQUIRE(levels[4] == Catch::Approx(150.66666666666666).epsilon(1e-14));
}

TEST_CASE("the adjusted average of one observation is it, and alpha 1 keeps each value exactly") {
  REQUIRE(vanilla::adjusted_ema(column_of({123.0}), 0.2)[0] == 123.0);

  const math::Vector values = column_of({0.1, 0.7, 0.30000000000000004});
  const math::Vector unsmoothed = vanilla::adjusted_ema(values, 1.0);
  for (math::Index row = 0; row < values.size(); ++row) {
    REQUIRE(unsmoothed[row] == values[row]);
  }
}

TEST_CASE("the adjusted average stays accurate for a smoothing factor too small to normalize") {
  // Converting a plain-seeded average divides by `1 - (1 - alpha)^k`, which is 0 once `1 - alpha`
  // rounds to 1, and is only a few ulps before that: 164 for 150, then NaN. The running weighted mean
  // becomes the plain mean instead.
  for (const math::Scalar alpha : {1e-16, 1e-20}) {
    const math::Vector levels = vanilla::adjusted_ema(column_of({100.0, 200.0}), alpha);
    REQUIRE(levels[0] == 100.0);
    REQUIRE(levels[1] == Catch::Approx(150.0).epsilon(1e-12));
    REQUIRE(vanilla::adjusted_ema(column_of({100.0}), alpha)[0] == 100.0);
  }
  REQUIRE_THROWS_AS(vanilla::adjusted_ema(column_of({1.0}), 0.0), std::invalid_argument);
  REQUIRE_THROWS_AS(vanilla::adjusted_ema(column_of({1.0}), 0.5, 0), std::invalid_argument);
}

TEST_CASE("a listing day's spike weighs an ordinary bar's weight in a short exponential line") {
  // The new-listing case: no history before the first print, and that print far from the rest. A
  // plain seed keeps 0.8^9 = 13.4% of the line on it after nine more bars; compensated, about 3%.
  const math::Vector closes = column_of({200.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0});
  const math::Scalar alpha = analytics::alpha_from_span(9);
  REQUIRE(alpha == 0.2);
  const math::Index last = closes.size() - 1;

  REQUIRE(analytics::ema(closes, alpha)[last] == Catch::Approx(113.4217728).epsilon(1e-9));
  REQUIRE(vanilla::exponential_line(closes, 9)[last] == Catch::Approx(103.0).epsilon(1e-2));
}

TEST_CASE("an exponential line is the plain average from three spans of rows, compensated below") {
  const std::size_t span = 8;
  const math::Scalar alpha = analytics::alpha_from_span(span);

  // Three spans exactly: the plain average over every row, bit for bit.
  const math::Vector enough = wandering(3 * span);
  const math::Vector plain = analytics::ema(enough, alpha, span);
  const math::Vector line = vanilla::exponential_line(enough, span);
  for (math::Index row = 0; row < enough.size(); ++row) {
    REQUIRE(math::observed(line[row]) == math::observed(plain[row]));
    if (math::observed(plain[row])) {
      REQUIRE(line[row] == plain[row]);
    }
  }

  // One row fewer: compensated, and visibly not the plain average.
  const math::Vector short_of = wandering(3 * span - 1);
  const math::Vector compensated = vanilla::adjusted_ema(short_of, alpha, span);
  const math::Vector short_line = vanilla::exponential_line(short_of, span);
  const math::Index last = short_of.size() - 1;
  REQUIRE(short_line[last] == compensated[last]);
  REQUIRE(short_line[last] != analytics::ema(short_of, alpha, span)[last]);
}

TEST_CASE("an exponential line reads the whole series, not the last slow window") {
  // The defect this replaced: recomputed over the last `slow + 1` closes, the slow line is reseeded
  // every bar and differs from the average over the whole history by `(1 - alpha)^slow` of a raw close.
  const std::size_t slow = 8;
  const math::Vector closes = wandering(60);
  const math::Index last = closes.size() - 1;
  const math::Vector whole = vanilla::exponential_line(closes, slow);
  const math::Vector sliced = analytics::ema(closes.tail(slow + 1), analytics::alpha_from_span(slow), slow);

  REQUIRE(whole[last] == analytics::ema(closes, analytics::alpha_from_span(slow), slow)[last]);
  REQUIRE(whole[last] != Catch::Approx(sliced[sliced.size() - 1]).epsilon(1e-6));
}

namespace {

/** pandas' `ewm(alpha, adjust=True, ignore_na=True)` at the last row, computed directly. */
math::Scalar adjusted_last(const math::Vector& values, math::Scalar alpha) {
  math::Scalar weighted{0};
  math::Scalar total{0};
  math::Scalar weight{1};
  for (math::Index row = values.size() - 1; row >= 0; --row) {
    if (!math::observed(values[row])) {
      continue;
    }
    weighted += weight * values[row];
    total += weight;
    weight *= 1.0 - alpha;
  }
  return weighted / total;
}

} // namespace

TEST_CASE("the adjusted average holds across leading and inner gaps and a warm-up") {
  const math::Scalar nan = math::missing();
  const math::Vector values = column_of({nan, nan, 100.0, 104.0, nan, 98.0, 101.0, nan, 103.0});
  const math::Scalar alpha = 0.3;
  const math::Vector levels = vanilla::adjusted_ema(values, alpha, 3);

  // Unobserved inputs, and rows the warm-up suppressed, stay unobserved.
  for (const math::Index row : {0, 1, 2, 3, 4, 7}) {
    REQUIRE_FALSE(math::observed(levels[row]));
  }
  // Every other row is the adjusted average of the observations up to it -- counting the two the
  // warm-up suppressed.
  for (const math::Index row : {5, 6, 8}) {
    REQUIRE(levels[row] == Catch::Approx(adjusted_last(values.head(row + 1), alpha)).epsilon(1e-13));
  }

  // Nothing to compensate: an empty column and an all-missing one come back as they were.
  const math::Vector empty(0);
  REQUIRE(vanilla::adjusted_ema(empty, alpha).size() == 0);
  REQUIRE(vanilla::exponential_line(empty, 3).size() == 0);
  const math::Vector absent = column_of({nan, nan, nan});
  const math::Vector line = vanilla::exponential_line(absent, 2);
  for (math::Index row = 0; row < absent.size(); ++row) {
    REQUIRE_FALSE(math::observed(line[row]));
  }
}

TEST_CASE("a flat market leaves both exponential lines exactly flat, either side of each switch") {
  // `(S - d * y0) / (1 - d)` gives 99.999999999999986 for fifty-one closes of 100, and a crossover
  // comparing with `<` would buy a flat market. Every row count around the warm-up and the two
  // three-span boundaries (60 for span 20, 150 for span 50).
  for (const std::size_t rows : {51u, 59u, 60u, 61u, 149u, 150u, 151u}) {
    const math::Vector closes = math::Vector::Constant(static_cast<math::Index>(rows), 100.0);
    const math::Index last = closes.size() - 1;
    REQUIRE(vanilla::exponential_line(closes, 20)[last] == 100.0);
    REQUIRE(vanilla::exponential_line(closes, 50)[last] == 100.0);
  }
}

TEST_CASE("the three-span switch counts observed closes, not rows") {
  // 150 rows but 51 closes: the plain seed would still weigh about 13%, so the line is compensated.
  math::Vector closes = math::Vector::Constant(150, 120.0);
  closes[0] = 100.0;
  for (math::Index row = 1; row < 100; ++row) {
    closes[row] = math::missing();
  }
  const math::Index last = closes.size() - 1;
  REQUIRE(vanilla::exponential_line(closes, 50)[last] == Catch::Approx(119.87802735708622).epsilon(1e-12));
  REQUIRE(analytics::ema(closes, analytics::alpha_from_span(50), 50)[last] ==
          Catch::Approx(117.2940162004476).epsilon(1e-12));
}

TEST_CASE("one line switching method can change the signal on an unchanged close") {
  // Documented, not a defect: compensation leans away from the first close, so when the fast line
  // turns plain at three spans and the slow one does not, the two can swap on a close equal to the
  // last. Spans 3/5: long at eight closes, flat at nine.
  const math::Vector eight = column_of({95.0, 100.0, 105.0, 100.0, 100.0, 95.0, 105.0, 100.0});
  const math::Vector nine = column_of({95.0, 100.0, 105.0, 100.0, 100.0, 95.0, 105.0, 100.0, 100.0});
  REQUIRE(vanilla::exponential_line(eight, 5)[7] < vanilla::exponential_line(eight, 3)[7]);
  REQUIRE_FALSE(vanilla::exponential_line(nine, 5)[8] < vanilla::exponential_line(nine, 3)[8]);
}

TEST_CASE("a simple crossover averages the slow window's slice, whose sums the result depends on") {
  // Eigen reduces a window in an order that depends on where it starts in the masked copy, so the
  // same window over a shorter slice can differ in the last bit: here the fast mean over `tail(4)`
  // exceeds the slow one, over `tail(5)` it equals it. The strategy reads `tail(slow + 1)`, as it
  // always has, and stays flat.
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 3;
  params.slow_ = 4;
  params.quantity_ = Decimal64::parse("10");
  vanilla::SmaCrossover crossing(params);
  runner::DryRunner<vanilla::SmaCrossover> dry(crossing);
  arrange(dry);

  const char* closes[] = {"99.29", "99.89", "99.12", "100.58", "99.97"};
  runner::DryRunner<vanilla::SmaCrossover>::Report last;
  for (std::size_t at = 0; at < 5; ++at) {
    last = dry.feed(bar(at, closes[at]));
  }

  // The fixture still shows the hazard with these closes as the cache holds them.
  const auto cached = dry.context().bars(samsung())->column(analytics::BarField::CLOSE);
  const math::Scalar slow = analytics::sma(cached.tail(5), 4)[4];
  const math::Scalar fast_slow_slice = analytics::sma(cached.tail(5), 3)[4];
  const math::Scalar fast_own_slice = analytics::sma(cached.tail(4), 3)[3];
  REQUIRE(fast_slow_slice == slow);
  REQUIRE(slow < fast_own_slice);

  REQUIRE(last.would_place_.empty());
}

TEST_CASE("a crossover whose fast window is not faster refuses to act") {
  // Inverting the signal would trade the opposite of what was configured. Doing nothing is visible
  // in a log; trading backwards is not.
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 8;
  params.slow_ = 3;
  params.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::SmaCrossover> dry(vanilla::SmaCrossover{params});
  arrange(dry);
  REQUIRE(first_order_bar(dry, 20) == 20);
}

TEST_CASE("a crossover goes flat when the fast average falls back below the slow one") {
  // Stated as a level, so the exit needs no memory of having entered.
  vanilla::MovingAverageCrossover::Params params;
  params.instrument_ = samsung();
  params.fast_ = 2;
  params.slow_ = 4;
  params.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::SmaCrossover> dry(vanilla::SmaCrossover{params});
  arrange(dry);

  for (std::size_t at = 0; at < 6; ++at) {
    const auto close = std::to_string(60000 + 500 * at);
    dry.feed(bar(at, close.c_str()));
  }
  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(position);
  dry.context().set_portfolio(portfolio);

  // A sharp fall pulls the fast average under the slow one.
  for (std::size_t at = 6; at < 12; ++at) {
    const auto close = std::to_string(62500 - 1500 * (at - 5));
    dry.feed(bar(at, close.c_str()));
  }
  const auto report = dry.feed(bar(12, "40000"));
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].side_ == OrderSide::SELL);
}

TEST_CASE("a z-score reversion emits nothing until its window is full, then buys a dip") {
  vanilla::ZScoreReversion reverting;
  reverting.params_.instrument_ = samsung();
  reverting.params_.window_ = 6;
  reverting.params_.entry_ = 1.0;
  reverting.params_.exit_ = 0.2;
  reverting.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::ZScoreReversion> dry(reverting);
  arrange(dry);

  // A flat run, so the score is undefined, then a sharp drop that must produce the entry.
  for (std::size_t at = 0; at < 6; ++at) {
    REQUIRE(dry.feed(bar(at, "70000")).quiet());
  }
  // Still nothing: a window with no dispersion has no score, which is a different thing from a
  // score of zero and is why the check is on observability rather than on a value.
  REQUIRE(dry.feed(bar(6, "70000")).quiet());

  const auto dipped = dry.feed(bar(7, "50000"));
  REQUIRE(dipped.would_place_.size() == 1);
  REQUIRE(dipped.would_place_[0].side_ == OrderSide::BUY);
}

TEST_CASE("a reversion asks whether it holds anything, not what its first lot says") {
  // **A fourth first-row reader, found by grepping for them after the third was caught.**
  // A broker listing a spent lot of 0 beside a live one is ordinary, and `quantity_of` returns the
  // first row -- so this strategy read "flat" while holding 10, and on the next dip entered *again*
  // on top of the position it already had. It is a doubling, in a shipped strategy, from a listing
  // the broker is entitled to send.
  vanilla::ZScoreReversion reverting;
  reverting.params_.instrument_ = samsung();
  reverting.params_.window_ = 6;
  reverting.params_.entry_ = 1.0;
  reverting.params_.exit_ = 0.2;
  reverting.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::ZScoreReversion> dry(reverting);
  arrange(dry);

  // **The live lot is 25 and the entry size is 10, deliberately different.** An earlier version held
  // exactly the entry quantity, and reconciliation then cancelled the spurious entry down to no
  // order at all -- so the test passed against the defect and proved nothing. That is the fixture
  // arranging for the defect not to matter, which this repository has now hit three times.
  strategy::Portfolio<runner::DryTag> portfolio;
  for (const char* lot : {"0", "25"}) {
    HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = Decimal64::parse(lot);
    portfolio.positions_.push_back(position);
  }
  dry.context().set_portfolio(portfolio);

  for (std::size_t at = 0; at < 7; ++at) {
    REQUIRE(dry.feed(bar(at, "70000")).quiet());
  }

  // The dip that would have entered. It is already holding 25, and the exit band has not been
  // reached, so the correct answer is to do nothing. Reading the first row instead says "flat",
  // targets the entry size of 10, and reconciliation turns that into a SELL of 15 -- an order that
  // reduces a position the strategy believes it does not have.
  const auto dipped = dry.feed(bar(7, "50000"));
  CHECK(dipped.would_place_.empty());
}

TEST_CASE("a reversion with its exit at or beyond its entry refuses to act") {
  // The band would have no room, so the position would open and close on the same bar forever.
  vanilla::ZScoreReversion reverting;
  reverting.params_.instrument_ = samsung();
  reverting.params_.window_ = 4;
  reverting.params_.entry_ = 1.0;
  reverting.params_.exit_ = 1.0;
  reverting.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::ZScoreReversion> dry(reverting);
  arrange(dry);
  REQUIRE(first_order_bar(dry, 15) == 15);
}

TEST_CASE("an RSI threshold emits nothing before its period has produced an index") {
  vanilla::RsiThreshold indexed;
  indexed.params_.instrument_ = samsung();
  indexed.params_.period_ = 5;
  indexed.params_.oversold_ = 45.0;
  indexed.params_.overbought_ = 70.0;
  indexed.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::RsiThreshold> dry(indexed);
  arrange(dry);

  // A falling series, so the index is low and the entry must fire as soon as it exists.
  std::size_t first = 30;
  for (std::size_t at = 0; at < 30; ++at) {
    const auto close = std::to_string(90000 - 1000 * at);
    const auto report = dry.feed(bar(at, close.c_str()));
    if (!report.would_place_.empty() && first == 30) {
      first = at;
    }
  }
  // Row zero has no price change and the smoothing needs `period_` of them, so the first index
  // value lands on row `period_` -- which is `warmup_bars() - 1`.
  REQUIRE(first == indexed.warmup_bars() - 1);
}

TEST_CASE("an RSI threshold reads Wilder's index over the whole history, not the last period") {
  // Over only the last `period + 1` closes, Wilder's smoothing seeds on the last bar and never steps,
  // which is the simple-average RSI. These closes keep the whole-history index above 32 until the last
  // bar, where it is 23.69 and the strategy buys; over the old slice it would be 36.36, and it would not.
  vanilla::RsiThreshold indexed;
  indexed.params_.instrument_ = samsung();
  indexed.params_.period_ = 3;
  indexed.params_.oversold_ = 30.0;
  indexed.params_.overbought_ = 70.0;
  indexed.params_.quantity_ = Decimal64::parse("10");
  runner::DryRunner<vanilla::RsiThreshold> dry(indexed);
  arrange(dry);

  const char* closes[] = {"100", "106", "101", "95", "94", "95", "94", "98", "97", "91"};
  const std::size_t count = sizeof(closes) / sizeof(closes[0]);
  for (std::size_t at = 0; at < count; ++at) {
    const auto report = dry.feed(bar(at, closes[at]));
    REQUIRE(report.would_place_.empty() == (at + 1 < count));
  }

  // The fixture still separates the two indices, as the cache holds these closes.
  const auto cached = dry.context().bars(samsung())->column(analytics::BarField::CLOSE);
  const math::Vector whole = analytics::rsi(cached, 3);
  const math::Vector sliced = analytics::rsi(cached.tail(4), 3);
  REQUIRE(whole[whole.size() - 1] == Catch::Approx(23.688311688311686).epsilon(1e-12));
  REQUIRE(sliced[sliced.size() - 1] == Catch::Approx(36.36363636363636).epsilon(1e-12));
}

TEST_CASE("a Donchian breakout measures the channel excluding the current bar") {
  // A channel including today could never be broken: today's high is always inside a range that
  // contains it. Getting that wrong produces a strategy that silently does nothing.
  vanilla::DonchianBreakout breaking;
  breaking.params_.instrument_ = samsung();
  breaking.params_.lookback_ = 5;
  breaking.params_.quantity_ = Decimal64::parse("10");

  runner::DryRunner<vanilla::DonchianBreakout> dry(breaking);
  arrange(dry);

  const auto first = first_order_bar(dry, 20);
  REQUIRE(first < 20);
  REQUIRE(first == breaking.warmup_bars() - 1);
}

TEST_CASE("volatility targeting sizes from return deviation, not price deviation") {
  vanilla::VolatilityTargetSizing sizing;
  sizing.params_.instrument_ = samsung();
  sizing.params_.window_ = 5;
  sizing.params_.risk_budget_ = Decimal64::parse("100000");
  sizing.params_.max_quantity_ = Decimal64::parse("1000");

  runner::DryRunner<vanilla::VolatilityTargetSizing> dry(sizing);
  arrange(dry);

  // A flat series has no volatility to budget against, so nothing is sized from it -- and nothing is
  // clamped to the maximum either, which would be the largest position on the least information.
  for (std::size_t at = 0; at < 10; ++at) {
    REQUIRE(dry.feed(bar(at, "70000")).would_place_.empty());
  }

  // A series that moves produces a finite size.
  std::size_t placed = 0;
  Decimal64 quantity;
  for (std::size_t at = 10; at < 25; ++at) {
    const auto close = std::to_string(70000 + ((at % 2 == 0) ? 1400 : -1400));
    const auto report = dry.feed(bar(at, close.c_str()));
    if (!report.would_place_.empty()) {
      ++placed;
      quantity = report.would_place_[0].quantity_;
    }
  }
  REQUIRE(placed > 0);
  REQUIRE(quantity.coefficient() > 0);
  REQUIRE_FALSE(decimal_less(sizing.params_.max_quantity_, quantity));
}

TEST_CASE("equal weight splits the universe exactly and leaves the residual unallocated") {
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};

  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix()});

  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  REQUIRE(report.would_place_.size() == 2);
  // A hundred million at half weight is fifty million, which buys 714 shares at 70,000.
  REQUIRE(report.would_place_[0].quantity_ == Decimal64::parse("714"));
  REQUIRE(report.would_place_[1].quantity_ == Decimal64::parse("714"));
  REQUIRE(report.would_place_[0].tag_ == "equal-weight");

  // It rebalances only at the stated phase, not on every bar: a price move changes every weight, and
  // chasing that costs more turnover than the drift it corrects.
  REQUIRE(dry.feed(strategy::MarketPhase::PRE_OPEN).quiet());
}

TEST_CASE("equal weight over three names truncates rather than overshooting one") {
  vanilla::EqualWeightRebalance weighted;
  const auto third = InstrumentId{Country::KR, Venue::COMBINED, "035720"};
  weighted.params_.universe_ = {samsung(), hynix(), third};

  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix(), third});

  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  REQUIRE(report.would_place_.size() == 3);
  // 0.3333 of a hundred million is 33,330,000, which buys 476 shares. The residual stays
  // unallocated rather than making one arbitrary name larger than the others.
  for (const auto& order : report.would_place_) {
    REQUIRE(order.quantity_ == Decimal64::parse("476"));
  }
}

namespace {

/** A portfolio holding `held` shares of Samsung, read, so nothing withholds the plan. */
strategy::Portfolio<runner::DryTag> holding(const char* held) {
  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  portfolio.positions_.push_back(position);
  return portfolio;
}

} // namespace

TEST_CASE("a weight is held, not bought again at every rebalance", "[weights]") {
  // Half of a hundred million at 70,000 is 714 shares. `SizeKind::TARGET_WEIGHT` says the runner
  // diffs against the position; it used to order 714 again on every rebalance.
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;  // 0.5 each

  const auto placed_against = [&](const char* held) {
    runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
    arrange(dry, {samsung(), hynix()});
    dry.context().set_portfolio(holding(held));
    const auto report = dry.feed(strategy::MarketPhase::OPEN);
    for (const auto& order : report.would_place_) {
      if (order.instrument_ == samsung()) {
        return std::make_pair(order.side_, order.quantity_.str());
      }
    }
    return std::make_pair(OrderSide::BUY, std::string("none"));
  };

  CHECK(placed_against("0") == std::make_pair(OrderSide::BUY, std::string("714")));
  CHECK(placed_against("714").second == "none");
  CHECK(placed_against("300") == std::make_pair(OrderSide::BUY, std::string("414")));
  CHECK(placed_against("1000") == std::make_pair(OrderSide::SELL, std::string("286")));
}

TEST_CASE("a weight with its buy already working places nothing more", "[weights]") {
  // The double-sizing case a target exists to prevent: half of a hundred million at 70,000 is 714
  // shares wanted, and 714 are already working.
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix()});
  auto portfolio = holding("0");
  WorkingOrder<runner::DryTag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.type_ = OrderType::MARKET;
  working.order_.quantity_ = Decimal64::parse("714");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.amendable_quantity_ = Decimal64::parse("714");
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);
  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  for (const auto& order : report.would_place_) {
    CHECK_FALSE(order.instrument_ == samsung());
  }
  CHECK(report.would_cancel_.empty());
}

TEST_CASE("a weight on an instrument with a resting stop is refused, and the stop stays",
          "[weights]") {
  // Target reconciliation withdraws a resting stop when a trade is needed; a weight intent cannot
  // wait for that stop's history the way a target does, so it is refused and nothing is withdrawn.
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix()});
  auto portfolio = holding("100");
  WorkingOrder<runner::DryTag> stop;
  stop.order_.instrument_ = samsung();
  stop.order_.side_ = OrderSide::SELL;
  stop.order_.status_ = OrderStatus::PENDING;
  stop.order_.type_ = OrderType::STOP_LIMIT;
  stop.order_.trigger_price_ = Decimal64::parse("65000");
  stop.order_.price_ = Decimal64::parse("64900");
  stop.order_.quantity_ = Decimal64::parse("100");
  stop.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  stop.amendable_quantity_ = Decimal64::parse("100");
  portfolio.working_.push_back(stop);
  dry.context().set_portfolio(portfolio);
  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  CHECK(report.would_cancel_.empty());
  bool refused = false;
  for (const auto& rejection : report.rejected_) {
    refused = refused || (rejection.instrument_ == samsung() &&
                          rejection.reason_ == strategy::RejectReason::STOP_ORDER_RESTING);
  }
  CHECK(refused);
  // The other name is unaffected.
  REQUIRE(report.would_place_.size() == 1);
  CHECK(report.would_place_[0].instrument_ == hynix());
}

TEST_CASE("a cleared portfolio value refuses a weight rather than sizing on the old one", "[weights]") {
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix()});
  dry.context().set_portfolio(holding("0"));
  dry.clear_portfolio_value();
  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  CHECK(report.would_place_.empty());
  REQUIRE(report.rejected_.size() == 2);
  CHECK(report.rejected_[0].reason_ == strategy::RejectReason::NO_PORTFOLIO_VALUE);
}

TEST_CASE("a stop on a holding already at its weight is left alone, quietly", "[weights]") {
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry, {samsung(), hynix()});
  auto portfolio = holding("714");
  WorkingOrder<runner::DryTag> stop;
  stop.order_.instrument_ = samsung();
  stop.order_.side_ = OrderSide::SELL;
  stop.order_.status_ = OrderStatus::PENDING;
  stop.order_.type_ = OrderType::STOP_LIMIT;
  stop.order_.trigger_price_ = Decimal64::parse("65000");
  stop.order_.price_ = Decimal64::parse("64900");
  stop.order_.quantity_ = Decimal64::parse("714");
  stop.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  stop.amendable_quantity_ = Decimal64::parse("714");
  portfolio.working_.push_back(stop);
  dry.context().set_portfolio(portfolio);
  const auto report = dry.feed(strategy::MarketPhase::OPEN);
  CHECK(report.would_cancel_.empty());
  for (const auto& rejection : report.rejected_) {
    CHECK_FALSE(rejection.instrument_ == samsung());
  }
  for (const auto& order : report.would_place_) {
    CHECK_FALSE(order.instrument_ == samsung());
  }
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a replay sizes a weight against its own equity", "[weights]") {
  // No caller could: `Backtester` keeps its runner private and nothing set the portfolio value, so
  // every weight was refused `no_portfolio_value` and the vanilla never traded in a replay.
  vanilla::EqualWeightRebalance weighted;
  weighted.params_.universe_ = {samsung(), hynix()};
  weighted.params_.weight_scale_ = 1;

  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {reference_for(samsung()), reference_for(hynix())};
  config.limits_.permitted_phases_ = std::nullopt;
  config.calendar_.country_ = Country::KR;
  const auto day0 = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  for (int day = -1; day < 5; ++day) {
    config.calendar_.days_.push_back(TradingDay{
      strategy::SessionWindow::krx().local_date(day0 + std::chrono::hours(24 * day)), true});
  }
  backtest::Backtester<vanilla::EqualWeightRebalance> engine(weighted, config);

  // Two close-stamped bars a session, 08:00 and 10:00 KST (23:00 the day before and 01:00 UTC), so
  // the clock passes from PRE_OPEN to OPEN every day and on_phase fires once a day with both priced.
  // A post-close bar the evening before the first day gives both instruments a price.
  std::vector<std::chrono::system_clock::time_point> stamps{
    day0 - std::chrono::hours(24) + std::chrono::hours(8)};
  for (int day = 0; day < 3; ++day) {
    stamps.push_back(day0 + std::chrono::hours(24 * day) - std::chrono::hours(1));
    stamps.push_back(day0 + std::chrono::hours(24 * day + 1));
  }
  for (const auto& at : stamps) {
    for (const auto& id : {samsung(), hynix()}) {
      Bar value;
      value.timestamp_ = at;
      value.open_ = Decimal64::parse("70000");
      value.high_ = Decimal64::parse("70000");
      value.low_ = Decimal64::parse("70000");
      value.close_ = Decimal64::parse("70000");
      value.volume_ = Decimal64::parse("1000000");
      engine.feed(strategy::make_bar_event(id, BarInterval::ONE_DAY, BarStamp::CLOSE, value));
    }
  }
  engine.finish();

  // Three rebalances, one a day: the first buys each weight, and the next two find it held and place
  // nothing, where a weight reconciled as a trade bought the full allocation again each day.
  const auto& result = engine.result();
  CHECK(result.rejected_.empty());
  REQUIRE(result.executions_.size() == 2);
  for (const auto& execution : result.executions_) {
    CHECK(execution.fill_.side_ == OrderSide::BUY);
    CHECK(execution.fill_.quantity_ == Decimal64::parse("714"));
  }
}
#endif

TEST_CASE("an empty universe asks for nothing rather than dividing by zero") {
  vanilla::EqualWeightRebalance weighted;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry);
  REQUIRE(dry.feed(strategy::MarketPhase::OPEN).quiet());
}

#endif

namespace {

/** An instant `hours` after the listings below were first read. */
std::chrono::system_clock::time_point at_hour(int hours) {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + hours);
}

/**
 * A holding of `held`, read at `holdings_at`, with the protective stop's row in the given state or
 * absent.
 */
strategy::Portfolio<runner::DryTag> stop_listing(
  std::optional<OrderStatus> stop_status,
  int holdings_at = 0,
  const char* held = "10"
) {
  strategy::Portfolio<runner::DryTag> portfolio;
  // A listing that was read: a plan that withdraws something is withheld against an account nobody
  // has read, which is the runner's own rule and not what these tests are about.
  portfolio.as_of_ = at_hour(holdings_at);
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  portfolio.positions_.push_back(position);
  if (stop_status) {
    WorkingOrder<runner::DryTag> stop;
    stop.ref_ = OrderRef<runner::DryTag>{"STOP-1", ""};
    stop.order_.order_id_ = "STOP-1";
    stop.order_.instrument_ = samsung();
    stop.order_.side_ = OrderSide::SELL;
    stop.order_.type_ = OrderType::STOP_LIMIT;
    stop.order_.status_ = *stop_status;
    stop.order_.quantity_ = Decimal64::parse("10");
    stop.order_.price_ = Decimal64::parse("69000");
    stop.order_.trigger_price_ = Decimal64::parse("69500");
    stop.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    portfolio.working_.push_back(stop);
  }
  return portfolio;
}

/** Forwards `on_phase` to a liquidation and nothing else, as a composing strategy does. */
struct OnlyPhaseLiquidation {
  vanilla::TimedLiquidation liquidation_;

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    liquidation_.on_phase(ctx, phase);
  }
};

/** The broker's history row for the stop, in `status` with `filled` executed. */
OrderSnapshot stop_record(OrderStatus status, const char* filled) {
  OrderSnapshot row;
  row.order_id_ = "STOP-1";
  row.instrument_ = samsung();
  row.side_ = OrderSide::SELL;
  row.type_ = OrderType::STOP_LIMIT;
  row.status_ = status;
  row.quantity_ = Decimal64::parse("10");
  row.execution_.filled_quantity_ = Decimal64::parse(filled);
  return row;
}

/** The phase exit: the stop is withdrawn, nothing is sold, and the flatten is kept. */
template <typename Strategy>
void exits_behind_the_stop(runner::DryRunner<Strategy>& dry) {
  arrange(dry);
  dry.context().set_portfolio(stop_listing(OrderStatus::PENDING));
  const auto exit = dry.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(exit.would_cancel_.size() == 1);
  CHECK(exit.would_cancel_[0].primary_ == "STOP-1");
  CHECK(exit.would_place_.empty());
  REQUIRE(exit.rejected_.size() == 1);
  CHECK(exit.rejected_[0].reason_ == strategy::RejectReason::STOP_ORDER_RESTING);
  REQUIRE(dry.deferred_targets().size() == 1);
  CHECK(dry.targets_deferred() == 1);

  // The listing reports the withdrawal pending: nothing asks for history, nothing is sent, nothing is
  // re-reported, and the target is still kept -- even with a record that would otherwise release it.
  dry.context().set_portfolio(stop_listing(OrderStatus::PENDING_CANCEL));
  CHECK_FALSE(dry.deferred_awaiting_evidence());
  const auto pending = dry.retry_deferred({stop_record(OrderStatus::CANCELED, "0")});
  CHECK(pending.would_cancel_.empty());
  CHECK(pending.would_place_.empty());
  CHECK(pending.rejected_.empty());
  CHECK(dry.deferred_targets().size() == 1);

  // The listing no longer reports the stop, but the broker's record of it is not terminal yet, or not
  // there at all: a stale answer waits.
  dry.context().set_portfolio(stop_listing(std::nullopt));
  CHECK(dry.deferred_awaiting_evidence());
  CHECK(dry.retry_deferred({stop_record(OrderStatus::PENDING, "0")}).would_place_.empty());
  CHECK(dry.retry_deferred({}).would_place_.empty());
  CHECK(dry.deferred_targets().size() == 1);
}

/** The cancelled case: the stop's own record is terminal with nothing filled, so the holding sells. */
template <typename Strategy>
void flattens_after_the_stop_is_gone(runner::DryRunner<Strategy>& dry) {
  exits_behind_the_stop(dry);
  // No bar, no phase change, no holdings read, no strategy callback.
  const auto flat = dry.retry_deferred({stop_record(OrderStatus::CANCELED, "0")});
  REQUIRE(flat.would_place_.size() == 1);
  CHECK(flat.would_place_[0].side_ == OrderSide::SELL);
  CHECK(flat.would_place_[0].quantity_ == Decimal64::parse("10"));
  CHECK(dry.deferred_targets().empty());
  CHECK(dry.deferred_dropped_after_fill() == 0);
}

} // namespace

TEST_CASE("timed liquidation completes after a resting stop is withdrawn, with no bar after", "[trigger]") {
  // The cancellation is confirmed after the session's last bar; nothing
  // out of phase arrives again, so a flatten the strategy has to restate is never restated. The runner
  // keeps it instead.
  runner::DryRunner<vanilla::TimedLiquidation> dry(vanilla::TimedLiquidation{});
  flattens_after_the_stop_is_gone(dry);
}

TEST_CASE("a composing strategy that forwards only on_phase is not relied on to retry", "[trigger]") {
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  flattens_after_the_stop_is_gone(dry);
}

TEST_CASE("a stop that filled while it was withdrawn is not sold again", "[trigger]") {
  // **Answered by the stop's own record.** The listing drops a filled
  // stop exactly as it drops a cancelled one, and holdings are no proof either way; the broker's row
  // for the stop is. Anything filled -- even with a stale holding still showing ten -- sends nothing.
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  exits_behind_the_stop(dry);
  const auto after = dry.retry_deferred({stop_record(OrderStatus::FILLED, "10")});
  CHECK(after.would_place_.empty());
  CHECK(dry.deferred_targets().empty());
  CHECK(dry.deferred_dropped_after_fill() == 1);
}

TEST_CASE("a stop cancelled after a partial fill leaves the remainder to the strategy", "[trigger]") {
  // KIS reports such an order `CANCELED`; the fill is read from the quantity, never the status. The
  // runner cannot know the remainder without a holding it can trust, so it stops, counted.
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  exits_behind_the_stop(dry);
  const auto after = dry.retry_deferred({stop_record(OrderStatus::CANCELED, "4")});
  CHECK(after.would_place_.empty());
  CHECK(dry.deferred_targets().empty());
  CHECK(dry.deferred_dropped_after_fill() == 1);
}

TEST_CASE("a deferred target gives way to what the strategy says next", "[trigger]") {
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  arrange(dry);
  dry.context().set_portfolio(stop_listing(OrderStatus::PENDING));
  dry.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(dry.deferred_targets().size() == 1);

  // The strategy changes its mind about the instrument in an ordinary pass: hold the ten. The fallback
  // for its silence is dropped, so the stop's cancellation sells nothing.
  strategy::TargetPosition hold;
  hold.instrument_ = samsung();
  hold.target_quantity_ = Decimal64::parse("10");
  dry.context().submit(hold);
  dry.settle_closing();
  CHECK(dry.deferred_targets().empty());
  dry.context().set_portfolio(stop_listing(std::nullopt));
  const auto after = dry.retry_deferred({stop_record(OrderStatus::CANCELED, "0")});
  CHECK(after.would_place_.empty());
}

TEST_CASE("an ordinary pass never releases a deferred target", "[trigger]") {
  // Each order update in a listing is its own pass; releasing
  // there sent the fallback before a later update in the same listing let the strategy speak.
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  exits_behind_the_stop(dry);
  OrderSnapshot unrelated;
  unrelated.order_id_ = "OTHER";
  unrelated.instrument_ = samsung();
  const auto update = dry.feed(unrelated);
  CHECK(update.would_place_.empty());
  CHECK(dry.deferred_targets().size() == 1);
}

TEST_CASE("a flatten withheld whole defers nothing", "[trigger]") {
  // Its withdrawal never went out, so there is no stop leaving to wait for; it is reported as
  // withheld, which is the bucket that already says so.
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  arrange(dry);
  auto unread = stop_listing(OrderStatus::PENDING);
  unread.as_of_ = {};
  dry.context().set_portfolio(unread);
  const auto exit = dry.feed(strategy::MarketPhase::POST_CLOSE);
  CHECK(exit.withheld_plans_.size() == 1);
  CHECK(dry.deferred_targets().empty());
}

TEST_CASE("one snapshot of listing and holdings releases a deferred target at once", "[trigger]") {
  // A replay's ledger is the broker: the stop it withdrew is gone and the holding after it is known
  // together, so there is no later holdings read to wait for.
  runner::DryRunner<OnlyPhaseLiquidation> dry(OnlyPhaseLiquidation{});
  arrange(dry);
  dry.context().set_portfolio(stop_listing(OrderStatus::PENDING));
  dry.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(dry.deferred_targets().size() == 1);

  // Still listed: nothing.
  CHECK(dry.retry_deferred_on_one_snapshot().would_place_.empty());
  dry.context().set_portfolio(stop_listing(std::nullopt));
  const auto flat = dry.retry_deferred_on_one_snapshot();
  REQUIRE(flat.would_place_.size() == 1);
  CHECK(flat.would_place_[0].side_ == OrderSide::SELL);
  CHECK(dry.deferred_targets().empty());
}
