#include <algotradelib/strategy>
#include <algotradelib/src/runner/dispatch.hpp>

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

TEST_CASE("an unknown grid refuses every price rather than passing them all") {
  // An ETN, or a listing whose ladder this library does not have. Answering "on tick" here
  // would let an off-grid price through a check that appeared to have passed.
  const auto rule = PriceRule::unknown("no table for this instrument type");
  REQUIRE_FALSE(Price::checked(Decimal64::parse("70000"), rule));
  REQUIRE_FALSE(Price::rounded(Decimal64::parse("70000"), rule, OrderSide::BUY));
}

// ---------------------------------------------------------------------------
// SizeSpec
// ---------------------------------------------------------------------------

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

TEST_CASE("a handler is found by the name it is declared under") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::declares_on_bar<BarOnly>);
  // And the handlers it does not declare are absent, which is what makes them optional.
  STATIC_REQUIRE_FALSE(detail::declares_on_quote<BarOnly>);
  STATIC_REQUIRE_FALSE(detail::declares_on_start<BarOnly>);
  STATIC_REQUIRE(detail::declares_on_start<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_stop<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_bar<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_quote<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_fill<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_order_update<EveryHandler>);
  STATIC_REQUIRE(detail::declares_on_phase<EveryHandler>);
}

TEST_CASE("a type with no handler is rejected") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE_FALSE(detail::declares_any_handler<NoHandlers>);
  // Which is what ATL_ASSERT_STRATEGY reports on, naming the handlers it looked for.
}

TEST_CASE("a handler with the wrong event type is still the handler its name says") {
  // `on_bar` taking a QuoteEvent is declared as `on_bar`, so the runner calls it, and the call does not
  // compile: ATL_ASSERT_STRATEGY reports it in the compiler's words at `dispatch::bar`, rather than the
  // strategy reading as having no handler. (A concrete-context handler is the same: declared, and a
  // compile error under any runner whose context it does not take.)
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::declares_on_bar<WrongEventType>);
  STATIC_REQUIRE(detail::declares_any_handler<WrongEventType>);
  STATIC_REQUIRE(detail::declares_on_bar<BoundToOneContext>);
}

namespace {

/** Inherits its handler. */
struct InheritsBarOnly : BarOnly {};

/** One callable `on_fill` beside one the runner cannot call: one callable overload is enough. */
struct OverloadedFill {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx&, const strategy::Fill&) { ++fills_; }
  template <class Ctx> void on_fill(Ctx&, strategy::Fill&) {}
  int fills_{0};
};

/** Names `on_fill` as something other than a public handler. */
struct PrivateFill {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}

private:
  template <class Ctx> void on_fill(Ctx&, const strategy::Fill&) {}
};
struct FillDataMember {
  int on_fill{0};
};
struct FillStatic {
  static void on_fill() {}
};
struct FillAlias {
  using on_fill = int;
};

/** The reproduction, marked `final`: the declared-name lookup cannot reach it. */
struct FinalNonConstFill final {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx&, strategy::Fill&) { ++fills_; }
  template <class Ctx> void on_order_update(Ctx, const OrderSnapshot&) { ++updates_; }  // context by value
  int fills_{0};
  int updates_{0};
};

/** One malformed handler of each kind: each declared, and each a compile error when the runner calls it. */
struct MalformedEach {
  template <class Ctx> void on_start(Ctx&, int) {}
  template <class Ctx> void on_stop() {}
  template <class Ctx> void on_bar(Ctx&, strategy::BarEvent&) {}
  template <class Ctx> void on_quote(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx&, strategy::Fill&) {}
  template <class Ctx> void on_order_update(Ctx&, const strategy::QuoteEvent&) {}
  template <class Ctx> void on_phase(Ctx&) {}
};

/** Events by value: callable, since a copy of a `const` event is a valid argument. */
struct EventsByValue {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_order_update(Ctx&, OrderSnapshot) { ++updates_; }
  template <class Ctx> void on_phase(Ctx&, strategy::MarketPhase) { ++phases_; }
  int updates_{0};
  int phases_{0};
};

/** The phase as a `const` reference, which is the call the runner makes. */
struct PhaseByConstRef {
  template <class Ctx> void on_phase(Ctx&, const strategy::MarketPhase&) { ++phases_; }
  int phases_{0};
};

} // namespace

namespace {

/** Decides on whole periods. */
struct SliceOnly {
  std::size_t slices_{0};
  template <class Ctx> void on_bars(Ctx&, const strategy::BarSlice&) { ++slices_; }
};

/** `on_bars` inherited publicly, which is declared and callable. */
struct InheritsSliceOnly : SliceOnly {};

} // namespace

ATL_ASSERT_STRATEGY(SliceOnly, MinimalContext);
ATL_ASSERT_STRATEGY(SliceOnly, FullContext);
ATL_ASSERT_STRATEGY(InheritsSliceOnly, MinimalContext);
ATL_ASSERT_STRATEGY(InheritsBarOnly, MinimalContext);
ATL_ASSERT_STRATEGY(OverloadedFill, MinimalContext);
ATL_ASSERT_STRATEGY(EventsByValue, MinimalContext);
ATL_ASSERT_STRATEGY(PhaseByConstRef, MinimalContext);

TEST_CASE("a slice handler is found by name and called through dispatch") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::declares_on_bars<SliceOnly>);
  STATIC_REQUIRE(detail::declares_on_bars<InheritsSliceOnly>);
  STATIC_REQUIRE_FALSE(detail::declares_on_bar<SliceOnly>);
  STATIC_REQUIRE_FALSE(detail::declares_on_bars<BarOnly>);

  SliceOnly strategy;
  MinimalContext context;
  strategy::BarSlice slice;
  strategy::dispatch::bars(strategy, context, slice);
  CHECK(strategy.slices_ == 1);
  // A strategy without `on_bars` is skipped, as for every other handler.
  BarOnly bars_only;
  strategy::dispatch::bars(bars_only, context, slice);
}

TEST_CASE("a slice finds an instrument's bar and nothing for one it lacks") {
  strategy::BarSlice slice;
  strategy::BarEvent event;
  event.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  slice.bars_.push_back(event);
  REQUIRE(slice.find(event.instrument_) != nullptr);
  CHECK(slice.find(event.instrument_)->instrument_.symbol_ == "005930");
  CHECK(slice.find(InstrumentId{Country::KR, Venue::COMBINED, "000660"}) == nullptr);
}

TEST_CASE("a declared handler is found by name, whatever its signature") {
  namespace detail = strategy::strategy_detail;
  // A template, a concrete one, an inherited one, an overload set, a private one.
  STATIC_REQUIRE(detail::declares_on_bar<BarOnly>);
  STATIC_REQUIRE(detail::declares_on_bar<BoundToOneContext>);
  STATIC_REQUIRE(detail::declares_on_bar<InheritsBarOnly>);
  STATIC_REQUIRE(detail::declares_on_fill<OverloadedFill>);
  STATIC_REQUIRE(detail::declares_on_fill<PrivateFill>);
  // A member of that name of any other kind is declared too, so the runner calls it and the call does
  // not compile.
  STATIC_REQUIRE(detail::declares_on_fill<FillDataMember>);
  STATIC_REQUIRE(detail::declares_on_fill<FillStatic>);
  STATIC_REQUIRE(detail::declares_on_fill<FillAlias>);
  // Absent, and every handler of a strategy that names none.
  STATIC_REQUIRE_FALSE(detail::declares_on_fill<BarOnly>);
  STATIC_REQUIRE_FALSE(detail::declares_on_start<EventsByValue>);
  STATIC_REQUIRE_FALSE(detail::declares_on_fill<int>);
  // **The stated gap**: a `final` class cannot be looked up this way and reads as declaring nothing.
  STATIC_REQUIRE_FALSE(detail::declares_on_fill<FinalNonConstFill>);
  STATIC_REQUIRE_FALSE(detail::declares_on_bar<FinalNonConstFill>);
}

TEST_CASE("each malformed handler is still declared, so the runner calls it") {
  // A strategy whose handler the call does not fit cannot be compiled in a test; the probes in
  // tests/compile-fail/test-strategy-refuses-*.cpp show the compiler refusing it. Pinned here: none of
  // these reads as absent.
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(detail::declares_on_start<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_stop<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_bar<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_quote<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_fill<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_order_update<MalformedEach>);
  STATIC_REQUIRE(detail::declares_on_phase<MalformedEach>);
}

// The probe contexts are not copyable either, so a by-value handler is rejected as the runner rejects it.
static_assert(!std::is_copy_constructible_v<MinimalContext>);
static_assert(!std::is_copy_assignable_v<FullContext>);
static_assert(std::is_move_constructible_v<MinimalContext>);

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
  // The probe learns a resting convention from how the held-to-sellable *distance* moves, which only
  // means something when both numbers describe the same shares. So a fixture for it has to state a
  // coverage, exactly as a broker publishing the figure does; the unstated case is its own test.
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
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

TEST_CASE("two rows of one instrument agree on a sellable figure written at different scales") {
  // KIS pages can repeat a row; "70" and "70.0" are one figure, not a contradiction that refuses the sell.
  auto repeated = observed("70", "70", "0");
  auto second = repeated.positions_.front();
  second.sellable_quantity_ = Decimal64::parse("70.0");
  repeated.positions_.push_back(second);
  const auto reading = repeated.sellable_reading_for(samsung());
  REQUIRE(reading.quantity_);
  CHECK(reading.basis_.is_enforceable());

  // Figures that differ in value still make the basis unestablished.
  auto contradicted = observed("70", "70", "0");
  auto other = contradicted.positions_.front();
  other.sellable_quantity_ = Decimal64::parse("71");
  contradicted.positions_.push_back(other);
  CHECK_FALSE(contradicted.sellable_reading_for(samsung()).basis_.is_enforceable());
}

TEST_CASE("the probe learns nothing from a figure of unknown coverage") {
  // **Why this gate exists.** The probe reads the distance between what is held and what is sellable.
  // If `rmnd_qty` is account-wide while `trde_able_qty` covers one venue -- which is precisely what an
  // unestablished coverage means at Kiwoom -- that distance is a venue split, and a position moving
  // between venues would be read as a broker's resting convention. A confident wrong verdict about
  // whether a figure is already net is worse than no verdict, because the guard's arithmetic follows it.
  Probe probe;

  // The same two readings that yield GROSS_OF_RESTING below, with the coverage unstated.
  auto first = observed("100", "100", "0");
  first.sellable_basis_ = SellableBasis{};
  auto second = observed("100", "100", "30");
  second.sellable_basis_ = SellableBasis{};

  probe.observe(first);
  probe.observe(second);

  REQUIRE(probe.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(probe.overall().total() == 0);
  REQUIRE(probe.evidence(samsung()).gross_ == 0);
  REQUIRE(probe.evidence(samsung()).net_ == 0);

  // A partial coverage is refused for the same reason and is not a milder case: a figure covering
  // part of the holding makes the distance a partition rather than a reservation.
  auto partial_first = observed("100", "100", "0");
  partial_first.sellable_basis_.coverage_ = SellableBasis::Coverage::PARTIAL;
  auto partial_second = observed("100", "100", "30");
  partial_second.sellable_basis_.coverage_ = SellableBasis::Coverage::PARTIAL;

  Probe partial_probe;
  partial_probe.observe(partial_first);
  partial_probe.observe(partial_second);
  REQUIRE(partial_probe.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(partial_probe.overall().total() == 0);

  // The positive control: the identical readings *with* a coverage stated do reach a verdict, so this
  // test cannot pass because the probe stopped working.
  Probe stated;
  stated.observe(observed("100", "100", "0"));
  stated.observe(observed("100", "100", "30"));
  REQUIRE(stated.verdict() == strategy::SellableConvention::GROSS_OF_RESTING);

  // **And a gap drops the baseline rather than spanning it**, which is the half that skipping alone
  // does not give: a declared reading, then an undeclared one, then a declared one
  // would otherwise be judged against the reading from *before* the gap -- so the distance appears to
  // move by everything that happened while the probe was not looking, and it records a confident
  // verdict about a window it never observed.
  Probe across_a_gap;
  across_a_gap.observe(observed("100", "100", "0"));

  auto unobservable = observed("100", "70", "0");
  unobservable.sellable_basis_ = SellableBasis{};
  across_a_gap.observe(unobservable);

  across_a_gap.observe(observed("100", "70", "30"));

  // Nothing is concluded: the only comparable pair is separated by a reading nobody could interpret.
  REQUIRE(across_a_gap.verdict() == strategy::SellableConvention::UNDETERMINED);
  REQUIRE(across_a_gap.overall().total() == 0);
}

TEST_CASE("a fill in the same window is not evidence of anything") {
  Probe probe;
  probe.observe(observed("100", "100", "0"));
  // The holding moved, so the sell filled somewhere in here and both numbers moved with it.
  probe.observe(observed("70", "70", "30"));

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

/** A holding the broker states as sellable in full, which the guard checks a sell against by default. */
HoldingPosition sellable_holding(const char* quantity) {
  auto position = holding(quantity);
  position.sellable_quantity_ = position.quantity_;
  return position;
}

/** The basis every shipped adapter declares: the whole holding, resting sells deducted by this library. */
SellableBasis whole_holding_basis() {
  return SellableBasis{SellableBasis::Coverage::WHOLE_HOLDING, SellableBasis::RestingTreatment::DEDUCT, false};
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

TEST_CASE("a position whose rows will not add up is rejected, not read as flat") {
  // The behaviour change that came with the netting, pinned because it is one. `value_or(zero)` on
  // a failed sum would read a broken listing as *flat* and order the entire target -- the largest
  // order the strategy could ask for, sized from a snapshot the library just failed to understand.
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("9223372036854775807"));
  portfolio.positions_.push_back(holding("9223372036854775807"));
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  const auto plan = strategy::reconcile(target("0"), portfolio, inputs);
  REQUIRE(plan.place_.empty());
  REQUIRE(plan.rejected_.size() == 1);
  REQUIRE(plan.rejected_[0].reason_ == strategy::RejectReason::NOT_REPRESENTABLE);
}

TEST_CASE("over-ordering through reserved rows that may have filled re-sends only what they do not cover") {
  // A reserved row is the part of an order holdings have not shown, and it has often filled.
  // Its withdrawal is then refused, so a whole remainder sent beside it traded twice.
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reserved_rows_are_unconfirmed_ = true;

  SECTION("a reserved surplus is withdrawn and nothing is sent beside it") {
    ProbePortfolio portfolio;
    portfolio.positions_.push_back(holding("70"));
    auto reserved = working("70", "0", OrderStatus::PENDING);
    reserved.reserved_ = true;
    portfolio.working_.push_back(reserved);
    const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
    CHECK(plan.cancel_.size() == 1);
    CHECK(plan.place_.empty());
  }

  SECTION("a listed surplus beside a smaller reserved row is re-sent net of the reserved one") {
    ProbePortfolio portfolio;
    auto reserved = working("20", "0", OrderStatus::PENDING);
    reserved.reserved_ = true;
    portfolio.working_.push_back(reserved);
    portfolio.working_.push_back(working("130", "0", OrderStatus::PENDING));
    const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
    CHECK(plan.cancel_.size() == 2);
    REQUIRE(plan.place_.size() == 1);
    CHECK(plan.place_[0].quantity_ == Decimal64::parse("80"));
  }

  SECTION("a reserved surplus on the sell side, the same") {
    ProbePortfolio portfolio;
    portfolio.positions_.push_back(holding("100"));
    auto reserved = working("80", "0", OrderStatus::PENDING, OrderSide::SELL);
    reserved.reserved_ = true;
    portfolio.working_.push_back(reserved);
    const auto plan = strategy::reconcile(target("40"), portfolio, inputs);
    CHECK(plan.cancel_.size() == 1);
    CHECK(plan.place_.empty());
  }

  SECTION("off, as for a replay, the whole remainder is sent as before") {
    inputs.reserved_rows_are_unconfirmed_ = false;
    ProbePortfolio portfolio;
    auto reserved = working("150", "0", OrderStatus::PENDING);
    reserved.reserved_ = true;
    portfolio.working_.push_back(reserved);
    const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
    CHECK(plan.cancel_.size() == 1);
    REQUIRE(plan.place_.size() == 1);
    CHECK(plan.place_[0].quantity_ == Decimal64::parse("100"));
  }
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

TEST_CASE("a band with no tick on the cautious side of a clamped limit refuses it") {
  // At 2,000-5,000 won the KRX tick is 5. A band of 2,002-2,004 holds no tick: a buy clamped down to
  // 2,004 rounds down to 2,000, below the band, and a sell clamped up to 2,002 rounds up to 2,005,
  // above it. Either order would be refused by the exchange; the plan refuses it first.
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  PriceLimits limits;
  limits.lower_ = Decimal64::parse("2002");
  limits.upper_ = Decimal64::parse("2004");
  inputs.limits_ = limits;
  const auto price = [](const char* value) {
    auto checked = strategy::Price::checked(Decimal64::parse(value), krx());
    REQUIRE(checked);
    return *checked;
  };
  const auto refused = [](const auto& plan) {
    return plan.place_.empty() && plan.rejected_.size() == 1 &&
      plan.rejected_[0].reason_ == RejectReason::NO_PERMITTED_PRICE;
  };

  SECTION("a target") {
    ProbePortfolio flat;
    CHECK(refused(strategy::reconcile(target("10", price("2010")), flat, inputs)));
    ProbePortfolio held;
    held.positions_.push_back(holding("100"));
    CHECK(refused(strategy::reconcile(target("0", price("1995")), held, inputs)));
    CHECK(strategy::reject_reason_name(RejectReason::NO_PERMITTED_PRICE) == "no_permitted_price");
  }

  SECTION("an intent") {
    ProbePortfolio portfolio;
    const auto ten = SizeSpec::quantity(Decimal64::parse("10"));
    CHECK(refused(strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::BUY, ten, price("2010")), portfolio, inputs)));
    CHECK(refused(strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::SELL, ten, price("1995")), portfolio, inputs)));
  }

  SECTION("a limit the strategy set beyond the band is still left alone") {
    ProbePortfolio portfolio;
    const auto ten = SizeSpec::quantity(Decimal64::parse("10"));
    const auto buy = strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::BUY, ten, price("1995")), portfolio, inputs);
    REQUIRE(buy.place_.size() == 1);
    CHECK(buy.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("1995")));
    const auto sell = strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::SELL, ten, price("2010")), portfolio, inputs);
    REQUIRE(sell.place_.size() == 1);
    CHECK(sell.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("2010")));
  }

  SECTION("a band holding a tick keeps the clamped and rounded price") {
    limits.lower_ = Decimal64::parse("2002");
    limits.upper_ = Decimal64::parse("2007");
    inputs.limits_ = limits;
    ProbePortfolio portfolio;
    const auto ten = SizeSpec::quantity(Decimal64::parse("10"));
    const auto buy = strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::BUY, ten, price("2010")), portfolio, inputs);
    REQUIRE(buy.place_.size() == 1);
    CHECK(buy.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("2005")));
    const auto sell = strategy::reconcile(OrderIntent::limit(samsung(), OrderSide::SELL, ten, price("1995")), portfolio, inputs);
    REQUIRE(sell.place_.size() == 1);
    CHECK(sell.place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("2005")));
  }
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

TEST_CASE("an intent sized against a price that is not positive is refused, not sized negative") {
  // A notional of a million at -10 would be -100,000 shares: a buy with a negative quantity. A price is
  // negative only from a damaged response or a market where prices can be; neither sizes an order.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("-10");
  inputs.portfolio_value_ = Decimal64::parse("10000000");

  const auto notional = strategy::reconcile(
    OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::notional(Money{Decimal64::parse("1000000"), Currency{"KRW"}})),
    portfolio, inputs
  );
  REQUIRE(notional.place_.empty());
  REQUIRE(notional.rejected_[0].reason_ == RejectReason::NO_PRICE_AVAILABLE);

  // Called directly, a weight is sized as a trade too.
  const auto weight = strategy::reconcile(
    OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::target_weight(Decimal64::parse("0.25"))), portfolio, inputs
  );
  REQUIRE(weight.place_.empty());
  REQUIRE(weight.rejected_[0].reason_ == RejectReason::NO_PRICE_AVAILABLE);
}

TEST_CASE("an intent whose size is negative places nothing") {
  // A side and a magnitude: a negative magnitude is a sign error in the strategy, and an order of -5
  // shares is not a sell of 5.
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("70000");
  inputs.portfolio_value_ = Decimal64::parse("10000000");

  const auto quantity = strategy::reconcile(
    OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("-5"))), portfolio, inputs
  );
  REQUIRE(quantity.place_.empty());
  REQUIRE(quantity.rejected_[0].reason_ == RejectReason::NEGATIVE_SIZE);

  const auto notional = strategy::reconcile(
    OrderIntent::market(samsung(), OrderSide::SELL, SizeSpec::notional(Money{Decimal64::parse("-1000000"), Currency{"KRW"}})),
    portfolio, inputs
  );
  REQUIRE(notional.place_.empty());
  REQUIRE(notional.rejected_[0].reason_ == RejectReason::NEGATIVE_SIZE);

  // A weight states a holding, and a negative holding is what `weight_target` already refuses.
  const auto weight = strategy::reconcile(
    OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::target_weight(Decimal64::parse("-0.25"))), portfolio, inputs
  );
  REQUIRE(weight.place_.empty());
  REQUIRE(weight.rejected_[0].reason_ == RejectReason::UNSUPPORTED_WEIGHT);
}

TEST_CASE("a weight states a holding, and what a holding cannot state is refused", "[weights]") {
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;
  inputs.reference_price_ = Decimal64::parse("70000");
  inputs.portfolio_value_ = Decimal64::parse("10000000");
  const auto quarter = SizeSpec::target_weight(Decimal64::parse("0.25"));

  // A quarter of ten million at 70,000 is 35 shares, as a target.
  const auto held = strategy::weight_target(OrderIntent::market(samsung(), OrderSide::BUY, quarter), inputs);
  REQUIRE(held.target_.has_value());
  CHECK(held.target_->target_quantity_ == Decimal64::parse("35"));

  const auto refused = [&](const OrderIntent& intent) {
    const auto out = strategy::weight_target(intent, inputs);
    return !out.target_ && out.refused_ == RejectReason::UNSUPPORTED_WEIGHT;
  };
  // A sell would become a buy if its side were discarded.
  CHECK(refused(OrderIntent::market(samsung(), OrderSide::SELL, quarter)));
  CHECK(refused(OrderIntent::market(
    samsung(), OrderSide::BUY, SizeSpec::target_weight(Decimal64::parse("-0.25")))));
  CHECK(refused(OrderIntent::market(samsung(), OrderSide::BUY, quarter).with_client_order_id("k-1")));
  const auto trigger = strategy::Price::checked(Decimal64::parse("69000"), krx());
  const auto limit = strategy::Price::checked(Decimal64::parse("68900"), krx());
  REQUIRE(trigger);
  REQUIRE(limit);
  CHECK(refused(OrderIntent::stop_limit(samsung(), OrderSide::BUY, quarter, *trigger, *limit)));

  CHECK(refused(OrderIntent::limit(samsung(), OrderSide::BUY, quarter, *limit,
                                   TimeInForce::IMMEDIATE_OR_CANCEL)));

  // A limit prices the size: a quarter of ten million at 68,900 is 36 shares.
  const auto limited =
    strategy::weight_target(OrderIntent::limit(samsung(), OrderSide::BUY, quarter, *limit), inputs);
  REQUIRE(limited.target_.has_value());
  CHECK(limited.target_->target_quantity_ == Decimal64::parse("36"));

  // A weight of zero is a target of zero: it sells the holding, as a target of zero does.
  const auto none = strategy::weight_target(
    OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::target_weight(Decimal64::parse("0"))),
    inputs);
  REQUIRE(none.target_.has_value());
  CHECK(none.target_->target_quantity_ == Decimal64::parse("0"));

  // A value of zero or less is not one to be a fraction of: refused, never a liquidation or a short.
  for (const char* value : {"0", "-5000000"}) {
    auto broke = inputs;
    broke.portfolio_value_ = Decimal64::parse(value);
    const auto out = strategy::weight_target(OrderIntent::market(samsung(), OrderSide::BUY, quarter), broke);
    CHECK_FALSE(out.target_.has_value());
    CHECK(out.refused_ == RejectReason::NO_PORTFOLIO_VALUE);
  }

  // A value stated in another currency is not the instrument's value.
  inputs.portfolio_value_currency_ = Currency{"USD"};
  const auto crossed = strategy::weight_target(OrderIntent::market(samsung(), OrderSide::BUY, quarter), inputs);
  CHECK_FALSE(crossed.target_.has_value());
  CHECK(crossed.refused_ == RejectReason::NO_PORTFOLIO_VALUE);
  inputs.portfolio_value_currency_ = Currency{"KRW"};
  CHECK(strategy::weight_target(OrderIntent::market(samsung(), OrderSide::BUY, quarter), inputs).target_);
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

  // The same order priced in dollars has no answer, and the library's refusal to convert is why
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

TEST_CASE("a split holding cannot borrow one lot's declaration for another lot's figure") {
  // **The first-row pattern this repository has paid for before.** The
  // figure and its basis were two accessors, each walking the rows and each taking the first match on
  // its own criterion: the figure from the first row of the instrument, the basis from the first row
  // carrying an override. For a holding split across two lots those are different rows.
  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard guard(limits);

  // Lot one: a figure of 100 and nothing said about it.
  ProbePortfolio split;
  HoldingPosition first;
  first.instrument_ = samsung();
  first.quantity_ = Decimal64::parse("100");
  first.sellable_quantity_ = Decimal64::parse("100");
  split.positions_.push_back(first);

  // Lot two: a declared basis, and the declaration answered thirty.
  HoldingPosition second;
  second.instrument_ = samsung();
  second.quantity_ = Decimal64::parse("30");
  second.sellable_quantity_ = Decimal64::parse("30");
  second.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  split.positions_.push_back(second);

  // A sell of 100 must **not** pass: the only number anyone characterized is thirty, and the hundred
  // sitting on the first row is uninterpretable. Under the two-accessor version this was admitted.
  const auto refused = guard.check(planned("100", "70000", OrderSide::SELL), split, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::SELLABLE_BASIS_UNESTABLISHED);

  // **Both row orders**, because a rule that reads the first row is right by accident in one of them.
  ProbePortfolio reversed;
  reversed.positions_.push_back(second);
  reversed.positions_.push_back(first);
  const auto also = guard.check(planned("100", "70000", OrderSide::SELL), reversed, open_market());
  REQUIRE(also);
  REQUIRE(also->reason_ == GuardReason::SELLABLE_BASIS_UNESTABLISHED);

  // Even a sell of thirty refuses, which is the deliberately conservative half: two lots described
  // differently cannot be reconciled by this library, so nothing is enforced against either.
  REQUIRE(guard.check(planned("30", "70000", OrderSide::SELL), split, open_market()));

  // **Two lots that agree on a basis but disagree on the figure also refuse**; a fixture once asserted the
  // opposite. If both rows say `WHOLE_HOLDING`, both claim to state the limit for the *same* shares, so 100
  // and 30 are a contradiction rather than two facts, and taking the first row's number would put a sell above
  // the real limit on the wire whenever the larger row comes first. Reachable from the shipped KIS parser,
  // which concatenates every `output1` row across pages without checking for a repeated `pdno`.
  ProbePortfolio conflicting = split;
  conflicting.positions_[0].sellable_basis_ = second.sellable_basis_;
  const auto contradiction =
    guard.check(planned("100", "70000", OrderSide::SELL), conflicting, open_market());
  REQUIRE(contradiction);
  REQUIRE(contradiction->reason_ == GuardReason::SELLABLE_BASIS_UNESTABLISHED);

  // Both orders again, since a first-row rule is right by accident in one of them.
  ProbePortfolio conflicting_reversed;
  conflicting_reversed.positions_.push_back(conflicting.positions_[1]);
  conflicting_reversed.positions_.push_back(conflicting.positions_[0]);
  REQUIRE(guard.check(planned("30", "70000", OrderSide::SELL), conflicting_reversed, open_market()));

  // **The positive control, now coherent**: two lots described the same way *and* stating the same
  // limit. That is what a whole-holding figure repeated across rows actually looks like, and it
  // enforces exactly as a single row would. Without this the test would also pass if a split holding
  // simply always refused, which is the failure mode of being too strict.
  ProbePortfolio agreeing = conflicting;
  agreeing.positions_[1].sellable_quantity_ = Decimal64::parse("100");
  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::SELL), agreeing, open_market()));
  REQUIRE(guard.check(planned("101", "70000", OrderSide::SELL), agreeing, open_market()));

  // And a lot carrying no figure beside one that does is not a conflict -- it is the only figure
  // anyone stated, so it is the reading rather than a refusal.
  ProbePortfolio one_silent = agreeing;
  one_silent.positions_[0].sellable_quantity_.reset();
  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::SELL), one_silent, open_market()));
}

TEST_CASE("a figure covering part of a holding refuses, and says so distinguishably") {
  // `PARTIAL` is not a synonym for unestablished: one means nobody looked, the other means somebody
  // looked and found an answer that cannot bound a sale of the whole position. A venue-scoped
  // quantity is the case that matters -- Kiwoom's, if `dmst_stex_tp` turns out to filter.
  RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  const RiskGuard guard(limits);

  ProbePortfolio partial;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  position.sellable_quantity_ = Decimal64::parse("100");
  partial.positions_.push_back(position);
  partial.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::PARTIAL,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };

  REQUIRE_FALSE(partial.sellable_basis_.is_enforceable());
  const auto refused = guard.check(planned("10", "70000", OrderSide::SELL), partial, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::SELLABLE_BASIS_UNESTABLISHED);

  // Distinguishable from an absent basis by the value itself rather than by both being refusals,
  // which is what lets a caller tell "unasked" from "asked and partial".
  REQUIRE(sellable_coverage_name(partial.sellable_basis_.coverage_) == "partial");
  REQUIRE(sellable_coverage_name(SellableBasis{}.coverage_) == "unestablished");
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

namespace {

/** Inputs with the market shut, which is what the closing exception below is about. */
GuardInputs shut_market() {
  auto inputs = open_market();
  inputs.phase_ = strategy::MarketPhase::CLOSED;
  return inputs;
}

/** Whether `guard` refused `order` because of the phase, specifically. */
bool refused_for_phase(
  const RiskGuard& guard, const PlannedOrder& order, const ProbePortfolio& portfolio
) {
  const auto refused = guard.check(order, portfolio, shut_market());
  return refused && refused->reason_ == GuardReason::PHASE_NOT_PERMITTED;
}

} // namespace

TEST_CASE("a sell that has finished does not hold back the next one") {
  // A terminal row is not outstanding, and counting it would refuse an exit because of an order that
  // already ended. **The row is `CANCELED` with a non-zero remainder, and that is the whole point.**
  // An earlier version used a `FILLED` row of 100 filled 100, whose remaining quantity is zero --
  // so an implementation that dropped the `order_is_open` check entirely still passed it, and the
  // test proved nothing. A cancelled sell of 100 with nothing filled has
  // 100 remaining, so counting it would take the whole holding and refuse the exit.
  const RiskGuard guard;
  ProbePortfolio portfolio;
  portfolio.sellable_basis_ = whole_holding_basis();
  portfolio.positions_.push_back(sellable_holding("100"));
  portfolio.working_.push_back(working("100", "0", OrderStatus::CANCELED, OrderSide::SELL));

  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::SELL), portfolio, shut_market()));
}

TEST_CASE("a working sell reporting more filled than ordered refuses rather than counting zero") {
  // `Portfolio::working_quantity` *skips* such a row, which is right for a figure a strategy reads
  // and wrong here: skipping it understates what is already committed to closing and would admit an
  // order that carries the position through flat. A contradictory snapshot cannot establish that
  // anything is an exit.
  const RiskGuard guard;
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  portfolio.working_.push_back(working("60", "80", OrderStatus::PARTIAL_FILLED, OrderSide::SELL));

  REQUIRE(refused_for_phase(guard, planned("10", "70000", OrderSide::SELL), portfolio));
}

TEST_CASE("a buy that covers a short is permitted, and one that overshoots is not") {
  // The mirror. A rule written only for long positions would refuse both of these, and a shipped
  // exit would work in one direction.
  const RiskGuard guard;
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("-100"));

  REQUIRE_FALSE(guard.check(planned("100", "70000", OrderSide::BUY), portfolio, shut_market()));
  REQUIRE(refused_for_phase(guard, planned("101", "70000", OrderSide::BUY), portfolio));
}

TEST_CASE("an order whose quantity is not positive is never treated as closing") {
  // `PlannedOrder::quantity_` is a public field and `SizeSpec::quantity` accepts any decimal, so a
  // negative size is constructible; `reconcile(OrderIntent)` rejects one only once it *rounds to
  // zero*. Without this check a sell of -1 against a long of 100 satisfies `committed <= |held|`
  // and an inconsistent order relaxes a restriction, which is the one direction this must not fail
  // in.
  const RiskGuard guard;
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));

  REQUIRE(refused_for_phase(guard, planned("-1", "70000", OrderSide::SELL), portfolio));
  REQUIRE(refused_for_phase(guard, planned("0", "70000", OrderSide::SELL), portfolio));
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

TEST_CASE("the position ceiling limits a short as firmly as a long") {
  // The ceiling is on magnitude. A limit that only counted longs would let a strategy build an
  // unbounded short while appearing to be risk-limited.
  RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("100");
  // A short book: the sellable check is off, as an account that can borrow (or a short replay) sets it.
  limits.enforce_sellable_quantity_ = false;
  const RiskGuard guard(limits);
  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("-95"));

  const auto refused =
    guard.check(planned("10", "70000", OrderSide::SELL), portfolio, open_market());
  REQUIRE(refused);
  REQUIRE(refused->reason_ == GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(refused->attempted_ == std::optional<Decimal64>(Decimal64::parse("105")));
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

TEST_CASE("an ordinary portfolio is valued and permitted, not refused by the tightening") {
  // **The property the added refusals put at risk.** Each added a refusal -- an incomplete
  // basis, a non-positive price, an inconsistent order row -- and a limit that refuses ordinary
  // trading is a worse outcome than the permissive defect it replaced. This is an ordinary
  // afternoon: three positions, one partially filled working buy, one resting sell, and a lot left
  // behind by a closed position that nothing registered.
  const auto hynix = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  const auto naver = InstrumentId{Country::KR, Venue::COMBINED, "035420"};
  const auto closed = InstrumentId{Country::KR, Venue::COMBINED, "051910"};

  ProbePortfolio portfolio;
  portfolio.sellable_basis_ = whole_holding_basis();
  portfolio.positions_.push_back(sellable_holding("100"));
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

TEST_CASE("an exposure basis that does not describe the portfolio refuses") {
  // No silent understatement, and the reason the basis replaced an optional total. An instrument left out of the
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
  // and the library's refusal to add currencies without a rate the caller can see.
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
  // **The last three were once missing.** A test called "every guard
  // reason" that covers most of them is worse than one that names its subset, because the gap is
  // exactly where a new enumerator slips through unnamed -- and `guard_reason_name`'s trailing
  // `return "unknown"` means an unhandled reason compiles and reports nothing useful rather than
  // failing to build. The library's test and example builds now make a missing case an error
  // (`-Werror=switch`); this list still checks the names themselves.
  REQUIRE(
    strategy::guard_reason_name(GuardReason::QUANTITY_NOT_SELLABLE) == "quantity_not_sellable"
  );
  REQUIRE(
    strategy::guard_reason_name(GuardReason::SELLABLE_BASIS_UNESTABLISHED) ==
    "sellable_basis_unestablished"
  );
  REQUIRE(
    strategy::guard_reason_name(GuardReason::INSUFFICIENT_BUYING_POWER) ==
    "insufficient_buying_power"
  );
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

TEST_CASE("a stop-limit intent carries both prices and nothing else carries a trigger", "[trigger]") {
  const auto trigger = Price::checked(Decimal64::parse("69500"), krx());
  const auto limit = Price::checked(Decimal64::parse("69000"), krx());
  REQUIRE(trigger);
  REQUIRE(limit);
  const auto stop = OrderIntent::stop_limit(
    samsung(), OrderSide::SELL, SizeSpec::quantity(Decimal64::parse("10")), *trigger, *limit
  );
  CHECK(stop.type() == OrderType::STOP_LIMIT);
  CHECK(stop.trigger() == trigger);
  CHECK(stop.limit() == limit);
  CHECK_FALSE(OrderIntent::limit(
    samsung(), OrderSide::SELL, SizeSpec::quantity(Decimal64::parse("10")), *limit
  ).trigger());
  CHECK_FALSE(OrderIntent::market(
    samsung(), OrderSide::SELL, SizeSpec::quantity(Decimal64::parse("10"))
  ).trigger());
}

TEST_CASE("a stop intent reconciles into a stop-limit order with its trigger untouched", "[trigger]") {
  ProbePortfolio portfolio;
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  SECTION("the limit sizes a notional and the trigger is carried exactly") {
    const auto intent = OrderIntent::stop_limit(
      samsung(),
      OrderSide::SELL,
      SizeSpec::notional(Money{Decimal64::parse("1380000"), Currency{"KRW"}}),
      *Price::checked(Decimal64::parse("69500"), krx()),
      *Price::checked(Decimal64::parse("69000"), krx())
    );
    const auto plan = strategy::reconcile(intent, portfolio, inputs);
    REQUIRE(plan.rejected_.empty());
    REQUIRE(plan.place_.size() == 1);
    const auto& order = plan.place_[0];
    CHECK(order.type_ == OrderType::STOP_LIMIT);
    // 1,380,000 at the 69,000 limit is 20 shares; at the trigger it would be 19.
    CHECK(order.quantity_ == Decimal64::parse("20"));
    CHECK(order.price_ == std::optional<Decimal64>(Decimal64::parse("69000")));
    CHECK(order.trigger_price_ == std::optional<Decimal64>(Decimal64::parse("69500")));
  }

  SECTION("a trigger off the instrument's own grid is refused, never rounded") {
    // Built against a grid that is not the instrument's, which is the only way a `Price` can be off
    // the grid reconciliation checks: 69,550 is not on KRX's 100-won step at that level.
    const auto off = Price::checked(Decimal64::parse("69550"), PriceRule::unconstrained());
    REQUIRE(off);
    const auto intent = OrderIntent::stop_limit(
      samsung(),
      OrderSide::SELL,
      SizeSpec::quantity(Decimal64::parse("10")),
      *off,
      *Price::checked(Decimal64::parse("69000"), krx())
    );
    const auto plan = strategy::reconcile(intent, portfolio, inputs);
    CHECK(plan.place_.empty());
    REQUIRE(plan.rejected_.size() == 1);
    CHECK(plan.rejected_[0].reason_ == strategy::RejectReason::TRIGGER_OFF_GRID);
    CHECK(strategy::reject_reason_name(strategy::RejectReason::TRIGGER_OFF_GRID) == "trigger_off_grid");
  }

  SECTION("a limit or market intent plans no trigger") {
    const auto plan = strategy::reconcile(
      OrderIntent::market(samsung(), OrderSide::BUY, SizeSpec::quantity(Decimal64::parse("1"))),
      portfolio,
      inputs
    );
    REQUIRE(plan.place_.size() == 1);
    CHECK_FALSE(plan.place_[0].trigger_price_);
  }
}

TEST_CASE("a target that needs a trade withdraws a resting stop first", "[trigger]") {
  // Each sequence is one a strategy that protects a holding with a stop and states targets meets.
  const auto reference = samsung_reference();
  ReconcileInputs inputs;
  inputs.instrument_ = &reference;

  ProbePortfolio portfolio;
  portfolio.positions_.push_back(holding("100"));
  auto stop = working("100", "0", OrderStatus::PENDING, OrderSide::SELL);
  stop.ref_ = OrderRef<strategy::ProbeBroker>{"STOP-1", ""};
  stop.order_.type_ = OrderType::STOP_LIMIT;
  stop.order_.price_ = Decimal64::parse("69000");
  stop.order_.trigger_price_ = Decimal64::parse("69500");
  portfolio.working_.push_back(stop);

  const auto withdraws = [&](const char* quantity) {
    const auto plan = strategy::reconcile(target(quantity), portfolio, inputs);
    REQUIRE(plan.cancel_.size() == 1);
    CHECK(plan.cancel_[0].primary_ == "STOP-1");
    CHECK(plan.place_.empty());
    REQUIRE(plan.rejected_.size() == 1);
    CHECK(plan.rejected_[0].reason_ == strategy::RejectReason::STOP_ORDER_RESTING);
  };

  SECTION("holding what is held keeps the protective stop and withdraws the rest") {
    // Before: every open order on the instrument was cancelled as unwanted, the stop included.
    auto buy = working("5", "0", OrderStatus::PENDING, OrderSide::BUY);
    buy.ref_ = OrderRef<strategy::ProbeBroker>{"BUY-1", ""};
    portfolio.working_.push_back(buy);
    const auto plan = strategy::reconcile(target("100"), portfolio, inputs);
    CHECK(plan.rejected_.empty());
    CHECK(plan.place_.empty());
    REQUIRE(plan.cancel_.size() == 1);
    CHECK(plan.cancel_[0].primary_ == "BUY-1");
  }

  SECTION("flattening does not count the stop as the exit") {
    // Before: the resting sell of 100 was "already working", so nothing was sent and the position
    // stayed open behind a conditional order that may never release.
    withdraws("0");
  }

  SECTION("a stop the broker reports under another basis still counts") {
    portfolio.working_.front().order_.type_ = OrderType::LIMIT;
    withdraws("0");
  }

  SECTION("only the stop is withdrawn; the restated target decides the rest") {
    // A sell limit already working toward the target is not the stop's problem. Withdrawing it too
    // would only have the restated target place it again.
    auto sell = working("40", "0", OrderStatus::PENDING, OrderSide::SELL);
    sell.ref_ = OrderRef<strategy::ProbeBroker>{"SELL-1", ""};
    sell.order_.type_ = OrderType::LIMIT;
    sell.order_.price_ = Decimal64::parse("70000");
    portfolio.working_.push_back(sell);
    withdraws("0");
  }

  SECTION("a stop already on its way out is not withdrawn twice, and still holds the trade") {
    portfolio.working_.front().order_.status_ = OrderStatus::PENDING_CANCEL;
    const auto plan = strategy::reconcile(target("0"), portfolio, inputs);
    CHECK(plan.cancel_.empty());
    CHECK(plan.place_.empty());
    REQUIRE(plan.rejected_.size() == 1);
    CHECK(plan.rejected_[0].reason_ == strategy::RejectReason::STOP_ORDER_RESTING);
  }

  SECTION("a stop that is no longer open does not hold the target up") {
    portfolio.working_.front().order_.status_ = OrderStatus::CANCELED;
    const auto plan = strategy::reconcile(target("0"), portfolio, inputs);
    CHECK(plan.rejected_.empty());
    REQUIRE(plan.place_.size() == 1);
    CHECK(plan.place_[0].side_ == OrderSide::SELL);
  }
}

TEST_CASE("the default limits refuse a sell the account has not been shown to hold", "[short]") {
  // On by default, so that a runner bound to a cash account cannot send a sale of shares it does not hold:
  // a target below zero, or one that crosses zero. Turning the check off is the explicit choice.
  const RiskGuard guard;
  ProbePortfolio flat;
  flat.sellable_basis_ = whole_holding_basis();
  const auto from_flat = guard.check(planned("1", "70000", OrderSide::SELL), flat, open_market());
  REQUIRE(from_flat);
  CHECK(from_flat->reason_ == GuardReason::QUANTITY_NOT_SELLABLE);

  ProbePortfolio long_five;
  long_five.sellable_basis_ = whole_holding_basis();
  long_five.positions_.push_back(sellable_holding("5"));
  const auto through_zero = guard.check(planned("6", "70000", OrderSide::SELL), long_five, open_market());
  REQUIRE(through_zero);
  CHECK(through_zero->reason_ == GuardReason::QUANTITY_NOT_SELLABLE);
  CHECK_FALSE(guard.check(planned("5", "70000", OrderSide::SELL), long_five, open_market()));

  RiskLimits borrowing;
  borrowing.enforce_sellable_quantity_ = false;
  CHECK_FALSE(RiskGuard(borrowing).check(planned("1", "70000", OrderSide::SELL), flat, open_market()));
}
