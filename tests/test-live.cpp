#include <algotradelib/backtest>
#include <algotradelib/kis>
#include <algotradelib/live>
#include <algotradelib/tossinvest>
#include <algotradelib/vanilla>

#include "live_program.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdio>
#include <deque>
#include <optional>
#include <sstream>
#include <stdexcept>
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
 * Capability variants for the price band, for the reason the calendar pair states below.
 *
 * All three shipped adapters publish a band, so the `false` side is the one nothing would otherwise
 * instantiate -- and a branch inside `if constexpr` that nothing instantiates has never been
 * compiled at all.
 */
struct WithPriceLimits : strategy::NoCaps {
  static constexpr bool has_price_limits = true;
  static constexpr bool has_working_orders = true;
};
struct WithoutPriceLimits : strategy::NoCaps {
  static constexpr bool has_price_limits = false;
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
    if (throw_on_place_) {
      throw std::runtime_error("the fake session was told to throw");
    }
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
    // **Placement only**, so a test can have the broker refuse an *order* while every read still
    // answers. The session-wide switch below fails the polls too, which makes a loop that never
    // feeds a bar and therefore never places anything -- a fixture that arranges for the subject
    // not to happen.
    if (place_error_) {
      return Result<Placed>(*place_error_);
    }
    if (error_) {
      return Result<Placed>(*error_);
    }
    Placed ack;
    // The broker's number for this order: the same one every time, unless a test queued its own.
    std::string id = "0001569139";
    if (!order_ids_.empty()) {
      id = order_ids_.front();
      order_ids_.pop_front();
    }
    ack.ack_.order_id_ = id;
    ack.ref_ = Ref{id, ""};
    return Result<Placed>(std::move(ack));
  }

  /** The numbers the next placements are acknowledged with, in order. */
  std::deque<std::string> order_ids_;

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

  /** Makes every later **placement** fail with exactly `error`, so a test can pin the broker's words. */
  void fail_place_with(BrokerError error) { place_error_ = std::move(error); }

  /** Lets placements succeed again. */
  void heal_place() { place_error_.reset(); }

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
    const auto own = bars_for_.find(query.instrument_.symbol_);
    BarSeries series = own != bars_for_.end() ? own->second : bars_;
    series.instrument_ = query.instrument_;
    return Result<BarSeries>(std::move(series));
  }

  /** Scripts one instrument's bars, so two instruments' reads can end at different times. */
  void set_bars_for(const std::string& symbol, BarSeries series) { bars_for_[symbol] = std::move(series); }
  std::map<std::string, BarSeries> bars_for_;

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

  /**
   * Returns today's band for `instrument`, or the failure a test asked for on that one instrument.
   *
   * Per instrument for the reason `buying_power` is per currency: the case worth testing is the
   * mixed one, where the broker answers about one name and fails about another, which is not an
   * outage anybody notices and is exactly when a band from another day goes on being applied.
   */
  Result<PriceLimits> price_limits(const InstrumentId& instrument) {
    price_limits_asked_.push_back(instrument);
    for (const auto& failing : failing_instruments_) {
      if (failing == instrument) {
        BrokerError error;
        error.category_ = ErrorCategory::TRANSPORT;
        return Result<PriceLimits>(error);
      }
    }
    for (const auto& throttled : throttled_instruments_) {
      if (throttled == instrument) {
        BrokerError error;
        error.category_ = ErrorCategory::RATE_LIMIT;
        return Result<PriceLimits>(error);
      }
    }
    if (error_) {
      return Result<PriceLimits>(*error_);
    }
    PriceLimits limits;
    limits.currency_ = Currency{"KRW"};
    limits.upper_ = price_limits_upper_;
    return Result<PriceLimits>(std::move(limits));
  }

  /** Sets the ceiling a successful band inquiry answers with. */
  void set_price_limit_upper(const char* price) { price_limits_upper_ = Decimal64::parse(price); }

  /** Makes the band inquiry fail for `instrument` alone. */
  void fail_price_limits_for(const InstrumentId& instrument) {
    failing_instruments_.push_back(instrument);
  }

  /** Makes the band inquiry answer `instrument` with a rate-limit refusal alone. */
  void throttle_price_limits_for(const InstrumentId& instrument) {
    throttled_instruments_.push_back(instrument);
  }

  /** Returns every instrument the band inquiry was asked about. */
  const std::vector<InstrumentId>& price_limits_asked() const noexcept {
    return price_limits_asked_;
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

  std::vector<InstrumentId> price_limits_asked_;
  std::vector<InstrumentId> failing_instruments_;
  std::vector<InstrumentId> throttled_instruments_;
  Decimal64 price_limits_upper_{Decimal64::parse("71000")};

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
  Result<OrderPage> order_history(const OrderHistoryQuery& query) {
    ++history_calls_;
    history_queries_.push_back(query);
    if (history_by_query_) {
      return Result<OrderPage>(history_by_query_(query));
    }
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
  /**
   * Returns the scripted reference data, or a refusal when none was scripted.
   *
   * Independent of the session-wide failure switch, so a test can fail every poll and still get as
   * far as the loop.
   */
  Result<Instrument> instrument(const InstrumentId& id) {
    if (!reference_ || !(reference_->id_ == id)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = true;
      error.message_ = "no reference data scripted for " + id.symbol_;
      return Result<Instrument>(error);
    }
    return Result<Instrument>(*reference_);
  }
  void set_reference(Instrument reference) { reference_ = std::move(reference); }
  /** Makes every placement throw, as a failing executor would, until told otherwise. */
  void throw_on_place(bool enabled = true) { throw_on_place_ = enabled; }
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
  /** Answers each history query itself, where a test needs a page per date. */
  std::function<OrderPage(const OrderHistoryQuery&)> history_by_query_;
  const std::vector<OrderHistoryQuery>& history_queries() const noexcept { return history_queries_; }

private:
  BarSeries bars_;
  QuoteSnapshot quote_;
  HoldingsSnapshot holdings_;
  std::optional<Instrument> reference_;
  bool throw_on_place_{false};
  std::vector<OrderHistoryQuery> history_queries_;

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
  std::optional<BrokerError> place_error_;
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

TEST_CASE("a stop's trigger reaches the session", "[trigger]") {
  // A copy that was never written agrees with the default -- which here is "no trigger", so the
  // order would go out as a plain limit.
  FakeSession<WithoutKey> session;
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session);

  auto order = planned();
  order.type_ = OrderType::STOP_LIMIT;
  order.trigger_price_ = Decimal64::parse("69500");
  REQUIRE(executor.place(order) == runner::SendOutcome::ACCEPTED);
  REQUIRE(session.placed().size() == 1);
  REQUIRE(session.placed()[0].trigger_price_ == std::optional<Decimal64>(Decimal64::parse("69500")));
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

TEST_CASE("a target restated before any listing poll places once, with no limit set", "[own-orders]") {
  // A runner fed several bars before a listing -- by hand, as here, or as `SessionSource::poll_bars`
  // fed a cold start's history before it fed history through `feed_warmup` -- and a target strategy
  // restates its target on every one. The broker's listing is not read again between them. With no limit that reads
  // what is outstanding, the second pass used to start from that listing, see nothing working, and
  // send the same order again: one per history bar, all of them filled at the open.
  using ListedRunner = runner::Runner<
    HoldTen, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  FakeSession<WithWorkingOrders> session;
  ListedRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithWorkingOrders>("0", nullptr));

  for (int hour = 0; hour < 5; ++hour) {
    live_runner.feed(bar_at(hour, "70000"));
  }

  REQUIRE(session.placed().size() == 1);
  // The order is working as far as the next pass is concerned, and it is the reservation this runner
  // wrote, which the broker's next listing replaces.
  REQUIRE(live_runner.context().portfolio().working_.size() == 1);
  CHECK(live_runner.context().portfolio().working_[0].reserved_);
  CHECK(live_runner.context().portfolio().working_quantity(samsung(), OrderSide::BUY) ==
        Decimal64::parse("10"));
  // And its money is not spent from the published figure, because no buying-power check asked for it.
  CHECK(live_runner.context().portfolio().buying_power_.empty());
}

namespace {

/** States one scripted instruction per bar, then nothing. */
struct Scripted {
  std::deque<std::variant<strategy::TargetPosition, strategy::OrderIntent>>* next_{nullptr};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    if (next_ == nullptr || next_->empty()) {
      return;
    }
    std::visit([&](const auto& instruction) { ctx.submit(instruction); }, next_->front());
    next_->pop_front();
  }
};

using ScriptedRunner = runner::Runner<
  Scripted, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
>;

strategy::TargetPosition target_of(const char* quantity) {
  strategy::TargetPosition target;
  target.instrument_ = samsung();
  target.target_quantity_ = Decimal64::parse(quantity);
  return target;
}

/** A row as the broker lists it: its own number, unfilled or partly filled. */
WorkingOrder<FakeBroker> listed_row(
  const char* id, OrderSide side, const char* quantity, const char* filled, OrderType type = OrderType::MARKET
) {
  WorkingOrder<FakeBroker> row;
  row.ref_ = OrderRef<FakeBroker>{id, ""};
  row.order_.order_id_ = id;
  row.order_.instrument_ = samsung();
  row.order_.side_ = side;
  row.order_.type_ = type;
  row.order_.quantity_ = Decimal64::parse(quantity);
  row.order_.execution_.filled_quantity_ = Decimal64::parse(filled);
  row.order_.status_ =
    Decimal64::parse(filled).coefficient() == 0 ? OrderStatus::PENDING : OrderStatus::PARTIAL_FILLED;
  row.amendable_quantity_ = *decimal_subtract(row.order_.quantity_, row.order_.execution_.filled_quantity_);
  return row;
}

/** A history row: the order ended, with `filled` executed. */
OrderSnapshot ended_row(const char* id, OrderSide side, const char* quantity, const char* filled) {
  OrderSnapshot row;
  row.order_id_ = id;
  row.instrument_ = samsung();
  row.side_ = side;
  row.quantity_ = Decimal64::parse(quantity);
  row.execution_.filled_quantity_ = Decimal64::parse(filled);
  row.status_ = OrderStatus::CANCELED;
  return row;
}

/** What a holdings read and a listing read answered, both at hour `hour`. */
strategy::Portfolio<FakeBroker> polled(int hour, const char* held, std::vector<WorkingOrder<FakeBroker>> rows) {
  auto portfolio = held_position<WithWorkingOrders>(held, nullptr);
  portfolio.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + hour);
  portfolio.working_as_of_ = portfolio.as_of_;
  portfolio.working_ = std::move(rows);
  return portfolio;
}

/** Installs a poll's answer as `SessionSource` does: holdings and listing replaced, wholesale. */
void install(ScriptedRunner& runner_instance, strategy::Portfolio<FakeBroker> portfolio) {
  runner_instance.context().set_portfolio(std::move(portfolio));
}

struct ConfirmRig {
  FakeSession<WithWorkingOrders> session;
  std::deque<std::variant<strategy::TargetPosition, strategy::OrderIntent>> script;
  ScriptedRunner runner_instance{
    Scripted{&script}, {}, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  };

  ConfirmRig() {
    // The bars here are an hour apart; the cases that are about the timeout set their own.
    runner_instance.set_confirmation_timeout(std::chrono::hours(24));
    runner_instance.context().set_instrument(samsung_reference());
    runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
    runner_instance.context().set_last_price(samsung(), Decimal64::parse("70000"));
    install(runner_instance, polled(0, "0", {}));
  }

  Decimal64 working_buys() const {
    return runner_instance.context().portfolio().working_quantity(samsung(), OrderSide::BUY);
  }
};

} // namespace

TEST_CASE("a filled market order no listing shows stays counted until holdings show it", "[confirm]") {
  // Lab F15: a market order that filled at once is in no listing, and holdings lag the fill. The
  // listing used to retire its reservation, and the next bar's target bought again.
  ConfirmRig rig;
  for (int at = 0; at < 3; ++at) {
    rig.script.push_back(target_of("10"));
  }
  rig.runner_instance.feed(bar_at(1, "70000"));
  REQUIRE(rig.session.placed().size() == 1);

  // The listing is empty and holdings still say nothing: the order is still counted.
  install(rig.runner_instance, polled(2, "0", {}));
  rig.runner_instance.feed(bar_at(2, "70000"));
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);

  // Holdings show the ten: confirmed, the row is gone, and nothing more is sent.
  install(rig.runner_instance, polled(3, "10", {}));
  rig.runner_instance.feed(bar_at(3, "70000"));
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  CHECK(rig.runner_instance.context().portfolio().working_.empty());
}

TEST_CASE("a listed partial fill is counted once, whatever holdings lag", "[confirm]") {
  ConfirmRig rig;
  for (int at = 0; at < 4; ++at) {
    rig.script.push_back(target_of("10"));
  }
  rig.runner_instance.feed(bar_at(1, "70000"));
  REQUIRE(rig.session.placed().size() == 1);

  // Listed with four filled, holdings not showing them yet: six listed plus four unexplained.
  install(rig.runner_instance, polled(2, "0", {listed_row("0001569139", OrderSide::BUY, "10", "4")}));
  rig.runner_instance.feed(bar_at(2, "70000"));
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));

  // Holdings show the four: only the listing's six remain.
  install(rig.runner_instance, polled(3, "4", {listed_row("0001569139", OrderSide::BUY, "10", "4")}));
  rig.runner_instance.feed(bar_at(3, "70000"));
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("6"));
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);

  // Filled and gone from the listing, holdings show all ten.
  install(rig.runner_instance, polled(4, "10", {}));
  rig.runner_instance.feed(bar_at(4, "70000"));
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
}

TEST_CASE("an order that left the listing is settled by its history", "[confirm]") {
  ConfirmRig rig;
  for (int at = 0; at < 4; ++at) {
    rig.script.push_back(target_of("10"));
  }
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(2, "0", {listed_row("0001569139", OrderSide::BUY, "10", "0")}));
  rig.runner_instance.feed(bar_at(2, "70000"));
  REQUIRE(rig.session.placed().size() == 1);
  // A listing carried it, so no history is needed while it does.
  CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  // Gone from the listing, holdings unchanged: cancelled or filled, only its history says.
  install(rig.runner_instance, polled(3, "0", {}));
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  SECTION("ended with nothing filled: forgotten, and the target sends again") {
    rig.runner_instance.confirm_from_history({ended_row("0001569139", OrderSide::BUY, "10", "0")});
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
    rig.runner_instance.feed(bar_at(3, "70000"));
    CHECK(rig.session.placed().size() == 2);
  }

  SECTION("ended with three filled: those three wait for holdings, and only seven are sent again") {
    rig.runner_instance.confirm_from_history({ended_row("0001569139", OrderSide::BUY, "10", "3")});
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("3"));
    CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
    rig.runner_instance.feed(bar_at(3, "70000"));
    REQUIRE(rig.session.placed().size() == 2);
    CHECK(rig.session.placed()[1].quantity_ == Decimal64::parse("7"));
  }

  SECTION("a history row still open does not settle it") {
    auto open = ended_row("0001569139", OrderSide::BUY, "10", "0");
    open.status_ = OrderStatus::PENDING;
    rig.runner_instance.confirm_from_history({open});
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));
  }
}

namespace {
/** A placement whose request was written and whose reply never came. */
BrokerError reply_lost() {
  BrokerError error;
  error.category_ = ErrorCategory::TRANSPORT;
  error.message_ = "the reply was lost";
  return error;
}
} // namespace

TEST_CASE("an order whose reply was lost is counted until holdings show it, once a sweep finds it", "[confirm]") {
  // Lab F35: the sweep found the filled order in the listing and cleared the ledger; holdings lagged
  // the fill, no listing carried it, and the next bar's target bought again -- twice the target.
  ConfirmRig rig;
  for (int at = 0; at < 4; ++at) {
    rig.script.push_back(target_of("10"));
  }
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.session.heal_place();
  REQUIRE(rig.session.placed().size() == 1);
  REQUIRE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  // Counted from the send, with no reference, while the ledger holds the question.
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));

  SECTION("matched: counted on until holdings show it") {
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("0001569139"));
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));

    install(rig.runner_instance, polled(2, "0", {}));
    rig.runner_instance.feed(bar_at(2, "70000"));
    CHECK(rig.session.placed().size() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));

    install(rig.runner_instance, polled(3, "10", {}));
    rig.runner_instance.feed(bar_at(3, "70000"));
    CHECK(rig.session.placed().size() == 1);
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
    CHECK(rig.runner_instance.context().portfolio().working_.empty());
  }

  SECTION("matched while resting: the listing covers it, by the reference the sweep named") {
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("0001569139"));
    install(rig.runner_instance, polled(2, "0", {listed_row("0001569139", OrderSide::BUY, "10", "0")}));
    rig.runner_instance.feed(bar_at(2, "70000"));
    CHECK(rig.session.placed().size() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));
  }

  SECTION("resolved after a listing replaced the book: no row is left behind twice") {
    install(rig.runner_instance, polled(2, "0", {}));
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("0001569139"));
    CHECK(rig.working_buys() == Decimal64::parse("10"));
  }

  SECTION("never placed: forgotten, and the target sends again") {
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::nullopt);
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
    CHECK(rig.runner_instance.context().portfolio().working_.empty());
    install(rig.runner_instance, polled(2, "0", {}));
    rig.runner_instance.feed(bar_at(2, "70000"));
    CHECK(rig.session.placed().size() == 2);
  }

  SECTION("another instrument's answer, or the other side's, leaves it alone") {
    rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::SELL, std::nullopt);
    rig.runner_instance.resolve_indeterminate_placement(
      InstrumentId{Country::KR, Venue::COMBINED, "000660"}, OrderSide::BUY, std::nullopt
    );
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  }
}

TEST_CASE("an order whose reply was lost carries no reference, not the previous order's", "[confirm]") {
  // `last_ref()` still names the last accepted order after a failed send. A row carrying it could be
  // withdrawn in that order's place.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.script.push_back(target_of("0"));
  rig.script.push_back(target_of("10"));
  rig.runner_instance.feed(bar_at(1, "70000"));
  REQUIRE(rig.session.placed().size() == 1);
  install(rig.runner_instance, polled(2, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.runner_instance.unconfirmed_orders() == 0);

  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(2, "70000"));
  rig.session.heal_place();
  REQUIRE(rig.session.placed().size() == 2);
  REQUIRE(rig.session.placed()[1].side_ == OrderSide::SELL);
  const auto& working = rig.runner_instance.context().portfolio().working_;
  REQUIRE(working.size() == 1);
  CHECK(working.front().ref_.primary_.empty());

  // A target opposing the row withdraws nothing at the broker: the row names no order.
  rig.runner_instance.feed(bar_at(3, "70000"));
  CHECK(rig.session.canceled().empty());
  CHECK(rig.session.placed().size() == 2);
}

TEST_CASE("an order whose reply was lost does not time out on its own", "[confirm]") {
  // Its instrument is already withheld; a timeout's mark would change nothing, and dropping the entry
  // would lose the count while the question is still open.
  ConfirmRig rig;
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(10));
  rig.script.push_back(target_of("10"));
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.session.heal_place();
  // Twenty-nine hours on, by the loop's clock and by a holdings read, with an empty history answer.
  auto late = polled(30, "0", {});
  rig.runner_instance.set_observed_at(late.as_of_);
  install(rig.runner_instance, std::move(late));
  CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({});
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 0);
}

TEST_CASE("an order whose reply was lost times out once the ledger is cleared by hand", "[confirm]") {
  // An operator clearing an `ambiguous` sweep by hand lifts the hold without naming the order. If it
  // never existed, nothing would ever cover it: it times out instead, visibly, and withholds again.
  ConfirmRig rig;
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(10));
  rig.script.push_back(target_of("10"));
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.session.heal_place();
  rig.runner_instance.ledger().resolve_after_reading_orders(samsung());

  auto late = polled(30, "0", {});
  rig.runner_instance.set_observed_at(late.as_of_);
  install(rig.runner_instance, std::move(late));
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  SECTION("its history has nothing for it") {
    rig.runner_instance.confirm_from_history({});
  }
  SECTION("a history row whose number is missing is not taken for it") {
    // An empty `odno`/`ord_no` reads as an empty string; it must not equal a reference not yet named.
    rig.runner_instance.confirm_from_history({ended_row("", OrderSide::BUY, "10", "0")});
  }
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
  CHECK(rig.session.placed().size() == 1);
}

TEST_CASE("an unconfirmed order past the timeout withholds its instrument", "[confirm]") {
  // A wrong timeout costs trading on the instrument, never a duplicate: the ledger mark is cleared
  // only by an operator who has read the account.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(1, "0", {}));
  CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  rig.runner_instance.set_observed_at(
    std::chrono::system_clock::time_point{} + std::chrono::hours(500001) + std::chrono::minutes(2)
  );
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  // The history has no answer, but the holdings in hand predate the deadline: not yet. It waits on
  // holdings now, without asking for the history again on every listing poll.
  rig.runner_instance.confirm_from_history({});
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  // A holdings read taken past it that still does not show the order: withheld.
  auto late = polled(1, "0", {});
  late.as_of_ = late.working_as_of_ =
    std::chrono::system_clock::time_point{} + std::chrono::hours(500001) + std::chrono::minutes(2);
  install(rig.runner_instance, late);
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
  CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
  CHECK(rig.runner_instance.context().portfolio().working_.empty());
}

TEST_CASE("a fill its history reports is not marked when holdings come after the timeout", "[confirm]") {
  // A loop polling slower than the confirmation timeout reads the listing -- and so the history --
  // before the holdings that show the fill. The history's answer must stand until holdings arrive.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.script.push_back(target_of("10"));
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.runner_instance.feed(bar_at(1, "70000"));
  // A market order filled at once and never listed; an hour later, the listing (empty) comes before
  // the holdings poll of that iteration, and the timeout sends the runner to the history.
  install(rig.runner_instance, polled(1, "0", {}));
  rig.runner_instance.set_observed_at(
    std::chrono::system_clock::time_point{} + std::chrono::hours(500002)
  );
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  auto filled = ended_row("0001569139", OrderSide::BUY, "10", "10");
  filled.status_ = OrderStatus::FILLED;
  rig.runner_instance.confirm_from_history({filled});
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 0);
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));
  // Settled, so it asks for no more history while it waits.
  CHECK_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  SECTION("the holdings read of the same iteration confirms it; the next bar sends nothing") {
    install(rig.runner_instance, polled(2, "10", {}));
    rig.runner_instance.feed(bar_at(2, "70000"));
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
    CHECK(rig.session.placed().size() == 1);
  }

  const auto settled_at = std::chrono::system_clock::time_point{} + std::chrono::hours(500002);
  const auto holdings_at = [&](std::chrono::seconds after, const char* held) {
    auto portfolio = polled(2, held, {});
    portfolio.as_of_ = portfolio.working_as_of_ = settled_at + after;
    return portfolio;
  };

  SECTION("an hour of clock with no newer holdings read is not a timeout") {
    // The next iteration's listing poll: the clock is past the timeout, holdings are not yet read.
    rig.runner_instance.set_observed_at(settled_at + std::chrono::hours(1));
    rig.runner_instance.refresh_unconfirmed();
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    // Then that iteration's holdings show the fill.
    install(rig.runner_instance, holdings_at(std::chrono::hours(1), "10"));
    rig.runner_instance.refresh_unconfirmed();
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  }

  SECTION("a holdings read a timeout after settling that still lacks the fill withholds the instrument") {
    rig.runner_instance.set_observed_at(settled_at + std::chrono::minutes(2));
    install(rig.runner_instance, holdings_at(std::chrono::seconds(30), "0"));
    rig.runner_instance.refresh_unconfirmed();
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    install(rig.runner_instance, holdings_at(std::chrono::minutes(2), "0"));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  }
}

TEST_CASE("a fill the history does not return is not marked before the holdings that show it", "[confirm]") {
  // The unsettled side of the slow-loop race: past the timeout, the listing poll reads a history that
  // has no row for the order yet; the holdings read of the same iteration shows the fill.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(1, "0", {}));
  rig.runner_instance.set_observed_at(std::chrono::system_clock::time_point{} + std::chrono::hours(500002));
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({});
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  install(rig.runner_instance, polled(2, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 0);
}

TEST_CASE("an order explained again after a history miss starts over", "[confirm]") {
  // A listing that carries the order again explains it, so a later gap gets a fresh timeout and a
  // fresh history read, not the earlier miss.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(1, "0", {}));
  rig.runner_instance.set_observed_at(std::chrono::system_clock::time_point{} + std::chrono::hours(500002));
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({});
  REQUIRE_FALSE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());

  install(rig.runner_instance, polled(2, "0", {listed_row("0001569139", OrderSide::BUY, "10", "0")}));
  rig.runner_instance.refresh_unconfirmed();
  // Gone again: it asks for the history, which it would not while still flagged.
  install(rig.runner_instance, polled(2, "0", {}));
  CHECK(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
}

TEST_CASE("a fill that confirmed one order is not counted again for the next", "[confirm]") {
  // Two buys inside the lag, on one instrument. Holdings showing the first must leave the second
  // counted: measured from its own baseline it would look confirmed too, and the target would buy
  // its quantity a third time.
  ConfirmRig rig;
  rig.session.order_ids_ = {"A", "B"};
  rig.script.push_back(target_of("10"));
  rig.script.push_back(target_of("20"));
  rig.script.push_back(target_of("20"));
  rig.script.push_back(target_of("20"));
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.runner_instance.feed(bar_at(2, "70000"));
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.runner_instance.unconfirmed_orders() == 2);

  install(rig.runner_instance, polled(3, "10", {}));
  rig.runner_instance.feed(bar_at(3, "70000"));
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));
  CHECK(rig.session.placed().size() == 2);

  install(rig.runner_instance, polled(4, "10", {}));
  rig.runner_instance.feed(bar_at(4, "70000"));
  CHECK(rig.session.placed().size() == 2);
}

TEST_CASE("a resting sell a listing shows does not hold up the buy beside it", "[confirm]") {
  // Orders on both sides of one instrument are credited only together; a resting order a listing
  // shows in full adds nothing, so a protective sell does not keep a filled buy counted forever.
  ConfirmRig rig;
  rig.session.order_ids_ = {"S", "B"};
  install(rig.runner_instance, polled(0, "10", {}));
  const auto rule = samsung_reference().price_rule_;
  rig.script.push_back(
    strategy::OrderIntent::limit(
      samsung(), OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("10")),
      *strategy::Price::checked(Decimal64::parse("90000"), rule)
    )
  );
  rig.script.push_back(
    strategy::OrderIntent::market(samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("5")))
  );
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(1, "10", {listed_row("S", OrderSide::SELL, "10", "0", OrderType::LIMIT)}));
  rig.runner_instance.feed(bar_at(2, "70000"));
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.runner_instance.unconfirmed_orders() == 2);

  install(rig.runner_instance, polled(3, "15", {listed_row("S", OrderSide::SELL, "10", "0", OrderType::LIMIT)}));
  rig.runner_instance.feed(bar_at(3, "70000"));
  // The buy is confirmed; the sell stays, explained by its listing, with no row of its own.
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("0"));
  CHECK(rig.runner_instance.context().portfolio().working_.size() == 1);
}

TEST_CASE("a withdrawal of a counted order asks its history what filled", "[confirm]") {
  // Cancelled before any listing showed it: without this the entry would wait out the timeout.
  ConfirmRig rig;
  const auto rule = samsung_reference().price_rule_;
  rig.script.push_back(
    strategy::OrderIntent::limit(
      samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")),
      *strategy::Price::checked(Decimal64::parse("60000"), rule)
    )
  );
  rig.script.push_back(target_of("0"));
  rig.runner_instance.feed(bar_at(1, "70000"));
  REQUIRE(rig.session.placed().size() == 1);
  // The flat target withdraws the reserved buy.
  rig.runner_instance.feed(bar_at(2, "70000"));
  REQUIRE(rig.session.canceled_refs().size() == 1);
  install(rig.runner_instance, polled(2, "0", {}));
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({ended_row("0001569139", OrderSide::BUY, "10", "0")});
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
  CHECK(rig.runner_instance.context().portfolio().working_.empty());
}

TEST_CASE("a DAY order's history row is final once its session closed", "[confirm]") {
  // A broker may leave an expired DAY order's remainder on its history row, which then never reads
  // as ended.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(1, "0", {listed_row("0001569139", OrderSide::BUY, "10", "0")}));
  rig.runner_instance.feed(strategy::MarketPhase::CLOSED);
  install(rig.runner_instance, polled(2, "0", {}));
  auto open = ended_row("0001569139", OrderSide::BUY, "10", "0");
  open.status_ = OrderStatus::PENDING;
  REQUIRE(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({open});
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
}

TEST_CASE("a runner that sends to no broker keeps no unconfirmed orders", "[confirm]") {
  runner::DryRunner<HoldTen, WithWorkingOrders> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));
  dry.feed(bar_at(1, "70000"));
  CHECK(dry.unconfirmed_orders() == 0);
}

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

namespace {

/** Routes an omitted venue to KRX, as KIS documents, so a test can tell the traits were asked. */
struct KrxWhenOmitted {
  static constexpr Venue effective_route(Venue asked) noexcept {
    return asked == Venue::UNSPECIFIED ? Venue::KRX : asked;
  }
};

/** The fake session, with traits that say how it routes -- as every shipped broker's do. */
template <typename CapsType>
class RoutedFakeSession : public FakeSession<CapsType> {
public:
  using Traits = KrxWhenOmitted;
};

} // namespace

TEST_CASE("an unrouted order is sent with the binding's default route", "[route]") {
  FakeSession<WithoutKey> session;

  // No default: the order names no venue and none is sent, which is the broker's own default.
  live::SessionExecutor<FakeSession<WithoutKey>> unrouted(session);
  CHECK(unrouted.default_route() == Venue::UNSPECIFIED);
  REQUIRE(unrouted.place(planned()) == runner::SendOutcome::ACCEPTED);
  CHECK(session.placed().back().venue_ == Venue::UNSPECIFIED);

  // A default: an order naming no venue is sent there, and one naming a venue keeps it.
  live::SessionExecutor<FakeSession<WithoutKey>> executor(session, Venue::KRX);
  CHECK(executor.default_route() == Venue::KRX);
  REQUIRE(executor.place(planned()) == runner::SendOutcome::ACCEPTED);
  CHECK(session.placed().back().venue_ == Venue::KRX);
  auto routed = planned();
  routed.venue_ = Venue::NXT;
  REQUIRE(executor.place(routed) == runner::SendOutcome::ACCEPTED);
  CHECK(session.placed().back().venue_ == Venue::NXT);
}

TEST_CASE("the route a binding reports applies its default before the broker's", "[route]") {
  // **Every binding answers.** `effective_route` is optional on a broker's traits and a hand-written
  // session need have none, so without a fallback a journal over such a binding would record the
  // strategy's blank while the default went on the wire.
  FakeSession<WithoutKey> bare;
  CHECK(live::SessionExecutor<FakeSession<WithoutKey>>(bare).effective_route(Venue::UNSPECIFIED) ==
    Venue::UNSPECIFIED);
  CHECK(live::SessionExecutor<FakeSession<WithoutKey>>(bare, Venue::NXT)
    .effective_route(Venue::UNSPECIFIED) == Venue::NXT);

  // Traits that say how the broker routes an omitted venue are asked about the *requested* one: with
  // a default, the omission never reaches them.
  RoutedFakeSession<WithoutKey> routed;
  CHECK(live::SessionExecutor<RoutedFakeSession<WithoutKey>>(routed)
    .effective_route(Venue::UNSPECIFIED) == Venue::KRX);
  CHECK(live::SessionExecutor<RoutedFakeSession<WithoutKey>>(routed, Venue::NXT)
    .effective_route(Venue::UNSPECIFIED) == Venue::NXT);
  CHECK(live::SessionExecutor<RoutedFakeSession<WithoutKey>>(routed, Venue::NXT)
    .effective_route(Venue::COMBINED) == Venue::COMBINED);
}

TEST_CASE("a runner's journal, order, reservation and withdrawal all name the default route", "[route]") {
  // The same sell-then-withdraw as the case above, bound with a default route and a journal. Each of
  // the four places the route is written down is checked, because each is read by something
  // different: the journal by recovery, the request by the broker, the reserved row by the
  // withdrawal built from it, and the report by the operator. A default applied only at the wire
  // would leave the other three naming a venue the order never went to.
  //
  // The runner resolves the route before the journal sees the order, so this case does not guard
  // the executor's own fallback for a session without traits; the case above does.
  FakeSession<SellableInHoldings> session;
  runner::MemoryJournal journal;
  strategy::RiskLimits limits;
  // Enforced so the sell's reservation persists into the next pass, which is where it is withdrawn.
  limits.enforce_sellable_quantity_ = true;

  auto live_runner = live::bind_journaled(
    session, journal, FlipTarget{}, limits, live::default_series_capacity, Venue::KRX
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<SellableInHoldings>("10", "10"));

  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed()[0].venue_ == Venue::KRX);
  REQUIRE(first.would_place_.size() == 1);
  CHECK(first.would_place_[0].venue_ == Venue::KRX);
  REQUIRE_FALSE(journal.records().empty());
  CHECK(journal.records().front().outcome_ == runner::JournalOutcome::PENDING);
  CHECK(journal.records().front().venue_ == std::optional<Venue>(Venue::KRX));

  const auto& working = live_runner.context().portfolio().working_;
  REQUIRE(working.size() == 1);
  CHECK(working[0].reserved_);
  CHECK(working[0].order_.venue_ == Venue::KRX);

  // Long from the same ten held: the reserved sell is withdrawn, at the venue it was sent to. At
  // Kiwoom a withdrawal naming no venue is refused, so a blank here left the order resting.
  live_runner.feed(bar_at(2, "70000"));
  REQUIRE(session.canceled().size() == 1);
  CHECK(session.canceled()[0].venue_ == Venue::KRX);
}

TEST_CASE("an unjournaled binding takes a default route too", "[route]") {
  // `bind` builds its own executor, so a parameter it forgot to pass on would leave the runner
  // sending unrouted orders while its caller had stated a route.
  FakeSession<WithoutKey> session;
  auto bound = live::bind(
    session, HoldTen{}, strategy::RiskLimits{}, live::default_series_capacity, Venue::KRX
  );
  CHECK(bound.executor().default_route() == Venue::KRX);
  CHECK(live::bind(session, HoldTen{}).executor().default_route() == Venue::UNSPECIFIED);
}

TEST_CASE("a binding with no default route sends and records the planned venue", "[route]") {
  // The control for the case above, and the promise to every existing caller: nothing changes.
  FakeSession<SellableInHoldings> session;
  runner::MemoryJournal journal;
  strategy::RiskLimits limits;
  limits.enforce_sellable_quantity_ = true;

  auto live_runner = live::bind_journaled(session, journal, FlipTarget{}, limits);
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<SellableInHoldings>("10", "10"));

  live_runner.feed(bar_at(1, "70000"));
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed()[0].venue_ == Venue::UNSPECIFIED);
  REQUIRE_FALSE(journal.records().empty());
  CHECK(journal.records().front().venue_ == std::optional<Venue>(Venue::UNSPECIFIED));
  live_runner.feed(bar_at(2, "70000"));
  REQUIRE(session.canceled().size() == 1);
  CHECK(session.canceled()[0].venue_ == Venue::UNSPECIFIED);
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

/** Buys on every bar and flattens on a stop, so the closing pass places an order of its own. */
struct SellsOnStop {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1"))
      )
    );
  }
};

/** Buys one share at a stated limit, so the order carries a price a band could move. */
struct LimitBuy {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = strategy::Price::checked(Decimal64::parse("80000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")), *limit
      )
    );
  }
};

/** Sells one share at a stated limit, so an exit can be priced below a floor. */
struct LimitSell {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = strategy::Price::checked(Decimal64::parse("60000"), reference->price_rule_);
    if (!limit) {
      return;
    }
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("1")), *limit
      )
    );
  }
};

/**
 * Sells one share at a price of zero, which `Price::checked` admits.
 *
 * Zero sits on a stated tick grid and `RiskGuard` has no general positivity rule, so this reaches
 * the binding -- where every shipped adapter refuses it before any request goes out. It is the
 * concrete case behind `Runner::unclamped_orders_refused`'s second site: a withheld floor would have
 * lifted this price into range.
 */
struct SellsAtZero {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = strategy::Price::checked(Decimal64::parse("0"), reference->price_rule_);
    if (!limit) {
      return;
    }
    ctx.submit(
      strategy::OrderIntent::limit(
        bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("1")), *limit
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
  // The first intent was kept, the broker having been unreachable; the second bar's statement
  // superseded it, so it is not sent as well.
  CHECK(live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("only a refusal for want of a broker is about the connection") {
  // The rule `Runner` keeps an unsent instruction by: never written, and not the client's own
  // validation, which would refuse the same instruction again.
  BrokerError error;
  error.reached_broker_ = false;
  for (const auto category : {ErrorCategory::TRANSPORT, ErrorCategory::CANCELLED,
                              ErrorCategory::AUTHENTICATION, ErrorCategory::PROTOCOL}) {
    error.category_ = category;
    CHECK(runner::broker_unreachable(error));
  }
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  CHECK_FALSE(runner::broker_unreachable(error));
  // A request that was written is never this, whatever its category: it may exist.
  error.reached_broker_ = true;
  error.category_ = ErrorCategory::TRANSPORT;
  CHECK_FALSE(runner::broker_unreachable(error));
  CHECK_FALSE(runner::broker_rate_limited(error));
  // A rate limit the broker answered is about the moment, not the instruction.
  error.category_ = ErrorCategory::RATE_LIMIT;
  CHECK(runner::broker_rate_limited(error));
  CHECK_FALSE(runner::broker_unreachable(error));
}

TEST_CASE("a target the broker could not be reached for is planned again once it answers") {
  // Lab: a daily strategy's target, released at the open into a dropped connection, used to be
  // dropped, so the trade waited a day for the next bar. It is kept -- and offered again only after a
  // holdings read newer than the failure, so a dead connection is not retried every tick.
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithoutKey>("0", nullptr);
  live_runner.context().set_portfolio(portfolio);

  session.fail_next_token(ErrorCategory::TRANSPORT);
  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  CHECK(first.rejected_[0].reason_ == strategy::RejectReason::BINDING_REFUSED);
  CHECK(session.placed().empty());
  CHECK(live_runner.targets_retained() == 1);

  // No holdings read since: not offered again.
  const auto quiet = live_runner.retry_retained_targets();
  CHECK(quiet.would_place_.empty());
  CHECK(session.placed().empty());
  CHECK(live_runner.targets_retained() == 1);

  // A holdings read arrives, but the broker is gone again by the time the target is sent: kept once
  // more, and gated on the next read, not offered again on the next pass.
  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  session.fail_next_token(ErrorCategory::TRANSPORT);
  CHECK(live_runner.retry_retained_targets().would_place_.empty());
  CHECK(live_runner.targets_retained() == 1);
  CHECK(live_runner.retry_retained_targets().would_place_.empty());
  CHECK(session.placed().empty());

  // The broker answered a holdings read: the target is planned against it and sent.
  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  const auto again = live_runner.retry_retained_targets();
  CHECK(again.would_place_.size() == 1);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed().front().quantity_ == Decimal64::parse("10"));
  CHECK(live_runner.targets_retained() == 0);
}

TEST_CASE("a target the broker turned away for its rate limit is planned again once it answers") {
  // KIS's `EGW00201` at a busy open: the broker answered and processed nothing. Dropped, a daily
  // strategy's order waited a day. Kept, and gated on a newer holdings read as an unreachable send is.
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithoutKey>("0", nullptr);
  live_runner.context().set_portfolio(portfolio);

  session.fail_with(ErrorCategory::RATE_LIMIT);
  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  CHECK(first.rejected_[0].reason_ == strategy::RejectReason::BROKER_REFUSED);
  CHECK(live_runner.targets_retained() == 1);
  session.succeed();

  CHECK(live_runner.retry_retained_targets().would_place_.empty());
  CHECK(session.placed().size() == 1);  // the refused request, counted by the fake as made

  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  CHECK(live_runner.retry_retained_targets().would_place_.size() == 1);
  CHECK(session.placed().size() == 2);
  CHECK(live_runner.targets_retained() == 0);

  SECTION("but not a broker's refusal of the instruction itself") {
    LiveRunner refused_runner(
      HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
    );
    refused_runner.context().set_instrument(samsung_reference());
    refused_runner.context().set_phase(strategy::MarketPhase::OPEN);
    refused_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
    refused_runner.context().set_portfolio(portfolio);
    session.fail_with(ErrorCategory::HTTP, 400);
    REQUIRE(refused_runner.feed(bar_at(2, "70000")).rejected_.size() == 1);
    CHECK(refused_runner.targets_retained() == 0);
  }
}

TEST_CASE("an order intent the broker turned away for its rate limit is kept") {
  // Kept like a phase refusal the broker answered, with its holding, and sent once the broker answers.
  // (A keyed one is not kept: it would reuse a key the broker has seen; `keep_intent_for_phase`.)
  using Runner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(BuysOne{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithKey>("0", nullptr);
  live_runner.context().set_portfolio(portfolio);

  session.fail_with(ErrorCategory::RATE_LIMIT);
  REQUIRE(live_runner.feed(bar_at(1, "70000")).rejected_.size() == 1);
  CHECK(live_runner.intents_kept_for_phase() == 1);
  session.succeed();

  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  CHECK(live_runner.retry_retained_targets().would_place_.size() == 1);
  CHECK(live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("a target the binding refuses for good is not kept") {
  // The client's own validation would refuse it again; keeping it would only repeat the refusal.
  FakeSession<WithoutKey> session;
  LiveRunner live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithoutKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithoutKey>("0", nullptr);
  live_runner.context().set_portfolio(portfolio);

  session.refuse_next_locally();
  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  CHECK(live_runner.targets_retained() == 0);

  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  CHECK(live_runner.retry_retained_targets().would_place_.empty());
  CHECK(session.placed().empty());
}

TEST_CASE("an order intent the broker could not be reached for is kept and sent once it answers") {
  // The protective-stop case: a kept intent released at the open into an outage was dropped, and the
  // position went unprotected for the session. It is kept with its holding, as a phase refusal is.
  using Runner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(BuysOne{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithKey>("0", nullptr);
  live_runner.context().set_portfolio(portfolio);

  session.fail_next_token();
  const auto first = live_runner.feed(bar_at(1, "70000"));
  REQUIRE(first.rejected_.size() == 1);
  CHECK(session.placed().empty());
  CHECK(live_runner.intents_kept_for_phase() == 1);

  CHECK(live_runner.retry_retained_targets().would_place_.empty());
  CHECK(live_runner.intents_kept_for_phase() == 1);

  portfolio.as_of_ += std::chrono::minutes(1);
  live_runner.context().set_portfolio(portfolio);
  const auto again = live_runner.retry_retained_targets();
  CHECK(again.would_place_.size() == 1);
  CHECK(session.placed().size() == 1);
  CHECK(live_runner.intents_kept_for_phase() == 0);
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

TEST_CASE("an order the broker refused after a band was withheld is counted, and only then") {
  // What withholding costs, measured where the outcome is known. The band this runner is holding
  // would have moved the price; the policy declined it, the strategy's own price went out, and the
  // broker refused it. That is the whole of the argument for withholding being answerable rather
  // than merely arguable -- a run whose count tracks its rejections is a run whose band policy is
  // costing it orders.
  using Runner = runner::Runner<
    LimitBuy, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(LimitBuy{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));
  PriceLimits band;
  band.upper_ = Decimal64::parse("71000");
  live_runner.set_price_limits(samsung(), band);

  SECTION("the broker refuses it") {
    // Reached the broker and was refused by it, which is `SendOutcome::REJECTED`.
    session.fail_with(ErrorCategory::INVALID_ARGUMENT);
    live_runner.feed(bar_at(1, "70000"));
    CHECK(live_runner.price_bands_withheld() == 1);
    CHECK(live_runner.unclamped_orders_rejected() == 1);
  }

  SECTION("the broker accepts it") {
    // The band was still withheld -- that is the policy -- and nothing was lost by it.
    live_runner.feed(bar_at(1, "70000"));
    CHECK(live_runner.price_bands_withheld() == 1);
    CHECK(live_runner.unclamped_orders_rejected() == 0);
  }

  SECTION("the binding refuses it") {
    // No broker was asked, so the counter about brokers stays still -- and the one about everything
    // that never reached a broker moves, because a price the binding will not send is a price the
    // withheld band might have moved into range.
    session.refuse_next_locally();
    live_runner.feed(bar_at(1, "70000"));
    CHECK(live_runner.price_bands_withheld() == 1);
    CHECK(live_runner.unclamped_orders_rejected() == 0);
    CHECK(live_runner.unclamped_orders_refused() == 1);
    CHECK(session.placed().empty());
  }
}

TEST_CASE("an exit priced below a withheld floor is refused rather than lifted onto it") {
  // **The case that costs something, named rather than argued away.** A stale lower bound can only
  // raise a sell's price, so applying one makes an exit less willing -- but where the strategy's own
  // price is below *today's real* floor, the exchange refuses the order outright, and the clamped
  // price would have been accepted and could have traded. Withholding can therefore lose a fill, and
  // this is what it looks like: a reducing sell, refused, counted.
  using Runner = runner::Runner<
    LimitSell, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(LimitSell{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("10", nullptr));
  PriceLimits band;
  band.lower_ = Decimal64::parse("65000");
  live_runner.set_price_limits(samsung(), band);
  // Stands for the exchange refusing a price outside today's band.
  session.fail_with(ErrorCategory::INVALID_ARGUMENT);

  live_runner.feed(bar_at(1, "70000"));

  REQUIRE(session.placed().size() == 1);
  // The strategy's own price went out, not the floor the runner was holding.
  CHECK(session.placed()[0].price_ == std::optional<Decimal64>(Decimal64::parse("60000")));
  CHECK(live_runner.price_bands_withheld() == 1);
  CHECK(live_runner.unclamped_orders_rejected() == 1);
  CHECK(live_runner.unclamped_orders_refused() == 0);
}

TEST_CASE("a price no adapter will send is counted as a local refusal, not as silence") {
  // **The third outcome, and the one that was invisible.** A sell at zero passes `Price::checked`,
  // because zero is on a stated grid, and passes a guard that has no positivity rule -- then the
  // binding refuses it and no request goes out. Counting only what a broker refused left this
  // looking exactly like a run where withholding cost nothing, and applying the withheld floor
  // would have produced a valid order.
  using Runner = runner::Runner<
    SellsAtZero, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(SellsAtZero{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("10", nullptr));
  PriceLimits band;
  band.lower_ = Decimal64::parse("65000");
  live_runner.set_price_limits(samsung(), band);
  // Stands for what every shipped adapter does with a nonpositive limit price: refuse before any
  // I/O, with `reached_broker_` false.
  session.refuse_next_locally();

  const auto report = live_runner.feed(bar_at(1, "70000"));

  REQUIRE(report.rejected_.size() == 1);
  CHECK(report.rejected_[0].reason_ == strategy::RejectReason::BINDING_REFUSED);
  CHECK(session.placed().empty());
  CHECK(live_runner.price_bands_withheld() == 1);
  CHECK(live_runner.unclamped_orders_refused() == 1);
  // The broker was never asked, so the counter about brokers must not move.
  CHECK(live_runner.unclamped_orders_rejected() == 0);
}

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
using BandSession = FakeSession<WithPriceLimits>;
using NoBandSession = FakeSession<WithoutPriceLimits>;
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

namespace {

/** Fifty closed daily bars, the shape of a cold start's first read. */
BarSeries history_bars(int count) {
  BarSeries series;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::CLOSE;
  for (int at = 0; at < count; ++at) {
    Bar bar;
    bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (100 + at));
    bar.close_ = Decimal64::parse(std::to_string(70000 + 100 * at));
    bar.high_ = bar.close_;
    bar.low_ = bar.close_;
    series.bars_.push_back(bar);
  }
  return series;
}

/** A holdings answer stamped when it is read, so only the listing can age in these cases. */
template <typename Session, typename Clock>
class FreshHoldingsSource {
public:
  static constexpr bool provides_working_orders = true;

  FreshHoldingsSource(Session& session, std::vector<InstrumentId> universe, const Clock* clock)
  : inner_(session, std::move(universe)), session_(&session), clock_(clock) {}

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    if (kind == runner::PollKind::HOLDINGS) {
      HoldingsSnapshot snapshot;
      snapshot.received_at_ = clock_->now();
      if (held_) {
        HoldingPosition position;
        position.instrument_ = samsung();
        position.quantity_ = *held_;
        snapshot.positions_.push_back(position);
      }
      session_->set_holdings(snapshot);
    }
    if (kind == runner::PollKind::WORKING_ORDERS && listing_fails_) {
      return runner::PollOutcome::FAILED;
    }
    const auto outcome = inner_.poll(kind, runner_instance);
    if (kind == runner::PollKind::BARS && next_bars_) {
      session_->set_bars(*next_bars_);
      next_bars_.reset();
    }
    return outcome;
  }

  /** Makes the working-order poll fail, without reaching the session, until told otherwise. */
  bool listing_fails_{false};
  /** The Samsung position every holdings read reports, or none. */
  std::optional<Decimal64> held_;
  /** What the session answers to bar queries after the next bar poll, once. */
  std::optional<BarSeries> next_bars_;

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    return inner_.read_order_list(runner_instance);
  }

private:
  live::SessionSource<Session> inner_;
  Session* session_;
  const Clock* clock_;
};

} // namespace

TEST_CASE("a cold start's history read places one order, not one per bar", "[own-orders]") {
  // The shipped consumer path end to end: `SessionSource` feeds the first bar read, fifty bars, after
  // the startup listing and holdings reads -- forty-nine as history (`Runner::feed_warmup`, which sends
  // nothing) and the newest live. Default limits, a target strategy. The order the newest bar sent
  // stays working until a listing replaces it; that a later bar respects it is the next case.
  using LoopRunner = runner::Runner<
    HoldTen, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  using Source = FreshHoldingsSource<FakeSession<WithWorkingOrders>, runner::ManualClock>;
  FakeSession<WithWorkingOrders> session;
  session.set_bars(history_bars(50));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    HoldTen{}, any_phase, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());

  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::ManualClock* clock = nullptr;
  Source source(session, {samsung()}, clock);
  runner::PollingLoop<LoopRunner, Source, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{start});
  source = Source(session, {samsung()}, &loop.clock());
  loop.set_maximum_portfolio_age(std::chrono::minutes(1));
  runner::StopSource stop;

  loop.run(stop, 1);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed()[0].quantity_ == Decimal64::parse("10"));
  REQUIRE(live_runner.context().portfolio().working_.size() == 1);
  CHECK(live_runner.context().portfolio().working_[0].reserved_);

  // The next working-order poll lists nothing -- a market order that filled at once is in no listing
  // -- and holdings do not show the fill yet. The order stays counted, so nothing is sent again (lab
  // F15: a listing that retired it let a target buy twice).
  loop.run(stop, 1);
  REQUIRE(live_runner.context().portfolio().working_.size() == 1);
  CHECK(live_runner.context().portfolio().working_[0].reserved_);
  CHECK(live_runner.unconfirmed_orders() == 1);
  CHECK(session.placed().size() == 1);

  // Holdings show the ten: the order is confirmed, its row is gone, and still nothing more is sent.
  source.held_ = Decimal64::parse("10");
  loop.run(stop, 1);
  CHECK(live_runner.context().portfolio().working_.empty());
  CHECK(live_runner.unconfirmed_orders() == 0);
  CHECK(session.placed().size() == 1);
}

TEST_CASE("a bar fed before the next listing sees the order the last one sent", "[own-orders]") {
  // What the reservation is for on the shipped path: the cold start sends one order, and the next bar
  // arrives -- a later read bringing one more closed bar -- while the working-order listing is not
  // due yet. Holdings are polled in between, as a live loop does, and must keep the reserved row: the
  // target restated on the new bar is already met by the order working.
  using LoopRunner = runner::Runner<
    HoldTen, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  using Source = FreshHoldingsSource<FakeSession<WithWorkingOrders>, runner::ManualClock>;
  FakeSession<WithWorkingOrders> session;
  session.set_bars(history_bars(50));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    HoldTen{}, any_phase, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());

  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.holdings_ = std::chrono::seconds(1);
  // Bars come faster than the listing, so the second bar is fed with only the reservation to go on.
  schedule.working_orders_ = std::chrono::seconds(30);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  Source source(session, {samsung()}, nullptr);
  runner::PollingLoop<LoopRunner, Source, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{start});
  source = Source(session, {samsung()}, &loop.clock());
  // After the first bar poll, the session's next read has one more closed bar.
  source.next_bars_ = history_bars(51);
  loop.set_maximum_portfolio_age(std::chrono::minutes(1));
  runner::StopSource stop;

  // One run, because each run polls the listing first: two iterations a second apart.
  const auto report = loop.run(stop, 2);
  // Not vacuous: two bar polls and two holdings polls, one listing; forty-nine bars fed as history,
  // and the fifty-first fed at all (its close is the last price), so the fiftieth and the fifty-first
  // were each fed live, on their own pass.
  REQUIRE(report.attempted(runner::PollKind::BARS) == 2);
  REQUIRE(report.attempted(runner::PollKind::HOLDINGS) == 2);
  REQUIRE(report.attempted(runner::PollKind::WORKING_ORDERS) == 1);
  REQUIRE(live_runner.warmup_instructions_discarded() == 49);
  REQUIRE(live_runner.context().last_price(samsung()) == Decimal64::parse("75000"));
  CHECK(session.placed().size() == 1);
  REQUIRE(live_runner.context().portfolio().working_.size() == 1);
  CHECK(live_runner.context().portfolio().working_[0].reserved_);
}

TEST_CASE("a run that polls no working-order listing holds, whatever its age policy",
          "[own-orders]") {
  // The reservation lives until a working-order listing replaces it, so a run that never polls one
  // would keep an expired or refused order "working" and stop chasing its target. Opting out of a
  // holdings age is not opting out of a listing, so an unbounded age does not lift this.
  using LoopRunner = runner::Runner<
    HoldTen, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  using Source = FreshHoldingsSource<FakeSession<WithWorkingOrders>, runner::ManualClock>;
  static_assert(LoopRunner::keeps_sent_orders_until_listed, "a live binding keeps its orders");
  FakeSession<WithWorkingOrders> session;
  session.set_bars(history_bars(3));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    HoldTen{}, any_phase, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());

  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  Source source(session, {samsung()}, nullptr);
  runner::PollingLoop<LoopRunner, Source, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{start});
  source = Source(session, {samsung()}, &loop.clock());
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;

  const auto report = loop.run(stop, 3);
  CHECK(session.placed().empty());
  REQUIRE(report.final_gate_.has_value());
  CHECK(*report.final_gate_ == strategy::HaltReason::PORTFOLIO_STALE);
  std::size_t said = 0;
  for (const auto& note : report.notes_) {
    if (note.find("no working-order cadence") != std::string::npos) {
      ++said;
    }
  }
  CHECK(said == 1);
}

TEST_CASE("a listing older than the portfolio age policy holds the loop until one is read",
          "[own-orders]") {
  // Polled, but failing: the listing ages past the policy and the loop holds, naming it once; the
  // next listing that arrives lifts the hold.
  using LoopRunner = runner::Runner<
    HoldTen, WithWorkingOrders, FakeBroker, live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  using Source = FreshHoldingsSource<FakeSession<WithWorkingOrders>, runner::ManualClock>;
  FakeSession<WithWorkingOrders> session;
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    HoldTen{}, any_phase, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());

  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 200);
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  Source source(session, {samsung()}, nullptr);
  runner::PollingLoop<LoopRunner, Source, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{start});
  source = Source(session, {samsung()}, &loop.clock());
  loop.set_maximum_portfolio_age(std::chrono::seconds(3));
  runner::StopSource stop;

  const auto early = loop.run(stop, 2);
  CHECK_FALSE(early.final_gate_.has_value());

  // Four failing listings: fewer than the loop's abandonment limit, so the run is held, not given up.
  source.listing_fails_ = true;
  const auto late = loop.run(stop, 4);
  CHECK_FALSE(late.abandoned_);
  REQUIRE(late.final_gate_.has_value());
  std::size_t said = 0;
  for (const auto& note : late.notes_) {
    if (note.find("working-order listing older than the portfolio age policy") != std::string::npos) {
      ++said;
    }
  }
  CHECK(said == 1);

  source.listing_fails_ = false;
  const auto healed = loop.run(stop, 2);
  CHECK_FALSE(healed.final_gate_.has_value());
}

namespace {

/** Records every close it is shown, so a test can tell a forming bar from a closed one. */
struct RecordsCloses {
  std::vector<std::string> closes_;

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent& bar) {
    closes_.push_back(bar.bar_.close_.str());
  }
};

/** Two daily bars stamped at their open, the second the session still trading at `second_close`. */
BarSeries daily_with_forming(const char* second_close) {
  BarSeries series;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::OPEN;
  Bar first;
  first.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 300);
  first.open_ = Decimal64::parse("70000");
  first.close_ = Decimal64::parse("70500");
  first.high_ = first.close_;
  first.low_ = first.open_.value();
  Bar second = first;
  second.timestamp_ = first.timestamp_ + std::chrono::hours(24);
  second.close_ = Decimal64::parse(second_close);
  second.high_ = second.close_;
  series.bars_ = {first, second};
  return series;
}

} // namespace

TEST_CASE("a bar whose period has not ended is not fed, and its close is fed once it has",
          "[forming]") {
  // Every shipped adapter's daily query runs up to today, and the broker answers today's row while
  // the session trades. Fed, the strategy decided on the price so far as a close, and the real close
  // never arrived because the bar's timestamp had already been seen.
  using LoopRunner = runner::Runner<
    RecordsCloses, WithWorkingOrders, FakeBroker,
    live::SessionExecutor<FakeSession<WithWorkingOrders>>
  >;
  FakeSession<WithWorkingOrders> session;
  session.set_bars(daily_with_forming("71000"));
  LoopRunner live_runner(
    RecordsCloses{}, {}, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());

  live::SessionSource<FakeSession<WithWorkingOrders>> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::minutes(1);
  schedule.tick_ = std::chrono::minutes(1);
  schedule.calendar_.reset();
  // Three hours into the second bar's day: the first bar has ended, the second has not.
  const auto midday = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 301 + 3);
  runner::PollingLoop<LoopRunner, decltype(source), runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{midday});
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
  runner::StopSource stop;

  loop.run(stop, 3);
  REQUIRE(live_runner.strategy().closes_ == std::vector<std::string>{"70500"});
  CHECK(source.bars_not_yet_closed() >= 1);
  // The decision instant is the first bar's period end, never ahead of the clock.
  CHECK_FALSE(midday < live_runner.context().now());

  // The session closes at 72,000 and the day ends; the next poll feeds the bar once, closed.
  session.set_bars(daily_with_forming("72000"));
  loop.clock().advance(std::chrono::hours(22));
  loop.run(stop, 3);
  REQUIRE(live_runner.strategy().closes_ == std::vector<std::string>{"70500", "72000"});
}

namespace {

/** 00:00 KST on day `day` after the epoch, which is how every shipped adapter stamps a daily bar. */
std::chrono::system_clock::time_point kst_midnight(int day) {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * day) - std::chrono::hours(9);
}

/** Two daily bars, stamped at 00:00 KST on the two days before `day`. */
BarSeries kst_daily_before(int day) {
  BarSeries series;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::OPEN;
  for (int back = 2; back >= 1; --back) {
    Bar bar;
    bar.timestamp_ = kst_midnight(day - back);
    bar.open_ = Decimal64::parse("70000");
    bar.close_ = Decimal64::parse("70000");
    bar.high_ = bar.close_;
    bar.low_ = bar.close_;
    series.bars_.push_back(bar);
  }
  return series;
}

/** A calendar on which `day` and the days either side of it trade. */
TradingCalendar trading_around(int day) {
  TradingCalendar calendar;
  calendar.country_ = Country::KR;
  calendar.received_at_ = kst_midnight(day);
  for (int at = day - 3; at <= day + 1; ++at) {
    calendar.days_.push_back(
      TradingDay{strategy::SessionWindow::krx().local_date(kst_midnight(at) + std::chrono::hours(12)), true}
    );
  }
  return calendar;
}

template <typename Strategy>
using PhaseRunner = runner::Runner<
  Strategy, WithCalendar, FakeBroker, live::SessionExecutor<FakeSession<WithCalendar>>
>;
using PhaseSource = FreshHoldingsSource<FakeSession<WithCalendar>, runner::ManualClock>;

/** Polls every half hour, the listing too, from 00:30 KST on `day`, with a stated portfolio age. */
runner::PollSchedule half_hourly() {
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::minutes(30);
  schedule.holdings_ = std::chrono::minutes(30);
  schedule.working_orders_ = std::chrono::minutes(30);
  schedule.calendar_ = std::chrono::hours(6);
  schedule.tick_ = std::chrono::minutes(30);
  return schedule;
}

constexpr int trading_day = 400;

} // namespace

TEST_CASE("a daily target decided before the open is placed once, at the open", "[phase-retain]") {
  // Every shipped adapter stamps a daily bar at 00:00 KST, so its period ends at the next midnight
  // and `SessionSource` feeds it then, while the market is shut. The default phase limit refused the
  // entry and the target was dropped: a daily strategy under default limits never opened a position.
  FakeSession<WithCalendar> session;
  session.set_bars(kst_daily_before(trading_day));
  session.set_calendar(trading_around(trading_day));
  PhaseRunner<HoldTen> live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithCalendar>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  PhaseSource source(session, {samsung()}, nullptr);
  runner::PollingLoop<PhaseRunner<HoldTen>, PhaseSource, runner::ManualClock> loop(
    live_runner, source, half_hourly(),
    runner::ManualClock{kst_midnight(trading_day) + std::chrono::minutes(30)});
  source = PhaseSource(session, {samsung()}, &loop.clock());
  loop.set_maximum_portfolio_age(std::chrono::hours(1));
  runner::StopSource stop;

  // 00:30 to 08:30 KST: the bar is fed, the entry refused for the phase, and the target kept.
  loop.run(stop, 17);
  CHECK(session.placed().empty());
  CHECK(live_runner.targets_retained_for_phase() == 1);

  // Through the open: placed once, in the first open iteration, and not again.
  loop.run(stop, 6);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed()[0].side_ == OrderSide::BUY);
  CHECK(live_runner.targets_retained() == 0);
}

TEST_CASE("an exit a broker refuses out of session is placed once the session opens",
          "[phase-retain]") {
  // The phase limit admits an order that reduces a position in any phase, and a broker that is not
  // taking orders at midnight refuses it. Refused, the target used to wait for its next bar -- the
  // next midnight, refused again -- so a daily strategy could enter and never leave.
  FakeSession<WithCalendar> session;
  session.set_bars(kst_daily_before(trading_day));
  session.set_calendar(trading_around(trading_day));
  BrokerError closed;
  closed.category_ = ErrorCategory::INVALID_ARGUMENT;
  closed.broker_code_ = "40580000";
  closed.message_ = "not an order-acceptance time";
  session.fail_place_with(closed);
  PhaseRunner<SellsAll> live_runner(
    SellsAll{}, {}, 512, live::SessionExecutor<FakeSession<WithCalendar>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  PhaseSource source(session, {samsung()}, nullptr);
  runner::PollingLoop<PhaseRunner<SellsAll>, PhaseSource, runner::ManualClock> loop(
    live_runner, source, half_hourly(),
    runner::ManualClock{kst_midnight(trading_day) + std::chrono::minutes(30)});
  source = PhaseSource(session, {samsung()}, &loop.clock());
  source.held_ = Decimal64::parse("10");
  loop.set_maximum_portfolio_age(std::chrono::hours(1));
  runner::StopSource stop;

  // The first poll feeds both history bars; only the newest may send, and its exit is sent and
  // refused (the fake records every attempt). Then nothing more all night: a target kept for the
  // phase is not retried until the phase admits it, so a closed market is not asked again every half
  // hour.
  const auto night = loop.run(stop, 17);
  CHECK(night.orders_broker_refused_ == 1);
  CHECK(session.placed().size() == 1);
  CHECK(live_runner.targets_retained_for_phase() == 1);

  // The broker takes orders again, and the exit goes out once at the open.
  session.heal_place();
  loop.run(stop, 6);
  REQUIRE(session.placed().size() == 2);
  CHECK(session.placed()[1].side_ == OrderSide::SELL);
  CHECK(session.placed()[1].quantity_ == Decimal64::parse("10"));
  CHECK(live_runner.targets_retained() == 0);
}

TEST_CASE("an operator's halt discards a target kept for the phase", "[phase-retain]") {
  // Decided at midnight, waiting for the open: an operator who halts at 08:50 and resumes at 10:00
  // did not ask for a decision made before the halt to go out at 10:00.
  FakeSession<WithCalendar> session;
  session.set_bars(kst_daily_before(trading_day));
  session.set_calendar(trading_around(trading_day));
  PhaseRunner<HoldTen> live_runner(
    HoldTen{}, {}, 512, live::SessionExecutor<FakeSession<WithCalendar>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  PhaseSource source(session, {samsung()}, nullptr);
  runner::PollingLoop<PhaseRunner<HoldTen>, PhaseSource, runner::ManualClock> loop(
    live_runner, source, half_hourly(),
    runner::ManualClock{kst_midnight(trading_day) + std::chrono::minutes(30)});
  source = PhaseSource(session, {samsung()}, &loop.clock());
  loop.set_maximum_portfolio_age(std::chrono::hours(1));
  runner::StopSource stop;

  loop.run(stop, 17);
  REQUIRE(live_runner.targets_retained_for_phase() == 1);
  live_runner.guard().halt(strategy::HaltReason::OPERATOR);
  loop.run(stop, 2);
  CHECK(live_runner.targets_retained() == 0);
  live_runner.guard().resume(strategy::HaltReason::OPERATOR);
  loop.run(stop, 4);
  CHECK(session.placed().empty());
}

TEST_CASE("a replay does not keep a target the phase refused", "[phase-retain]") {
  // A replay never calls `retry_retained_targets`, so a kept target would only pile up.
  runner::DryRunner<HoldTen> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::PRE_OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));
  strategy::Portfolio<runner::DryTag> flat;
  flat.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  dry.context().set_portfolio(flat);
  const auto report = dry.feed(bar_at(0, "70000"));
  REQUIRE(report.refused_.size() == 1);
  CHECK(report.refused_[0].reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED);
  CHECK(dry.targets_retained() == 0);
}

namespace {

/** Buys ten on the tenth bar it sees and never says anything else: an event, not a level. */
struct EntersOnTenth {
  std::size_t bars_{0};
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (++bars_ != 10) {
      return;
    }
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
  }
};

/** Records the order in which bars of every instrument were shown to it. */
struct RecordsSequence {
  std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> seen_;
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent& bar) {
    seen_.push_back({bar.instrument_.symbol_, bar.bar_.timestamp_});
  }
};

/** A plain loop over a real `SessionSource`, placing in any phase and accepting any account age. */
template <typename Strategy>
struct WarmupRig {
  using LoopRunner =
    runner::Runner<Strategy, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>>;
  FakeSession<WithKey> session;
  LoopRunner live_runner;
  live::SessionSource<FakeSession<WithKey>> source;
  runner::PollingLoop<LoopRunner, live::SessionSource<FakeSession<WithKey>>, runner::ManualClock> loop;
  runner::StopSource stop;

  static strategy::RiskLimits any_phase() {
    strategy::RiskLimits limits;
    limits.permitted_phases_ = std::nullopt;
    return limits;
  }
  static runner::PollSchedule every_second() {
    runner::PollSchedule schedule;
    schedule.bars_ = std::chrono::seconds(1);
    schedule.tick_ = std::chrono::seconds(1);
    schedule.calendar_.reset();
    return schedule;
  }

  explicit WarmupRig(std::vector<InstrumentId> universe)
  : live_runner(Strategy{}, any_phase(), 512, live::SessionExecutor<FakeSession<WithKey>>(session)),
    source(session, universe),
    loop(live_runner, source, every_second(),
         runner::ManualClock{std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 900)}) {
    for (const auto& id : universe) {
      auto reference = samsung_reference();
      reference.id_ = id;
      live_runner.context().set_instrument(reference);
      live_runner.context().set_last_price(id, Decimal64::parse("70000"));
    }
    live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));
    loop.allow_unbounded_portfolio_age();
    loop.allow_absent_portfolio();
  }
};

} // namespace

TEST_CASE("a cold start does not act on an event in its history", "[warmup]") {
  // The history read is fifty bars; the strategy's entry fires on the tenth. A restarted process used
  // to send it, at today's price, for a signal forty bars old.
  WarmupRig<EntersOnTenth> rig({samsung()});
  rig.session.set_bars(history_bars(50));
  const auto report = rig.loop.run(rig.stop, 1);
  CHECK(rig.session.placed().empty());
  CHECK(rig.live_runner.strategy().bars_ == 50);
  CHECK(rig.live_runner.warmup_instructions_discarded() == 1);
  CHECK(rig.source.bars_fed_as_warmup() == 49);
  std::size_t said = 0;
  for (const auto& note : report.notes_) {
    said += note.find("decided on bars fed as history were not sent") != std::string::npos;
  }
  CHECK(said == 1);
}

TEST_CASE("only the newest of a backlog of bars sends", "[warmup]") {
  WarmupRig<BuysOne> rig({samsung()});
  rig.session.set_bars(history_bars(3));
  rig.loop.run(rig.stop, 1);
  REQUIRE(rig.session.placed().size() == 1);
  // An outage: the next read brings two new bars. Both are shown; only the newest's order goes out.
  rig.session.set_bars(history_bars(5));
  rig.loop.run(rig.stop, 1);
  CHECK(rig.session.placed().size() == 2);
  CHECK(rig.source.bars_fed_as_warmup() == 3);
}

TEST_CASE("the newest closed bar sends when the session's own bar is still forming", "[warmup]") {
  // The read ends in a bar still trading, which is held; the closed bar before it is the newest the
  // source can feed, and it sends.
  WarmupRig<BuysOne> rig({samsung()});
  rig.session.set_bars(daily_with_forming("71000"));
  rig.loop.clock() = runner::ManualClock{
    std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 301 + 3)};
  rig.loop.run(rig.stop, 1);
  REQUIRE(rig.session.placed().size() == 1);
  CHECK(rig.source.bars_fed_as_warmup() == 0);
  CHECK(rig.source.bars_not_yet_closed() == 1);
}

TEST_CASE("a read of several instruments is fed in time order, as a replay feeds it", "[warmup]") {
  WarmupRig<RecordsSequence> rig({samsung(), hynix()});
  rig.session.set_bars(history_bars(4));
  rig.loop.run(rig.stop, 1);
  const auto& seen = rig.live_runner.strategy().seen_;
  REQUIRE(seen.size() == 8);
  for (std::size_t at = 1; at < seen.size(); ++at) {
    CHECK_FALSE(seen[at].second < seen[at - 1].second);
  }
  // Ties in universe order: Samsung, then Hynix, for every session.
  CHECK(seen[0].first == samsung().symbol_);
  CHECK(seen[1].first == hynix().symbol_);
}

TEST_CASE("an instrument whose read ends early does not send before the others' history", "[warmup]") {
  // A suspended name's last bar is days old, so it sorts ahead of the other names' older bars. Sent
  // there, a cross-sectional strategy would act on a stale cross-section at today's price; it is fed
  // as history, and the one live bar is the newest bar after all the history.
  WarmupRig<BuysOne> rig({samsung(), hynix()});
  rig.session.set_bars_for(samsung().symbol_, history_bars(6));
  rig.session.set_bars_for(hynix().symbol_, history_bars(3));
  rig.loop.run(rig.stop, 1);
  REQUIRE(rig.session.placed().size() == 1);
  CHECK(rig.session.placed()[0].instrument_ == samsung());
  CHECK(rig.source.bars_fed_as_warmup() == 8);
}

TEST_CASE("a name one bar behind waits, whatever order the universe lists it in", "[warmup]") {
  // Hynix suspended today: its newest bar ties with Samsung's history bar of the same session, and
  // it is that stale session whichever name is listed first.
  for (const bool hynix_first : {false, true}) {
    WarmupRig<BuysOne> rig(hynix_first ? std::vector<InstrumentId>{hynix(), samsung()}
                                       : std::vector<InstrumentId>{samsung(), hynix()});
    rig.session.set_bars_for(samsung().symbol_, history_bars(6));
    rig.session.set_bars_for(hynix().symbol_, history_bars(5));
    rig.loop.run(rig.stop, 1);
    REQUIRE(rig.session.placed().size() == 1);
    CHECK(rig.session.placed()[0].instrument_ == samsung());

    // The next session: Samsung one new bar, Hynix resumed with two (its missed session and today).
    // Both send, Hynix's missed session is history, and nothing already fed is fed again.
    rig.session.set_bars_for(samsung().symbol_, history_bars(7));
    rig.session.set_bars_for(hynix().symbol_, history_bars(7));
    rig.loop.run(rig.stop, 1);
    CHECK(rig.session.placed().size() == 3);
    CHECK(rig.source.bars_fed_as_warmup() == 11);
  }
}

TEST_CASE("a bar fed as history changes the strategy and no counter or reservation", "[warmup]") {
  runner::DryRunner<BuysOne> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  for (int hour = 0; hour < 5; ++hour) {
    dry.feed_warmup(bar_at(hour, "70000"));
  }
  CHECK(dry.warmup_instructions_discarded() == 5);
  CHECK(dry.orders_guard_refused() == 0);
  CHECK(dry.placements_at_broker() == 0);
  CHECK(dry.targets_retained() == 0);
  CHECK(dry.context().portfolio().working_.empty());
#if defined(ATL_HAS_EIGEN)
  const auto* series = dry.context().bars(samsung(), BarInterval::ONE_DAY);
  REQUIRE(series != nullptr);
  CHECK(series->size() == 5);
#endif
  // And the next ordinary bar sends as usual.
  CHECK(dry.feed(bar_at(5, "70000")).would_place_.size() == 1);
}

namespace {

/** Protects whatever it holds with a sell stop-limit, stated on every bar. */
struct StopsWhatItHolds {
  std::optional<std::string> key_;
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    const auto held = ctx.portfolio().net_quantity_of(bar.instrument_);
    if (!held || held->coefficient() <= 0) {
      return;
    }
    const auto trigger = strategy::Price::checked(Decimal64::parse("65000"), PriceRule::krx_equity());
    const auto limit = strategy::Price::checked(Decimal64::parse("64900"), PriceRule::krx_equity());
    auto intent = strategy::OrderIntent::stop_limit(
      bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(*held), *trigger, *limit);
    if (key_) {
      intent = intent.with_client_order_id(*key_);
    }
    ctx.submit(intent);
  }
};

/** A calendar, a listing, and idempotency keys a broker honours. */
struct WithCalendarAndKeys : WithCalendar {
  static constexpr bool has_client_order_id = true;
};

/** A phase rig: a calendar, the listing, fresh holdings, from 00:30 KST on `trading_day`. */
template <typename Strategy, typename Caps = WithCalendar>
struct KeptRig {
  using Runner = runner::Runner<Strategy, Caps, FakeBroker, live::SessionExecutor<FakeSession<Caps>>>;
  using Source = FreshHoldingsSource<FakeSession<Caps>, runner::ManualClock>;
  FakeSession<Caps> session;
  Runner live_runner;
  Source source;
  runner::PollingLoop<Runner, Source, runner::ManualClock> loop;
  runner::StopSource stop;

  explicit KeptRig(Strategy strategy = {})
  : live_runner(std::move(strategy), {}, 512, live::SessionExecutor<FakeSession<Caps>>(session)),
    source(session, {samsung()}, nullptr),
    loop(live_runner, source, half_hourly(),
         runner::ManualClock{kst_midnight(trading_day) + std::chrono::minutes(30)}) {
    session.set_bars(kst_daily_before(trading_day));
    session.set_calendar(trading_around(trading_day));
    live_runner.context().set_instrument(samsung_reference());
    source = Source(session, {samsung()}, &loop.clock());
    loop.set_maximum_portfolio_age(std::chrono::hours(1));
  }

  void refuse_out_of_hours() {
    BrokerError closed;
    closed.category_ = ErrorCategory::INVALID_ARGUMENT;
    closed.broker_code_ = "40580000";
    closed.message_ = "not an order-acceptance time";
    session.fail_place_with(closed);
  }
};

} // namespace

TEST_CASE("a protective stop refused out of hours rests at the open, once", "[kept-intents]") {
  // A daily strategy decides at midnight; its stop reduces the position, so the phase limit lets it
  // out, and a broker not taking orders refuses it. Kept, it goes out at the open instead of being
  // restated -- and refused -- every midnight while the position stays unprotected.
  KeptRig<StopsWhatItHolds> rig;
  rig.source.held_ = Decimal64::parse("10");
  rig.refuse_out_of_hours();
  rig.loop.run(rig.stop, 17);  // 00:30 to 08:30
  CHECK(rig.session.placed().size() == 1);  // the midnight attempt, refused
  CHECK(rig.live_runner.intents_kept_for_phase() == 1);

  rig.session.heal_place();
  rig.loop.run(rig.stop, 6);
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.session.placed()[1].type_ == OrderType::STOP_LIMIT);
  CHECK(rig.session.placed()[1].quantity_ == Decimal64::parse("10"));
  CHECK(rig.live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("an opening order decided at midnight is placed at the open, once", "[kept-intents]") {
  KeptRig<BuysOne> rig;
  rig.loop.run(rig.stop, 17);
  CHECK(rig.session.placed().empty());  // refused for the phase, never sent
  CHECK(rig.live_runner.intents_kept_for_phase() == 1);
  rig.loop.run(rig.stop, 6);
  REQUIRE(rig.session.placed().size() == 1);
  CHECK(rig.session.placed()[0].side_ == OrderSide::BUY);
}

TEST_CASE("a kept intent is dropped when the holding changed before the open", "[kept-intents]") {
  // Another client traded the account overnight: the quantity decided at midnight is no longer one
  // anybody decided for this book.
  KeptRig<BuysOne> rig;
  rig.loop.run(rig.stop, 17);
  REQUIRE(rig.live_runner.intents_kept_for_phase() == 1);
  rig.source.held_ = Decimal64::parse("5");
  rig.loop.run(rig.stop, 6);
  CHECK(rig.session.placed().empty());
  CHECK(rig.live_runner.kept_intents_dropped() == 1);
}

TEST_CASE("a kept stop is dropped when a stop rests at the open", "[kept-intents]") {
  KeptRig<StopsWhatItHolds> rig;
  rig.source.held_ = Decimal64::parse("10");
  rig.refuse_out_of_hours();
  rig.loop.run(rig.stop, 17);
  REQUIRE(rig.live_runner.intents_kept_for_phase() == 1);
  rig.session.heal_place();
  // The operator placed a stop from another terminal before the open.
  WorkingOrders<FakeBroker> listing;
  WorkingOrder<FakeBroker> stop;
  stop.order_.instrument_ = samsung();
  stop.order_.side_ = OrderSide::SELL;
  stop.order_.status_ = OrderStatus::PENDING;
  stop.order_.type_ = OrderType::STOP_LIMIT;
  stop.order_.trigger_price_ = Decimal64::parse("64000");
  stop.order_.price_ = Decimal64::parse("63900");
  stop.order_.quantity_ = Decimal64::parse("10");
  stop.order_.execution_.filled_quantity_ = Decimal64::parse("0");
  stop.amendable_quantity_ = Decimal64::parse("10");
  stop.ref_ = FakeSession<WithCalendar>::Ref{"9", ""};
  listing.orders_.push_back(stop);
  rig.session.set_working_orders(listing);
  rig.loop.run(rig.stop, 6);
  CHECK(rig.session.placed().size() == 1);  // only the midnight attempt
  CHECK(rig.live_runner.kept_intents_dropped() == 1);
}

namespace {

/** States a target of ten and two orders for the instrument on every bar: the pass that is kept. */
struct TargetStopAndTakeProfit {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
    const auto held = ctx.portfolio().net_quantity_of(bar.instrument_);
    if (!held || held->coefficient() <= 0) {
      return;
    }
    const auto rule = PriceRule::krx_equity();
    ctx.submit(strategy::OrderIntent::stop_limit(
      bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("5")),
      *strategy::Price::checked(Decimal64::parse("65000"), rule),
      *strategy::Price::checked(Decimal64::parse("64900"), rule)));
    ctx.submit(strategy::OrderIntent::limit(
      bar.instrument_, OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse("5")),
      *strategy::Price::checked(Decimal64::parse("80000"), rule)));
  }
};

} // namespace

TEST_CASE("a target, a stop and a take-profit kept together go out once, in order", "[kept-intents]") {
  KeptRig<TargetStopAndTakeProfit> rig;
  rig.source.held_ = Decimal64::parse("10");
  rig.refuse_out_of_hours();
  rig.loop.run(rig.stop, 17);
  CHECK(rig.live_runner.intents_kept_for_phase() == 2);
  rig.session.heal_place();
  rig.loop.run(rig.stop, 6);
  // Two midnight attempts (the target needed no trade), then both at the open, in the order stated.
  REQUIRE(rig.session.placed().size() == 4);
  CHECK(rig.session.placed()[2].type_ == OrderType::STOP_LIMIT);
  CHECK(rig.session.placed()[3].type_ == OrderType::LIMIT);
  CHECK(rig.live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("a later bar's statement supersedes every kept intent for the instrument", "[kept-intents]") {
  KeptRig<TargetStopAndTakeProfit> rig;
  rig.source.held_ = Decimal64::parse("10");
  rig.refuse_out_of_hours();
  rig.loop.run(rig.stop, 17);
  REQUIRE(rig.live_runner.intents_kept_for_phase() == 2);
  // The strategy speaks again for the instrument before the open: a direct pass standing for the next
  // bar. Its target is met (nothing to send), and both kept intents are forgotten rather than
  // released beside the new pass's own.
  rig.live_runner.context().submit([] {
    strategy::TargetPosition target;
    target.instrument_ = samsung();
    target.target_quantity_ = Decimal64::parse("10");
    return target;
  }());
  (void)rig.live_runner.feed(strategy::MarketPhase::PRE_OPEN);
  CHECK(rig.live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("an operator's halt discards a kept intent", "[kept-intents]") {
  KeptRig<BuysOne> rig;
  rig.loop.run(rig.stop, 17);
  REQUIRE(rig.live_runner.intents_kept_for_phase() == 1);
  rig.live_runner.guard().halt(strategy::HaltReason::OPERATOR);
  rig.loop.run(rig.stop, 2);
  CHECK(rig.live_runner.intents_kept_for_phase() == 0);
  rig.live_runner.guard().resume(strategy::HaltReason::OPERATOR);
  rig.loop.run(rig.stop, 4);
  CHECK(rig.session.placed().empty());
}

TEST_CASE("a keyed intent a broker answered is not kept", "[kept-intents]") {
  // A second order at the open would reuse the idempotency key of the one refused at midnight.
  KeptRig<StopsWhatItHolds, WithCalendarAndKeys> rig(StopsWhatItHolds{std::string("stop-1")});
  rig.source.held_ = Decimal64::parse("10");
  rig.refuse_out_of_hours();
  rig.loop.run(rig.stop, 17);
  CHECK(rig.live_runner.intents_kept_for_phase() == 0);
}

TEST_CASE("a dry run keeps no intent the phase refused", "[kept-intents]") {
  runner::DryRunner<BuysOne> dry;
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::PRE_OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));
  strategy::Portfolio<runner::DryTag> flat;
  flat.as_of_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100);
  dry.context().set_portfolio(flat);
  const auto report = dry.feed(bar_at(0, "70000"));
  REQUIRE(report.refused_.size() == 1);
  CHECK(dry.intents_kept_for_phase() == 0);
}

TEST_CASE("a broker refusal reaches a live run's report, with what the broker said") {
  // **The defect this slice exists for, exercised through the real source.** Five direct `feed`
  // cases prove the counters classify correctly and prove nothing about the defect, which lives in
  // `SessionSource` discarding the report `feed` returns. So this drives a real `SessionSource` and a
  // real `PollingLoop`: the poll succeeds, the order it produces is refused by the broker, and the
  // run's report has to say so.
  using LoopRunner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  session.set_bars(two_bars("70000", "71000"));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    BuysOne{}, any_phase, 512, live::SessionExecutor<FakeSession<WithKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  live::SessionSource<FakeSession<WithKey>> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<LoopRunner, live::SessionSource<FakeSession<WithKey>>, runner::ManualClock>
    loop(live_runner, source, schedule, runner::ManualClock{});
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
  runner::StopSource stop;

  BrokerError refusal;
  refusal.category_ = ErrorCategory::INVALID_ARGUMENT;
  refusal.broker_code_ = "APBK0918";
  refusal.message_ = "the account may not trade this instrument";
  session.fail_place_with(refusal);

  const auto report = loop.run(stop, 1);

  // The poll itself succeeded: this is not a failed request, it is a successful one the broker
  // answered with a refusal, which is exactly the case that used to be invisible.
  CHECK(report.attempted(runner::PollKind::BARS) == 1);
  CHECK(report.failed(runner::PollKind::BARS) == 0);
  // One: the scripted series carries two bars and this strategy buys on each, but only the newest
  // bar of a read may send; the older one is history (`Runner::feed_warmup`).
  CHECK(report.orders_broker_refused_ == 1);
  CHECK(report.orders_guard_refused_ == 0);
  CHECK(report.orders_unbuildable_ == 0);
  REQUIRE(report.last_broker_refusal_.has_value());
  CHECK(report.last_broker_refusal_->code_ == "APBK0918");
  CHECK(report.last_broker_refusal_->message_ == "the account may not trade this instrument");
  CHECK(report.last_broker_refusal_->instrument_ == samsung());

  // And a second run reports its own polling only: no stale count and no stale text, although the
  // runner still holds both.
  session.heal_place();
  const auto second = loop.run(stop, 1);
  CHECK(second.orders_broker_refused_ == 0);
  CHECK_FALSE(second.last_broker_refusal_.has_value());
  CHECK(live_runner.last_broker_refusal().has_value());
}

/**
 * A real `SessionSource` that asks for a stop once it has fed something.
 *
 * The stop has to arrive **during** the run and after the polls, because a stop requested before
 * `run` makes the loop's gate `STOPPING` from its first iteration and nothing is ever placed -- a
 * fixture that arranges for its own subject not to happen.
 */
template <typename Session>
class StoppingSource {
public:
  static constexpr bool provides_working_orders = true;

  StoppingSource(Session& session, std::vector<InstrumentId> universe, runner::StopSource& stop)
  : inner_(session, std::move(universe)), session_(&session), stop_(&stop) {}

  /** What the broker will say to the *closing* pass, which must be a different answer to be useful. */
  void refuse_the_stop_with(BrokerError error) { closing_error_ = std::move(error); }

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    const auto outcome = inner_.poll(kind, runner_instance);
    if (kind == runner::PollKind::BARS) {
      // The polling pass has had its refusal. From here the broker answers differently, so a report
      // that reads the record *after* the stop names an order none of its figures counted.
      if (closing_error_) {
        session_->fail_place_with(*closing_error_);
      }
      stop_->request(runner::StopIntent::FLATTEN);
    }
    return outcome;
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    return inner_.read_order_list(runner_instance);
  }

private:
  live::SessionSource<Session> inner_;
  Session* session_;
  runner::StopSource* stop_;
  std::optional<BrokerError> closing_error_;
};

TEST_CASE("a closing refusal does not relabel the polling refusal the report counted") {
  // **A report must not name an order none of its figures counted.** The counters stop at the moment
  // a stop is honoured; the record used to be read *after* it, so a refusal from the closing pass
  // replaced the polling one while the count still described the polling one. Different broker codes
  // on the two passes are what makes that visible.
  using LoopRunner = runner::Runner<
    SellsOnStop, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  session.set_bars(two_bars("70000", "71000"));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    SellsOnStop{}, any_phase, 512, live::SessionExecutor<FakeSession<WithKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("10", nullptr));

  runner::StopSource stop;
  StoppingSource<FakeSession<WithKey>> source(session, {samsung()}, stop);
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<LoopRunner, StoppingSource<FakeSession<WithKey>>, runner::ManualClock>
    loop(live_runner, source, schedule, runner::ManualClock{});
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();

  BrokerError polling;
  polling.category_ = ErrorCategory::INVALID_ARGUMENT;
  polling.broker_code_ = "WHILE-POLLING";
  polling.message_ = "refused during the polling loop";
  session.fail_place_with(polling);

  BrokerError closing;
  closing.category_ = ErrorCategory::INVALID_ARGUMENT;
  closing.broker_code_ = "WHILE-CLOSING";
  closing.message_ = "refused during the stop's flatten";
  source.refuse_the_stop_with(closing);

  const auto report = loop.run(stop, 1);

  // The stop's own flatten was refused too, under a different code -- and the record must still be
  // the polling one, because that is what the counters describe.
  REQUIRE(report.orders_broker_refused_ >= 1);
  REQUIRE(report.last_broker_refusal_.has_value());
  CHECK(report.last_broker_refusal_->code_ == "WHILE-POLLING");
  // And the runner does hold the later one, which is what makes this a boundary rather than an
  // accident: the loop chose which refusal its report describes.
  REQUIRE(live_runner.last_broker_refusal().has_value());
  CHECK(live_runner.last_broker_refusal()->code_ == "WHILE-CLOSING");
}

TEST_CASE("a refusal's account is replaced by the next one, not accumulated") {
  // One record, overwritten, which is what makes it affordable -- and the assertion is exact
  // replacement rather than "the newest text appears somewhere", because the second is satisfied by
  // an implementation that keeps every refusal forever.
  using Runner = runner::Runner<
    BuysOne, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  Runner live_runner(BuysOne{}, {}, 512, live::SessionExecutor<FakeSession<WithKey>>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  BrokerError first;
  first.category_ = ErrorCategory::INVALID_ARGUMENT;
  first.broker_code_ = "FIRST";
  first.message_ = "the first refusal";
  session.fail_place_with(first);
  live_runner.feed(bar_at(1, "70000"));
  REQUIRE(live_runner.last_broker_refusal().has_value());
  CHECK(live_runner.last_broker_refusal()->code_ == "FIRST");

  BrokerError second;
  second.category_ = ErrorCategory::INVALID_ARGUMENT;
  second.broker_code_ = "SECOND";
  second.message_ = "the second refusal";
  session.fail_place_with(second);
  live_runner.feed(bar_at(2, "70000"));

  REQUIRE(live_runner.last_broker_refusal().has_value());
  CHECK(live_runner.last_broker_refusal()->code_ == "SECOND");
  CHECK(live_runner.last_broker_refusal()->message_ == "the second refusal");
  CHECK(live_runner.orders_broker_refused() == 2);
}

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

  // The inquiry ran for Samsung and its order went out, so the declared path works. The *count* of
  // placements matters, not just the absence of refusals: a run that placed nothing would also have an
  // empty refusal list, and this test would then pass while the declared path did nothing at all.
  REQUIRE(session.sellable_asked().size() == 1);
  REQUIRE(session.sellable_asked()[0] == samsung());
  REQUIRE(report.refused_.empty());
  REQUIRE(session.placed().size() == 1);

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

TEST_CASE("a price-limits poll records today's band for every instrument it polls") {
  BandSession session;
  session.set_price_limit_upper("71000");
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung(), hynix()});
  live_runner.allow_unbounded_price_limits_age();

  const auto outcome = source.poll(runner::PollKind::PRICE_LIMITS, live_runner);

  CHECK(outcome == runner::PollOutcome::UPDATED);
  // The whole universe, because an instrument this loop trades is one whose band it needs.
  REQUIRE(session.price_limits_asked().size() == 2);
  CHECK(session.price_limits_asked()[0] == samsung());
  CHECK(session.price_limits_asked()[1] == hynix());
  // And it feeds nothing, which is `PollKind::PRICE_LIMITS`'s own obligation.
  CHECK(live_runner.strategy().bars_ == 0);
  CHECK(live_runner.strategy().quotes_ == 0);
}

TEST_CASE("a broker that publishes no band compiles no request") {
  // The `if constexpr` false branch. All three shipped adapters publish a band, so without this
  // instantiation that branch is never compiled at all -- which is how a call to a member the
  // capability set says does not exist survives in a header for a year.
  NoBandSession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<NoBandSession> source(session, {samsung()});

  CHECK(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::UNCHANGED);
  CHECK(session.price_limits_asked().empty());
}

TEST_CASE("a band poll that answers for one instrument and fails for another has failed") {
  // The same partial-answer rule the buying-power sweep argues in full: reporting `UPDATED` because
  // something arrived resets the loop's consecutive-failure count, and a name that fails every
  // sweep would then go on being priced against a band from another day.
  BandSession session;
  session.fail_price_limits_for(hynix());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung(), hynix()});

  CHECK(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::FAILED);
  CHECK(source.last_error().has_value());
  // Both were asked: one name failing must not abandon the rest of the round.
  CHECK(session.price_limits_asked().size() == 2);
}

TEST_CASE("a throttled band poll does not downgrade another instrument's outright failure") {
  BandSession session;
  session.fail_price_limits_for(samsung());
  session.throttle_price_limits_for(hynix());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung(), hynix()});

  CHECK(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  // The reason names the instrument that actually failed, not the one throttled behind it.
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("an outright band failure wins however late in the sweep it lands") {
  BandSession session;
  session.throttle_price_limits_for(samsung());
  session.fail_price_limits_for(hynix());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung(), hynix()});

  CHECK(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error().has_value());
  CHECK(source.last_error()->category_ == ErrorCategory::TRANSPORT);
}

TEST_CASE("a throttled band poll alone is a throttled poll, not an outage") {
  BandSession session;
  session.throttle_price_limits_for(samsung());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung()});

  CHECK(
    source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::RATE_LIMITED
  );
}

TEST_CASE("a failed band poll leaves the band it could not refresh in place") {
  // The same rule the calendar and the budget follow. Erasing the previous answer would turn a
  // timed-out request into an unclamped order, and the age policy -- not the transport -- is what
  // decides when a band has stopped being usable.
  BandSession session;
  session.set_price_limit_upper("71000");
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<BandSession> source(session, {samsung()});
  live_runner.allow_unbounded_price_limits_age();
  REQUIRE(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::UPDATED);

  session.fail_price_limits_for(samsung());
  CHECK(source.poll(runner::PollKind::PRICE_LIMITS, live_runner) == runner::PollOutcome::FAILED);

  // Still clamping, which is what "left in place" means where it can be observed.
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  CHECK(live_runner.price_bands_withheld() == 0);
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

namespace {
/** A journal whose every write fails, so a sweep's resolution cannot be recorded. */
struct UnwritableJournal : runner::MemoryJournal {
  bool append(const runner::JournalEntry&) { return false; }
};
} // namespace

TEST_CASE("a sweep that finds an order whose reply was lost hands it to the runner", "[confirm]") {
  // `SessionSource::reconcile` is the one place a sweep resolves in process; the runner must hear of it.
  ConfirmRig rig;
  rig.script.push_back(target_of("10"));
  rig.script.push_back(target_of("10"));
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.session.heal_place();
  REQUIRE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  REQUIRE(rig.runner_instance.unconfirmed_orders() == 1);

  // The broker lists it, resting, and the journal holds the instruction the sweep answers.
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  listed.ref_ = OrderRef<FakeBroker>{"KRX-1", ""};
  orders.orders_.push_back(listed);
  rig.session.set_working_orders(orders);
  rig.runner_instance.context().set_now(swept_at());

  SECTION("recorded: named by the listing's number, and counted on") {
    runner::MemoryJournal journal;
    journal.append(journaled_placement());
    live::SessionSource<WorkingSession, runner::MemoryJournal> source(rig.session, {samsung()});
    source.set_journal(journal);
    CHECK(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    // The listing covers it now: one row, the broker's, and no reference-less one beside it.
    rig.runner_instance.refresh_unconfirmed();
    const auto& working_rows = rig.runner_instance.context().portfolio().working_;
    REQUIRE(working_rows.size() == 1);
    CHECK(working_rows.front().ref_.primary_ == "KRX-1");
    CHECK_FALSE(working_rows.front().reserved_);
  }

  SECTION("not recorded: the runner halts and the order stays as it was") {
    UnwritableJournal journal;
    journal.MemoryJournal::append(journaled_placement());
    live::SessionSource<WorkingSession, UnwritableJournal> source(rig.session, {samsung()});
    source.set_journal(journal);
    source.read_order_list(rig.runner_instance);
    CHECK(rig.runner_instance.local_failure().has_value());
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    // Not named: the resolution that would have named it was never written down, so the hold stands.
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    rig.runner_instance.refresh_unconfirmed();
    bool unnamed = false;
    for (const auto& row : rig.runner_instance.context().portfolio().working_) {
      unnamed = unnamed || (row.reserved_ && row.ref_.primary_.empty());
    }
    CHECK(unnamed);
  }
}

TEST_CASE("a sweep before any bar stamps its resolution with the loop's clock", "[recovery]") {
  // Observed on a KIS demo account (2026-10-02, `labs/FINDINGS.md` F29): a process restarted after
  // `kill -9` resolved its predecessor's order at the startup read, before any bar set the decision
  // instant. The resolution line was stamped at the epoch, the journal reader takes such a line for
  // damage, and the next start halted ("lines this library did not write").
  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  const auto resolution_stamp = [](const runner::MemoryJournal& journal) {
    for (const auto& entry : journal.records()) {
      if (entry.outcome_ == runner::JournalOutcome::RESOLVED) {
        return std::optional<std::chrono::system_clock::time_point>(entry.recorded_at_);
      }
    }
    return std::optional<std::chrono::system_clock::time_point>{};
  };

  SECTION("a loop's reading stands in for the unset decision instant") {
    runner::MemoryJournal journal;
    journal.append(journaled_placement());
    auto live_runner = live::bind_observer(session, CountEvents{});
    live_runner.set_observed_at(swept_at());
    live_runner.ledger().mark(samsung(), "an order request failed in transit");
    live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
    source.set_journal(journal);

    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
    const auto stamp = resolution_stamp(journal);
    REQUIRE(stamp.has_value());
    CHECK(*stamp == swept_at());
  }

  SECTION("with no loop either, the wall clock") {
    runner::MemoryJournal journal;
    journal.append(journaled_placement());
    auto live_runner = live::bind_observer(session, CountEvents{});
    live_runner.ledger().mark(samsung(), "an order request failed in transit");
    live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
    source.set_journal(journal);

    const auto before = std::chrono::system_clock::now();
    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    const auto stamp = resolution_stamp(journal);
    REQUIRE(stamp.has_value());
    CHECK_FALSE(*stamp < before);
  }
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
  // Once to satisfy the gate, then the sweep over the days the matching window needs -- one stated
  // date per request, because this fake, like Kiwoom, takes no range: three days here (the window's
  // own day and one either side). The sweep happens only when something is owed, so a clean start
  // still costs one.
  CHECK(session.history_calls() == 4);
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
  // history covers every lifecycle state -- one stated date per request, since this fake, like
  // Kiwoom's `kt00007`, takes a single required date and no range.
  CHECK(session.history_calls() == 3);
  for (const auto& query : session.history_queries()) {
    REQUIRE(query.from_);
    REQUIRE(query.to_);
    CHECK(query.from_->day_ == query.to_->day_);
  }
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

namespace {

/** The KIS-contract session, able to place and withdraw, so a live runner can track its orders. */
class KisContractTradingSession : public KisContractSession {
public:
  using Placed = PlacedOrder<FakeBroker>;

  Result<Placed> place(OrderRequest) {
    Placed ack;
    ack.ack_.order_id_ = "KIS-1";
    ack.ref_ = Ref{"KIS-1", ""};
    return Result<Placed>(std::move(ack));
  }

  Result<Placed> cancel(const Ref& ref, const AmendInstruction&) {
    Placed ack;
    ack.ref_ = ref;
    return Result<Placed>(std::move(ack));
  }
};

} // namespace

TEST_CASE("a sent order's history is read from the day it was sent, whatever bar was last fed", "[confirm]") {
  // Combined review of the 2026-10-02 run: the read's start is when the order was sent (the loop's
  // clock), its end was the decision instant (the last bar's period end). A daily strategy sending on
  // a Monday with Friday's bar fed asked KIS for an inverted range, which KIS's conversion refuses, so
  // every working-order poll failed and the loop gave up.
  using KisRunner = runner::Runner<
    Scripted, KisLikeCaps, FakeBroker, live::SessionExecutor<KisContractTradingSession>
  >;
  KisContractTradingSession session;
  std::deque<std::variant<strategy::TargetPosition, strategy::OrderIntent>> script;
  script.push_back(target_of("10"));
  KisRunner live_runner(Scripted{&script}, {}, 512, live::SessionExecutor<KisContractTradingSession>(session));
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(polled(0, "0", {}));

  // Friday's bar, decided on Monday morning: three days between the decision instant and the clock.
  const auto friday = bar_at(1, "70000");
  live_runner.set_observed_at(friday.period_end_ + std::chrono::hours(72));
  live_runner.feed(friday);
  REQUIRE(live_runner.unconfirmed_orders() == 1);

  // A listing carried it, and the next one does not: only its history can say what became of it.
  auto listed = polled(1, "0", {listed_row("KIS-1", OrderSide::BUY, "10", "0")});
  listed.working_as_of_ = listed.as_of_ = friday.period_end_ + std::chrono::hours(72);
  live_runner.context().set_portfolio(listed);
  live_runner.refresh_unconfirmed();

  live::SessionSource<KisContractTradingSession, runner::MemoryJournal> source(session, {samsung()});
  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UPDATED);
  CHECK_FALSE(session.rejection().has_value());
  REQUIRE(session.last_query().has_value());
  REQUIRE(session.last_query()->to_.has_value());
  const auto sent_day = runner::date_of(friday.period_end_ + std::chrono::hours(72));
  CHECK_FALSE(*session.last_query()->to_ < sent_day);
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

/** An account-only broker: it publishes neither a calendar nor a band. */
struct AccountOnlyCaps : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
  static constexpr bool has_buying_power = true;
};
/** A broker read for market data alone: a calendar and a band. */
struct MarketDataCaps : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
  static constexpr bool has_calendar = true;
  static constexpr bool has_price_limits = true;
};

TEST_CASE("a second session serves the market reads and the first the account") {
  // `example-live-program` on a KIS demo account: the demo host refuses the calendar and reference
  // data, so they come from a session that cannot trade. Different types, so the calendar and band
  // polls are gated on the market-data session's capabilities -- the trading one publishes neither.
  FakeSession<AccountOnlyCaps> trading;
  FakeSession<MarketDataCaps> market;
  market.set_bars(two_bars("70000", "71000"));
  market.set_calendar(one_day_calendar(Date{2026, 3, 10}));
  market.set_price_limit_upper("91000");
  auto live_runner = live::bind_observer(trading, CountEvents{});
  live::SessionSource<FakeSession<AccountOnlyCaps>, runner::MemoryJournal, FakeSession<MarketDataCaps>>
    source(trading, market, {samsung()});
  source.set_buying_power_currencies({Currency{"KRW"}});

  // Every market read succeeds while the trading session fails everything, so none reached it.
  trading.fail_with(ErrorCategory::TRANSPORT);
  for (const auto kind :
       {runner::PollKind::BARS, runner::PollKind::QUOTES, runner::PollKind::CALENDAR,
        runner::PollKind::PRICE_LIMITS}) {
    INFO(static_cast<int>(kind));
    CHECK(source.poll(kind, live_runner) == runner::PollOutcome::UPDATED);
  }
  CHECK(market.bar_calls() == 1);
  CHECK(market.quote_calls() == 1);
  CHECK(market.calendar_calls() == 1);
  CHECK(market.price_limits_asked().size() == 1);
  CHECK(trading.bar_calls() == 0);
  CHECK(trading.quote_calls() == 0);
  CHECK(trading.calendar_calls() == 0);
  CHECK(trading.price_limits_asked().empty());

  // And the account reads go to the trading session, never the market one.
  trading.succeed();
  market.fail_with(ErrorCategory::TRANSPORT);
  for (const auto kind :
       {runner::PollKind::HOLDINGS, runner::PollKind::WORKING_ORDERS,
        runner::PollKind::BUYING_POWER}) {
    INFO(static_cast<int>(kind));
    CHECK(source.poll(kind, live_runner) == runner::PollOutcome::UPDATED);
  }
  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(trading.holdings_calls() == 1);
  CHECK(trading.working_calls() == 2);
  CHECK(trading.buying_power_asked().size() == 1);
  CHECK(market.holdings_calls() == 0);
  CHECK(market.working_calls() == 0);
  CHECK(market.buying_power_asked().empty());
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

namespace {

/** Records every order update, and what its portfolio held at the time. */
struct WatchesOrders {
  std::vector<OrderSnapshot> updates_;

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_order_update(Ctx&, const OrderSnapshot& order) {
    updates_.push_back(order);
  }
};

/** A working row with a stated reference, filled quantity and status. */
WorkingOrder<FakeBroker> update_row(
  const char* primary,
  const char* secondary,
  const char* filled,
  OrderStatus status = OrderStatus::PENDING
) {
  auto row = working();
  row.ref_ = OrderRef<FakeBroker>{primary, secondary};
  row.order_.order_id_ = primary;
  row.order_.execution_.filled_quantity_ = Decimal64::parse(filled);
  row.order_.status_ = status;
  return row;
}

} // namespace

TEST_CASE("a resting order is reported once, not once per poll") {
  // A working listing republishes every resting order on every poll. Reporting each of them would
  // make `on_order_update` a per-poll firehose rather than a change notification.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {update_row("A1", "", "0")});
  CHECK(live_runner.strategy().updates_.size() == 1);

  poll_rows(source, live_runner, session, {update_row("A1", "", "0")});
  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("execution progress on a resting order is reported, with the new figure") {
  // Asserted on the *value* and not only the count: a detector feeding the previous row would pass
  // a count-only assertion while telling the strategy nothing had happened.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {update_row("A1", "", "0")});
  poll_rows(
    source, live_runner, session, {update_row("A1", "", "4", OrderStatus::PARTIAL_FILLED)}
  );

  REQUIRE(live_runner.strategy().updates_.size() == 2);
  CHECK(live_runner.strategy().updates_[1].execution_.filled_quantity_ == Decimal64::parse("4"));
  CHECK(live_runner.strategy().updates_[1].status_ == OrderStatus::PARTIAL_FILLED);
}

TEST_CASE("a quantity restated at another scale is not a change") {
  // `Decimal64::operator==` compares the coefficient *and the scale*, so `Decimal64(100, 1)` and
  // `Decimal64(10, 0)` are distinct values although both are ten.
  //
  // **Built at an explicit scale on purpose.** `Decimal64::parse` strips trailing fractional zeros,
  // so `parse("10")` and `parse("10.0")` are the *same* value -- a fixture written that way passes
  // whether the detector compares numerically or with `==`, which is a test that proves nothing.
  // The two-argument constructor is the case that can actually reach here, from an adapter this
  // library has never seen.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto first = update_row("A1", "", "0");
  first.order_.quantity_ = Decimal64(10, 0);
  poll_rows(source, live_runner, session, {first});
  REQUIRE(live_runner.strategy().updates_.size() == 1);

  auto restated = update_row("A1", "", "0");
  restated.order_.quantity_ = Decimal64(100, 1);
  REQUIRE_FALSE(first.order_.quantity_ == restated.order_.quantity_);
  poll_rows(source, live_runner, session, {restated});

  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("a row that leaves the listing is reported to nobody") {
  // A listing cannot say whether an order filled, was cancelled, expired or was rejected, so a
  // disappearance is counted where it is already counted and interpreted nowhere.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(source, live_runner, session, {update_row("A1", "", "0")});
  poll_rows(source, live_runner, session, {});

  CHECK(live_runner.strategy().updates_.size() == 1);
  CHECK(source.charged_costs().working_list_disappearances_ == 1);
}

TEST_CASE("two rows sharing an order number and differing in branch are two orders") {
  // KIS's reference carries `odno` *and* `ord_gno_brno`, and the branch code is what an amendment
  // must address. Keyed on `order_id_` alone these two rows would compare against each other and
  // one update would be suppressed.
  //
  // The KIS workbook states no uniqueness scope either way, so this is an adversarial fixture
  // protecting the address identity rather than evidence that KIS emits such a pair.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(
    source, live_runner, session,
    {update_row("0001569139", "01", "0"), update_row("0001569139", "02", "0")}
  );
  CHECK(live_runner.strategy().updates_.size() == 2);

  // And a change to one of them is still reported while the other is unchanged.
  poll_rows(
    source, live_runner, session,
    {update_row("0001569139", "01", "0"),
     update_row("0001569139", "02", "5", OrderStatus::PARTIAL_FILLED)}
  );
  REQUIRE(live_runner.strategy().updates_.size() == 3);
  CHECK(live_runner.strategy().updates_[2].execution_.filled_quantity_ == Decimal64::parse("5"));
}

TEST_CASE("a row with no reference at all is counted and reported to nobody") {
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto anonymous = update_row("A1", "", "0");
  anonymous.ref_ = OrderRef<FakeBroker>{"", ""};
  anonymous.order_.order_id_ = "";
  poll_rows(source, live_runner, session, {anonymous});

  CHECK(live_runner.strategy().updates_.empty());
}

TEST_CASE("a row with a reference but no display order id is still reported") {
  // The converse of the case above, and the one that matters for an adapter this library has never
  // seen: a third party may fill an addressable reference and leave the display id empty, and
  // dropping its updates would punish it for a field it does not need.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  auto addressable = update_row("A1", "", "0");
  addressable.order_.order_id_ = "";
  poll_rows(source, live_runner, session, {addressable});

  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("the startup read adopts the listing rather than reporting it") {
  // `read_order_list` runs inside `PollingLoop::gate_for`, which the loop calls *before* it applies
  // the startup hold, and it is handed a mutable runner -- so a `Runner::feed` there would dispatch
  // to a strategy that can still submit, against the picture the gate exists to withhold.
  //
  // **The second half is what makes this a test rather than a coincidence.** Skipping the feed
  // without adopting the rows would also leave the count at zero here, and the whole listing would
  // then arrive at the first scheduled poll as if it were new. So the same unchanged listing is
  // polled afterwards and must still report nothing.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  WorkingOrders<FakeBroker> listing;
  listing.orders_ = {update_row("A1", "", "0")};
  session.set_working_orders(listing);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  CHECK(live_runner.strategy().updates_.empty());
  // And it is state rather than an event: the same call installed it where a strategy reads what a
  // predecessor left resting.
  CHECK(live_runner.context().portfolio().working_.size() == 1);

  poll_rows(source, live_runner, session, {update_row("A1", "", "0")});
  CHECK(live_runner.strategy().updates_.empty());

  // A genuine change after that startup read is still reported, so adopting is not muting.
  poll_rows(
    source, live_runner, session, {update_row("A1", "", "3", OrderStatus::PARTIAL_FILLED)}
  );
  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("one order stated twice in a listing is reported once") {
  // A duplicated row would otherwise be one change delivered as two events -- and a strategy
  // answering an update may submit, so a duplicated row becomes a duplicated order. Both brokers
  // that page this listing append their pages without deduplicating, so the input is not excluded
  // upstream.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(
    source, live_runner, session, {update_row("A1", "", "0"), update_row("A1", "", "0")}
  );
  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("two readings of one order that disagree are reported to nobody") {
  // Neither reading can be preferred and nothing here can say which is true, so reporting either
  // states something unestablished and reporting both hands a strategy a contradiction as two
  // events. The same answer slice 27 reached for two sellable figures that contradicted.
  WorkingSession session;
  auto live_runner = live::bind_observer(session, WatchesOrders{});
  live::SessionSource<WorkingSession> source(session, {samsung()});

  poll_rows(
    source,
    live_runner,
    session,
    {update_row("A1", "", "0"), update_row("A1", "", "5", OrderStatus::PARTIAL_FILLED)}
  );
  CHECK(live_runner.strategy().updates_.empty());

  // And it keeps no baseline it could not settle on: a later listing stating the order once
  // reports it afresh rather than against a reading this one could not choose.
  poll_rows(source, live_runner, session, {update_row("A1", "", "5", OrderStatus::PARTIAL_FILLED)});
  CHECK(live_runner.strategy().updates_.size() == 1);
}

TEST_CASE("a stop's trigger moved on its own is reported, and a disagreement about it is not") {
  // A stop's trigger can be amended at the same limit and quantity, through this library or any
  // other channel. A strategy holding the stop is owed that event, and two rows for one order that
  // disagree only about the trigger are a contradiction like any other.
  const auto stop_row = [](const char* trigger) {
    auto row = update_row("A1", "", "0");
    row.order_.type_ = OrderType::STOP_LIMIT;
    row.order_.trigger_price_ = Decimal64::parse(trigger);
    return row;
  };

  SECTION("a trigger-only change is an update, carrying the new trigger") {
    WorkingSession session;
    auto live_runner = live::bind_observer(session, WatchesOrders{});
    live::SessionSource<WorkingSession> source(session, {samsung()});
    poll_rows(source, live_runner, session, {stop_row("69500")});
    poll_rows(source, live_runner, session, {stop_row("69500")});
    REQUIRE(live_runner.strategy().updates_.size() == 1);
    poll_rows(source, live_runner, session, {stop_row("69800")});
    REQUIRE(live_runner.strategy().updates_.size() == 2);
    CHECK(live_runner.strategy().updates_[1].trigger_price_ == Decimal64::parse("69800"));
  }

  SECTION("two rows that differ only in their trigger are reported to nobody") {
    WorkingSession session;
    auto live_runner = live::bind_observer(session, WatchesOrders{});
    live::SessionSource<WorkingSession> source(session, {samsung()});
    poll_rows(source, live_runner, session, {stop_row("69500"), stop_row("69800")});
    CHECK(live_runner.strategy().updates_.empty());
  }
}

namespace {

/** Flattens on leaving the session; holds ten again if an update names the order "KEEP". */
struct FlattensOnPhase {
  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    if (phase == strategy::MarketPhase::OPEN) {
      return;
    }
    strategy::TargetPosition flat;
    flat.instrument_ = samsung();
    flat.target_quantity_ = Decimal64::parse("0");
    ctx.submit(flat);
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot& order) {
    if (order.order_id_ != "KEEP") {
      return;
    }
    strategy::TargetPosition hold;
    hold.instrument_ = samsung();
    hold.target_quantity_ = Decimal64::parse("10");
    ctx.submit(hold);
  }
};

std::chrono::system_clock::time_point live_hour(int hours) {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 100 + hours);
}

/** Polls a working-order listing received at `hour`. */
template <typename Source, typename Runner>
void poll_listing_at(
  Source& source, Runner& bound, WorkingSession& session,
  std::vector<WorkingOrder<FakeBroker>> rows, int hour
) {
  WorkingOrders<FakeBroker> listing;
  listing.orders_ = std::move(rows);
  listing.received_at_ = live_hour(hour);
  session.set_working_orders(std::move(listing));
  REQUIRE(source.poll(runner::PollKind::WORKING_ORDERS, bound) == runner::PollOutcome::UPDATED);
}

/** Polls holdings of `held` received at `hour`. */
template <typename Source, typename Runner>
void poll_holdings_at(Source& source, Runner& bound, WorkingSession& session, const char* held, int hour) {
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  snapshot.positions_.push_back(position);
  snapshot.received_at_ = live_hour(hour);
  snapshot.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING, SellableBasis::RestingTreatment::DEDUCT, false,
  };
  session.set_holdings(snapshot);
  REQUIRE(source.poll(runner::PollKind::HOLDINGS, bound) == runner::PollOutcome::UPDATED);
}

WorkingOrder<FakeBroker> resting_stop() {
  auto stop = update_row("STOP-1", "", "0");
  stop.order_.side_ = OrderSide::SELL;
  stop.order_.type_ = OrderType::STOP_LIMIT;
  stop.order_.quantity_ = Decimal64::parse("10");
  stop.order_.price_ = Decimal64::parse("69000");
  stop.order_.trigger_price_ = Decimal64::parse("69500");
  return stop;
}

/** Binds the strategy, holds ten behind a resting stop, and leaves the session. */
template <typename Bound>
void leave_behind_a_stop(Bound& bound, WorkingSession& session) {
  bound.context().set_instrument(samsung_reference());
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithWorkingOrders>("10", nullptr);
  portfolio.working_.push_back(resting_stop());
  bound.context().set_portfolio(portfolio);
  bound.feed(strategy::MarketPhase::POST_CLOSE);
  CHECK(session.canceled().size() == 1);
  CHECK(session.placed().empty());
  REQUIRE(bound.deferred_targets().size() == 1);
}

} // namespace

/** Scripts the broker's history to hold the stop's own row, in `status` with `filled` executed. */
void record_stop(WorkingSession& session, OrderStatus status, const char* filled) {
  OrderSnapshot row;
  row.order_id_ = "STOP-1";
  row.instrument_ = samsung();
  row.side_ = OrderSide::SELL;
  row.type_ = OrderType::STOP_LIMIT;
  row.status_ = status;
  row.quantity_ = Decimal64::parse("10");
  row.execution_.filled_quantity_ = Decimal64::parse(filled);
  OrderPage page;
  page.orders_.push_back(row);
  session.set_history(std::move(page));
}

TEST_CASE("a deferred flatten is sent once the broker records its stop cancelled unfilled", "[trigger]") {
  // No holdings poll at all: the working-order poll that no longer lists the stop reads the stop's own
  // history row on demand, and that row decides. (Codex round 4's first blocker.)
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);
  CHECK(session.history_calls() == 0);

  record_stop(session, OrderStatus::CANCELED, "0");
  poll_listing_at(source, bound, session, {}, 1);
  // A broker that takes no range -- this fake, like Kiwoom -- is asked one stated date at a time:
  // Kiwoom's `kt00007` requires the date and refuses a range. (Codex round 5.)
  REQUIRE(session.history_calls() >= 1);
  for (const auto& query : session.history_queries()) {
    REQUIRE(query.from_);
    REQUIRE(query.to_);
    CHECK(query.from_->day_ == query.to_->day_);
  }
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed()[0].side_ == OrderSide::SELL);
  CHECK(session.placed()[0].quantity_ == Decimal64::parse("10"));
  CHECK(bound.deferred_targets().empty());
}

TEST_CASE("a deferred flatten does not sell again after the stop filled", "[trigger]") {
  // Holdings still say ten -- stale, the broker's endpoints promise no consistency -- and the stop's own
  // record says it filled. (Codex round 4's second blocker.)
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);
  poll_holdings_at(source, bound, session, "10", 2);

  record_stop(session, OrderStatus::FILLED, "10");
  poll_listing_at(source, bound, session, {}, 1);
  CHECK(session.placed().empty());
  CHECK(bound.deferred_targets().empty());
  CHECK(bound.deferred_dropped_after_fill() == 1);
}

TEST_CASE("a deferred flatten waits while the stop's record is not terminal, or cannot be read", "[trigger]") {
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);

  // History lags the listing: still open. Asked again on each working-order poll, and nothing sent.
  record_stop(session, OrderStatus::PENDING, "0");
  poll_listing_at(source, bound, session, {}, 1);
  CHECK(session.history_calls() >= 1);
  CHECK(session.placed().empty());
  REQUIRE(bound.deferred_targets().size() == 1);

  // The history read fails: reported like any poll failure, and the target still waits.
  session.fail_history_with(ErrorCategory::TRANSPORT);
  WorkingOrders<FakeBroker> listing;
  listing.received_at_ = live_hour(2);
  session.set_working_orders(listing);
  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, bound) != runner::PollOutcome::UPDATED);
  CHECK(session.placed().empty());
  CHECK(bound.deferred_targets().size() == 1);
}

TEST_CASE("an update in the same listing supersedes the deferred flatten before it can go", "[trigger]") {
  // **Codex round 3's second blocker.** The listing that drops the stop also carries an update the
  // strategy answers by holding ten. That answer must win, whatever the order of rows.
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);
  record_stop(session, OrderStatus::CANCELED, "0");

  auto first = update_row("OTHER", "", "0");
  auto keep = update_row("KEEP", "", "0");
  poll_listing_at(source, bound, session, {first, keep}, 1);
  CHECK(bound.deferred_targets().empty());
  CHECK(session.placed().empty());
}

TEST_CASE("the evidence read covers the Korean day when UTC has not reached it yet", "[trigger]") {
  // **Codex round 6.** At 08:45 KST on 30 September it is 23:45 UTC on the 29th. A stop placed and
  // withdrawn that morning lives in Kiwoom's history under 30 September, which a read ending at the UTC
  // date never asked for.
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  const auto morning_kst = std::chrono::system_clock::time_point{} +
    std::chrono::hours(24 * 20726) - std::chrono::minutes(15);  // 2026-09-29T23:45Z
  REQUIRE(runner::date_of(morning_kst).day_ == 29);
  bound.context().set_now(morning_kst);
  leave_behind_a_stop(bound, session);

  record_stop(session, OrderStatus::CANCELED, "0");
  poll_listing_at(source, bound, session, {}, 1);
  bool asked_for_the_30th = false;
  for (const auto& query : session.history_queries()) {
    REQUIRE(query.from_);
    if (query.from_->month_ == 9 && query.from_->day_ == 30) {
      asked_for_the_30th = true;
    }
  }
  CHECK(asked_for_the_30th);
  CHECK(session.placed().size() == 1);
}

TEST_CASE("a row from another day under the same order number is a different order", "[recovery]") {
  // **Codex's round-1 finding on the single-date sweep.** Kiwoom numbers orders per day, and a sweep
  // read one date at a time can see yesterday's order under today's number. Merged by the number alone,
  // today's row -- the journalled order, filled and gone from the working list -- was dropped, the
  // older row failed the time window, and recovery said `NOT_PLACED`.
  WorkingSession session;
  session.set_working_orders(WorkingOrders<FakeBroker>{});
  auto yesterday = listed_order("KRX-1");
  yesterday.ordered_at_ = journaled_at() - std::chrono::hours(20);
  yesterday.status_ = OrderStatus::FILLED;
  auto today = listed_order("KRX-1");
  today.status_ = OrderStatus::FILLED;
  const auto today_date = runner::date_of(journaled_at());
  session.history_by_query_ = [&](const OrderHistoryQuery& query) {
    OrderPage page;
    if (query.from_ && query.from_->day_ == today_date.day_) {
      page.orders_.push_back(today);
    } else {
      page.orders_.push_back(yesterday);
    }
    return page;
  };

  runner::MemoryJournal journal;
  journal.append(journaled_placement());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");
  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE_FALSE(source.last_reconcile().matches_.empty());
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::MATCHED);
  CHECK(source.last_reconcile().matches_.front().order_id_ == "KRX-1");

  // Yesterday's row with no readable time does not swallow today's either (Codex round 2): the pages
  // are never merged against each other.
  yesterday.ordered_at_.reset();
  runner::MemoryJournal again;
  again.append(journaled_placement());
  auto second = live::bind_observer(session, CountEvents{});
  second.context().set_now(swept_at());
  second.ledger().mark(samsung(), "an order request failed in transit");
  live::SessionSource<WorkingSession, runner::MemoryJournal> other(session, {samsung()});
  other.set_journal(again);
  CHECK(other.read_order_list(second) == runner::PollOutcome::UPDATED);
  REQUIRE_FALSE(other.last_reconcile().matches_.empty());
  // Both rows are kept. The unstamped one cannot be ruled out by time, so the verdict is an ambiguity
  // an operator settles -- never `NOT_PLACED`, and never today's order silently attributed away.
  CHECK(other.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::AMBIGUOUS);

  // Against the working list: an exact copy of a resting row is one candidate; a terminal row under
  // the same number is kept.
  auto resting = listed_order("KRX-1");
  resting.status_ = OrderStatus::PENDING;
  std::vector<OrderSnapshot> candidates{resting};
  runner::merge_dated_history(candidates, {resting, yesterday});
  CHECK(candidates.size() == 2);
}

TEST_CASE("an open history row is not dropped under a working row that only shares its number", "[recovery]") {
  // **Codex round 3.** Yesterday's journalled order is still open in yesterday's history; today a
  // different order resting in the working list carries the same number. Dropping the history row as
  // the working row's copy left no candidate inside the window, and recovery said `NOT_PLACED`.
  WorkingSession session;
  auto today = working();
  today.ref_ = OrderRef<FakeBroker>{"KRX-1", ""};
  today.order_.order_id_ = "KRX-1";
  today.order_.ordered_at_ = journaled_at() + std::chrono::hours(20);
  WorkingOrders<FakeBroker> listing;
  listing.orders_.push_back(today);
  session.set_working_orders(listing);
  auto pending = listed_order("KRX-1");
  pending.status_ = OrderStatus::PENDING;
  OrderPage page;
  page.orders_.push_back(pending);
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");
  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE_FALSE(source.last_reconcile().matches_.empty());
  // The history row survives and is the journalled order: matched, on the evidence of its own row.
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::MATCHED);
  CHECK(source.last_reconcile().matches_.front().order_id_ == "KRX-1");
}

TEST_CASE("a resting order listed in both inquiries recovers as one match on a no-range broker", "[recovery]") {
  // The happy path of the exact-copy rule, end to end: the working row and the history row state the
  // same id, instrument and order time -- as Kiwoom's and Toss's listings do -- so they are one
  // candidate and the journalled order is `MATCHED`. (Fable's note at slice 44's merge check.)
  WorkingSession session;
  auto resting = working();
  resting.ref_ = OrderRef<FakeBroker>{"KRX-1", ""};
  resting.order_ = listed_order("KRX-1");
  resting.order_.status_ = OrderStatus::PENDING;
  WorkingOrders<FakeBroker> listing;
  listing.orders_.push_back(resting);
  session.set_working_orders(listing);
  OrderPage page;
  page.orders_.push_back(resting.order_);
  session.set_history(page);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");
  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE_FALSE(source.last_reconcile().matches_.empty());
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::MATCHED);
  CHECK(source.last_reconcile().matches_.front().candidates_ == 1);
  CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));

  // Stamped differently, the same order is two candidates and the hold stays: the stated cost.
  auto restamped = resting.order_;
  restamped.ordered_at_ = *restamped.ordered_at_ + std::chrono::seconds(1);
  OrderPage other;
  other.orders_.push_back(restamped);
  session.set_history(other);
  auto second = live::bind_observer(session, CountEvents{});
  second.context().set_now(swept_at());
  second.ledger().mark(samsung(), "an order request failed in transit");
  runner::MemoryJournal again;
  again.append(journaled_placement());
  live::SessionSource<WorkingSession, runner::MemoryJournal> source2(session, {samsung()});
  source2.set_journal(again);
  CHECK(source2.read_order_list(second) == runner::PollOutcome::UPDATED);
  CHECK(source2.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::AMBIGUOUS);
  CHECK(source2.last_reconcile().matches_.front().candidates_ == 2);
  CHECK(second.ledger().requires_reconcile(samsung()));
}

// ---------------------------------------------------------------------------
// The live program: examples/live_program.hpp, driven against the in-memory session.
//
// What `main` cannot be tested for, its wiring can: the order of journal, recovery, loop and report,
// and which exit each ending takes. No broker is involved, so what these cannot show is any real
// broker's behaviour.
// ---------------------------------------------------------------------------

namespace {

using ProgramSession = FakeSession<WithWorkingOrders>;

/** The instant every program test starts at: 2026-03-30 10:00 in Seoul, a Monday. */
std::chrono::system_clock::time_point program_start() {
  return std::chrono::system_clock::time_point{} + std::chrono::seconds(1774832400);
}

/** A file path that exists only for the duration of one test, removed on the way in and out. */
class TempPath {
public:
  explicit TempPath(std::string path) : path_(std::move(path)) { std::remove(path_.c_str()); }
  ~TempPath() { std::remove(path_.c_str()); }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return path_; }

private:
  std::string path_;
};

/** Returns whether `path` names a file that exists. */
bool file_exists(const std::string& path) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (!handle) {
    return false;
  }
  std::fclose(handle);
  return true;
}

/** Reads `path` whole. */
std::string file_text(const std::string& path) {
  std::string text;
  if (std::FILE* handle = std::fopen(path.c_str(), "rb")) {
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, handle)) > 0) {
      text.append(buffer, read);
    }
    std::fclose(handle);
  }
  return text;
}

/** Writes `text` to `path`. */
void write_text(const std::string& path, const std::string& text) {
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  REQUIRE(handle != nullptr);
  std::fwrite(text.data(), 1, text.size(), handle);
  std::fclose(handle);
}

/** A strategy that wants one share, as the program's does. */
strategy::vanilla::BuyAndHold one_share() {
  strategy::vanilla::BuyAndHold holding;
  holding.params_.instrument_ = samsung();
  holding.params_.quantity_ = Decimal64::parse("1");
  return holding;
}

/**
 * A session that answers everything a trading run needs, stamped at `program_start()`.
 *
 * Holdings are empty but current, so the portfolio gate opens and BuyAndHold wants its share. The
 * loop reads the account before the bars, so the bars read at start are the ones that trade.
 *
 * The fake never lists or holds an order it accepted, so a *later* bar would buy again against a
 * broker that forgot it; the bars here never change, which keeps that out of the fixture.
 */
void script_ready(ProgramSession& session) {
  session.set_reference(samsung_reference());
  session.set_bars(two_bars("70000", "71000"));
  HoldingsSnapshot holdings;
  holdings.received_at_ = program_start();
  session.set_holdings(holdings);
}

/**
 * Settings for a short run: every kind due each second, and no phase restriction.
 *
 * `permitted_phases_` is lifted because the phase is not what these tests are about, and the fake
 * publishes no calendar; the program itself keeps the default.
 */
live_program::Settings program_settings(const TempPath& journal, const TempPath& control) {
  live_program::Settings settings;
  settings.universe_ = {samsung()};
  settings.limits_.permitted_phases_ = std::nullopt;
  settings.limits_.max_position_quantity_ = Decimal64::parse("1");
  settings.schedule_.bars_ = std::chrono::seconds(1);
  settings.schedule_.holdings_ = std::chrono::seconds(1);
  settings.schedule_.working_orders_ = std::chrono::seconds(1);
  settings.schedule_.tick_ = std::chrono::seconds(1);
  settings.schedule_.calendar_.reset();
  settings.maximum_portfolio_age_ = std::chrono::minutes(1);
  settings.journal_path_ = journal.str();
  settings.journal_scope_ = "fake:demo:1";
  settings.control_file_ = control.str();
  settings.max_iterations_ = 3;
  return settings;
}

} // namespace

TEST_CASE("the live program journals, recovers clean and places its order", "[live][program]") {
  const TempPath journal("atl-test-program-trade.jsonl");
  const TempPath control("atl-test-program-trade.stop");
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::CLEAN);
  CHECK(out.str().find("recovery: clean") != std::string::npos);
  // The account was read before the bars, so the decision on the bars read at start was not
  // withheld: the first decision is the one that trades.
  CHECK(out.str().find("plans withheld  : 0") != std::string::npos);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed().front().quantity_ == Decimal64::parse("1"));
  // Recorded before it was sent, and answered after.
  const auto text = file_text(journal.str());
  CHECK(text.find("\"outcome\":\"pending\"") != std::string::npos);
  CHECK(text.find("\"outcome\":\"accepted\"") != std::string::npos);
  CHECK(text.find("fake:demo:1") != std::string::npos);
}

TEST_CASE("the live program reads reference data and bars from its market-data session", "[live][program]") {
  // The trading session has no reference data and no bars, as a KIS demo host has no reference data:
  // the run gets past both only by reading them from the second session, and still places through
  // the first.
  const TempPath journal("atl-test-program-market-data.jsonl");
  const TempPath control("atl-test-program-market-data.stop");
  ProgramSession session;
  HoldingsSnapshot holdings;
  holdings.received_at_ = program_start();
  session.set_holdings(holdings);
  BandSession market;
  market.set_reference(samsung_reference());
  market.set_bars(two_bars("70000", "71000"));
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, market, one_share(), program_settings(journal, control), stop, out,
    runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::CLEAN);
  REQUIRE(session.placed().size() == 1);
  CHECK(market.placed().empty());
  CHECK(market.bar_calls() > 0);
  CHECK(session.bar_calls() == 0);
  CHECK(market.holdings_calls() == 0);

  // The one-session form is the same program with nothing read from elsewhere: no reference data,
  // so it stops before trading.
  const TempPath again("atl-test-program-market-data-own.jsonl");
  std::ostringstream own;
  const auto alone = live_program::trade(
    session, one_share(), program_settings(again, control), stop, own,
    runner::ManualClock{program_start()}
  );
  CHECK(alone == live_program::Exit::REFERENCE_UNREADABLE);
}

TEST_CASE("the live program sends and journals its stated route", "[live][program][route]") {
  // `Settings::default_route_` is how `example-live-program` makes a Kiwoom run placeable; it has to
  // reach the executor the runner sends through, and the file journal recovery will read.
  const TempPath journal("atl-test-program-route.jsonl");
  const TempPath control("atl-test-program-route.stop");
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;
  auto settings = program_settings(journal, control);
  settings.default_route_ = Venue::NXT;

  const auto exit = live_program::trade(
    session, one_share(), settings, stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::CLEAN);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed().front().venue_ == Venue::NXT);
  CHECK(file_text(journal.str()).find("\"route\":\"NXT\"") != std::string::npos);
}

TEST_CASE("the live program withholds what a previous run left pending", "[live][program]") {
  const TempPath journal("atl-test-program-pending.jsonl");
  const TempPath control("atl-test-program-pending.stop");
  {
    // A predecessor that died between writing the line and hearing the answer. Stamped at the
    // program's own start, so the startup sweep's window is still open and cannot call it
    // `NOT_PLACED` from an empty listing.
    runner::FileJournal previous(journal.str(), "fake:demo:1");
    REQUIRE(previous.open());
    runner::JournalEntry entry;
    entry.id_ = "previous-1";
    entry.outcome_ = runner::JournalOutcome::PENDING;
    entry.recorded_at_ = program_start();
    entry.instrument_ = samsung();
    entry.side_ = OrderSide::BUY;
    entry.quantity_ = Decimal64::parse("1");
    entry.price_ = Decimal64::parse("70000");
    REQUIRE(previous.append(entry));
  }
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::CLEAN);
  CHECK(out.str().find("recovery: NOT clean") != std::string::npos);
  CHECK(out.str().find("withheld until the broker's lists resolve it: 005930") != std::string::npos);
  // The share it might already have bought is not bought again.
  CHECK(session.placed().empty());
}

TEST_CASE("the live program refuses a journal that belongs to another account", "[live][program]") {
  const TempPath journal("atl-test-program-scope.jsonl");
  const TempPath control("atl-test-program-scope.stop");
  {
    runner::FileJournal other(journal.str(), "fake:live:2");
    REQUIRE(other.open());
  }
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::JOURNAL_REFUSED);
  CHECK(out.str().find("journal refused") != std::string::npos);
  CHECK(session.placed().empty());
  CHECK(session.holdings_calls() == 0);
}

TEST_CASE("the live program starts nothing without reference data", "[live][program]") {
  const TempPath journal("atl-test-program-reference.jsonl");
  const TempPath control("atl-test-program-reference.stop");
  ProgramSession session;
  session.set_bars(two_bars("70000", "71000"));
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  CHECK(exit == live_program::Exit::REFERENCE_UNREADABLE);
  CHECK(session.placed().empty());
  CHECK(session.holdings_calls() == 0);
}

TEST_CASE("the live program's observer sends nothing and opens no journal", "[live][program]") {
  const TempPath journal("atl-test-program-observe.jsonl");
  const TempPath control("atl-test-program-observe.stop");
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::observe(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::CLEAN);
  CHECK(session.placed().empty());
  CHECK_FALSE(file_exists(journal.str()));
  CHECK(out.str().find("observing: nothing can be sent") != std::string::npos);
  // The observer's report does not claim a close it could not have made.
  CHECK(out.str().find("closing complete") == std::string::npos);
  CHECK(out.str().find("simulated") != std::string::npos);
}

TEST_CASE("the live program stops by its control file and reports the closing pass", "[live][program]") {
  const TempPath journal("atl-test-program-control.jsonl");
  const TempPath control("atl-test-program-control.stop");
  write_text(control.str(), "withdraw\n");
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(out.str().find("stopped by      : withdraw") != std::string::npos);
  // Written before the program started, so the stop is pending at its first iteration. It reads the
  // broker's order list first -- which it used to skip, so it could never be complete and the program
  // always exited `CLOSING_INCOMPLETE` -- finds nothing resting, and exits clean.
  CHECK(out.str().find("order list read : yes") != std::string::npos);
  CHECK(out.str().find("closing complete: yes") != std::string::npos);
  CHECK(exit == live_program::Exit::CLEAN);
  CHECK(session.placed().empty());
}

TEST_CASE("the live program exits as abandoned when polling keeps failing", "[live][program]") {
  const TempPath journal("atl-test-program-abandon.jsonl");
  const TempPath control("atl-test-program-abandon.stop");
  ProgramSession session;
  script_ready(session);
  session.fail_with(ErrorCategory::TRANSPORT);
  auto settings = program_settings(journal, control);
  settings.max_iterations_ = 200;
  runner::StopSource stop;
  std::ostringstream out;

  const auto exit = live_program::trade(
    session, one_share(), settings, stop, out, runner::ManualClock{program_start()}
  );

  INFO(out.str());
  CHECK(exit == live_program::Exit::LOOP_ABANDONED);
  CHECK(out.str().find("ABANDONED") != std::string::npos);
  CHECK(session.placed().empty());
}

// ---------------------------------------------------------------------------
// `on_fill` on a live runner: reported, never refused.
// ---------------------------------------------------------------------------

namespace {

/** A strategy that sets its protection from `on_fill`: correct in a replay, inert live. */
struct StopsOnFill {
  std::size_t fills_{0};

  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}

  template <class Ctx>
  void on_fill(Ctx&, const strategy::Fill&) {
    ++fills_;
  }
};

/** The same strategy without the handler. */
struct NoFillHandler {
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}
};

using OnFillSession = FakeSession<WithWorkingOrders>;
using OnFillLive = live::BoundRunner<StopsOnFill, OnFillSession>;
using OnFillObserver = live::ObservingRunner<StopsOnFill, OnFillSession>;
using OnFillJournaled = live::JournaledRunner<StopsOnFill, OnFillSession, runner::FileJournal>;
using NoHandlerLive = live::BoundRunner<NoFillHandler, OnFillSession>;

static_assert(OnFillLive::on_fill_never_fires, "a session-backed runner never fires on_fill");
static_assert(OnFillJournaled::on_fill_never_fires, "journaling does not change who receives fills");
static_assert(!OnFillObserver::on_fill_never_fires, "an observer sends nothing, so it stays silent");
static_assert(!NoHandlerLive::on_fill_never_fires, "no handler, nothing to report");
static_assert(
  !runner::DryRunner<StopsOnFill>::on_fill_never_fires, "a dry run sends nothing, so it stays silent"
);
static_assert(
  !backtest::Backtester<StopsOnFill>::Runner::on_fill_never_fires, "a replay fires on_fill itself"
);

/** Counts how many of `notes` carry the on_fill note. */
std::size_t on_fill_notes(const std::vector<std::string>& notes) {
  std::size_t count = 0;
  for (const auto& note : notes) {
    if (note.find("strategy defines on_fill; it never fires on this runner") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

/** Runs `bound` for three iterations over a session that answers every poll, from `start`. */
template <typename Bound>
runner::LoopReport run_three(
  Bound& bound,
  OnFillSession& session,
  std::chrono::system_clock::time_point start = {}
) {
  live::SessionSource<OnFillSession> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<Bound, live::SessionSource<OnFillSession>, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{start}
  );
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
  runner::StopSource stop;
  return loop.run(stop, 3);
}

} // namespace

TEST_CASE("a live run reports once that the strategy's on_fill never fires", "[live][on_fill]") {
  OnFillSession session;
  session.set_bars(two_bars("70000", "71000"));
  auto bound = live::bind(session, StopsOnFill{});

  const auto report = run_three(bound, session);

  CHECK(report.iterations_ == 3);
  CHECK(on_fill_notes(report.notes_) == 1);
  // And again on the next run: a report is read on its own.
  // Later than the first run, as a restarted loop over the same runner would be: a later clock, and
  // bars the runner has not seen (a new source would otherwise feed the first two again).
  const auto later = std::chrono::system_clock::time_point{} + std::chrono::hours(1);
  auto newer = two_bars("72000", "73000");
  for (auto& bar : newer.bars_) {
    bar.timestamp_ += std::chrono::hours(24 * 10);
  }
  session.set_bars(newer);
  CHECK(on_fill_notes(run_three(bound, session, later).notes_) == 1);
  CHECK(bound.strategy().fills_ == 0);
}

TEST_CASE("an observing run and a strategy without on_fill say nothing about it", "[live][on_fill]") {
  OnFillSession session;
  session.set_bars(two_bars("70000", "71000"));

  auto observer = live::bind_observer(session, StopsOnFill{});
  CHECK(on_fill_notes(run_three(observer, session).notes_) == 0);

  auto plain = live::bind(session, NoFillHandler{});
  CHECK(on_fill_notes(run_three(plain, session).notes_) == 0);
}

TEST_CASE("the live program says on_fill never fires before it trades", "[live][on_fill][program]") {
  const TempPath journal("atl-test-program-on-fill.jsonl");
  const TempPath control("atl-test-program-on-fill.stop");
  ProgramSession session;
  script_ready(session);
  runner::StopSource stop;
  std::ostringstream out;

  live_program::trade(
    session, StopsOnFill{}, program_settings(journal, control), stop, out, runner::ManualClock{program_start()}
  );

  const auto text = out.str();
  const auto before = text.find("note: this strategy defines on_fill");
  const auto loop_starts = text.find("to stop:");
  REQUIRE(before != std::string::npos);
  REQUIRE(loop_starts != std::string::npos);
  CHECK(before < loop_starts);
  CHECK(text.find("strategy defines on_fill; it never fires on this runner") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The order a loop reads the account in: the listing, then holdings, then anything that can send.
// ---------------------------------------------------------------------------

namespace {

/**
 * A resting buy fills right after the iteration's holdings read has answered.
 *
 * The fill is complete: the listing loses the row **and** holdings would show the shares -- to the
 * next read. What the loop does between the two reads is the subject.
 */
template <typename Session>
class FillAfterHoldings {
public:
  static constexpr bool provides_working_orders = true;

  FillAfterHoldings(Session& session, std::vector<InstrumentId> universe, HoldingsSnapshot filled)
  : inner_(session, std::move(universe)), session_(&session), filled_(std::move(filled)) {}

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    polled_.push_back(kind);
    const auto outcome = inner_.poll(kind, runner_instance);
    if (kind == runner::PollKind::HOLDINGS && !done_) {
      session_->set_working_orders(WorkingOrders<FakeBroker>{});
      session_->set_holdings(filled_);
      done_ = true;
    }
    return outcome;
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    return inner_.read_order_list(runner_instance);
  }

  const std::vector<runner::PollKind>& polled() const noexcept { return polled_; }

private:
  live::SessionSource<Session> inner_;
  Session* session_;
  HoldingsSnapshot filled_;
  std::vector<runner::PollKind> polled_;
  bool done_{false};
};

/** Holdings of `quantity` Samsung shares, stamped at the program tests' start. */
HoldingsSnapshot holding_samsung(const char* quantity) {
  HoldingsSnapshot snapshot;
  snapshot.received_at_ = program_start();
  if (std::string_view(quantity) != "0") {
    HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = Decimal64::parse(quantity);
    snapshot.positions_.push_back(position);
  }
  return snapshot;
}

/** Runs `iterations` of a loop over `source` with every kind due each second, and a ten-share target. */
template <typename Source>
runner::LoopReport run_ten_share_target(ProgramSession& session, Source& source, std::size_t iterations) {
  strategy::vanilla::BuyAndHold ten;
  ten.params_.instrument_ = samsung();
  ten.params_.quantity_ = Decimal64::parse("10");
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  auto bound = live::bind(session, ten, any_phase);
  bound.context().set_instrument(samsung_reference());
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), Source, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{program_start()}
  );
  loop.set_maximum_portfolio_age(std::chrono::minutes(1));
  runner::StopSource stop;
  return loop.run(stop, iterations);
}

} // namespace

TEST_CASE("a fill between two account reads does not buy again", "[live][poll-order]") {
  // The double buy this ordering exists to prevent. A resting buy of ten is the whole target; it fills
  // right after holdings answered flat. Read holdings-then-listing, the listing would show it gone
  // beside flat holdings, and the next bar would buy ten more.
  ProgramSession session;
  script_ready(session);
  session.set_holdings(holding_samsung("0"));
  WorkingOrders<FakeBroker> resting;
  resting.orders_.push_back(working());
  session.set_working_orders(resting);
  FillAfterHoldings<ProgramSession> source(session, {samsung()}, holding_samsung("10"));

  const auto report = run_ten_share_target(session, source, 3);

  INFO("plans withheld " << report.plans_withheld_);
  CHECK(session.placed().empty());
}

TEST_CASE("a loop reads the listing, then holdings, before anything that can send", "[live][poll-order]") {
  ProgramSession session;
  script_ready(session);
  FillAfterHoldings<ProgramSession> source(session, {samsung()}, holding_samsung("0"));

  run_ten_share_target(session, source, 1);

  const std::vector<runner::PollKind> expected{
    runner::PollKind::WORKING_ORDERS, runner::PollKind::HOLDINGS, runner::PollKind::BARS,
  };
  CHECK(source.polled() == expected);
}

namespace {

/** Restates a ten-share target whenever the broker reports a change to one of its orders. */
struct RestatesTenOnUpdate {
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot&) {
    strategy::TargetPosition ten;
    ten.instrument_ = samsung();
    ten.target_quantity_ = Decimal64::parse("10");
    ctx.submit(ten);
  }
};

/** Answers an order update with a fixed order rather than a target. */
struct BuysOneOnUpdate {
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot&) {
    ctx.submit(
      strategy::OrderIntent::market(samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")))
    );
  }
};

/** Wants ten shares on every bar. */
struct TenOnEveryBar {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    strategy::TargetPosition ten;
    ten.instrument_ = samsung();
    ten.target_quantity_ = Decimal64::parse("10");
    ctx.submit(ten);
  }
};

/** Binds `strategy` with no phase restriction and a reference and price for Samsung. */
template <typename Strategy>
auto bind_for_poll_order(WorkingSession& session, Strategy strategy_instance) {
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  auto bound = live::bind(session, std::move(strategy_instance), any_phase);
  bound.context().set_instrument(samsung_reference());
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  return bound;
}

} // namespace

TEST_CASE("an update's target is planned against the holdings read after its listing", "[live][poll-order]") {
  // A buy of ten rests; holdings read flat after it; five fill before the next listing, which the loop
  // reads before holdings. Planned there, the handler's target of ten would read 0 held and 5 working
  // and buy 5 more. It is withheld, retained, and planned against the next holdings read instead.
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  const auto withheld_before = bound.plans_withheld();

  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);

  CHECK(session.placed().empty());
  CHECK(bound.plans_withheld() == withheld_before + 1);
  CHECK(bound.targets_retained() == 1);

  poll_holdings_at(source, bound, session, "5", 3);
  // What `PollingLoop` does after an iteration's polls.
  bound.retry_retained_targets();

  // Five held and five still working is the target: nothing to place, nothing left retained.
  CHECK(session.placed().empty());
  CHECK(bound.targets_retained() == 0);
}

TEST_CASE("a bar after a listing with no newer holdings waits for the next holdings read", "[live][poll-order]") {
  // Holdings read flat; the resting buy fills and the next listing no longer shows it; the holdings
  // read that should follow fails. A bar then must not buy against the flat snapshot.
  WorkingSession session;
  auto bound = bind_for_poll_order(session, TenOnEveryBar{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  poll_listing_at(source, bound, session, {}, 2);
  const auto withheld_before = bound.plans_withheld();

  const auto series = two_bars("70000", "71000");
  bound.feed(strategy::make_bar_event(samsung(), series.interval_, series.stamp_, series.bars_.back()));

  CHECK(session.placed().empty());
  CHECK(bound.plans_withheld() == withheld_before + 1);
  CHECK(bound.targets_retained() == 1);

  // The next holdings read shows the fill, and the retained target finds nothing to do.
  poll_holdings_at(source, bound, session, "10", 3);
  bound.retry_retained_targets();
  CHECK(session.placed().empty());
  CHECK(bound.targets_retained() == 0);
}

TEST_CASE("an order intent stated against a superseded account is withheld and not replayed", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, BuysOneOnUpdate{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  const auto withheld_before = bound.plans_withheld();

  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);

  CHECK(session.placed().empty());
  CHECK(bound.plans_withheld() == withheld_before + 1);
  // A fixed quantity decided against the wrong picture is not replayed later.
  CHECK(bound.targets_retained() == 0);
  poll_holdings_at(source, bound, session, "5", 3);
  bound.retry_retained_targets();
  CHECK(session.placed().empty());
}

namespace {

/**
 * Fails the second holdings read and asks for a flatten at that moment, after a listing newer than the
 * last successful holdings.
 */
class FlattenAfterFailedHoldings {
public:
  static constexpr bool provides_working_orders = true;

  FlattenAfterFailedHoldings(WorkingSession& session, runner::StopSource& stop)
  : inner_(session, {samsung()}), session_(&session), stop_(&stop) {}

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    if (kind == runner::PollKind::WORKING_ORDERS) {
      ++listings_;
      // Each listing arrives an hour after the last, so the second is newer than the first holdings.
      WorkingOrders<FakeBroker> listing;
      listing.received_at_ = live_hour(listings_ * 2 - 1);
      session_->set_working_orders(listing);
    }
    if (kind == runner::PollKind::HOLDINGS && listings_ >= 2) {
      stop_->request(runner::StopIntent::FLATTEN);
      return runner::PollOutcome::FAILED;
    }
    return inner_.poll(kind, runner_instance);
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    return inner_.read_order_list(runner_instance);
  }

private:
  live::SessionSource<WorkingSession> inner_;
  WorkingSession* session_;
  runner::StopSource* stop_;
  int listings_{0};
};

} // namespace

TEST_CASE("a flatten after a listing newer than the holdings does not report a verified close", "[live][poll-order]") {
  WorkingSession session;
  HoldingsSnapshot held;
  held.received_at_ = live_hour(2);
  session.set_holdings(held);
  auto bound = bind_for_poll_order(session, TenOnEveryBar{});
  runner::StopSource stop;
  FlattenAfterFailedHoldings source(session, stop);
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), FlattenAfterFailedHoldings, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{live_hour(3)}
  );
  loop.allow_unbounded_portfolio_age();

  const auto report = loop.run(stop, 4);

  REQUIRE(report.stopped_by_ == runner::StopIntent::FLATTEN);
  // The listing at hour 3 is newer than the holdings from hour 2, and a fill between them would be in
  // neither: the closing pass could not know what it owed.
  CHECK(report.closing_source_unverified_);
  CHECK_FALSE(report.closing_complete());
}

namespace {

/** Restates ten on an update, and answers a bar with a fixed order of one. */
struct TargetOnUpdateOrderOnBar {
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot&) {
    strategy::TargetPosition ten;
    ten.instrument_ = samsung();
    ten.target_quantity_ = Decimal64::parse("10");
    ctx.submit(ten);
  }

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    ctx.submit(
      strategy::OrderIntent::market(samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("1")))
    );
  }
};

/** Changes the listing after the startup read, so the first scheduled listing carries an update. */
class ListingMovesAfterStartup {
public:
  static constexpr bool provides_working_orders = true;

  explicit ListingMovesAfterStartup(WorkingSession& session) : inner_(session, {samsung()}), session_(&session) {}

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    return inner_.poll(kind, runner_instance);
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    const auto outcome = inner_.read_order_list(runner_instance);
    // Five of the resting ten fill between the startup read and the first scheduled listing.
    WorkingOrders<FakeBroker> moved;
    moved.orders_.push_back(update_row("B-1", "", "5"));
    moved.received_at_ = live_hour(1);
    session_->set_working_orders(moved);
    return outcome;
  }

private:
  live::SessionSource<WorkingSession> inner_;
  WorkingSession* session_;
};

} // namespace

TEST_CASE("an update's target withheld for an unread account at startup is planned after holdings", "[live][poll-order]") {
  // The first scheduled listing runs before the first holdings read, so a target stated from its update
  // meets an account never read. It is retained and planned after the iteration's polls -- here against
  // holdings that (by this fake's script) show nothing held, so five of the ten are still wanted.
  WorkingSession session;
  WorkingOrders<FakeBroker> startup;
  startup.orders_.push_back(update_row("B-1", "", "0"));
  startup.received_at_ = live_hour(0);
  session.set_working_orders(startup);
  HoldingsSnapshot flat;
  flat.received_at_ = live_hour(2);
  session.set_holdings(flat);
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  ListingMovesAfterStartup source(session);
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), ListingMovesAfterStartup, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{live_hour(2)}
  );
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;

  const auto report = loop.run(stop, 1);

  CHECK(report.plans_withheld_ >= 1);
  CHECK(bound.targets_retained() == 0);
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed().front().quantity_ == Decimal64::parse("5"));
}

TEST_CASE("a later order decision replaces a retained target", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, TargetOnUpdateOrderOnBar{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);
  REQUIRE(bound.targets_retained() == 1);

  // The strategy decides again about the instrument -- with an order, which is itself withheld and
  // dropped. The earlier target must not come back after it.
  const auto series = two_bars("70000", "71000");
  bound.feed(strategy::make_bar_event(samsung(), series.interval_, series.stamp_, series.bars_.back()));
  CHECK(bound.targets_retained() == 0);

  poll_holdings_at(source, bound, session, "0", 3);
  bound.retry_retained_targets();
  CHECK(session.placed().empty());
}

TEST_CASE("nothing stated under an operator's halt is retained", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  bound.guard().halt(strategy::HaltReason::OPERATOR);

  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);
  CHECK(bound.targets_retained() == 0);

  bound.guard().resume(strategy::HaltReason::OPERATOR);
  poll_holdings_at(source, bound, session, "0", 3);
  bound.retry_retained_targets();
  CHECK(session.placed().empty());
}

TEST_CASE("a deferred release that throws does not leave supersession disabled", "[live][poll-order]") {
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);
  record_stop(session, OrderStatus::CANCELED, "0");
  session.throw_on_place();

  WorkingOrders<FakeBroker> empty;
  empty.received_at_ = live_hour(1);
  session.set_working_orders(empty);
  CHECK_THROWS(source.poll(runner::PollKind::WORKING_ORDERS, bound));

  // The listing (hour 1) is newer than the holdings (hour 0). With the exemption stuck on, the next
  // flatten would sell against that picture; it must be withheld instead.
  session.throw_on_place(false);
  const auto withheld_before = bound.plans_withheld();
  bound.feed(strategy::MarketPhase::POST_CLOSE);
  CHECK(bound.plans_withheld() == withheld_before + 1);
  CHECK(session.placed().empty());
}

TEST_CASE("a retained target waits while a hold is in force, then is planned", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);
  REQUIRE(bound.targets_retained() == 1);

  poll_holdings_at(source, bound, session, "0", 3);
  bound.guard().halt(strategy::HaltReason::OPERATOR);
  bound.retry_retained_targets();
  // Not drained into a refusal: still retained, nothing sent.
  CHECK(bound.targets_retained() == 1);
  CHECK(session.placed().empty());

  bound.guard().resume(strategy::HaltReason::OPERATOR);
  bound.retry_retained_targets();
  CHECK(bound.targets_retained() == 0);
  // By this fake's script nothing is held and five still work, so five of the ten are placed.
  REQUIRE(session.placed().size() == 1);
  CHECK(session.placed().front().quantity_ == Decimal64::parse("5"));
}

TEST_CASE("a stop discards retained targets", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);
  REQUIRE(bound.targets_retained() == 1);

  runner::PollSchedule schedule;
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), live::SessionSource<WorkingSession>, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{live_hour(3)}
  );
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;
  stop.request(runner::StopIntent::QUIESCE);
  loop.run(stop, 1);

  CHECK(bound.targets_retained() == 0);
}

namespace {

/** Wants ten on an order update; wants nothing once a phase is announced. */
struct EntryOnUpdateFlatOnPhase {
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OrderSnapshot&) {
    strategy::TargetPosition ten;
    ten.instrument_ = samsung();
    ten.target_quantity_ = Decimal64::parse("10");
    ctx.submit(ten);
  }

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase) {
    strategy::TargetPosition flat;
    flat.instrument_ = samsung();
    flat.target_quantity_ = Decimal64::parse("0");
    ctx.submit(flat);
  }
};

/** Requests a stop from inside the holdings poll, after the iteration's start. */
class StopsDuringHoldings {
public:
  static constexpr bool provides_working_orders = true;

  StopsDuringHoldings(WorkingSession& session, runner::StopSource& stop)
  : inner_(session, {samsung()}), stop_(&stop) {}

  template <typename Runner>
  runner::PollOutcome poll(runner::PollKind kind, Runner& runner_instance) {
    const auto outcome = inner_.poll(kind, runner_instance);
    if (kind == runner::PollKind::HOLDINGS) {
      stop_->request(runner::StopIntent::QUIESCE);
    }
    return outcome;
  }

  template <typename Runner>
  runner::PollOutcome read_order_list(Runner& runner_instance) {
    return inner_.read_order_list(runner_instance);
  }

private:
  live::SessionSource<WorkingSession> inner_;
  runner::StopSource* stop_;
};

/** Leaves `bound` with one retained ten-share target on Samsung, as the update tests do. */
template <typename Bound>
void retain_one_target(Bound& bound, WorkingSession& session) {
  live::SessionSource<WorkingSession> source(session, {samsung()});
  poll_listing_at(source, bound, session, {update_row("B-1", "", "0")}, 0);
  poll_holdings_at(source, bound, session, "0", 1);
  poll_listing_at(source, bound, session, {update_row("B-1", "", "5")}, 2);
  REQUIRE(bound.targets_retained() == 1);
}

} // namespace

TEST_CASE("a retained entry does not trade before the phase decision that follows it", "[live][poll-order]") {
  // A retained entry of ten, then an iteration whose holdings read is good and whose phase changes.
  // `on_phase` wants flat. Retried before the announcement, the entry would buy five and the flat
  // target would arrive too late to stop it.
  WorkingSession session;
  auto bound = bind_for_poll_order(session, EntryOnUpdateFlatOnPhase{});
  retain_one_target(bound, session);

  const auto start = live_hour(3);
  bound.context().set_calendar(one_day_calendar(strategy::SessionWindow::krx().local_date(start)));
  HoldingsSnapshot flat;
  flat.received_at_ = live_hour(4);
  session.set_holdings(flat);
  live::SessionSource<WorkingSession> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), live::SessionSource<WorkingSession>, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{start}
  );
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;

  const auto report = loop.run(stop, 1);

  REQUIRE(report.phase_transitions_ >= 1);
  CHECK(session.placed().empty());
  CHECK(bound.targets_retained() == 0);
}

TEST_CASE("a stop requested during a poll discards retained targets", "[live][poll-order]") {
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  retain_one_target(bound, session);

  runner::StopSource stop;
  StopsDuringHoldings source(session, stop);
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), StopsDuringHoldings, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{live_hour(3)}
  );
  loop.allow_unbounded_portfolio_age();

  const auto report = loop.run(stop, 2);

  REQUIRE(report.stopped_by_ == runner::StopIntent::QUIESCE);
  CHECK(bound.targets_retained() == 0);
  CHECK(session.placed().empty());
}

TEST_CASE("an abandoned run discards retained targets", "[live][poll-order]") {
  // Retained work lives for one run: a run given up after repeated poll failures, then resumed by hand,
  // must not place a target decided before an outage of unknown length.
  WorkingSession session;
  auto bound = bind_for_poll_order(session, RestatesTenOnUpdate{});
  retain_one_target(bound, session);

  session.fail_with(ErrorCategory::TRANSPORT);
  live::SessionSource<WorkingSession> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<decltype(bound), live::SessionSource<WorkingSession>, runner::ManualClock> loop(
    bound, source, schedule, runner::ManualClock{live_hour(3)}
  );
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;

  const auto report = loop.run(stop, 200);

  REQUIRE(report.abandoned_);
  CHECK(bound.targets_retained() == 0);
  CHECK(session.placed().empty());
}

namespace {

/** Wants half the account in Samsung, which a live runner, holding no portfolio value, cannot size. */
struct WantsHalf {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(
        bar.instrument_, OrderSide::BUY, strategy::SizeSpec::target_weight(Decimal64::parse("0.5"))
      )
    );
  }
};

} // namespace

TEST_CASE("a live run's report says why reconciliation could not build an instruction") {
  // Lab: `EqualWeightRebalance` through `live_program` reported "not buildable: 80" and nothing else,
  // because the source discards each pass's report and its reasons with it. The run now carries the
  // last one, as it carries the broker's last refusal.
  using LoopRunner = runner::Runner<
    WantsHalf, WithKey, FakeBroker, live::SessionExecutor<FakeSession<WithKey>>
  >;
  FakeSession<WithKey> session;
  session.set_bars(two_bars("70000", "71000"));
  strategy::RiskLimits any_phase;
  any_phase.permitted_phases_ = std::nullopt;
  LoopRunner live_runner(
    WantsHalf{}, any_phase, 512, live::SessionExecutor<FakeSession<WithKey>>(session)
  );
  live_runner.context().set_instrument(samsung_reference());
  live_runner.context().set_last_price(samsung(), Decimal64::parse("70000"));
  live_runner.context().set_portfolio(held_position<WithKey>("0", nullptr));

  live::SessionSource<FakeSession<WithKey>> source(session, {samsung()});
  runner::PollSchedule schedule;
  schedule.bars_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  schedule.calendar_.reset();
  runner::PollingLoop<LoopRunner, live::SessionSource<FakeSession<WithKey>>, runner::ManualClock>
    loop(live_runner, source, schedule, runner::ManualClock{});
  loop.allow_unbounded_portfolio_age();
  loop.allow_absent_portfolio();
  runner::StopSource stop;

  const auto report = loop.run(stop, 1);

  CHECK(report.orders_unbuildable_ == 1);
  REQUIRE(report.last_unbuildable_.has_value());
  CHECK(report.last_unbuildable_->reason_ == strategy::RejectReason::NO_PORTFOLIO_VALUE);
  CHECK(report.last_unbuildable_->instrument_ == samsung());
  CHECK(session.placed().empty());

  // A run that refuses nothing reports nothing, although the runner still holds the record.
  session.set_bars({});
  const auto quiet = loop.run(stop, 1);
  CHECK(quiet.orders_unbuildable_ == 0);
  CHECK_FALSE(quiet.last_unbuildable_.has_value());
  CHECK(live_runner.last_unbuildable().has_value());
}

TEST_CASE("a file journal reads back a resolution a startup sweep wrote", "[recovery][file]") {
  // F29 through the reader that failed: a `RESOLVED` line stamped at the epoch was damage to
  // `FileJournal`, so the next open counted it as a line this library did not write and recovery halted.
  const TempPath path("atl-test-startup-sweep.jsonl");
  const auto sent = std::chrono::system_clock::now() - std::chrono::minutes(5);
  {
    runner::FileJournal previous(path.str(), "fake:demo:1");
    REQUIRE(previous.open());
    auto entry = journaled_placement();
    entry.recorded_at_ = sent;
    REQUIRE(previous.append(entry));
  }

  WorkingSession session;
  WorkingOrders<FakeBroker> orders;
  auto listed = working();
  listed.order_ = listed_order("KRX-1");
  listed.order_.ordered_at_ = sent;
  orders.orders_.push_back(listed);
  session.set_working_orders(orders);

  {
    runner::FileJournal journal(path.str(), "fake:demo:1");
    REQUIRE(journal.open());
    auto live_runner = live::bind_observer(session, CountEvents{});
    const auto recovery = runner::recover_runner(live_runner, journal);
    REQUIRE_FALSE(recovery.clean());
    live_runner.set_observed_at(std::chrono::system_clock::now());
    live::SessionSource<WorkingSession, runner::FileJournal> source(session, {samsung()});
    source.set_journal(journal);
    CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
    CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
  }

  // The next start: every line readable, nothing owed.
  runner::FileJournal next(path.str(), "fake:demo:1");
  REQUIRE(next.open());
  const auto replay = next.replay();
  CHECK(replay.unreadable_lines_ == 0);
  CHECK(runner::unresolved_instructions(replay).empty());
  auto restarted = live::bind_observer(session, CountEvents{});
  CHECK(runner::recover_runner(restarted, next).clean());
}
