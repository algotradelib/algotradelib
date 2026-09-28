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

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace tossinvest {

/**
 * Compile-time identity of Toss Securities, shared by both of its clients.
 *
 * Sharing one tag is what makes a reference obtained from `TossBlockingClient`
 * usable with `TossClient`, while keeping it distinct from another broker's.
 */
struct Broker {
  /** Normalized broker name. */
  static constexpr std::string_view name = "tossinvest";
};

namespace traits_detail {

/** Opaque order reference issued by the Toss adapter. */
using Ref = OrderRef<tossinvest::Broker>;

/**
 * Pure conversions shared by the asynchronous and blocking Toss traits.
 *
 * Holding them in one place is what keeps `BrokerSession<TossClient>` and
 * `BlockingSession<TossBlockingClient>` from drifting apart in how they
 * normalize the same Toss response.
 */
struct Normalizers {
  /** Both brokers publish resting orders on both sides of the book. */
  static constexpr bool has_depth = true;

  /** Both brokers publish today's price bounds. */
  static constexpr bool has_price_limits = true;

  /** Both brokers publish recent executions. */
  static constexpr bool has_trade_prints = true;

  /** Only Toss quotes several instruments in one request. */
  static constexpr bool has_batch_prices = true;

  /** Toss answers what may be spent, per currency, on a cash basis. */
  static constexpr bool has_buying_power = true;

  /**
   * Toss reports an amount and no quantity.
   *
   * KIS's inquiry answers both; asking Toss how many shares are affordable is arithmetic a caller
   * does for itself, and a port verb answering it on one broker only would be a field permanently
   * empty on the other.
   */
  static constexpr bool has_buying_power_quantity = false;

  /**
   * Toss leaves the sellable quantity off its holdings rows, and answers a per-instrument inquiry.
   *
   * So the runner fills the field only for the instruments a sell touches, and spends one request
   * each time it does.
   */
  static constexpr SellableSource sellable_source = SellableSource::INQUIRY;

  /**
   * What `/api/v1/sellable-quantity` answers about.
   *
   * The endpoint is 특정 종목의 판매 가능 수량 -- one symbol's sellable quantity -- so it answers for
   * the whole holding of the instrument asked about, which follows from the endpoint's own shape
   * rather than from an assumption. What its schema does **not** state is anything about resting
   * orders: `SellableQuantityResponse.sellableQuantity` is documented as
   * "판매 가능 수량. KR: 정수 (주 단위). US: 소수점 포함 가능 (주 단위)." -- units, and nothing else.
   *
   * So `DEDUCT` is the rule this library applies rather than a claim about Toss, for the same reason
   * KIS's holdings read carries it: deducting a resting sell the broker had also deducted refuses a
   * trade, while not deducting one sells the same shares twice. `verified_` false says nobody has
   * measured which it is. `issues/pre-trade-checks-sellable-deduction.md` is the KIS half of the
   * same open question.
   */
  static constexpr SellableBasis sellable_inquiry_basis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };

  /** Toss publishes the account's commission rates; KIS publishes none for domestic stock. */
  static constexpr bool has_commission_schedule = true;

  /** Both brokers publish static reference data for an instrument. */
  static constexpr bool has_reference_data = true;

  /** Only Toss answers for several instruments in one reference-data request. */
  static constexpr bool has_batch_reference_data = true;

  /** Toss publishes the trading calendar for both markets it serves. */
  static constexpr bool has_calendar = true;

  /** One Toss calendar request covers the requested day and the ones either side. */
  static constexpr bool has_calendar_range = false;

  /** Toss publishes intraday session windows, reachable on the concrete client. */
  static constexpr bool has_calendar_sessions = true;

  /** Refuses a calendar request with an invalid date. */
  static std::optional<BrokerError> validate_calendar(Country, std::optional<Date> from) {
    if (!from || is_valid_date(*from)) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_date";
    error.message_ = "calendar request requires a valid Gregorian date";
    return error;
  }

  /** Returns whether this broker publishes bars at `interval`. */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return interval == BarInterval::ONE_MINUTE || interval == BarInterval::ONE_DAY;
  }

  /** Refuses a bar request this broker cannot answer, before any I/O. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return tossinvest::unsupported_bar_interval(query.interval_);
  }

  /** Returns an opaque reference carrying Toss's order identifier. */
  static Ref make_ref(std::string order_id) {
    Ref ref;
    ref.primary_ = std::move(order_id);
    return ref;
  }

  /**
   * Accepts every order Toss's own client will validate.
   *
   * Toss honors `OrderRequest::client_order_id_` as a real idempotency key for
   * ten minutes, and validates its form, the price, the quantity, and the
   * validity condition itself. The port adds no second opinion, so it cannot
   * become stricter than the broker it fronts.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest&) { return std::nullopt; }

  /** Normalizes a Toss acknowledgement into an acknowledgement and a reference. */
  static Result<PlacedOrder<tossinvest::Broker>> normalize_place(Result<OrderAck> raw) {
    if (!raw) {
      return Result<PlacedOrder<tossinvest::Broker>>(std::move(raw.error()));
    }
    PlacedOrder<tossinvest::Broker> placed;
    placed.ref_ = make_ref(raw.value().order_id_);
    placed.ack_ = std::move(raw.value());
    return Result<PlacedOrder<tossinvest::Broker>>(std::move(placed));
  }

  /**
   * Normalizes a Toss `OPEN` listing into working orders.
   *
   * Toss addresses an order by one identifier and reports no amendable quantity.
   * The absence is preserved as an empty optional rather than as a zero a caller
   * could mistake for "nothing left to cancel".
   */
  static Result<WorkingOrders<tossinvest::Broker>> normalize_working_orders(
    Result<OrderPage> raw
  ) {
    if (!raw) {
      return Result<WorkingOrders<tossinvest::Broker>>(std::move(raw.error()));
    }
    WorkingOrders<tossinvest::Broker> orders;
    orders.received_at_ = raw.value().received_at_;
    orders.orders_.reserve(raw.value().orders_.size());
    for (auto& order : raw.value().orders_) {
      WorkingOrder<tossinvest::Broker> working;
      working.ref_ = make_ref(order.order_id_);
      working.order_ = std::move(order);
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders<tossinvest::Broker>>(std::move(orders));
  }

  /**
   * Converts a normalized modification into Toss's own amendment.
   *
   * Toss requires a quantity for a Korean order and forbids one for a United
   * States order, and validates that itself. Because Toss reports no amendable
   * quantity, a modification built with `replace_with()` and no explicit
   * quantity carries none; state the quantity for a Korean order.
   */
  static Result<tossinvest::TossAmendment> to_replacement(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    // Toss's `OrderModifyRequest` carries `orderType`, `quantity`, `price` and
    // the high-value flag, and **no validity field at all**: a modification
    // cannot change the condition or the session, and Toss cannot refuse a
    // request to. So this refuses on Toss's behalf, and the question is not
    // whether the requested value is one Toss supports -- it can place a
    // closing-auction order -- but whether it *differs* from what the order
    // already is.
    //
    // Which means the original has to be known. An unset `resting_` is refused
    // rather than assumed to match: a hand-built instruction naming DAY/REGULAR
    // against a resting CLS order compared equal to a defaulted original and was
    // sent, and the order stayed CLS while the caller was told it had moved.
    if (!instruction.resting_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unknown_resting_shape";
      error.message_ =
        "Toss cannot change an order's validity condition or session by amending it, and this "
        "instruction does not say what the order's are, so neither can be verified; build the "
        "instruction with reprice() or replace_with() from the working order";
      return Result<tossinvest::TossAmendment>(std::move(error));
    }
    // An unset field on the shape asks for no change, which is always safe here.
    if (instruction.shape_.time_in_force_ &&
        *instruction.shape_.time_in_force_ != instruction.resting_->time_in_force_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unsupported_time_in_force";
      error.message_ =
        "Toss cannot change an order's validity condition by amending it; cancel the order and "
        "place a new one instead";
      return Result<tossinvest::TossAmendment>(std::move(error));
    }
    if (instruction.shape_.session_ &&
        *instruction.shape_.session_ != instruction.resting_->session_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unsupported_session";
      error.message_ =
        "Toss cannot move an order to another trading session by amending it; cancel the order "
        "and place a new one instead";
      return Result<tossinvest::TossAmendment>(std::move(error));
    }

    tossinvest::TossAmendment amendment;
    amendment.order_id_ = ref.primary_;
    amendment.type_ = instruction.shape_.type_;
    amendment.quantity_ = instruction.quantity_;
    amendment.price_ = instruction.shape_.price_;
    amendment.confirm_high_value_ = instruction.confirm_high_value_;
    return Result<tossinvest::TossAmendment>(std::move(amendment));
  }

  /**
   * Converts a normalized cancellation into Toss's own instruction.
   *
   * Toss cancels an order in full and its cancel route carries an empty body, so
   * only the identifier is transmitted. A partial cancellation is refused rather
   * than widened into a full one, because widening would withdraw more of a live
   * order than the caller asked to withdraw.
   */
  static Result<tossinvest::TossAmendment> to_cancellation(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    if (!instruction.all_remaining_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "partial_cancel_unsupported";
      error.message_ =
        "Toss cancels an order in full; use cancel_all() or modify the order to a smaller "
        "quantity instead of cancelling part of it";
      return Result<tossinvest::TossAmendment>(std::move(error));
    }
    tossinvest::TossAmendment amendment;
    amendment.order_id_ = ref.primary_;
    return Result<tossinvest::TossAmendment>(std::move(amendment));
  }

  /**
   * Converts a normalized history query into a Toss `CLOSED` listing.
   *
   * Toss groups orders by lifecycle rather than by date: terminal orders are the
   * `CLOSED` scope, and orders still open belong to working orders. Every filter
   * a caller may state is expressible, so none is refused here.
   */
  static Result<tossinvest::TossOrderQuery> to_history_query(
    std::int64_t account_sequence,
    const OrderHistoryQuery& query
  ) {
    tossinvest::TossOrderQuery converted;
    converted.account_sequence_ = account_sequence;
    converted.scope_ = tossinvest::TossOrderScope::CLOSED;
    converted.symbol_ = query.symbol_;
    converted.from_ = query.from_;
    converted.to_ = query.to_;
    converted.cursor_ = query.cursor_;
    converted.limit_ = query.limit_;
    return Result<tossinvest::TossOrderQuery>(std::move(converted));
  }

  /** Returns the listing query for the orders still open on one account. */
  static tossinvest::TossOrderQuery open_query(std::int64_t account_sequence) {
    tossinvest::TossOrderQuery query;
    query.account_sequence_ = account_sequence;
    query.scope_ = tossinvest::TossOrderScope::OPEN;
    return query;
  }

  /** Returns the account selector a session binds. */
  static std::int64_t selector_of(const tossinvest::TossAccount& account) {
    return account.sequence_;
  }

  /** Returns the account number as Toss displays it. */
  static std::string display_of(const tossinvest::TossAccount& account) {
    return account.account_number_;
  }

  /** Returns the raw account type, preserved so a future value stays usable. */
  static std::string type_of(const tossinvest::TossAccount& account) {
    return account.account_type_;
  }
};

/** Capability answers shared by the asynchronous and blocking Toss traits. */
struct Capabilities {
  /** Toss lists brokerage accounts over the network. */
  static constexpr bool has_account_discovery = true;

  /** Toss honors a client order identifier as an idempotency key for ten minutes. */
  static constexpr bool has_client_order_id = true;

  /** Toss returns every pending order in its `OPEN` listing. */
  static constexpr bool has_working_orders = true;

  /** Toss reports no separately amendable quantity. */
  static constexpr bool has_amendable_quantity = false;

  /** Toss retrieves one order in any lifecycle state by identifier. */
  static constexpr bool has_order_by_id = true;

  /** Toss cancels an order in full only. */
  static constexpr bool has_partial_cancel = false;

  /** Toss accepts a listing with no date range. */
  static constexpr bool history_requires_range = false;

  /** Toss pages a `CLOSED` listing with a caller-visible cursor. */
  static constexpr bool has_history_cursor = true;

  /** Toss honours a per-page row limit on an order-history inquiry. */
  static constexpr bool has_history_limit = true;

  /** Toss filters a listing by instrument. */
  static constexpr bool has_history_symbol_filter = true;
};

} // namespace traits_detail
} // namespace tossinvest

/** Broker port adaptation for the asynchronous Toss Securities client. */
template <>
struct BrokerTraits<tossinvest::TossClient>
: tossinvest::traits_detail::Normalizers, tossinvest::traits_detail::Capabilities {
  /**
   * Toss routes nowhere, so an order reaches no particular venue.
   *
   * Not a gap: its listings carry no venue and never could, so `UNSPECIFIED` is
   * the truth about a Toss order rather than an unrecorded fact, and recovery
   * matches such an instruction on its other attributes.
   */
  static constexpr Venue effective_route(Venue) noexcept { return Venue::UNSPECIFIED; }

  /** Concrete client this specialization adapts. */
  using Client = tossinvest::TossClient;

  /** Toss application credentials. */
  using Config = tossinvest::TossConfig;

  /** Toss account selector, the numeric account sequence. */
  using Account = std::int64_t;

  /** Compile-time identity of Toss Securities. */
  using BrokerTag = tossinvest::Broker;

  /** Acknowledgement Toss returns for an order instruction. */
  using PlaceAck = OrderAck;

  /** Toss modification and cancellation instruction. */
  using Amendment = tossinvest::TossAmendment;

  /** Toss order-listing selector. */
  using HistoryQuery = tossinvest::TossOrderQuery;

  /** Toss amendable-order result. */
  using WorkingOrdersResult = OrderPage;

  /** Item type of the broker's own account listing. */
  using AccountListItem = tossinvest::TossAccount;

  /** Normalized broker name. */
  static constexpr std::string_view name = tossinvest::Broker::name;

  /** This client completes through a completion token. */
  static constexpr bool blocking = false;

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /** Forwards a reference-data request. */
  template <typename Token>
  static auto instrument(Client& client, InstrumentId subject, Token&& token) {
    return client.async_instrument(std::move(subject), std::forward<Token>(token));
  }

  /** Forwards a calendar request. */
  template <typename Token>
  static auto calendar(Client& client, Country country, std::optional<Date> from, Token&& token) {
    return client.async_calendar(country, from, std::forward<Token>(token));
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

  /** Forwards a batch quote request. */
  template <typename Token>
  static auto prices(Client& client, std::vector<InstrumentId> instruments, Token&& token) {
    return client.async_prices(std::move(instruments), std::forward<Token>(token));
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

  /** Forwards an account listing request. */
  template <typename Token>
  static auto accounts(Client& client, Token&& token) {
    return client.async_accounts(std::forward<Token>(token));
  }

  /** Forwards a buying-power request for the bound account. */
  template <typename Token>
  static auto buying_power(Client& client, const Account& account, Currency currency, Token&& token) {
    return client.async_buying_power(currency, account, std::forward<Token>(token));
  }

  /** Forwards a sellable-quantity request for one instrument. */
  template <typename Token>
  static auto sellable_quantity(
    Client& client,
    const Account& account,
    InstrumentId instrument,
    Token&& token
  ) {
    return client.async_sellable_quantity(std::move(instrument), account, std::forward<Token>(token));
  }

  /** Forwards a commission-schedule request for the bound account. */
  template <typename Token>
  static auto commissions(Client& client, const Account& account, Token&& token) {
    return client.async_commissions(account, std::forward<Token>(token));
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

  /** Forwards an order cancellation, which transmits the identifier only. */
  template <typename Token>
  static auto cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_cancel_order(
      account, std::move(amendment.order_id_), std::forward<Token>(token)
    );
  }

  /** Forwards an `OPEN` listing for the bound account. */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_orders(open_query(account), std::forward<Token>(token));
  }

  /** Forwards a `CLOSED` listing for the bound account. */
  template <typename Token>
  static auto order_history(
    Client& client,
    const Account& account,
    HistoryQuery query,
    Token&& token
  ) {
    static_cast<void>(account);
    return client.async_orders(std::move(query), std::forward<Token>(token));
  }

  /** Forwards a single-order inquiry for the bound account. */
  template <typename Token>
  static auto order_by_id(
    Client& client,
    const Account& account,
    std::string order_id,
    Token&& token
  ) {
    return client.async_order(account, std::move(order_id), std::forward<Token>(token));
  }
};

/** Broker port adaptation for the blocking Toss Securities client facade. */
template <>
struct BrokerTraits<tossinvest::TossBlockingClient>
: tossinvest::traits_detail::Normalizers, tossinvest::traits_detail::Capabilities {
  /**
   * Toss routes nowhere, so an order reaches no particular venue.
   *
   * Not a gap: its listings carry no venue and never could, so `UNSPECIFIED` is
   * the truth about a Toss order rather than an unrecorded fact, and recovery
   * matches such an instruction on its other attributes.
   */
  static constexpr Venue effective_route(Venue) noexcept { return Venue::UNSPECIFIED; }

  /** Concrete client this specialization adapts. */
  using Client = tossinvest::TossBlockingClient;

  /** Toss application credentials. */
  using Config = tossinvest::TossConfig;

  /** Toss account selector, the numeric account sequence. */
  using Account = std::int64_t;

  /** Compile-time identity of Toss Securities. */
  using BrokerTag = tossinvest::Broker;

  /** Acknowledgement Toss returns for an order instruction. */
  using PlaceAck = OrderAck;

  /** Toss modification and cancellation instruction. */
  using Amendment = tossinvest::TossAmendment;

  /** Toss order-listing selector. */
  using HistoryQuery = tossinvest::TossOrderQuery;

  /** Toss amendable-order result. */
  using WorkingOrdersResult = OrderPage;

  /** Item type of the broker's own account listing. */
  using AccountListItem = tossinvest::TossAccount;

  /** Normalized broker name. */
  static constexpr std::string_view name = tossinvest::Broker::name;

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

  /** Returns the calendar for one market. */
  static Result<TradingCalendar> calendar(Client& client, Country country, std::optional<Date> from) {
    return client.calendar(country, from);
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

  /** Returns a quote for every instrument in one request. */
  static Result<std::vector<QuoteSnapshot>> prices(
    Client& client,
    std::vector<InstrumentId> instruments
  ) {
    return client.prices(std::move(instruments));
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

  /** Returns the broker's own account listing. */
  static Result<std::vector<AccountListItem>> accounts(Client& client) {
    return client.accounts();
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

  /** Returns how much of `instrument` the bound account may sell. */
  static Result<Decimal64> sellable_quantity(
    Client& client,
    const Account& account,
    InstrumentId instrument
  ) {
    return client.sellable_quantity(std::move(instrument), account);
  }

  /** Returns the commission rates in force for the bound account. */
  static Result<CommissionSchedule> commissions(Client& client, const Account& account) {
    return client.commissions(account);
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

  /** Cancels an order for the bound account, transmitting the identifier only. */
  static Result<PlaceAck> cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.cancel_order(account, std::move(amendment.order_id_));
  }

  /** Returns the `OPEN` listing for the bound account. */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.orders(open_query(account));
  }

  /** Returns the `CLOSED` listing for the bound account. */
  static Result<OrderPage> order_history(
    Client& client,
    const Account& account,
    HistoryQuery query
  ) {
    static_cast<void>(account);
    return client.orders(std::move(query));
  }

  /** Returns one order in any lifecycle state for the bound account. */
  static Result<OrderSnapshot> order_by_id(
    Client& client,
    const Account& account,
    std::string order_id
  ) {
    return client.order(account, std::move(order_id));
  }
};

} // namespace algotradelib
