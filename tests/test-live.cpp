#include <algotradelib/kis>
#include <algotradelib/live>
#include <algotradelib/tossinvest>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using namespace algotradelib;

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;
namespace live = algotradelib::live;

namespace {

/** Compile-time identity of the fake broker. */
struct FakeBroker {
  static constexpr std::string_view name = "fake";
};

/** Capabilities a fake session can be given, to exercise both sides of a gate. */
struct WithKey : strategy::NoCaps {
  static constexpr bool has_client_order_id = true;
};
struct WithoutKey : strategy::NoCaps {
  static constexpr bool has_client_order_id = false;
};

/**
 * Capability variants for the working-order inquiry.
 *
 * `SessionSource::read_order_list` branches on `has_working_orders` at compile time -- working orders
 * where the broker offers them, order history where it does not -- so both branches need a broker to
 * be instantiated against, or only one of them is ever compiled.
 */
struct WithWorkingOrders : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
};
struct WithoutWorkingOrders : strategy::NoCaps {
  static constexpr bool has_working_orders = false;
};
/** A broker whose holdings already carry the sellable figure, as KIS's do. */
struct SellableInHoldings : strategy::NoCaps {
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;
};
/** A broker that answers a per-instrument inquiry, as Toss does. */
struct SellableByInquiry : strategy::NoCaps {
  static constexpr SellableSource sellable_source = SellableSource::INQUIRY;
  // Declares what Toss declares, because this stands for Toss. An inquiry answer carries its own
  // basis rather than inheriting the holdings read's -- the poll established nothing about a figure
  // it never reported -- so a capability set that says nothing here has its checked sells refused.
  static constexpr SellableBasis sellable_inquiry_basis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
};
/** A broker that publishes what may be spent, per currency, as both of them do. */
struct WithBuyingPower : strategy::NoCaps {
  static constexpr bool has_buying_power = true;
  static constexpr bool has_working_orders = true;
};

/**
 * Capability variants for the trading calendar, for the same reason as the pair above.
 *
 * `SessionSource::poll_calendar` branches on `has_calendar` at compile time, and a branch nothing
 * instantiates is a branch the compiler has never checked -- so both need a session to be built
 * against. Both shipped brokers publish a calendar, so the `false` side is the one that would
 * otherwise go uncompiled.
 */
struct WithCalendar : strategy::NoCaps {
  static constexpr bool has_calendar = true;
  static constexpr bool has_working_orders = true;
};
struct WithoutCalendar : strategy::NoCaps {
  static constexpr bool has_calendar = false;
  static constexpr bool has_working_orders = true;
};

/**
 * A session that records what it was asked and answers however a test says.
 *
 * `SessionExecutor` is duck-typed on the session, so this needs no broker adapter and no transport:
 * the live send path is exercised end to end with nothing on the wire. A transport failure mid-POST
 * is not something a real broker can be asked for, which is the whole reason this seam exists.
 */
template <typename CapsType>
class FakeSession {
public:
  using Caps = CapsType;
  using BrokerTag = FakeBroker;
  using Ref = OrderRef<FakeBroker>;
  using Placed = PlacedOrder<FakeBroker>;

  Result<Placed> place(OrderRequest order) {
    if (local_refusal_) {
      // Refused before the request is recorded, because a client refusing locally sends nothing.
      auto refusal = *local_refusal_;
      local_refusal_.reset();
      return Result<Placed>(refusal);
    }
    placed_.push_back(order);
    if (observed_ != nullptr) {
      journal_at_send_ = observed_->records();
    }
    if (error_) {
      return Result<Placed>(*error_);
    }
    Placed ack;
    ack.ack_.order_id_ = "0001569139";
    ack.ref_ = Ref{"0001569139", ""};
    return Result<Placed>(std::move(ack));
  }

  Result<Placed> cancel(const Ref& ref, const AmendInstruction& instruction) {
    canceled_.push_back(instruction);
    canceled_refs_.push_back(ref);
    if (error_) {
      return Result<Placed>(*error_);
    }
    Placed ack;
    ack.ref_ = ref;
    return Result<Placed>(std::move(ack));
  }

  /** Answers the sellable inquiry, counting how often it was asked. */
  Result<Decimal64> sellable_quantity(const InstrumentId& instrument) {
    sellable_asked_.push_back(instrument);
    if (error_) {
      return Result<Decimal64>(*error_);
    }
    return Result<Decimal64>(sellable_);
  }

  /** Sets what the sellable inquiry answers. */
  void set_sellable(const char* quantity) { sellable_ = Decimal64::parse(quantity); }

  /** Returns every instrument the sellable inquiry was asked about. */
  const std::vector<InstrumentId>& sellable_asked() const noexcept { return sellable_asked_; }

  /** Makes every later call fail with `category` and `status`. */
  void fail_with(ErrorCategory category, int status = 0) {
    BrokerError error;
    error.category_ = category;
    error.http_status_ = status;
    error_ = error;
  }

  /**
   * Makes the next call fail the way a client's own validation does: before any I/O.
   *
   * The real brokers do this for an order id too long for their format, a time in force they do
   * not support, and a dozen other shapes -- all `INVALID_ARGUMENT`, none of them a broker's
   * answer. `once` because the case worth testing is a bad instruction followed by a good one.
   */
  void refuse_next_locally() {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_client_order_id";
    error.message_ = "this client refused the instruction before sending it";
    local_refusal_ = error;
  }

  /**
   * Makes the next call fail the way a token refresh does: the *prerequisite* request failed, and
   * the caller's own never went.
   *
   * `AUTHENTICATION`, which a category test alone reads as a broker's refusal of the order --
   * though the order was never built. Both clients mark such a failure before handing it to the
   * callers waiting behind the refresh.
   */
  void fail_next_token(ErrorCategory category = ErrorCategory::AUTHENTICATION) {
    BrokerError error;
    error.category_ = category;
    error.reached_broker_ = false;
    error.broker_code_ = "token_refresh_failed";
    error.message_ = "the token request failed, so this instruction was never sent";
    local_refusal_ = error;
  }

  void succeed() { error_.reset(); }

  std::vector<InstrumentId> sellable_asked_;
  Decimal64 sellable_{Decimal64::parse("0")};

  const std::vector<OrderRequest>& placed() const noexcept { return placed_; }
  const std::vector<AmendInstruction>& canceled() const noexcept { return canceled_; }
  const std::vector<Ref>& canceled_refs() const noexcept { return canceled_refs_; }

  // ---- the read side, which `SessionSource` polls ----

  /** Returns the scripted bar series, counting the call. */
  Result<BarSeries> bars(const BarQuery& query) {
    ++bar_calls_;
    if (error_) {
      return Result<BarSeries>(*error_);
    }
    BarSeries series = bars_;
    series.instrument_ = query.instrument_;
    return Result<BarSeries>(std::move(series));
  }

  /** Returns the scripted quote, counting the call. */
  Result<QuoteSnapshot> quote(const InstrumentId& instrument) {
    ++quote_calls_;
    if (error_) {
      return Result<QuoteSnapshot>(*error_);
    }
    QuoteSnapshot snapshot = quote_;
    snapshot.instrument_ = instrument;
    return Result<QuoteSnapshot>(std::move(snapshot));
  }

  /** Returns the scripted holdings, counting the call. */
  Result<HoldingsSnapshot> holdings() {
    ++holdings_calls_;
    if (error_) {
      return Result<HoldingsSnapshot>(*error_);
    }
    return Result<HoldingsSnapshot>(holdings_);
  }

  /**
   * Returns the scripted trading calendar, recording which market was asked about.
   *
   * The market is recorded because only one of the two shipped brokers serves more than one, and a
   * source asking about the wrong country would answer confidently about the wrong holidays.
   */
  Result<TradingCalendar> calendar(Country country, std::optional<Date> from = std::nullopt) {
    ++calendar_calls_;
    calendar_asked_ = country;
    calendar_from_ = from;
    if (calendar_error_) {
      return Result<TradingCalendar>(*calendar_error_);
    }
    return Result<TradingCalendar>(calendar_);
  }

  /** Scripts the calendar every request answers with. */
  void set_calendar(TradingCalendar calendar) { calendar_ = std::move(calendar); }

  /** Scripts a calendar request failing with `category`. */
  void fail_calendar(ErrorCategory category) {
    BrokerError error;
    error.category_ = category;
    error.broker_code_ = "calendar_unavailable";
    calendar_error_ = error;
  }

  /** Lets the calendar answer again. */
  void heal_calendar() { calendar_error_.reset(); }

  /** Returns how many calendar requests were made. */
  int calendar_calls() const noexcept { return calendar_calls_; }

  /** Returns which market the last one named. */
  std::optional<Country> calendar_asked() const noexcept { return calendar_asked_; }

  /** Returns the date the last one named, which the source deliberately leaves to the broker. */
  const std::optional<Date>& calendar_from() const noexcept { return calendar_from_; }

  /**
   * Returns what may be spent in `currency`, or the failure a test asked for in that one currency.
   *
   * Per currency rather than session-wide, because the case worth testing is the mixed one: a
   * broker answering for the account's home currency and failing for the other is not an outage a
   * caller would notice, and it is exactly when a stale budget goes on being sized against.
   */
  Result<BuyingPower> buying_power(const Currency& currency) {
    buying_power_asked_.push_back(currency);
    for (const auto& failing : failing_currencies_) {
      if (failing == currency) {
        BrokerError error;
        error.category_ = ErrorCategory::TRANSPORT;
        return Result<BuyingPower>(error);
      }
    }
    for (const auto& throttled : throttled_currencies_) {
      if (throttled == currency) {
        BrokerError error;
        error.category_ = ErrorCategory::RATE_LIMIT;
        return Result<BuyingPower>(error);
      }
    }
    if (error_) {
      return Result<BuyingPower>(*error_);
    }
    BuyingPower reading;
    reading.cash_ = Money{buying_power_amount_, currency};
    reading.received_at_ = std::chrono::system_clock::now();
    return Result<BuyingPower>(std::move(reading));
  }

  /** Makes the buying-power inquiry fail for `currency` alone. */
  void fail_buying_power_in(const Currency& currency) {
    failing_currencies_.push_back(currency);
  }

  /** Makes the buying-power inquiry answer `currency` with a rate-limit refusal alone. */
  void throttle_buying_power_in(const Currency& currency) {
    throttled_currencies_.push_back(currency);
  }

  /** Sets what a successful buying-power inquiry answers, in whatever currency was asked. */
  void set_buying_power(const char* amount) { buying_power_amount_ = Decimal64::parse(amount); }

  /** Returns every currency the buying-power inquiry was asked about. */
  const std::vector<Currency>& buying_power_asked() const noexcept { return buying_power_asked_; }

  /** Returns the scripted working orders, counting the call. */
  Result<WorkingOrders<FakeBroker>> working_orders() {
    ++working_calls_;
    if (error_) {
      return Result<WorkingOrders<FakeBroker>>(*error_);
    }
    return Result<WorkingOrders<FakeBroker>>(working_orders_);
  }

  /**
   * Returns the scripted history page, counting the call.
   *
   * Carries its own failure switch, separate from the session-wide one, so a test can make the
   * *history* fail while working orders still succeed. That combination is the one the sweep has to
   * survive: it has a listing, it just does not have the one that covers closed orders.
   */
  Result<OrderPage> order_history(const OrderHistoryQuery&) {
    ++history_calls_;
    if (history_error_) {
      return Result<OrderPage>(*history_error_);
    }
    if (error_) {
      return Result<OrderPage>(*error_);
    }
    return Result<OrderPage>(history_);
  }

  void fail_history_with(ErrorCategory category, int http_status = 0) {
    BrokerError error;
    error.category_ = category;
    error.http_status_ = http_status;
    history_error_ = error;
  }

  void set_bars(BarSeries series) { bars_ = std::move(series); }
  void set_quote(QuoteSnapshot snapshot) { quote_ = std::move(snapshot); }
  void set_holdings(HoldingsSnapshot snapshot) { holdings_ = std::move(snapshot); }
  void set_working_orders(WorkingOrders<FakeBroker> orders) {
    working_orders_ = std::move(orders);
  }
  void set_history(OrderPage page) { history_ = std::move(page); }

  /** Watches `journal`, capturing what it held at the moment a placement was attempted. */
  void observe_journal(const runner::MemoryJournal& journal) { observed_ = &journal; }

  /** Returns the journal's contents as of the last placement this session was asked for. */
  const std::vector<runner::JournalEntry>& journal_at_send() const noexcept {
    return journal_at_send_;
  }

  std::size_t bar_calls() const noexcept { return bar_calls_; }
  std::size_t quote_calls() const noexcept { return quote_calls_; }
  std::size_t holdings_calls() const noexcept { return holdings_calls_; }
  std::size_t working_calls() const noexcept { return working_calls_; }
  std::size_t history_calls() const noexcept { return history_calls_; }

private:
  BarSeries bars_;
  QuoteSnapshot quote_;
  HoldingsSnapshot holdings_;
  WorkingOrders<FakeBroker> working_orders_;
  OrderPage history_;
  std::optional<BrokerError> history_error_;
  std::vector<Currency> failing_currencies_;
  std::vector<Currency> throttled_currencies_;
  std::vector<Currency> buying_power_asked_;
  Decimal64 buying_power_amount_{Decimal64::parse("5000000")};
  const runner::MemoryJournal* observed_{nullptr};
  std::vector<runner::JournalEntry> journal_at_send_;
  std::size_t bar_calls_{0};
  std::size_t quote_calls_{0};
  std::size_t holdings_calls_{0};
  std::size_t working_calls_{0};
  std::size_t history_calls_{0};
  int calendar_calls_{0};
  TradingCalendar calendar_;
  std::optional<Country> calendar_asked_;
  std::optional<Date> calendar_from_;
  std::optional<BrokerError> calendar_error_;

  std::vector<OrderRequest> placed_;
  std::vector<AmendInstruction> canceled_;
  std::vector<Ref> canceled_refs_;
  std::optional<BrokerError> error_;
  std::optional<BrokerError> local_refusal_;
};

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** A second instrument, so a per-instrument rule can be shown not to reach the other row. */
InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

/** A planned order as reconciliation would have produced it: on the grid, on the lot. */
strategy::PlannedOrder planned() {
  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.time_in_force_ = TimeInForce::DAY;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");
  order.tag_ = "test";
  return order;
}

WorkingOrder<FakeBroker> working() {
  WorkingOrder<FakeBroker> order;
  order.order_.instrument_ = samsung();
  order.order_.side_ = OrderSide::BUY;
  order.order_.status_ = OrderStatus::PENDING;
  order.order_.type_ = OrderType::LIMIT;
  // As a real adapter reports them; leaving these `UNKNOWN` models a row no
  // broker sends, and models it as the unreadable case recovery will not
  // attribute.
  order.order_.time_in_force_ = TimeInForce::DAY;
  order.order_.session_ = OrderSession::REGULAR;
  order.order_.price_ = Decimal64::parse("70000");
  order.order_.quantity_ = Decimal64::parse("10");
  order.amendable_quantity_ = Decimal64::parse("10");
  order.ref_ = OrderRef<FakeBroker>{"0001569139", ""};
  return order;
}

} // namespace

TEST_CASE("a reservation row can be withdrawn at the venue the order went to") {
  // The runner reserves a row for an order it has just sent, and that row is
  // what `cancel_all` reads until the broker's own listing arrives. A broker
  // listing fills `venue_` from what the broker routed the order to; this row
  // has no listing behind it, so it takes the venue the order was sent with.
  // Left `UNSPECIFIED`, a withdrawal would be addressed to the broker's default
  // venue rather than the one the order is actually resting at.
  strategy::PlannedOrder order;
  order.instrument_ = InstrumentId{Country::KR, Venue::NXT, "005930"};
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");

  WorkingOrder<FakeBroker> row;
  row.order_.instrument_ = order.instrument_;
  row.order_.venue_ = order.instrument_.venue_;
  row.amendable_quantity_ = order.quantity_;
  REQUIRE(cancel_all(row).venue_ == Venue::NXT);
}

TEST_CASE("a non-default order shape reaches the session rather than its default") {
  // The case above asserts the defaults, and a copy that was never written
  // agrees with a default. This one states three facts none of which is the
  // default, so a field the executor drops shows up as the default rather than
  // as the value asked for -- which is how a dropped field fails: silently, and
  // with a plausible value.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  auto order = planned();
  order.type_ = OrderType::BEST_LIMIT;
  order.time_in_force_ = TimeInForce::FILL_OR_KILL;
  order.session_ = OrderSession::AFTER_MARKET;
  // The routing venue too. This was missing, and a surviving mutation said so:
  // deleting the executor's copy of `venue_` failed nothing, because every test
  // left it at its default and a copy that never runs agrees with a default.
  // A venue that does not reach the wire is an order routed somewhere else.
  order.venue_ = Venue::NXT;
  REQUIRE(executor.place(order) == runner::SendOutcome::ACCEPTED);

  REQUIRE(session.placed().size() == 1);
  const auto& sent = session.placed()[0];
  REQUIRE(sent.type_ == OrderType::BEST_LIMIT);
  REQUIRE(sent.time_in_force_ == TimeInForce::FILL_OR_KILL);
  REQUIRE(sent.session_ == OrderSession::AFTER_MARKET);
  REQUIRE(sent.venue_ == Venue::NXT);
}

TEST_CASE("an accepted order reaches the session with its fields intact") {
  // Reconciliation already put the price on the tick grid and the quantity on the lot, so this
  // boundary adds nothing numeric; it moves fields, and a field lost here reaches an exchange.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.placed().size() == 1);
  const auto& sent = session.placed()[0];
  REQUIRE(sent.instrument_.symbol_ == "005930");
  REQUIRE(sent.side_ == OrderSide::BUY);
  REQUIRE(sent.type_ == OrderType::LIMIT);
  REQUIRE(sent.time_in_force_ == TimeInForce::DAY);
  REQUIRE(sent.session_ == OrderSession::REGULAR);
  REQUIRE(sent.quantity_ == Decimal64::parse("10"));
  REQUIRE(sent.price_ == std::optional<Decimal64>(Decimal64::parse("70000")));

  // And the reference the broker issued is kept, which is what a later amendment must address.
  REQUIRE(executor.last_ref());
  REQUIRE(executor.last_ref()->primary_ == "0001569139");
  // The compile-time broker tag travels with it, which is what stops one broker's reference being
  // handed to another's session.
  REQUIRE(executor.last_ref()->str() == "fake:0001569139");
  REQUIRE_FALSE(executor.last_error());
}

TEST_CASE("a transport failure on the live path leaves the outcome unknown") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::TRANSPORT);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::INDETERMINATE);
  // It still reached the session, which is exactly why the outcome is unknown rather than refused.
  REQUIRE(session.placed().size() == 1);
  REQUIRE(executor.last_error());
  REQUIRE(executor.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("a broker refusal on the live path is an answer, not a doubt") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::HTTP, 400);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::REJECTED);
  REQUIRE_FALSE(executor.last_ref());
}

TEST_CASE("an unparseable reply is treated as possibly successful") {
  // The trap the classification exists for: transport plainly succeeded, so it reads like a
  // client-side problem, and the order may be live.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::PROTOCOL);

  REQUIRE(executor.place(planned()) == runner::SendOutcome::INDETERMINATE);
}

TEST_CASE("an idempotency key a broker cannot honour is refused, never dropped") {
  // Dropping it would make a retry look safe on a broker offering no such guarantee, which is the
  // specific mistake OrderRequest::client_order_id_ documents. The strategy asked for a guarantee
  // the binding cannot provide.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  auto order = planned();
  order.client_order_id_ = "idem-1";
  // `REFUSED_LOCALLY`, not `REJECTED`: no broker refused this, because no broker was asked. The
  // distinction is what stops the runner charging a rate-limit request to a send nobody made.
  REQUIRE(executor.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
  // Refused before the session was touched, so nothing went out without the guarantee.
  REQUIRE(session.placed().empty());
  REQUIRE(executor.last_error());
  REQUIRE(executor.last_error()->category_ == ErrorCategory::INVALID_ARGUMENT);
}

TEST_CASE("an idempotency key a broker does honour is carried through") {
  FakeSession<WithKey> session;
  live::SessionExecutor<FakeSession<WithKey>> executor(session);

  auto order = planned();
  order.client_order_id_ = "idem-1";
  REQUIRE(executor.place(order) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.placed().size() == 1);
  REQUIRE(session.placed()[0].client_order_id_ == std::optional<std::string>("idem-1"));
}

TEST_CASE("a cancellation carries the order's own shape and amendable quantity") {
  // Restating either here would be inventing a value the broker will act on. cancel_all reads them
  // off the order, which is what a broker requiring them receives.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  REQUIRE(executor.cancel(working()) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.canceled().size() == 1);
  const auto& instruction = session.canceled()[0];
  REQUIRE(instruction.all_remaining_);
  REQUIRE(instruction.shape_.type_ == OrderType::LIMIT);
  REQUIRE(instruction.shape_.price_ == std::optional<Decimal64>(Decimal64::parse("70000")));
  REQUIRE(instruction.quantity_ == std::optional<Decimal64>(Decimal64::parse("10")));
  REQUIRE(session.canceled_refs()[0].primary_ == "0001569139");
}

TEST_CASE("a cancellation that fails in transit reports the doubt it leaves") {
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);
  session.fail_with(ErrorCategory::CANCELLED);

  // A local abandonment: the withdrawal may already have been sent.
  REQUIRE(executor.cancel(working()) == runner::SendOutcome::INDETERMINATE);
}

// ---------------------------------------------------------------------------
// The whole pipeline over a session, with nothing on the wire
// ---------------------------------------------------------------------------

namespace {

/** Holds ten shares, stated as a target. */
struct HoldTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
  }
};

using LiveRunner = runner::Runner<
  HoldTen, WithoutKey, FakeBroker, live::SessionExecutor<FakeSession<WithoutKey>>
>;

Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

strategy::BarEvent bar_at(int hour, const char* close) {
  Bar bar;
  bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + hour);
  bar.high_ = Decimal64::parse(close);
  bar.low_ = Decimal64::parse(close);
  bar.close_ = Decimal64::parse(close);
  bar.volume_ = Decimal64::parse("1000000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);
}

/** Sells everything it holds, so a sellable check has something to refuse. */
struct SellsAll {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("0");
    ctx.submit(target);
  }
};

template <typename CapsType>
using SellRunner = runner::Runner<
  SellsAll, CapsType, FakeBroker, live::SessionExecutor<FakeSession<CapsType>>
>;

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

template <typename CapsType>
using FlipRunner = runner::Runner<
  FlipTarget, CapsType, FakeBroker, live::SessionExecutor<FakeSession<CapsType>>
>;

/** A portfolio holding `held`, with `sellable` stated only when the broker publishes it. */
template <typename CapsType>
strategy::Portfolio<FakeBroker> held_position(const char* held, const char* sellable) {
  strategy::Portfolio<FakeBroker> portfolio;
  // Stamped, because an unstamped snapshot now means "never read" rather than "empty and current",
  // and a plan derived from one is withheld. This helper stands for a listing the broker answered.
  portfolio.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  // Declared for the same reason it is stamped: an unestablished basis now means "nobody said what
  // this figure covers" and a checked sell refuses, so a helper standing for a broker's answered
  // listing has to state what that broker states. This is the conservative pair every shipped
  // adapter declares -- whole holding, and this library deducts the resting sells itself.
  portfolio.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  if (sellable != nullptr) {
    position.sellable_quantity_ = Decimal64::parse(sellable);
  }
  portfolio.positions_.push_back(position);
  return portfolio;
}

} // namespace

TEST_CASE("a withdrawal the binding refused says so on its own entry") {
  // The cancellation half of the same rule. Both refusals land in `rejected_cancel_` because the
  // order's fate is the same -- it was not withdrawn -- and the entry says which, because a later
  // instruction in the same pass clears the executor's last error.
  FakeSession<SellableInHoldings> session;
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  FlipRunner<SellableInHoldings> live_runner(
    FlipTarget{}, limits, 512, live::SessionExecutor<FakeSession<SellableInHoldings>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

  strategy::Portfolio<FakeBroker> portfolio = held_position<SellableInHoldings>("10", "10");
  // A row the broker listed with no reference of its own: nothing can address it, so the binding
  // refuses rather than asking a broker to withdraw an order identified by an empty string.
  WorkingOrder<FakeBroker> unaddressable;
  unaddressable.order_.instrument_ = samsung();
  unaddressable.order_.side_ = OrderSide::SELL;
  unaddressable.order_.status_ = OrderStatus::PENDING;
  unaddressable.order_.quantity_ = Decimal64::parse("10");
  unaddressable.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  unaddressable.amendable_quantity_ = Decimal64::parse("10");
  portfolio.working_.push_back(unaddressable);
  live_runner.context().set_portfolio(portfolio);

  // Bar one targets flat, which the resting sell already achieves; bar two targets long, so the
  // sell opposes it and must be withdrawn.
  live_runner.feed(bar_at(1, "70000"));
  const auto second = live_runner.feed(bar_at(2, "70000"));

  REQUIRE(second.rejected_cancel_.size() == 1);
  CHECK_FALSE(second.rejected_cancel_[0].by_broker_);
  CHECK(second.would_cancel_.empty());
  CHECK(session.canceled().empty());
}

TEST_CASE("a reserved order is withdrawn by the reference the broker actually issued") {
  // The reservation is a working row this library wrote, and reconciliation reads working rows: a
  // row opposing the next target goes on the cancel list **by its reference**. A synthetic row with
  // an empty one would have the runner ask a broker to withdraw an order identified by nothing --
  // the one thing a cancellation cannot be vague about.
  FakeSession<SellableInHoldings> session;
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  FlipRunner<SellableInHoldings> live_runner(
    FlipTarget{}, limits, 512, live::SessionExecutor<FakeSession<SellableInHoldings>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<SellableInHoldings>("10", "10"));

  // Flat from ten held: one sell, accepted, and reserved for the next pass.
  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE(session.placed().size() == 1);

  // Long from the same ten held, with nothing polled in between: the resting sell opposes the buy
  // and has to be withdrawn first.
  const auto second = live_runner.feed(bar_at(2, "70000"));
  REQUIRE(session.canceled_refs().size() == 1);
  REQUIRE(session.canceled_refs()[0].primary_ == "0001569139");
  // And the instruction carries the order's own quantity, not the zero a default row would give a
  // broker that requires one.
  REQUIRE(session.canceled().size() == 1);
  REQUIRE(session.canceled()[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(second.unavailable_cancel_.empty());
}

/** Buys one share every bar, plainly. */
struct BuysOne {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      )
    );
  }
};

/** Asks for an idempotency key on the first bar only, then buys plainly. */
struct KeyedOnce {
  int bars_{0};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    auto intent = strategy::OrderIntent::market(
      bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
    );
    if (bars_++ == 0) {
      ctx.submit(intent.with_client_order_id("idem-1"));
      return;
    }
    ctx.submit(intent);
  }
};

TEST_CASE("a send the binding itself refused spends no rate-limit budget") {
  // The point of the fourth outcome. `REJECTED` means the *broker* refused, which obliges the
  // runner to count a request -- the broker counted one. Reporting a purely local refusal that way
  // spent budget for a request nobody made, and with a tight window that refuses a later order
  // which would have been allowed.
  using Runner = runner::Runner<
    KeyedOnce, WithoutKey, FakeBroker, live::SessionExecutor<FakeSession<WithoutKey>>
  >;
  FakeSession<WithoutKey> session;
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  // Wide enough that both bars fall inside it, or the window would have expired between them and
  // the test would pass whatever the first send spent.
  limits.rate_window_ = std::chrono::hours(24);

  Runner live_runner(
    KeyedOnce{}, limits, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read before it places; this case is about
  // something else, so it supplies one.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  // Bar one asks for a guarantee this broker cannot give, so nothing is sent.
  const auto first = live_runner.feed(bar_at(1, "70000"));
  CHECK(session.placed().empty());
  CHECK(first.would_place_.empty());
  REQUIRE(first.rejected_.size() == 1);
  // **The reason is on the entry**, not in `last_error()`: a pass places several orders and a later
  // accepted one clears that, so a report would outlive the only record of why this entry is here.
  // It also must not read as `NOT_REPRESENTABLE`, which claims the arithmetic overflowed.
  CHECK(first.rejected_[0].reason_ == strategy::RejectReason::BINDING_REFUSED);

  // Bar two asks for nothing unusual, and the window must still have room for it.
  const auto second = live_runner.feed(bar_at(2, "70000"));
  CHECK(second.refused_.empty());
  CHECK(second.would_place_.size() == 1);
  CHECK(session.placed().size() == 1);
}

TEST_CASE("a client refusing before it sends is not the broker refusing") {
  // The path a capability check cannot catch: the binding is willing, the broker supports the
  // feature, and the *client* refuses the instruction's shape before any I/O -- a Toss order id
  // that is empty or too long, a time in force it does not express. That is `INVALID_ARGUMENT`,
  // exactly like a broker's own refusal, and the two differ in whether a request went out at all.
  using Runner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  session.refuse_next_locally();
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  limits.rate_window_ = std::chrono::hours(24);

  Runner live_runner(
    BuysOne{}, limits, 512, live::SessionExecutor<FakeSession<WithKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read before it places; this case is about
  // something else, so it supplies one.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  // Not `BROKER_REFUSED`: the broker never saw it.
  CHECK(first.rejected_[0].reason_ == strategy::RejectReason::BINDING_REFUSED);
  CHECK(session.placed().empty());

  // And the window still has room, because nothing was counted against it.
  const auto second = live_runner.feed(bar_at(2, "70000"));
  CHECK(second.refused_.empty());
  CHECK(second.would_place_.size() == 1);
  CHECK(session.placed().size() == 1);
}

TEST_CASE("a token refresh that failed is not the broker refusing an order it never saw") {
  // The prerequisite-request boundary. Both clients validate, then refresh a token, then build the
  // order; a refresh that fails is handed to every caller waiting behind it. By category alone that
  // is `AUTHENTICATION`, which reads as the broker refusing -- so the runner would count a request
  // the broker never received, and a later valid order would be refused once credentials recover.
  using Runner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  session.fail_next_token();
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  limits.rate_window_ = std::chrono::hours(24);

  Runner live_runner(
    BuysOne{}, limits, 512, live::SessionExecutor<FakeSession<WithKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read before it places; this case is about
  // something else, so it supplies one.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  CHECK(first.rejected_[0].reason_ == strategy::RejectReason::BINDING_REFUSED);
  CHECK(session.placed().empty());
  // Nor is it indeterminate: an order that was never built leaves nothing to reconcile.
  CHECK(first.indeterminate_.empty());

  // Credentials recover, and the window has room because nothing was ever sent.
  const auto second = live_runner.feed(bar_at(2, "70000"));
  CHECK(second.refused_.empty());
  CHECK(second.would_place_.size() == 1);
  CHECK(session.placed().size() == 1);
}

TEST_CASE("a token request that failed in transit leaves no doubt about an unsent order") {
  // `TRANSPORT` maps to `INDETERMINATE` -- correctly, for the order's own request, where the bytes
  // may have arrived. On the *token* request it is the wrong answer twice over: the order was never
  // built, so nothing is in doubt, and withholding the instrument costs a reconciliation for an
  // order that does not exist.
  FakeSession<WithKey> session;
  session.fail_next_token(ErrorCategory::TRANSPORT);
  live::SessionExecutor<FakeSession<WithKey>> executor(session);

  CHECK(executor.place(planned()) == runner::SendOutcome::REFUSED_LOCALLY);
  CHECK(session.placed().empty());
}

TEST_CASE("a withdrawal is never addressed to a broker by an empty reference") {
  // The other half of the reference the reservation now carries. A row can still reach the executor
  // without one -- an executor that cannot report what the broker issued leaves it blank -- and
  // sending that is a request which can only fail while looking like an attempt. Refused as an
  // answer rather than as an outage: the order was never addressed, so nothing is in doubt.
  FakeSession<SellableInHoldings> session;
  live::SessionExecutor<FakeSession<SellableInHoldings>> executor(session);

  WorkingOrder<FakeBroker> unaddressable = working();
  unaddressable.ref_ = OrderRef<FakeBroker>{};

  CHECK(executor.cancel(unaddressable) == runner::SendOutcome::REFUSED_LOCALLY);
  CHECK(session.canceled().empty());
  REQUIRE(executor.last_error().has_value());
  CHECK(executor.last_error()->category_ == ErrorCategory::INVALID_ARGUMENT);

  // And an ordinary one still goes.
  CHECK(executor.cancel(working()) == runner::SendOutcome::ACCEPTED);
  CHECK(session.canceled().size() == 1);
}

TEST_CASE("a broker carrying the figure in its holdings is never asked for it") {
  // The whole point of `sellable_source`: at KIS the answer is already on every position the
  // runner polls, so the check costs nothing. A test counts the requests, because "it should not
  // ask" is the kind of claim that quietly stops being true.
  FakeSession<SellableInHoldings> session;
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  SellRunner<SellableInHoldings> live_runner(
    SellsAll{}, limits, 512, live::SessionExecutor<FakeSession<SellableInHoldings>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<SellableInHoldings>("10", "10"));

  const auto report = live_runner.feed(bar_at(1, "70000"));

  REQUIRE(session.sellable_asked().empty());
  REQUIRE(session.placed().size() == 1);
  REQUIRE(report.refused_.empty());

  // And when such a broker's position somehow carries no figure -- a field that stopped arriving,
  // a row that failed to yield one -- the capability is what stops a pointless request, not the
  // presence of the value. Without that gate this asks a broker whose answer the runner has
  // already decided comes from the snapshot.
  FakeSession<SellableInHoldings> silent;
  SellRunner<SellableInHoldings> strict(
    SellsAll{}, limits, 512, live::SessionExecutor<FakeSession<SellableInHoldings>>(silent)
  );
  strict.context().set_instrument(samsung_reference());
  strict.context().set_phase(strategy::MarketPhase::OPEN);
  strict.context().set_last_price(samsung(), Decimal64::parse("70000"));
  strict.context().set_portfolio(held_position<SellableInHoldings>("10", nullptr));

  const auto refused = strict.feed(bar_at(1, "70000"));
  REQUIRE(silent.sellable_asked().empty());
  REQUIRE(silent.placed().empty());
  REQUIRE(refused.refused_.size() == 1);
  REQUIRE(refused.refused_[0].reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
}

TEST_CASE("a broker that must be asked is asked once, for the instrument being sold") {
  FakeSession<SellableByInquiry> session;
  session.set_sellable("10");
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  SellRunner<SellableByInquiry> live_runner(
    SellsAll{}, limits, 512, live::SessionExecutor<FakeSession<SellableByInquiry>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // Toss leaves the field absent, which is why this broker has to be asked at all.
  live_runner.context().set_portfolio(held_position<SellableByInquiry>("10", nullptr));

  const auto report = live_runner.feed(bar_at(1, "70000"));

  REQUIRE(session.sellable_asked().size() == 1);
  REQUIRE(session.sellable_asked()[0] == samsung());
  REQUIRE(session.placed().size() == 1);
  REQUIRE(report.refused_.empty());

  // The answer is kept with the position, so the next pass in the same holdings cycle reuses it
  // rather than spending another request on a figure that has not been repolled.
  REQUIRE(live_runner.context().portfolio().sellable_quantity_of(samsung()) ==
          Decimal64::parse("10"));
  live_runner.feed(bar_at(2, "70000"));
  REQUIRE(session.sellable_asked().size() == 1);
}

TEST_CASE("an inquiry that fails leaves the quantity unknown, and unknown refuses") {
  FakeSession<SellableByInquiry> session;
  session.fail_with(ErrorCategory::TRANSPORT);
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  SellRunner<SellableByInquiry> live_runner(
    SellsAll{}, limits, 512, live::SessionExecutor<FakeSession<SellableByInquiry>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<SellableByInquiry>("10", nullptr));

  const auto report = live_runner.feed(bar_at(1, "70000"));

  // Nothing was sent on a figure nobody could establish, and the refusal names the reason rather
  // than reporting a sellable quantity of zero the broker never stated.
  REQUIRE(session.placed().empty());
  REQUIRE(report.refused_.size() == 1);
  REQUIRE(report.refused_[0].reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
}

TEST_CASE("a strategy drives a session through the same pipeline a dry run uses") {
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read before it places; this case is about
  // something else, so it supplies one.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  const auto report = live_runner.feed(bar_at(0, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  // It actually reached the session, on the lot. The target named no limit, so reconciliation
  // produced a market order -- and a market order carries no price, which is the invariant
  // OrderRequest states and every adapter enforces before any I/O.
  REQUIRE(session.placed().size() == 1);
  REQUIRE(session.placed()[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(session.placed()[0].type_ == OrderType::MARKET);
  REQUIRE_FALSE(session.placed()[0].price_);
}

TEST_CASE("a mid-flight failure on the live path stops the next order for that instrument") {
  // The acceptance criterion, end to end over a session: nothing further is submitted, and the
  // session is the only thing that could have sent it.
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read before it places; this case is about
  // something else, so it supplies one.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  session.fail_with(ErrorCategory::TRANSPORT);
  const auto first = live_runner.feed(bar_at(0, "70000"));
  REQUIRE(first.indeterminate_.size() == 1);
  REQUIRE(live_runner.ledger().requires_reconcile(samsung()));
  REQUIRE(session.placed().size() == 1);

  // The transport recovers; the doubt about the first order does not.
  session.succeed();
  const auto second = live_runner.feed(bar_at(24, "70000"));
  REQUIRE(second.withheld_.size() == 1);
  REQUIRE(second.would_place_.empty());
  // Still exactly one request ever made, which is the promise: no retry, no double position.
  REQUIRE(session.placed().size() == 1);

  // Only reading the broker's order list releases it.
  live_runner.ledger().resolve_after_reading_orders(samsung());
  const auto third = live_runner.feed(bar_at(48, "70000"));
  REQUIRE(third.would_place_.size() == 1);
  REQUIRE(session.placed().size() == 2);
}

// ---------------------------------------------------------------------------
// Binding in one call
// ---------------------------------------------------------------------------

TEST_CASE("binding reads the capabilities and the tag off the session") {
  // The repetition the factories remove, and the mismatch they make unexpressible: writing the four
  // parameters out lets a caller pair one broker's tag with another's capabilities.
  FakeSession<WithKey> session;
  auto bound = live::bind(session, HoldTen{}, {});

  STATIC_REQUIRE(std::is_same_v<decltype(bound)::Tag, FakeBroker>);
  STATIC_REQUIRE(decltype(bound)::Context::caps::has_client_order_id);

  bound.context().set_instrument(samsung_reference());
  bound.context().set_phase(strategy::MarketPhase::OPEN);
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read; this case is about the factory's
  // wiring, so it supplies one rather than opting out of the requirement.
  bound.context().set_portfolio(held_position<WithKey>("0", nullptr));

  const auto report = bound.feed(bar_at(0, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  // It reached the session, which is what `bind` is for.
  REQUIRE(session.placed().size() == 1);
}

TEST_CASE("an observing binding has the same types and nothing to send through") {
  // Same Caps and BrokerTag as the live binding, so a capability-gated strategy compiles the same
  // branch in both and observing cannot diverge from trading.
  FakeSession<WithKey> session;
  auto observing = live::bind_observer(session, HoldTen{}, {});

  STATIC_REQUIRE(std::is_same_v<decltype(observing)::Tag, FakeBroker>);
  STATIC_REQUIRE(decltype(observing)::Context::caps::has_client_order_id);

  observing.context().set_instrument(samsung_reference());
  observing.context().set_phase(strategy::MarketPhase::OPEN);
  observing.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto report = observing.feed(bar_at(0, "70000"));
  // The decision is reported as a placement, where it belongs -- not as a refusal, which is what
  // halting the guard would have produced.
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(report.refused_.empty());
  // And the session was never touched: there is nothing to send through, rather than a check that
  // declined to send.
  REQUIRE(session.placed().empty());
}

TEST_CASE("an observing binding records nothing in the ledger, because nothing was sent") {
  FakeSession<WithKey> session;
  session.fail_with(ErrorCategory::TRANSPORT);
  auto observing = live::bind_observer(session, HoldTen{}, {});
  observing.context().set_instrument(samsung_reference());
  observing.context().set_phase(strategy::MarketPhase::OPEN);
  observing.context().set_last_price(samsung(), Decimal64::parse("70000"));

  // The session would have failed, and it is never asked, so no doubt is created.
  observing.feed(bar_at(0, "70000"));
  REQUIRE(observing.ledger().empty());
}

// ---------------------------------------------------------------------------
// SessionSource: the read half of a live loop.
//
// Duck-typed on the session exactly as SessionExecutor is, so these run with no adapter and nothing
// on the wire. What they assert is the part a real broker could not be asked to demonstrate on
// demand: a rate-limit refusal, a failure mid-poll, and the same closed bar arriving twice.
// ---------------------------------------------------------------------------

namespace {

/** A strategy that counts what it was fed, so a test can prove the source actually fed it. */
struct CountEvents {
  std::size_t bars_{0};
  std::size_t quotes_{0};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {
    ++bars_;
  }

  template <class Ctx>
  void on_quote(Ctx&, const strategy::QuoteEvent&) {
    ++quotes_;
  }
};

using WorkingSession = FakeSession<WithWorkingOrders>;
using BudgetSession = FakeSession<WithBuyingPower>;
using HistorySession = FakeSession<WithoutWorkingOrders>;
using CalendarSession = FakeSession<WithCalendar>;
using NoCalendarSession = FakeSession<WithoutCalendar>;

/** A calendar saying `date` trades, as a broker would answer. */
TradingCalendar one_day_calendar(const Date& date, bool trades = true) {
  TradingCalendar calendar;
  calendar.country_ = Country::KR;
  calendar.received_at_ = std::chrono::system_clock::now();
  calendar.days_.push_back(algotradelib::TradingDay{date, trades});
  return calendar;
}

/** A two-bar series ending at `last_close`. */
BarSeries two_bars(const char* first_close, const char* last_close) {
  BarSeries series;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::CLOSE;
  Bar first;
  first.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  first.close_ = Decimal64::parse(first_close);
  first.high_ = first.close_;
  first.low_ = first.close_;
  Bar second = first;
  second.timestamp_ = first.timestamp_ + std::chrono::hours(24);
  second.close_ = Decimal64::parse(last_close);
  second.high_ = second.close_;
  second.low_ = second.close_;
  series.bars_ = {first, second};
  return series;
}

} // namespace

TEST_CASE("a rate-limit refusal is not counted as a poll failure") {
  // The limiter working as designed. Counting it as an outage would make a loop polling slightly too
  // fast shut itself down instead of simply slowing down.
  BrokerError limited;
  limited.category_ = ErrorCategory::RATE_LIMIT;
  CHECK(live::poll_outcome_of(limited) == runner::PollOutcome::RATE_LIMITED);

  for (const auto category : {
         ErrorCategory::TRANSPORT,
         ErrorCategory::PROTOCOL,
         ErrorCategory::HTTP,
         ErrorCategory::AUTHENTICATION,
         ErrorCategory::CANCELLED,
         ErrorCategory::INVALID_ARGUMENT,
       }) {
    BrokerError error;
    error.category_ = category;
    CHECK(live::poll_outcome_of(error) == runner::PollOutcome::FAILED);
  }
}

TEST_CASE("a holdings poll carries the basis with the rows it describes") {
  // **The boundary every hand-built `Portfolio` test skips, and a real defect lived here.** The
  // adapters set `HoldingsSnapshot::sellable_basis_` and `RiskGuard` reads
  // `Portfolio::sellable_basis_`; if the poll between them copies only the rows, a broker that
  // declared is treated as having declared nothing and every checked sell refuses. That shipped for
  // four commits and no fixture test could see it, because they all write the `Portfolio` directly.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  HoldingsSnapshot declared;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  position.sellable_quantity_ = Decimal64::parse("100");
  declared.positions_.push_back(position);
  declared.received_at_ = std::chrono::system_clock::now();
  declared.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  session.set_holdings(declared);

  REQUIRE(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  const auto& carried = live_runner.context().portfolio();
  CHECK(carried.sellable_basis_.coverage_ == SellableBasis::Coverage::WHOLE_HOLDING);
  CHECK(carried.sellable_basis_.resting_ == SellableBasis::RestingTreatment::DEDUCT);
  CHECK(carried.sellable_basis_.is_enforceable());

  // **And the other direction, which is the dangerous one.** A later snapshot that establishes
  // nothing must not inherit the enforceable basis this one left behind: a third-party source's
  // undeclared figure would otherwise be authorized by an earlier broker's declaration. The basis is
  // assigned with the rows, not merged into them.
  HoldingsSnapshot undeclared;
  HoldingPosition later = position;
  later.sellable_quantity_ = Decimal64::parse("100");
  undeclared.positions_.push_back(later);
  undeclared.received_at_ = std::chrono::system_clock::now();
  session.set_holdings(undeclared);

  REQUIRE(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  const auto& replaced = live_runner.context().portfolio();
  CHECK(replaced.sellable_basis_.coverage_ == SellableBasis::Coverage::UNESTABLISHED);
  CHECK_FALSE(replaced.sellable_basis_.is_enforceable());
  // The rows did arrive, so this is not a poll that failed -- it is a poll that established nothing.
  CHECK(replaced.positions_.size() == 1);
}

TEST_CASE("a failed holdings poll leaves the rows and their basis together") {
  // **The waiver's own consequence, asserted rather than only documented.** Asserting a Kiwoom scope
  // turns the holdings verb on; it does not promise a read has succeeded, and a failed poll keeps the
  // previous portfolio rather than erasing it -- so a run can go on sizing against an older snapshot,
  // which is what the portfolio age policy exists to bound. What must not happen is the two halves
  // coming apart: rows from one read described by another read's basis, in either direction.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  HoldingsSnapshot declared;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("100");
  position.sellable_quantity_ = Decimal64::parse("100");
  declared.positions_.push_back(position);
  declared.received_at_ = std::chrono::system_clock::now();
  declared.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  session.set_holdings(declared);
  REQUIRE(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(live_runner.context().portfolio().sellable_basis_.is_enforceable());

  // Now the read fails. The previous portfolio survives, which is deliberate and is slice 26's rule
  // rather than this branch's -- and it survives *whole*.
  session.fail_with(ErrorCategory::TRANSPORT, 503);
  const auto outcome = source.poll(runner::PollKind::HOLDINGS, live_runner);
  REQUIRE(outcome != runner::PollOutcome::UPDATED);

  const auto& kept = live_runner.context().portfolio();
  // The rows are still there, and so is the basis that describes them. Clearing one and not the other
  // is the defect this pins: an enforceable basis over no rows refuses harmlessly, but rows left
  // behind with no basis would refuse a sell the previous read had fully established.
  CHECK(kept.positions_.size() == 1);
  CHECK(kept.sellable_basis_.is_enforceable());
  CHECK(kept.sellable_quantity_of(samsung()) == Decimal64::parse("100"));
}

TEST_CASE("an inquiry answers for one instrument and certifies no other") {
  // **One declaration must not travel to a figure it says nothing about.** The inquiry is asked per
  // instrument, so writing its basis to the whole portfolio would let the asked instrument's
  // declaration make an undeclared figure on another row enforceable -- and a wrong-sized sell of
  // that other instrument would reach the wire.
  //
  // Driven through `Runner::write_sellable` rather than by setting the fields, because that is the
  // code under test: a version of this that built the portfolio by hand passed while the defect was
  // present, which is how the defect got here.
  FakeSession<SellableByInquiry> session;
  session.set_sellable("10");
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  SellRunner<SellableByInquiry> live_runner(
    SellsAll{}, limits, 512, live::SessionExecutor<FakeSession<SellableByInquiry>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

  // Samsung has no figure, so it is the one asked about. Hynix arrives with a figure already
  // populated and nothing said about it -- the third-party shape: a source whose author filled the
  // field they found in their own broker's response and never heard of a basis.
  auto portfolio = held_position<SellableByInquiry>("10", nullptr);
  portfolio.sellable_basis_ = SellableBasis{};
  HoldingPosition undeclared;
  undeclared.instrument_ = hynix();
  undeclared.quantity_ = Decimal64::parse("100");
  undeclared.sellable_quantity_ = Decimal64::parse("100");
  portfolio.positions_.push_back(undeclared);
  live_runner.context().set_portfolio(portfolio);

  const auto report = live_runner.feed(bar_at(1, "70000"));

  // The inquiry ran for Samsung and its order went out, so the declared path works.
  REQUIRE(session.sellable_asked().size() == 1);
  REQUIRE(session.sellable_asked()[0] == samsung());
  REQUIRE(report.refused_.empty());

  const auto& after = live_runner.context().portfolio();
  REQUIRE(after.sellable_reading_for(samsung()).basis_.is_enforceable());

  // **And Hynix is untouched.** This is the assertion the defect failed: its figure was populated
  // before the inquiry and nobody has said what it covers.
  REQUIRE_FALSE(after.sellable_reading_for(hynix()).basis_.is_enforceable());
  // The portfolio-wide basis stays unestablished too, which is what stops the promotion.
  REQUIRE_FALSE(after.sellable_basis_.is_enforceable());
}

TEST_CASE("polling bars feeds each new bar exactly once") {
  // The property that matters: a poll returning the same closed bar again is the normal case between
  // closes, and feeding it twice would double every signal derived from it.
  WorkingSession session;
  session.set_bars(two_bars("70000", "71000"));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  const auto first = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(first == runner::PollOutcome::UPDATED);
  CHECK(live_runner.strategy().bars_ == 2);

  // Same series again: nothing new.
  const auto second = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(second == runner::PollOutcome::UNCHANGED);
  CHECK(live_runner.strategy().bars_ == 2);

  // One more bar appears, and only it is fed.
  auto extended = two_bars("70000", "71000");
  Bar third = extended.bars_.back();
  third.timestamp_ += std::chrono::hours(24);
  third.close_ = Decimal64::parse("72000");
  extended.bars_.push_back(third);
  session.set_bars(extended);

  const auto after = source.poll(runner::PollKind::BARS, live_runner);
  CHECK(after == runner::PollOutcome::UPDATED);
  CHECK(live_runner.strategy().bars_ == 3);
}

TEST_CASE("a buying-power poll that answers for one currency and fails for another has failed") {
  // The partial answer is the dangerous one, because it looks like success. Reporting `UPDATED`
  // because *something* arrived resets the loop's consecutive-failure count, and a currency that
  // fails every time would then never abandon a run: the loop would report healthy polls forever
  // while orders in that currency were sized against a figure from hours ago.
  BudgetSession session;
  session.set_buying_power("5000000");
  session.fail_buying_power_in(Currency::usd());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BudgetSession> source(session, {samsung()});
  source.set_buying_power_currencies({Currency::krw(), Currency::usd()});

  const auto outcome = source.poll(runner::PollKind::BUYING_POWER, live_runner);
  CHECK(outcome == runner::PollOutcome::FAILED);
  CHECK(source.last_error().has_value());
  // Both were asked: one currency failing must not abandon the rest of the round.
  CHECK(session.buying_power_asked().size() == 2);
  // And the currency that answered was still applied, because dropping a budget the guard is about
  // to size against is the worse direction; the staleness gate is what retires an old reading.
  const auto& readings = live_runner.context().portfolio().buying_power_;
  REQUIRE(readings.size() == 1);
  CHECK(readings[0].cash_.currency_ == Currency::krw());
}

TEST_CASE("a throttled currency does not downgrade another currency's outright failure") {
  // The two are not the same news: the limiter refusing is the limiter working, and the loop
  // deliberately does not count a rate-limit refusal against a poll kind. Taking whichever answer
  // came last would let a currency failing every sweep be reported as merely throttled the moment
  // another currency was throttled behind it, and the failing budget would age on unnoticed.
  BudgetSession session;
  session.set_buying_power("5000000");
  session.fail_buying_power_in(Currency::krw());
  session.throttle_buying_power_in(Currency::usd());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BudgetSession> source(session, {samsung()});
  // KRW fails outright first, USD is throttled second, so the last answer is the throttle.
  source.set_buying_power_currencies({Currency::krw(), Currency::usd()});

  CHECK(source.poll(runner::PollKind::BUYING_POWER, live_runner) == runner::PollOutcome::FAILED);
  // And the reason names the currency that actually failed, not the one behind it: `last_error()`
  // promises why the last failing poll failed, and a reader chases that reason.
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("an outright failure wins however late in the sweep it lands") {
  // The same rule from the other side, so the answer does not depend on the order the currencies
  // happen to be configured in.
  BudgetSession session;
  session.set_buying_power("5000000");
  session.throttle_buying_power_in(Currency::krw());
  session.fail_buying_power_in(Currency::usd());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BudgetSession> source(session, {samsung()});
  source.set_buying_power_currencies({Currency::krw(), Currency::usd()});

  CHECK(source.poll(runner::PollKind::BUYING_POWER, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("a throttled currency alone is a throttled poll, not an outage") {
  BudgetSession session;
  session.set_buying_power("5000000");
  session.throttle_buying_power_in(Currency::usd());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BudgetSession> source(session, {samsung()});
  source.set_buying_power_currencies({Currency::krw(), Currency::usd()});

  CHECK(
    source.poll(runner::PollKind::BUYING_POWER, live_runner) == runner::PollOutcome::RATE_LIMITED
  );
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::RATE_LIMIT);
}

TEST_CASE("a buying-power poll that answers for every configured currency has succeeded") {
  BudgetSession session;
  session.set_buying_power("5000000");
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BudgetSession> source(session, {samsung()});
  source.set_buying_power_currencies({Currency::krw(), Currency::usd()});

  const auto outcome = source.poll(runner::PollKind::BUYING_POWER, live_runner);
  CHECK(outcome == runner::PollOutcome::UPDATED);
  CHECK_FALSE(source.last_error().has_value());
  CHECK(live_runner.context().portfolio().buying_power_.size() == 2);
  // Published so the loop's gate can hold for a budget that never arrived at all.
  CHECK(source.buying_power_currencies().size() == 2);
}

TEST_CASE("a failing poll reports the failure and its reason") {
  WorkingSession session;
  session.set_bars(two_bars("70000", "71000"));
  session.fail_with(ErrorCategory::TRANSPORT);
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::BARS, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
  // Nothing was fed, and nothing is owed: a poll that failed sent nothing, so unlike an order write
  // it leaves no question about what exists.
  CHECK(live_runner.strategy().bars_ == 0);
  CHECK(live_runner.ledger().empty());
}

TEST_CASE("polling holdings stamps the portfolio with the broker's own time") {
  // The loop's staleness gate measures against as_of_, so it must be when the broker says the
  // positions were true rather than when the reply happened to arrive.
  WorkingSession session;
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  snapshot.positions_.push_back(position);
  snapshot.received_at_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  session.set_holdings(snapshot);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  const auto& portfolio = live_runner.context().portfolio();
  REQUIRE(portfolio.positions_.size() == 1);
  CHECK(portfolio.positions_[0].quantity_ == Decimal64::parse("10"));
  CHECK(portfolio.as_of_ == snapshot.received_at_);
}

TEST_CASE("an account holding nothing still stamps the portfolio") {
  // **The fact any "has a portfolio arrived" question rests on.** The epoch on `as_of_` is what
  // tells "never arrived" from "arrived empty": `PollingLoop`'s staleness gate reads it that way
  // today, and `plan/todo/holdings-failure-is-not-a-send-gate.md` proposes reading it that way
  // unconditionally. If a successful read of an account with no positions left the stamp at the
  // epoch, every such rule would hold an empty account forever.
  WorkingSession session;
  HoldingsSnapshot empty;
  empty.received_at_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  session.set_holdings(empty);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::HOLDINGS, live_runner) == runner::PollOutcome::UPDATED);
  const auto& portfolio = live_runner.context().portfolio();
  CHECK(portfolio.positions_.empty());
  // Stamped, and therefore distinguishable from a portfolio that never arrived.
  CHECK(portfolio.as_of_ == empty.received_at_);
  CHECK(portfolio.as_of_ != std::chrono::system_clock::time_point{});
}

TEST_CASE("holdings and working orders merge rather than replace each other") {
  // They are separate inquiries on separate cadences. Rebuilding the portfolio from either alone
  // would drop the other, and a dropped working order is one reconciliation places again.
  WorkingSession session;
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  snapshot.positions_.push_back(position);
  session.set_holdings(snapshot);
  WorkingOrders<FakeBroker> orders;
  orders.orders_.push_back(working());
  session.set_working_orders(orders);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  source.poll(runner::PollKind::HOLDINGS, live_runner);
  source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
  CHECK(live_runner.context().portfolio().positions_.size() == 1);
  CHECK(live_runner.context().portfolio().working_.size() == 1);

  // And in the other order, because neither cadence is guaranteed to fire first.
  auto other = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> other_source(session, {samsung()});
  other_source.poll(runner::PollKind::WORKING_ORDERS, other);
  other_source.poll(runner::PollKind::HOLDINGS, other);
  CHECK(other.context().portfolio().positions_.size() == 1);
  CHECK(other.context().portfolio().working_.size() == 1);
}

TEST_CASE("the order-list read follows the broker's capability") {
  // A compile-time choice, so both branches need a broker to instantiate against or only one is ever
  // compiled. The gate needs *an* order list, not a particular one.
  SECTION("a broker with working orders uses that inquiry") {
    WorkingSession session;
    auto live_runner = live::bind_observer(session, CountEvents{});
    live::SessionSource<WorkingSession> source(session, {samsung()});

    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK(session.working_calls() == 1);
    CHECK(session.history_calls() == 0);
  }

  SECTION("a broker without it falls back to order history") {
    HistorySession session;
    auto live_runner = live::bind_observer(session, CountEvents{});
    live::SessionSource<HistorySession> source(session, {samsung()});

    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK(session.history_calls() == 1);
    CHECK(session.working_calls() == 0);
  }
}

TEST_CASE("a failing order-list read leaves the gate closed") {
  // The gate exists because an empty ledger does not mean nothing is owed -- it means nothing is
  // remembered. A read that failed has established nothing, so it must not satisfy the gate.
  HistorySession session;
  session.fail_with(ErrorCategory::HTTP, 503);
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->http_status_ == 503);
}

TEST_CASE("a broker without the working-order inquiry never polls for one") {
  // A cadence is data, not a capability claim: a caller who sets one for an inquiry this broker lacks
  // gets a poll that does nothing rather than a build failure.
  HistorySession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UNCHANGED);
  CHECK(session.working_calls() == 0);
}

// ---------------------------------------------------------------------------
// Sweeping the ledger from a listing the source already fetched.
//
// The half of the order journal that needs a session. Everything about *what* a match means is
// decided in `runner::match_against` and tested without one; what these assert is that the rows a
// real inquiry returns reach it, that the ledger is cleared by what they settle, and that a source
// with no journal behaves exactly as it did before.
// ---------------------------------------------------------------------------

namespace {

/** The instant the journal entries below were recorded at. */
std::chrono::system_clock::time_point journaled_at() {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
}

/** An unanswered placement in a journal, which is what a killed process leaves behind. */
runner::JournalEntry journaled_placement(const char* id = "run-1") {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = runner::JournalOutcome::PENDING;
  entry.recorded_at_ = journaled_at();
  entry.instrument_ = samsung();
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::LIMIT;
  entry.quantity_ = Decimal64::parse("10");
  entry.price_ = Decimal64::parse("70000");
  return entry;
}

/** A listing row matching `journaled_placement`, as the broker would report it. */
OrderSnapshot listed_order(const char* order_id) {
  auto snapshot = working().order_;
  snapshot.order_id_ = order_id;
  snapshot.ordered_at_ = journaled_at();
  return snapshot;
}

/** Long enough after the send that an absent row means the order never existed. */
std::chrono::system_clock::time_point swept_at() {
  return journaled_at() + std::chrono::minutes(10);
}

} // namespace

TEST_CASE("a source with no journal reads the list and matches nothing") {
  // The pre-existing behaviour, unchanged: the gate lifts because this process has looked at the
  // broker's own record, and a mark nothing accounts for stays exactly where it is.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession> source(session, {samsung()});
  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
  CHECK(source.last_reconcile().matches_.empty());
}

TEST_CASE("the order-list read resolves what the broker's rows settle") {
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().order_id_ == "KRX-1");
  CHECK(source.last_reconcile().settled());
  // The instrument may trade again, and only reading the broker's own list got it there.
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("a working-order poll sweeps the ledger without a request of its own") {
  // The rows a working-order poll already fetches are exactly what the matcher needs, so
  // reconciliation rides along rather than asking the broker again.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UPDATED);
  CHECK(session.working_calls() == 1);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("the history fallback sweeps against the page it read") {
  // A broker with no working-order inquiry still reconciles, from the wider listing it does offer.
  HistorySession session;
  OrderPage page;
  page.orders_.push_back(listed_order("KRX-1"));
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<HistorySession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  // Twice: once to satisfy the gate, once by the sweep with the calendar range the matching window
  // actually needs. The second read happens only when something is owed, so a clean start still
  // costs one.
  CHECK(session.history_calls() == 2);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an ambiguous listing leaves the instrument withheld") {
  // Two rows that could both be the journaled order. Guessing would either leave a live order
  // unwatched or double a position, so the hold stays and the report says why.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  for (const auto* id : {"KRX-1", "KRX-2"}) {
    auto listed = working();
    listed.order_ = listed_order(id);
    orders.orders_.push_back(listed);
  }
  session.set_working_orders(orders);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(source.last_reconcile().ambiguous_);
  CHECK_FALSE(source.last_reconcile().settled());
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an empty listing settles nothing until the window has closed") {
  WorkingSession session;
  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  // Five seconds after the send, an absent row is not evidence of anything.
  live_runner.context().set_now(journaled_at() + std::chrono::seconds(5));
  source.read_order_list(live_runner);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::UNDECIDED);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));

  // Ten minutes after it, the order was never placed.
  live_runner.context().set_now(swept_at());
  source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::NOT_PLACED);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an open-orders listing alone cannot conclude a placement was not placed") {
  // The failure this guards against: a market or aggressive limit order fills and leaves the open
  // listing before the first sweep. Matching against open orders alone finds nothing, and after the
  // window closes that absence reads as `not_placed` -- clearing the ledger for an order that
  // existed and executed, so the runner places it again.
  WorkingSession session;
  session.set_working_orders(WorkingOrders<FakeBroker>{});

  auto filled = listed_order("KRX-1");
  filled.status_ = OrderStatus::FILLED;
  OrderPage page;
  page.orders_.push_back(filled);
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  // The history was consulted even though this broker has the working-order inquiry, because only
  // history covers every lifecycle state.
  CHECK(session.history_calls() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::MATCHED);
  CHECK(source.last_reconcile().matches_.front().order_id_ == "KRX-1");
}

TEST_CASE("a failed history read concludes nothing from absence") {
  // Seeing part of the picture is worse than seeing none: `not_placed` is a conclusion drawn from
  // absence, and absence from a listing that never arrived is not evidence.
  WorkingSession session;
  session.set_working_orders(WorkingOrders<FakeBroker>{});

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);
  session.fail_history_with(ErrorCategory::HTTP, 503);

  source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
  // Without the history the sweep decides nothing, rather than concluding the order never existed.
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::UNDECIDED);
  CHECK(source.last_reconcile().recorded_ == 0);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an order in both listings is one candidate, not two") {
  // A still-open order appears in the working-order inquiry *and* in history. Two copies of one row
  // would read as two candidates and the matcher would report `ambiguous` for an instruction that
  // is not ambiguous at all.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  OrderPage page;
  page.orders_.push_back(listed_order("KRX-1"));
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK_FALSE(source.last_reconcile().ambiguous_);
  CHECK(source.last_reconcile().matches_.front().candidates_ == 1);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("a cancellation needs no history, because working orders answer it") {
  // The asymmetry worth stating: working orders are sufficient for the cancellation question --
  // is it still resting? -- and insufficient for the placement question.
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  auto entry = journaled_placement();
  entry.action_ = runner::JournalAction::CANCEL;
  entry.order_id_ = "KRX-1";
  runner::MemoryJournal journal;
  journal.append(entry);

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "a cancellation request failed in transit");

  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(session.history_calls() == 0);
  CHECK(source.last_reconcile().settled());
}

// ---------------------------------------------------------------------------
// The journal has to be in the runner's own send path, not beside it
// ---------------------------------------------------------------------------

TEST_CASE("a journaled binding records before the session is asked") {
  // The assertion that would have caught the documented setup being wrong. Observing the pending
  // entry *after* `feed` returns would pass even for a runner that journals nothing and sends
  // through a plain executor, so the journal is read from inside the session's own `place`.
  runner::MemoryJournal journal("test:account");
  FakeSession<WithKey> session;
  session.observe_journal(journal);

  auto live_runner = live::bind_journaled(session, journal, HoldTen{}, {});
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  // A session-backed runner requires an account it has read; this case is about the journal.
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  const auto report = live_runner.feed(bar_at(0, "70000"));
  REQUIRE(report.would_place_.size() == 1);
  REQUIRE(session.placed().size() == 1);

  // What the journal held at the moment the session was asked: the pending line, already written.
  REQUIRE(session.journal_at_send().size() == 1);
  CHECK(session.journal_at_send().front().outcome_ == runner::JournalOutcome::PENDING);
  CHECK(session.journal_at_send().front().instrument_ == samsung());
  CHECK(session.journal_at_send().front().scope_ == "test:account");

  // And the terminal line afterwards, carrying the reference the session issued.
  REQUIRE(journal.records().size() == 2);
  CHECK(journal.records().back().outcome_ == runner::JournalOutcome::ACCEPTED);
  CHECK_FALSE(journal.records().back().order_id_.empty());

  // Nothing is owed, because the instruction was answered.
  CHECK(runner::unresolved_instructions(journal.replay()).empty());
}

TEST_CASE("a journaled binding keeps the session's capabilities and tag") {
  // Same `Caps` and `BrokerTag` as a plain binding, so a capability-gated strategy compiles the
  // same branch whether or not a journal is attached.
  runner::MemoryJournal journal("test:account");
  FakeSession<WithKey> session;
  auto live_runner = live::bind_journaled(session, journal, HoldTen{}, {});
  STATIC_REQUIRE(std::is_same_v<decltype(live_runner)::Tag, FakeBroker>);
  STATIC_REQUIRE(decltype(live_runner)::Context::caps::has_client_order_id);
}

// ---------------------------------------------------------------------------
// The history query has to satisfy the production adapter, not just the fake
// ---------------------------------------------------------------------------

namespace {

/** KIS's real answers about what an order-history inquiry accepts. */
struct KisLikeCaps : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
  static constexpr bool history_requires_range = true;
  static constexpr bool has_history_cursor = false;
  static constexpr bool has_history_limit = false;
};

/**
 * A session that validates every history query through KIS's own conversion.
 *
 * The gap the round-2 review found: a fake that accepts any query shape proves nothing about a
 * query the production adapter refuses. `KisTraits::to_history_query` is the function the real
 * client calls before any network I/O, so routing the fake through it makes the test assert the
 * contract rather than my idea of it.
 */
class KisContractSession {
public:
  using Caps = KisLikeCaps;
  using BrokerTag = FakeBroker;
  using Ref = OrderRef<FakeBroker>;
  using KisTraits = BrokerTraits<kis::KisClient>;

  Result<WorkingOrders<FakeBroker>> working_orders() {
    return Result<WorkingOrders<FakeBroker>>(WorkingOrders<FakeBroker>{});
  }

  Result<OrderPage> order_history(const OrderHistoryQuery& query) {
    ++history_calls_;
    last_query_ = query;
    auto converted = KisTraits::to_history_query(kis::KisAccount{}, query);
    if (!converted) {
      rejection_ = converted.error();
      return Result<OrderPage>(converted.error());
    }
    rejection_.reset();
    return Result<OrderPage>(history_);
  }

  Result<BarSeries> bars(const BarQuery&) { return Result<BarSeries>(BarSeries{}); }
  Result<QuoteSnapshot> quote(const InstrumentId&) {
    return Result<QuoteSnapshot>(QuoteSnapshot{});
  }
  Result<HoldingsSnapshot> holdings() { return Result<HoldingsSnapshot>(HoldingsSnapshot{}); }

  void set_history(OrderPage page) { history_ = std::move(page); }
  std::size_t history_calls() const noexcept { return history_calls_; }
  const std::optional<BrokerError>& rejection() const noexcept { return rejection_; }
  const std::optional<OrderHistoryQuery>& last_query() const noexcept { return last_query_; }

private:
  OrderPage history_;
  std::size_t history_calls_{0};
  std::optional<BrokerError> rejection_;
  std::optional<OrderHistoryQuery> last_query_;
};

} // namespace

TEST_CASE("a placement sweep builds a history query KIS actually accepts") {
  // Before the capability existed, `read_history` always set `limit_`, and KIS rejects that outright
  // with `limit_unsupported` rather than ignoring it. Every KIS placement sweep therefore took the
  // history-failure branch and the instrument stayed withheld forever -- safe, and completely
  // nonfunctional.
  KisContractSession session;
  OrderPage page;
  page.orders_.push_back(listed_order("KRX-1"));
  session.set_history(page);

  runner::MemoryJournal journal("kis:test");
  journal.append(journaled_placement());

  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");

  live::SessionSource<KisContractSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(session.history_calls() == 1);
  // The production conversion accepted it.
  CHECK_FALSE(session.rejection().has_value());
  // No limit, because KIS refuses one; a range, because KIS requires one.
  REQUIRE(session.last_query().has_value());
  CHECK_FALSE(session.last_query()->limit_.has_value());
  CHECK(session.last_query()->from_.has_value());
  CHECK(session.last_query()->to_.has_value());
  CHECK_FALSE(session.last_query()->cursor_.has_value());

  // And the sweep actually resolved, which is the point of all of it.
  CHECK(source.last_reconcile().settled());
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
}

TEST_CASE("the history-limit capability says what each broker does") {
  STATIC_REQUIRE_FALSE(BrokerCaps<kis::KisClient>::has_history_limit);
  STATIC_REQUIRE(BrokerCaps<tossinvest::TossClient>::has_history_limit);
  // The range requirement is the mirror image, and the two are independent answers.
  STATIC_REQUIRE(BrokerCaps<kis::KisClient>::history_requires_range);
  STATIC_REQUIRE_FALSE(BrokerCaps<tossinvest::TossClient>::history_requires_range);
}

TEST_CASE("polling the calendar puts it where the loop and the strategy both read it") {
  // The one poll whose answer is not an event. `runner::PollingLoop` derives the market phase from
  // it and a strategy answers "does tomorrow trade" from the same copy, so it lands on the context
  // rather than being fed -- and before this branch `Context::set_calendar` had no caller at all,
  // which made `Context::calendar()` permanently empty in every world.
  CalendarSession session;
  const Date today{2026, 3, 10};
  session.set_calendar(one_day_calendar(today));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<CalendarSession> source(session, {samsung()});

  const auto outcome = source.poll(runner::PollKind::CALENDAR, live_runner);

  CHECK(outcome == runner::PollOutcome::UPDATED);
  REQUIRE(live_runner.context().calendar().is_trading_day(today).has_value());
  CHECK(*live_runner.context().calendar().is_trading_day(today));
  // The domestic market by default, which is the only one KIS publishes and the one the KRX session
  // window states the hours for.
  CHECK(session.calendar_asked() == Country::KR);
  CHECK(source.calendar_country() == Country::KR);
  // And no date is named, so each broker supplies its own "today" -- today *there*, not here.
  CHECK_FALSE(session.calendar_from().has_value());
}

TEST_CASE("a calendar poll asks about the market it was configured for") {
  CalendarSession session;
  session.set_calendar(one_day_calendar(Date{2026, 3, 10}));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<CalendarSession> source(session, {samsung()});
  source.set_calendar_country(Country::US);

  source.poll(runner::PollKind::CALENDAR, live_runner);

  CHECK(session.calendar_asked() == Country::US);
}

TEST_CASE("a failed calendar poll leaves the last answer standing") {
  // The `poll_buying_power` rule, one field over. Erasing the calendar because a reference-data
  // request timed out would take a run that knew the market was open and, with the default
  // `permitted_phases_`, refuse every order in it -- an uncovered date is `UNKNOWN`.
  CalendarSession session;
  const Date today{2026, 3, 10};
  session.set_calendar(one_day_calendar(today));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<CalendarSession> source(session, {samsung()});

  REQUIRE(source.poll(runner::PollKind::CALENDAR, live_runner) == runner::PollOutcome::UPDATED);

  session.fail_calendar(ErrorCategory::TRANSPORT);
  CHECK(source.poll(runner::PollKind::CALENDAR, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
  // Still there, which is the point.
  REQUIRE(live_runner.context().calendar().is_trading_day(today).has_value());

  // A rate-limit refusal is the limiter working, not an outage, and it keeps the answer too.
  session.fail_calendar(ErrorCategory::RATE_LIMIT);
  CHECK(source.poll(runner::PollKind::CALENDAR, live_runner) == runner::PollOutcome::RATE_LIMITED);
  CHECK(live_runner.context().calendar().is_trading_day(today).has_value());

  session.heal_calendar();
  CHECK(source.poll(runner::PollKind::CALENDAR, live_runner) == runner::PollOutcome::UPDATED);
  CHECK_FALSE(source.last_error().has_value());
}

TEST_CASE("a broker that publishes no calendar is asked for none") {
  // Decision 2b: the capability is absent and absent is not faked. The loop then never establishes a
  // phase, which it reports rather than guesses at.
  NoCalendarSession session;
  session.set_calendar(one_day_calendar(Date{2026, 3, 10}));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<NoCalendarSession> source(session, {samsung()});

  const auto outcome = source.poll(runner::PollKind::CALENDAR, live_runner);

  CHECK(outcome == runner::PollOutcome::UNCHANGED);
  CHECK(session.calendar_calls() == 0);
  CHECK(live_runner.context().calendar().days_.empty());
}

namespace {

/** A working-order row with an id, a filled quantity, and optionally a stated charge. */
WorkingOrder<FakeBroker> charged_row(
  const char* id,
  const char* filled,
  std::optional<const char*> commission,
  const Currency& currency = Currency::krw()
) {
  auto row = working();
  row.order_.order_id_ = id;
  row.ref_ = OrderRef<FakeBroker>{id, ""};
  row.order_.execution_.filled_quantity_ = Decimal64::parse(filled);
  if (commission) {
    row.order_.execution_.commission_ = Money{Decimal64::parse(*commission), currency};
  }
  return row;
}

/** Polls working orders once against `rows`. */
template <typename Source, typename Runner>
void poll_rows(
  Source& source,
  Runner& live_runner,
  FakeSession<WithWorkingOrders>& session,
  std::vector<WorkingOrder<FakeBroker>> rows
) {
  WorkingOrders<FakeBroker> listing;
  listing.orders_ = std::move(rows);
  session.set_working_orders(std::move(listing));
  REQUIRE(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) ==
          runner::PollOutcome::UPDATED);
}

} // namespace

TEST_CASE("a broker-stated commission is observed once, not once per poll") {
  // The figure on an order snapshot is *cumulative for that order*, so adding each reading would
  // multiply one commission by the number of polls that happened to see it -- and a loop polling
  // every few seconds would report a charge orders of magnitude too large.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));

  // The same order again, unchanged.
  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(source.charged_costs().orders_stating_cost_ == 1);

  // More of it fills and the charge rises: the total moves by the difference, not by the whole
  // figure again.
  poll_rows(source, live_runner, session, {charged_row("A1", "20", "2800")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("2800")));
  CHECK(source.charged_costs().orders_stating_cost_ == 1);
}

TEST_CASE("an order stating no commission is an absence, not a charge of zero") {
  // Every KIS order is one of these: its snapshots carry no commission field at all. A currency
  // reading zero would be this library claiming the broker charged nothing.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("K1", "10", std::nullopt)});
  CHECK_FALSE(source.charged_costs().commission_in(Currency::krw()));
  CHECK(source.charged_costs().orders_stating_none_ == 1);
  CHECK(source.charged_costs().orders_stating_cost_ == 0);

  // A stated zero is a different answer, and it is counted as a statement.
  poll_rows(source, live_runner, session, {charged_row("T1", "10", "0")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("0")));
  CHECK(source.charged_costs().orders_stating_cost_ == 1);
}

TEST_CASE("a stale reading at the same filled quantity does not reduce a charge") {
  // Nothing on an order snapshot says which of two readings the broker sent later: `filled_at_`
  // timestamps the latest fill and `received_at_` is local arrival. So at an equal filled quantity
  // the held value stands and the disagreement is reported, rather than being settled by whichever
  // payload happened to arrive second.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  poll_rows(source, live_runner, session, {charged_row("A1", "10", "900")});

  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(source.charged_costs().conflicting_restatements_ == 1);
}

TEST_CASE("costs stated in two currencies are kept apart and never summed") {
  // `Money` refuses to add unlike currencies, and a Toss universe can span KR and US. One scalar
  // total would either be wrong or would have to refuse to exist.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {
    charged_row("KR1", "10", "1400"),
    charged_row("US1", "10", "2", Currency::usd()),
  });

  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(source.charged_costs().commission_in(Currency::usd()) ==
        std::optional<Decimal64>(Decimal64::parse("2")));
  CHECK(source.charged_costs().commission_.size() == 2);
}

TEST_CASE("an order that fills between two polls is missed, and the counter does not notice") {
  // **This test asserts the limitation rather than a feature**, because the figure is documented as
  // a subtotal of unknown coverage and a reader has to be able to see why. A loop polls *working*
  // orders; an order placed and completely filled between two polls appears in no listing at all,
  // so its charge is absent from the subtotal and the disappearance counter -- which only sees rows
  // that were once present -- reads zero over it. A total with known coverage needs a deliberate
  // order-history poll, which this loop does not make.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  // "B1" was placed and filled in here, and was never listed.
  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});

  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK(source.charged_costs().working_list_disappearances_ == 0);

  // And when a row *does* leave the listing, the counter says only that it left -- which happens on
  // a cancellation exactly as it does on a fill, so it certifies nothing either way.
  poll_rows(source, live_runner, session, {});
  CHECK(source.charged_costs().working_list_disappearances_ == 1);
}

TEST_CASE("silence between two stated figures does not add the second one twice") {
  // Found in review. The held reading was overwritten with empty while the total kept the old
  // amount, so the next stated figure was added in full on top of it: 100, then silence, then 150
  // reported 250. The held state and the total have to stay in step.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "100")});
  poll_rows(source, live_runner, session, {charged_row("A1", "20", std::nullopt)});
  // Silence withdrew nothing.
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("100")));

  poll_rows(source, live_runner, session, {charged_row("A1", "30", "150")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("150")));
}

TEST_CASE("a first figure at an unchanged filled quantity is taken, not discarded") {
  // Also found in review. Only a strictly greater filled quantity was accepted, so an order seen
  // first without a figure and then with one at the same quantity was ignored entirely -- and it
  // was not a conflict either, because that check needed two present amounts. The charge simply
  // vanished from the subtotal.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", std::nullopt)});
  CHECK_FALSE(source.charged_costs().commission_in(Currency::krw()));
  CHECK(source.charged_costs().orders_stating_none_ == 1);

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  // Counted as stating a cost now, and still counted as having been first seen without one: both
  // counters are monotone, so a run-to-run subtraction of either cannot wrap.
  CHECK(source.charged_costs().orders_stating_cost_ == 1);
  CHECK(source.charged_costs().orders_stating_none_ == 1);
  // A first figure is new information rather than a disagreement.
  CHECK(source.charged_costs().conflicting_restatements_ == 0);
}

TEST_CASE("an order that changes the currency of its charge is a conflict, not a replacement") {
  // An order does not change the currency of its own charge. Replacing it would strand what was
  // already accumulated in the first currency and base every later delta on the second.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "1400")});
  poll_rows(source, live_runner, session,
            {charged_row("A1", "20", "3", Currency::usd())});

  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
  CHECK_FALSE(source.charged_costs().commission_in(Currency::usd()));
  CHECK(source.charged_costs().conflicting_restatements_ == 1);
}

TEST_CASE("a tax restated at one filled quantity is noticed too") {
  // The disagreement check read commission and not tax, so a tax figure that changed under an
  // unchanged quantity was neither taken nor reported.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto first = charged_row("A1", "10", "1400");
  first.order_.execution_.tax_ = Money{Decimal64::parse("500"), Currency::krw()};
  poll_rows(source, live_runner, session, {first});

  auto restated = charged_row("A1", "10", "1400");
  restated.order_.execution_.tax_ = Money{Decimal64::parse("300"), Currency::krw()};
  poll_rows(source, live_runner, session, {restated});

  CHECK(source.charged_costs().tax_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("500")));
  CHECK(source.charged_costs().conflicting_restatements_ == 1);
}

TEST_CASE("an order that starts stating nothing and later states a charge differences cleanly") {
  // The run-scope failure review found: `orders_stating_none_` used to fall when an order first
  // stated a charge, and `since` subtracts it as an unsigned count -- so the ordinary transition
  // of one order across two runs reported a count of `0 - 1`, wrapping to an enormous number.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", std::nullopt)});
  const auto after_first_run = source.charged_costs();

  poll_rows(source, live_runner, session, {charged_row("A1", "20", "1400")});
  const auto second_run = source.charged_costs().since(after_first_run);

  CHECK(second_run.orders_stating_cost_ == 1);
  CHECK(second_run.orders_stating_none_ == 0);
  CHECK(second_run.commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("1400")));
}

TEST_CASE("a fee arriving after a silent row at the same new quantity is taken, not discarded") {
  // Found at round 4, and it is the sharper form of the silence case. A silent row advanced the
  // order's filled quantity while the held figure stayed at the quantity it was accepted at, so
  // the fee that then arrived **for the new quantity** looked like a second opinion about the old
  // one and was dropped as a conflict. The quantity a figure was accepted at therefore lives with
  // the figure, not with the order.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "100")});
  poll_rows(source, live_runner, session, {charged_row("A1", "20", std::nullopt)});
  poll_rows(source, live_runner, session, {charged_row("A1", "20", "150")});

  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("150")));
  // New information, not a disagreement: nothing had been accepted at twenty shares.
  CHECK(source.charged_costs().conflicting_restatements_ == 0);

  // And a second, differing figure at that same quantity *is* a disagreement.
  poll_rows(source, live_runner, session, {charged_row("A1", "20", "900")});
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("150")));
  CHECK(source.charged_costs().conflicting_restatements_ == 1);
}

TEST_CASE("a tax arriving after a silent row at the same new quantity is taken too") {
  // The same rule, on the other component, because the first version of this check read commission
  // and not tax.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto first = charged_row("A1", "10", "100");
  first.order_.execution_.tax_ = Money{Decimal64::parse("50"), Currency::krw()};
  poll_rows(source, live_runner, session, {first});
  poll_rows(source, live_runner, session, {charged_row("A1", "20", std::nullopt)});

  auto later = charged_row("A1", "20", "150");
  later.order_.execution_.tax_ = Money{Decimal64::parse("80"), Currency::krw()};
  poll_rows(source, live_runner, session, {later});

  CHECK(source.charged_costs().tax_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("80")));
  CHECK(source.charged_costs().conflicting_restatements_ == 0);
}

TEST_CASE("a sum that will not fit is counted, not silently left out of the total") {
  // Each figure is representable on its own and their sum is not. The old accumulator swallowed the
  // failed add and the caller recorded the amount as counted anyway, so the subtotal stayed at the
  // first order's figure and nothing said it was short.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {
    charged_row("A1", "10", "5000000000000000000"),
    charged_row("A2", "10", "5000000000000000000"),
  });

  CHECK(source.charged_costs().unrepresentable_sums_ == 1);
  // The first figure stands; the second was left out rather than approximated, and the count is
  // what says the total is incomplete.
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("5000000000000000000")));
}

TEST_CASE("a zero stated in a currency already seen is still an observation of that run") {
  // Round 6's counterexample, and the narrower half of the one before it. Keeping a zero only when
  // its currency was *new* left the ordinary case broken: a run whose only observation is an order
  // stating zero, in a currency an earlier run already saw, moves no total -- so a delta computed
  // from the money alone omitted it and the run reported absence where the broker had spoken.
  // What decides whether something was stated is the count of accepted statements, not the money.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto first = charged_row("A1", "10", "5");
  first.order_.execution_.tax_ = Money{Decimal64::parse("3"), Currency::krw()};
  poll_rows(source, live_runner, session, {first});
  const auto after_first_run = source.charged_costs();

  auto second = charged_row("B1", "10", "0");
  second.order_.execution_.tax_ = Money{Decimal64::parse("0"), Currency::krw()};
  poll_rows(source, live_runner, session, {second});

  const auto second_run = source.charged_costs().since(after_first_run);
  // The broker stated zero, so the run states zero -- not nothing.
  CHECK(second_run.commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("0")));
  CHECK(second_run.tax_in(Currency::krw()) == std::optional<Decimal64>(Decimal64::parse("0")));
  CHECK(second_run.commission_statements_in(Currency::krw()) == 1);
  CHECK(second_run.orders_stating_cost_ == 1);

  // And a run that observed nothing new still reports nothing, which is the other half: the
  // distinction is between a statement of zero and no statement, not between zero and non-zero.
  const auto after_second_run = source.charged_costs();
  poll_rows(source, live_runner, session, {second});
  const auto third_run = source.charged_costs().since(after_second_run);
  CHECK_FALSE(third_run.commission_in(Currency::krw()));
  CHECK(third_run.commission_.empty());
}

TEST_CASE("a per-order delta that cannot be represented is counted, not dropped") {
  // The same failure one step earlier, and it was silent for the same reason: `take_figure`
  // returned without contributing when the delta between two stated figures would not fit, so the
  // subtotal stayed put and nothing said a figure had been left out.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {charged_row("A1", "10", "5000000000000000000")});
  // A greater filled quantity restating the figure at a scale that cannot be aligned with it.
  poll_rows(source, live_runner, session, {charged_row("A1", "20", "0.1")});

  CHECK(source.charged_costs().unrepresentable_sums_ == 1);
  // The held figure stands, because nothing was contributed: the invariant survives, and the
  // count is what says the answer is short.
  CHECK(source.charged_costs().commission_in(Currency::krw()) ==
        std::optional<Decimal64>(Decimal64::parse("5000000000000000000")));
}

TEST_CASE("a hand-assembled live runner will not place against an account it never read") {
  // `live::bind` is not the only way to reach a broker: `doxygen/vignettes/binding-a-broker.md`
  // shows a `Runner` assembled directly on a `SessionExecutor`, and that path has no factory and no
  // `PollingLoop` to hold anything. The requirement therefore travels with the executor --
  // `SessionExecutor::reaches_a_broker` -- rather than being installed by whoever built the runner.
  using Runner = runner::Runner<
    HoldTen, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;

  SECTION("an unread account places nothing, and says why") {
    Runner live_runner(HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
    live_runner.context().set_instrument(samsung_reference());
    live_runner.context().set_phase(strategy::MarketPhase::OPEN);
    live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

    const auto report = live_runner.feed(bar_at(0, "70000"));

    // Nothing reached the broker, which is the assertion that matters.
    CHECK(session.placed().empty());
    CHECK(report.would_place_.empty());
    REQUIRE(report.withheld_plans_.size() == 1);
    CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::PORTFOLIO_ABSENT);
    CHECK_FALSE(report.quiet());
  }

  SECTION("a stamped read lets the same order go") {
    Runner live_runner(HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
    live_runner.context().set_instrument(samsung_reference());
    live_runner.context().set_phase(strategy::MarketPhase::OPEN);
    live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
    live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

    const auto report = live_runner.feed(bar_at(0, "70000"));

    CHECK(session.placed().size() == 1);
    CHECK(report.would_place_.size() == 1);
    CHECK(report.withheld_plans_.empty());
  }

  SECTION("so does saying the run expects no holdings") {
    Runner live_runner(HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
    live_runner.context().set_instrument(samsung_reference());
    live_runner.context().set_phase(strategy::MarketPhase::OPEN);
    live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
    live_runner.allow_absent_portfolio();

    const auto report = live_runner.feed(bar_at(0, "70000"));

    CHECK(session.placed().size() == 1);
    CHECK(report.withheld_plans_.empty());
  }
}

TEST_CASE("a dry run is not caught by the live requirement") {
  // The other side of the executor property, and the reason it is a property of the executor rather
  // than a default on every runner: a dry run has no holdings inquiry that could have failed.
  STATIC_REQUIRE_FALSE(
    runner::runner_detail::sends_to_a_broker<runner::RecordingExecutor<runner::DryTag>>::value);
  STATIC_REQUIRE(
    runner::runner_detail::sends_to_a_broker<live::SessionExecutor<FakeSession<WithKey>>>::value);
  // And journalling does not change who receives the order.
  STATIC_REQUIRE(runner::runner_detail::sends_to_a_broker<
    runner::JournalingExecutor<live::SessionExecutor<FakeSession<WithKey>>, runner::MemoryJournal>
  >::value);
  STATIC_REQUIRE_FALSE(runner::runner_detail::sends_to_a_broker<
    runner::JournalingExecutor<runner::RecordingExecutor<runner::DryTag>, runner::MemoryJournal>
  >::value);
}

namespace {

/**
 * A consumer's own adapter: it forwards to a real session and declares nothing.
 *
 * The shape the fail-closed default exists for. Nothing about this type tells the library it reaches
 * a broker -- it has no marker, it is not in `live/`, and its name is its author's choice -- and yet
 * every order it is handed goes to one. A trait that defaulted to "harmless" would exempt exactly
 * this, which is the one case the exemption cannot be afforded for.
 */
template <typename Session>
class UnmarkedAdapter {
public:
  explicit UnmarkedAdapter(Session& session) : inner_(session) {}

  runner::SendOutcome place(const strategy::PlannedOrder& order) { return inner_.place(order); }
  runner::SendOutcome cancel(const WorkingOrder<FakeBroker>& working) {
    return inner_.cancel(working);
  }

private:
  live::SessionExecutor<Session> inner_;
};

} // namespace

TEST_CASE("an executor that says nothing is assumed to reach a broker") {
  // The default is `true` and an executor opts out, because the two directions fail differently: an
  // unmarked adapter assumed harmless sends a real order against an account nobody read, and an
  // unmarked double assumed live refuses until it is given a portfolio. Only the second is
  // discoverable, so silence means live.
  STATIC_REQUIRE(runner::runner_detail::sends_to_a_broker<
    UnmarkedAdapter<FakeSession<WithKey>>>::value);

  using Runner =
    runner::Runner<HoldTen, WithKey, FakeBroker, UnmarkedAdapter<FakeSession<WithKey>>>;
  FakeSession<WithKey> session;

  SECTION("and so will not place against an account it never read") {
    Runner live_runner(HoldTen{}, {}, 512, UnmarkedAdapter<FakeSession<WithKey>>(session));
    live_runner.context().set_instrument(samsung_reference());
    live_runner.context().set_phase(strategy::MarketPhase::OPEN);
    live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));

    const auto report = live_runner.feed(bar_at(0, "70000"));

    CHECK(session.placed().empty());
    REQUIRE(report.withheld_plans_.size() == 1);
    CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::PORTFOLIO_ABSENT);
  }

  SECTION("a stamped read lets it through, so the default is not simply a refusal") {
    Runner live_runner(HoldTen{}, {}, 512, UnmarkedAdapter<FakeSession<WithKey>>(session));
    live_runner.context().set_instrument(samsung_reference());
    live_runner.context().set_phase(strategy::MarketPhase::OPEN);
    live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
    live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

    const auto report = live_runner.feed(bar_at(0, "70000"));

    CHECK(session.placed().size() == 1);
    CHECK(report.withheld_plans_.empty());
  }
}
