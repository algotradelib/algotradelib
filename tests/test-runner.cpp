#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace algotradelib;

// A strategy handler cannot take the runner's context by value: what it submitted would go to the copy.
static_assert(!std::is_copy_constructible_v<runner::Context<strategy::ProbeBroker, strategy::NoCaps>>);
static_assert(!std::is_copy_assignable_v<runner::Context<strategy::ProbeBroker, strategy::NoCaps>>);
static_assert(std::is_move_constructible_v<runner::Context<strategy::ProbeBroker, strategy::NoCaps>>);

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;

namespace {

using Clock = std::chrono::system_clock;

/** Builds a UTC instant without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) +
    std::chrono::minutes(minute);
}

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

strategy::BarEvent bar_at(int day, const char* close) {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, day, 6, 30);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);
}

/** Holds a fixed number of shares, stated as a target. */
struct HoldTen {
  std::size_t bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ++bars_;
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    target.tag_ = "hold-ten";
    ctx.submit(target);
    ctx.log().note("targeted ten");
  }
};

/** Cancels the same resting order twice in one bar, which is one withdrawal. */
struct CancelSameRefTwice {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "first");
      ctx.cancel(working.ref_, "second");
    }
  }
};

/** Defines only on_start, to prove the other handlers cost nothing. */
struct StartOnly {
  bool started_{false};
  template <class Ctx> void on_start(Ctx&) { started_ = true; }
};

/** Prices a limit above today's band, to show the runner clamping it. */
struct LimitAt80k {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit =
      strategy::Price::checked(Decimal64::parse("80000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("1");
    target.limit_ = limit;
    ctx.submit(target);
  }
};

/** Buys ten once per bar, so two bars accumulate across two passes. */
struct BuyEachBar {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      )
    );
  }
};

/** Sells sixty every bar, which one pass can afford and two cannot. */
struct SellSixty {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("60"))
      )
    );
  }
};

/** Arranges a runner with reference data, an open market, and a last price. */
template <typename R>
void arrange(R& dry) {
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));
}

/** Sells the same instrument twice in one pass, each order fitting the sellable quantity alone. */
struct SellTwice {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    for (int i = 0; i < 2; ++i) {
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("60"))
        )
      );
    }
  }
};

} // namespace

TEST_CASE("the same reference cancelled twice by the strategy is one request") {
  using Tag = runner::DryRunner<CancelSameRefTwice>::Tag;
  runner::DryRunner<CancelSameRefTwice> dry;
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  WorkingOrder<Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("4");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.ref_ = OrderRef<Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_cancel_.size() == 1);
}

TEST_CASE("a second sell in one pass is sized against the first and not the same snapshot") {
  // The portfolio on the context is what the *broker* last reported, and nothing puts a locally
  // accepted order into it. So every order in a pass used to be checked from the same starting
  // point: a hundred sellable, two sells of sixty, both permitted, twenty shares sold that were
  // never there. The pass now checks each order against a view carrying what it has already sent.
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  runner::DryRunner<SellTwice> dry(SellTwice{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<SellTwice>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  // Standing for a broker that published the figure and stated what it covers. An unstated basis
  // refuses, which is its own test rather than a surprise here.
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].quantity_ == Decimal64::parse("60"));
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
  // Sixty already sent plus sixty more against a hundred sellable, so the numbers name the breach.
  REQUIRE(report.refused_[0].limit_ == Decimal64::parse("100"));
  REQUIRE(report.refused_[0].attempted_ == Decimal64::parse("120"));
}

TEST_CASE("an unresolved position refuses the exposure ceiling rather than being left out") {
  // **The reachable shape of an incomplete basis.** Reconciliation rejects an order whose *own*
  // instrument has no reference data before a PlannedOrder exists, so the candidate is always
  // resolvable here; what can be unresolved is something else the account holds. Left out of the
  // sum it would under-report, so it refuses.
  strategy::RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("10000000");
  runner::DryRunner<BuyEachBar> dry(BuyEachBar{}, limits);
  using Tag = runner::DryRunner<BuyEachBar>::Tag;
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  HoldingPosition unknown;
  // Registered nowhere, and carrying no mark of its own either, so neither source resolves it.
  unknown.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "000660"};
  unknown.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(unknown);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::VALUE_NOT_KNOWN);
}

TEST_CASE("a strategy defining one handler is driven without the others existing") {
  runner::DryRunner<StartOnly> dry;
  arrange(dry);

  REQUIRE(dry.start().quiet());
  REQUIRE(dry.strategy().started_);
  // Feeding a bar to a strategy with no on_bar compiles and does nothing.
  REQUIRE(dry.feed(bar_at(10, "70000")).quiet());
  REQUIRE(dry.stop().quiet());
}

TEST_CASE("the probe does not learn the broker's convention from this runner's own reservation") {
  // Without this the probe would be worse than useless: with the sellable check
  // on, an accepted sell is kept as a row of ours so the next order cannot re-sell the same shares.
  // If the probe counted that row, the resting quantity would rise with no change in the broker's
  // sellable figure -- and it would record GROSS_OF_RESTING on evidence it manufactured itself,
  // every time, for any broker, confidently.
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  runner::DryRunner<SellSixty> dry(SellSixty{}, limits);
  arrange(dry);
  dry.probe_sellable_convention();

  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  // Standing for a broker that published the figure and stated what it covers. An unstated basis
  // refuses, which is its own test rather than a surprise here.
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  dry.context().set_portfolio(portfolio);

  dry.feed(bar_at(10, "70000"));   // sells sixty, and reserves it
  dry.feed(bar_at(11, "70000"));   // observes a portfolio carrying that reservation

  // Nothing was learned, because nothing the broker said changed.
  REQUIRE(dry.sellable_convention().overall().total() == 0);
  REQUIRE(dry.sellable_convention().verdict() == strategy::SellableConvention::UNDETERMINED);

  // And the broker's own answer still teaches it: sixty rests, and the broker's figure drops by
  // sixty, which is the real observation the probe exists for.
  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> polled;
  polled.positions_.push_back(held);
  // The probe needs a stated coverage on every reading it compares, not only the first: two readings
  // whose basis differs are not comparable, and one with no basis teaches nothing.
  polled.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  dry.context().set_portfolio(polled);
  dry.feed(bar_at(12, "70000"));

  auto reported = polled;
  reported.positions_[0].sellable_quantity_ = Decimal64::parse("40");
  WorkingOrder<runner::DryRunner<SellSixty>::Tag> broker_row;
  broker_row.order_.instrument_ = samsung();
  broker_row.order_.side_ = OrderSide::SELL;
  broker_row.order_.status_ = OrderStatus::PENDING;
  broker_row.order_.quantity_ = Decimal64::parse("60");
  broker_row.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  broker_row.ref_ = OrderRef<runner::DryRunner<SellSixty>::Tag>{"0001", ""};
  reported.working_.push_back(broker_row);
  dry.context().set_portfolio(reported);
  dry.observe_sellable_convention();

  REQUIRE(dry.sellable_convention().overall().net_ == 1);
}

TEST_CASE("two identities sharing a symbol keep their own bands") {
  // `InstrumentId` equality is country, venue and symbol, and the band map used to be keyed on the
  // symbol alone -- so the second registration below silently replaced the first, and an NXT order
  // was clamped to KRX's band.
  runner::DryRunner<LimitAt80k> dry;
  arrange(dry);
  dry.allow_unbounded_price_limits_age();
  Instrument on_nxt = samsung_reference();
  on_nxt.id_.venue_ = Venue::NXT;
  dry.context().set_instrument(on_nxt);

  PriceLimits combined;
  combined.upper_ = Decimal64::parse("71000");
  dry.set_price_limits(samsung(), combined);
  PriceLimits nxt;
  nxt.upper_ = Decimal64::parse("72000");
  dry.set_price_limits(on_nxt.id_, nxt);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  CHECK(report.would_place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("71000")));
}

TEST_CASE("an order a guard refused while carrying an unclamped price is counted too") {
  // The half of withholding's cost that no exchange ever sees, and the reason one counter was not
  // enough. RiskGuard values an order at quantity times price, so a withheld ceiling hands every
  // notional limit a larger number: this order is refused at 80,000 and would have passed at the
  // band's 71,000. Nothing is sent, so `unclamped_orders_rejected` stays at zero through it.
  strategy::RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("75000");
  runner::DryRunner<LimitAt80k> dry(LimitAt80k{}, limits);
  arrange(dry);
  PriceLimits band;
  band.upper_ = Decimal64::parse("71000");
  dry.set_price_limits(samsung(), band);

  const auto report = dry.feed(bar_at(10, "70000"));
  CHECK(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 1);
  CHECK(report.refused_[0].reason_ == strategy::GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  CHECK(dry.unclamped_orders_refused() == 1);
  CHECK(dry.unclamped_orders_rejected() == 0);
}

TEST_CASE("a band dated after the instant it is aged at is not applied") {
  // A clock that goes backwards. The age would be negative and pass every positive maximum, so a
  // band from the caller's own future would clamp -- which is the one direction this policy exists
  // to close. A duration the runner cannot compute is not an age.
  runner::DryRunner<LimitAt80k> dry;
  arrange(dry);
  dry.set_maximum_price_limits_age(std::chrono::hours(12));
  dry.set_observed_at(utc(2026, 3, 20, 6, 30));
  PriceLimits band;
  band.upper_ = Decimal64::parse("71000");
  dry.set_price_limits(samsung(), band);
  dry.set_observed_at(utc(2026, 3, 10, 6, 30));

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  CHECK(report.would_place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("80000")));
  CHECK(dry.price_bands_withheld() == 1);
}

namespace {

/**
 * An executor-shaped type with a `last_error()` that is not a `BrokerError`.
 *
 * It is never instantiated as an executor -- only the trait is asked about it -- so it carries no
 * more than the one method under test. A strict build rejects an unused constant, which is why
 * `reaches_a_broker` is not here.
 */
struct UnrelatedLastError {
  const std::optional<std::string>& last_error() const noexcept { return last_error_; }
  std::optional<std::string> last_error_;
};

} // namespace

/** An executor that refuses every order and may or may not say why. */
struct SilentRejector {
  static constexpr bool reaches_a_broker = true;

  runner::SendOutcome place(const strategy::PlannedOrder&) {
    return runner::SendOutcome::REJECTED;
  }

  /** Never reached here, and required because `Runner` compiles the withdrawal path unconditionally. */
  runner::SendOutcome cancel(const WorkingOrder<runner::DryTag>&) {
    return runner::SendOutcome::ACCEPTED;
  }

  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

  std::optional<BrokerError> last_error_;
};

TEST_CASE("a refusal that says nothing does not inherit the last one's account") {
  // **The record's contract is "the last refusal, or nothing", never "the last refusal that came
  // with words".** `SessionExecutor` always states something on this path, so this is about a
  // binding somebody else writes: it answers `REJECTED` and says nothing, the counter still moves,
  // and a record kept from an earlier refusal would be handed on as though it described this one --
  // the wrong instrument under the wrong tag.
  using Refusing = runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, SilentRejector>;
  SilentRejector executor;
  BrokerError first;
  first.broker_code_ = "SPOKE";
  first.message_ = "the broker explained itself";
  executor.last_error_ = first;

  Refusing runner_instance(HoldTen{}, {}, 512, executor);
  arrange(runner_instance);
  // This executor reaches a broker, so the runner requires an account it has read before it places.
  strategy::Portfolio<runner::DryTag> known;
  known.as_of_ = utc(2026, 3, 10, 6, 30);
  runner_instance.context().set_portfolio(known);

  runner_instance.feed(bar_at(10, "70000"));
  REQUIRE(runner_instance.last_broker_refusal().has_value());
  CHECK(runner_instance.last_broker_refusal()->code_ == "SPOKE");

  // The same executor, refusing again and saying nothing this time.
  runner_instance.executor().last_error_.reset();
  runner_instance.feed(bar_at(11, "70000"));

  CHECK(runner_instance.orders_broker_refused() == 2);
  CHECK_FALSE(runner_instance.last_broker_refusal().has_value());
}

TEST_CASE("the refusal record is detected by shape, not by a method's name") {
  // **A name is not a contract.** An earlier version of `states_last_error` asked only whether
  // `last_error()` was a valid expression, so any executor with a method of that name selected the
  // branch that reads `broker_code_` -- and a caller whose `last_error()` returned something else
  // stopped compiling for having chosen a name. The detection is on the documented return type.
  STATIC_REQUIRE(runner::runner_detail::states_last_error<UnrelatedLastError>::value == false);
}

TEST_CASE("each way an instruction fails is counted apart from the others") {
  // **A live run used to say nothing about any of this.** `Runner::feed` returns a report describing
  // what became of every instruction and `live::SessionSource` discards it on every poll, so a run in
  // which everything was refused looked exactly like one in which everything was accepted. These are
  // the figures a driving loop differences to say otherwise -- and they are counted by *origin*,
  // because an operator's next move differs for each.
  SECTION("reconciliation could not build it") {
    // No reference data, so neither the tick grid nor the lot size is known.
    runner::DryRunner<HoldTen> dry;
    dry.context().set_phase(strategy::MarketPhase::OPEN);
    dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

    const auto report = dry.feed(bar_at(10, "70000"));
    REQUIRE(report.rejected_.size() == 1);
    CHECK(dry.orders_unbuildable() == 1);
    // And why, which a live source's discarded report would otherwise take with it.
    REQUIRE(dry.last_unbuildable().has_value());
    CHECK(dry.last_unbuildable()->reason_ == report.rejected_.front().reason_);
    CHECK(dry.orders_guard_refused() == 0);
    CHECK(dry.orders_broker_refused() == 0);
    CHECK(dry.orders_binding_refused() == 0);
    CHECK(dry.orders_indeterminate() == 0);
    CHECK(dry.orders_reconcile_withheld() == 0);
  }

  SECTION("a guard refused it") {
    strategy::RiskLimits limits;
    limits.max_order_notional_ = Decimal64::parse("1");
    runner::DryRunner<HoldTen> dry(HoldTen{}, limits);
    arrange(dry);

    const auto report = dry.feed(bar_at(10, "70000"));
    REQUIRE(report.refused_.size() == 1);
    CHECK(dry.orders_guard_refused() == 1);
    CHECK(dry.orders_unbuildable() == 0);
    CHECK(dry.orders_broker_refused() == 0);
  }

  SECTION("a plan held back for want of an account is counted apart") {
    // Nothing is wrong with the instructions: the runner cannot vouch for the account they would be
    // sized against, so the whole plan goes back untouched. Counted apart from the six origins for
    // that reason -- and because the loop's own gate does not always describe it.
    runner::DryRunner<HoldTen> dry;
    arrange(dry);
    dry.require_known_portfolio(true);

    const auto report = dry.feed(bar_at(10, "70000"));
    REQUIRE(report.withheld_plans_.size() == 1);
    CHECK(dry.plans_withheld() == 1);
    CHECK(dry.orders_unbuildable() == 0);
    CHECK(dry.orders_guard_refused() == 0);
  }

  SECTION("a clean run counts nothing at all") {
    // Absence versus zero, from the other side: every figure stays at zero when nothing failed, so a
    // reader seeing zeros is being told the run was clean rather than that nothing was measured.
    runner::DryRunner<HoldTen> dry;
    arrange(dry);

    const auto report = dry.feed(bar_at(10, "70000"));
    REQUIRE(report.would_place_.size() == 1);
    CHECK(dry.orders_unbuildable() == 0);
    CHECK(dry.orders_guard_refused() == 0);
    CHECK(dry.orders_broker_refused() == 0);
    CHECK(dry.orders_binding_refused() == 0);
    CHECK(dry.orders_indeterminate() == 0);
    CHECK(dry.orders_reconcile_withheld() == 0);
    CHECK(dry.plans_withheld() == 0);
    CHECK(dry.instructions_unavailable() == 0);
    CHECK_FALSE(dry.last_broker_refusal().has_value());
  }
}

TEST_CASE("the log is bounded, so a long session cannot grow it without limit") {
  runner::Context<runner::DryRunner<HoldTen>::Tag, strategy::NoCaps> context;
  for (int at = 0; at < 5000; ++at) {
    context.log().note("line");
  }
  REQUIRE(context.log().lines().size() <= 4096);

  const auto taken = context.log().take();
  REQUIRE_FALSE(taken.empty());
  REQUIRE(context.log().lines().empty());
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("the runner caches bars as a contiguous series an indicator can read") {
  runner::DryRunner<HoldTen> dry(HoldTen{}, {}, 8);
  arrange(dry);

  for (int day = 10; day < 16; ++day) {
    dry.feed(bar_at(day, "70000"));
  }

  const auto* series = dry.context().bars(samsung(), BarInterval::ONE_DAY);
  REQUIRE(series != nullptr);
  REQUIRE(series->size() == 6);
  // The same call an indicator makes, over the runner's own memory.
  const auto closes = series->column(analytics::BarField::CLOSE);
  REQUIRE(closes.size() == 6);
  REQUIRE(closes[5] == 70000.0);

  // An instrument never seen has no series, rather than an empty one that looks like a gap.
  REQUIRE(dry.context().bars(InstrumentId{Country::KR, Venue::COMBINED, "000660"}) == nullptr);
}
#endif

// ---------------------------------------------------------------------------
// Transport indeterminacy: the most important behaviour here
// ---------------------------------------------------------------------------

namespace {

using runner::IndeterminacyLedger;
using runner::SendOutcome;
using runner::WithheldReason;

/** An executor that reports a chosen outcome, so the failure paths are reachable without a broker. */
template <typename Tag>
class ScriptedExecutor {
public:

  /** A double reaches no broker, so it is exempt from having to have read the account. */
  static constexpr bool reaches_a_broker = false;
  SendOutcome place(const strategy::PlannedOrder& order) {
    placed_.push_back(order);
    return place_outcome_;
  }

  SendOutcome cancel(const WorkingOrder<Tag>& working) {
    canceled_.push_back(working.ref_);
    return cancel_outcome_;
  }

  const std::vector<strategy::PlannedOrder>& placed() const noexcept { return placed_; }
  const std::vector<OrderRef<Tag>>& canceled() const noexcept { return canceled_; }

  SendOutcome place_outcome_{SendOutcome::ACCEPTED};
  SendOutcome cancel_outcome_{SendOutcome::ACCEPTED};

private:
  std::vector<strategy::PlannedOrder> placed_;
  std::vector<OrderRef<Tag>> canceled_;
};

/** Cancels a reference the portfolio does not hold, to show it being reported rather than sent. */
struct CancelGhost {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    ctx.cancel(OrderRef<runner::DryTag>{"never-seen", ""}, "ghost");
  }
};

using ScriptedRunner = runner::Runner<
  HoldTen, strategy::NoCaps, runner::DryTag, ScriptedExecutor<runner::DryTag>
>;

/** Targets zero in Samsung on every bar, so the closing path is reachable with a scripted outcome. */
struct FlattenSamsung {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    strategy::TargetPosition target;
    target.instrument_ = samsung();
    target.target_quantity_ = Decimal64::parse("0");
    target.tag_ = "flatten";
    ctx.submit(target);
  }
};

using FlattenRunner = runner::Runner<
  FlattenSamsung, strategy::NoCaps, runner::DryTag, ScriptedExecutor<runner::DryTag>
>;

/** A hundred Samsung held, stamped now, which the close below reduces to flat. */
strategy::Portfolio<runner::DryTag> holding_a_hundred() {
  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(position);
  return portfolio;
}

} // namespace

TEST_CASE("a close that never left the process is not counted") {
  // `REFUSED_LOCALLY` means the binding declined it and no request went out, so there is nothing to
  // witness. The same division `note_sent` makes, and for the same reason.
  FlattenRunner runner_instance;
  arrange(runner_instance);
  runner_instance.context().set_phase(strategy::MarketPhase::CLOSED);
  runner_instance.context().set_portfolio(holding_a_hundred());
  runner_instance.executor().place_outcome_ = SendOutcome::REFUSED_LOCALLY;

  runner_instance.feed(bar_at(10, "70000"));

  REQUIRE(runner_instance.sends_outside_permitted_phase() == 0);
}

TEST_CASE("a broker refusal needs no reconciliation, because it is an answer") {
  // The distinction that makes a send's outcome more than a boolean: a rejection says the order
  // does not exist, so nothing is in doubt.
  ScriptedRunner dry;
  arrange(dry);
  dry.executor().place_outcome_ = SendOutcome::REJECTED;

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.empty());
  REQUIRE_FALSE(report.rejected_.empty());
  REQUIRE(dry.ledger().empty());

  // So the next bar is free to try again.
  dry.executor().place_outcome_ = SendOutcome::ACCEPTED;
  REQUIRE(dry.feed(bar_at(11, "70000")).would_place_.size() == 1);
}

TEST_CASE("a withheld order is never handed to the executor at all") {
  // Belt and braces on the promise: the executor is the only thing that can reach a broker, so
  // asserting it was not called is asserting nothing was sent.
  ScriptedRunner dry;
  arrange(dry);
  dry.ledger().mark(samsung(), "earlier failure");

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.withheld_.size() == 1);
  REQUIRE(dry.executor().placed().empty());
}

TEST_CASE("every send outcome and withheld reason has a stable name") {
  REQUIRE(runner::send_outcome_name(SendOutcome::ACCEPTED) == "accepted");
  REQUIRE(runner::send_outcome_name(SendOutcome::REJECTED) == "rejected");
  REQUIRE(runner::send_outcome_name(SendOutcome::INDETERMINATE) == "indeterminate");
  REQUIRE(
    runner::withheld_reason_name(WithheldReason::RECONCILE_REQUIRED) == "reconcile_required"
  );
}

TEST_CASE("every error category is classified by whether the order might exist") {
  const auto classify = [](ErrorCategory category, int status = 0) {
    BrokerError error;
    error.category_ = category;
    error.http_status_ = status;
    return runner::outcome_of(error);
  };

  // The connection failed; whether the bytes arrived is exactly what is unknown.
  REQUIRE(classify(ErrorCategory::TRANSPORT) == SendOutcome::INDETERMINATE);
  // A local abandonment -- a deadline, a closing session -- says nothing about whether the request
  // had already gone out. Treating it as failure is how an unwatched order ends up resting.
  REQUIRE(classify(ErrorCategory::CANCELLED) == SendOutcome::INDETERMINATE);
  // The broker answered and the answer could not be parsed. It may well have been an
  // acknowledgement, which is the trap: it reads like a client-side problem.
  REQUIRE(classify(ErrorCategory::PROTOCOL) == SendOutcome::INDETERMINATE);
  // The broker's own infrastructure failing, possibly after the instruction reached the engine.
  REQUIRE(classify(ErrorCategory::HTTP, 500) == SendOutcome::INDETERMINATE);
  REQUIRE(classify(ErrorCategory::HTTP, 503) == SendOutcome::INDETERMINATE);

  // A 4xx is the broker declining to act, which is an answer.
  REQUIRE(classify(ErrorCategory::HTTP, 400) == SendOutcome::REJECTED);
  REQUIRE(classify(ErrorCategory::HTTP, 404) == SendOutcome::REJECTED);
  // Authentication and rate limiting are both refused before processing.
  REQUIRE(classify(ErrorCategory::AUTHENTICATION) == SendOutcome::REJECTED);
  REQUIRE(classify(ErrorCategory::RATE_LIMIT) == SendOutcome::REJECTED);
  // An invalid argument is rejected locally or declined by the broker; either way it does not exist.
  REQUIRE(classify(ErrorCategory::INVALID_ARGUMENT) == SendOutcome::REJECTED);
}

TEST_CASE("a cancellation naming an order the snapshot does not hold is reported, not sent blind") {
  // The snapshot is stale and the order may already be gone. Building a cancellation needs the
  // order's own quantity on a broker that requires one, so an order the runner cannot describe is
  // one it must not invent an instruction for.
  using Tag = runner::DryTag;
  using Tag = runner::DryTag;
  runner::Runner<CancelGhost, strategy::NoCaps, Tag, ScriptedExecutor<Tag>> dry;
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.unresolved_cancel_.size() == 1);
  REQUIRE(report.would_cancel_.empty());
  REQUIRE(dry.executor().canceled().empty());
}

namespace {

/** A portfolio carrying one resting sell, stamped or not as the case under test needs. */
strategy::Portfolio<runner::DryTag> resting_sell(bool stamped) {
  strategy::Portfolio<runner::DryTag> portfolio;
  if (stamped) {
    portfolio.as_of_ = utc(2026, 3, 10, 6, 30);
  }
  WorkingOrder<runner::DryTag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::SELL;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("10");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.ref_ = OrderRef<runner::DryTag>{"0001", ""};
  portfolio.working_.push_back(working);
  return portfolio;
}

/** Wants to be flat, so its plan is a withdrawal and nothing else. */
struct FlatTargetStrategy {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("0");
    target.tag_ = "exit";
    ctx.submit(target);
  }
};

} // namespace

TEST_CASE("a plan derived from an account never read is withheld whole, and says so") {
  // The runner half of the gate, with no loop and therefore no holds at all: the epoch trigger is
  // intrinsic, so a consumer driving a runner directly is protected by the same rule.
  runner::DryRunner<FlatTargetStrategy> dry;
  arrange(dry);
  dry.context().set_portfolio(resting_sell(/*stamped=*/false));

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.withheld_plans_.size() == 1);
  const auto& withheld = report.withheld_plans_.front();
  CHECK(withheld.reason_ == runner::WithheldPlanReason::PORTFOLIO_ABSENT);
  CHECK(withheld.instrument_ == samsung());
  CHECK(withheld.tag_ == "exit");
  REQUIRE(withheld.not_canceled_.size() == 1);
  CHECK(withheld.not_canceled_.front() == OrderRef<runner::DryTag>{"0001", ""});
  // A target of zero against an apparently-flat account plans no placement at all, which is exactly
  // why `WithheldOrder` could not carry this result.
  CHECK(withheld.not_placed_ == 0);
  // Nothing was sent, in either direction.
  CHECK(report.would_cancel_.empty());
  CHECK(report.would_place_.empty());
  // And the run did not decide to do nothing: a `quiet()` that said so would hide the whole result.
  CHECK_FALSE(report.quiet());
}

TEST_CASE("a plan is withheld when a hold would refuse the replacement it depends on") {
  // The third trigger, and the one that has nothing to do with the portfolio. The snapshot is fresh
  // and the account is known; what makes the plan unsafe is that its cancellations would go out
  // while its placement half is refused, leaving a bare withdrawal the strategy never stated.
  runner::DryRunner<FlatTargetStrategy> dry;
  arrange(dry);
  dry.context().set_portfolio(resting_sell(/*stamped=*/true));
  dry.guard().halt(strategy::HaltReason::BUYING_POWER_UNREAD);

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::PLACEMENT_HELD);
  // The hold is named, so an operator is not left to guess which of eight it was.
  CHECK(report.withheld_plans_.front().detail_ == "buying_power_unread");
  CHECK(report.would_cancel_.empty());
}

TEST_CASE("a plan derived from a snapshot past the stated age is withheld") {
  // The second trigger. The policy is evaluated here, against the snapshot in hand, rather than
  // being handed in as a verdict computed about some earlier portfolio.
  runner::DryRunner<FlatTargetStrategy> dry;
  arrange(dry);
  auto portfolio = resting_sell(/*stamped=*/true);
  portfolio.as_of_ = utc(2026, 3, 1, 6, 30);
  dry.context().set_portfolio(portfolio);
  dry.set_maximum_portfolio_age(std::chrono::minutes(5));

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::PORTFOLIO_STALE);
  CHECK(report.would_cancel_.empty());
}

namespace {

/** Withdraws the first resting row by name, which is the instruction a plan never derived. */
struct WithdrawsOne {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    const auto& working = ctx.portfolio().working_;
    if (!working.empty()) {
      ctx.cancel(working.front().ref_, "bye");
    }
  }
};

} // namespace

TEST_CASE("a named withdrawal survives a hold that only means the runner is uninformed") {
  // The half of this rule that *permits* rather than refuses, and the reason the two halves cannot
  // be separated: an order that cannot be withdrawn keeps resting and can fill, which is worse
  // than the exposure a holdings-shaped hold was guarding.
  runner::DryRunner<WithdrawsOne> dry;
  arrange(dry);
  dry.context().set_portfolio(resting_sell(/*stamped=*/false));

  auto uninformed = GENERATE(
    strategy::HaltReason::ORDER_LIST_UNREAD,
    strategy::HaltReason::PORTFOLIO_ABSENT,
    strategy::HaltReason::PORTFOLIO_STALE,
    strategy::HaltReason::BUYING_POWER_UNREAD,
    strategy::HaltReason::POLL_FAILING);
  dry.guard().halt(uninformed);

  const auto report = dry.feed(bar_at(10, "70000"));

  CAPTURE(strategy::halt_reason_name(uninformed));
  REQUIRE(report.would_cancel_.size() == 1);
  CHECK(report.refused_cancel_.empty());
  CHECK(report.unavailable_cancel_.empty());
  CHECK(report.unresolved_cancel_.empty());
}

TEST_CASE("every halt reason has a name, and the count matches the enumerators") {
  const std::array<strategy::HaltReason, strategy::halt_reason_count> all{
    strategy::HaltReason::OPERATOR,
    strategy::HaltReason::LOCAL_FAILURE,
    strategy::HaltReason::STOPPING,
    strategy::HaltReason::ORDER_LIST_UNREAD,
    strategy::HaltReason::PORTFOLIO_ABSENT,
    strategy::HaltReason::PORTFOLIO_STALE,
    strategy::HaltReason::BUYING_POWER_UNREAD,
    strategy::HaltReason::POLL_FAILING,
  };
  for (std::size_t index = 0; index < all.size(); ++index) {
    CAPTURE(index);
    // Declaration order is gravity order and `blocking_reason` depends on it, so the position each
    // reason occupies is part of the contract rather than an implementation detail.
    CHECK(static_cast<std::size_t>(all[index]) == index);
    CHECK(strategy::halt_reason_name(all[index]) != "unknown");
  }
  CHECK(strategy::halt_reason_name(strategy::HaltReason::PORTFOLIO_ABSENT) == "portfolio_absent");
  CHECK(runner::withheld_plan_reason_name(runner::WithheldPlanReason::PLACEMENT_HELD) ==
        "placement_held");
}


TEST_CASE("a context finds an instrument's reference data and last price by its whole id", "[runner][context]") {
  runner::Context<runner::DryTag, strategy::NoCaps> ctx;
  const InstrumentId combined{Country::KR, Venue::COMBINED, "005930"};
  const InstrumentId krx{Country::KR, Venue::KRX, "005930"};
  const InstrumentId abroad{Country::US, Venue::COMBINED, "005930"};

  CHECK_FALSE(ctx.instrument(combined).has_value());
  CHECK_FALSE(ctx.last_price(combined).has_value());

  // The same symbol at another venue or in another country is another instrument.
  auto reference = samsung_reference();
  ctx.set_instrument(reference);
  ctx.set_last_price(combined, Decimal64::parse("70000"));
  REQUIRE(ctx.instrument(combined).has_value());
  CHECK(ctx.instrument(combined)->currency_ == Currency{"KRW"});
  CHECK_FALSE(ctx.instrument(krx).has_value());
  CHECK_FALSE(ctx.instrument(abroad).has_value());
  CHECK_FALSE(ctx.last_price(krx).has_value());
  CHECK_FALSE(ctx.last_price(abroad).has_value());

  auto other = reference;
  other.id_ = abroad;
  other.currency_ = Currency{"USD"};
  ctx.set_instrument(other);
  ctx.set_last_price(abroad, Decimal64::parse("12.5"));
  CHECK(ctx.instrument(combined)->currency_ == Currency{"KRW"});
  CHECK(ctx.instrument(abroad)->currency_ == Currency{"USD"});
  CHECK(*ctx.last_price(combined) == Decimal64::parse("70000"));
  CHECK(*ctx.last_price(abroad) == Decimal64::parse("12.5"));

  // Many later registrations leave the first ones as they were; a second registration of the same
  // instrument, and a later price, replace.
  for (int at = 0; at < 2000; ++at) {
    auto more = reference;
    more.id_ = InstrumentId{Country::KR, Venue::COMBINED, "S" + std::to_string(at)};
    ctx.set_instrument(more);
    ctx.set_last_price(more.id_, Decimal64(at, 0));
  }
  CHECK(ctx.instrument(combined)->lot_size_ == Decimal64::parse("1"));
  CHECK(ctx.instrument(abroad)->currency_ == Currency{"USD"});
  reference.lot_size_ = Decimal64::parse("10");
  ctx.set_instrument(reference);
  CHECK(ctx.instrument(combined)->lot_size_ == Decimal64::parse("10"));
  CHECK(*ctx.last_price(InstrumentId{Country::KR, Venue::COMBINED, "S1999"}) == Decimal64(1999, 0));
  ctx.set_last_price(combined, Decimal64::parse("71000"));
  CHECK(*ctx.last_price(combined) == Decimal64::parse("71000"));
}

namespace {

// How a cancellation changes the portfolio its runner shows, as an executor holding the runner's context may.
enum class OnCancel { SWAP_ROWS, DROP_FIRST_ROW };

// Records what it places; its cancellation rewrites the context's position rows.
struct RewritesOnCancel {
  static constexpr bool reaches_a_broker = false;
  runner::Context<runner::DryTag, strategy::NoCaps>* context_{nullptr};
  OnCancel how_{OnCancel::SWAP_ROWS};
  std::vector<strategy::PlannedOrder> placed_;

  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    placed_.push_back(order);
    return runner::SendOutcome::ACCEPTED;
  }
  runner::SendOutcome cancel(const WorkingOrder<runner::DryTag>&) {
    auto portfolio = context_->portfolio();
    if (how_ == OnCancel::SWAP_ROWS) {
      std::swap(portfolio.positions_[0], portfolio.positions_[1]);
    } else {
      portfolio.positions_.erase(portfolio.positions_.begin());
    }
    context_->set_portfolio(std::move(portfolio));
    return runner::SendOutcome::ACCEPTED;
  }
};

InstrumentId named(const char* symbol) { return InstrumentId{Country::KR, Venue::COMBINED, symbol}; }

// Cancels its working order in A, then holds 110 of A, then states 100 of B fifteen times: seventeen
// instructions, so the pass indexes the portfolio's rows.
struct CancelsThenTargets {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    ctx.cancel(OrderRef<typename Ctx::Broker>{"0001", ""});
    const auto hold = [&ctx](const char* symbol, const char* quantity) {
      strategy::TargetPosition target;
      target.instrument_ = named(symbol);
      target.target_quantity_ = Decimal64::parse(quantity);
      target.tag_ = "test";
      ctx.submit(target);
    };
    hold("A", "110");
    for (int k = 0; k < 15; ++k) {
      hold("B", "100");
    }
  }
};

} // namespace

TEST_CASE("a pass plans from the portfolio as an executor's cancellation left it") {
  // The pass indexes the portfolio's rows before planning; an executor's cancellation then replaces the
  // portfolio (rows swapped, or one dropped). Every later target must be planned against the portfolio as it
  // now is, as a scan reads it, not against the index of the one before.
  for (const auto how : {OnCancel::SWAP_ROWS, OnCancel::DROP_FIRST_ROW}) {
    INFO((how == OnCancel::SWAP_ROWS ? "rows swapped" : "first row dropped"));
    strategy::RiskLimits limits;
    limits.permitted_phases_ = std::nullopt;
    runner::Runner<CancelsThenTargets, strategy::NoCaps, runner::DryTag, RewritesOnCancel> run(CancelsThenTargets{}, limits);
    run.executor().context_ = &run.context();
    run.executor().how_ = how;
    for (const char* symbol : {"A", "B"}) {
      auto reference = samsung_reference();
      reference.id_ = named(symbol);
      run.context().set_instrument(reference);
      run.context().set_last_price(named(symbol), Decimal64::parse("70000"));
    }
    run.context().set_phase(strategy::MarketPhase::OPEN);
    strategy::Portfolio<runner::DryTag> portfolio;
    HoldingPosition a;
    a.instrument_ = named("A");
    a.quantity_ = Decimal64::parse("10");
    HoldingPosition b;
    b.instrument_ = named("B");
    b.quantity_ = Decimal64::parse("100");
    portfolio.positions_ = {a, b};
    WorkingOrder<runner::DryTag> working;
    working.order_.instrument_ = named("A");
    working.order_.side_ = OrderSide::BUY;
    working.order_.type_ = OrderType::LIMIT;
    working.order_.status_ = OrderStatus::PENDING;
    working.order_.quantity_ = Decimal64::parse("1");
    working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    working.ref_ = OrderRef<runner::DryTag>{"0001", ""};
    portfolio.working_.push_back(working);
    run.context().set_portfolio(portfolio);

    run.feed(bar_at(10, "70000"));
    const auto& placed = run.executor().placed_;
    // The cancelled buy of 1 is still listed, so it counts as working. Swapped, A still holds 10 and B 100:
    // buy 99 of A, nothing of B. Dropped, A holds nothing: buy 109 of A, and B still holds 100.
    REQUIRE(placed.size() == 1);
    CHECK(placed[0].instrument_ == named("A"));
    CHECK(placed[0].side_ == OrderSide::BUY);
    CHECK(placed[0].quantity_ == Decimal64::parse(how == OnCancel::SWAP_ROWS ? "99" : "109"));
  }
}

#if defined(ATL_HAS_EIGEN)
TEST_CASE("a cached series stays where it is as more instruments and intervals are cached") {
  // `ctx.bars` hands out a pointer to the cache; the series behind it must not move when other instruments, or
  // another interval of the same one, are cached after it.
  runner::Context<runner::DryTag, strategy::NoCaps> context(8);
  const auto bar_for = [](const InstrumentId& id, BarInterval interval, int minute, const char* close) {
    Bar bar;
    bar.timestamp_ = utc(2026, 3, 10, 1, minute);
    bar.open_ = bar.high_ = bar.low_ = bar.close_ = Decimal64::parse(close);
    bar.volume_ = Decimal64::parse("1");
    return strategy::make_bar_event(id, interval, BarStamp::CLOSE, bar);
  };
  context.record_bar(bar_for(samsung(), BarInterval::ONE_DAY, 0, "70000"));
  const auto* daily = context.bars(samsung(), BarInterval::ONE_DAY);
  REQUIRE(daily != nullptr);
  for (int k = 0; k < 600; ++k) {
    context.record_bar(bar_for(InstrumentId{Country::KR, Venue::COMBINED, "N" + std::to_string(k)}, BarInterval::ONE_DAY, 0, "100"));
  }
  context.record_bar(bar_for(samsung(), BarInterval::ONE_MINUTE, 1, "70100"));
  REQUIRE(context.bars(samsung(), BarInterval::ONE_DAY) == daily);
  REQUIRE(daily->size() == 1);
  const auto* minute = context.bars(samsung(), BarInterval::ONE_MINUTE);
  REQUIRE(minute != nullptr);
  REQUIRE(minute != daily);
  REQUIRE(minute->size() == 1);
  REQUIRE(context.bars(samsung(), BarInterval::ONE_HOUR) == nullptr);
  REQUIRE(context.bars(InstrumentId{Country::KR, Venue::COMBINED, "N599"}) != nullptr);
  REQUIRE(context.bars(InstrumentId{Country::KR, Venue::COMBINED, "N600"}) == nullptr);
}
#endif

TEST_CASE("a context moves without throwing") {
  // Moving a context into a runner's place (from a handler, or between runners) must not fail halfway; the bar
  // history's conversion between keeping and not keeping bars is done in place for that reason.
  using Ctx = runner::DryRunner<BuyEachBar>::Context;
  STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Ctx>);
  // Move construction is as nothrow as the standard library's maps make it: MSVC's allocate a node when
  // move-constructed, so there a context never moved into a new one without the chance of a throw.
  constexpr bool maps_move_without_throwing =
    std::is_nothrow_move_constructible_v<std::map<int, int>> &&
    std::is_nothrow_move_constructible_v<std::unordered_map<int, int>>;
  STATIC_REQUIRE((!maps_move_without_throwing || std::is_nothrow_move_constructible_v<Ctx>));
}

#if defined(ATL_HAS_EIGEN)
namespace {

strategy::BarEvent minute_bar(int minute) {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, 10, 1, minute);
  bar.open_ = bar.high_ = bar.low_ = bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_MINUTE, BarStamp::CLOSE, bar);
}

/** Says it reads no bar history; counts its bars. */
struct KeepsNoHistory {
  static constexpr bool reads_bar_history = false;
  int bars_{0};
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) { ++bars_; }
};

/** The same, taking slices. */
struct SlicesKeepingNoHistory {
  static constexpr bool reads_bar_history = false;
  template <class Ctx>
  void on_bars(Ctx&, const strategy::BarSlice&) {}
};

/** Inherits the declaration, then reads the history from a handler that is not a bar handler. */
struct BreaksTheDeclaration : KeepsNoHistory {
  template <class Ctx>
  void on_phase(Ctx& ctx, const strategy::MarketPhase&) { static_cast<void>(ctx.bars(samsung())); }
};

/** Inherits the declaration and overrides it. */
struct ReadsAfterAll : KeepsNoHistory {
  static constexpr bool reads_bar_history = true;
};

struct DeclaresANonStatic {
  const bool reads_bar_history = false;
};
struct DeclaresAnInt {
  static constexpr int reads_bar_history = 0;
};
class DeclaresAPrivate {
  static constexpr bool reads_bar_history = false;
};
struct DeclaresAFunction {
  static constexpr bool reads_bar_history() { return false; }
};

} // namespace

TEST_CASE("reads_bar_history is read only from a public static bool and defaults to keeping history") {
  namespace detail = strategy::strategy_detail;
  STATIC_REQUIRE(runner::DryRunner<BuyEachBar>::records_bar_history);
  STATIC_REQUIRE_FALSE(runner::DryRunner<KeepsNoHistory>::records_bar_history);
  STATIC_REQUIRE_FALSE(runner::DryRunner<BreaksTheDeclaration>::records_bar_history);
  STATIC_REQUIRE(runner::DryRunner<ReadsAfterAll>::records_bar_history);
  // Declared but of another kind: found, and refused by bar_history_policy's static_assert rather than read as
  // absent, which would silently keep (or drop) the history.
  STATIC_REQUIRE(detail::declares_reads_bar_history<DeclaresANonStatic>);
  STATIC_REQUIRE(detail::declares_reads_bar_history<DeclaresAnInt>);
  STATIC_REQUIRE(detail::declares_reads_bar_history<DeclaresAPrivate>);
  STATIC_REQUIRE(detail::declares_reads_bar_history<DeclaresAFunction>);
  STATIC_REQUIRE_FALSE(detail::names_static_bool_reads_bar_history<DeclaresANonStatic>::value);
  STATIC_REQUIRE_FALSE(detail::names_static_bool_reads_bar_history<DeclaresAnInt>::value);
  STATIC_REQUIRE_FALSE(detail::names_static_bool_reads_bar_history<DeclaresAPrivate>::value);
  STATIC_REQUIRE_FALSE(detail::names_static_bool_reads_bar_history<DeclaresAFunction>::value);
  STATIC_REQUIRE(detail::names_static_bool_reads_bar_history<KeepsNoHistory>::value);
}

TEST_CASE("ctx.bars throws when the runner keeps no bar history") {
  runner::DryRunner<KeepsNoHistory> dry;
  // Before any bar too: a missing history is not the same as one not filled yet.
  CHECK_THROWS_AS(dry.context().bars(samsung(), BarInterval::ONE_MINUTE), std::logic_error);
  dry.feed(minute_bar(1));
  CHECK(dry.strategy().bars_ == 1);
  CHECK(dry.context().last_price(samsung()) == Decimal64::parse("70000"));
  CHECK(dry.context().now() == utc(2026, 3, 10, 1, 1));
  CHECK_THROWS_AS(dry.context().bars(samsung(), BarInterval::ONE_MINUTE), std::logic_error);
  // Nor can it be filled by hand.
  CHECK_THROWS_AS(dry.context().record_bar(minute_bar(2)), std::logic_error);

  // A read from any handler is refused, not answered with nullptr as if the series were empty.
  runner::DryRunner<BreaksTheDeclaration> breaks;
  CHECK_THROWS_AS(breaks.feed(strategy::MarketPhase::OPEN), std::logic_error);

  // An unseen series of a runner keeping history is still nullptr.
  runner::DryRunner<ReadsAfterAll> reads;
  CHECK(reads.context().bars(samsung(), BarInterval::ONE_MINUTE) == nullptr);
  reads.feed(minute_bar(1));
  REQUIRE(reads.context().bars(samsung(), BarInterval::ONE_MINUTE) != nullptr);
}

TEST_CASE("a runner keeping no bar history still refuses a bar not later than its series' last") {
  const std::string later = "a series buffer observation must be later than the last";
  runner::DryRunner<KeepsNoHistory> dry;
  dry.feed(minute_bar(2));
  CHECK_THROWS_WITH(dry.feed(minute_bar(2)), later);
  CHECK_THROWS_WITH(dry.feed_warmup(minute_bar(1)), later);
  CHECK(dry.strategy().bars_ == 1);
  dry.feed_warmup(minute_bar(3));
  CHECK(dry.strategy().bars_ == 2);

  runner::DryRunner<SlicesKeepingNoHistory> slices;
  slices.record(minute_bar(2));
  CHECK_THROWS_WITH(slices.record(minute_bar(2)), later);
  CHECK_THROWS_WITH(slices.record_history_only(minute_bar(1)), later);
  slices.record_history_only(minute_bar(3));
  CHECK_THROWS_WITH(slices.record(minute_bar(3)), later);
  CHECK(slices.context().last_price(samsung()) == Decimal64::parse("70000"));
}

TEST_CASE("a context moved between runners keeping and not keeping history keeps each series' newest bar") {
  const std::string later = "a series buffer observation must be later than the last";
  // A context holding a series, moved into a runner keeping no history: its newest bar still bounds the next.
  runner::DryRunner<KeepsNoHistory> dropping;
  runner::DryRunner<KeepsNoHistory>::Context holding;
  holding.record_bar(minute_bar(10));
  dropping.context() = std::move(holding);
  CHECK_THROWS_WITH(dropping.feed(minute_bar(9)), later);
  CHECK_THROWS_WITH(dropping.feed(minute_bar(10)), later);
  CHECK(dropping.strategy().bars_ == 0);
  dropping.feed(minute_bar(11));
  CHECK(dropping.strategy().bars_ == 1);
  CHECK_THROWS_AS(dropping.context().bars(samsung(), BarInterval::ONE_MINUTE), std::logic_error);

  // And back: a context that kept only newest bar times, moved into a runner keeping history.
  runner::DryRunner<ReadsAfterAll> keeping;
  keeping.context() = std::move(dropping.context());
  CHECK_THROWS_WITH(keeping.feed(minute_bar(11)), later);
  keeping.feed(minute_bar(12));
  const auto* series = keeping.context().bars(samsung(), BarInterval::ONE_MINUTE);
  REQUIRE(series != nullptr);
  CHECK(series->size() == 1);
  CHECK_THROWS_WITH(keeping.feed(minute_bar(12)), later);
}

TEST_CASE("a runner keeping no bar history keeps refusing reads after its context or itself is moved") {
  using Runner = runner::DryRunner<KeepsNoHistory>;
  using Ctx = Runner::Context;
  Runner dry;
  dry.feed(minute_bar(1));
  // A context moved into the runner's place keeps the runner's setting, not the one it was made with.
  Ctx fresh;
  dry.context() = std::move(fresh);
  CHECK_THROWS_AS(dry.context().bars(samsung(), BarInterval::ONE_MINUTE), std::logic_error);
  Runner moved(std::move(dry));
  CHECK_THROWS_AS(moved.context().bars(samsung(), BarInterval::ONE_MINUTE), std::logic_error);
  moved.feed(minute_bar(2));
  CHECK(moved.strategy().bars_ == 2);
  // A context of one's own keeps its history whatever is moved into it.
  Ctx own;
  own = std::move(moved.context());
  CHECK(own.bars(samsung(), BarInterval::ONE_MINUTE) == nullptr);
}
#endif
