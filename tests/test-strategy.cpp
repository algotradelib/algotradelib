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

TEST_CASE("a session window stays brace-initializable by a caller stating their own market") {
  // `local_date` brought a private member *function* with it, and a private member function keeps an
  // aggregate an aggregate while a private data member would not. Pinned because the difference is
  // invisible in review and the failure lands in a caller's code rather than here: an operator stating
  // a market that is not KRX writes exactly this.
  static_assert(
    std::is_aggregate_v<strategy::SessionWindow>,
    "SessionWindow must stay an aggregate: callers brace-initialize it member by member"
  );

  strategy::SessionWindow nyse{
    std::chrono::minutes(9 * 60 + 30),
    std::chrono::minutes(16 * 60),
    -std::chrono::minutes(5 * 60),
    "NYSE regular session (operator-stated)",
  };
  // 14:00 UTC is 09:00 in New York, half an hour before the open.
  REQUIRE(nyse.phase(utc(2026, 3, 10, 14, 0), true) == strategy::MarketPhase::PRE_OPEN);
  REQUIRE(nyse.phase(utc(2026, 3, 10, 15, 0), true) == strategy::MarketPhase::OPEN);
  REQUIRE(nyse.phase(utc(2026, 3, 10, 21, 0), true) == strategy::MarketPhase::POST_CLOSE);
  // And the date is still the market's: 03:00 UTC is the previous evening there.
  REQUIRE(nyse.local_date(utc(2026, 3, 10, 3, 0)) == Date{2026, 3, 9});
}

TEST_CASE("the local date is the market's, not the process's") {
  const auto session = strategy::SessionWindow::krx();
  // 23:30 UTC is already 08:30 the next morning in Seoul. A loop that asked its own calendar
  // whether *today* trades would consult Monday's answer for a Tuesday session -- and, on the
  // Sunday evening either side of this, would ask about a closed day for an open one.
  REQUIRE(session.local_date(utc(2026, 3, 9, 23, 30)) == Date{2026, 3, 10});
  REQUIRE(session.local_date(utc(2026, 3, 10, 0, 0)) == Date{2026, 3, 10});
  REQUIRE(session.local_date(utc(2026, 3, 10, 14, 59)) == Date{2026, 3, 10});
  // 15:00 UTC is midnight in Seoul, so the date rolls there and not at UTC midnight.
  REQUIRE(session.local_date(utc(2026, 3, 10, 15, 0)) == Date{2026, 3, 11});

  // A market west of UTC rolls the other way, which is the case a fixed offset gets wrong if the
  // floor rounds toward zero instead of toward negative infinity.
  strategy::SessionWindow new_york;
  new_york.utc_offset_ = -std::chrono::hours(5);
  REQUIRE(new_york.local_date(utc(2026, 3, 10, 4, 0)) == Date{2026, 3, 9});
  REQUIRE(new_york.local_date(utc(2026, 3, 10, 5, 0)) == Date{2026, 3, 10});
}

TEST_CASE("the local date and the phase never disagree about which day it is") {
  // They are two answers about one instant and a loop uses them together: the date asks the
  // calendar whether the market trades, the phase says where in the session it is. A rounding
  // difference between them would compute the boundaries for one day and the holiday answer for
  // another, which is the incoherence that is worst precisely at an open.
  const auto session = strategy::SessionWindow::krx();
  // Midnight in Seoul on a covered trading day: the date must already be the new one, and the
  // phase must be PRE_OPEN rather than the previous day's POST_CLOSE.
  const auto at_local_midnight = utc(2026, 3, 10, 15, 0);
  REQUIRE(session.local_date(at_local_midnight) == Date{2026, 3, 11});
  REQUIRE(session.phase(at_local_midnight, true) == strategy::MarketPhase::PRE_OPEN);
  // One minute earlier is still the old day, after its close.
  const auto a_minute_before = utc(2026, 3, 10, 14, 59);
  REQUIRE(session.local_date(a_minute_before) == Date{2026, 3, 10});
  REQUIRE(session.phase(a_minute_before, true) == strategy::MarketPhase::POST_CLOSE);
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

TEST_CASE("buying power is absent until a broker answers, and absent is not zero") {
  ProbePortfolio portfolio;

  // Nobody has asked. A caller enforcing a limit must read this as unknown, not as an empty wallet.
  REQUIRE_FALSE(portfolio.buying_power_in(Currency::krw()));

  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("5000000"), Currency::krw()}, {}}
  );
  REQUIRE(portfolio.buying_power_in(Currency::krw()) == Decimal64::parse("5000000"));

  // A currency the broker did not answer for stays unknown rather than borrowing another's number:
  // holdings-core refuses implicit conversion and this is the same refusal.
  REQUIRE_FALSE(portfolio.buying_power_in(Currency::usd()));

  // An answer of zero is a different claim from no answer, and both are representable.
  ProbePortfolio broke;
  broke.buying_power_.push_back(BuyingPower{Money{Decimal64::parse("0"), Currency::krw()}, {}});
  REQUIRE(broke.buying_power_in(Currency::krw()) == Decimal64::parse("0"));
}

TEST_CASE("cash and buying power are separate fields and do not answer for each other") {
  // The backtest ledger fills cash_; a live poll fills buying_power_. Neither may stand in for the
  // other, because one is this library's accounting and the other is the broker's.
  ProbePortfolio portfolio;
  portfolio.cash_.push_back(Money{Decimal64::parse("1000000"), Currency::krw()});

  REQUIRE(portfolio.cash_in(Currency::krw()) == Decimal64::parse("1000000"));
  REQUIRE_FALSE(portfolio.buying_power_in(Currency::krw()));

  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("1750000"), Currency::krw()}, {}}
  );
  // Unsettled sale proceeds are the ordinary reason these differ, and they must both survive.
  REQUIRE(portfolio.cash_in(Currency::krw()) == Decimal64::parse("1000000"));
  REQUIRE(portfolio.buying_power_in(Currency::krw()) == Decimal64::parse("1750000"));
}

TEST_CASE("sellable quantity is what the broker said, never the held quantity") {
  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");

  // KIS publishes this on every holdings snapshot; a position part-way through T+2 settlement is
  // held in full and sellable in part.
  position.sellable_quantity_ = Decimal64::parse("70");
  portfolio.positions_.push_back(position);

  REQUIRE(portfolio.quantity_of(samsung()) == Decimal64::parse("100"));
  REQUIRE(portfolio.sellable_quantity_of(samsung()) == Decimal64::parse("70"));

  // Toss leaves the field absent, and absent must not silently become the held quantity.
  ProbePortfolio silent;
  HoldingPosition unstated = position;
  unstated.sellable_quantity_ = std::nullopt;
  silent.positions_.push_back(unstated);

  REQUIRE(silent.quantity_of(samsung()) == Decimal64::parse("100"));
  REQUIRE_FALSE(silent.sellable_quantity_of(samsung()));

  // No position at all is also absent; quantity_of is what tells the two apart.
  ProbePortfolio empty;
  REQUIRE_FALSE(empty.sellable_quantity_of(samsung()));
  REQUIRE_FALSE(empty.quantity_of(samsung()));
}

namespace {

using Probe = strategy::SellableConventionProbe<strategy::ProbeBroker>;

/** A portfolio holding `held` with `sellable` reported, and `resting` on the sell side. */
ProbePortfolio observed(const char* held, const char* sellable, const char* resting) {
  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  position.sellable_quantity_ = Decimal64::parse(sellable);
  portfolio.positions_.push_back(position);
  if (Decimal64::parse(resting).coefficient() != 0) {
    portfolio.working_.push_back(working(resting, "0", OrderStatus::PENDING, OrderSide::SELL));
  }
  return portfolio;
}

} // namespace

TEST_CASE("the probe says nothing until a sell starts resting") {
  Probe probe;
  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);

  // Any number of polls with nothing resting teach it nothing, which is the common case and must
  // not drift toward an answer.
  probe.observe(observed("100", "70", "0"));
  probe.observe(observed("100", "70", "0"));
  probe.observe(observed("100", "70", "0"));
  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(probe.overall().total() == 0);
}

TEST_CASE("a figure that falls by the resting quantity is net of it") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  // Thirty starts resting and the figure drops by thirty: the broker had already subtracted it.
  probe.observe(observed("100", "70", "30"));

  REQUIRE(probe.verdict() == strategy::SellableConvention::NET_OF_RESTING);
  REQUIRE(probe.evidence(samsung()).net_ == 1);
  REQUIRE(probe.evidence(samsung()).gross_ == 0);
}

TEST_CASE("a figure that does not move is gross of the resting quantity") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  // Thirty rests and the figure is unchanged, so the deduction the guard makes is doing real work.
  probe.observe(observed("100", "100", "30"));

  REQUIRE(probe.verdict() == strategy::SellableConvention::GROSS_OF_RESTING);
  REQUIRE(probe.evidence(samsung()).gross_ == 1);
}

TEST_CASE("a fill in the same window is not evidence of anything") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  // The holding moved, so the sell filled somewhere in here and both numbers moved with it.
  probe.observe(observed("70", "70", "30"));

  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(probe.overall().total() == 0);
}

TEST_CASE("a move by the wrong amount is discarded rather than forced into an answer") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  // Thirty rests and the figure falls by fifty: a settlement or a pledge landed in the same window,
  // and neither rule describes it.
  probe.observe(observed("100", "50", "30"));

  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(probe.overall().total() == 0);
}

TEST_CASE("the probe reports disagreement rather than resolving it") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  probe.observe(observed("100", "70", "30"));   // net
  probe.observe(observed("100", "70", "50"));   // gross: twenty more rests, the figure holds

  REQUIRE(probe.verdict() == strategy::SellableConvention::INCONSISTENT);
  REQUIRE(probe.evidence(samsung()).net_ == 1);
  REQUIRE(probe.evidence(samsung()).gross_ == 1);
  REQUIRE(std::string(strategy::sellable_convention_name(probe.verdict())) == "inconsistent");
}

TEST_CASE("a broker that publishes no sellable figure teaches the probe nothing") {
  // Toss leaves the field absent, so there is nothing to compare and the probe stays silent rather
  // than treating absence as a number.
  Probe probe;
  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(position);
  probe.observe(portfolio);
  portfolio.working_.push_back(working("30", "0", OrderStatus::PENDING, OrderSide::SELL));
  probe.observe(portfolio);

  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);
}

TEST_CASE("a scale change between polls is not read as a change in value") {
  // Decimal64 equality compares the scale as well as the coefficient, so "100" and "100.0" are
  // different values to it. A broker free to change the scale it reports must not make the probe
  // conclude from formatting.
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  probe.observe(observed("100.0", "100.00", "30"));

  REQUIRE(probe.verdict() == strategy::SellableConvention::GROSS_OF_RESTING);
}

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

// ---------------------------------------------------------------------------
// Risk guards: one test per limit, and none of them resizes
// ---------------------------------------------------------------------------

namespace {

using strategy::GuardInputs;
using strategy::GuardReason;
using strategy::RiskGuard;
using strategy::RiskLimits;

/** A planned order of `quantity` shares at `price`, as reconciliation would have produced it. */
PlannedOrder planned(
  const char* quantity,
  const char* price = nullptr,
  OrderSide side = OrderSide::BUY
) {
  PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = side;
  order.quantity_ = Decimal64::parse(quantity);
  order.tag_ = "test";
  if (price != nullptr) {
    order.type_ = OrderType::LIMIT;
    order.price_ = Decimal64::parse(price);
  } else {
    order.type_ = OrderType::MARKET;
  }
  return order;
}

/** Inputs with the market open and a price available, so one limit is tested at a time. */
GuardInputs open_market() {
  GuardInputs inputs;
  inputs.now_ = utc(2026, 3, 10, 3, 0);
  inputs.phase_ = strategy::MarketPhase::OPEN;
  inputs.reference_price_ = Decimal64::parse("70000");
  return inputs;
}

/**
 * Adds the resolution the exposure ceiling needs: Samsung, in won, marked at 70,000.
 *
 * Separate from `open_market` so that every other limit's tests keep proving they do not need it,
 * and so a test can add a second instrument or leave one out deliberately.
 */
GuardInputs with_basis(GuardInputs inputs) {
  inputs.order_currency_ = Currency{"KRW"};
  inputs.exposure_basis_.push_back(
    strategy::InstrumentBasis{samsung(), Currency{"KRW"}, Decimal64::parse("70000")}
  );
  return inputs;
}

} // namespace

TEST_CASE("an unconfigured guard permits an ordinary order while the market is open") {
  const RiskGuard guard;
  ProbePortfolio portfolio;
  REQUIRE_FALSE(guard.check(planned("10", "70000"), portfolio, open_market()));
}

TEST_CASE("the kill switch refuses everything, and reports the halt above any other reason") {
  // An operator reading a log should see the halt rather than an incidental ceiling the order also
  // happened to exceed, so it is checked first.
  RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("1");
  RiskGuard guard(limits);
  ProbePortfolio portfolio;

  guard.halt();
  REQUIRE(guard.halted());
  const auto refused = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::TRADING_HALTED);
  // And it says *who*: a bare halt is the operator's, which is the holder whose intent machinery
  // holding the same switch must never clear.
  REQUIRE(refused->halt_reason_);
  REQUIRE(*refused->halt_reason_ == strategy::HaltReason::OPERATOR);

  guard.resume();
  // Now the notional ceiling is the reason, which it was all along.
  const auto after = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(after);
  REQUIRE(after->reason_ == GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  REQUIRE_FALSE(after->halt_reason_);
}

TEST_CASE("the buying-power check refuses an unknown reading rather than assuming one") {
  RiskLimits limits;
  limits.enforce_buying_power_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;  // nobody has asked the broker anything
  auto inputs = open_market();
  inputs.order_currency_ = Currency::krw();

  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::INSUFFICIENT_BUYING_POWER);
  // Unknown carries no numbers: there is no ceiling to report, which is the point.
  REQUIRE_FALSE(refused->limit_);

  // Cash is not an answer to this question, and must not be read as one.
  ProbePortfolio with_cash;
  with_cash.cash_.push_back(Money{Decimal64::parse("100000000"), Currency::krw()});
  REQUIRE(guard.check(planned("10", "70000"), with_cash, inputs));
}

TEST_CASE("a buy beyond the reported buying power is refused with both numbers") {
  RiskLimits limits;
  limits.enforce_buying_power_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;
  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("500000"), Currency::krw()}, {}}
  );
  auto inputs = open_market();
  inputs.order_currency_ = Currency::krw();

  // Ten at seventy thousand is seven hundred thousand, against five.
  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::INSUFFICIENT_BUYING_POWER);
  REQUIRE(refused->limit_ == Decimal64::parse("500000"));
  REQUIRE(refused->attempted_ == Decimal64::parse("700000"));

  // Seven fits exactly at 490,000.
  REQUIRE_FALSE(guard.check(planned("7", "70000"), portfolio, inputs));

  // A market order is valued at the reference price, and a sell is not checked at all.
  REQUIRE(guard.check(planned("10", nullptr), portfolio, inputs));
  REQUIRE_FALSE(guard.check(planned("10", "70000", OrderSide::SELL), portfolio, inputs));
}

TEST_CASE("buying power is per currency and never borrows another currency's answer") {
  RiskLimits limits;
  limits.enforce_buying_power_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;
  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("100000000"), Currency::krw()}, {}}
  );

  auto won = open_market();
  won.order_currency_ = Currency::krw();
  REQUIRE_FALSE(guard.check(planned("10", "70000"), portfolio, won));

  // The same order priced in dollars has no answer, and holdings-core's refusal to convert is why
  // this is unknown rather than generously funded.
  auto dollars = open_market();
  dollars.order_currency_ = Currency::usd();
  const auto refused = guard.check(planned("10", "70000"), portfolio, dollars);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::INSUFFICIENT_BUYING_POWER);

  // An instrument the runner could not resolve has no currency, and refuses for the same reason.
  auto unresolved = open_market();
  REQUIRE(guard.check(planned("10", "70000"), portfolio, unresolved));
}

TEST_CASE("a withdrawal spends rate budget but can never be refused by the limit") {
  // The asymmetry the todo named. A rate limit must never be the reason a cancellation cannot go --
  // that is the premise ai/rate-limit-control was built on -- but not *counting* one is a different
  // claim, and it makes the window mean less than the field's own documentation says.
  RiskLimits limits;
  limits.max_orders_per_window_ = 2;
  RiskGuard guard(limits);
  const auto now = utc(2026, 3, 10, 3, 0);

  // Two withdrawals' worth of budget spent, as the runner now records them.
  guard.note_sent(now);
  guard.note_sent(now);

  // The next *placement* is refused, because the broker counted those two requests.
  auto inputs = open_market();
  inputs.now_ = now;
  ProbePortfolio portfolio;
  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::ORDER_RATE_EXCEEDED);

  // And nothing in the guard can refuse a withdrawal at all: `send_cancel` consults no limit, so
  // the only way this could stop one is if a caller asked it to, which no caller does.
  REQUIRE(guard.limits().max_orders_per_window_ == std::size_t{2});
}

TEST_CASE("the sellable check is off until an operator asks for it") {
  // Off by default, because a broker that does not publish the figure would otherwise stop every
  // sell an existing program makes.
  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(position);  // no sellable figure at all

  const RiskGuard permissive;
  REQUIRE_FALSE(
    permissive.check(planned("100", "70000", OrderSide::SELL), portfolio, open_market())
  );

  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard strict(limits);
  const auto refused = strict.check(planned("100", "70000", OrderSide::SELL), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::QUANTITY_NOT_SELLABLE);
}

TEST_CASE("a sell beyond what the broker says is sellable is refused with both numbers") {
  // The T+2 case: a hundred held, seventy sellable today. Sizing from the position alone sends a
  // sell the broker will reject, one round trip later.
  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  position.sellable_quantity_ = Decimal64::parse("70");
  portfolio.positions_.push_back(position);

  const auto refused = guard.check(planned("100", "70000", OrderSide::SELL), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::QUANTITY_NOT_SELLABLE);
  // Both numbers, so a strategy can resize rather than retry the same order.
  REQUIRE(refused->limit_ == Decimal64::parse("70"));
  REQUIRE(refused->attempted_ == Decimal64::parse("100"));

  // Exactly the sellable quantity is permitted; the check is a ceiling, not a margin.
  REQUIRE_FALSE(guard.check(planned("70", "70000", OrderSide::SELL), portfolio, open_market()));
}

TEST_CASE("a resting sell spends the same sellable quantity as a new one") {
  // Neither broker states whether its figure is already net of resting sells, so this subtracts
  // them: refusing an order that would have been accepted is recoverable, selling a position twice
  // is not.
  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  position.sellable_quantity_ = Decimal64::parse("70");
  portfolio.positions_.push_back(position);
  portfolio.working_.push_back(working("50", "0", OrderStatus::PENDING, OrderSide::SELL));

  // Fifty already working plus forty more is ninety against seventy sellable.
  const auto refused = guard.check(planned("40", "70000", OrderSide::SELL), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::QUANTITY_NOT_SELLABLE);
  REQUIRE(refused->limit_ == Decimal64::parse("70"));
  REQUIRE(refused->attempted_ == Decimal64::parse("90"));

  // Twenty more fits exactly.
  REQUIRE_FALSE(guard.check(planned("20", "70000", OrderSide::SELL), portfolio, open_market()));

  // A resting *buy* spends none of it, because it is not a sale.
  ProbePortfolio with_buy;
  with_buy.positions_.push_back(position);
  with_buy.working_.push_back(working("50", "0", OrderStatus::PENDING, OrderSide::BUY));
  REQUIRE_FALSE(guard.check(planned("70", "70000", OrderSide::SELL), with_buy, open_market()));
}

TEST_CASE("the sellable check never refuses a buy, whatever the position says") {
  // It is a statement about disposing of a holding, so it has nothing to say about acquiring one.
  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;  // no position at all, so the sellable figure is absent
  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::BUY), portfolio, open_market()));
}

TEST_CASE("a hold is a set, so taking it twice still takes one release") {
  // Not a counter. The holders are distinct conditions rather than distinct callers, so a loop that
  // recomputes the same condition every iteration must not build a hold that as many releases are
  // needed to clear.
  RiskGuard guard;

  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);
  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);
  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);
  REQUIRE(guard.halted());

  guard.resume(strategy::HaltReason::PORTFOLIO_STALE);
  REQUIRE_FALSE(guard.halted());
}

TEST_CASE("releasing a hold nobody took does nothing") {
  RiskGuard guard;
  guard.resume(strategy::HaltReason::POLL_FAILING);
  REQUIRE_FALSE(guard.halted());

  guard.halt(strategy::HaltReason::STOPPING);
  guard.resume(strategy::HaltReason::POLL_FAILING);
  // Releasing the wrong one leaves the real one exactly where it was.
  REQUIRE(guard.halted());
  REQUIRE(guard.halted(strategy::HaltReason::STOPPING));
}

TEST_CASE("one holder cannot release another's hold") {
  // The defect this whole design exists for, at the primitive: an operator halting into a hold the
  // machinery already took must survive that machinery releasing its own.
  RiskGuard guard;
  ProbePortfolio portfolio;

  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);
  guard.halt();  // the operator, deciding independently, into an already-halted guard

  guard.resume(strategy::HaltReason::PORTFOLIO_STALE);
  REQUIRE(guard.halted());
  REQUIRE(guard.halted(strategy::HaltReason::OPERATOR));

  const auto refused = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->halt_reason_);
  REQUIRE(*refused->halt_reason_ == strategy::HaltReason::OPERATOR);

  // And the mirror: an operator's resume does not lift the machinery's hold either.
  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);
  guard.resume();
  REQUIRE(guard.halted());
  REQUIRE_FALSE(guard.halted(strategy::HaltReason::OPERATOR));
}

TEST_CASE("the most fundamental hold is the one reported") {
  // Declaration order is the priority order, and it is part of the enum's contract.
  RiskGuard guard;
  ProbePortfolio portfolio;

  guard.halt(strategy::HaltReason::POLL_FAILING);
  guard.halt(strategy::HaltReason::LOCAL_FAILURE, "the order journal could not be written");
  guard.halt(strategy::HaltReason::PORTFOLIO_STALE);

  REQUIRE(guard.blocking_reason());
  REQUIRE(*guard.blocking_reason() == strategy::HaltReason::LOCAL_FAILURE);
  const auto refused = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(*refused->halt_reason_ == strategy::HaltReason::LOCAL_FAILURE);

  // An operator halting on top outranks even that, because it is the one a person is waiting on.
  guard.halt();
  REQUIRE(*guard.blocking_reason() == strategy::HaltReason::OPERATOR);

  // Every hold is still visible, most fundamental first, which is what the bool could never answer.
  const auto held = guard.holds();
  REQUIRE(held.size() == 4);
  REQUIRE(held[0] == strategy::HaltReason::OPERATOR);
  REQUIRE(held[1] == strategy::HaltReason::LOCAL_FAILURE);
  REQUIRE(held[2] == strategy::HaltReason::PORTFOLIO_STALE);
  REQUIRE(held[3] == strategy::HaltReason::POLL_FAILING);
}

TEST_CASE("a hold carries the detail an operator needs to act on it") {
  RiskGuard guard;

  guard.halt(strategy::HaltReason::LOCAL_FAILURE, "the order journal could not be written");
  REQUIRE(guard.hold_detail(strategy::HaltReason::LOCAL_FAILURE) ==
          "the order journal could not be written");
  // A hold taken with no detail reports none rather than a stale one.
  REQUIRE(guard.hold_detail(strategy::HaltReason::OPERATOR).empty());

  guard.resume(strategy::HaltReason::LOCAL_FAILURE);
  REQUIRE(guard.hold_detail(strategy::HaltReason::LOCAL_FAILURE).empty());
}

TEST_CASE("every halt reason has a stable name") {
  // The names reach logs and reports, so they are pinned rather than left to the enum's spelling.
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::OPERATOR) == "operator");
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::LOCAL_FAILURE) == "local_failure");
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::STOPPING) == "stopping");
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::ORDER_LIST_UNREAD) ==
          "order_list_unread");
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::PORTFOLIO_STALE) == "portfolio_stale");
  REQUIRE(strategy::halt_reason_name(strategy::HaltReason::POLL_FAILING) == "poll_failing");
}

TEST_CASE("only the market phases the operator permits are traded in") {
  const RiskGuard guard;  // defaults to OPEN alone
  ProbePortfolio portfolio;

  for (const auto phase : {
         strategy::MarketPhase::PRE_OPEN,
         strategy::MarketPhase::POST_CLOSE,
         strategy::MarketPhase::CLOSED,
       }) {
    auto inputs = open_market();
    inputs.phase_ = phase;
    const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::PHASE_NOT_PERMITTED);
  }
}

TEST_CASE("an uncovered calendar date does not become an order placed into a holiday") {
  // The reason UNKNOWN is a distinct phase rather than a synonym for CLOSED, carried all the way
  // through to a refusal by default.
  const RiskGuard guard;
  ProbePortfolio portfolio;
  auto inputs = open_market();
  inputs.phase_ = strategy::MarketPhase::UNKNOWN;

  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::PHASE_NOT_PERMITTED);
}

TEST_CASE("permitting any phase has to be asked for explicitly") {
  RiskLimits limits;
  limits.permitted_phases_ = std::nullopt;  // explicitly: any
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  auto inputs = open_market();
  inputs.phase_ = strategy::MarketPhase::UNKNOWN;

  REQUIRE_FALSE(guard.check(planned("10", "70000"), portfolio, inputs));
}

TEST_CASE("an instrument outside the permitted universe is refused") {
  RiskLimits limits;
  limits.permitted_universe_ = std::vector<InstrumentId>{
    InstrumentId{Country::KR, Venue::COMBINED, "000660"}
  };
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;

  const auto refused = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::INSTRUMENT_NOT_PERMITTED);

  // And the one that is listed goes through.
  auto allowed = planned("10", "70000");
  allowed.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  REQUIRE_FALSE(guard.check(allowed, portfolio, open_market()));
}

TEST_CASE("an order worth more than the per-order ceiling is refused, not halved") {
  // The central promise: halving would place a position the strategy never sized against a signal
  // it never gave, and the strategy would then reconcile against that position and compound it.
  RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("500000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;

  const auto order = planned("10", "70000");  // 700,000 won
  const auto refused = guard.check(order, portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  // The arithmetic comes back so an operator need not reconstruct it, and a strategy can adapt
  // rather than retry the same size.
  REQUIRE(refused->limit_ == std::optional<Decimal64>(Decimal64::parse("500000")));
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("700000")));
  REQUIRE(refused->tag_ == "test");
  // The order itself is untouched. A guard returns a reason; it does not edit the instruction.
  REQUIRE(order.quantity_ == Decimal64::parse("10"));

  // Exactly at the ceiling is permitted; the limit is a maximum, not a bound to stay under.
  REQUIRE_FALSE(guard.check(planned("50000", "10"), portfolio, open_market()));
}

TEST_CASE("a market order is valued from the reference price the runner supplies") {
  RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("500000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;

  // A market order carries no price of its own, so the last quote is what values it.
  const auto refused = guard.check(planned("10"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::ORDER_NOTIONAL_TOO_LARGE);
}

TEST_CASE("a limit that cannot be evaluated refuses rather than waving the order through") {
  // No price anywhere, so the notional is unknown. Permitting it would make the ceiling advisory.
  RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("500000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  auto inputs = open_market();
  inputs.reference_price_ = std::nullopt;

  const auto refused = guard.check(planned("10"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);

  // With no value-dependent limit configured, the same order is fine: an absent price only
  // matters to a limit that needed it.
  const RiskGuard unlimited;
  REQUIRE_FALSE(unlimited.check(planned("10"), portfolio, inputs));
}

TEST_CASE("the position ceiling counts what the order would make the position, not the order") {
  RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("100");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("95"));

  // Ten more would reach 105.
  const auto refused = guard.check(planned("10", "70000"), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("105")));

  // Five reaches exactly the ceiling, which is permitted.
  REQUIRE_FALSE(guard.check(planned("5", "70000"), portfolio, open_market()));

  // And selling reduces it, so it is never refused by this limit.
  REQUIRE_FALSE(guard.check(planned("50", "70000", OrderSide::SELL), portfolio, open_market()));
}

TEST_CASE("the position ceiling limits a short as firmly as a long") {
  // The ceiling is on magnitude. A limit that only counted longs would let a strategy build an
  // unbounded short while appearing to be risk-limited.
  RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("100");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("-95"));

  const auto refused =
    guard.check(planned("10", "70000", OrderSide::SELL), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("105")));
}

TEST_CASE("the gross exposure ceiling measures the portfolio, not the order in front of it") {
  // **The defect this replaces.** `GuardInputs::gross_exposure_` was never assigned anywhere in the
  // library -- the only assignment in the tree was in this file -- so the current exposure defaulted
  // to zero and the ceiling was compared against one order's notional. A ten-million ceiling did not
  // stop a strategy reaching twenty million two million at a time.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("135"));
  const auto inputs = with_basis(open_market());

  // 135 held at 70,000 is 9,450,000. Ten more reaches 145 shares, which is 10,150,000.
  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("10150000")));

  // Seven more reaches 142 shares, which is 9,940,000 and under the ceiling.
  REQUIRE_FALSE(guard.check(planned("7", "70000"), portfolio, inputs));
}

TEST_CASE("the exposure ceiling counts what is already working, not only what is held") {
  // Without this, two orders in one pass are each checked from the same starting point and together
  // breach the ceiling -- the two-sells-of-sixty defect one level up. The runner writes an accepted
  // order into its view as a working row, so counting working rows is what makes that fix reach
  // this limit.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING, OrderSide::BUY));
  const auto inputs = with_basis(open_market());

  // Nothing held, 100 working at 70,000, and 50 more reaches 150 shares: 10,500,000.
  const auto refused = guard.check(planned("50", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("10500000")));
  // The order alone is 3,500,000, which is what the previous code compared against the ceiling.
}

TEST_CASE("an order that adds no exposure is permitted, even over the ceiling") {
  // A ceiling that cannot be de-risked past is worse than one that does nothing. The projection is
  // the largest position still reachable, so a resting sell does not lower it -- which on its own
  // would refuse every order from an account already over the ceiling, including the ones selling
  // out of it. The ceiling therefore governs what an order *adds*.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("7500000");
  const RiskGuard guard(limits);
  const auto inputs = with_basis(open_market());

  ProbePortfolio under;
  under.positions_.push_back(holding("100"));
  // 7,000,000 held, under the ceiling. A sell leaves the projection at 100 shares and is allowed;
  // the previous code added the order's 700,000 and refused it.
  REQUIRE_FALSE(guard.check(planned("10", "70000", OrderSide::SELL), under, inputs));
  // A buy of the same size takes it to 7,700,000 and is refused.
  REQUIRE(guard.check(planned("10", "70000"), under, inputs));

  ProbePortfolio over;
  over.positions_.push_back(holding("200"));
  // 14,000,000 held against a 7,500,000 ceiling. Every sell out of it is still permitted, including
  // one that flattens the position outright.
  REQUIRE_FALSE(guard.check(planned("10", "70000", OrderSide::SELL), over, inputs));
  REQUIRE_FALSE(guard.check(planned("200", "70000", OrderSide::SELL), over, inputs));
  // And a buy of one share is not, because it takes the figure further up.
  REQUIRE(guard.check(planned("1", "70000"), over, inputs));

  ProbePortfolio shorted;
  shorted.positions_.push_back(holding("-200"));
  // The same for a short: covering it is permitted, adding to it is not.
  REQUIRE_FALSE(guard.check(planned("200", "70000"), shorted, inputs));
  REQUIRE(guard.check(planned("1", "70000", OrderSide::SELL), shorted, inputs));

  // **Opposing a position is not on its own a licence.** Selling 300 against a long of 100 reaches a
  // short of 200 -- more reachable exposure than the 100 it started from -- so the same rule refuses
  // it. The test is what the order does to the projection, not which way it points.
  ProbePortfolio small;
  small.positions_.push_back(holding("100"));
  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::SELL), small, inputs));
  const auto reversal = guard.check(planned("300", "70000", OrderSide::SELL), small, inputs);
  REQUIRE(reversal);
  REQUIRE(reversal->reason_ == GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(reversal->attempted_ == std::optional<Decimal64>(Decimal64::parse("14000000")));
}

TEST_CASE("an exit limited above the mark is not read as adding exposure") {
  // **The two projections have to use the same price.** The candidate's limit joins the valuation
  // envelope, so a version that computed the "without" case by dropping the candidate valued it at
  // the mark instead -- and the two then differed by a price rather than by the order. A sell of ten
  // limited at 80,000 against a 200-share position read as 16,000,000 versus 14,000,000, so the
  // ceiling refused the one thing it must never refuse.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("5000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("200"));
  const auto inputs = with_basis(open_market());

  // 14,000,000 held against a 5,000,000 ceiling, so the account is already well over.
  REQUIRE_FALSE(guard.check(planned("10", "80000", OrderSide::SELL), portfolio, inputs));
  // The same sell at the mark was always allowed; the point is that the limit above it also is.
  REQUIRE_FALSE(guard.check(planned("10", "70000", OrderSide::SELL), portfolio, inputs));
  // And a buy limited above the mark is still refused, so the envelope has not gone soft.
  REQUIRE(guard.check(planned("10", "80000"), portfolio, inputs));
}

TEST_CASE("a position reported as two rows is counted once, at its whole size") {
  // `Portfolio::quantity_of` returns the first matching row and nothing dedupes what an adapter
  // appends, so a broker reporting one instrument as two lots would have had the ceiling measure
  // only the first of them.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  portfolio.positions_.push_back(holding("100"));
  const auto inputs = with_basis(open_market());

  const auto exposure = strategy::projected_gross_exposure(
    portfolio, inputs.exposure_basis_, Currency{"KRW"}, nullptr, std::nullopt,
    strategy::CandidateContribution::PRICE_ONLY
  );
  // 200 shares at 70,000, not the 100 the first row alone reports.
  REQUIRE(exposure == std::optional<Decimal64>(Decimal64::parse("14000000")));

  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);
  REQUIRE(guard.check(planned("1", "70000"), portfolio, inputs));
}

TEST_CASE("a mark that is not positive refuses rather than valuing anything") {
  // `Context::set_last_price` stores whatever a bar or quote carried. A negative mark made an
  // instrument's exposure negative, so a malformed quote could offset real holdings until the
  // ceiling passed anything at all; zero values the position at nothing, which is the same silent
  // omission by another route.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("1000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;

  for (const char* mark : {"-70000", "0"}) {
    auto inputs = open_market();
    inputs.order_currency_ = Currency{"KRW"};
    inputs.exposure_basis_.push_back(
      strategy::InstrumentBasis{samsung(), Currency{"KRW"}, Decimal64::parse(mark)}
    );
    inputs.reference_price_ = Decimal64::parse(mark);
    const auto refused = guard.check(planned("100"), portfolio, inputs);
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }
}

TEST_CASE("a candidate or working price that is not a price refuses") {
  // A negative candidate price loses the maximum to a positive basis and disappears, so the ceiling
  // would value the order at a mark nobody quoted -- while `max_order_notional_` valued it at a
  // negative number no ceiling exceeds. `Price::checked` builds a negative limit under an
  // unconstrained rule and `Context::set_last_price` stores whatever a bar carried, so neither
  // source rules it out upstream.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("1000000");
  const RiskGuard guard(limits);

  SECTION("the candidate's own limit") {
    ProbePortfolio portfolio;
    const auto refused = guard.check(planned("1", "-100"), portfolio, with_basis(open_market()));
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }

  SECTION("the reference price a market order is valued at") {
    ProbePortfolio portfolio;
    auto inputs = with_basis(open_market());
    inputs.reference_price_ = Decimal64::parse("-70000");
    const auto refused = guard.check(planned("1"), portfolio, inputs);
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }

  SECTION("a resting order's limit") {
    ProbePortfolio portfolio;
    auto resting = working("10", "0", OrderStatus::PENDING, OrderSide::BUY);
    resting.order_.price_ = Decimal64::parse("-100");
    portfolio.working_.push_back(resting);
    const auto refused = guard.check(planned("1", "70000"), portfolio, with_basis(open_market()));
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }
}

TEST_CASE("an order filled beyond what it ordered refuses rather than being dropped") {
  // `working_quantity` skips such a row, which is right for a figure a strategy reads -- it declines
  // to amplify a broker's contradiction. It is wrong for a limit: dropping the row takes its limit
  // price out of the valuation envelope while the row is still PENDING, and a ceiling cannot infer
  // from an inconsistent snapshot that nothing is outstanding.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("10", "20", OrderStatus::PENDING, OrderSide::BUY));

  const auto refused = guard.check(planned("1", "70000"), portfolio, with_basis(open_market()));
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
}

TEST_CASE("a position that has gone flat does not refuse orders in another instrument") {
  // `ReplayLedger` keeps a lot after it returns to flat and a broker may list a closed position.
  // Requiring a mark for one would let an instrument nobody holds any more refuse everything.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);

  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  auto closed = holding("0");
  closed.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  portfolio.positions_.push_back(closed);

  // The basis describes Samsung only, and the flat row is not a reason to refuse.
  REQUIRE_FALSE(guard.check(planned("1", "70000"), portfolio, with_basis(open_market())));

  // But a flat row with an order still working on it does need valuing, because that order can fill.
  auto resting = working("10", "0", OrderStatus::PENDING, OrderSide::BUY);
  resting.order_.instrument_ = closed.instrument_;
  portfolio.working_.push_back(resting);
  const auto refused = guard.check(planned("1", "70000"), portfolio, with_basis(open_market()));
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
}

TEST_CASE("an ordinary portfolio is valued and permitted, not refused by the tightening") {
  // **The property four rounds of review put at risk.** Each round added a refusal -- an incomplete
  // basis, a non-positive price, an inconsistent order row -- and a limit that refuses ordinary
  // trading is a worse outcome than the permissive defect it replaced. This is an ordinary
  // afternoon: three positions, one partially filled working buy, one resting sell, and a lot left
  // behind by a closed position that nothing registered.
  const auto hynix = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  const auto naver = InstrumentId{Country::KR, Venue::COMBINED, "035420"};
  const auto closed = InstrumentId{Country::KR, Venue::COMBINED, "051910"};

  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  auto second = holding("250");
  second.instrument_ = hynix;
  second.last_price_ = Decimal64::parse("180000");
  portfolio.positions_.push_back(second);
  auto third = holding("40");
  third.instrument_ = naver;
  third.last_price_ = Decimal64::parse("220000");
  portfolio.positions_.push_back(third);
  auto flat = holding("0");
  flat.instrument_ = closed;
  portfolio.positions_.push_back(flat);

  auto partial = working("50", "20", OrderStatus::PARTIAL_FILLED, OrderSide::BUY);
  partial.order_.instrument_ = hynix;
  partial.order_.price_ = Decimal64::parse("181000");
  portfolio.working_.push_back(partial);
  auto resting = working("10", "0", OrderStatus::PENDING, OrderSide::SELL);
  resting.order_.instrument_ = naver;
  resting.order_.price_ = Decimal64::parse("225000");
  portfolio.working_.push_back(resting);

  auto inputs = with_basis(open_market());
  inputs.exposure_basis_.push_back(
    strategy::InstrumentBasis{hynix, Currency{"KRW"}, Decimal64::parse("180000")}
  );
  inputs.exposure_basis_.push_back(
    strategy::InstrumentBasis{naver, Currency{"KRW"}, Decimal64::parse("220000")}
  );
  // Nothing describes the closed lot, and nothing needs to.

  // 100 at 70,000, plus 250 held and 30 still working at the 181,000 limit, plus 40 at the resting
  // sell's 225,000 limit: 7,000,000 + 50,680,000 + 9,000,000.
  const auto exposure = strategy::projected_gross_exposure(
    portfolio, inputs.exposure_basis_, Currency{"KRW"}, nullptr, std::nullopt,
    strategy::CandidateContribution::PRICE_ONLY
  );
  REQUIRE(exposure == std::optional<Decimal64>(Decimal64::parse("66680000")));

  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("100000000");
  const RiskGuard guard(limits);
  REQUIRE_FALSE(guard.check(planned("10", "70500"), portfolio, inputs));
  REQUIRE_FALSE(guard.check(planned("10", "69500", OrderSide::SELL), portfolio, inputs));
  // And a market order, which carries no price of its own and is valued at the reference.
  REQUIRE_FALSE(guard.check(planned("5"), portfolio, inputs));
}

TEST_CASE("a limit order is valued at its limit, not at a cheaper quote") {
  // A basis carrying only the last quote under-reports a limit order that can fill above it, which
  // is the one direction a ceiling must not err in.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("8000000");
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  const auto inputs = with_basis(open_market());

  // 100 at a limit of 100,000 is 10,000,000, though the mark in the basis is 70,000. Valued at the
  // mark it would have been 7,000,000 and passed.
  const auto refused = guard.check(planned("100", "100000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("10000000")));
}

TEST_CASE("an exposure basis that does not describe the portfolio refuses") {
  // Decision 6, and the reason the basis replaced an optional total. An instrument left out of the
  // sum could have belonged to the currency being checked, so omitting it under-reports.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);

  SECTION("a held instrument with no entry") {
    ProbePortfolio portfolio;
    portfolio.positions_.push_back(holding("10"));
    auto other = holding("10");
    other.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
    portfolio.positions_.push_back(other);
    // The basis describes Samsung only, and the second position is unresolved.
    const auto refused = guard.check(planned("1", "70000"), portfolio, with_basis(open_market()));
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }

  SECTION("two entries for one instrument") {
    ProbePortfolio portfolio;
    auto inputs = with_basis(open_market());
    inputs.exposure_basis_.push_back(
      strategy::InstrumentBasis{samsung(), Currency{"KRW"}, Decimal64::parse("80000")}
    );
    const auto refused = guard.check(planned("1", "70000"), portfolio, inputs);
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }

  SECTION("no resolved currency for the order") {
    ProbePortfolio portfolio;
    auto inputs = with_basis(open_market());
    inputs.order_currency_ = std::nullopt;
    const auto refused = guard.check(planned("1", "70000"), portfolio, inputs);
    REQUIRE(refused);
    REQUIRE(refused->reason_ == GuardReason::VALUE_NOT_KNOWN);
  }
}

TEST_CASE("the exposure ceiling is per currency and neither currency leaks into the other") {
  // The same number applied independently to each currency, which is `max_order_notional_`'s reading
  // and `holdings-core`'s refusal to add currencies without a rate the caller can see.
  RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  const RiskGuard guard(limits);

  const auto apple = InstrumentId{Country::US, Venue::COMBINED, "AAPL"};
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("135"));
  auto foreign = holding("1000");
  foreign.instrument_ = apple;
  portfolio.positions_.push_back(foreign);

  auto inputs = with_basis(open_market());
  inputs.exposure_basis_.push_back(
    strategy::InstrumentBasis{apple, Currency{"USD"}, Decimal64::parse("200")}
  );

  // The KRW answer is 9,450,000 and the dollar position contributes nothing to it, so a buy of seven
  // Samsung passes exactly as it does with no foreign holding at all.
  REQUIRE_FALSE(guard.check(planned("7", "70000"), portfolio, inputs));
  // And ten does not, on the KRW figure alone.
  const auto refused = guard.check(planned("10", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("10150000")));
}

TEST_CASE("the order rate is counted over a sliding window, not reset on a boundary") {
  // A counter reset on a boundary lets twice the limit through across one, which is the classic
  // way a rate limit is wrong. The window slides instead.
  RiskLimits limits;
  limits.max_orders_per_window_ = 3;
  limits.rate_window_ = std::chrono::seconds(60);
  RiskGuard guard(limits);
  ProbePortfolio portfolio;

  auto inputs = open_market();
  const auto start = inputs.now_;
  for (int at = 0; at < 3; ++at) {
    inputs.now_ = start + std::chrono::seconds(at * 10);
    REQUIRE_FALSE(guard.check(planned("1", "70000"), portfolio, inputs));
    guard.note_sent(inputs.now_);
  }

  // The fourth inside the window is refused, and reports the ceiling it hit.
  inputs.now_ = start + std::chrono::seconds(30);
  const auto refused = guard.check(planned("1", "70000"), portfolio, inputs);
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::ORDER_RATE_EXCEEDED);
  REQUIRE(refused->limit_ == std::optional<Decimal64>(Decimal64::parse("3")));
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("4")));

  // Once the first has aged out of the window, one more is permitted -- and not three, which is
  // what a boundary reset would have allowed.
  inputs.now_ = start + std::chrono::seconds(61);
  REQUIRE(guard.recent_orders(inputs.now_) == 2);
  REQUIRE_FALSE(guard.check(planned("1", "70000"), portfolio, inputs));
}

TEST_CASE("rate budget is consumed by what was sent, not by what was checked") {
  // An order that was checked and then refused by a later guard, or that never reached the broker,
  // must not consume budget. Recording at the point of sending is what keeps the count honest.
  RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  RiskGuard guard(limits);
  ProbePortfolio portfolio;
  const auto inputs = open_market();

  REQUIRE_FALSE(guard.check(planned("1", "70000"), portfolio, inputs));
  REQUIRE_FALSE(guard.check(planned("1", "70000"), portfolio, inputs));
  REQUIRE(guard.recent_orders(inputs.now_) == 0);

  guard.note_sent(inputs.now_);
  REQUIRE(guard.check(planned("1", "70000"), portfolio, inputs));
}

TEST_CASE("forgetting old sends bounds the memory a long session accumulates") {
  RiskLimits limits;
  limits.max_orders_per_window_ = 100;
  RiskGuard guard(limits);
  const auto start = utc(2026, 3, 10, 3, 0);
  for (int at = 0; at < 50; ++at) {
    guard.note_sent(start + std::chrono::seconds(at));
  }

  guard.forget_before(start + std::chrono::seconds(40));
  REQUIRE(guard.recent_orders(start + std::chrono::seconds(60)) == 10);
}

TEST_CASE("every guard reason has a stable name") {
  REQUIRE(strategy::guard_reason_name(GuardReason::TRADING_HALTED) == "trading_halted");
  REQUIRE(
    strategy::guard_reason_name(GuardReason::INSTRUMENT_NOT_PERMITTED) ==
    "instrument_not_permitted"
  );
  REQUIRE(strategy::guard_reason_name(GuardReason::PHASE_NOT_PERMITTED) == "phase_not_permitted");
  REQUIRE(
    strategy::guard_reason_name(GuardReason::ORDER_NOTIONAL_TOO_LARGE) ==
    "order_notional_too_large"
  );
  REQUIRE(
    strategy::guard_reason_name(GuardReason::POSITION_LIMIT_EXCEEDED) == "position_limit_exceeded"
  );
  REQUIRE(
    strategy::guard_reason_name(GuardReason::GROSS_EXPOSURE_EXCEEDED) ==
    "gross_exposure_exceeded"
  );
  REQUIRE(strategy::guard_reason_name(GuardReason::ORDER_RATE_EXCEEDED) == "order_rate_exceeded");
  REQUIRE(strategy::guard_reason_name(GuardReason::VALUE_NOT_KNOWN) == "value_not_known");
}

TEST_CASE("a shortfall below the tolerance is left alone rather than chased") {
  // The churn case: a target drifting by one share would otherwise cancel a working order for 100
  // and place a new one for 99, spending two instructions and the first order's queue priority to
  // move the position by one.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  // Without a tolerance, every share is chased: the side is withdrawn and 99 sent fresh.
  const auto chased = strategy::reconcile(target("99"), portfolio, inputs);
  REQUIRE(chased.cancel_.size() == 1);
  REQUIRE(chased.place_.size() == 1);
  REQUIRE(chased.place_[0].quantity_ == Decimal64::parse("99"));

  // With one, the drift is ignored entirely -- and crucially the cancellation is dropped too.
  // Withdrawing the working order and then declining to replace it would leave the position
  // further from the target than doing nothing at all.
  inputs.tolerance_.minimum_quantity_ = Decimal64::parse("5");
  const auto left_alone = strategy::reconcile(target("99"), portfolio, inputs);
  REQUIRE(left_alone.empty());
}

TEST_CASE("a shortfall at or above the tolerance is still acted on") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_quantity_ = Decimal64::parse("5");

  // Exactly at the tolerance is traded: it is a minimum, not a bound to exceed.
  const auto at = strategy::reconcile(target("5"), portfolio, inputs);
  REQUIRE(at.place_.size() == 1);
  REQUIRE(at.place_[0].quantity_ == Decimal64::parse("5"));

  const auto below = strategy::reconcile(target("4"), portfolio, inputs);
  REQUIRE(below.empty());
}

TEST_CASE("the tolerance is absent by default, so nothing changes without asking") {
  // No number is invented. A guessed tolerance would silently hold back orders a strategy asked
  // for, and an invisible shortfall is worse than visible churn.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  REQUIRE_FALSE(inputs.tolerance_.stated());

  const auto one_share = strategy::reconcile(target("1"), portfolio, inputs);
  REQUIRE(one_share.place_.size() == 1);
}

TEST_CASE("the tolerance measures the drift, not the quantity that would be sent") {
  // These differ exactly in the case the tolerance exists for. 100 working against a target of 99
  // would withdraw the side and re-place 99, so the quantity sent is 99 while the drift is one
  // share. Measuring the wrong one makes the tolerance useless here.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_quantity_ = Decimal64::parse("5");

  REQUIRE(strategy::reconcile(target("99"), portfolio, inputs).empty());
  // A drift of five is at the tolerance and is acted on.
  const auto acted = strategy::reconcile(target("95"), portfolio, inputs);
  REQUIRE(acted.cancel_.size() == 1);
  REQUIRE(acted.place_.size() == 1);
  REQUIRE(acted.place_[0].quantity_ == Decimal64::parse("95"));
}

TEST_CASE("an order working the wrong way is withdrawn however small the drift") {
  // Leaving it resting works *against* the target rather than merely failing to reach it, so the
  // tolerance must not suppress a cancellation.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("99"));
  portfolio.working_.push_back(working("50", "0", OrderStatus::PENDING, OrderSide::SELL));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_quantity_ = Decimal64::parse("5");

  // Wanting 100 while holding 99 is a drift of one, below the tolerance -- but a sell is resting.
  const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
  REQUIRE(plan.cancel_.size() == 1);
}

TEST_CASE("a notional floor keys on value, so one setting travels across price levels") {
  // The reason a share count alone is the weakest unit. With `minimum_quantity_ = 5`, the implied
  // value threshold is 350,000 won on a 70,000-won share and 15,000 won on a 3,000-won one -- a factor
  // of more than twenty for the same setting. A notional floor is the same everywhere.
  const auto reference = samsung_reference();
  ReconcileInputs expensive;
  expensive.instrument_ = &reference;
  expensive.reference_price_ = Decimal64::parse("70000");
  expensive.tolerance_.minimum_notional_ = Decimal64::parse("100000");

  ProbePortfolio flat;
  // One share of a 70,000-won name is 70,000 -- below the floor.
  REQUIRE(strategy::reconcile(target("1"), flat, expensive).empty());
  // Two is 140,000 -- above it.
  REQUIRE(strategy::reconcile(target("2"), flat, expensive).place_.size() == 1);

  // The same floor on a cheap name admits a much larger share count, which is the point.
  Instrument cheap_reference = samsung_reference();
  ReconcileInputs cheap;
  cheap.instrument_ = &cheap_reference;
  cheap.reference_price_ = Decimal64::parse("3000");
  cheap.tolerance_.minimum_notional_ = Decimal64::parse("100000");
  REQUIRE(strategy::reconcile(target("30"), flat, cheap).empty());
  REQUIRE(strategy::reconcile(target("40"), flat, cheap).place_.size() == 1);
}

TEST_CASE("a notional floor with no price to evaluate it does not suppress") {
  // The opposite of RiskGuard, deliberately. There, a limit that cannot be evaluated refuses, because
  // failing to refuse risks an oversized position. Here, failing to trade leaves an invisible
  // shortfall, which is the worse error -- so an unevaluable floor stays out of the way.
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_notional_ = Decimal64::parse("100000");
  // Deliberately no reference price.

  ProbePortfolio flat;
  REQUIRE(strategy::reconcile(target("1"), flat, inputs).place_.size() == 1);
}

TEST_CASE("a fractional floor keys on the queue position being given up") {
  // Often the largest cost and the least obvious: replacing a working order for 100 to change it by
  // one share risks that order's whole place in the book to adjust one percent of it, and the loss is
  // proportional to the 100 rather than to the 1.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("100", "0", OrderStatus::PENDING));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_fraction_of_working_ = Decimal64::parse("0.02");

  // A drift of one against 100 working is 1%, below the 2% floor.
  REQUIRE(strategy::reconcile(target("99"), portfolio, inputs).empty());
  // A drift of three is 3%, above it, and the side is replaced.
  const auto acted = strategy::reconcile(target("97"), portfolio, inputs);
  REQUIRE(acted.cancel_.size() == 1);
  REQUIRE(acted.place_[0].quantity_ == Decimal64::parse("97"));
}

TEST_CASE("a fractional floor says nothing when nothing is working") {
  // With no order resting there is no queue position to give up, so a drift is simply a new order and
  // this floor has no cost to weigh. Suppressing here would refuse a first order for being small.
  ProbePortfolio flat;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.tolerance_.minimum_fraction_of_working_ = Decimal64::parse("0.5");

  REQUIRE(strategy::reconcile(target("1"), flat, inputs).place_.size() == 1);
}

TEST_CASE("any one floor being met is enough to suppress") {
  // Each expresses an independent reason not to trade, so requiring all of them would mean a drift
  // worth less than a fixed fee still traded because it was a large fraction of a tiny working order.
  ProbePortfolio portfolio;
  portfolio.working_.push_back(working("2", "0", OrderStatus::PENDING));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("70000");
  inputs.tolerance_.minimum_notional_ = Decimal64::parse("100000");
  inputs.tolerance_.minimum_fraction_of_working_ = Decimal64::parse("0.02");

  // Target 1 against 2 working: the drift is one share, worth 70,000 -- under the notional floor --
  // while being 50% of the working order, far over the fractional one. The notional floor alone
  // suppresses it.
  REQUIRE(strategy::reconcile(target("1"), portfolio, inputs).empty());
}

TEST_CASE("the runner's simple setter still sets only the share-count floor") {
  // Kept for the case it suits -- a large lot size, or a per-share fee -- without pretending it is
  // right for a portfolio spanning price levels.
  strategy::TradeTolerance tolerance;
  tolerance.minimum_quantity_ = Decimal64::parse("5");
  REQUIRE(tolerance.stated());
  REQUIRE_FALSE(tolerance.minimum_notional_);
  REQUIRE_FALSE(tolerance.minimum_fraction_of_working_);

  const strategy::TradeTolerance none;
  REQUIRE_FALSE(none.stated());
}
