#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;

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

/** Cancels every working order, then states a target — order matters. */
struct CancelThenTarget {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "clear");
    }
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("5");
    ctx.submit(target);
  }
};

/** Cancels nothing in particular, twice, so two unaddressed withdrawals reach the runner. */
struct CancelTwiceUnaddressed {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    // A default-constructed row carries a default-constructed reference, which is the unaddressed
    // one, without the test naming the broker tag.
    typename std::decay_t<decltype(ctx.portfolio().working_)>::value_type unaddressed;
    ctx.cancel(unaddressed.ref_, "first");
    ctx.cancel(unaddressed.ref_, "second");
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

/** Asks for a quarter of the portfolio, to show weight sizing needing a total. */
struct QuarterWeight {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_,
        OrderSide::BUY,
        strategy::SizeSpec::target_weight(Decimal64::parse("0.25"))
      )
    );
  }
};

/** Targets two instruments per bar, to show a hold on one not silencing the other. */
struct TwoNames {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    for (const char* symbol : {"005930", "000660"}) {
      strategy::TargetPosition target;
      target.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, symbol};
      target.target_quantity_ = Decimal64::parse("1");
      ctx.submit(target);
    }
  }
};

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

/** Buys the same instrument twice in one pass, each order affordable on its own. */
struct BuyTwice {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    for (int i = 0; i < 2; ++i) {
      ctx.submit(
        strategy::OrderIntent::market(
          bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10"))
        )
      );
    }
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

/** Sells ten once per bar, to prove an exit is not refused by an exposure ceiling. */
struct SellEachBar {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("10"))
      )
    );
  }
};

/** Targets flat on the first bar and long on the second, so the second opposes the first. */
struct FlipTarget {
  int bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = bars_++ == 0 ? Decimal64::parse("0") : Decimal64::parse("20");
    ctx.submit(target);
  }
};

/** Buys ten every bar, so two bars are two orders unless something remembers the first. */
struct BuyTen {
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

} // namespace

TEST_CASE("a dry run turns a target into the order it would have sent") {
  runner::DryRunner<HoldTen> dry;
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(report.would_place_[0].side_ == OrderSide::BUY);
  REQUIRE(report.would_place_[0].tag_ == "hold-ten");
  REQUIRE(report.rejected_.empty());
  REQUIRE(report.refused_.empty());
  REQUIRE(report.log_.size() == 1);
  REQUIRE(report.log_[0] == "targeted ten");
  REQUIRE(dry.strategy().bars_ == 1);
}

TEST_CASE("a bar advances the clock to its period end, not into the bar") {
  // Advancing to a timestamp that marks the period's open would place the runner inside a bar it
  // is being told about, which is the off-by-one that makes a backtest unreproducible.
  runner::DryRunner<HoldTen> dry;
  arrange(dry);
  const auto event = bar_at(10, "70000");

  dry.feed(event);
  REQUIRE(dry.context().now() == event.period_end_);
  REQUIRE(dry.context().now() == event.bar_.timestamp_);
}

TEST_CASE("a target already held asks for nothing on the next bar") {
  runner::DryRunner<HoldTen> dry;
  arrange(dry);
  strategy::Portfolio<runner::DryRunner<HoldTen>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.quiet());
}

TEST_CASE("cancellations are collected before anything would be placed") {
  // The sequencing Plan documents as the caller's obligation, and here is the caller. A
  // replacement placed before its predecessor is withdrawn is briefly a double position.
  using Tag = runner::DryRunner<CancelThenTarget>::Tag;
  runner::DryRunner<CancelThenTarget> dry;
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  WorkingOrder<Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("3");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.ref_ = OrderRef<Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE_FALSE(report.would_cancel_.empty());
  REQUIRE(report.would_place_.size() == 1);
  // Target five with three working and none held, so two more after the cancellation.
  REQUIRE(report.would_place_[0].quantity_ == Decimal64::parse("2"));
}

TEST_CASE("an order the strategy already withdrew is not withdrawn again by reconciliation") {
  // **The orders have to oppose the target, which the existing fixture's do not.** Reconciliation
  // withdraws a working row only when it works *against* the target
  // (`strategy/reconcile.hpp`), so the existing case -- one working buy of 3 under a buy target of
  // 5 -- plans no withdrawal at all and the explicit one goes alone. Two working *sells* under a
  // buy target both oppose it, so reconciliation plans a withdrawal for each, against a portfolio
  // that does not carry the two the strategy just cancelled. Four requests for two orders.
  using Tag = runner::DryRunner<CancelThenTarget>::Tag;
  runner::DryRunner<CancelThenTarget> dry;
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  // Stamped: an unstamped snapshot reads as never having been read, and a plan derived from one
  // is withheld before any of it is sent.
  portfolio.as_of_ = utc(2026, 3, 1, 6, 30);
  for (const char* id : {"0001", "0002"}) {
    WorkingOrder<Tag> working;
    working.order_.instrument_ = samsung();
    working.order_.side_ = OrderSide::SELL;
    working.order_.status_ = OrderStatus::PENDING;
    working.order_.quantity_ = Decimal64::parse("4");
    working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    working.ref_ = OrderRef<Tag>{id, ""};
    portfolio.working_.push_back(working);
  }
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  // One withdrawal per order. Before the dedupe this was four.
  REQUIRE(report.would_cancel_.size() == 2);
  REQUIRE(report.would_cancel_[0] == OrderRef<Tag>{"0001", ""});
  REQUIRE(report.would_cancel_[1] == OrderRef<Tag>{"0002", ""});
  // And nothing moved to the other buckets instead of being sent.
  REQUIRE(report.rejected_cancel_.empty());
  REQUIRE(report.unresolved_cancel_.empty());

  // **The placement is unchanged, which is the point of not touching the planning view.** Nothing
  // is held and nothing is working on the buy side, so the target of 5 is placed in full -- the
  // same order the previous code placed after sending twice as many withdrawals.
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].side_ == OrderSide::BUY);
  REQUIRE(report.would_place_[0].quantity_ == Decimal64::parse("5"));
}

TEST_CASE("two unaddressed withdrawals are both reported, not collapsed into one") {
  // The dedupe keys on the reference, and two empty references compare equal. Exempting them keeps
  // one report entry per instruction a caller issued, which is what an operator counts.
  runner::DryRunner<CancelTwiceUnaddressed> dry;
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.unresolved_cancel_.size() == 2);
}

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

TEST_CASE("the exposure ceiling sees what this pass has already sent") {
  // Two buys of ten at 70,000 in one pass, against a ceiling of a million. Each is 700,000 on its
  // own, so before this branch both passed -- the ceiling was compared against one order's notional
  // with the current exposure defaulting to zero. Together they are 1,400,000.
  strategy::RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("1000000");
  runner::DryRunner<BuyTwice> dry(BuyTwice{}, limits);
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(report.refused_[0].attempted_ == std::optional<Decimal64>(Decimal64::parse("1400000")));
}

TEST_CASE("the exposure ceiling carries into the next pass, with no poll between") {
  // The half a within-pass view cannot close. The reservation is persisted for this limit for the
  // same reason it is for the position ceiling: without it, ten bought on each of two bars passes
  // twice against a ceiling one of them should have reached, because the second pass starts from
  // the broker's last word again.
  strategy::RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("1000000");
  runner::DryRunner<BuyEachBar> dry(BuyEachBar{}, limits);
  arrange(dry);

  const auto first = dry.feed(bar_at(10, "70000"));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE(first.refused_.empty());

  // No portfolio replacement between the two, which is the case a poll would have resolved.
  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_place_.empty());
  REQUIRE(second.refused_.size() == 1);
  REQUIRE(second.refused_[0].reason_ == strategy::GuardReason::GROSS_EXPOSURE_EXCEEDED);
  REQUIRE(second.refused_[0].attempted_ == std::optional<Decimal64>(Decimal64::parse("1400000")));
}

TEST_CASE("an exposure ceiling never refuses the way out of a position over it") {
  // A ceiling that cannot be de-risked past is the failure mode this arithmetic exists to avoid.
  // 200 held at 70,000 is 14,000,000 against a ceiling of 5,000,000 -- already over, however it got
  // there -- and every sell out of it must still go.
  strategy::RiskLimits limits;
  limits.max_gross_exposure_ = Decimal64::parse("5000000");
  runner::DryRunner<SellEachBar> dry(SellEachBar{}, limits);
  using Tag = runner::DryRunner<SellEachBar>::Tag;
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("200");
  held.last_price_ = Decimal64::parse("70000");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.refused_.empty());
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].side_ == OrderSide::SELL);
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

TEST_CASE("a second sell in one pass is sized against the first, not against the same snapshot") {
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

TEST_CASE("a pass without the sellable check is unaffected by the view it now builds") {
  // The view is built for every pass, so the orders that do not consult it must be unchanged.
  runner::DryRunner<SellTwice> dry;
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<SellTwice>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 2);
  REQUIRE(report.refused_.empty());
}

TEST_CASE("a second buy in one pass spends what the first already committed") {
  // The money half of the same stale-view defect: one reading of a million, two buys of ten at
  // seventy thousand. Each is affordable against the reading alone; together they are not, and
  // nothing reduces the reading between them unless the pass does it.
  strategy::RiskLimits limits;
  limits.enforce_buying_power_ = true;
  runner::DryRunner<BuyTwice> dry(BuyTwice{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<BuyTwice>::Tag> portfolio;
  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("1000000"), Currency::krw()}, {}}
  );
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::INSUFFICIENT_BUYING_POWER);
  // 1,000,000 less the 700,000 already committed leaves 300,000 against another 700,000.
  REQUIRE(report.refused_[0].limit_ == Decimal64::parse("300000"));
  REQUIRE(report.refused_[0].attempted_ == Decimal64::parse("700000"));
}

TEST_CASE("a second buy in one pass counts against the position ceiling") {
  // The position half of the same stale-view defect, and the one a ceiling is most obviously for:
  // two buys of ten against a ceiling of fifteen. Each fits the *held* quantity of zero, so a
  // ceiling read from holdings alone passes both and builds twenty.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("15");
  runner::DryRunner<BuyTwice> dry(BuyTwice{}, limits);
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(report.refused_[0].limit_ == Decimal64::parse("15"));
  // Ten already working plus ten more, against a holding of nothing.
  REQUIRE(report.refused_[0].attempted_ == Decimal64::parse("20"));
}

TEST_CASE("an order resting from a previous pass counts against the position ceiling") {
  // Not only within one pass: a broker's own listing of what is outstanding is the same evidence,
  // and a ceiling that ignored it would let each bar add another order to the pile.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("15");
  runner::DryRunner<BuyTwice> dry(BuyTwice{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<BuyTwice>::Tag> portfolio;
  WorkingOrder<runner::DryRunner<BuyTwice>::Tag> resting;
  resting.order_.instrument_ = samsung();
  resting.order_.side_ = OrderSide::BUY;
  resting.order_.status_ = OrderStatus::PENDING;
  resting.order_.quantity_ = Decimal64::parse("10");
  resting.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  portfolio.working_.push_back(resting);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  // Ten are already in the market, so the first of the two new buys already breaches fifteen.
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 2);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(report.refused_[0].attempted_ == Decimal64::parse("20"));
}

TEST_CASE("an order accepted in an earlier pass still counts against the ceiling") {
  // The cross-pass half, for a caller who enabled *only* the ceiling. Nothing polls between the two
  // bars, so the broker's listing never mentions the first order; if the reservation is not
  // persisted for this limit, the second bar sees zero held and zero working and places again.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("15");
  runner::DryRunner<BuyTen> dry(BuyTen{}, limits);
  arrange(dry);

  const auto first = dry.feed(bar_at(10, "70000"));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE(first.refused_.empty());

  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_place_.empty());
  REQUIRE(second.refused_.size() == 1);
  REQUIRE(second.refused_[0].reason_ == strategy::GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(second.refused_[0].attempted_ == Decimal64::parse("20"));
}

TEST_CASE("a ceiling alone does not move the published buying power") {
  // The reservation exists to keep a limit honest, and it must not do anything a caller did not ask
  // for. `buying_power_` is a public figure a strategy may size its next order from, so a caller who
  // enabled the ceiling and nothing else must see it exactly where the broker left it.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("100");
  runner::DryRunner<BuyTen> dry(BuyTen{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<BuyTen>::Tag> portfolio;
  portfolio.buying_power_.push_back(
    BuyingPower{Money{Decimal64::parse("1000000"), Currency::krw()}, {}}
  );
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);

  const auto after = dry.context().portfolio().buying_power_in(Currency::krw());
  REQUIRE(after);
  REQUIRE(*after == Decimal64::parse("1000000"));

  // The working quantity is reserved either way, because that is what the ceiling reads.
  REQUIRE(
    dry.context().portfolio().working_quantity(samsung(), OrderSide::BUY) ==
    Decimal64::parse("10")
  );
}

TEST_CASE("a replay withdraws a reservation it never had a reference for") {
  // A reference is only meaningful to a broker. A replay's engine matches a cancellation on
  // instrument and side, so a reservation carrying none is still withdrawable there -- and must be,
  // or a queued order the caller asked to withdraw would fill anyway.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("100");
  runner::DryRunner<FlipTarget> dry(FlipTarget{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<FlipTarget>::Tag> portfolio;
  // Stamped: an unstamped snapshot reads as never having been read, and a plan derived from one
  // is withheld before any of it is sent.
  portfolio.as_of_ = utc(2026, 3, 1, 6, 30);
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  const auto first = dry.feed(bar_at(10, "70000"));
  REQUIRE(first.would_place_.size() == 1);

  // Flat, then long: the resting sell opposes the buy and has to go first.
  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_cancel_.size() == 1);
  REQUIRE(second.unresolved_cancel_.empty());
  REQUIRE(second.rejected_cancel_.empty());
  // And the reservation it withdrew is no longer counted against the ceiling on a third bar.
  REQUIRE(second.rejected_.empty());
}

TEST_CASE("a sell resting on the other side is not relief for a buy") {
  // Same-side only, deliberately. Counting a resting sell as relief for a buy assumes a fill that
  // may never come, and a ceiling exists for what happens when everything outstanding *does* fill.
  strategy::RiskLimits limits;
  limits.max_position_quantity_ = Decimal64::parse("15");
  runner::DryRunner<BuyTwice> dry(BuyTwice{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<BuyTwice>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("10");
  portfolio.positions_.push_back(held);
  WorkingOrder<runner::DryRunner<BuyTwice>::Tag> resting;
  resting.order_.instrument_ = samsung();
  resting.order_.side_ = OrderSide::SELL;
  resting.order_.status_ = OrderStatus::PENDING;
  resting.order_.quantity_ = Decimal64::parse("10");
  resting.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  portfolio.working_.push_back(resting);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  // Ten held plus ten bought is twenty, whatever the resting sell may later do about it.
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 2);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::POSITION_LIMIT_EXCEEDED);
  REQUIRE(report.refused_[0].attempted_ == Decimal64::parse("20"));
}

TEST_CASE("the probe learns the broker's sellable convention from ordinary polls") {
  // The point of the probe: no experiment, no extra request, no order placed for the sake of
  // measuring. It reads the portfolios the runner is fed anyway.
  runner::DryRunner<HoldTen> dry;
  arrange(dry);
  dry.probe_sellable_convention();

  using Tag = runner::DryRunner<HoldTen>::Tag;
  strategy::Portfolio<Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);
  dry.feed(bar_at(10, "70000"));
  REQUIRE(dry.sellable_convention().verdict() == strategy::SellableConvention::UNDETERMINED);

  // A sell of thirty is now resting, as a later poll reports it, and the figure came back lower by
  // exactly thirty.
  WorkingOrder<Tag> resting;
  resting.order_.instrument_ = samsung();
  resting.order_.side_ = OrderSide::SELL;
  resting.order_.status_ = OrderStatus::PENDING;
  resting.order_.quantity_ = Decimal64::parse("30");
  resting.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  portfolio.working_.push_back(resting);
  portfolio.positions_[0].sellable_quantity_ = Decimal64::parse("70");
  dry.context().set_portfolio(portfolio);
  dry.feed(bar_at(11, "70000"));

  REQUIRE(dry.sellable_convention().verdict() == strategy::SellableConvention::NET_OF_RESTING);
  REQUIRE(dry.sellable_convention().overall().net_ == 1);
}

TEST_CASE("the probe is off by default and changes nothing when on") {
  using Tag = runner::DryRunner<HoldTen>::Tag;
  strategy::Portfolio<Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);

  runner::DryRunner<HoldTen> quiet;
  arrange(quiet);
  quiet.context().set_portfolio(portfolio);
  const auto without = quiet.feed(bar_at(10, "70000"));
  // Off by default: it never looked, so it knows nothing.
  REQUIRE(quiet.sellable_convention().overall().total() == 0);

  runner::DryRunner<HoldTen> probing;
  arrange(probing);
  probing.probe_sellable_convention();
  probing.context().set_portfolio(portfolio);
  const auto with = probing.feed(bar_at(10, "70000"));

  // A run that observes places exactly what a run that does not would have placed.
  REQUIRE(with.would_place_.size() == without.would_place_.size());
  REQUIRE(with.refused_.size() == without.refused_.size());
  REQUIRE(with.would_place_.size() == 1);
}

TEST_CASE("a second pass cannot re-sell what the first pass already committed") {
  // The in-pass view dies with the pass. Without carrying the reservation forward, a second bar
  // before the next poll starts from the broker's last word again -- a hundred sellable, nothing
  // resting -- and sells the same shares twice.
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  runner::DryRunner<SellSixty> dry(SellSixty{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  const auto first = dry.feed(bar_at(10, "70000"));
  REQUIRE(first.would_place_.size() == 1);

  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_place_.empty());
  REQUIRE(second.refused_.size() == 1);
  REQUIRE(second.refused_[0].reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
  REQUIRE(second.refused_[0].attempted_ == Decimal64::parse("120"));
}

TEST_CASE("a poll replaces a reservation rather than adding to it") {
  // Why this needs no matcher: a working-order poll assigns `working_` wholesale, so the broker's
  // own listing discards the reservation instead of being counted beside it. A runner that added
  // them would refuse a legitimate sell for as long as the order rested.
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;
  runner::DryRunner<SellSixty> dry(SellSixty{}, limits);
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  held.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  dry.feed(bar_at(10, "70000"));
  REQUIRE(dry.context().portfolio().working_.size() == 1);

  // The broker's own answer arrives: sixty sold and settled, forty left, nothing resting.
  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> polled;
  HoldingPosition after;
  after.instrument_ = samsung();
  after.quantity_ = Decimal64::parse("40");
  after.sellable_quantity_ = Decimal64::parse("40");
  polled.positions_.push_back(after);
  dry.context().set_portfolio(polled);

  // Forty sellable against a sixty-share order, and the reservation is gone rather than doubled.
  const auto next = dry.feed(bar_at(11, "70000"));
  REQUIRE(next.refused_.size() == 1);
  REQUIRE(next.refused_[0].attempted_ == Decimal64::parse("60"));
  REQUIRE(next.refused_[0].limit_ == Decimal64::parse("40"));
}

TEST_CASE("a runner without the checks carries no reservation between passes") {
  // The write-through reaches reconciliation as well, so it is scoped to callers who asked for this
  // class of check rather than changing what every existing program does on upgrade.
  runner::DryRunner<SellSixty> dry;
  arrange(dry);

  strategy::Portfolio<runner::DryRunner<SellSixty>::Tag> portfolio;
  HoldingPosition held;
  held.instrument_ = samsung();
  held.quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(held);
  dry.context().set_portfolio(portfolio);

  dry.feed(bar_at(10, "70000"));
  const auto second = dry.feed(bar_at(11, "70000"));

  REQUIRE(dry.context().portfolio().working_.empty());
  REQUIRE(second.would_place_.size() == 1);
}

TEST_CASE("withdrawals spend the rate window a later placement is checked against") {
  // The asymmetry this branch absorbed: a placement recorded a send on three of its four outcomes
  // and a cancellation recorded none, so a withdrawal of many resting orders was many requests the
  // broker counted and the guard did not. The next placement was then checked against a window it
  // believed was empty.
  using Tag = runner::DryRunner<CancelThenTarget>::Tag;
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  runner::DryRunner<CancelThenTarget> dry(CancelThenTarget{}, limits);
  arrange(dry);

  strategy::Portfolio<Tag> portfolio;
  WorkingOrder<Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("3");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.ref_ = OrderRef<Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(10, "70000"));

  // The withdrawal went and was not refused: a rate limit must never be why an order cannot be
  // withdrawn, and `send_cancel` consults no limit at all.
  REQUIRE(report.would_cancel_.size() == 1);
  REQUIRE(report.refused_cancel_.empty());

  // And they spent the window, so the placement that follows them is refused by it.
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::ORDER_RATE_EXCEEDED);
}

TEST_CASE("the probe does not learn the broker's convention from this runner's own reservation") {
  // Found in review, and it would have made the probe worse than useless: with the sellable check
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

TEST_CASE("a guard refusal is reported and no order survives it") {
  strategy::RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("100000");
  runner::DryRunner<HoldTen> dry(HoldTen{}, limits);
  arrange(dry);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  REQUIRE(report.refused_[0].tag_ == "hold-ten");
}

TEST_CASE("the kill switch stops a dry run as it would a live one") {
  runner::DryRunner<HoldTen> dry;
  arrange(dry);
  dry.guard().halt();

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::TRADING_HALTED);
}

TEST_CASE("the market phase the runner is in gates the order") {
  runner::DryRunner<HoldTen> dry;
  arrange(dry);
  dry.context().set_phase(strategy::MarketPhase::POST_CLOSE);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED);
}

TEST_CASE("an instrument the runner has not loaded is rejected, not guessed at") {
  runner::DryRunner<HoldTen> dry;
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  // Deliberately no reference data: without a tick grid and a lot size there is nothing to build.

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.rejected_.size() == 1);
  REQUIRE(report.rejected_[0].reason_ == strategy::RejectReason::NO_REFERENCE_DATA);
}

TEST_CASE("rate budget is consumed across bars, as it will be in production") {
  // A dry run records the send a live runner would perform, so the rate limit behaves the same
  // here rather than being permanently unused and therefore untested until production.
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  // Wider than the gap between the two bars on purpose. A window of exactly one day against bars
  // exactly a day apart would let the first send age out at the boundary -- correct behaviour for
  // a half-open window, and a fixture that would have tested nothing.
  limits.rate_window_ = std::chrono::hours(48);
  runner::DryRunner<HoldTen> dry(HoldTen{}, limits);
  arrange(dry);

  REQUIRE(dry.feed(bar_at(10, "70000")).would_place_.size() == 1);
  // The position is still flat in this stub portfolio, so the strategy asks again -- and the rate
  // limit is what stops it.
  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_place_.empty());
  REQUIRE(second.refused_.size() == 1);
  REQUIRE(second.refused_[0].reason_ == strategy::GuardReason::ORDER_RATE_EXCEEDED);
}

TEST_CASE("a price band recorded on the runner clamps the order it produces") {
  runner::DryRunner<LimitAt80k> dry;
  arrange(dry);
  PriceLimits band;
  band.upper_ = Decimal64::parse("71000");
  dry.set_price_limits(samsung(), band);

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].price_ == std::optional<Decimal64>(Decimal64::parse("71000")));
  REQUIRE(report.would_place_[0].price_clamped_);
}

TEST_CASE("a weighted intent needs the portfolio value the runner was given") {
  runner::DryRunner<QuarterWeight> dry;
  arrange(dry);
  const auto without = dry.feed(bar_at(10, "70000"));
  REQUIRE(without.rejected_.size() == 1);
  REQUIRE(without.rejected_[0].reason_ == strategy::RejectReason::NO_PORTFOLIO_VALUE);

  dry.set_portfolio_value(Decimal64::parse("10000000"));
  const auto with = dry.feed(bar_at(11, "70000"));
  REQUIRE(with.would_place_.size() == 1);
  REQUIRE(with.would_place_[0].quantity_ == Decimal64::parse("35"));
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

TEST_CASE("the queue keeps the order a strategy asked in") {
  // A single ordered queue rather than one list per kind, because a cancellation followed by a
  // target means something different from the reverse.
  runner::Context<runner::DryRunner<HoldTen>::Tag, strategy::NoCaps> context;
  using Tag = runner::DryRunner<HoldTen>::Tag;

  context.cancel(OrderRef<Tag>{"0001", ""}, "first");
  strategy::TargetPosition target;
  target.instrument_ = samsung();
  target.target_quantity_ = Decimal64::parse("1");
  context.submit(target);

  const auto queued = context.drain();
  REQUIRE(queued.size() == 2);
  REQUIRE(std::holds_alternative<strategy::CancelIntent<Tag>>(queued[0]));
  REQUIRE(std::holds_alternative<strategy::TargetPosition>(queued[1]));
  REQUIRE(context.queued().empty());
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
// Transport indeterminacy: the most important behaviour in the branch
// ---------------------------------------------------------------------------

namespace {

using runner::IndeterminacyLedger;
using runner::SendOutcome;
using runner::WithheldReason;

/** An executor that reports a chosen outcome, so the failure paths are reachable without a broker. */
template <typename Tag>
class ScriptedExecutor {
public:
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

TEST_CASE("an order sent under a phase the limits refuse is counted") {
  // The witness for the closing exception. It is the library sending into a session the operator's
  // own `permitted_phases_` said not to trade in, which is correct and must not be invisible.
  FlattenRunner runner_instance;
  arrange(runner_instance);
  runner_instance.context().set_phase(strategy::MarketPhase::CLOSED);
  runner_instance.context().set_portfolio(holding_a_hundred());
  REQUIRE(runner_instance.sends_outside_permitted_phase() == 0);

  const auto report = runner_instance.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(runner_instance.sends_outside_permitted_phase() == 1);
}

TEST_CASE("the same close inside the session is not counted") {
  // The other half, and without it the counter could be "orders sent" and the test above would
  // still pass. The phase is permitted here, so no exception was used and nothing is counted.
  FlattenRunner runner_instance;
  arrange(runner_instance);  // leaves the phase at OPEN
  runner_instance.context().set_portfolio(holding_a_hundred());

  const auto report = runner_instance.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(runner_instance.sends_outside_permitted_phase() == 0);
}

TEST_CASE("a broker rejecting an out-of-hours close still counts as a send") {
  // **The case the counter exists for.** Outside a session a rejection is the likeliest answer to a
  // request that did go out -- that is the cost option 2 was chosen with -- so a witness that
  // counted only acceptances would read zero through every one of them and be silent in exactly the
  // situation it was added to report. `Runner::settle` already treats a rejection as a request the
  // broker counted, which is why it spends rate-limit budget.
  FlattenRunner runner_instance;
  arrange(runner_instance);
  runner_instance.context().set_phase(strategy::MarketPhase::CLOSED);
  runner_instance.context().set_portfolio(holding_a_hundred());
  runner_instance.executor().place_outcome_ = SendOutcome::REJECTED;

  const auto report = runner_instance.feed(bar_at(10, "70000"));

  REQUIRE(report.would_place_.empty());
  REQUIRE(report.rejected_.size() == 1);
  REQUIRE(runner_instance.sends_outside_permitted_phase() == 1);
}

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

TEST_CASE("a close a later limit refuses is not counted, because it was never sent") {
  // **This is what makes the counter a measure of the harm rather than a proxy for it.** The phase
  // check is third in the guard's ordering, so an order it admits still faces every ceiling after
  // it. Counting at the admission would report this order, which went nowhere. The test fails
  // against a counter incremented inside `RiskGuard`, which is the implementation it exists to rule
  // out.
  strategy::RiskLimits limits;
  limits.max_order_notional_ = Decimal64::parse("1000");
  FlattenRunner runner_instance(FlattenSamsung{}, limits);
  arrange(runner_instance);
  runner_instance.context().set_phase(strategy::MarketPhase::CLOSED);
  runner_instance.context().set_portfolio(holding_a_hundred());

  const auto report = runner_instance.feed(bar_at(10, "70000"));

  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::ORDER_NOTIONAL_TOO_LARGE);
  REQUIRE(runner_instance.executor().placed().empty());
  REQUIRE(runner_instance.sends_outside_permitted_phase() == 0);
}


TEST_CASE("the ledger only forgets after the broker's order list has been read") {
  // Not on a timeout, not on a later success, and not on a subsequent bar. A timeout would resume
  // trading on a guess; a success on a different instruction says nothing about the one that
  // vanished.
  IndeterminacyLedger ledger;
  REQUIRE(ledger.empty());
  REQUIRE_FALSE(ledger.requires_reconcile(samsung()));

  ledger.mark(samsung(), "an order request failed in transit");
  REQUIRE(ledger.requires_reconcile(samsung()));
  REQUIRE(ledger.detail(samsung()) == "an order request failed in transit");
  REQUIRE(ledger.pending().size() == 1);

  // A second failure keeps the first reason: the earliest unresolved instruction is the one whose
  // existence is in doubt, and a later failure adds nothing about it.
  ledger.mark(samsung(), "a later, different failure");
  REQUIRE(ledger.pending().size() == 1);
  REQUIRE(ledger.detail(samsung()) == "an order request failed in transit");

  // Another instrument is unaffected: the hold is per instrument, not global.
  REQUIRE_FALSE(ledger.requires_reconcile(InstrumentId{Country::KR, Venue::COMBINED, "000660"}));

  ledger.resolve_after_reading_orders(samsung());
  REQUIRE(ledger.empty());
  REQUIRE_FALSE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("an order that fails in transit leaves the instrument requiring reconciliation") {
  ScriptedRunner dry;
  arrange(dry);
  dry.executor().place_outcome_ = SendOutcome::INDETERMINATE;

  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.indeterminate_.size() == 1);
  REQUIRE(report.would_place_.empty());
  REQUIRE(dry.ledger().requires_reconcile(samsung()));
}

TEST_CASE("nothing further is submitted for an instrument whose state is unknown") {
  // **The rule this whole mechanism exists for.** The order may or may not be live, so sending
  // another could double the position. It is withheld and reported, never retried.
  ScriptedRunner dry;
  arrange(dry);
  dry.executor().place_outcome_ = SendOutcome::INDETERMINATE;
  REQUIRE(dry.feed(bar_at(10, "70000")).indeterminate_.size() == 1);

  // The transport recovers, and it changes nothing: the question is about the earlier order.
  dry.executor().place_outcome_ = SendOutcome::ACCEPTED;
  const auto second = dry.feed(bar_at(11, "70000"));
  REQUIRE(second.would_place_.empty());
  REQUIRE(second.withheld_.size() == 1);
  REQUIRE(second.withheld_[0].reason_ == WithheldReason::RECONCILE_REQUIRED);
  REQUIRE(second.withheld_[0].detail_ == "an order request failed in transit");
  // And it reached the executor exactly once, on the failing attempt -- never again.
  REQUIRE(dry.executor().placed().size() == 1);
}

TEST_CASE("reading the broker's order list is what allows trading to resume") {
  ScriptedRunner dry;
  arrange(dry);
  dry.executor().place_outcome_ = SendOutcome::INDETERMINATE;
  dry.feed(bar_at(10, "70000"));
  dry.executor().place_outcome_ = SendOutcome::ACCEPTED;
  REQUIRE(dry.feed(bar_at(11, "70000")).withheld_.size() == 1);

  // An operator, or the runner's own reconciliation step, having read the order list:
  dry.ledger().resolve_after_reading_orders(samsung());
  const auto resumed = dry.feed(bar_at(12, "70000"));
  REQUIRE(resumed.would_place_.size() == 1);
  REQUIRE(resumed.withheld_.empty());
}

TEST_CASE("a hold on one instrument does not stop trading in another") {
  // A cross-sectional strategy must not be silenced entirely because one name is unresolved.
  runner::Runner<TwoNames, strategy::NoCaps, runner::DryTag, ScriptedExecutor<runner::DryTag>> dry;
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  for (const char* symbol : {"005930", "000660"}) {
    auto reference = samsung_reference();
    reference.id_ = InstrumentId{Country::KR, Venue::COMBINED, symbol};
    dry.context().set_instrument(reference);
    dry.context().set_last_price(reference.id_, Decimal64::parse("70000"));
  }

  dry.ledger().mark(samsung(), "earlier failure");
  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(report.withheld_.size() == 1);
  REQUIRE(report.withheld_[0].order_.instrument_.symbol_ == "005930");
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.would_place_[0].instrument_.symbol_ == "000660");
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

TEST_CASE("a cancellation that fails in transit also obliges reconciliation") {
  // The order may now be withdrawn or may still be resting, which is the same question an order
  // POST leaves.
  using Tag = runner::DryTag;
  runner::Runner<CancelThenTarget, strategy::NoCaps, Tag, ScriptedExecutor<Tag>> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  strategy::Portfolio<Tag> portfolio;
  WorkingOrder<Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("3");
  working.ref_ = OrderRef<Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);

  dry.executor().cancel_outcome_ = SendOutcome::INDETERMINATE;
  const auto report = dry.feed(bar_at(10, "70000"));
  REQUIRE(dry.ledger().requires_reconcile(samsung()));
  REQUIRE(dry.ledger().detail(samsung()) == "a cancellation request failed in transit");
  // The order that would have followed the cancellation is withheld, not sent into the doubt.
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.withheld_.size() == 1);
  // And the cancellation itself does not read as one that happened. The order may be withdrawn or
  // may still be resting; `would_cancel_` claims the first, so an unknown outcome must not go there.
  REQUIRE(report.would_cancel_.empty());
  REQUIRE(report.indeterminate_cancel_.size() == 1);
  REQUIRE(report.indeterminate_cancel_.front().primary_ == "0001");
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

TEST_CASE("the asymmetry is the point: an unknown classification errs toward reconciling") {
  // Calling an indeterminate outcome a rejection risks an unwatched live order and a doubled
  // position. Calling a rejection indeterminate costs one reconciliation. Only one loses money, so
  // a category this library does not recognize is treated as unknown.
  BrokerError error;
  error.category_ = static_cast<ErrorCategory>(99);
  REQUIRE(runner::outcome_of(error) == SendOutcome::INDETERMINATE);
}

TEST_CASE("a cancellation the broker rejected is not reported as a withdrawal") {
  // A refusal is an answer about the request: it was not accepted, so nothing needs reconciling.
  // `would_cancel_` says the broker took the withdrawal, which would be false. It is not evidence
  // the order is working either -- a fill is an ordinary reason a withdrawal is refused.
  using Tag = runner::DryTag;
  runner::Runner<CancelThenTarget, strategy::NoCaps, Tag, ScriptedExecutor<Tag>> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  strategy::Portfolio<Tag> portfolio;
  WorkingOrder<Tag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("3");
  working.ref_ = OrderRef<Tag>{"0001", ""};
  portfolio.working_.push_back(working);
  dry.context().set_portfolio(portfolio);

  dry.executor().cancel_outcome_ = SendOutcome::REJECTED;
  const auto report = dry.feed(bar_at(10, "70000"));

  REQUIRE(report.rejected_cancel_.size() == 1);
  REQUIRE(report.rejected_cancel_.front().ref_.primary_ == "0001");
  // The broker answered, so the entry says so: the other way into this bucket is a withdrawal the
  // binding refused to send, and the two differ in whether a request went out at all.
  REQUIRE(report.rejected_cancel_.front().by_broker_);
  REQUIRE(report.would_cancel_.empty());
  // Unlike an indeterminate one, a rejection leaves nothing in doubt.
  REQUIRE_FALSE(dry.ledger().requires_reconcile(samsung()));
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

TEST_CASE("a fresh snapshot within the stated age plans and sends as before") {
  // The other side of all three triggers, so the rule is not simply "never act".
  runner::DryRunner<FlatTargetStrategy> dry;
  arrange(dry);
  auto portfolio = resting_sell(/*stamped=*/true);
  dry.context().set_portfolio(portfolio);
  dry.set_maximum_portfolio_age(std::chrono::hours(24 * 365));

  const auto report = dry.feed(bar_at(10, "70000"));

  CHECK(report.withheld_plans_.empty());
  REQUIRE(report.would_cancel_.size() == 1);
  CHECK(report.would_cancel_.front() == OrderRef<runner::DryTag>{"0001", ""});
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
  // The half of this branch that *permits* rather than refuses, and the reason the two halves could
  // not ship apart: an order that cannot be withdrawn keeps resting and can fill, which is worse
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

TEST_CASE("a hold somebody set still refuses a named withdrawal") {
  // The kill switch, intact. One case per reason rather than one test over all three, so a future
  // reclassification of any single reason is a failure here rather than a silent widening.
  runner::DryRunner<WithdrawsOne> dry;
  arrange(dry);
  dry.context().set_portfolio(resting_sell(/*stamped=*/true));

  SECTION("an operator's halt refuses it, and says which hold did") {
    dry.guard().halt();
    const auto report = dry.feed(bar_at(10, "70000"));
    CHECK(report.would_cancel_.empty());
    REQUIRE(report.refused_cancel_.size() == 1);
    CHECK(report.refused_cancel_.front().reason_ == strategy::HaltReason::OPERATOR);
  }

  SECTION("a stop in progress refuses it") {
    dry.guard().halt(strategy::HaltReason::STOPPING);
    const auto report = dry.feed(bar_at(10, "70000"));
    CHECK(report.would_cancel_.empty());
    REQUIRE(report.refused_cancel_.size() == 1);
    CHECK(report.refused_cancel_.front().reason_ == strategy::HaltReason::STOPPING);
  }

  SECTION("a local failure reports it as unavailable, not refused") {
    // A different bucket, and the distinction is real: nothing was asked of the broker, because the
    // machinery that would have recorded the request is what failed.
    dry.guard().halt(strategy::HaltReason::LOCAL_FAILURE);
    const auto report = dry.feed(bar_at(10, "70000"));
    CHECK(report.would_cancel_.empty());
    CHECK(report.refused_cancel_.empty());
    CHECK(report.unavailable_cancel_.size() == 1);
  }
}

TEST_CASE("a hold somebody set outranks one that only means the runner is uninformed") {
  // Both in force at once. The permissive rule must not be able to let an instruction through a
  // kill switch merely because a second, weaker hold is also present.
  runner::DryRunner<WithdrawsOne> dry;
  arrange(dry);
  dry.context().set_portfolio(resting_sell(/*stamped=*/false));
  dry.guard().halt(strategy::HaltReason::PORTFOLIO_ABSENT);
  dry.guard().halt();

  const auto report = dry.feed(bar_at(10, "70000"));

  CHECK(report.would_cancel_.empty());
  REQUIRE(report.refused_cancel_.size() == 1);
  CHECK(report.refused_cancel_.front().reason_ == strategy::HaltReason::OPERATOR);
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

namespace {

/** Wants five long, so a larger resting buy has to be withdrawn and replaced. */
struct WantsFive {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("5");
    target.tag_ = "resize";
    ctx.submit(target);
  }
};

/** A portfolio carrying a resting BUY larger than the target, stamped and current. */
strategy::Portfolio<runner::DryTag> oversized_resting_buy() {
  strategy::Portfolio<runner::DryTag> portfolio;
  portfolio.as_of_ = utc(2026, 3, 10, 6, 30);
  WorkingOrder<runner::DryTag> working;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.status_ = OrderStatus::PENDING;
  working.order_.quantity_ = Decimal64::parse("10");
  working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  working.ref_ = OrderRef<runner::DryTag>{"0002", ""};
  portfolio.working_.push_back(working);
  return portfolio;
}

} // namespace

TEST_CASE("a resize is withheld whole when the hold would refuse its replacement half") {
  // The shape the plan calls "a placement wearing an amendment's clothes", and the one the
  // cancel-only cases cannot exercise: this plan really does have both halves. Sending the
  // withdrawal and then having the replacement refused turns a resize into a bare cancellation,
  // which is not the instruction the strategy stated.
  auto uninformed = GENERATE(
    strategy::HaltReason::ORDER_LIST_UNREAD,
    strategy::HaltReason::BUYING_POWER_UNREAD);

  runner::DryRunner<WantsFive> dry;
  arrange(dry);
  dry.context().set_portfolio(oversized_resting_buy());
  dry.guard().halt(uninformed);

  const auto report = dry.feed(bar_at(10, "70000"));

  CAPTURE(strategy::halt_reason_name(uninformed));
  REQUIRE(report.withheld_plans_.size() == 1);
  const auto& withheld = report.withheld_plans_.front();
  CHECK(withheld.reason_ == runner::WithheldPlanReason::PLACEMENT_HELD);
  CHECK(withheld.detail_ == strategy::halt_reason_name(uninformed));
  CHECK(withheld.tag_ == "resize");
  // Both halves are named, and they are told apart by the counts rather than by two entries.
  REQUIRE(withheld.not_canceled_.size() == 1);
  CHECK(withheld.not_canceled_.front() == OrderRef<runner::DryTag>{"0002", ""});
  CHECK(withheld.not_placed_ == 1);
  // Nothing escaped in either direction, and no other bucket absorbed it.
  CHECK(report.would_cancel_.empty());
  CHECK(report.would_place_.empty());
  CHECK(report.refused_.empty());
  CHECK(report.withheld_.empty());
  CHECK_FALSE(report.quiet());
}

TEST_CASE("an unheld resize still cancels and replaces, so the rule is not simply never act") {
  runner::DryRunner<WantsFive> dry;
  arrange(dry);
  dry.context().set_portfolio(oversized_resting_buy());

  const auto report = dry.feed(bar_at(10, "70000"));

  CHECK(report.withheld_plans_.empty());
  REQUIRE(report.would_cancel_.size() == 1);
  REQUIRE(report.would_place_.size() == 1);
  CHECK(report.would_place_.front().quantity_ == Decimal64::parse("5"));
}
