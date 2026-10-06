#pragma once

/**
 * Declares that this translation unit has reached a broker adapter.
 *
 * Read by `tests/test-strategy-isolation.cpp`; see `ATL_TRANSPORT_LAYER_PRESENT` for why the
 * layers announce themselves rather than being inferred.
 */
#define ATL_BROKER_ADAPTER_PRESENT 1

#include "./client.hpp"

#include "../../core/broker_session.hpp"
#include "../../core/broker_traits.hpp"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>

#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace kiwoom {

/**
 * Compile-time identity of Kiwoom, shared by its asynchronous and blocking clients.
 *
 * Sharing one tag is what makes a reference obtained from `KiwoomBlockingClient` usable with
 * `KiwoomClient`, while keeping it distinct from another broker's.
 */
struct Broker {
  /** Normalized broker name. */
  static constexpr std::string_view name = "kiwoom";
};

namespace traits_detail {

/** Opaque order reference issued by the Kiwoom adapter. */
using Ref = OrderRef<kiwoom::Broker>;

/**
 * Pure conversions shared by the asynchronous and blocking Kiwoom traits.
 *
 * Holding them in one place is what keeps `BrokerSession<KiwoomClient>` and
 * `BlockingSession<KiwoomBlockingClient>` from drifting apart in how they normalize the same
 * Kiwoom response.
 */
struct Normalizers {
  /** Kiwoom publishes ten levels each side, through `ka10004`. */
  static constexpr bool has_depth = true;

  /** Kiwoom publishes both bounds on its `ka10007` 시세표성정보 response. */
  static constexpr bool has_price_limits = true;

  /** Kiwoom publishes recent executions, through `ka10003`. */
  static constexpr bool has_trade_prints = true;

  /** Every Kiwoom market-data endpoint takes one `stk_cd`. */
  static constexpr bool has_batch_prices = false;

  /** Kiwoom answers what may be spent, in won, from `kt00001` 예수금상세현황요청. */
  static constexpr bool has_buying_power = true;

  /**
   * Kiwoom answers how many shares are affordable as well as how much money there is.
   *
   * **This constant asks what the *broker* publishes, not what the port's verb returns.** `kt00010`
   * 주문인출가능금액요청 publishes an orderable amount *and quantity* for each of seven 증거금률
   * tiers, for one instrument. The port asks only for an amount, because Toss reports no quantity
   * at all; a caller that wants Kiwoom's per-tier answer gates on this and reaches
   * `KiwoomClient::async_orderable_amount` through `session.client()`.
   */
  static constexpr bool has_buying_power_quantity = true;

  /**
   * **This library has no way to learn a Kiwoom sellable quantity it can stand behind yet.**
   *
   * `kt00018` reports `trde_able_qty` on every row and it would cost no request — but the port
   * does not serve that listing at all (see `holdings`), because one `dmst_stex_tp` cannot be shown
   * to cover the account. A sellable figure taken from a listing that may be missing a venue is a
   * figure the guard would enforce against the wrong number, so the source is `NONE` and not
   * `HOLDINGS`: the capability says what this library can do, not what Kiwoom publishes.
   *
   * It becomes `HOLDINGS`, unchanged, the moment
   * `issues/kiwoom-api-holdings-venue-scope.md` is answered.
   */
  static constexpr SellableSource sellable_source = SellableSource::NONE;

  /**
   * Kiwoom publishes no commission schedule.
   *
   * Not "shaped differently" -- absent. Nothing in the catalogue states the rates the account is
   * charged. It does report **charged** commission and tax per holding on `kt00018`
   * (`pur_cmsn`, `sell_cmsn`, `tax`, `sum_cmsn`) and per working order on `ka10075`
   * (`tdy_trde_cmsn`, `tdy_trde_tax`), which is a different question -- what was paid, not what the
   * rate is -- and is `plan/todo/kiwoom-holding-cost.md`.
   */
  static constexpr bool has_commission_schedule = false;

  /** Kiwoom publishes static reference data, through `ka10100` 종목정보 조회. */
  static constexpr bool has_reference_data = true;

  /**
   * One Kiwoom reference-data request covers one instrument.
   *
   * `ka10099` 종목정보 리스트 returns a whole market rather than a chosen set, which is a different
   * operation from "these two hundred symbols" and is not what this constant asks about.
   */
  static constexpr bool has_batch_reference_data = false;

  /**
   * **Kiwoom publishes no trading calendar.** Nothing in the catalogue returns trading dates or
   * holidays; the chart endpoints return dates that happened to trade, and the realtime 장시작시간
   * notice (`0s`) is a different thing again.
   *
   * So Kiwoom is the first shipped adapter to answer `false` here, and
   * `live::SessionSource::poll_calendar` reaches its no-calendar branch through a real
   * `BrokerTraits` specialization rather than a test fixture for the first time. The branch itself
   * has been compiled and tested since `ai/live-market-phase`
   * (`tests/test-live.cpp`, "a broker that publishes no calendar is asked for none").
   *
   * Decision 2b: the capability is absent, and absent is not faked. A runner bound to Kiwoom never
   * establishes a market phase and reports that rather than guessing one.
   */
  static constexpr bool has_calendar = false;

  /** There is no calendar, so there is no range. */
  static constexpr bool has_calendar_range = false;

  /** There is no calendar, so there are no session windows. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * Refuses a calendar request Kiwoom cannot answer.
   *
   * Present although `has_calendar` is false, because `BrokerCaps` is read by generic code that
   * does not know which broker it is looking at, and a trait that compiles only for the brokers
   * that happen to have the capability is the gap this function closes.
   */
  static std::optional<BrokerError> validate_calendar(Country country, std::optional<Date> from) {
    static_cast<void>(country);
    static_cast<void>(from);
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "calendar_unsupported";
    error.message_ = "Kiwoom publishes no trading calendar";
    return error;
  }

  /** Kiwoom publishes minute, daily, weekly, monthly and yearly bars. */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return true;
  }

  /** Refuses a bar request this broker cannot answer, before any I/O. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return kiwoom::unsupported_bar_interval(query.interval_);
  }

  /**
   * Refuses the normalized holdings verb until `kt00018`'s scope is established.
   *
   * **The port promises an account's holdings and Kiwoom cannot yet be shown to answer that
   * question.** `kt00018` takes `dmst_stex_tp` of `KRX` or `NXT` and publishes no all-venue value,
   * and nothing in the specification says whether that selects which positions are listed or only
   * how they are valued. Under the filtering reading, one request omits whatever is held at the
   * other venue -- and an understated holding is the *permissive* direction for a position ceiling,
   * which would then admit buys against room that does not exist, and for
   * `Portfolio::net_quantity_of`, which every exit is sized from.
   *
   * Three things were considered and rejected. Merging two requests **doubles every position**
   * under the valuation reading, which is worse in both directions that matter. Returning one
   * venue's answer through the port makes a completeness claim nobody has checked. Requiring the
   * caller to name a venue -- which this adapter also does -- is a decision about which request to
   * send, not evidence about what comes back.
   *
   * So the normalized verb refuses and **the broker-specific inquiry stays available**:
   * `session.client().holdings(KiwoomAccount{})` answers with the rows Kiwoom returned for the
   * configured venue, which is exactly what an operator needs to run the experiment at
   * `issues/kiwoom-api-holdings-venue-scope.md` and settle this. It is one request pair and about a
   * minute, and this refusal is deleted when it is done.
   */
  static Result<HoldingsSnapshot> refuse_holdings() {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "holdings_scope_unestablished";
    error.message_ =
      "Kiwoom's balance inquiry names one venue (kt00018 dmst_stex_tp: KRX or NXT) and publishes no "
      "all-venue value, and whether that filters the positions or only values them is not stated. "
      "This library will not present one venue's answer as the account's holdings unless you say "
      "that is what it is: set KiwoomConfig::holdings_position_scope_ to "
      "KiwoomPositionScope::VALUATION_ONLY to assert it, and "
      "KiwoomConfig::holdings_sellable_scope_ separately for trde_able_qty. Until then read the rows "
      "through session.client(). Both assertions are unverified; see KiwoomConfig's documentation";
    return Result<HoldingsSnapshot>(std::move(error));
  }

  /**
   * Returns an opaque reference carrying Kiwoom's order number and the order's symbol.
   *
   * **The symbol is part of the address, not decoration.** `kt10002` 정정주문 and `kt10003` 취소주문
   * both require `stk_cd` beside `orig_ord_no`, and `AmendInstruction` carries no instrument, so a
   * reference that held only the number could not be amended at all. This is the situation
   * `OrderRef`'s second part exists for — KIS puts its branch code there for the same reason — and
   * the parts stay opaque to strategy code, which stores, compares and hashes a reference and never
   * parses one.
   */
  static Ref make_ref(const KiwoomOrderRef& order) {
    Ref ref;
    ref.primary_ = order.order_number_;
    ref.secondary_ = order.symbol_;
    return ref;
  }

  /** Returns Kiwoom's own reference form from an opaque reference. */
  static KiwoomOrderRef from_ref(const Ref& ref) {
    KiwoomOrderRef order;
    order.order_number_ = ref.primary_;
    order.symbol_ = ref.secondary_;
    return order;
  }

  /**
   * Rejects an order Kiwoom cannot carry out as described.
   *
   * The rule lives in `kiwoom::unsupported_client_order_id`, which the client also applies, so the
   * port cannot drift from the client it fronts.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    // **The holdings refusal is deliberately not repeated here**, although this is the port's
    // pre-flight. It lives in `KiwoomClient::validate_order`, which every placement reaches --
    // including this port's, because `place` calls the client immediately after this returns and
    // the client refuses before any I/O. Checking it here as well would only change *which* message
    // a caller gets: this function does not validate the order's venue, price or shape, so it would
    // report the holdings gap for an order that is also malformed, burying the mistake the caller
    // can actually fix. See `kiwoom::unestablished_holdings_scope`.
    return kiwoom::unsupported_client_order_id(order);
  }

  /** Normalizes a Kiwoom acknowledgement into an acknowledgement and a reference. */
  static Result<PlacedOrder<kiwoom::Broker>> normalize_place(Result<KiwoomOrderAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<kiwoom::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<kiwoom::Broker> placed;
    placed.ack_ = std::move(raw.value().ack_);
    placed.ref_ = make_ref(raw.value().order_);
    return Result<PlacedOrder<kiwoom::Broker>>(std::move(placed));
  }

  /**
   * Normalizes one page of Kiwoom amendable orders.
   *
   * The reference is the row's own `ord_no`. `orig_ord_no` is deliberately *not* used: Kiwoom
   * documents it as 원주문번호, which is populated when the row is itself a correction and names the
   * order that correction replaced. Amending it would address an order that is no longer live. It
   * is lineage, not an address, and stays reachable through
   * `KiwoomOpenOrder::original_order_number_` — the identical distinction the KIS adapter records
   * for `orgn_odno`.
   */
  static Result<WorkingOrders<kiwoom::Broker>> normalize_working_orders(
    Result<KiwoomOpenOrderPage> raw
  ) {
    if (!raw) {
      return Result<WorkingOrders<kiwoom::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<kiwoom::Broker> orders;
    orders.received_at_ = raw.value().received_at_;
    orders.orders_.reserve(raw.value().orders_.size());
    for (auto& open_order : raw.value().orders_) {
      WorkingOrder<kiwoom::Broker> working;
      working.order_ = std::move(open_order.order_);
      working.ref_ = make_ref(open_order.order_ref_);
      working.amendable_quantity_ = open_order.amendable_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<kiwoom::Broker>>(std::move(orders));
  }

  /**
   * Converts a normalized amendment into Kiwoom's own amendment.
   *
   * **Kiwoom's amendment cannot change an order's shape, and this refuses rather than pretending
   * it can.** `kt10002` 정정주문 carries `mdfy_qty`, `mdfy_uv` and `mdfy_cond_uv` and nothing else:
   * there is no field for the price basis, the validity condition or the session. KIS re-sends
   * `ORD_DVSN` on an amendment and therefore *can* move a shape; Kiwoom cannot, so an instruction
   * asking for one of those changes is refused with `INVALID_ARGUMENT` before any I/O. Accepting it
   * and sending only the price would report success for a change that did not happen, which is the
   * exact failure `OrderShape`'s optional members were introduced to stop.
   *
   * A stated value that *equals* the resting one is not a change and is accepted, which is why
   * `AmendInstruction::resting_` is consulted rather than the stated fields alone.
   *
   * `all_remaining_` **is** representable, and this was wrong in the first version of this file:
   * Kiwoom documents the quantity `0` as "잔량 전부" on both endpoints. So an instruction that asks
   * for everything still resting sends `0` and needs no quantity at all — which is the better
   * answer, because a quantity read from a working-order listing can be stale by the time the
   * broker processes the cancellation, and Kiwoom's own `0` is evaluated when it does.
   *
   * `confirm_high_value_` has no Kiwoom counterpart, and is ignored rather than refused: it
   * acknowledges a confirmation a broker may require, and a broker that requires none has nothing
   * to acknowledge. The KIS adapter treats it the same way.
   */
  static Result<KiwoomAmendment> to_amendment(
    const Ref& ref,
    const AmendInstruction& instruction,
    bool cancel
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;

    if (!instruction.all_remaining_ && !instruction.quantity_) {
      error.broker_code_ = "quantity_required";
      error.message_ =
        "a Kiwoom modification or cancellation affects a stated quantity; set all_remaining_ to "
        "act on everything still resting, or build the instruction with cancel_all()";
      return Result<KiwoomAmendment>(std::move(error));
    }
    if (auto refusal = refuse_shape_change(instruction, cancel)) {
      return Result<KiwoomAmendment>(std::move(*refusal));
    }

    KiwoomAmendment amendment;
    amendment.order_ = from_ref(ref);
    amendment.venue_ = instruction.venue_;
    // Kiwoom's own "everything still resting", which it evaluates when it processes the
    // instruction rather than when the caller read the listing.
    amendment.quantity_ =
      instruction.all_remaining_ ? Decimal64() : *instruction.quantity_;
    if (!cancel) {
      // The basis is known here and nowhere after: `kt10002` carries no `trde_tp`, so this is the
      // last point that can tell a trigger the order wants from one it would silently lose.
      const auto trigger = amended_trigger_price(instruction);
      if (auto invalid = kiwoom::invalid_trigger(instruction.shape_.type_, trigger)) {
        return Result<KiwoomAmendment>(std::move(*invalid));
      }
      amendment.price_ = instruction.shape_.price_;
      amendment.condition_price_ = trigger;
    }
    // **Never on a cancellation.** `kt10003` has no trigger field and the client refuses a
    // cancellation carrying one; `cancel_all` restates the resting trigger, so copying it here would
    // make every stop order impossible to withdraw through the port.
    return Result<KiwoomAmendment>(std::move(amendment));
  }

  /**
   * Refuses an instruction that asks Kiwoom to change something its amendment cannot carry.
   *
   * A field the caller left unset means "leave it as it is", so it is never a change. A field the
   * caller set is a change only when it differs from the resting shape — and when the resting shape
   * is unknown, a stated value cannot be shown to be a no-op, so it is refused. That is the
   * conservative direction: the alternative accepts a request to move a shape and silently does
   * not move it.
   */
  static std::optional<BrokerError> refuse_shape_change(
    const AmendInstruction& instruction,
    bool cancel
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "shape_change_unsupported";

    const auto refuse = [&error, cancel](const char* what) {
      error.message_ = cancel ?
        std::string("Kiwoom's cancellation carries a quantity and nothing else, so it cannot "
                    "change an order's ") + what + " on the way out" :
        std::string("Kiwoom's amendment endpoints carry a quantity and a price only, so they "
                    "cannot change an order's ") + what +
          "; cancel the order and place a new one instead";
      return error;
    };

    // An unset field means "leave it as it is", so it is never a change. A set field is a change
    // only when it differs from what the order already is -- and a set field with no resting shape
    // to compare against cannot be shown to be a restatement, so it is refused rather than assumed
    // harmless.
    const auto differs = [](auto stated, auto current) {
      return stated.has_value() && (!current.has_value() || *stated != *current);
    };

    if (cancel) {
      // **A cancellation transmits a quantity and nothing else**, so every other stated field is
      // one Kiwoom cannot act on. The price and the trigger are refused because acting on them is
      // what a caller would expect; these three are refused for the same reason and it is not a
      // lesser one -- reporting acceptance for a stated change that never reached the broker is
      // the defect, whatever the field.
      //
      // `cancel_all()` and `cancel_quantity()` copy the resting order's whole shape into both
      // `shape_` and `resting_`, so every comparison below sees a restatement and the ordinary
      // withdrawal path is unaffected.
      const bool restates_price = instruction.resting_ &&
        instruction.resting_->price_ == instruction.shape_.price_;
      if (instruction.shape_.price_ && !restates_price) {
        error.broker_code_ = "price_on_cancellation";
        error.message_ =
          "Kiwoom's cancellation carries a quantity and no price, so it cannot reprice an order on "
          "the way out; cancel it and place a new one instead";
        return error;
      }
      // The same rule as the price: a restatement of the resting trigger -- which is what
      // `cancel_all` writes -- is the ordinary withdrawal, and only a stated *change* is refused.
      const bool restates_trigger = instruction.resting_ &&
        instruction.resting_->trigger_price_ == instruction.shape_.trigger_price_;
      if (instruction.shape_.trigger_price_ && !restates_trigger) {
        error.broker_code_ = "trigger_on_cancellation";
        error.message_ =
          "Kiwoom's cancellation carries a quantity and no trigger price, so it cannot move an "
          "order's stop on the way out; cancel it and place a new one instead";
        return error;
      }
    } else if (!instruction.resting_) {
      // **A modification with no resting shape is refused, and that is the conservative
      // direction.** `OrderShape::type_` is a plain value defaulting to `LIMIT`, so this adapter
      // cannot tell a caller who *means* `LIMIT` from one who left it alone -- and if the resting
      // order is a best-limit order, accepting the instruction would send a price and leave the
      // basis where it was, reporting success for a change that did not happen. KIS refuses the
      // same case for the same reason, as `incomplete_shape`.
      error.broker_code_ = "incomplete_shape";
      error.message_ =
        "Kiwoom's modification cannot change an order's price basis, validity condition or session, "
        "and without the order's current shape this adapter cannot tell a restatement from a "
        "change; build the instruction with reprice() or replace_with(), which take it from the "
        "resting order";
      return error;
    }

    const auto resting_condition = instruction.resting_ ?
      instruction.resting_->time_in_force_ :
      std::optional<TimeInForce>();
    const auto resting_session = instruction.resting_ ?
      instruction.resting_->session_ :
      std::optional<OrderSession>();
    if (differs(instruction.shape_.time_in_force_, resting_condition)) {
      return refuse("validity condition");
    }
    if (differs(instruction.shape_.session_, resting_session)) {
      return refuse("session");
    }
    // `OrderShape::type_` is a plain value rather than an optional, so "left alone" and "stated
    // `LIMIT`" are the same bytes and the comparison has to be made in two steps.
    if (instruction.resting_) {
      if (instruction.shape_.type_ != instruction.resting_->type_) {
        return refuse("price basis");
      }
    } else if (instruction.shape_.type_ != OrderShape().type_) {
      // **With no resting shape, a value that differs from the default is still a stated change.**
      // Only the default is ambiguous; `MARKET` on a cancellation is a caller saying something, and
      // `kt10003` transmits no basis, so accepting it would report acceptance for a change the
      // broker never saw. Compared against `OrderShape`'s own default rather than a literal, so the
      // two cannot drift apart.
      return refuse("price basis");
    }
    return std::nullopt;
  }

  /** Converts a normalized modification into Kiwoom's own amendment. */
  static Result<KiwoomAmendment> to_replacement(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction, false);
  }

  /** Converts a normalized cancellation into Kiwoom's own amendment. */
  static Result<KiwoomAmendment> to_cancellation(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction, true);
  }

  /**
   * Converts a normalized history query into Kiwoom's single-date inquiry.
   *
   * **`kt00007` takes one 주문일자 and no range**, so a query spanning more than one day is refused
   * rather than answered with one day of it. Walking the range request by request is not what this
   * adapter does: multi-request aggregation brings its own paging and partial-failure semantics —
   * what a listing means when the third day fails — and none of that is part of the contract this
   * verb states. A caller that wants a week asks seven times and decides for itself.
   *
   * `symbol_` is passed through as `stk_cd`, which Kiwoom documents as a filter, rather than being
   * applied locally to a listing that answered a different question.
   */
  static Result<KiwoomHistoryQuery> to_history_query(
    const KiwoomAccount&,
    const OrderHistoryQuery& query
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (!query.from_) {
      error.broker_code_ = "date_required";
      error.message_ = "Kiwoom's order-history inquiry lists one calendar date, which must be given";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.to_ && !(query.to_->year_ == query.from_->year_ &&
                       query.to_->month_ == query.from_->month_ &&
                       query.to_->day_ == query.from_->day_)) {
      error.broker_code_ = "single_date_only";
      error.message_ =
        "Kiwoom's order-history inquiry lists one calendar date; ask for each date separately "
        "rather than for a range";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.cursor_) {
      error.broker_code_ = "cursor_unsupported";
      error.message_ = "Kiwoom order history pages internally and exposes no cursor";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    if (query.limit_) {
      error.broker_code_ = "limit_unsupported";
      error.message_ = "Kiwoom order history documents no row limit and cannot honor one";
      return Result<KiwoomHistoryQuery>(std::move(error));
    }
    KiwoomHistoryQuery converted;
    converted.date_ = *query.from_;
    converted.symbol_ = query.symbol_ ? *query.symbol_ : std::string();
    return Result<KiwoomHistoryQuery>(std::move(converted));
  }

  /**
   * Returns the account selector a session binds, which carries nothing.
   *
   * No Kiwoom endpoint takes an account number; the credential is the account. See
   * `KiwoomAccount`.
   */
  static KiwoomAccount selector_of(const KiwoomAccountInfo&) { return KiwoomAccount{}; }

  /** Returns the account number as Kiwoom displays it. */
  static std::string display_of(const KiwoomAccountInfo& account) {
    return account.account_number_;
  }

  /**
   * Returns the account type, which Kiwoom does not publish.
   *
   * `ka00001` returns `acctNo` and nothing else. Empty means the broker stated no type, which is
   * what the field is for; inventing one would be worse than saying nothing.
   */
  static std::string type_of(const KiwoomAccountInfo&) { return {}; }
};

/** Capability answers shared by the asynchronous and blocking Kiwoom traits. */
struct Capabilities {
  /**
   * Kiwoom publishes the one account its credential drives, through `ka00001`.
   *
   * True even though the selector is empty and there is nothing to choose between: the question is
   * whether the broker lists accounts over the network, and it does. A caller that wants to check
   * it is pointed at the account it meant has no other way to find out.
   */
  static constexpr bool has_account_discovery = true;

  /** Kiwoom documents no caller-supplied order identifier. */
  static constexpr bool has_client_order_id = false;

  /** Kiwoom sends a stop-limit (`28`) order's trigger as `cond_uv`, and a modified one as `mdfy_cond_uv`. */
  static constexpr bool has_trigger_price = true;

  /** A broker that takes orders, not a market-data source; see `BrokerCaps::data_only`. */
  static constexpr bool data_only = false;

  /** Kiwoom publishes its unfilled orders, through `ka10075`. */
  static constexpr bool has_working_orders = true;

  /** Kiwoom reports `oso_qty` 미체결수량, which is what remains amendable. */
  static constexpr bool has_amendable_quantity = true;

  /**
   * `ka10075` 미체결 states no terminal status this library will stand behind.
   *
   * The specification documents `ord_stt` on this endpoint with the single value `접수`, and the
   * listing is by definition the unfilled one. `normalized_order_status` accepts `체결` and `취소`
   * because it is shared with the history parser, which is where those belong -- and a parser able
   * to represent a terminal status is evidence about the parser, never about what the endpoint
   * sends. Answering `true` on that evidence would leave a caller waiting for an event that may
   * never arrive, so this stays `false` until an official statement or an observed payload says
   * otherwise.
   */
  static constexpr bool working_orders_state_terminal = false;

  /**
   * Kiwoom has no single-order inquiry.
   *
   * `kt00007`'s `fr_ord_no` is documented as a lower bound — "시작주문번호 입력 시 이전 주문은
   * 조회되지 않음" — so it selects a suffix of the day's orders rather than addressing one.
   */
  static constexpr bool has_order_by_id = false;

  /** Kiwoom cancels a stated quantity through `kt10003`'s `cncl_qty`. */
  static constexpr bool has_partial_cancel = true;

  /** Kiwoom's order-history inquiry takes one date, not a range. */
  static constexpr bool history_requires_range = false;

  /** Kiwoom follows its own `cont-yn` / `next-key` continuation and exposes none. */
  static constexpr bool has_history_cursor = false;

  /** Kiwoom documents no row limit on an order-history inquiry. */
  static constexpr bool has_history_limit = false;

  /** `kt00007` takes `stk_cd`, so the filter is the broker's rather than applied locally. */
  static constexpr bool has_history_symbol_filter = true;
};

} // namespace traits_detail
} // namespace kiwoom

/**
 * Returns the venue a Kiwoom order with `asked` actually reaches.
 *
 * **Kiwoom documents `dmst_stex_tp` with no default**, unlike KIS, whose sheet states that an
 * omitted `EXCG_ID_DVSN_CD` proceeds at KRX. So there is no route this library could record for an
 * unspecified venue, and `KiwoomClient::validate_venue` refuses one before any I/O rather than
 * choosing. This function exists to say that in the one place the port asks: an unspecified venue
 * resolves to itself, and a journal line therefore never claims an order went somewhere this
 * library picked.
 */
inline constexpr Venue kiwoom_route(Venue asked) noexcept { return kiwoom::kiwoom_effective_route(asked); }

/** Broker port adaptation for the asynchronous Kiwoom client. */
template <>
struct BrokerTraits<kiwoom::KiwoomClient>
: kiwoom::traits_detail::Normalizers, kiwoom::traits_detail::Capabilities {
  /** Where an order carrying `asked` actually goes; see `kiwoom_route`. */
  static constexpr Venue effective_route(Venue asked) noexcept { return kiwoom_route(asked); }

  /** Concrete client this specialization adapts. */
  using Client = kiwoom::KiwoomClient;

  /** Kiwoom application credentials and environment selection. */
  using Config = kiwoom::KiwoomConfig;

  /** Kiwoom account selector, which carries nothing because Kiwoom takes nothing. */
  using Account = kiwoom::KiwoomAccount;

  /** Item type of the broker's own account listing. */
  using AccountListItem = kiwoom::KiwoomAccountInfo;

  /** Compile-time identity of Kiwoom. */
  using BrokerTag = kiwoom::Broker;

  /** Acknowledgement Kiwoom returns for an order instruction. */
  using PlaceAck = kiwoom::KiwoomOrderAck;

  /** Kiwoom modification and cancellation instruction. */
  using Amendment = kiwoom::KiwoomAmendment;

  /** Kiwoom order-history selector. */
  using HistoryQuery = kiwoom::KiwoomHistoryQuery;

  /** Kiwoom amendable-order result. */
  using WorkingOrdersResult = kiwoom::KiwoomOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kiwoom::Broker::name;

  /** This client completes through a completion token. */
  static constexpr bool blocking = false;

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a price-bound request. */
  template <typename Token>
  static auto price_limits(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_price_limits(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a recent-execution request. */
  template <typename Token>
  static auto trades(Client& client, InstrumentId instrument, int count, Token&& token) {
    return client.async_trades(std::move(instrument), count, std::forward<Token>(token));
  }

  /** Forwards an order book request. */
  template <typename Token>
  static auto depth(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_depth(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a bar request. */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }

  /** Forwards a reference-data request. */
  template <typename Token>
  static auto instrument(Client& client, InstrumentId subject, Token&& token) {
    return client.async_instrument(std::move(subject), std::forward<Token>(token));
  }

  /** Forwards an account listing request. */
  template <typename Token>
  static auto accounts(Client& client, Token&& token) {
    return client.async_accounts(std::forward<Token>(token));
  }

  /** Forwards a buying-power request for the bound account. */
  template <typename Token>
  static auto buying_power(
    Client& client,
    const Account& account,
    Currency currency,
    Token&& token
  ) {
    return client.async_buying_power(currency, account, std::forward<Token>(token));
  }

  /**
   * Serves the normalized holdings verb once the operator has asserted what the venue means.
   *
   * Refuses before any I/O while the scope is unstated -- see `refuse_holdings`, which is the
   * behaviour this adapter shipped with and stays the default. An assertion is what turns it on, and
   * `KiwoomConfig::holdings_position_scope_` documents that asserting is not knowing.
   *
   * The refusal is posted to the client's strand and then dispatched on the handler's own
   * executor, which is the path every other completion of this client takes. Posting to the
   * handler's executor directly does not compile once it is Asio's `inline_executor` -- the default
   * a handler naming no executor carries in recent Boost, and what `BrokerSession` binds a plain
   * callback to -- and posting to `system_executor` instead would complete outside the
   * `io_context` running this client, which does not count it as work.
   *
   * **Work is held on the handler's executor from initiation until the dispatch**, as an Asio
   * operation does. Without it a caller whose handler is bound to a *different* `io_context` can
   * run that context to completion before the client's strand has posted anything, and the refusal
   * then lands in a stopped context and is never delivered. `prefer` rather than `require`, because
   * an executor that tracks nothing -- `inline_executor` -- has no work to hold.
   */
  template <typename Token>
  static auto holdings(Client& client, const Account& account, Token&& token) {
    if (client.config().holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED) {
      static_cast<void>(account);
      return net::async_initiate<Token, void(Result<HoldingsSnapshot>)>(
        [strand = client.get_executor()](auto&& handler) mutable {
          auto work = net::prefer(
            net::get_associated_executor(handler), net::execution::outstanding_work.tracked
          );
          net::post(
            strand,
            [handler = std::forward<decltype(handler)>(handler), work = std::move(work)]() mutable {
              net::dispatch(work, [handler = std::move(handler)]() mutable {
                std::move(handler)(refuse_holdings());
              });
            }
          );
        },
        token
      );
    }
    return client.async_holdings(account, std::forward<Token>(token));
  }

  /** Forwards an order submission for the bound account. */
  template <typename Token>
  static auto place_order(
    Client& client,
    const Account& account,
    OrderRequest order,
    Token&& token
  ) {
    return client.async_place_order(account, std::move(order), std::forward<Token>(token));
  }

  /** Forwards an order modification for the bound account. */
  template <typename Token>
  static auto replace_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_replace_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /** Forwards an order cancellation for the bound account. */
  template <typename Token>
  static auto cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_cancel_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /** Forwards an amendable-order inquiry for the bound account. */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_open_orders(account, std::forward<Token>(token));
  }

  /** Forwards an order-history inquiry for the bound account. */
  template <typename Token>
  static auto order_history(
    Client& client,
    const Account& account,
    HistoryQuery query,
    Token&& token
  ) {
    return client.async_order_history(account, std::move(query), std::forward<Token>(token));
  }
};

/** Broker port adaptation for the blocking Kiwoom client facade. */
template <>
struct BrokerTraits<kiwoom::KiwoomBlockingClient>
: kiwoom::traits_detail::Normalizers, kiwoom::traits_detail::Capabilities {
  /** Where an order carrying `asked` actually goes; see `kiwoom_route`. */
  static constexpr Venue effective_route(Venue asked) noexcept { return kiwoom_route(asked); }

  /** Concrete client this specialization adapts. */
  using Client = kiwoom::KiwoomBlockingClient;

  /** Kiwoom application credentials and environment selection. */
  using Config = kiwoom::KiwoomConfig;

  /** Kiwoom account selector, which carries nothing because Kiwoom takes nothing. */
  using Account = kiwoom::KiwoomAccount;

  /** Item type of the broker's own account listing. */
  using AccountListItem = kiwoom::KiwoomAccountInfo;

  /** Compile-time identity of Kiwoom. */
  using BrokerTag = kiwoom::Broker;

  /** Acknowledgement Kiwoom returns for an order instruction. */
  using PlaceAck = kiwoom::KiwoomOrderAck;

  /** Kiwoom modification and cancellation instruction. */
  using Amendment = kiwoom::KiwoomAmendment;

  /** Kiwoom order-history selector. */
  using HistoryQuery = kiwoom::KiwoomHistoryQuery;

  /** Kiwoom amendable-order result. */
  using WorkingOrdersResult = kiwoom::KiwoomOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kiwoom::Broker::name;

  /** This client drives its own event loop and returns a result directly. */
  static constexpr bool blocking = true;

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /** Returns a quote. */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return client.quote(std::move(instrument));
  }

  /** Returns today's price bounds. */
  static Result<PriceLimits> price_limits(Client& client, InstrumentId instrument) {
    return client.price_limits(std::move(instrument));
  }

  /** Returns recent executions. */
  static Result<std::vector<TradePrint>> trades(
    Client& client,
    InstrumentId instrument,
    int count
  ) {
    return client.trades(std::move(instrument), count);
  }

  /** Returns the order book. */
  static Result<DepthSnapshot> depth(Client& client, InstrumentId instrument) {
    return client.depth(std::move(instrument));
  }

  /** Returns bars, paging inside the client. */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    return client.bars(std::move(query));
  }

  /** Returns static reference data for one instrument. */
  static Result<Instrument> instrument(Client& client, InstrumentId subject) {
    return client.instrument(std::move(subject));
  }

  /** Returns the one account the bound credential drives. */
  static Result<std::vector<AccountListItem>> accounts(Client& client) {
    return client.accounts();
  }

  /** Serves the normalized holdings verb once a scope is asserted; see the asynchronous thunk. */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) {
    if (client.config().holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED) {
      static_cast<void>(account);
      return refuse_holdings();
    }
    return client.holdings(account);
  }

  /** Returns what the bound account may spend in `currency`. */
  static Result<BuyingPower> buying_power(
    Client& client,
    const Account& account,
    Currency currency
  ) {
    return client.buying_power(currency, account);
  }

  /** Submits an order for the bound account. */
  static Result<PlaceAck> place_order(Client& client, const Account& account, OrderRequest order) {
    return client.place_order(account, std::move(order));
  }

  /** Modifies an order for the bound account. */
  static Result<PlaceAck> replace_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.replace_order(account, std::move(amendment));
  }

  /** Cancels an order for the bound account. */
  static Result<PlaceAck> cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.cancel_order(account, std::move(amendment));
  }

  /** Returns the amendable orders for the bound account. */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.open_orders(account);
  }

  /** Returns order history for the bound account. */
  static Result<OrderPage> order_history(
    Client& client,
    const Account& account,
    HistoryQuery query
  ) {
    return client.order_history(account, std::move(query));
  }
};

} // namespace algotradelib
