#include <algotradelib/backtest>
#include <algotradelib/krx>
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

/**
 * States each instrument's whole holding as sellable, as a broker's holdings rows do: the guard checks a
 * sell against the figure by default, in a dry run too.
 */
void state_sellable(strategy::Portfolio<runner::DryTag>& portfolio) {
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  // The instrument's whole holding on each of its rows: rows of one instrument are never summed.
  for (auto& position : portfolio.positions_) {
    if (const auto net = portfolio.net_quantity_of(position.instrument_)) {
      position.sellable_quantity_ = *net;
    }
  }
}

InstrumentId samsung() {
  return InstrumentId{Country::KR, Venue::COMBINED, "005930"};
}
InstrumentId hynix() {
  return InstrumentId{Country::KR, Venue::COMBINED, "000660"};
}

Instrument reference_for(const InstrumentId& id) {
  Instrument reference;
  reference.id_ = id;
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = krx::equity_price_rule();
  return reference;
}

/** A bar `index` days along, closing at `close`. */
strategy::BarEvent bar(std::size_t index, const char* close, const InstrumentId& id = samsung()) {
  Bar value;
  value.timestamp_ =
    std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + 24 * static_cast<int>(index));
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
  state_sellable(portfolio);
  dry.context().set_portfolio(portfolio);

  REQUIRE(dry.feed(bar(1, "71000")).quiet());
  // And a bar for a different instrument is ignored entirely.
  REQUIRE(dry.feed(bar(2, "70000", hynix())).quiet());
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

} // namespace

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

TEST_CASE("the three-span switch counts observed closes, not rows") {
  // 150 rows but 51 closes: the plain seed would still weigh about 13%, so the line is compensated.
  math::Vector closes = math::Vector::Constant(150, 120.0);
  closes[0] = 100.0;
  for (math::Index row = 1; row < 100; ++row) {
    closes[row] = math::missing();
  }
  const math::Index last = closes.size() - 1;
  REQUIRE(vanilla::exponential_line(closes, 50)[last] == Catch::Approx(119.87802735708622).epsilon(1e-12));
  REQUIRE(
    analytics::ema(closes, analytics::alpha_from_span(50), 50)[last] == Catch::Approx(117.2940162004476).epsilon(1e-12)
  );
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

TEST_CASE("a reversion with an entry or exit that is not positive refuses to act") {
  // An exit at or above zero standard deviations below the mean would sell above it, and an entry at or
  // above zero would buy at or above the mean: neither is a reversion, and the documented rule is to
  // trade nothing.
  for (const auto [entry, exit] : {std::pair{1.0, 0.0}, std::pair{1.0, -1.0}, std::pair{0.0, -1.0}}) {
    CAPTURE(entry, exit);
    vanilla::ZScoreReversion reverting;
    reverting.params_.instrument_ = samsung();
    reverting.params_.window_ = 6;
    reverting.params_.entry_ = entry;
    reverting.params_.exit_ = exit;
    reverting.params_.quantity_ = Decimal64::parse("10");

    runner::DryRunner<vanilla::ZScoreReversion> dry(reverting);
    arrange(dry);
    // The same flat run and sharp drop that make a valid reversion buy.
    for (std::size_t at = 0; at < 7; ++at) {
      REQUIRE(dry.feed(bar(at, "70000")).quiet());
    }
    REQUIRE(dry.feed(bar(7, "50000")).would_place_.empty());
  }
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
#endif
