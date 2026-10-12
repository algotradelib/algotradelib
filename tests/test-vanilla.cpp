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

/**
 * States each instrument's whole holding as sellable, as a broker's holdings rows do: the guard checks a
 * sell against the figure by default, in a dry run too.
 */
void state_sellable(strategy::Portfolio<runner::DryTag>& portfolio) {
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING, SellableBasis::RestingTreatment::DEDUCT, false,
  };
  // The instrument's whole holding on each of its rows: rows of one instrument are never summed.
  for (auto& position : portfolio.positions_) {
    if (const auto net = portfolio.net_quantity_of(position.instrument_)) {
      position.sellable_quantity_ = *net;
    }
  }
}

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
  state_sellable(portfolio);
  dry.context().set_portfolio(portfolio);

  REQUIRE(dry.feed(bar(1, "71000")).quiet());
  // And a bar for a different instrument is ignored entirely.
  REQUIRE(dry.feed(bar(2, "70000", hynix())).quiet());
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
  state_sellable(portfolio);
  dry.context().set_portfolio(portfolio);

  const auto after = dry.feed(strategy::MarketPhase::POST_CLOSE);

  // Exactly one order, for the whole net holding.
  REQUIRE(after.refused_.empty());
  REQUIRE(after.would_place_.size() == 1);
  REQUIRE(after.would_place_[0].side_ == OrderSide::SELL);
  REQUIRE(after.would_place_[0].quantity_ == Decimal64::parse("100"));
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
  REQUIRE(analytics::ema(closes, analytics::alpha_from_span(50), 50)[last] ==
          Catch::Approx(117.2940162004476).epsilon(1e-12));
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

namespace {

/** A portfolio holding `held` shares of Samsung, read, so nothing withholds the plan. */
strategy::Portfolio<runner::DryTag> holding(const char* held) {
  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  portfolio.positions_.push_back(position);
  state_sellable(portfolio);
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
  state_sellable(portfolio);
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
  state_sellable(portfolio);
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
  state_sellable(portfolio);
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
  state_sellable(portfolio);
  return portfolio;
}

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
