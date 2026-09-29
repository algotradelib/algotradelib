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

#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace kis {

/**
 * Compile-time identity of KIS, shared by its asynchronous and blocking clients.
 *
 * Sharing one tag is what makes a reference obtained from `KisBlockingClient`
 * usable with `KisClient`, while keeping it distinct from another broker's.
 */
struct Broker {
  /** Normalized broker name. */
  static constexpr std::string_view name = "kis";
};

namespace traits_detail {

/** Opaque order reference issued by the KIS adapter. */
using Ref = OrderRef<kis::Broker>;

/**
 * Pure conversions shared by the asynchronous and blocking KIS traits.
 *
 * Holding them in one place is what keeps `BrokerSession<KisClient>` and
 * `BlockingSession<KisBlockingClient>` from drifting apart in how they normalize
 * the same KIS response.
 */
struct Normalizers {
  /** Both brokers publish resting orders on both sides of the book. */
  static constexpr bool has_depth = true;

  /** Both brokers publish today's price bounds. */
  static constexpr bool has_price_limits = true;

  /** Both brokers publish recent executions. */
  static constexpr bool has_trade_prints = true;

  /** Only Toss quotes several instruments in one request. */
  static constexpr bool has_batch_prices = false;

  /** KIS answers what may be spent, in won, from its 매수가능조회 inquiry. */
  static constexpr bool has_buying_power = true;

  /**
   * KIS answers how many shares are affordable as well as how much money there is.
   *
   * The port asks only for the amount, because Toss reports no quantity; a caller wanting KIS's
   * per-instrument quantity gates on this and reaches it through `session.client()`.
   */
  static constexpr bool has_buying_power_quantity = true;

  /**
   * The sellable quantity arrives on every holdings row as `ord_psbl_qty`, so it costs no request.
   *
   * KIS also publishes a dedicated 매도가능수량조회 (`TTTC8408R`), which this library deliberately
   * does not implement: it returns the same field for one instrument, on an endpoint a demo account
   * cannot call. That is a statement about this library's surface, not about KIS's catalogue.
   */
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;

  /**
   * KIS publishes no commission schedule for domestic stock.
   *
   * Not "shaped differently" -- absent. The one commission sheet in its API workbook is
   * 선물옵션기간약정수수료일별, which is futures and options. A backtest against a KIS account keeps
   * the commission the caller stated, and the report says so.
   */
  static constexpr bool has_commission_schedule = false;

  /** Both brokers publish static reference data for an instrument. */
  static constexpr bool has_reference_data = true;

  /** Only Toss answers for several instruments in one reference-data request. */
  static constexpr bool has_batch_reference_data = false;

  /** KIS publishes the trading calendar, from a base date onwards. */
  static constexpr bool has_calendar = true;

  /** One KIS calendar request covers a span of dates. */
  static constexpr bool has_calendar_range = true;

  /** KIS publishes no intraday session windows. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * Refuses a calendar request KIS cannot answer.
   *
   * KIS's holiday inquiry is the domestic market's, so another country is refused
   * rather than answered with Korea's dates.
   */
  static std::optional<BrokerError> validate_calendar(Country country, std::optional<Date> from) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (country != Country::KR) {
      error.broker_code_ = "unsupported_market";
      error.message_ = "KIS publishes the domestic trading calendar only";
      return error;
    }
    if (from && !is_valid_date(*from)) {
      error.broker_code_ = "invalid_date";
      error.message_ = "calendar request requires a valid Gregorian date";
      return error;
    }
    return std::nullopt;
  }

  /** Returns whether this broker publishes bars at `interval`. */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return true;
  }

  /** Refuses a bar request this broker cannot answer, before any I/O. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return kis::unsupported_bar_interval(query.interval_);
  }

  /** Returns an opaque reference carrying KIS's order number and branch code. */
  static Ref make_ref(const KisOrderRef& order) {
    Ref ref;
    ref.primary_ = order.order_number_;
    ref.secondary_ = order.branch_number_;
    return ref;
  }

  /** Returns KIS's own reference form from an opaque reference. */
  static KisOrderRef from_ref(const Ref& ref) {
    KisOrderRef order;
    order.order_number_ = ref.primary_;
    order.branch_number_ = ref.secondary_;
    return order;
  }

  /**
   * Rejects an order KIS cannot carry out as described.
   *
   * The rule itself lives in `kis::unsupported_client_order_id`, which the client
   * also applies, so the port cannot drift from the client it fronts. The port
   * refuses earlier — before a client is even asked — and the client refuses
   * regardless of whether a caller came through the port at all.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    return kis::unsupported_client_order_id(order);
  }

  /** Normalizes a KIS acknowledgement into an acknowledgement and a reference. */
  static Result<PlacedOrder<kis::Broker>> normalize_place(Result<KisOrderAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<kis::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<kis::Broker> placed;
    placed.ack_ = std::move(raw.value().ack_);
    placed.ref_ = make_ref(raw.value().order_);
    return Result<PlacedOrder<kis::Broker>>(std::move(placed));
  }

  /**
   * Normalizes one page of KIS amendable orders.
   *
   * The reference is the row's own `odno`, which KIS documents as "주문시 한국투자
   * 증권 시스템에서 채번된 주문번호" and which is the value `order-rvsecncl` carries
   * in `ORGN_ODNO`.
   *
   * `orgn_odno` is deliberately *not* used. KIS documents it as "정정/취소주문 인경우
   * 원주문번호": it is populated only when the row is itself a correction, and it
   * names the order that correction replaced. The inquiry's own response example
   * shows a row with `odno` `0001569139`, `orgn_odno` `0001569136`, and
   * `rvse_cncl_dvsn_name` `BUY AMEND*` — amending `0001569136` would address an
   * order that is no longer live. The field is lineage, not an address, and stays
   * reachable through `KisOpenOrder::original_order_` on the concrete client.
   */
  static Result<WorkingOrders<kis::Broker>> normalize_working_orders(
    Result<KisOpenOrderPage> raw
  ) {
    if (!raw) {
      return Result<WorkingOrders<kis::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<kis::Broker> orders;
    orders.received_at_ = raw.value().received_at_;
    orders.orders_.reserve(raw.value().orders_.size());
    for (auto& open_order : raw.value().orders_) {
      WorkingOrder<kis::Broker> working;
      working.order_ = std::move(open_order.order_);
      working.ref_ = make_ref(open_order.order_ref_);
      working.amendable_quantity_ = open_order.amendable_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<kis::Broker>>(std::move(orders));
  }

  /**
   * Converts a normalized amendment into KIS's own amendment.
   *
   * KIS validates a positive whole quantity on every modification and
   * cancellation, including a full cancellation, because its
   * `order-rvsecncl` endpoint carries one request shape for both. An instruction
   * with no quantity is therefore refused rather than completed with an invented
   * one; `cancel_all()` fills it from the amendable quantity KIS reports.
   *
   * `AmendInstruction::confirm_high_value_` has no KIS counterpart and is
   * ignored, for the reason given on `validate_place`.
   */
  static Result<KisAmendment> to_amendment(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    if (!instruction.quantity_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "quantity_required";
      error.message_ =
        "KIS requires an explicit quantity on a modification or cancellation, including a full "
        "cancellation; read the amendable quantity from working orders first";
      return Result<KisAmendment>(std::move(error));
    }

    KisAmendment amendment;
    amendment.order_ = from_ref(ref);
    // What the shape leaves unset stays as it is, which the resting shape says.
    // KIS composes one `ORD_DVSN` from all three, so it cannot send an
    // amendment with a condition or session it does not know -- an unset field
    // with no resting shape to resolve it from is refused rather than defaulted,
    // because defaulting it here is how `00` got sent for an IOC order.
    const auto condition = instruction.shape_.time_in_force_
      ? instruction.shape_.time_in_force_
      : (instruction.resting_ ? instruction.resting_->time_in_force_ : std::nullopt);
    const auto session = instruction.shape_.session_
      ? instruction.shape_.session_
      : (instruction.resting_ ? instruction.resting_->session_ : std::nullopt);
    if (!condition || !session) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "incomplete_shape";
      error.message_ =
        "KIS re-sends the order code on an amendment and cannot compose one without the order's "
        "validity condition and session; state them on the shape or build the instruction with "
        "reprice() or replace_with(), which take them from the resting order";
      return Result<KisAmendment>(std::move(error));
    }
    amendment.type_ = instruction.shape_.type_;
    amendment.time_in_force_ = *condition;
    amendment.session_ = *session;
    amendment.venue_ = instruction.venue_;
    amendment.quantity_ = *instruction.quantity_;
    amendment.price_ = instruction.shape_.price_;
    amendment.trigger_price_ = amended_trigger_price(instruction);
    amendment.all_remaining_ = instruction.all_remaining_;
    return Result<KisAmendment>(std::move(amendment));
  }

  /** Converts a normalized modification into KIS's own amendment. */
  static Result<KisAmendment> to_replacement(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction);
  }

  /** Converts a normalized cancellation into KIS's own amendment. */
  static Result<KisAmendment> to_cancellation(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction);
  }

  /**
   * Converts a normalized history query into KIS's inclusive calendar range.
   *
   * KIS requires both dates, pages internally, returns every page, and this
   * adapter sends an empty `PDNO`. Each filter KIS cannot honor is refused
   * rather than dropped, so a listing never answers a different question from
   * the one asked.
   */
  static Result<KisHistoryRange> to_history_query(
    const KisAccount&,
    const OrderHistoryQuery& query
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (!query.from_ || !query.to_) {
      error.broker_code_ = "range_required";
      error.message_ = "KIS order history requires an inclusive from and to calendar date";
      return Result<KisHistoryRange>(std::move(error));
    }
    if (query.symbol_) {
      error.broker_code_ = "symbol_filter_unsupported";
      error.message_ =
        "KIS order history is not filtered by instrument by this adapter; filter the returned "
        "orders instead";
      return Result<KisHistoryRange>(std::move(error));
    }
    if (query.cursor_) {
      error.broker_code_ = "cursor_unsupported";
      error.message_ = "KIS order history pages internally and exposes no cursor";
      return Result<KisHistoryRange>(std::move(error));
    }
    if (query.limit_) {
      error.broker_code_ = "limit_unsupported";
      error.message_ = "KIS order history returns every page and cannot honor a row limit";
      return Result<KisHistoryRange>(std::move(error));
    }
    return Result<KisHistoryRange>(KisHistoryRange{*query.from_, *query.to_});
  }
};

/** Capability answers shared by the asynchronous and blocking KIS traits. */
struct Capabilities {
  /** KIS selects an account from configuration rather than listing accounts. */
  static constexpr bool has_account_discovery = false;

  /** KIS documents no client-supplied idempotency key for individual customers. */
  static constexpr bool has_client_order_id = false;

  /** KIS sends a stop-limit (`22`) order's trigger as `CNDT_PRIC`, on placement and amendment. */
  static constexpr bool has_trigger_price = true;

  /** KIS publishes its modifiable and cancellable orders, except on demo accounts. */
  static constexpr bool has_working_orders = true;

  /** KIS reports `psbl_qty`, the quantity still amendable. */
  static constexpr bool has_amendable_quantity = true;

  /**
   * 정정취소가능주문조회 states no terminal status.
   *
   * The parser derives the status from the filled quantity and can produce only `PENDING` or
   * `PARTIAL_FILLED`, and the inquiry lists what is still open to amendment. An order's outcome
   * is therefore not observable here, and a caller must read it from holdings or the history.
   */
  static constexpr bool working_orders_state_terminal = false;

  /** KIS has no single-order inquiry by order number. */
  static constexpr bool has_order_by_id = false;

  /** KIS cancels a stated quantity through `QTY_ALL_ORD_YN`. */
  static constexpr bool has_partial_cancel = true;

  /** KIS requires an inclusive calendar range on an order-history inquiry. */
  static constexpr bool history_requires_range = true;

  /** KIS follows its own continuation cursors and exposes none. */
  static constexpr bool has_history_cursor = false;

  /**
   * KIS returns every page of an order history and refuses a row limit.
   *
   * `to_history_query` rejects `OrderHistoryQuery::limit_` outright with `limit_unsupported`, so a
   * caller that sets one gets an `INVALID_ARGUMENT` before any network I/O rather than a silently
   * ignored ceiling. Anything building a history query generically has to ask this first.
   */
  static constexpr bool has_history_limit = false;

  /** This adapter sends an empty `PDNO` and exposes no instrument filter. */
  static constexpr bool has_history_symbol_filter = false;
};

} // namespace traits_detail
} // namespace kis

/**
 * Returns the venue a KIS order with `asked` actually reaches.
 *
 * KIS documents an omitted `EXCG_ID_DVSN_CD` as proceeding at the Korea
 * Exchange, so an order that names no venue is not an order that goes nowhere --
 * it goes to KRX. Recovery has to record *that*, because `UNSPECIFIED` in a
 * journal line is a positive assertion that no particular venue was reached, and
 * it confirms a row from any venue. An NXT lookalike could then be attributed to
 * an order that went to KRX.
 *
 * Shared by both KIS clients' traits, and applied where the planned order is
 * resolved rather than where the request is built, so the durable journal line
 * written *before* the send already names the venue the order will reach.
 */
inline constexpr Venue kis_effective_route(Venue asked) noexcept {
  return asked == Venue::UNSPECIFIED ? Venue::KRX : asked;
}

/** Broker port adaptation for the asynchronous KIS client. */
template <>
struct BrokerTraits<kis::KisClient>
: kis::traits_detail::Normalizers, kis::traits_detail::Capabilities {
  /** Where an order carrying `asked` actually goes; see `kis_effective_route`. */
  static constexpr Venue effective_route(Venue asked) noexcept {
    return kis_effective_route(asked);
  }

  /** Concrete client this specialization adapts. */
  using Client = kis::KisClient;

  /** KIS application credentials and environment selection. */
  using Config = kis::KisConfig;

  /** KIS account selector. */
  using Account = kis::KisAccount;

  /** Compile-time identity of KIS. */
  using BrokerTag = kis::Broker;

  /** Acknowledgement KIS returns for an order instruction. */
  using PlaceAck = kis::KisOrderAck;

  /** KIS modification and cancellation instruction. */
  using Amendment = kis::KisAmendment;

  /** KIS order-history selector. */
  using HistoryQuery = kis::KisHistoryRange;

  /** KIS amendable-order result. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kis::Broker::name;

  /** This client completes through a completion token. */
  static constexpr bool blocking = false;

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /** Forwards a reference-data request for one instrument. */
  template <typename Token>
  static auto instrument(Client& client, InstrumentId subject, Token&& token) {
    return client.async_instrument(std::move(subject), std::forward<Token>(token));
  }

  /**
   * Forwards a calendar request, supplying today when no date was given.
   *
   * KIS requires a base date, so an absent one becomes today in Seoul rather than
   * being passed through as a blank the broker would reject.
   */
  template <typename Token>
  static auto calendar(Client& client, Country, std::optional<Date> from, Token&& token) {
    return client.async_calendar(
      from ? *from : kis::today_in_seoul(), false, std::forward<Token>(token)
    );
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

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a buying-power request for the bound account. */
  template <typename Token>
  static auto buying_power(Client& client, const Account& account, Currency currency, Token&& token) {
    return client.async_buying_power(currency, account, std::forward<Token>(token));
  }

  /** Forwards a holdings request for the bound account. */
  template <typename Token>
  static auto holdings(Client& client, const Account& account, Token&& token) {
    return client.async_holdings(account, std::forward<Token>(token));
  }

  /** Forwards an order submission for the bound account. */
  template <typename Token>
  static auto place_order(Client& client, const Account& account, OrderRequest order, Token&& token) {
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
    return client.async_order_history(account, query, std::forward<Token>(token));
  }
};

/** Broker port adaptation for the blocking KIS client facade. */
template <>
struct BrokerTraits<kis::KisBlockingClient>
: kis::traits_detail::Normalizers, kis::traits_detail::Capabilities {
  /** Where an order carrying `asked` actually goes; see `kis_effective_route`. */
  static constexpr Venue effective_route(Venue asked) noexcept {
    return kis_effective_route(asked);
  }

  /** Concrete client this specialization adapts. */
  using Client = kis::KisBlockingClient;

  /** KIS application credentials and environment selection. */
  using Config = kis::KisConfig;

  /** KIS account selector. */
  using Account = kis::KisAccount;

  /** Compile-time identity of KIS. */
  using BrokerTag = kis::Broker;

  /** Acknowledgement KIS returns for an order instruction. */
  using PlaceAck = kis::KisOrderAck;

  /** KIS modification and cancellation instruction. */
  using Amendment = kis::KisAmendment;

  /** KIS order-history selector. */
  using HistoryQuery = kis::KisHistoryRange;

  /** KIS amendable-order result. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kis::Broker::name;

  /** This client drives its own event loop and returns a result directly. */
  static constexpr bool blocking = true;

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /** Returns static reference data for one instrument. */
  static Result<Instrument> instrument(Client& client, InstrumentId subject) {
    return client.instrument(std::move(subject));
  }

  /** Returns the calendar, supplying today when no date was given. */
  static Result<TradingCalendar> calendar(Client& client, Country, std::optional<Date> from) {
    return client.calendar(from ? *from : kis::today_in_seoul());
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

  /** Returns a quote. */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return client.quote(std::move(instrument));
  }

  /** Returns holdings for the bound account. */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) {
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
    return client.order_history(account, query);
  }
};

} // namespace algotradelib
