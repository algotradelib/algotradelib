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
struct FakeBroker {};

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
    if (cancel_error_) {
      return Result<Placed>(*cancel_error_);
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

  /** Makes every later **withdrawal** fail with `error`, as a broker refuses to cancel a finished order. */
  void fail_cancel_with(BrokerError error) { cancel_error_ = std::move(error); }

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
  std::optional<BrokerError> cancel_error_;
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

/**
 * States each position's whole quantity as sellable, as a broker's holdings rows do: the guard checks a sell
 * against the figure by default, so a fixture standing for an answered holdings read has to carry one.
 */
void state_sellable(HoldingsSnapshot& snapshot) {
  snapshot.sellable_basis_ = SellableBasis{
    SellableBasis::Coverage::WHOLE_HOLDING, SellableBasis::RestingTreatment::DEDUCT, false,
  };
  for (auto& position : snapshot.positions_) {
    position.sellable_quantity_ = position.quantity_;
  }
}

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
  // Sellable as held, as a broker's holdings row states it (the guard checks a sell against it by default).
  auto portfolio = held_position<WithWorkingOrders>(held, held);
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

TEST_CASE("an order that goes out between bars is counted in the rate window when it goes out", "[confirm][rate]") {
  // The decision instant moves with bars; a loop's reading moves every iteration. A target the runner
  // kept and released at a later read, or one whose mark a sweep settled between bars, is sent with the
  // decision instant still at the last bar. Recorded there, the send already looked a window old to the
  // next bar, which then sent a second order inside the limit.
  FakeSession<WithWorkingOrders> session;
  std::deque<std::variant<strategy::TargetPosition, strategy::OrderIntent>> script;
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  limits.rate_window_ = std::chrono::seconds(60);
  ScriptedRunner runner_instance{
    Scripted{&script}, limits, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  };
  runner_instance.set_confirmation_timeout(std::chrono::hours(24));
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), Decimal64::parse("70000"));
  install(runner_instance, polled(0, "0", {}));

  // A bar that decides nothing, then two minutes with no bar: the loop's reading alone has moved.
  runner_instance.feed(bar_at(1, "70000"));
  const auto decided = runner_instance.context().now();
  const auto sent_at = decided + std::chrono::seconds(120);
  runner_instance.set_observed_at(sent_at);
  runner_instance.context().submit(target_of("10"));
  runner_instance.settle_closing();
  REQUIRE(session.placed().size() == 1);
  REQUIRE(runner_instance.context().now() == decided);
  CHECK(runner_instance.guard().recent_orders(sent_at) == 1);

  // A second later the next decision arrives. One order a minute: it is refused.
  runner_instance.context().set_now(sent_at + std::chrono::seconds(1));
  runner_instance.set_observed_at(sent_at + std::chrono::seconds(1));
  runner_instance.context().submit(target_of("20"));
  const auto report = runner_instance.settle_closing();
  CHECK(session.placed().size() == 1);
  REQUIRE(report.refused_.size() == 1);
  CHECK(report.refused_.front().reason_ == strategy::GuardReason::ORDER_RATE_EXCEEDED);
}

TEST_CASE("with no loop reading, the rate window runs on decision time, before the epoch too", "[confirm][rate]") {
  // A replay gives the runner no reading of any clock, and its history may predate 1970. An unset
  // reading is the epoch: taken for a real one, it filed every earlier send at zero and refused the
  // rest of the history.
  FakeSession<WithWorkingOrders> session;
  std::deque<std::variant<strategy::TargetPosition, strategy::OrderIntent>> script;
  strategy::RiskLimits limits;
  limits.max_orders_per_window_ = 1;
  limits.rate_window_ = std::chrono::seconds(60);
  ScriptedRunner runner_instance{
    Scripted{&script}, limits, 512, live::SessionExecutor<FakeSession<WithWorkingOrders>>(session)
  };
  runner_instance.set_confirmation_timeout(std::chrono::hours(24));
  runner_instance.context().set_instrument(samsung_reference());
  runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
  runner_instance.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = polled(0, "0", {});
  const auto epoch = std::chrono::system_clock::time_point{};
  portfolio.as_of_ = epoch - std::chrono::seconds(300);
  portfolio.working_as_of_ = portfolio.as_of_;
  install(runner_instance, portfolio);

  runner_instance.context().set_now(epoch - std::chrono::seconds(120));
  runner_instance.context().submit(target_of("10"));
  runner_instance.settle_closing();
  REQUIRE(session.placed().size() == 1);
  CHECK(runner_instance.guard().recent_orders(epoch - std::chrono::seconds(100)) == 1);

  // Sixty-one seconds later, still before the epoch: the first has left the window.
  runner_instance.context().set_now(epoch - std::chrono::seconds(59));
  runner_instance.context().submit(target_of("20"));
  const auto report = runner_instance.settle_closing();
  CHECK(report.refused_.empty());
  CHECK(session.placed().size() == 2);
}

TEST_CASE("a withdrawal that goes out between bars is counted in the rate window when it goes out", "[confirm][rate]") {
  // Accepted, refused or of unknown outcome, it spent a request then, not at the last bar.
  for (const int outcome : {0, 1, 2}) {
    ConfirmRig rig;
    const auto resting = listed_row("X", OrderSide::BUY, "10", "0", OrderType::LIMIT);
    install(rig.runner_instance, polled(0, "0", {resting}));
    rig.runner_instance.feed(bar_at(1, "70000"));
    const auto decided = rig.runner_instance.context().now();
    const auto sent_at = decided + std::chrono::seconds(120);
    rig.runner_instance.set_observed_at(sent_at);
    if (outcome == 1) {
      BrokerError refused;
      refused.category_ = ErrorCategory::INVALID_ARGUMENT;
      refused.reached_broker_ = true;
      refused.message_ = "the order has already been executed";
      rig.session.fail_cancel_with(refused);
    } else if (outcome == 2) {
      BrokerError lost;
      lost.category_ = ErrorCategory::TRANSPORT;
      rig.session.fail_cancel_with(lost);
    }
    rig.runner_instance.context().cancel(resting.ref_, "between bars");
    rig.runner_instance.settle_closing();
    CAPTURE(outcome);
    REQUIRE(rig.runner_instance.context().now() == decided);
    CHECK(rig.runner_instance.guard().recent_orders(sent_at + std::chrono::seconds(61)) == 0);
    CHECK(rig.runner_instance.guard().recent_orders(sent_at) == 1);
    CHECK(rig.runner_instance.guard().recent_orders(decided + std::chrono::seconds(170)) == 1);
  }
}

TEST_CASE("a filled market order no listing shows stays counted until holdings show it", "[confirm]") {
  // A market order that filled at once is in no listing, and holdings lag the fill. The
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

  // Order numbers repeat across days, and the history is read from the day before. Yesterday's order
  // under today's number, cancelled with nothing filled, used to settle today's filled order at zero.
  const auto sent = bar_at(1, "70000").bar_.timestamp_;
  auto yesterday = ended_row("0001569139", OrderSide::BUY, "10", "0");
  yesterday.ordered_at_ = sent - std::chrono::hours(24);
  auto today = ended_row("0001569139", OrderSide::BUY, "10", "10");
  today.status_ = OrderStatus::FILLED;
  today.ordered_at_ = sent;

  SECTION("a same-numbered order from another day does not answer for it") {
    rig.runner_instance.confirm_from_history({yesterday, today});
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));
    rig.runner_instance.feed(bar_at(3, "70000"));
    CHECK(rig.session.placed().size() == 1);

    rig.runner_instance.confirm_from_history({yesterday});
    CHECK(rig.working_buys() == Decimal64::parse("10"));
  }

  SECTION("undated rows that disagree answer nothing; undated rows that agree are one answer") {
    yesterday.ordered_at_.reset();
    today.ordered_at_.reset();
    rig.runner_instance.confirm_from_history({yesterday, today});
    CHECK(rig.runner_instance.unconfirmed_orders() == 1);
    CHECK(rig.working_buys() == Decimal64::parse("10"));

    rig.runner_instance.confirm_from_history({yesterday, yesterday});
    CHECK(rig.runner_instance.unconfirmed_orders() == 0);
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
  // The sweep once found the filled order in the listing and cleared the ledger; holdings lagged
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
  // Counted from the send, with no reference. While the ledger holds the question the row is not shown:
  // nothing can be placed on the instrument, and a listing may already carry the order itself.
  CHECK(rig.runner_instance.unconfirmed_orders() == 1);
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.working_buys() == Decimal64::parse("0"));

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

  // From the next pass the row is not shown while the ledger holds the question, and a target opposing
  // it withdraws nothing at the broker.
  rig.runner_instance.feed(bar_at(3, "70000"));
  CHECK(rig.session.canceled().empty());
  CHECK(rig.session.placed().size() == 2);
}

TEST_CASE("a resting order whose reply was lost is not counted twice beside its own listed row", "[confirm]") {
  // A listing carries the order before any sweep names it (a failed history read,
  // an ambiguous sweep, no journal). Its reserved row beside the broker's own made 20 working for a
  // target of 10, and the target withdrew the real order -- cancels are not held by the ledger.
  ConfirmRig rig;
  auto limit_target = target_of("10");
  limit_target.limit_ = strategy::Price::checked(Decimal64::parse("69000"), samsung_reference().price_rule_);
  rig.script.push_back(limit_target);
  rig.script.push_back(limit_target);
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.feed(bar_at(1, "70000"));
  rig.session.heal_place();
  REQUIRE(rig.session.placed().size() == 1);
  REQUIRE(rig.runner_instance.ledger().requires_reconcile(samsung()));

  auto resting = listed_row("0001569139", OrderSide::BUY, "10", "0", OrderType::LIMIT);
  resting.order_.price_ = Decimal64::parse("69000");
  install(rig.runner_instance, polled(2, "0", {resting}));
  rig.runner_instance.feed(bar_at(2, "70000"));
  CHECK(rig.session.canceled().empty());
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.working_buys() == Decimal64::parse("10"));
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
  CHECK(rig.working_buys() == Decimal64::parse("0"));
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

namespace {
/** What a broker answers when asked to withdraw an order that has already filled. */
BrokerError already_finished() {
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = true;
  error.broker_code_ = "cancel_refused";
  error.message_ = "the order has already been executed";
  return error;
}
} // namespace

TEST_CASE("a target does not re-place an order it only reserved while holdings lag", "[confirm]") {
  // A buy filled 7 of 10 (holdings show them), the signal turned (the 3 left withdrawn, 7 sold,
  // not yet shown), and turned back. Seven reserved on the buy side met a need of three, so the side was
  // withdrawn -- refused, the order was done -- and three sent beside it, every bar.
  ConfirmRig rig;
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(10));
  rig.session.order_ids_ = {"B", "S", "B2", "B3"};
  rig.script.push_back(target_of("10"));
  rig.runner_instance.feed(bar_at(1, "70000"));
  install(rig.runner_instance, polled(2, "7", {listed_row("B", OrderSide::BUY, "10", "7")}));
  rig.script.push_back(target_of("0"));
  rig.runner_instance.feed(bar_at(2, "70000"));
  REQUIRE(rig.session.placed().size() == 2);
  REQUIRE(rig.session.placed()[1].side_ == OrderSide::SELL);

  // From here a withdrawal is refused: both orders are finished. Holdings still show only the buy's 7.
  rig.session.fail_cancel_with(already_finished());
  for (int bar = 3; bar <= 5; ++bar) {
    install(rig.runner_instance, polled(bar, "7", {}));
    rig.script.push_back(target_of("10"));
    rig.runner_instance.feed(bar_at(bar, "70000"));
  }
  CHECK(rig.session.placed().size() == 2);
  CHECK(rig.session.canceled().size() > 1);  // still withdrawing, only not replacing
  // A refused withdrawal of a finished order neither halts the runner nor withholds the instrument
  // (the reservation plan's unwritten test 7).
  CHECK_FALSE(rig.runner_instance.local_failure().has_value());
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));

  // Holdings catch up (7 bought, 7 sold: none held). Unchanged holdings settle an opposite pair only once
  // each history has said what it filled -- for the sell, never listed, past the confirmation timeout.
  auto caught_up = polled(6, "0", {});
  rig.runner_instance.set_observed_at(caught_up.as_of_);
  install(rig.runner_instance, std::move(caught_up));
  auto sold = ended_row("S", OrderSide::SELL, "7", "7");
  sold.status_ = OrderStatus::FILLED;
  rig.runner_instance.confirm_from_history({ended_row("B", OrderSide::BUY, "10", "7"), sold});
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 0);
  rig.script.push_back(target_of("10"));
  rig.runner_instance.feed(bar_at(6, "70000"));
  REQUIRE(rig.session.placed().size() == 3);
  CHECK(rig.session.placed()[2].side_ == OrderSide::BUY);
  CHECK(rig.session.placed()[2].quantity_ == Decimal64::parse("10"));
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
  // A daily strategy's target, released at the open into a dropped connection, used to be
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
  live_runner.context().set_portfolio(held_position<WithKey>("10", "10"));
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
      state_sellable(snapshot);
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
  // -- and holdings do not show the fill yet. The order stays counted, so nothing is sent again (a
  // listing that retired it once let a target buy twice).
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
  live_runner.context().set_portfolio(held_position<WithKey>("10", "10"));

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
    // Not named: the resolution that would have named it was never written down, so the hold stands,
    // and the broker's own row is the only one shown for the order.
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    rig.runner_instance.refresh_unconfirmed();
    const auto& rows = rig.runner_instance.context().portfolio().working_;
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().ref_.primary_ == "KRX-1");
  }
}

TEST_CASE("a sweep before any bar stamps its resolution with the loop's clock", "[recovery]") {
  // Observed on a KIS demo account (2026-10-02): a process restarted after
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

TEST_CASE("a sweep counts time on the later of the decision instant and the loop's reading", "[confirm]") {
  // The decision instant moves with bars, quotes and phase changes only. On daily bars, or with a stop
  // pending, it stands still while the wall clock runs, and an order absent from every listing stayed
  // too recent to conclude from until the next bar.
  WorkingSession session;
  session.set_working_orders(WorkingOrders<FakeBroker>{});
  session.set_history(OrderPage{});

  const auto sweep = [&session](
                       std::chrono::system_clock::time_point decided, std::chrono::system_clock::time_point observed
                     ) {
    runner::MemoryJournal journal;
    journal.append(journaled_placement());
    auto live_runner = live::bind_observer(session, CountEvents{});
    live_runner.context().set_now(decided);
    live_runner.set_observed_at(observed);
    live_runner.ledger().mark(samsung(), "an order request failed in transit");
    live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
    source.set_journal(journal);
    source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
    REQUIRE(source.last_reconcile().matches_.size() == 1);
    return std::pair(source.last_reconcile().matches_.front().verdict_, live_runner.ledger().requires_reconcile(samsung()));
  };
  const auto just_after = journaled_at() + std::chrono::seconds(5);

  SECTION("the decision instant stood still and the loop's reading moved on: the absence is concluded") {
    const auto [verdict, marked] = sweep(just_after, swept_at());
    CHECK(verdict == runner::MatchVerdict::NOT_PLACED);
    CHECK_FALSE(marked);
  }

  SECTION("the decision instant is the later one: as before") {
    const auto [verdict, marked] = sweep(swept_at(), just_after);
    CHECK(verdict == runner::MatchVerdict::NOT_PLACED);
    CHECK_FALSE(marked);
  }

  SECTION("neither is far enough on: nothing is concluded and the hold stands") {
    const auto [verdict, marked] = sweep(just_after, just_after + std::chrono::seconds(20));
    CHECK(verdict == runner::MatchVerdict::UNDECIDED);
    CHECK(marked);
  }

  SECTION("the boundary is the window's, on the later reading") {
    const runner::MatchWindow window;
    const auto boundary = journaled_at() + window.maximum_request_ + window.clock_skew_ + window.listing_delay_;
    CHECK(sweep(just_after, boundary - std::chrono::seconds(1)).first == runner::MatchVerdict::UNDECIDED);
    CHECK(sweep(just_after, boundary).first == runner::MatchVerdict::NOT_PLACED);
  }
}

TEST_CASE("a loop's sweep resolves a mark while the decision instant stands still", "[confirm][loop]") {
  // No bar arrives, as between two daily bars or under a stop, so the decision instant stays where the
  // last one left it. The loop reports its own reading each iteration, and the sweep counts on that.
  const auto started = journaled_at() + std::chrono::seconds(5);
  WorkingSession session;
  WorkingOrders<FakeBroker> listing;
  listing.received_at_ = started;
  session.set_working_orders(listing);
  session.set_history(OrderPage{});
  HoldingsSnapshot held;
  held.received_at_ = started;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse("10");
  held.positions_.push_back(position);
  session.set_holdings(held);

  runner::MemoryJournal journal;
  journal.append(journaled_placement());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(started);
  live_runner.ledger().mark(samsung(), "an order request failed in transit");
  using Source = live::SessionSource<WorkingSession, runner::MemoryJournal>;
  Source source(session, {samsung()});
  source.set_journal(journal);

  runner::PollSchedule schedule;
  schedule.bars_.reset();
  schedule.calendar_.reset();
  schedule.holdings_ = std::chrono::seconds(10);
  schedule.working_orders_ = std::chrono::seconds(10);
  schedule.tick_ = std::chrono::seconds(10);
  runner::PollingLoop<decltype(live_runner), Source, runner::ManualClock> loop(
    live_runner, source, schedule, runner::ManualClock{started}
  );
  loop.allow_unbounded_portfolio_age();
  runner::StopSource stop;

  SECTION("running: the mark is gone once the window has passed on the loop's clock, and not before") {
    loop.run(stop, 11);
    CHECK(live_runner.context().now() == started);
    CHECK(live_runner.ledger().requires_reconcile(samsung()));
    loop.run(stop, 4);
    CHECK(live_runner.context().now() == started);
    CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
  }

  SECTION("stopping: a wait longer than the window ends when the mark does") {
    loop.set_closing_wait(std::chrono::seconds(300));
    stop.request(runner::StopIntent::WITHDRAW);
    const auto report = loop.run(stop, 0);
    CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
    CHECK_FALSE(report.closing_wait_expired_);
    CHECK(report.closing_waited_ >= std::chrono::seconds(110));
    CHECK(report.closing_waited_ < std::chrono::seconds(130));
  }
}

// ---------------------------------------------------------------------------
// The journal has to be in the runner's own send path, not beside it
// ---------------------------------------------------------------------------

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
 * The gap: a fake that accepts any query shape proves nothing about a
 * query the production adapter refuses. `KisTraits::to_history_query` is the function the real
 * client calls before any network I/O, so routing the fake through it makes the test assert the
 * contract rather than an idea of it.
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
  // The read's start is when the order was sent (the loop's
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
  // The capability is absent, and absent is not faked. The loop then never establishes a
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

TEST_CASE("an order that starts stating nothing and later states a charge differences cleanly") {
  // The run-scope failure: `orders_stating_none_` used to fall when an order first
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
  // The sharper form of the silence case. A silent row advanced the
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
  // The narrower half of the case before it. Keeping a zero only when
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
template <typename Source, typename Runner, typename Session>
void poll_listing_at(
  Source& source, Runner& bound, Session& session,
  std::vector<WorkingOrder<FakeBroker>> rows, int hour
) {
  WorkingOrders<FakeBroker> listing;
  listing.orders_ = std::move(rows);
  listing.received_at_ = live_hour(hour);
  session.set_working_orders(std::move(listing));
  REQUIRE(source.poll(runner::PollKind::WORKING_ORDERS, bound) == runner::PollOutcome::UPDATED);
}

/** Polls holdings of `held` received at `hour`. */
template <typename Source, typename Runner, typename Session>
void poll_holdings_at(Source& source, Runner& bound, Session& session, const char* held, int hour) {
  HoldingsSnapshot snapshot;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = Decimal64::parse(held);
  snapshot.positions_.push_back(position);
  snapshot.received_at_ = live_hour(hour);
  state_sellable(snapshot);
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
template <typename Bound, typename Session>
void leave_behind_a_stop(Bound& bound, Session& session, const WorkingOrder<FakeBroker>& stop = resting_stop()) {
  bound.context().set_instrument(samsung_reference());
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WithWorkingOrders>("10", nullptr);
  portfolio.working_.push_back(stop);
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

TEST_CASE("a deferred flatten does not sell again after the stop filled", "[trigger]") {
  // Holdings still say ten -- stale, the broker's endpoints promise no consistency -- and the stop's own
  // record says it filled.
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

TEST_CASE("a deferred flatten is not released by another day's order under the stop's number", "[trigger]") {
  // The history reads from the day before. Yesterday's STOP-1, cancelled with nothing filled, came first and
  // released the flatten although today's STOP-1 filled: a second sale of the ten.
  WorkingSession session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<WorkingSession> source(session, {samsung()});
  leave_behind_a_stop(bound, session);
  poll_holdings_at(source, bound, session, "10", 2);

  OrderSnapshot yesterday;
  yesterday.order_id_ = "STOP-1";
  yesterday.instrument_ = samsung();
  yesterday.side_ = OrderSide::SELL;
  yesterday.type_ = OrderType::STOP_LIMIT;
  yesterday.status_ = OrderStatus::CANCELED;
  yesterday.quantity_ = Decimal64::parse("10");
  auto today = yesterday;
  today.status_ = OrderStatus::FILLED;
  today.execution_.filled_quantity_ = Decimal64::parse("10");
  OrderPage page;
  page.orders_ = {yesterday, today};
  session.set_history(std::move(page));
  poll_listing_at(source, bound, session, {}, 1);
  CHECK(session.placed().empty());
  CHECK(bound.deferred_targets().size() == 1);
}

namespace {
/** A broker whose history takes a date range, as KIS's does; `WorkingSession`'s is read a day at a time. */
struct RangedWorkingOrders : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
  static constexpr bool history_requires_range = true;
};

/** The stop's own history row, ordered at `at`. */
OrderSnapshot stop_row(OrderStatus status, const char* filled, std::chrono::system_clock::time_point at) {
  OrderSnapshot row;
  row.order_id_ = "STOP-1";
  row.instrument_ = samsung();
  row.side_ = OrderSide::SELL;
  row.type_ = OrderType::STOP_LIMIT;
  row.status_ = status;
  row.quantity_ = Decimal64::parse("10");
  row.execution_.filled_quantity_ = Decimal64::parse(filled);
  row.ordered_at_ = at;
  return row;
}

/**
 * A stop listed with its order time, and yesterday's order under the same number in the history, cancelled
 * with nothing filled. The flatten waits for today's row, whatever the decision clock says.
 */
template <typename Caps>
void a_dated_stop_is_decided_by_its_own_row() {
  FakeSession<Caps> session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<FakeSession<Caps>> source(session, {samsung()});
  const auto placed_at = live_hour(0) - std::chrono::minutes(10);
  auto stop = resting_stop();
  stop.order_.ordered_at_ = placed_at;
  // The loop's clock half a day past the decision instant the flatten is deferred at.
  bound.set_observed_at(live_hour(12));
  leave_behind_a_stop(bound, session, stop);
  poll_holdings_at(source, bound, session, "10", 2);
  const auto yesterday = stop_row(OrderStatus::CANCELED, "0", placed_at - std::chrono::hours(24));

  // Today's row has not reached the history yet: yesterday's does not answer for it.
  OrderPage lagging;
  lagging.orders_ = {yesterday};
  session.set_history(std::move(lagging));
  poll_listing_at(source, bound, session, {}, 1);
  CHECK(session.placed().empty());
  REQUIRE(bound.deferred_targets().size() == 1);

  SECTION("today's stop filled: no second sale") {
    OrderPage page;
    page.orders_ = {yesterday, stop_row(OrderStatus::FILLED, "10", placed_at)};
    session.set_history(std::move(page));
    poll_listing_at(source, bound, session, {}, 2);
    CHECK(session.placed().empty());
    CHECK(bound.deferred_targets().empty());
    CHECK(bound.deferred_dropped_after_fill() == 1);
  }
  SECTION("today's stop cancelled with nothing filled: the flatten goes") {
    OrderPage page;
    page.orders_ = {yesterday, stop_row(OrderStatus::CANCELED, "0", placed_at)};
    session.set_history(std::move(page));
    poll_listing_at(source, bound, session, {}, 2);
    CHECK(session.placed().size() == 1);
    CHECK(bound.deferred_targets().empty());
  }
}
} // namespace

TEST_CASE("a deferred flatten is decided by its stop's own row, read a day at a time", "[trigger]") {
  a_dated_stop_is_decided_by_its_own_row<WithWorkingOrders>();
}

TEST_CASE("a deferred flatten is decided by its stop's own row, read over a range", "[trigger]") {
  a_dated_stop_is_decided_by_its_own_row<RangedWorkingOrders>();
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

TEST_CASE("a row from another day under the same order number is a different order", "[recovery]") {
  // Kiwoom numbers orders per day, and a sweep
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

  // Yesterday's row with no readable time does not swallow today's either: the pages
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

} // namespace

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
  // `EqualWeightRebalance` through `live_program` once reported "not buildable: 80" and nothing else,
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
  // Through the reader that failed: a `RESOLVED` line stamped at the epoch was damage to
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

// ---------------------------------------------------------------------------
// A history page that says there is more is never read as the whole history
// ---------------------------------------------------------------------------

namespace {

/** A broker with a working-order listing whose history pages through a cursor. */
struct WorkingWithCursor : strategy::NoCaps {
  static constexpr bool has_working_orders = true;
  static constexpr bool has_history_cursor = true;
};

/** A broker with no working-order listing whose history pages through a cursor. */
struct HistoryWithCursor : strategy::NoCaps {
  static constexpr bool has_working_orders = false;
  static constexpr bool has_history_cursor = true;
};

/** `page` saying there is another page after it. */
OrderPage saying_more(OrderPage page, const char* cursor = "more") {
  page.next_cursor_ = std::string(cursor);
  return page;
}

/** A journaled cancellation of `order_id`, unanswered. */
runner::JournalEntry journaled_cancellation(const char* order_id) {
  auto entry = journaled_placement();
  entry.action_ = runner::JournalAction::CANCEL;
  entry.order_id_ = order_id;
  return entry;
}

} // namespace

TEST_CASE("a history page saying more, from a broker with no cursor, concludes nothing", "[history]") {
  // Page one does not hold the journaled order and says there is another page this adapter cannot ask
  // for. Read as the whole history, the absence would conclude the order was never placed and clear its
  // hold -- for an order that may be on page two.
  WorkingSession session;
  session.set_working_orders(WorkingOrders<FakeBroker>{});
  session.set_history(saying_more(OrderPage{}));

  runner::MemoryJournal journal;
  journal.append(journaled_placement());
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "an order request failed in transit");
  live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  // The listing itself arrived, so the poll reports it; the history read failed and decided nothing.
  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::UNDECIDED);
  CHECK(source.last_reconcile().recorded_ == 0);
  CHECK(live_runner.ledger().requires_reconcile(samsung()));
  REQUIRE(source.last_error());
  CHECK(source.last_error()->broker_code_ == "history_not_continued");
  CHECK(source.last_error()->category_ == ErrorCategory::PROTOCOL);
}

TEST_CASE("a page that says no more, or says nothing, is the whole history", "[history]") {
  for (const char* cursor : {"", "absent"}) {
    WorkingSession session;
    session.set_working_orders(WorkingOrders<FakeBroker>{});
    OrderPage page;
    if (std::string(cursor) != "absent") {
      page.next_cursor_ = std::string(cursor);
    }
    session.set_history(page);

    runner::MemoryJournal journal;
    journal.append(journaled_placement());
    auto live_runner = live::bind_observer(session, CountEvents{});
    live_runner.context().set_now(swept_at());
    live_runner.ledger().mark(samsung(), "an order request failed in transit");
    live::SessionSource<WorkingSession, runner::MemoryJournal> source(session, {samsung()});
    source.set_journal(journal);

    source.poll(runner::PollKind::WORKING_ORDERS, live_runner);
    CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::NOT_PLACED);
    CHECK_FALSE(live_runner.ledger().requires_reconcile(samsung()));
  }
}

TEST_CASE("the startup read refuses a page saying more from a broker with no cursor", "[history]") {
  // No working-order listing, so the gate reads history. A page saying more, with no way to ask for it,
  // is not the broker's record; the gate stays unread.
  HistorySession session;
  session.set_history(saying_more(OrderPage{}));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error());
  CHECK(source.last_error()->broker_code_ == "history_not_continued");
}

TEST_CASE("the startup read follows a declared cursor", "[history]") {
  // The order a journaled cancellation names is on page two. Read as one page, the cancellation is
  // matched against a history that does not hold it.
  FakeSession<HistoryWithCursor> session;
  session.history_by_query_ = [](const OrderHistoryQuery& query) {
    OrderPage page;
    if (!query.cursor_) {
      return saying_more(page, "p2");
    }
    page.orders_.push_back(listed_order("KRX-1"));
    return page;
  };
  runner::MemoryJournal journal;
  journal.append(journaled_cancellation("KRX-1"));
  auto live_runner = live::bind_observer(session, CountEvents{});
  live_runner.context().set_now(swept_at());
  live_runner.ledger().mark(samsung(), "a cancellation request failed in transit");
  live::SessionSource<FakeSession<HistoryWithCursor>, runner::MemoryJournal> source(session, {samsung()});
  source.set_journal(journal);

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE(session.history_queries().size() >= 2);
  REQUIRE(session.history_queries()[1].cursor_);
  CHECK(*session.history_queries()[1].cursor_ == "p2");
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::MATCHED);
}

TEST_CASE("the startup read sends no limit to a broker that does not honour one", "[history]") {
  // `HistorySession` declares `has_history_limit = false`, as KIS does; KIS refuses a query carrying one.
  HistorySession session;
  auto live_runner = live::bind_observer(session, CountEvents{});
  live::SessionSource<HistorySession> source(session, {samsung()});

  CHECK(source.read_order_list(live_runner) == runner::PollOutcome::UPDATED);
  REQUIRE_FALSE(session.history_queries().empty());
  CHECK_FALSE(session.history_queries().front().limit_);
}

TEST_CASE("a deferred stop's evidence that runs past the page limit releases nothing", "[history][trigger]") {
  // The evidence path reports `*last_error_` when its read fails; running out of pages used to fail with no
  // error set. Every page here holds the stop's cancelled row and says there is more.
  FakeSession<WorkingWithCursor> session;
  auto bound = live::bind(session, FlattensOnPhase{}, {});
  live::SessionSource<FakeSession<WorkingWithCursor>> source(session, {samsung()});
  bound.context().set_instrument(samsung_reference());
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  auto portfolio = held_position<WorkingWithCursor>("10", nullptr);
  portfolio.working_.push_back(resting_stop());
  bound.context().set_portfolio(portfolio);
  bound.feed(strategy::MarketPhase::POST_CLOSE);
  REQUIRE(bound.deferred_targets().size() == 1);

  session.history_by_query_ = [](const OrderHistoryQuery&) {
    OrderSnapshot row;
    row.order_id_ = "STOP-1";
    row.instrument_ = samsung();
    row.side_ = OrderSide::SELL;
    row.type_ = OrderType::STOP_LIMIT;
    row.status_ = OrderStatus::CANCELED;
    row.quantity_ = Decimal64::parse("10");
    row.execution_.filled_quantity_ = Decimal64::parse("0");
    OrderPage page;
    page.orders_.push_back(row);
    return saying_more(page);
  };
  WorkingOrders<FakeBroker> listing;
  listing.received_at_ = live_hour(1);
  session.set_working_orders(listing);

  CHECK(source.poll(runner::PollKind::WORKING_ORDERS, bound) == runner::PollOutcome::FAILED);
  REQUIRE(source.last_error());
  CHECK(source.last_error()->broker_code_ == "history_page_limit");
  CHECK(session.placed().empty());
  CHECK(bound.deferred_targets().size() == 1);
}

// ---------------------------------------------------------------------------------------------------
// Serialized mode (`Runner::serialized`): a journaled runner owns one order per instrument until
// holdings, or the order's own ended history row, account for it.

namespace {

using Instruction = std::variant<strategy::TargetPosition, strategy::OrderIntent>;

/** States every instruction of one batch per bar, then nothing. */
struct Batched {
  std::deque<std::vector<Instruction>>* next_{nullptr};

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    if (next_ == nullptr || next_->empty()) {
      return;
    }
    for (const auto& instruction : next_->front()) {
      std::visit([&](const auto& stated) { ctx.submit(stated); }, instruction);
    }
    next_->pop_front();
  }
};

/** A journal whose writes can be made to fail from a chosen moment. */
struct SwitchableJournal : runner::MemoryJournal {
  using runner::MemoryJournal::MemoryJournal;
  bool fail_{false};
  bool append(const runner::JournalEntry& entry) { return !fail_ && runner::MemoryJournal::append(entry); }
};

/** The journal's clock, moved by the rig with the bars it feeds, as one clock runs both live. */
struct SharedClock {
  const std::chrono::system_clock::time_point* at_{nullptr};
  std::chrono::system_clock::time_point now() const noexcept { return *at_; }
};

template <typename CapsType>
using SerialExecutorOf =
  runner::JournalingExecutor<live::SessionExecutor<FakeSession<CapsType>>, SwitchableJournal, SharedClock>;
template <typename CapsType>
using SerialRunnerOf = runner::Runner<Batched, CapsType, FakeBroker, SerialExecutorOf<CapsType>>;
using SerialSession = FakeSession<WithWorkingOrders>;
using SerialRunner = SerialRunnerOf<WithWorkingOrders>;

/** The instant `polled(hour, ...)` and `bar_at(hour, ...)` stand for. */
std::chrono::system_clock::time_point at_hour(int hour) {
  return std::chrono::system_clock::time_point{} + std::chrono::hours(500000 + hour);
}

/** A journal line a serialized runner writes for a market buy of ten, decided against `baseline`. */
runner::JournalEntry serial_line(
  const char* id, runner::JournalOutcome outcome, const char* order_id = "", const char* baseline = "0",
  const char* quantity = "10"
) {
  runner::JournalEntry entry;
  entry.id_ = id;
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = outcome;
  entry.recorded_at_ = at_hour(-1);
  entry.instrument_ = samsung();
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::MARKET;
  entry.quantity_ = Decimal64::parse(quantity);
  entry.order_id_ = order_id;
  if (baseline != nullptr) {
    entry.baseline_ = Decimal64::parse(baseline);
  }
  return entry;
}

/** A dated history row for order `id`, ended as `status` with `filled` executed. */
OrderSnapshot dated_row(
  const char* id, std::chrono::system_clock::time_point ordered_at, const char* filled,
  OrderStatus status = OrderStatus::CANCELED
) {
  auto row = ended_row(id, OrderSide::BUY, "10", filled);
  row.status_ = status;
  row.ordered_at_ = ordered_at;
  return row;
}

/** A serialized runner over a broker that lists working orders, recovered from the lines given. */
template <typename CapsType = WithWorkingOrders>
struct SerialRigOf {
  FakeSession<CapsType> session;
  SwitchableJournal journal{"test:account"};
  std::deque<std::vector<Instruction>> script;
  std::chrono::system_clock::time_point journal_now{at_hour(0)};
  SerialRunnerOf<CapsType> runner_instance;

  explicit SerialRigOf(const std::vector<runner::JournalEntry>& lines = {}, bool recover = true)
  : runner_instance(
      Batched{&script}, {}, 512,
      SerialExecutorOf<CapsType>(live::SessionExecutor<FakeSession<CapsType>>(session), journal, SharedClock{&journal_now})
    ) {
    for (const auto& line : lines) {
      journal.append(line);
    }
    runner_instance.set_confirmation_timeout(std::chrono::hours(24));
    runner_instance.context().set_instrument(samsung_reference());
    runner_instance.context().set_phase(strategy::MarketPhase::OPEN);
    runner_instance.context().set_last_price(samsung(), Decimal64::parse("70000"));
    if (recover) {
      runner::recover_runner(runner_instance, journal);
    }
    show(polled(0, "0", {}));
  }

  void show(strategy::Portfolio<FakeBroker> portfolio) { runner_instance.context().set_portfolio(std::move(portfolio)); }

  /** Feeds bar `hour` with one batch of instructions; the journal's clock reads that hour. */
  typename SerialRunnerOf<CapsType>::Report bar(int hour, std::vector<Instruction> batch) {
    journal_now = at_hour(hour);
    script.push_back(std::move(batch));
    return runner_instance.feed(bar_at(hour, "70000"));
  }

  /** The journal lines with `outcome`. */
  std::vector<runner::JournalEntry> lines(runner::JournalOutcome outcome) const {
    std::vector<runner::JournalEntry> found;
    for (const auto& entry : journal.records()) {
      if (entry.outcome_ == outcome) {
        found.push_back(entry);
      }
    }
    return found;
  }

  runner::ObligationState owed() const { return runner_instance.obligation_state(samsung()); }
};
using SerialRig = SerialRigOf<>;

strategy::OrderIntent buy(const char* quantity) {
  return strategy::OrderIntent::market(samsung(), OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse(quantity)));
}

strategy::OrderIntent sell(const char* quantity) {
  return strategy::OrderIntent::market(samsung(), OrderSide::SELL, strategy::SizeSpec::quantity(Decimal64::parse(quantity)));
}

strategy::TargetPosition limit_target(const char* quantity, const char* limit) {
  auto target = target_of(quantity);
  target.limit_ = strategy::Price::checked(Decimal64::parse(limit), PriceRule::krx_equity());
  return target;
}

} // namespace

TEST_CASE("a journaled runner is serialized unless told to overlap", "[serialized]") {
  SerialRig rig;
  CHECK(SerialRunner::journals_to_a_broker);
  CHECK(rig.runner_instance.serialized());
  // Chosen before recovery and trading, or not at all.
  CHECK_THROWS_AS(rig.runner_instance.allow_overlapping_orders(), std::logic_error);

  SerialRig overlapping({}, /*recover=*/false);
  overlapping.runner_instance.allow_overlapping_orders();
  CHECK_FALSE(overlapping.runner_instance.serialized());
  // An unjournaled runner never is.
  CHECK_FALSE(ScriptedRunner::journals_to_a_broker);
}

TEST_CASE("a serialized runner places nothing before recovery", "[serialized]") {
  SerialRig rig({}, /*recover=*/false);
  const auto report = rig.bar(1, {target_of("10")});
  CHECK(rig.session.placed().empty());
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::AWAITING_CONFIRMATION);
  CHECK(report.withheld_plans_.front().detail_.find("recover_runner") != std::string::npos);
}

TEST_CASE("a journaled runner over a broker listing no working orders places nothing unless told to overlap",
          "[serialized]") {
  FakeSession<WithKey> session;
  runner::MemoryJournal journal("test:account");
  auto bound = live::bind_journaled(session, journal, HoldTen{}, {});
  runner::recover_runner(bound, journal);
  CHECK(bound.serialized());
  bound.context().set_instrument(samsung_reference());
  bound.context().set_phase(strategy::MarketPhase::OPEN);
  bound.context().set_last_price(samsung(), Decimal64::parse("70000"));
  bound.context().set_portfolio(held_position<WithKey>("0", nullptr));
  const auto report = bound.feed(bar_at(0, "70000"));
  CHECK(session.placed().empty());
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().detail_.find("cannot serialize") != std::string::npos);
}

TEST_CASE("a buy and a sell on one instrument never overlap, so no fill is credited to the wrong order",
          "[serialized]") {
  // The serialized counterpart of the lab's mixed-pair reproduction (`labs/FINDINGS.md` F48): there, a
  // buy and a sell of 100 both filled before holdings moved, both were retired by unchanged holdings,
  // and a later buy was confirmed by the first one's fill. Here the sell waits for the buy.
  SerialRig rig;
  rig.show(polled(0, "100", {}));
  rig.session.order_ids_ = {"B", "C", "D"};

  rig.bar(1, {buy("100")});
  const auto held_back = rig.bar(2, {sell("100")});
  REQUIRE(rig.session.placed().size() == 1);
  REQUIRE(held_back.withheld_plans_.size() == 1);
  CHECK(held_back.withheld_plans_.front().reason_ == runner::WithheldPlanReason::AWAITING_CONFIRMATION);
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  REQUIRE(rig.lines(runner::JournalOutcome::PENDING).size() == 1);
  CHECK(rig.lines(runner::JournalOutcome::PENDING).front().baseline_ == Decimal64::parse("100"));

  // Holdings lag the buy: unchanged, nothing is accounted for, and a target of 200 is held back too.
  rig.show(polled(3, "100", {}));
  rig.bar(4, {target_of("200")});
  CHECK(rig.session.placed().size() == 1);

  // Holdings show the buy: accounted for, in the journal first, and the target is met by it.
  rig.show(polled(5, "200", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
  CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).front().baseline_ == Decimal64::parse("200"));
  CHECK(rig.owed() == runner::ObligationState::NONE);
  rig.bar(6, {target_of("200")});
  CHECK(rig.session.placed().size() == 1);

  // The sell, stated again, goes now, measured from 200.
  rig.bar(7, {sell("100")});
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.session.placed().back().side_ == OrderSide::SELL);
  CHECK(rig.lines(runner::JournalOutcome::PENDING).back().baseline_ == Decimal64::parse("200"));
}

TEST_CASE("a second order for one instrument in one pass waits, and another instrument trades", "[serialized]") {
  // An entry and its exit stated together: the exit waits for the entry, which is the protective-order
  // restriction serialized mode documents. Another instrument is not held by it.
  SerialRig rig;
  const InstrumentId hynix{Country::KR, Venue::COMBINED, "000660"};
  auto hynix_reference = samsung_reference();
  hynix_reference.id_ = hynix;
  rig.runner_instance.context().set_instrument(hynix_reference);
  rig.runner_instance.context().set_last_price(hynix, Decimal64::parse("100000"));
  auto other = target_of("5");
  other.instrument_ = hynix;

  const auto report = rig.bar(1, {buy("10"), sell("10"), other});
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.session.placed()[0].instrument_ == samsung());
  CHECK(rig.session.placed()[1].instrument_ == hynix);
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().detail_.find("earlier instruction in this pass") != std::string::npos);
}

TEST_CASE("a plan that would withdraw and place behind an unaccounted order is withheld whole", "[serialized]") {
  // A reversal would otherwise withdraw the resting order and leave its replacement waiting.
  SerialRig rig;
  rig.show(polled(0, "10", {}));
  rig.session.order_ids_ = {"S1", "B2"};
  rig.bar(1, {limit_target("0", "70000")});
  REQUIRE(rig.session.placed().size() == 1);
  rig.show(polled(2, "10", {listed_row("S1", OrderSide::SELL, "10", "0", OrderType::LIMIT)}));

  const auto report = rig.bar(3, {limit_target("20", "70000")});
  CHECK(rig.session.canceled().empty());
  CHECK(rig.session.placed().size() == 1);
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK_FALSE(report.withheld_plans_.front().not_canceled_.empty());
  CHECK(rig.runner_instance.targets_retained() == 1);

  // An explicit withdrawal still goes.
  rig.runner_instance.context().cancel(OrderRef<FakeBroker>{"S1", ""}, "operator");
  rig.runner_instance.settle_closing();
  CHECK(rig.session.canceled().size() == 1);
}

TEST_CASE("a serialized order is accounted for only by exact holdings or its own dated ended row", "[serialized]") {
  SerialRig rig;
  rig.session.order_ids_ = {"B"};
  rig.bar(1, {target_of("10")});
  REQUIRE(rig.session.placed().size() == 1);
  const auto sent = rig.runner_instance.context().now();
  // Listed resting, then gone, so its history is asked for.
  rig.show(polled(2, "0", {listed_row("B", OrderSide::BUY, "10", "0")}));
  rig.runner_instance.refresh_unconfirmed();
  rig.show(polled(3, "0", {}));

  SECTION("holdings show exactly what it filled") {
    rig.show(polled(4, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::NONE);
    REQUIRE(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
    CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).front().order_id_ == "B");
  }

  SECTION("holdings past it hold it for an operator, durably") {
    rig.show(polled(4, "15", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    REQUIRE(rig.lines(runner::JournalOutcome::HELD).size() == 1);
    CHECK(rig.lines(runner::JournalOutcome::HELD).front().requires_operator_);
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    // Holdings that later look exact do not undo it.
    rig.show(polled(5, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).empty());
  }

  SECTION("its dated row ended with nothing filled: no holdings read needed") {
    rig.runner_instance.confirm_from_history({dated_row("B", sent, "0")});
    CHECK(rig.owed() == runner::ObligationState::NONE);
    CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
  }

  SECTION("a partial fill before the cancel is accounted for once holdings show it") {
    rig.runner_instance.confirm_from_history({dated_row("B", sent, "4")});
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    rig.show(polled(4, "4", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::NONE);
  }

  SECTION("an undated row, a refused cancel, or another day's row says nothing") {
    auto undated = dated_row("B", sent, "0");
    undated.ordered_at_.reset();
    rig.runner_instance.confirm_from_history({undated});
    rig.runner_instance.confirm_from_history({dated_row("B", sent, "0", OrderStatus::CANCEL_REJECTED)});
    rig.runner_instance.confirm_from_history({dated_row("B", sent - std::chrono::hours(24), "0")});
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).empty());
  }

  SECTION("a row saying it ended is not believed while a listing shows it open") {
    rig.show(polled(4, "0", {listed_row("B", OrderSide::BUY, "10", "0")}));
    rig.runner_instance.confirm_from_history({dated_row("B", sent, "0")});
    CHECK(rig.owed() == runner::ObligationState::NAMED);
  }

  SECTION("a line that cannot be written stops the runner and keeps the order") {
    rig.journal.fail_ = true;
    rig.show(polled(4, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    CHECK(rig.runner_instance.local_failure().has_value());
  }
}

TEST_CASE("a holdings read older than the last accounted order cannot size the next one", "[serialized]") {
  SerialRig rig;
  rig.bar(1, {target_of("10")});
  rig.show(polled(4, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.owed() == runner::ObligationState::NONE);

  rig.show(polled(3, "0", {}));
  const auto report = rig.bar(5, {target_of("10")});
  CHECK(rig.session.placed().size() == 1);
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().detail_.find("newer read") != std::string::npos);
}

TEST_CASE("an order whose reply was lost is named only by the sweep that matched its own line", "[serialized]") {
  SerialRig rig;
  rig.session.fail_place_with(reply_lost());
  rig.bar(1, {target_of("10")});
  rig.session.heal_place();
  const auto sent = rig.runner_instance.context().now();
  REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
  const auto line = rig.lines(runner::JournalOutcome::PENDING).front().id_;

  // Holdings alone are not identity, ledger cleared or not.
  rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
  rig.show(polled(2, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);

  // Absence is not proof it was never placed, and another line's answer names nothing.
  rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::nullopt, line);
  rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("X"), "another-line");
  rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("X"));
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);

  // Named by its own line; the name counts once a dated row of its send confirms it, and the holdings
  // already in hand then account for it.
  rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("B"), line);
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  REQUIRE(rig.runner_instance.obligations(samsung()).size() == 1);
  CHECK(rig.runner_instance.obligations(samsung()).front().journal_id_ == line);
  CHECK(rig.runner_instance.obligations(samsung()).front().order_id_ == "B");
  CHECK(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
  rig.runner_instance.confirm_from_history({dated_row("B", sent, "10", OrderStatus::FILLED)});
  CHECK(rig.owed() == runner::ObligationState::NONE);
  REQUIRE(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
  CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).front().order_id_ == "B");
  // A late answer to an order already accounted for changes nothing.
  rig.runner_instance.resolve_indeterminate_placement(samsung(), OrderSide::BUY, std::string("B"), line);
  CHECK(rig.runner_instance.unconfirmed_orders() == 0);
}

TEST_CASE("a restart restores every order the journal leaves unaccounted for", "[serialized]") {
  using runner::JournalOutcome;
  const auto pending = serial_line("p-1", JournalOutcome::PENDING);

  SECTION("sent with no answer: unknown, withheld") {
    SerialRig rig({pending});
    CHECK(rig.owed() == runner::ObligationState::UNNAMED);
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    rig.bar(1, {target_of("10")});
    CHECK(rig.session.placed().empty());
  }

  SECTION("accepted: owed until its history and holdings account for it") {
    SerialRig rig({pending, serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    rig.bar(1, {target_of("10")});
    CHECK(rig.session.placed().empty());
    // No listing ever carried it in this process, and its history is asked for all the same.
    CHECK(rig.runner_instance.unconfirmed_awaiting_evidence().has_value());
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "10", OrderStatus::FILLED)});
    rig.show(polled(2, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::NONE);
    rig.bar(3, {target_of("10")});
    CHECK(rig.session.placed().empty());
  }

  SECTION("a cancelled reservation can only be withdrawn by the broker's own row") {
    SerialRig rig({pending, serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
    rig.runner_instance.context().cancel(OrderRef<FakeBroker>{"B", ""}, "operator");
    const auto refused = rig.runner_instance.settle_closing();
    CHECK(rig.session.canceled().empty());
    REQUIRE(refused.rejected_cancel_.size() == 1);
    CHECK_FALSE(refused.rejected_cancel_.front().by_broker_);

    rig.show(polled(1, "0", {listed_row("B", OrderSide::BUY, "10", "0")}));
    rig.runner_instance.context().cancel(OrderRef<FakeBroker>{"B", ""}, "operator");
    rig.runner_instance.settle_closing();
    CHECK(rig.session.canceled().size() == 1);
  }

  SECTION("accounted for: nothing owed, and only a holdings read after it sizes the next order") {
    auto confirmed = serial_line("p-1", JournalOutcome::CONFIRMED, "B", "10");
    confirmed.recorded_at_ = at_hour(0);
    SerialRig rig({pending, serial_line("p-1", JournalOutcome::ACCEPTED, "B"), confirmed});
    CHECK(rig.owed() == runner::ObligationState::NONE);
    rig.show(polled(0, "0", {}));
    rig.bar(1, {target_of("10")});
    CHECK(rig.session.placed().empty());
    rig.show(polled(1, "10", {}));
    rig.bar(2, {target_of("20")});
    CHECK(rig.session.placed().size() == 1);
  }

  SECTION("found absent by a sweep: still owed, and unknown") {
    SerialRig rig({pending, serial_line("p-1", JournalOutcome::RESOLVED)});
    CHECK(rig.owed() == runner::ObligationState::UNNAMED);
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
  }

  SECTION("held, written without a baseline, inconsistent or doubled: for an operator") {
    auto legacy = serial_line("old-1", JournalOutcome::ACCEPTED, "A", nullptr);
    auto held = serial_line("p-1", JournalOutcome::HELD, "B");
    held.requires_operator_ = true;
    auto other_quantity = serial_line("p-1", JournalOutcome::ACCEPTED, "B", "0", "20");
    for (const auto& lines : std::vector<std::vector<runner::JournalEntry>>{
           {legacy},
           {pending, serial_line("p-1", JournalOutcome::ACCEPTED, "B"), held},
           {pending, other_quantity},
           {pending, serial_line("p-2", JournalOutcome::PENDING)},
         }) {
      SerialRig rig(lines);
      CHECK(rig.owed() == runner::ObligationState::OPERATOR);
      CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
    }
  }

  SECTION("an operator's release accounts for it; the next order needs a later holdings read") {
    SerialRig rig({serial_line("old-1", JournalOutcome::ACCEPTED, "A", nullptr)});
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    REQUIRE(rig.runner_instance.release_obligations(samsung(), "read the order list"));
    CHECK(rig.owed() == runner::ObligationState::NONE);
    REQUIRE(rig.lines(JournalOutcome::CONFIRMED).size() == 1);
    CHECK(rig.lines(JournalOutcome::CONFIRMED).front().detail_.find("released by an operator") == 0);
    rig.bar(1, {target_of("10")});
    CHECK(rig.session.placed().empty());
    rig.show(polled(1, "0", {}));
    rig.bar(2, {target_of("10")});
    CHECK(rig.session.placed().size() == 1);
  }

  SECTION("a release that cannot be written keeps the order and stops the runner") {
    SerialRig rig({serial_line("old-1", JournalOutcome::ACCEPTED, "A", nullptr)});
    rig.journal.fail_ = true;
    CHECK_FALSE(rig.runner_instance.release_obligations(samsung(), "read the order list"));
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    CHECK(rig.runner_instance.local_failure().has_value());
  }

  SECTION("a restored order's timeout starts with the first real clock reading") {
    SerialRig rig({pending, serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
    rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(10));
    rig.runner_instance.set_observed_at(at_hour(1));
    rig.show(polled(1, "0", {}));
    rig.runner_instance.refresh_unconfirmed();
    rig.runner_instance.confirm_from_history({});
    rig.show(polled(1, "0", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 0);
  }
}

TEST_CASE("a restored order is settled through the source's own history read", "[serialized]") {
  SerialRig rig({serial_line("p-1", runner::JournalOutcome::PENDING), serial_line("p-1", runner::JournalOutcome::ACCEPTED, "B")});
  OrderPage page;
  page.orders_.push_back(dated_row("B", at_hour(-1), "10", OrderStatus::FILLED));
  rig.session.set_history(page);
  rig.session.set_working_orders(WorkingOrders<FakeBroker>{});
  live::SessionSource<SerialSession, SwitchableJournal> source(rig.session, {samsung()});
  source.set_journal(rig.journal);
  rig.runner_instance.set_observed_at(at_hour(1));
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  CHECK(rig.session.history_calls() > 0);
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  rig.show(polled(2, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.owed() == runner::ObligationState::NONE);
}

TEST_CASE("orders a serialized runner owes and cannot see are counted for a closing pass", "[serialized]") {
  SerialRig rig({serial_line("p-1", runner::JournalOutcome::PENDING), serial_line("p-1", runner::JournalOutcome::ACCEPTED, "B")});
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.runner_instance.obligations_unexplained() == 1);
  // Listed resting in full: the closing pass sees and withdraws it, so it is not counted.
  rig.show(polled(1, "0", {listed_row("B", OrderSide::BUY, "10", "0")}));
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.runner_instance.obligations_unexplained() == 0);
}

TEST_CASE("a holdings read older than the one an order was decided against cannot account for it", "[serialized]") {
  // Codex round 1 C1: a sell decided against the read that confirmed a buy was discharged by an older,
  // empty read, and a later read let a target sell a second time.
  SerialRig rig;
  rig.bar(1, {target_of("10")});
  rig.show(polled(4, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.owed() == runner::ObligationState::NONE);
  rig.bar(5, {target_of("0")});
  REQUIRE(rig.session.placed().size() == 2);

  rig.show(polled(3, "0", {}));
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
  rig.show(polled(6, "10", {}));
  rig.bar(7, {target_of("0")});
  CHECK(rig.session.placed().size() == 2);
}

TEST_CASE("history that contradicts an order is never read as accounting for it", "[serialized]") {
  using runner::JournalOutcome;
  const std::vector<runner::JournalEntry> accepted{
    serial_line("p-1", JournalOutcome::PENDING), serial_line("p-1", JournalOutcome::ACCEPTED, "B")
  };

  SECTION("more filled than it was for, beside exact holdings: held for an operator, durably") {
    SerialRig rig(accepted);
    rig.show(polled(1, "10", {}));
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "11", OrderStatus::FILLED)});
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
    REQUIRE(rig.lines(JournalOutcome::HELD).size() == 1);
    // And across a restart, whatever holdings then say.
    SerialRig restarted(rig.journal.records());
    restarted.show(polled(2, "10", {}));
    restarted.runner_instance.refresh_unconfirmed();
    CHECK(restarted.owed() == runner::ObligationState::OPERATOR);
  }

  SECTION("more filled than it was for, before holdings move: held, and a later exact read does not undo it") {
    SerialRig rig(accepted);
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "11", OrderStatus::FILLED)});
    rig.show(polled(1, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
  }

  SECTION("pages that disagree on whether it ended say nothing, in either order") {
    for (const bool cancelled_first : {true, false}) {
      SerialRig rig(accepted);
      auto ended = dated_row("B", at_hour(-1), "0", OrderStatus::CANCELED);
      auto refused = dated_row("B", at_hour(-1), "0", OrderStatus::CANCEL_REJECTED);
      rig.runner_instance.confirm_from_history(cancelled_first ? std::vector<OrderSnapshot>{ended, refused} :
                                                                 std::vector<OrderSnapshot>{refused, ended});
      CHECK(rig.owed() == runner::ObligationState::NAMED);
      CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
    }
  }

  SECTION("an ended row with no holdings ever read accounts for nothing filled") {
    SerialRig rig(accepted);
    rig.show(strategy::Portfolio<FakeBroker>{});
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "0")});
    CHECK(rig.owed() == runner::ObligationState::NONE);
  }
}

TEST_CASE("a name a ranged sweep gave from an undated row does not account for an order", "[serialized]") {
  // Codex round 1 C3: through a broker whose history takes a range, the sweep named a restored order from
  // an undated row and holdings then accounted for it.
  SerialRigOf<RangedWorkingOrders> rig({serial_line("p-1", runner::JournalOutcome::PENDING)});
  REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
  rig.show(polled(0, "10", {}));
  auto undated = ended_row("B", OrderSide::BUY, "10", "10");
  undated.status_ = OrderStatus::FILLED;
  undated.type_ = OrderType::MARKET;
  undated.time_in_force_ = TimeInForce::DAY;
  undated.session_ = OrderSession::REGULAR;
  OrderPage page;
  page.orders_.push_back(undated);
  rig.session.set_history(page);
  rig.session.set_working_orders(WorkingOrders<FakeBroker>{});
  live::SessionSource<FakeSession<RangedWorkingOrders>, SwitchableJournal> source(rig.session, {samsung()});
  source.set_journal(rig.journal);
  rig.runner_instance.context().set_now(at_hour(1));
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).empty());

  // A dated row of its send confirms the name, and the holdings in hand then account for it.
  page.orders_.front().ordered_at_ = at_hour(-1);
  rig.session.set_history(page);
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  CHECK(rig.owed() == runner::ObligationState::NONE);
}

TEST_CASE("a target behind this runner's own resting stop is withheld before the stop is withdrawn", "[serialized]") {
  // Codex round 1 C4: the target withdrew the stop and then waited on it, leaving the position unprotected.
  auto stop = serial_line("s-1", runner::JournalOutcome::PENDING, "", "10");
  stop.side_ = OrderSide::SELL;
  stop.type_ = OrderType::STOP_LIMIT;
  stop.price_ = Decimal64::parse("69000");
  stop.trigger_price_ = Decimal64::parse("69500");
  auto stop_accepted = stop;
  stop_accepted.outcome_ = runner::JournalOutcome::ACCEPTED;
  stop_accepted.order_id_ = "S";
  auto resting = listed_row("S", OrderSide::SELL, "10", "0", OrderType::STOP_LIMIT);
  resting.order_.price_ = Decimal64::parse("69000");
  resting.order_.trigger_price_ = Decimal64::parse("69500");

  SerialRig rig({stop, stop_accepted});
  rig.show(polled(0, "10", {resting}));
  REQUIRE(rig.owed() == runner::ObligationState::NAMED);
  const auto report = rig.bar(1, {target_of("0")});
  CHECK(rig.session.canceled().empty());
  REQUIRE(report.withheld_plans_.size() == 1);
  CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::AWAITING_CONFIRMATION);
  CHECK(rig.runner_instance.targets_retained() == 1);
  CHECK(rig.runner_instance.deferred_targets().empty());

  // A stop this runner does not owe withdraws and defers as before.
  SerialRig unowed;
  unowed.show(polled(0, "10", {resting}));
  unowed.bar(1, {target_of("0")});
  CHECK(unowed.session.canceled().size() == 1);
  CHECK(unowed.runner_instance.deferred_targets().size() == 1);
}

TEST_CASE("an overshoot held in one process is still held after a restart", "[serialized]") {
  SerialRig rig;
  rig.bar(1, {target_of("10")});
  rig.show(polled(2, "15", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.owed() == runner::ObligationState::OPERATOR);
  SerialRig restarted(rig.journal.records());
  restarted.show(polled(3, "10", {}));
  restarted.runner_instance.refresh_unconfirmed();
  CHECK(restarted.owed() == runner::ObligationState::OPERATOR);
  CHECK(restarted.lines(runner::JournalOutcome::CONFIRMED).empty());
}

TEST_CASE("an order accounted for just before a restart needs a holdings read made after it", "[serialized]") {
  // The journal's clock and the holdings' run together here, as they do live.
  SerialRig rig;
  rig.bar(1, {target_of("10")});
  rig.journal_now = at_hour(4);
  rig.show(polled(4, "10", {}));
  rig.runner_instance.refresh_unconfirmed();
  REQUIRE(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);

  SerialRig restarted(rig.journal.records());
  CHECK(restarted.owed() == runner::ObligationState::NONE);
  restarted.show(polled(4, "10", {}));
  restarted.bar(5, {target_of("20")});
  CHECK(restarted.session.placed().empty());
  restarted.show(polled(6, "10", {}));
  restarted.bar(7, {target_of("20")});
  CHECK(restarted.session.placed().size() == 1);
}

TEST_CASE("a final fill the history supplies is taken in before holdings can account for the order", "[serialized]") {
  // Codex round 2 C6: the preliminary refresh accounted for the order from holdings, or from an earlier
  // cap, before the rows just supplied were read.
  using runner::JournalOutcome;
  const std::vector<runner::JournalEntry> accepted{
    serial_line("p-1", JournalOutcome::PENDING), serial_line("p-1", JournalOutcome::ACCEPTED, "B")
  };

  SECTION("four filled, and holdings show ten: held, not accounted for") {
    SerialRig rig(accepted);
    rig.show(polled(1, "10", {}));
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
  }

  SECTION("four filled, then ten: held, and no second buy") {
    SerialRig rig(accepted);
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
    REQUIRE(rig.owed() == runner::ObligationState::NAMED);
    rig.show(polled(1, "4", {}));
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "10", OrderStatus::FILLED)});
    CHECK(rig.owed() == runner::ObligationState::OPERATOR);
    CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
    rig.show(polled(2, "4", {}));
    rig.bar(3, {target_of("10")});
    CHECK(rig.session.placed().empty());
  }

  SECTION("the same final fill again is no contradiction") {
    SerialRig rig(accepted);
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
    rig.show(polled(1, "4", {}));
    rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
    CHECK(rig.owed() == runner::ObligationState::NONE);
  }
}

TEST_CASE("a listing reporting more filled than an order's history said it ended with holds it", "[serialized]") {
  // Codex round 3 C8: the listing only delayed the discharge, and once it moved on the old cap accounted
  // for the order and a target bought six more.
  using runner::JournalOutcome;
  SerialRig rig({serial_line("p-1", JournalOutcome::PENDING), serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
  rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
  REQUIRE(rig.owed() == runner::ObligationState::NAMED);

  rig.session.set_working_orders(WorkingOrders<FakeBroker>{});
  live::SessionSource<SerialSession, SwitchableJournal> source(rig.session, {samsung()});
  source.set_journal(rig.journal);
  rig.show(polled(1, "4", {}));
  auto listed = listed_row("B", OrderSide::BUY, "10", "5");
  listed.order_.ordered_at_ = at_hour(-1);
  WorkingOrders<FakeBroker> orders;
  orders.orders_.push_back(listed);
  rig.session.set_working_orders(orders);
  REQUIRE(source.poll(runner::PollKind::WORKING_ORDERS, rig.runner_instance) == runner::PollOutcome::UPDATED);
  // Within the poll itself, before anything else runs.
  CHECK(rig.owed() == runner::ObligationState::OPERATOR);
  CHECK(rig.lines(JournalOutcome::HELD).size() == 1);

  // Gone from the listing, holdings still four: no account, no second buy, across a restart too.
  rig.session.set_working_orders(WorkingOrders<FakeBroker>{});
  REQUIRE(source.poll(runner::PollKind::WORKING_ORDERS, rig.runner_instance) == runner::PollOutcome::UPDATED);
  rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
  rig.show(polled(2, "4", {}));
  rig.bar(3, {target_of("10")});
  CHECK(rig.session.placed().empty());
  CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
  SerialRig restarted(rig.journal.records());
  CHECK(restarted.owed() == runner::ObligationState::OPERATOR);
}

TEST_CASE("a hold the journal could not record is kept, written later, and never turned into an account",
          "[serialized]") {
  // Astra's merge-readiness AM1: the failed HELD write left the order an ordinary one, a later refresh
  // accounted for it from its old cap, and after a restart a target bought six more.
  using runner::JournalOutcome;
  SerialRig rig({serial_line("p-1", JournalOutcome::PENDING), serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
  rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "4")});
  rig.show(polled(1, "4", {}));
  rig.journal.fail_ = true;
  rig.runner_instance.confirm_from_history({dated_row("B", at_hour(-1), "10", OrderStatus::FILLED)});
  CHECK(rig.runner_instance.local_failure().has_value());
  CHECK(rig.owed() == runner::ObligationState::OPERATOR);
  CHECK(rig.lines(JournalOutcome::HELD).empty());

  // The journal works again: the hold is written, nothing is accounted for, and the halt stays.
  rig.journal.fail_ = false;
  rig.runner_instance.refresh_unconfirmed();
  CHECK(rig.lines(JournalOutcome::HELD).size() == 1);
  CHECK(rig.lines(JournalOutcome::CONFIRMED).empty());
  CHECK(rig.owed() == runner::ObligationState::OPERATOR);
  CHECK(rig.runner_instance.local_failure().has_value());

  SerialRig restarted(rig.journal.records());
  CHECK(restarted.owed() == runner::ObligationState::OPERATOR);
  restarted.show(polled(2, "4", {}));
  restarted.bar(3, {target_of("10")});
  CHECK(restarted.session.placed().empty());
}

TEST_CASE("a serialized order past the timeout is counted, and its instrument trades once holdings show it",
          "[serialized]") {
  // A timeout mark outlived the holdings that accounted for the order, and only an operator could lift it.
  SerialRig rig;
  rig.session.order_ids_ = {"B", "C"};
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.bar(1, {target_of("10")});
  REQUIRE(rig.session.placed().size() == 1);
  const auto read = [&](int hour, strategy::Portfolio<FakeBroker> portfolio) {
    rig.runner_instance.set_observed_at(at_hour(hour));
    rig.show(std::move(portfolio));
    rig.runner_instance.refresh_unconfirmed();
  };
  read(2, polled(2, "0", {listed_row("B", OrderSide::BUY, "10", "0")}));
  read(3, polled(3, "0", {}));
  read(4, polled(4, "0", {}));
  rig.runner_instance.confirm_from_history({});
  read(5, polled(5, "0", {}));
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
  CHECK(rig.owed() == runner::ObligationState::NAMED);
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));

  SECTION("still owed, it withholds the instrument") {
    const auto report = rig.bar(6, {target_of("20")});
    CHECK(rig.session.placed().size() == 1);
    REQUIRE(report.withheld_plans_.size() == 1);
    CHECK(report.withheld_plans_.front().reason_ == runner::WithheldPlanReason::AWAITING_CONFIRMATION);
  }

  SECTION("holdings that show it free the instrument") {
    read(6, polled(6, "10", {}));
    CHECK(rig.owed() == runner::ObligationState::NONE);
    CHECK(rig.lines(runner::JournalOutcome::CONFIRMED).size() == 1);
    rig.runner_instance.set_observed_at(at_hour(7));
    rig.bar(7, {target_of("20")});
    REQUIRE(rig.session.placed().size() == 2);
    CHECK(rig.session.placed()[1].quantity_ == Decimal64::parse("10"));
    CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
  }

  SECTION("a mark from another cause outlives the confirmation") {
    rig.runner_instance.ledger().mark(samsung(), "a withdrawal request has an unknown outcome");
    read(6, polled(6, "10", {}));
    CHECK(rig.owed() == runner::ObligationState::NONE);
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
  }
}

TEST_CASE("a serialized order never named still marks the ledger past the timeout", "[serialized]") {
  SerialRig rig;
  rig.session.fail_place_with(reply_lost());
  rig.runner_instance.set_confirmation_timeout(std::chrono::minutes(1));
  rig.bar(1, {target_of("10")});
  rig.session.heal_place();
  REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
  // A sweep found nothing and lifted its hold; the order stays owed, unnamed.
  rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
  for (const int hour : {2, 3}) {
    rig.runner_instance.set_observed_at(at_hour(hour));
    rig.show(polled(hour, "0", {}));
    rig.runner_instance.refresh_unconfirmed();
  }
  CHECK(rig.runner_instance.orders_unconfirmed_timed_out() == 1);
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);
  CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
}

namespace {
/** A refusal as KIS, Kiwoom and Toss state one: in the reply's body, which their adapters classify `PROTOCOL`. */
BrokerError declared_refusal() {
  BrokerError error;
  error.category_ = ErrorCategory::PROTOCOL;
  error.reached_broker_ = true;
  error.broker_code_ = "40330000";
  error.message_ = "no quantity left to amend or cancel";
  return error;
}
} // namespace

TEST_CASE("a target that could only withdraw an order still owed is kept until the order is accounted for",
          "[serialized]") {
  // A buy fills at once and holdings lag it. The strategy says flat, once: the stale picture is met by
  // withdrawing the buy alone, the broker refuses (the order is done), and the exit used to be forgotten.
  SerialRig rig;
  rig.session.order_ids_ = {"B", "S"};
  rig.bar(1, {target_of("10")});
  REQUIRE(rig.session.placed().size() == 1);
  rig.show(polled(2, "0", {}));

  SECTION("the broker rejects the withdrawal") {
    rig.session.fail_cancel_with(already_finished());
    const auto reversal = rig.bar(2, {target_of("0")});
    CHECK(rig.session.canceled().size() == 1);
    CHECK(reversal.withheld_plans_.empty());
    CHECK(rig.session.placed().size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 1);
    CHECK(rig.owed() == runner::ObligationState::NAMED);

    // A newer read that still lags: the target waits for the order, so no second withdrawal goes.
    rig.show(polled(3, "0", {}));
    rig.runner_instance.retry_retained_targets();
    CHECK(rig.session.canceled().size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 1);

    // Holdings show the ten: accounted for, and the exit goes, once, as an order of its own.
    rig.show(polled(4, "10", {}));
    rig.runner_instance.retry_retained_targets();
    REQUIRE(rig.session.placed().size() == 2);
    CHECK(rig.session.placed().back().side_ == OrderSide::SELL);
    CHECK(rig.session.placed().back().quantity_ == Decimal64::parse("10"));
    CHECK(rig.runner_instance.targets_retained() == 0);
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    rig.show(polled(5, "10", {}));
    rig.runner_instance.retry_retained_targets();
    CHECK(rig.session.placed().size() == 2);
    CHECK(rig.session.canceled().size() == 1);
  }

  SECTION("the refusal reads as an unknown outcome, which marks the instrument") {
    rig.session.fail_cancel_with(declared_refusal());
    rig.bar(2, {target_of("0")});
    CHECK(rig.runner_instance.cancels_indeterminate() == 1);
    REQUIRE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    CHECK(rig.runner_instance.targets_retained() == 1);

    // Holdings show the ten before a listing has settled the withdrawal: the exit is planned, withheld for
    // the mark and kept, read after read, with nothing sent.
    for (const int hour : {3, 4}) {
      rig.show(polled(hour, "10", {}));
      const auto retried = rig.runner_instance.retry_retained_targets();
      CHECK(rig.session.placed().size() == 1);
      REQUIRE(retried.withheld_.size() == 1);
      CHECK(retried.withheld_.front().reason_ == runner::WithheldReason::RECONCILE_REQUIRED);
      CHECK(retried.withheld_.front().detail_.find("retained") != std::string::npos);
      CHECK(rig.runner_instance.targets_retained() == 1);
    }
    CHECK(rig.session.canceled().size() == 1);

    // A listing settles the withdrawal. The same holdings read sends nothing; the next one sends the exit.
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.runner_instance.retry_retained_targets();
    CHECK(rig.session.placed().size() == 1);
    rig.show(polled(5, "10", {}));
    rig.runner_instance.retry_retained_targets();
    REQUIRE(rig.session.placed().size() == 2);
    CHECK(rig.session.placed().back().side_ == OrderSide::SELL);
    CHECK(rig.session.placed().back().quantity_ == Decimal64::parse("10"));
    CHECK(rig.runner_instance.targets_retained() == 0);
  }
}

TEST_CASE("a reduction behind an unconfirmed exit is bought back once holdings show the exit", "[serialized]") {
  // The other branch of target reconciliation: twenty held, a sell of twenty fills, holdings still say
  // twenty, and the strategy says ten. The sell side is withdrawn and the replacement, net of the reserved
  // row, is nothing.
  SerialRig rig;
  rig.show(polled(0, "20", {}));
  rig.session.order_ids_ = {"S", "B"};
  rig.bar(1, {target_of("0")});
  REQUIRE(rig.session.placed().size() == 1);

  rig.show(polled(2, "20", {}));
  rig.session.fail_cancel_with(already_finished());
  rig.bar(2, {target_of("10")});
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.runner_instance.targets_retained() == 1);

  rig.show(polled(3, "0", {}));
  rig.runner_instance.retry_retained_targets();
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.session.placed().back().side_ == OrderSide::BUY);
  CHECK(rig.session.placed().back().quantity_ == Decimal64::parse("10"));
}

TEST_CASE("a target kept behind a resting order it withdrew sends nothing more when the order ends unfilled",
          "[serialized]") {
  SerialRig rig;
  rig.session.order_ids_ = {"B"};
  rig.bar(1, {limit_target("10", "69000")});
  REQUIRE(rig.session.placed().size() == 1);
  const auto sent = rig.runner_instance.context().now();
  const auto resting = listed_row("B", OrderSide::BUY, "10", "0", OrderType::LIMIT);

  // Flat, said once: the broker's own row for the owed order is withdrawn, and the target is kept.
  rig.show(polled(2, "0", {resting}));
  rig.bar(2, {target_of("0")});
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.runner_instance.targets_retained() == 1);

  // Still listed at a newer read: a second withdrawal would be refused, and none goes.
  rig.show(polled(3, "0", {resting}));
  rig.runner_instance.retry_retained_targets();
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.runner_instance.targets_retained() == 1);

  // Its own dated row says it ended with nothing filled: accounted for, and the target is already met.
  rig.show(polled(4, "0", {}));
  rig.runner_instance.confirm_from_history({dated_row("B", sent, "0")});
  rig.runner_instance.retry_retained_targets();
  CHECK(rig.owed() == runner::ObligationState::NONE);
  CHECK(rig.runner_instance.targets_retained() == 0);
  CHECK(rig.session.placed().size() == 1);
  CHECK(rig.session.canceled().size() == 1);
}

TEST_CASE("a target withheld for an unresolved order is kept unless the same pass restated it", "[serialized]") {
  SerialRig rig;
  rig.session.order_ids_ = {"B"};
  rig.runner_instance.ledger().mark(samsung(), "a cancellation request has an unknown outcome");
  const auto settle_and_read = [&] {
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.show(polled(2, "0", {}));
    rig.runner_instance.retry_retained_targets();
  };

  SECTION("a later target that needs nothing still wins") {
    // Every instruction is planned before any order is sent, so the buy is withheld after the flat target
    // has spoken. Kept, it would be bought when the mark cleared.
    const auto report = rig.bar(1, {target_of("10"), target_of("0")});
    REQUIRE(report.withheld_.size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 0);
    settle_and_read();
    CHECK(rig.session.placed().empty());
  }

  SECTION("a later target is the one kept") {
    rig.bar(1, {target_of("10"), target_of("20")});
    CHECK(rig.runner_instance.targets_retained() == 1);
    settle_and_read();
    REQUIRE(rig.session.placed().size() == 1);
    CHECK(rig.session.placed().front().quantity_ == Decimal64::parse("20"));
  }

  SECTION("a later order for the instrument is not undone either") {
    rig.bar(1, {target_of("10"), buy("3")});
    CHECK(rig.runner_instance.targets_retained() == 0);
    settle_and_read();
    CHECK(rig.session.placed().empty());
  }
}

TEST_CASE("a target that withdraws an order this runner does not owe is not kept", "[serialized]") {
  SerialRig rig;
  rig.show(polled(1, "0", {listed_row("X", OrderSide::BUY, "10", "0", OrderType::LIMIT)}));
  rig.bar(1, {target_of("0")});
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.runner_instance.targets_retained() == 0);
}

TEST_CASE("a target behind a restored order is kept whichever of its rows it withdraws", "[serialized]") {
  using runner::JournalOutcome;
  SerialRig rig({serial_line("p-1", JournalOutcome::PENDING), serial_line("p-1", JournalOutcome::ACCEPTED, "B")});
  REQUIRE(rig.owed() == runner::ObligationState::NAMED);

  SECTION("its reserved row carries no reference to withdraw by: nothing is sent, the target waits") {
    const auto report = rig.bar(1, {target_of("0")});
    CHECK(rig.session.canceled().empty());
    REQUIRE(report.rejected_cancel_.size() == 1);
    CHECK_FALSE(report.rejected_cancel_.front().by_broker_);
    CHECK(rig.runner_instance.targets_retained() == 1);

    // The listing later supplies the whole reference. The kept target does not withdraw again by itself;
    // stated again, it does.
    auto listed = listed_row("B", OrderSide::BUY, "10", "0");
    listed.ref_ = OrderRef<FakeBroker>{"B", "branch-7"};
    rig.show(polled(2, "0", {listed}));
    rig.runner_instance.retry_retained_targets();
    CHECK(rig.session.canceled().empty());
    CHECK(rig.runner_instance.targets_retained() == 1);
    rig.bar(3, {target_of("0")});
    CHECK(rig.session.canceled().size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 1);
  }

  SECTION("the broker's row carries more than the order's number, and is the same order") {
    auto listed = listed_row("B", OrderSide::BUY, "10", "0");
    listed.ref_ = OrderRef<FakeBroker>{"B", "branch-7"};
    rig.show(polled(1, "0", {listed}));
    rig.bar(1, {target_of("0")});
    CHECK(rig.session.canceled().size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 1);
  }
}

TEST_CASE("a statement queued before a retry replaces what was retained for its instrument", "[serialized]") {
  // `retry_retained_targets` appends what it releases to the context's queue. A caller driving the runner
  // itself may have queued a newer statement there, and the older one must not be settled after it.
  SerialRig rig;
  rig.session.order_ids_ = {"B", "S"};

  SECTION("a target kept for the ledger does not buy over a queued flat target") {
    rig.runner_instance.ledger().mark(samsung(), "a cancellation request has an unknown outcome");
    rig.bar(1, {target_of("10")});
    REQUIRE(rig.runner_instance.targets_retained() == 1);
    rig.runner_instance.ledger().resolve_after_reading_orders(samsung());
    rig.show(polled(2, "0", {}));

    SECTION("a queued target") {
      rig.runner_instance.context().submit(target_of("0"));
      rig.runner_instance.retry_retained_targets();
      rig.runner_instance.settle_closing();
      CHECK(rig.session.placed().empty());
      CHECK(rig.runner_instance.targets_retained() == 0);
    }
    SECTION("a queued order is the only one sent") {
      rig.runner_instance.context().submit(buy("3"));
      rig.runner_instance.retry_retained_targets();
      rig.runner_instance.settle_closing();
      REQUIRE(rig.session.placed().size() == 1);
      CHECK(rig.session.placed().front().quantity_ == Decimal64::parse("3"));
    }
    SECTION("a queued cancellation names an order, and the kept target still goes") {
      rig.runner_instance.context().cancel(OrderRef<FakeBroker>{"X", ""}, "operator");
      rig.runner_instance.retry_retained_targets();
      REQUIRE(rig.session.placed().size() == 1);
      CHECK(rig.session.placed().front().quantity_ == Decimal64::parse("10"));
    }
    SECTION("a queued target for another instrument leaves it") {
      const InstrumentId hynix{Country::KR, Venue::COMBINED, "000660"};
      auto hynix_reference = samsung_reference();
      hynix_reference.id_ = hynix;
      rig.runner_instance.context().set_instrument(hynix_reference);
      rig.runner_instance.context().set_last_price(hynix, Decimal64::parse("100000"));
      auto other = target_of("5");
      other.instrument_ = hynix;
      rig.runner_instance.context().submit(other);
      rig.runner_instance.retry_retained_targets();
      REQUIRE(rig.session.placed().size() == 2);
      CHECK(rig.session.placed()[0].instrument_ == hynix);
      CHECK(rig.session.placed()[1].instrument_ == samsung());
    }
  }

  SECTION("an exit kept behind a filled buy does not sell under a queued target that wants the buy") {
    rig.bar(1, {target_of("10")});
    REQUIRE(rig.session.placed().size() == 1);
    rig.show(polled(2, "0", {}));
    rig.session.fail_cancel_with(already_finished());
    rig.bar(2, {target_of("0")});
    REQUIRE(rig.runner_instance.targets_retained() == 1);

    rig.show(polled(3, "10", {}));
    rig.runner_instance.context().submit(target_of("10"));
    rig.runner_instance.retry_retained_targets();
    rig.runner_instance.settle_closing();
    CHECK(rig.session.placed().size() == 1);
    CHECK(rig.runner_instance.targets_retained() == 0);
  }
}

TEST_CASE("a target below zero on a cash account is refused before anything is sent", "[serialized][short]") {
  // What a short target becomes at a broker that takes cash orders only: a sell of shares the account does
  // not hold. Sent, the broker refuses it in its reply, which reads as an unknown outcome and leaves the
  // order owed until an operator releases it. The guard's sellable check, on by default, refuses it here.
  SerialRig rig;
  rig.session.order_ids_ = {"A", "B"};

  SECTION("from flat: nothing is sent, journaled or owed, and the instrument still trades") {
    const auto report = rig.bar(1, {target_of("-1")});
    CHECK(rig.session.placed().empty());
    REQUIRE(report.refused_.size() == 1);
    CHECK(report.refused_.front().reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
    CHECK(rig.journal.records().empty());
    CHECK(rig.owed() == runner::ObligationState::NONE);
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));

    rig.bar(2, {target_of("10")});
    REQUIRE(rig.session.placed().size() == 1);
    CHECK(rig.session.placed().front().side_ == OrderSide::BUY);
  }

  SECTION("through zero: refused whole rather than clipped to the holding, and flat is still allowed") {
    rig.show(polled(0, "5", {}));
    const auto report = rig.bar(1, {target_of("-1")});
    CHECK(rig.session.placed().empty());
    REQUIRE(report.refused_.size() == 1);
    CHECK(report.refused_.front().reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);

    rig.bar(2, {target_of("0")});
    REQUIRE(rig.session.placed().size() == 1);
    CHECK(rig.session.placed().front().side_ == OrderSide::SELL);
    CHECK(rig.session.placed().front().quantity_ == Decimal64::parse("5"));
  }
}

TEST_CASE("a short target behind a resting stop withdraws the stop before the guard can refuse its sell",
          "[serialized][short]") {
  // The guard refuses placements, and a target's withdrawals go out before the guard sees its orders. So a
  // target below zero behind a protective stop withdraws the stop and waits for it; the sell it then needs
  // would be refused. Pinned as what happens: a refused short is not always "nothing was sent".
  auto resting = listed_row("STOP-1", OrderSide::SELL, "10", "0", OrderType::STOP_LIMIT);
  resting.order_.price_ = Decimal64::parse("69000");
  resting.order_.trigger_price_ = Decimal64::parse("69500");

  SerialRig rig;
  rig.show(polled(0, "10", {resting}));
  const auto report = rig.bar(1, {target_of("-5")});
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.session.placed().empty());
  CHECK(report.refused_.empty());
  REQUIRE(rig.runner_instance.deferred_targets().size() == 1);

  // The stop is gone from the listing and holdings are read again: the target is released, and the sell
  // of fifteen it needs is refused. The ten are still held, and the stop is not.
  rig.show(polled(2, "10", {}));
  const auto released = rig.runner_instance.retry_deferred_on_one_snapshot();
  CHECK(rig.runner_instance.deferred_targets().empty());
  CHECK(rig.session.placed().empty());
  REQUIRE(released.refused_.size() == 1);
  CHECK(released.refused_.front().reason_ == strategy::GuardReason::QUANTITY_NOT_SELLABLE);
}

namespace {

/** A loop clock the test and its fake broker can both read: the program takes its clock by value. */
struct SharedLoopClock {
  runner::ManualClock* clock_{nullptr};
  std::chrono::system_clock::time_point now() const noexcept { return clock_->now(); }
  std::chrono::steady_clock::time_point monotonic_now() const noexcept { return clock_->monotonic_now(); }
  void wait_for(std::chrono::nanoseconds duration) noexcept { clock_->wait_for(duration); }
};

/**
 * A broker whose holdings show a fill only `lag_` after the order was taken, and whose operator asks for
 * a stop the moment the program's first order is in.
 *
 * Every read is stamped at the loop's clock, as a real reply is at its arrival. The listing is always
 * empty: a market order that fills at once is never in it.
 */
struct LaggingAccount : ProgramSession {
  runner::ManualClock* clock_{nullptr};
  runner::StopSource* stop_{nullptr};
  runner::StopIntent word_{runner::StopIntent::FLATTEN};
  std::chrono::seconds lag_{3};
  Decimal64 held_at_start_{Decimal64::parse("0")};
  bool lose_first_reply_{false};
  bool lost_{false};

  Result<Placed> place(OrderRequest order) {
    if (lose_first_reply_ && !lost_) {
      // Never reached the broker; this machine cannot know that.
      lost_ = true;
      stop_->request(word_);
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      return Result<Placed>(error);
    }
    auto placed = ProgramSession::place(order);
    if (placed) {
      fills_.push_back(Fill{clock_->now(), order.side_, order.quantity_});
      if (fills_.size() == 1 && stop_ != nullptr) {
        stop_->request(word_);
      }
    }
    return placed;
  }

  Result<HoldingsSnapshot> holdings() {
    ++holdings_reads_;
    auto held = held_at_start_;
    for (const auto& fill : fills_) {
      if (clock_->now() - fill.at_ >= lag_) {
        held = fill.side_ == OrderSide::BUY ? *decimal_add(held, fill.quantity_) : *decimal_subtract(held, fill.quantity_);
      }
    }
    HoldingsSnapshot snapshot;
    snapshot.received_at_ = clock_->now();
    if (held.coefficient() != 0) {
      HoldingPosition position;
      position.instrument_ = samsung();
      position.quantity_ = held;
      snapshot.positions_.push_back(position);
    }
    state_sellable(snapshot);
    return Result<HoldingsSnapshot>(std::move(snapshot));
  }

  Result<WorkingOrders<FakeBroker>> working_orders() {
    ++listing_reads_;
    WorkingOrders<FakeBroker> listing;
    listing.received_at_ = clock_->now();
    return Result<WorkingOrders<FakeBroker>>(std::move(listing));
  }

  struct Fill {
    std::chrono::system_clock::time_point at_;
    OrderSide side_;
    Decimal64 quantity_;
  };
  std::vector<Fill> fills_;
  std::size_t holdings_reads_{0};
  std::size_t listing_reads_{0};
};

/** The program's settings for a run that a stop ends: the account read once a minute, bars every second. */
live_program::Settings slow_account_settings(const TempPath& journal, const TempPath& control) {
  auto settings = program_settings(journal, control);
  settings.schedule_.holdings_ = std::chrono::seconds(60);
  settings.schedule_.working_orders_ = std::chrono::seconds(60);
  settings.max_iterations_ = 0;
  return settings;
}

/** Holds `quantity` shares: one to buy from flat, none to sell what is held. */
strategy::vanilla::BuyAndHold holds(const char* quantity) {
  auto holding = one_share();
  holding.params_.quantity_ = Decimal64::parse(quantity);
  return holding;
}

} // namespace

TEST_CASE("a flatten that lands on an order holdings do not show yet waits for them and then closes",
          "[live][program][closing-wait]") {
  // The production program, journaled and serialized. The order fills at once; holdings show it three
  // seconds later; the operator's flatten arrives in between. Built at once, the closing pass withdraws
  // an order that has filled and closes nothing; `labs/FINDINGS.md` F52 saw it on both demo accounts.
  const TempPath journal("atl-test-program-closing-wait.jsonl");
  const TempPath control("atl-test-program-closing-wait.stop");
  runner::ManualClock clock{program_start()};
  runner::StopSource stop;
  LaggingAccount session;
  session.clock_ = &clock;
  session.stop_ = &stop;
  session.set_reference(samsung_reference());
  session.set_bars(two_bars("70000", "71000"));
  std::ostringstream out;
  auto settings = slow_account_settings(journal, control);

  SECTION("a buy: the wait reads again, the share is there, and it is sold, once") {
    const auto exit = live_program::trade(session, holds("1"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLEAN);
    REQUIRE(session.placed().size() == 2);
    CHECK(session.placed()[0].side_ == OrderSide::BUY);
    CHECK(session.placed()[1].side_ == OrderSide::SELL);
    CHECK(session.placed()[1].quantity_ == Decimal64::parse("1"));
    CHECK(session.canceled().empty());
    CHECK(out.str().find("closing complete: yes") != std::string::npos);
    // One more read of each, five seconds after the first, not at the one-minute cadence.
    CHECK(session.holdings_reads_ == 2);
    CHECK(clock.now() - program_start() < std::chrono::seconds(15));
  }

  SECTION("a sell: the wait reads again, nothing is held, and nothing more is sent") {
    session.held_at_start_ = Decimal64::parse("1");
    const auto exit = live_program::trade(session, holds("0"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLEAN);
    REQUIRE(session.placed().size() == 1);
    CHECK(session.placed()[0].side_ == OrderSide::SELL);
    CHECK(session.canceled().empty());
  }

  SECTION("holdings that lag past the wait: the pass runs as it did, and says the wait ran out") {
    session.lag_ = std::chrono::seconds(300);
    const auto exit = live_program::trade(session, holds("1"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLOSING_INCOMPLETE);
    CHECK(session.placed().size() == 1);
    CHECK(out.str().find("the stop waited") != std::string::npos);
    // The read at the start, one every five seconds while the wait lasts, and a last one as its limit
    // passes: not once, and not every tick.
    CHECK(session.holdings_reads_ == 7);
    CHECK(clock.now() - program_start() >= std::chrono::seconds(30));
    CHECK(clock.now() - program_start() < std::chrono::seconds(40));
  }

  SECTION("with no wait, the pass is the old one") {
    settings.closing_wait_ = std::chrono::seconds(0);
    const auto exit = live_program::trade(session, holds("1"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLOSING_INCOMPLETE);
    CHECK(session.placed().size() == 1);
    CHECK(session.holdings_reads_ == 1);
    CHECK(out.str().find("the stop waited") == std::string::npos);
  }

  SECTION("an order whose reply was lost stays owed through empty listings and the whole wait") {
    // Serialized, a sweep only names an order: absence from a listing is not proof it was never placed.
    // The program's journal stamps its lines on the machine's clock, so the loop's starts there too.
    clock.jump(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - program_start()));
    session.lose_first_reply_ = true;
    session.set_history(OrderPage{});
    settings.closing_wait_ = std::chrono::seconds(200);
    const auto exit = live_program::trade(session, holds("1"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLOSING_INCOMPLETE);
    CHECK(session.placed().empty());
    CHECK(session.canceled().empty());
    CHECK(out.str().find("the stop waited 200 s") != std::string::npos);
  }

  SECTION("a withdraw waits the same way and then has nothing to withdraw") {
    session.word_ = runner::StopIntent::WITHDRAW;
    const auto exit = live_program::trade(session, holds("1"), settings, stop, out, SharedLoopClock{&clock});
    INFO(out.str());
    CHECK(exit == live_program::Exit::CLEAN);
    CHECK(session.placed().size() == 1);
    CHECK(session.canceled().empty());
  }
}

namespace {

using SerialRunner = SerialRunnerOf<WithWorkingOrders>;

/**
 * A source for a serialized rig under a loop: every poll reports no change (the rig installs the
 * portfolio itself), and a test can have the working-order poll do something on its nth call.
 */
struct QuietSource {
  static constexpr bool provides_working_orders = true;
  std::size_t working_polls_{0};
  std::size_t act_on_working_poll_{0};
  std::function<void()> act_;
  bool holdings_fail_{false};
  std::size_t holdings_polls_{0};
  std::size_t act_on_holdings_poll_{0};
  runner::ManualClock* clock_{nullptr};
  /** When the working-order endpoint was polled, on `clock_`. */
  std::vector<std::chrono::system_clock::time_point> working_asked_;

  runner::PollOutcome poll(runner::PollKind kind, SerialRunner&) {
    if (kind == runner::PollKind::WORKING_ORDERS && clock_ != nullptr) {
      working_asked_.push_back(clock_->now());
    }
    if (kind == runner::PollKind::WORKING_ORDERS && ++working_polls_ == act_on_working_poll_ && act_) {
      act_();
    }
    if (kind == runner::PollKind::HOLDINGS && ++holdings_polls_ == act_on_holdings_poll_ && act_) {
      act_();
    }
    if (kind == runner::PollKind::HOLDINGS && holdings_fail_) {
      return runner::PollOutcome::FAILED;
    }
    return runner::PollOutcome::UNCHANGED;
  }
  runner::PollOutcome read_order_list(SerialRunner&) { return runner::PollOutcome::UPDATED; }
};

using SerialLoop = runner::PollingLoop<SerialRunner, QuietSource, runner::ManualClock>;

/** A loop over a serialized rig whose account was last read at hour `hour`, polled every second. */
SerialLoop loop_over(SerialRig& rig, QuietSource& source, int hour) {
  runner::PollSchedule schedule;
  schedule.bars_.reset();
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(1);
  schedule.tick_ = std::chrono::seconds(1);
  SerialLoop loop(rig.runner_instance, source, schedule, runner::ManualClock{polled(hour, "0", {}).as_of_});
  loop.allow_unbounded_portfolio_age();
  return loop;
}

} // namespace

TEST_CASE("a stop does not withdraw again an order whose withdrawal the broker already took", "[serialized][closing-wait]") {
  // Seen on Kiwoom's demo: a far limit withdrawn six seconds before a flatten was withdrawn again by the
  // closing pass, refused, and the refusal read as an unknown outcome.
  enum class Row { SAME_ORDER, NEXT_DAY, UNDATED };
  for (const auto word : {runner::StopIntent::WITHDRAW, runner::StopIntent::FLATTEN}) {
    for (const auto shown : {Row::SAME_ORDER, Row::NEXT_DAY, Row::UNDATED}) {
      SerialRig rig;
      rig.session.order_ids_ = {"B"};
      rig.bar(1, {limit_target("10", "69000")});
      const auto sent = rig.runner_instance.context().now();
      auto resting = listed_row("B", OrderSide::BUY, "10", "0", OrderType::LIMIT);
      resting.order_.ordered_at_ = sent;
      rig.show(polled(2, "0", {resting}));
      rig.runner_instance.context().cancel(resting.ref_, "strategy");
      rig.runner_instance.settle_closing();
      REQUIRE(rig.session.canceled().size() == 1);
      // A broker reuses its order numbers from day to day: the same number a day later, or with no date
      // at all, is not known to be the order that was withdrawn.
      if (shown == Row::NEXT_DAY) {
        resting.order_.ordered_at_ = sent + std::chrono::hours(24);
        rig.show(polled(26, "0", {resting}));
      } else if (shown == Row::UNDATED) {
        resting.order_.ordered_at_.reset();
        rig.show(polled(2, "0", {resting}));
      }

      QuietSource source;
      auto loop = loop_over(rig, source, shown == Row::NEXT_DAY ? 26 : 2);
      runner::StopSource stop;
      stop.request(word);
      const auto report = loop.run(stop, 0);

      // It waited for the order's history to account for it, which this source never brings, and then ran
      // its pass with the row still listed.
      CAPTURE(runner::stop_intent_name(word), static_cast<int>(shown));
      CHECK(report.closing_wait_expired_);
      CHECK(rig.session.placed().size() == 1);
      if (shown == Row::SAME_ORDER) {
        CHECK(rig.session.canceled().size() == 1);
        CHECK(report.withdrawn_ == 0);
      } else {
        CHECK(rig.session.canceled().size() == 2);
        CHECK(report.withdrawn_ == 1);
      }
    }
  }
}

TEST_CASE("a stop discards a target waiting on a resting stop order", "[serialized][closing-wait]") {
  auto resting = listed_row("STOP-1", OrderSide::SELL, "10", "0", OrderType::STOP_LIMIT);
  resting.order_.price_ = Decimal64::parse("69000");
  resting.order_.trigger_price_ = Decimal64::parse("69500");
  SerialRig rig;
  rig.show(polled(0, "10", {resting}));
  rig.bar(1, {target_of("0")});
  REQUIRE(rig.runner_instance.deferred_targets().size() == 1);

  QuietSource source;
  auto loop = loop_over(rig, source, 0);
  runner::StopSource stop;

  SECTION("when the stop is carried out") {
    stop.request(runner::StopIntent::QUIESCE);
    loop.run(stop, 0);
  }

  SECTION("when the run is abandoned in the iteration that first saw the stop") {
    // Before the stop is taken up at all: the first poll fails and the run ends there.
    source.holdings_fail_ = true;
    runner::FailurePolicy policy;
    policy.consecutive_limit_ = 1;
    loop.set_failure_policy(policy);
    stop.request(runner::StopIntent::WITHDRAW);
    const auto report = loop.run(stop, 0);
    REQUIRE(report.abandoned_);
    CHECK(report.closing_interrupted_);
    CHECK(rig.runner_instance.guard().halted(strategy::HaltReason::STOPPING));
  }

  // Released after the stop, by a resumed runner, it would sell what the operator stopped to keep.
  CHECK(rig.runner_instance.deferred_targets().empty());
}

TEST_CASE("a flatten waits for a mark on what it has to close", "[serialized][closing-wait]") {
  // A cancel of unknown outcome marks its instrument, and a closing sell for a marked instrument is
  // withheld. A listing read that settles the mark is worth waiting for.
  SerialRig rig;
  rig.session.order_ids_ = {"S"};
  rig.show(polled(1, "10", {}));
  rig.runner_instance.ledger().mark(samsung(), "a cancellation request has an unknown outcome");
  QuietSource source;
  auto loop = loop_over(rig, source, 1);
  runner::StopSource stop;
  stop.request(runner::StopIntent::FLATTEN);

  SECTION("settled inside the wait: the ten are sold") {
    source.act_on_working_poll_ = 3;
    source.act_ = [&] { rig.runner_instance.ledger().resolve_after_reading_orders(samsung()); };
    const auto report = loop.run(stop, 0);
    CHECK_FALSE(report.closing_wait_expired_);
    CHECK(report.closing_waited_ > std::chrono::nanoseconds::zero());
    REQUIRE(rig.session.placed().size() == 1);
    CHECK(rig.session.placed().front().side_ == OrderSide::SELL);
    CHECK(rig.session.placed().front().quantity_ == Decimal64::parse("10"));
    CHECK(report.closing_complete());
  }

  SECTION("not settled in time: the sell is withheld, as it was, and the report says so") {
    const auto report = loop.run(stop, 0);
    CHECK(report.closing_wait_expired_);
    CHECK(rig.session.placed().empty());
    CHECK(report.closing_withheld_ == 1);
    CHECK_FALSE(report.closing_complete());
  }

  SECTION("under an operator's hold or the runner's own, the pass can send nothing: no wait") {
    SECTION("operator") { rig.runner_instance.guard().halt(strategy::HaltReason::OPERATOR); }
    SECTION("local failure") { rig.runner_instance.halt_locally("a test's failure"); }
    const auto report = loop.run(stop, 0);
    CHECK(report.iterations_ == 1);
    CHECK_FALSE(report.closing_wait_expired_);
    CHECK(rig.session.placed().empty());
    CHECK_FALSE(report.closing_complete());
    // And nothing paced: the poll after the order-list read is made in this iteration, as without a wait.
    CHECK(source.working_polls_ == 1);
  }
}

TEST_CASE("a flatten does not wait for a mark on rows that net to nothing", "[serialized][closing-wait]") {
  // The pass closes an instrument's net, and here there is none.
  SerialRig rig;
  auto portfolio = polled(1, "10", {});
  auto against = portfolio.positions_.front();
  against.quantity_ = Decimal64::parse("-10");
  portfolio.positions_.push_back(against);
  rig.show(portfolio);
  REQUIRE(rig.runner_instance.context().portfolio().net_quantity_of(samsung()).value().coefficient() == 0);
  rig.runner_instance.ledger().mark(samsung(), "a cancellation request has an unknown outcome");
  QuietSource source;
  auto loop = loop_over(rig, source, 1);
  runner::StopSource stop;
  stop.request(runner::StopIntent::FLATTEN);

  const auto report = loop.run(stop, 0);
  CHECK(report.iterations_ == 1);
  CHECK_FALSE(report.closing_wait_expired_);
  CHECK(rig.session.placed().empty());
}

TEST_CASE("a flatten arriving a second after the account was read still makes the read that was due",
          "[serialized][closing-wait]") {
  // A resting buy, read as resting; it fills; the flatten arrives one second after that read, with both
  // account reads due again on their one-second cadence. The iteration that takes the stop up makes
  // them, whatever the wait's limit: held back for the wait's five-second floor, they were lost when
  // the limit was shorter than the floor, and the pass withdrew an order that had filled and sold nothing.
  for (const auto limit : {std::chrono::seconds(30), std::chrono::seconds(1), std::chrono::seconds(0)}) {
    for (const std::size_t iterations : {std::size_t{0}, std::size_t{1}}) {
      SerialRig rig;
      rig.session.order_ids_ = {"B", "S"};
      rig.bar(1, {limit_target("10", "69000")});
      const auto sent = rig.runner_instance.context().now();
      auto resting = listed_row("B", OrderSide::BUY, "10", "0", OrderType::LIMIT);
      resting.order_.ordered_at_ = sent;
      rig.show(polled(2, "0", {resting}));

      QuietSource source;
      auto loop = loop_over(rig, source, 2);
      source.clock_ = &loop.clock();
      loop.set_closing_wait(limit);
      runner::StopSource stop;
      REQUIRE(loop.run(stop, 1).iterations_ == 1);
      REQUIRE(source.working_polls_ == 1);

      // The next read of the account, whenever the loop makes it, shows the fill.
      source.act_on_working_poll_ = 2;
      source.act_ = [&] { rig.show(polled(3, "10", {})); };
      loop.clock().wait_for(std::chrono::seconds(1));
      stop.request(runner::StopIntent::FLATTEN);

      const auto report = loop.run(stop, iterations);
      CAPTURE(limit.count(), iterations);
      CHECK(report.iterations_ == 1);
      CHECK_FALSE(report.closing_wait_expired_);
      CHECK(rig.session.canceled().empty());
      REQUIRE(rig.session.placed().size() == 2);
      CHECK(rig.session.placed().back().side_ == OrderSide::SELL);
      CHECK(rig.session.placed().back().quantity_ == Decimal64::parse("10"));
      // At once, not five seconds after the read before the stop.
      REQUIRE(source.working_asked_.size() == 2);
      CHECK(source.working_asked_[1] - source.working_asked_[0] < std::chrono::seconds(5));
    }
  }
}

TEST_CASE("a quiesce reads as any other iteration", "[serialized][closing-wait]") {
  SerialRig rig;
  rig.show(polled(2, "0", {}));
  QuietSource source;
  auto loop = loop_over(rig, source, 2);
  runner::StopSource stop;
  REQUIRE(loop.run(stop, 1).iterations_ == 1);
  loop.clock().wait_for(std::chrono::seconds(1));
  stop.request(runner::StopIntent::QUIESCE);
  const auto report = loop.run(stop, 0);
  CHECK(report.iterations_ == 1);
  CHECK(source.working_polls_ == 2);
  CHECK(rig.session.placed().empty());
}

namespace {

/** A loop clock that does something after each wait: between two iterations, where a signal lands. */
struct BetweenIterations {
  runner::ManualClock* clock_{nullptr};
  std::size_t* waits_{nullptr};
  std::function<void()> act_;
  std::chrono::system_clock::time_point now() const noexcept { return clock_->now(); }
  std::chrono::steady_clock::time_point monotonic_now() const noexcept { return clock_->monotonic_now(); }
  void wait_for(std::chrono::nanoseconds duration) {
    clock_->wait_for(duration);
    ++*waits_;
    if (act_) {
      act_();
    }
  }
};

} // namespace

TEST_CASE("a flatten arriving between two iterations makes the holdings read that is due",
          "[serialized][closing-wait]") {
  // Working orders once a minute, holdings every second, one run. The flatten arrives between two
  // iterations, a second after holdings were read. The next iteration reads them on their cadence, as
  // it would without a stop, and they show the fill. A wall clock stepped back afterwards changes
  // nothing: the read has been made and the pass has run.
  SerialRig rig;
  rig.session.order_ids_ = {"B", "S"};
  rig.bar(1, {limit_target("10", "69000")});
  auto resting = listed_row("B", OrderSide::BUY, "10", "0", OrderType::LIMIT);
  resting.order_.ordered_at_ = rig.runner_instance.context().now();
  rig.show(polled(2, "0", {resting}));

  QuietSource source;
  runner::PollSchedule schedule;
  schedule.bars_.reset();
  schedule.holdings_ = std::chrono::seconds(1);
  schedule.working_orders_ = std::chrono::seconds(60);
  schedule.tick_ = std::chrono::seconds(1);
  runner::ManualClock clock{polled(2, "0", {}).as_of_};
  runner::StopSource stop;
  std::size_t waits = 0;
  BetweenIterations between{&clock, &waits, [&] {
    if (waits == 2) {
      stop.request(runner::StopIntent::FLATTEN);
    } else if (waits == 3) {
      clock.jump(-std::chrono::seconds(10));
    }
  }};
  runner::PollingLoop<SerialRunner, QuietSource, BetweenIterations> loop(rig.runner_instance, source, schedule, between);
  loop.allow_unbounded_portfolio_age();
  // The third holdings read, whenever the loop makes it, shows the ten.
  source.act_on_holdings_poll_ = 3;
  source.act_ = [&] { rig.show(polled(3, "10", {})); };

  const auto report = loop.run(stop, 0);
  CHECK(report.iterations_ == 3);
  CHECK(waits == 2);
  CHECK(source.holdings_polls_ == 3);
  CHECK_FALSE(report.closing_wait_expired_);
  CHECK(rig.session.canceled().empty());
  REQUIRE(rig.session.placed().size() == 2);
  CHECK(rig.session.placed().back().side_ == OrderSide::SELL);
  CHECK(rig.session.placed().back().quantity_ == Decimal64::parse("10"));
}

TEST_CASE("a flatten does not wait for a mark on an instrument with nothing held", "[serialized][closing-wait]") {
  // Nothing to close there, so no read changes what the pass sends.
  SerialRig rig;
  rig.show(polled(1, "0", {}));
  REQUIRE(rig.runner_instance.context().portfolio().positions_.size() == 1);
  rig.runner_instance.ledger().mark(samsung(), "a cancellation request has an unknown outcome");
  QuietSource source;
  auto loop = loop_over(rig, source, 1);
  runner::StopSource stop;
  stop.request(runner::StopIntent::FLATTEN);

  const auto report = loop.run(stop, 0);
  CHECK(report.iterations_ == 1);
  CHECK_FALSE(report.closing_wait_expired_);
  CHECK(rig.session.placed().empty());
}

TEST_CASE("a flatten does not wait for an order held for an operator", "[serialized][closing-wait]") {
  // No read gives an account of it: only `release_obligations` does. Fifteen held where a buy of ten from
  // none explains ten; then still held, and marked with it, or gone since.
  for (const char* held : {"15", "0"}) {
    SerialRig rig;
    rig.session.order_ids_ = {"B"};
    rig.bar(1, {target_of("10")});
    REQUIRE(rig.session.placed().size() == 1);
    rig.show(polled(2, "15", {}));
    rig.runner_instance.refresh_unconfirmed();
    rig.show(polled(3, held, {}));
    rig.runner_instance.refresh_unconfirmed();
    REQUIRE(rig.owed() == runner::ObligationState::OPERATOR);
    REQUIRE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    QuietSource source;
    auto loop = loop_over(rig, source, 3);
    runner::StopSource stop;
    stop.request(runner::StopIntent::FLATTEN);

    const auto report = loop.run(stop, 0);
    CAPTURE(held);
    CHECK(report.iterations_ == 1);
    CHECK_FALSE(report.closing_wait_expired_);
    CHECK(rig.session.placed().size() == 1);
  }
}

TEST_CASE("a flatten does not wait for an order that is simply resting, and cannot close behind it in one pass",
          "[serialized][closing-wait]") {
  // The limit of the wait, pinned: only the pass can withdraw a resting order, and a serialized runner
  // then owes that order until it is accounted for, so the holding is not closed by this stop.
  SerialRig rig;
  rig.session.order_ids_ = {"B", "S"};
  rig.show(polled(0, "10", {}));
  rig.bar(1, {limit_target("20", "69000")});
  REQUIRE(rig.session.placed().size() == 1);
  // Dated as the broker lists it: recognizably this runner's order, and not one it has withdrawn.
  auto resting = listed_row("B", OrderSide::BUY, "10", "0", OrderType::LIMIT);
  resting.order_.ordered_at_ = rig.runner_instance.context().now();
  rig.show(polled(2, "10", {resting}));

  QuietSource source;
  auto loop = loop_over(rig, source, 2);
  runner::StopSource stop;
  stop.request(runner::StopIntent::FLATTEN);
  const auto report = loop.run(stop, 0);

  CHECK(report.closing_waited_ == std::chrono::nanoseconds::zero());
  CHECK(rig.session.canceled().size() == 1);
  CHECK(rig.session.placed().size() == 1);
  CHECK(report.closing_withheld_ == 1);
  CHECK_FALSE(report.closing_complete());
}

// ---------------------------------------------------------------------------------------------------
// An order a sweep did not find (a serialized runner keeps it owed): the search goes on, and a run stops
// placing once nobody has been able to account for it for too long.

namespace {

/** A market buy of ten as a broker's history lists it, dated. */
OrderSnapshot market_row(
  const char* id, std::chrono::system_clock::time_point ordered_at, const char* filled = "10",
  OrderStatus status = OrderStatus::FILLED
) {
  auto row = dated_row(id, ordered_at, filled, status);
  row.type_ = OrderType::MARKET;
  row.time_in_force_ = TimeInForce::DAY;
  row.session_ = OrderSession::REGULAR;
  return row;
}

OrderPage page_of(std::vector<OrderSnapshot> rows) {
  OrderPage page;
  page.orders_ = std::move(rows);
  return page;
}

/** The lines of a placement sent an hour before the rig starts, whose reply was lost and that a sweep did not find. */
std::vector<runner::JournalEntry> unfound_lines(const char* id = "p-1", const char* baseline = "0") {
  return {
    serial_line(id, runner::JournalOutcome::PENDING, "", baseline),
    serial_line(id, runner::JournalOutcome::INDETERMINATE, "", baseline),
    serial_line(id, runner::JournalOutcome::RESOLVED, "", baseline),
  };
}

using SerialSource = live::SessionSource<SerialSession, SwitchableJournal>;

/** The source of a serialized rig, reading an empty order list and `history`. */
SerialSource source_over(SerialRig& rig, OrderPage history = {}) {
  rig.session.set_working_orders(WorkingOrders<FakeBroker>{});
  rig.session.set_history(std::move(history));
  SerialSource source(rig.session, {samsung()});
  source.set_journal(rig.journal);
  return source;
}

} // namespace

TEST_CASE("a sweep that finds no order says what it read, and a serialized runner goes on owing it",
          "[serialized][unfound]") {
  SerialRig rig;
  rig.session.fail_place_with(reply_lost());
  rig.bar(1, {target_of("10")});
  rig.session.heal_place();
  REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
  auto source = source_over(rig);
  rig.runner_instance.set_observed_at(at_hour(2));

  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::NOT_PLACED);
  REQUIRE(rig.lines(runner::JournalOutcome::RESOLVED).size() == 1);
  const auto detail = rig.lines(runner::JournalOutcome::RESOLVED).front().detail_;
  CHECK(detail.find("does not exist") == std::string::npos);
  CHECK(detail.find("no matching order was found among the rows read") != std::string::npos);
  CHECK(detail.find("keeps the order owed") != std::string::npos);
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);
  CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
}

TEST_CASE("an order no sweep found is named once its dated row appears", "[serialized][unfound]") {
  using runner::JournalOutcome;

  SECTION("in the process that sent it, and holdings then account for it") {
    SerialRig rig;
    rig.session.fail_place_with(reply_lost());
    rig.bar(1, {target_of("10")});
    rig.session.heal_place();
    auto source = source_over(rig);
    rig.runner_instance.set_observed_at(at_hour(2));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);

    // Nothing new: the search is made and changes nothing.
    const auto calls = rig.session.history_calls();
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(rig.session.history_calls() > calls);
    CHECK(rig.lines(JournalOutcome::RESOLVED).size() == 1);
    CHECK(rig.owed() == runner::ObligationState::UNNAMED);

    // The broker's history shows it at last, filled. Marked meanwhile, as its timeout would.
    rig.runner_instance.ledger().mark(samsung(), "not accounted for within the confirmation timeout");
    rig.session.set_history(page_of({market_row("B", at_hour(1))}));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    REQUIRE(rig.lines(JournalOutcome::RESOLVED).size() == 2);
    CHECK(rig.lines(JournalOutcome::RESOLVED).back().order_id_ == "B");
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    CHECK_FALSE(rig.runner_instance.ledger().requires_reconcile(samsung()));
    // The one whose reply was lost; never a second.
    CHECK(rig.session.placed().size() == 1);
    rig.show(polled(3, "10", {}));
    rig.runner_instance.refresh_unconfirmed();
    CHECK(rig.owed() == runner::ObligationState::NONE);
    REQUIRE(rig.lines(JournalOutcome::CONFIRMED).size() == 1);
    CHECK(rig.lines(JournalOutcome::CONFIRMED).front().order_id_ == "B");
    // And the search ends with the name.
    const auto after = rig.session.history_calls();
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(rig.session.history_calls() == after);
  }

  SECTION("after a restart, from the journal alone") {
    SerialRig rig(unfound_lines());
    REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
    auto source = source_over(rig, page_of({market_row("B", at_hour(-1))}));
    rig.runner_instance.set_observed_at(at_hour(1));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    REQUIRE(rig.runner_instance.obligations().size() == 1);
    CHECK(rig.runner_instance.obligations().front().order_id_ == "B");
    CHECK(rig.runner_instance.obligations().front().instrument_ == samsung());
    // A second restart reads the name from the second line.
    SerialRig again(rig.journal.records());
    CHECK(again.owed() == runner::ObligationState::NAMED);
  }
}

TEST_CASE("the later search acts on one dated row of the order's own and on nothing else", "[serialized][unfound]") {
  using runner::JournalOutcome;
  SerialRig rig(unfound_lines());
  rig.runner_instance.set_observed_at(at_hour(1));
  const auto unchanged = [&] {
    return rig.owed() == runner::ObligationState::UNNAMED && rig.lines(JournalOutcome::RESOLVED).size() == 1;
  };

  SECTION("an undated row of the same shape may be another day's order") {
    auto undated = market_row("B", at_hour(-1));
    undated.ordered_at_.reset();
    auto source = source_over(rig, page_of({undated}));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(unchanged());
  }

  SECTION("a row dated outside its send is not it") {
    auto source = source_over(rig, page_of({market_row("B", at_hour(-3))}));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(unchanged());
  }

  SECTION("two rows that could be it decide nothing") {
    auto source = source_over(rig, page_of({market_row("B", at_hour(-1)), market_row("C", at_hour(-1))}));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(unchanged());
  }

  SECTION("a history read that fails decides nothing, and the listing still counts as read") {
    auto source = source_over(rig, page_of({market_row("B", at_hour(-1))}));
    rig.session.fail_history_with(ErrorCategory::TRANSPORT);
    CHECK(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(unchanged());
  }

  SECTION("a name that cannot be written stops the runner and names nothing") {
    auto source = source_over(rig, page_of({market_row("B", at_hour(-1))}));
    rig.journal.fail_ = true;
    source.read_order_list(rig.runner_instance);
    CHECK(rig.owed() == runner::ObligationState::UNNAMED);
    CHECK(rig.runner_instance.local_failure().has_value());
  }

  SECTION("the ledger stays while the journal still asks about the instrument") {
    // A cancellation of unknown outcome, sent just now: its window is open, so the sweep decides nothing.
    auto cancel = serial_line("c-1", JournalOutcome::PENDING, "Z");
    cancel.action_ = runner::JournalAction::CANCEL;
    cancel.recorded_at_ = at_hour(1);
    rig.journal.append(cancel);
    rig.runner_instance.ledger().mark(samsung(), "a cancellation of unknown outcome");
    auto source = source_over(rig, page_of({market_row("B", at_hour(-1))}));
    REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
    CHECK(rig.owed() == runner::ObligationState::NAMED);
    CHECK(rig.runner_instance.ledger().requires_reconcile(samsung()));
  }
}

TEST_CASE("an order this runner sent just before is never taken for the one whose outcome is unknown",
          "[serialized][unfound]") {
  // Round 0 of the review: A is accepted and accounted for; B, of the same shape, is sent within the
  // matching window and its request is lost. A's row was the only candidate for B, B was named A, and no
  // holdings could ever account for it.
  using runner::JournalOutcome;
  auto confirmed = serial_line("p-1", JournalOutcome::CONFIRMED, "A", "10");
  SerialRig rig({
    serial_line("p-1", JournalOutcome::PENDING),
    serial_line("p-1", JournalOutcome::ACCEPTED, "A"),
    confirmed,
    serial_line("p-2", JournalOutcome::PENDING, "", "10"),
    serial_line("p-2", JournalOutcome::INDETERMINATE, "", "10"),
  });
  REQUIRE(rig.owed() == runner::ObligationState::UNNAMED);
  rig.show(polled(0, "10", {}));
  auto source = source_over(rig, page_of({market_row("A", at_hour(-1))}));
  rig.runner_instance.set_observed_at(at_hour(1));

  // The first sweep: A's row is A's, so nothing is found for B.
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  REQUIRE(source.last_reconcile().matches_.size() == 1);
  CHECK(source.last_reconcile().matches_.front().verdict_ == runner::MatchVerdict::NOT_PLACED);
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);
  // The later search: the same.
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  CHECK(rig.owed() == runner::ObligationState::UNNAMED);
  // B's own row beside A's is one candidate, not two.
  rig.session.set_history(page_of({market_row("A", at_hour(-1)), market_row("B", at_hour(-1))}));
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  REQUIRE(rig.runner_instance.obligations().size() == 1);
  CHECK(rig.runner_instance.obligations().front().order_id_ == "B");
}

TEST_CASE("a runner that overlaps orders makes no later search", "[serialized][unfound]") {
  SerialRig rig(unfound_lines(), /*recover=*/false);
  rig.runner_instance.allow_overlapping_orders();
  runner::recover_runner(rig.runner_instance, rig.journal);
  auto source = source_over(rig, page_of({market_row("B", at_hour(-1))}));
  rig.runner_instance.set_observed_at(at_hour(1));
  REQUIRE(source.read_order_list(rig.runner_instance) == runner::PollOutcome::UPDATED);
  CHECK(rig.session.history_calls() == 0);
  CHECK(rig.lines(runner::JournalOutcome::RESOLVED).size() == 1);
}
