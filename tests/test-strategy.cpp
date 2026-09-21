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

  // An exact decimal at the scale the caller chose, so a third is 0.3333 or 0.333333 by their
  // decision rather than by a double's.
  const auto weight = SizeSpec::target_weight(Decimal64::parse("0.25"));
  REQUIRE(weight.kind() == SizeKind::TARGET_WEIGHT);
  REQUIRE(weight.as_target_weight() == Decimal64::parse("0.25"));
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

// ---------------------------------------------------------------------------
// Market phase
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::system_clock;

/** Builds a UTC instant, so a test states the moment it means without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  // Days since the epoch by proleptic Gregorian arithmetic, valid for the dates used here.
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  const long long epoch_days = era_days - 719468;
  return Clock::time_point{} + std::chrono::hours(epoch_days * 24 + hour) +
    std::chrono::minutes(minute);
}

} // namespace

TEST_CASE("an uncovered date is unknown, never assumed closed or open") {
  // The same distinction TradingCalendar draws. Assuming closed stops a strategy on a day that
  // is in fact open; assuming open lets it order into a holiday.
  const auto session = strategy::SessionWindow::krx();
  REQUIRE(session.phase(utc(2026, 3, 10, 2, 0), std::nullopt) == strategy::MarketPhase::UNKNOWN);
  REQUIRE(session.phase(utc(2026, 3, 10, 2, 0), false) == strategy::MarketPhase::CLOSED);
}

TEST_CASE("the KRX session boundaries fall where the exchange publishes them") {
  const auto session = strategy::SessionWindow::krx();
  // KST is UTC+9, so 09:00 KST is 00:00 UTC and 15:30 KST is 06:30 UTC.
  REQUIRE(session.phase(utc(2026, 3, 10, 23, 59), true) == strategy::MarketPhase::PRE_OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 0, 0), true) == strategy::MarketPhase::OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 3, 0), true) == strategy::MarketPhase::OPEN);
  REQUIRE(session.phase(utc(2026, 3, 10, 6, 29), true) == strategy::MarketPhase::OPEN);
  // The close is exclusive: at exactly 15:30 KST the session is over.
  REQUIRE(session.phase(utc(2026, 3, 10, 6, 30), true) == strategy::MarketPhase::POST_CLOSE);
  REQUIRE(session.phase(utc(2026, 3, 10, 14, 0), true) == strategy::MarketPhase::POST_CLOSE);
}

TEST_CASE("a phase name is stable for every enumerator") {
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::UNKNOWN) == "unknown");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::CLOSED) == "closed");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::PRE_OPEN) == "pre_open");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::OPEN) == "open");
  REQUIRE(strategy::market_phase_name(strategy::MarketPhase::POST_CLOSE) == "post_close");
}

// ---------------------------------------------------------------------------
// Bar events
// ---------------------------------------------------------------------------

TEST_CASE("a bar event states when its period ended, whichever edge it is stamped on") {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 0, 0);
  bar.high_ = Decimal64::parse("70500");
  bar.low_ = Decimal64::parse("69400");
  bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1200000");

  // A close-stamped bar already carries the end of its period.
  const auto closed = strategy::make_bar_event(
    samsung(), BarInterval::ONE_MINUTE, BarStamp::CLOSE, bar
  );
  REQUIRE(closed.period_end_ == bar.timestamp_);

  // An open-stamped bar ends one interval later. Getting this wrong is the off-by-one-bar
  // lookahead that BarStamp exists to prevent, which is why it is computed once here.
  const auto opened = strategy::make_bar_event(
    samsung(), BarInterval::ONE_MINUTE, BarStamp::OPEN, bar
  );
  REQUIRE(opened.period_end_ == bar.timestamp_ + std::chrono::minutes(1));

  const auto daily = strategy::make_bar_event(
    samsung(), BarInterval::ONE_DAY, BarStamp::OPEN, bar
  );
  REQUIRE(daily.period_end_ == bar.timestamp_ + std::chrono::hours(24));
  REQUIRE(daily.interval_ == BarInterval::ONE_DAY);
  REQUIRE(daily.stamp_ == BarStamp::OPEN);
  REQUIRE(daily.instrument_.symbol_ == "005930");
}

TEST_CASE("a fill reports one execution, not an order's running total") {
  // A partial fill arrives as one event per execution. Summing them is the strategy's business;
  // the running total lives in the portfolio.
  strategy::Fill fill;
  fill.order_id_ = "0001569139";
  fill.instrument_ = samsung();
  fill.side_ = OrderSide::BUY;
  fill.quantity_ = Decimal64::parse("40");
  fill.price_ = Decimal64::parse("70000");
  fill.tag_ = "sma-crossover";

  REQUIRE(fill.quantity_ == Decimal64::parse("40"));
  REQUIRE(fill.tag_ == "sma-crossover");
  // Broker-neutral: an order id as text, so on_fill's signature names no broker.
  REQUIRE(fill.order_id_ == "0001569139");
}

// ---------------------------------------------------------------------------
// The strategy surface, and what its conformance check catches
// ---------------------------------------------------------------------------

namespace {

using strategy::FullContext;
using strategy::MinimalContext;

/** A portable strategy: one templated handler, no broker named anywhere. */
struct BarOnly {
  std::size_t bars_seen_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_seen_;
    ctx.log().note("bar");
    (void)bar;
  }
};

/** A strategy using every handler, to prove the detection covers all of them. */
struct EveryHandler {
  template <class Ctx> void on_start(Ctx&) {}
  template <class Ctx> void on_stop(Ctx&) {}
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_quote(Ctx&, const strategy::QuoteEvent&) {}
  template <class Ctx> void on_fill(Ctx&, const strategy::Fill&) {}
  template <class Ctx> void on_order_update(Ctx&, const OrderSnapshot&) {}
  template <class Ctx> void on_phase(Ctx&, strategy::MarketPhase) {}
};

/** Not a strategy: no handler the runner can call. */
struct NoHandlers {
  void step() {}
};

/** Not portable: the handler is bound to one concrete context type. */
struct BoundToOneContext {
  void on_bar(FullContext&, const strategy::BarEvent&) {}
};

/** A handler whose event parameter is the wrong type, so it is not detected. */
struct WrongEventType {
  template <class Ctx> void on_bar(Ctx&, const strategy::QuoteEvent&) {}
};

/** A strategy that reads a capability, to prove both branches type-check. */
struct CapabilityGated {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    auto intent = strategy::OrderIntent::market(
      bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
    );
    if constexpr (Ctx::caps::has_client_order_id) {
      ctx.submit(intent.with_client_order_id("idem-1"));
    } else {
      ctx.submit(intent.with_tag("no-key"));
    }
  }
};

} // namespace

// The assertion itself, at namespace scope, which is where a runner would put it.
ATL_ASSERT_STRATEGY(BarOnly, MinimalContext);
ATL_ASSERT_STRATEGY(BarOnly, FullContext);
ATL_ASSERT_STRATEGY(EveryHandler, MinimalContext);
ATL_ASSERT_STRATEGY(CapabilityGated, MinimalContext);
ATL_ASSERT_STRATEGY(CapabilityGated, FullContext);

TEST_CASE("a templated handler is detected for every context") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::has_on_bar<BarOnly, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_bar<BarOnly, FullContext>::value);
  // And the handlers it does not define are absent, which is what makes them optional.
  STATIC_REQUIRE_FALSE(detail::has_on_quote<BarOnly, MinimalContext>::value);
  STATIC_REQUIRE_FALSE(detail::has_on_start<BarOnly, MinimalContext>::value);
}

TEST_CASE("every handler in the surface is detectable") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::has_on_start<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_stop<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_bar<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_quote<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_fill<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_order_update<EveryHandler, MinimalContext>::value);
  STATIC_REQUIRE(detail::has_on_phase<EveryHandler, MinimalContext>::value);
}

TEST_CASE("a type with no handler is rejected") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE_FALSE(detail::has_any_handler<NoHandlers, MinimalContext>);
  // Which is what ATL_ASSERT_STRATEGY reports on, naming the handlers it looked for.
}

TEST_CASE("a handler bound to one concrete context fails the genericity test") {
  // The failure this check exists for. It compiles for the runner it was written against and
  // silently excludes every other, so the strategy could not be dry-run or backtested without
  // being rewritten -- and nothing would say so until someone tried.
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::has_on_bar<BoundToOneContext, FullContext>::value);
  STATIC_REQUIRE_FALSE(detail::has_on_bar<BoundToOneContext, MinimalContext>::value);
  STATIC_REQUIRE_FALSE(
    detail::handlers_are_generic<BoundToOneContext, FullContext, MinimalContext>
  );
  // A properly templated one passes against both probes.
  STATIC_REQUIRE(detail::handlers_are_generic<BarOnly, FullContext, MinimalContext>);
  STATIC_REQUIRE(detail::handlers_are_generic<BarOnly, MinimalContext, FullContext>);
}

TEST_CASE("a handler with the wrong event type is not detected as that handler") {
  // An honest limitation, worth pinning: detection matches a signature, so a misspelled
  // parameter type reads as an absent handler rather than as a wrong one. The assertion message
  // therefore names the expected signatures instead of claiming the handler is missing.
  namespace detail = strategy::strategy_detail;
  // `on_bar` taking a QuoteEvent is neither: not `on_bar`, because the event type is wrong, and
  // not `on_quote`, because the name is. So the strategy reads as having no handler at all,
  // which is exactly what the assertion reports -- and why its message lists the expected
  // signatures rather than claiming a handler is absent.
  STATIC_REQUIRE_FALSE(detail::has_on_bar<WrongEventType, MinimalContext>::value);
  STATIC_REQUIRE_FALSE(detail::has_on_quote<WrongEventType, MinimalContext>::value);
  STATIC_REQUIRE_FALSE(detail::has_any_handler<WrongEventType, MinimalContext>);
}

TEST_CASE("a probe context records what a strategy asked for") {
  MinimalContext ctx;
  BarOnly strategy_instance;

  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 0, 0);
  bar.high_ = Decimal64::parse("70500");
  bar.low_ = Decimal64::parse("69400");
  bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1200000");
  const auto event =
    strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);

  strategy_instance.on_bar(ctx, event);
  strategy_instance.on_bar(ctx, event);

  REQUIRE(strategy_instance.bars_seen_ == 2);
  REQUIRE(ctx.log().lines().size() == 2);
  REQUIRE(ctx.log().lines()[0] == "bar");
  REQUIRE(ctx.submitted().empty());
}

TEST_CASE("both branches of a capability gate are exercised by the two probes") {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 0, 0);
  bar.high_ = Decimal64::parse("70500");
  bar.low_ = Decimal64::parse("69400");
  bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1200000");
  const auto event =
    strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);

  // A broker with no idempotency key: the strategy takes the other path.
  MinimalContext without;
  CapabilityGated gated;
  gated.on_bar(without, event);
  REQUIRE(without.submitted().size() == 1);
  REQUIRE_FALSE(without.submitted()[0].client_order_id());
  REQUIRE(without.submitted()[0].tag() == "no-key");

  // A broker that has one: the same source takes the key path.
  FullContext with;
  gated.on_bar(with, event);
  REQUIRE(with.submitted().size() == 1);
  REQUIRE(with.submitted()[0].client_order_id() == std::optional<std::string>("idem-1"));
}

// ---------------------------------------------------------------------------
// Portfolio: the arithmetic that prevents double-sizing
// ---------------------------------------------------------------------------

namespace {

using ProbePortfolio = strategy::Portfolio<strategy::ProbeBroker>;

/** Builds a working order with an ordered and a filled quantity. */
WorkingOrder<strategy::ProbeBroker> working(
  const char* ordered,
  const char* filled,
  OrderStatus status,
  OrderSide side = OrderSide::BUY
) {
  WorkingOrder<strategy::ProbeBroker> order;
  order.order_.instrument_ = samsung();
  order.order_.side_ = side;
  order.order_.status_ = status;
  order.order_.quantity_ = Decimal64::parse(ordered);
  order.order_.execution_.filled_quantity_ = Decimal64::parse(filled);
  order.ref_ = OrderRef<strategy::ProbeBroker>{"0001569139", ""};
  return order;
}

} // namespace

TEST_CASE("working quantity counts what is still in the market, not what was ordered") {
  // The partial-fill case that causes double-sizing: 100 ordered, 40 filled, so 60 is still
  // working. A strategy sizing against the position alone would ask for 60 more on top.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "40", OrderStatus::PARTIAL_FILLED));

  REQUIRE(portfolio.working_quantity(samsung(), OrderSide::BUY) == Decimal64::parse("60"));
  // The other side has nothing working.
  REQUIRE(portfolio.working_quantity(samsung(), OrderSide::SELL) == Decimal64::parse("0"));
}

TEST_CASE("a terminal order contributes no working quantity") {
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "100", OrderStatus::FILLED));
  portfolio.working_.push_back(working("50", "0", OrderStatus::CANCELED));
  portfolio.working_.push_back(working("30", "0", OrderStatus::REJECTED));

  REQUIRE(portfolio.working_quantity(samsung(), OrderSide::BUY) == Decimal64::parse("0"));
}

TEST_CASE("an order in an unrecognized state still counts as working") {
  // Under-counting working quantity is what causes double-sizing, so an order whose status this
  // library does not recognize is assumed live. Over-counting merely holds back an order.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::UNKNOWN));

  REQUIRE(portfolio.working_quantity(samsung(), OrderSide::BUY) == Decimal64::parse("100"));
}

TEST_CASE("a pending cancel or replace is still working until it is confirmed") {
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING_CANCEL));
  portfolio.working_.push_back(working("40", "0", OrderStatus::PENDING_REPLACE));

  REQUIRE(portfolio.working_quantity(samsung(), OrderSide::BUY) == Decimal64::parse("140"));
}

TEST_CASE("an absent position is nothing, not a position of zero") {
  ProbePortfolio portfolio;
  REQUIRE_FALSE(portfolio.quantity_of(samsung()));

  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("0");
  portfolio.positions_.push_back(held);
  // Now there *is* a position, and it happens to be flat. A broker omitting a closed position
  // and a broker reporting a flat one are making different claims.
  REQUIRE(portfolio.quantity_of(samsung()));
  REQUIRE(portfolio.quantity_of(samsung())->coefficient() == 0);
}

TEST_CASE("cash is per currency with no implicit conversion") {
  ProbePortfolio portfolio;
  portfolio.cash_.push_back(Money{Decimal64::parse("1000000"), Currency{"KRW"}});

  REQUIRE(portfolio.cash_in(Currency{"KRW"}) == std::optional<Decimal64>(Decimal64::parse("1000000")));
  REQUIRE_FALSE(portfolio.cash_in(Currency{"USD"}));
}

TEST_CASE("a working order is found by the reference that addresses it") {
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING));

  const OrderRef<strategy::ProbeBroker> present{"0001569139", ""};
  const OrderRef<strategy::ProbeBroker> absent{"0009999999", ""};
  REQUIRE(portfolio.find_working(present) != nullptr);
  REQUIRE(portfolio.find_working(absent) == nullptr);
  REQUIRE(present.str() == "probe:0001569139");
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

namespace {

using strategy::Plan;
using strategy::PlannedOrder;
using strategy::ReconcileInputs;
using strategy::RejectReason;
using strategy::TargetPosition;

/** Reference data for a Korean share: the KRX ladder and a lot of one. */
Instrument samsung_reference(const char* lot = "1") {
  Instrument reference;
  reference.id_ = samsung();
  reference.name_ = "Samsung Electronics";
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse(lot);
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/** A held position of `quantity` shares. */
HoldingPosition holding(const char* quantity) {
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(quantity);
  return position;
}

TargetPosition target(const char* quantity, std::optional<strategy::Price> limit = std::nullopt) {
  TargetPosition wanted;
  wanted.instrument_ = samsung();
  wanted.target_quantity_ = Decimal64::parse(quantity);
  wanted.limit_ = std::move(limit);
  wanted.tag_ = "test";
  return wanted;
}

} // namespace

TEST_CASE("a target already met produces no instruction at all") {
  // The common case, and it must cost nothing: a signal that has not changed asks for nothing.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.empty());
}

TEST_CASE("a target from flat becomes one order for the whole amount") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.cancel_.empty());
  REQUIRE(plan.place_[0].side_ == OrderSide::BUY);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("100"));
  // No limit on the target, so a market order -- the target did not name a price.
  REQUIRE(plan.place_[0].type_ == OrderType::MARKET);
  REQUIRE_FALSE(plan.place_[0].price_);
  REQUIRE(plan.place_[0].tag_ == "test");
}

TEST_CASE("a partial fill does not produce a second full-size order") {
  // **The case this whole engine exists for.** The signal still says hold 100. 40 are filled and
  // 60 are working. Sizing against the position alone would order 60 more on top of the 60
  // already in the market and end up holding 160.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("40"));
  portfolio.working_.push_back(working("100", "40", OrderStatus::PARTIAL_FILLED));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  // Held 40, working 60, projected 100 -- the target is already reached in flight.
  REQUIRE(plan.empty());
}

TEST_CASE("only the shortfall beyond what is working is sent") {
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("40"));
  portfolio.working_.push_back(working("100", "40", OrderStatus::PARTIAL_FILLED));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  // Now the target rises to 150: held 40, working 60, so 50 remains.
  const auto plan = strategy::reconcile(target("150"), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("50"));
  REQUIRE(plan.cancel_.empty());
}

TEST_CASE("an order working against the target is withdrawn before anything is sent") {
  // Buying toward a target while a sell rests on the same instrument works against itself.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("30", "0", OrderStatus::PENDING, OrderSide::SELL));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.cancel_.size() == 1);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("100"));
  REQUIRE(plan.place_[0].side_ == OrderSide::BUY);
}

TEST_CASE("a target of exactly what is held withdraws every working order") {
  // Nothing is needed, so anything still in the market would move the position off target.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  portfolio.working_.push_back(working("20", "0", OrderStatus::PENDING));
  portfolio.working_.push_back(working("30", "0", OrderStatus::PENDING, OrderSide::SELL));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.cancel_.size() == 2);
  REQUIRE(plan.place_.empty());
}

TEST_CASE("over-ordering withdraws the side and sends the remainder fresh") {
  // 150 working against a target of 100 from flat. Partial cancellation is a capability only one
  // broker has, so the side is withdrawn and the whole remainder re-sent, which is correct on both.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("150", "0", OrderStatus::PENDING));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.cancel_.size() == 1);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("100"));
}

TEST_CASE("reducing a position sells the difference") {
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("40"), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].side_ == OrderSide::SELL);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("60"));
}

TEST_CASE("a target with a limit price carries it onto the tick grid") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto limit = strategy::Price::checked(Decimal64::parse("70000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("10", limit), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].type_ == OrderType::LIMIT);
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("70000")));
  // Already on the grid, so nothing was adjusted -- and the flag says so.
  REQUIRE_FALSE(plan.place_[0].price_rounded_);
  REQUIRE_FALSE(plan.place_[0].price_clamped_);
}

TEST_CASE("a quantity is rounded down onto the lot, and the adjustment is recorded") {
  // Down, always: rounding up would trade more than the strategy sized and lose more than it
  // budgeted, while rounding down is a shortfall the next reconciliation sees.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference("10");
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("47"), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("40"));
  REQUIRE(plan.place_[0].quantity_rounded_);
}

TEST_CASE("a quantity that rounds away entirely is rejected, not silently dropped") {
  // The strategy asked for something and is getting nothing. An empty plan here would be
  // indistinguishable from a met target.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference("100");
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("47"), portfolio, inputs);
  REQUIRE(plan.place_.empty());
  REQUIRE(plan.rejected_.size() == 1);
  REQUIRE(plan.rejected_[0].reason_ == RejectReason::QUANTITY_ROUNDS_TO_ZERO);
  REQUIRE(plan.rejected_[0].tag_ == "test");
  REQUIRE(strategy::reject_reason_name(plan.rejected_[0].reason_) == "quantity_rounds_to_zero");
}

TEST_CASE("an absent lot size applies no rounding rather than inventing one") {
  // The deliberate asymmetry with an unknown tick grid, which refuses outright: an off-grid price
  // is a certain rejection, whereas a quantity of one share is valid under every lot there is.
  Instrument reference = samsung_reference();
  reference.lot_size_ = std::nullopt;
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  ProbePortfolio portfolio;

  const auto plan = strategy::reconcile(target("47"), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("47"));
  REQUIRE_FALSE(plan.place_[0].quantity_rounded_);
}

TEST_CASE("an unknown tick grid refuses a limit order outright") {
  Instrument reference = samsung_reference();
  reference.price_rule_ = PriceRule::unknown("no table for this type");
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  ProbePortfolio portfolio;

  // The price has to come from somewhere the grid was known, so build it against KRX and then
  // reconcile against an instrument whose grid is not.
  const auto limit = strategy::Price::checked(Decimal64::parse("70000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("10", limit), portfolio, inputs);
  REQUIRE(plan.place_.empty());
  REQUIRE(plan.rejected_.size() == 1);
  REQUIRE(plan.rejected_[0].reason_ == RejectReason::UNKNOWN_PRICE_GRID);
}

TEST_CASE("missing reference data is rejected by name") {
  ProbePortfolio portfolio;
  ReconcileInputs inputs;  // no instrument
  const auto plan = strategy::reconcile(target("10"), portfolio, inputs);
  REQUIRE(plan.rejected_.size() == 1);
  REQUIRE(plan.rejected_[0].reason_ == RejectReason::NO_REFERENCE_DATA);
}

TEST_CASE("a buy limit above today's ceiling is clamped down to it") {
  // The direction matters: clamping down makes the order less willing than instructed, the same
  // rule tick rounding follows, and turns a certain rejection into an acceptable order.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  PriceLimits limits;
  limits.upper_ = Decimal64::parse("71000");
  limits.lower_ = Decimal64::parse("60000");
  inputs.limits_ = limits;

  const auto limit = strategy::Price::checked(Decimal64::parse("80000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("10", limit), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("71000")));
  REQUIRE(plan.place_[0].price_clamped_);
}

TEST_CASE("a sell limit below today's floor is clamped up to it") {
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  PriceLimits limits;
  limits.upper_ = Decimal64::parse("80000");
  limits.lower_ = Decimal64::parse("60000");
  inputs.limits_ = limits;

  const auto limit = strategy::Price::checked(Decimal64::parse("50000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("0", limit), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].side_ == OrderSide::SELL);
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("60000")));
  REQUIRE(plan.place_[0].price_clamped_);
}

TEST_CASE("a limit the band permits is left exactly where the strategy put it") {
  // A buy below the floor and a sell above the ceiling are valid prices that will not fill today.
  // Moving them would invent a willingness the strategy never expressed.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  PriceLimits limits;
  limits.upper_ = Decimal64::parse("80000");
  limits.lower_ = Decimal64::parse("60000");
  inputs.limits_ = limits;

  const auto limit = strategy::Price::checked(Decimal64::parse("50000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("10", limit), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("50000")));
  REQUIRE_FALSE(plan.place_[0].price_clamped_);
}

TEST_CASE("clamping happens before rounding, so the result is on the grid either way") {
  // A published bound need not be a tick multiple. Clamping first and rounding second keeps the
  // result inside the band, because rounding moves it away from the bound it was clamped to.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  PriceLimits limits;
  limits.upper_ = Decimal64::parse("70050");  // not a multiple of the 100-won tick
  inputs.limits_ = limits;

  const auto limit = strategy::Price::checked(Decimal64::parse("80000"), krx());
  REQUIRE(limit);
  const auto plan = strategy::reconcile(target("10", limit), portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  // Clamped to 70050, then rounded down for a buy to 70000: inside the band and on the grid.
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("70000")));
  REQUIRE(plan.place_[0].price_clamped_);
  REQUIRE(plan.place_[0].price_rounded_);
  REQUIRE(Price::checked(*plan.place_[0].price_, krx()));
}

TEST_CASE("an intent is sized as asked and not diffed against the portfolio") {
  // An intent says "trade this much", so the position is deliberately not consulted: a strategy
  // emitting intents has taken responsibility for its own sizing.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto intent = OrderIntent::market(
    samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("10"))
  ).with_tag("by-intent");
  const auto plan = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(plan.place_[0].tag_ == "by-intent");
}

TEST_CASE("a notional intent is divided by a price into whole shares") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("70000");

  const auto intent = OrderIntent::market(
    samsung(),
    OrderSide::BUY,
    SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}})
  );
  const auto plan = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  // A million won buys fourteen shares at seventy thousand, with the remainder left unspent
  // rather than rounded into a fifteenth share that cannot be paid for.
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("14"));
}

TEST_CASE("a notional intent with no price to divide by is rejected by name") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;  // no reference price

  const auto intent = OrderIntent::market(
    samsung(),
    OrderSide::BUY,
    SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}})
  );
  const auto plan = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(plan.place_.empty());
  REQUIRE(plan.rejected_[0].reason_ == RejectReason::NO_PRICE_AVAILABLE);
}

TEST_CASE("a weighted intent needs the portfolio's value and says so when it lacks it") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("70000");

  const auto intent = OrderIntent::market(
    samsung(), OrderSide::BUY, SizeSpec::target_weight(Decimal64::parse("0.25"))
  );
  const auto without = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(without.rejected_[0].reason_ == RejectReason::NO_PORTFOLIO_VALUE);

  inputs.portfolio_value_ = Decimal64::parse("10000000");
  const auto with = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(with.place_.size() == 1);
  // A quarter of ten million is 2.5 million, which buys 35 shares at 70,000.
  REQUIRE(with.place_[0].quantity_ == Decimal64::parse("35"));
}

TEST_CASE("a limit intent uses its own price to size a notional") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  // Deliberately no reference price: the limit is the price the order will pay.
  const auto limit = strategy::Price::checked(Decimal64::parse("50000"), krx());
  REQUIRE(limit);

  const auto intent = OrderIntent::limit(
    samsung(),
    OrderSide::BUY,
    SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}}),
    *limit
  );
  const auto plan = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].quantity_ == Decimal64::parse("20"));
  REQUIRE(plan.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("50000")));
}

TEST_CASE("an idempotency key on the intent reaches the planned order") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto intent = OrderIntent::market(
    samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("1"))
  ).with_client_order_id("idem-7");
  const auto plan = strategy::reconcile(intent, portfolio, inputs);
  REQUIRE(plan.place_.size() == 1);
  REQUIRE(plan.place_[0].client_order_id_ == std::optional<std::string>("idem-7"));
}

TEST_CASE("a rejected instruction produces no order, ever") {
  // The plan is cleared on rejection rather than carrying a partial result, so a caller that
  // reads place_ without checking rejected_ cannot act on half a decision.
  ProbePortfolio portfolio;
  ReconcileInputs inputs;
  const auto plan = strategy::reconcile(target("10"), portfolio, inputs);
  REQUIRE(plan.place_.empty());
  REQUIRE(plan.cancel_.empty());
  REQUIRE_FALSE(plan.rejected_.empty());
}

TEST_CASE("an order that has already finished is never asked to cancel") {
  // A snapshot can carry an order that has since filled or been cancelled. Asking a broker to
  // cancel one of those is a request that can only fail, and it would be noise on exactly the
  // path where a real cancellation failure has to be noticed.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  portfolio.working_.push_back(working("50", "50", OrderStatus::FILLED));
  portfolio.working_.push_back(working("20", "0", OrderStatus::CANCELED, OrderSide::SELL));
  portfolio.working_.push_back(working("30", "0", OrderStatus::PENDING, OrderSide::SELL));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  // Target equals the holding, so every *live* working order is unwanted -- and only the live one
  // is withdrawn.
  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.cancel_.size() == 1);
  REQUIRE(plan.place_.empty());
}

TEST_CASE("the open-order predicate is exhaustive over every status") {
  // Listed explicitly rather than as "not terminal", so a status added later does not silently
  // start counting as live.
  REQUIRE(strategy::order_is_open(OrderStatus::PENDING));
  REQUIRE(strategy::order_is_open(OrderStatus::PENDING_CANCEL));
  REQUIRE(strategy::order_is_open(OrderStatus::PENDING_REPLACE));
  REQUIRE(strategy::order_is_open(OrderStatus::PARTIAL_FILLED));
  REQUIRE(strategy::order_is_open(OrderStatus::UNKNOWN));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::FILLED));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::CANCELED));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::REJECTED));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::CANCEL_REJECTED));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::REPLACE_REJECTED));
  REQUIRE_FALSE(strategy::order_is_open(OrderStatus::REPLACED));
}
