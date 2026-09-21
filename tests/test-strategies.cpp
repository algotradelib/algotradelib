#include <algotradelib/runner>
#include <algotradelib/strategies>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
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

  // After the close it flattens -- but the guard's default only permits trading while open, so the
  // order is refused rather than sent. The strategy did its job; the operator's limit had the last
  // word, which is the correct division.
  const auto after = dry.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(after.refused_.size() == 1);
  REQUIRE(after.refused_[0].reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED);

  // Permitting the post-close phase lets the flattening through.
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

TEST_CASE("an empty universe asks for nothing rather than dividing by zero") {
  vanilla::EqualWeightRebalance weighted;
  runner::DryRunner<vanilla::EqualWeightRebalance> dry(weighted);
  arrange(dry);
  REQUIRE(dry.feed(strategy::MarketPhase::OPEN).quiet());
}

#endif
