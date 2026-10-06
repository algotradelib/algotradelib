#pragma once

/**
 * @brief Defined as 1 once a broker adapter header is included.
 *
 * The library's tests use it to check that the strategy headers include no broker.
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
 * @brief The KIS broker tag: order references are typed by it, so a KIS reference works with both
 *   KIS clients and with no other broker.
 */
struct Broker {
  /** @brief The broker's name, `"kis"`. */
  static constexpr std::string_view name = "kis";
};

namespace traits_detail {

/** @brief A KIS order reference (`primary_` is the order number, `secondary_` the branch number). */
using Ref = OrderRef<kis::Broker>;

/** @brief The capabilities and conversions shared by KIS's asynchronous and blocking traits. */
struct Normalizers {
  /** @brief `true`: KIS publishes the order book. */
  static constexpr bool has_depth = true;

  /** @brief `true`: KIS publishes today's price band. */
  static constexpr bool has_price_limits = true;

  /** @brief `true`: KIS publishes recent trades. */
  static constexpr bool has_trade_prints = true;

  /** @brief `false`: one quote per request. */
  static constexpr bool has_batch_prices = false;

  /** @brief `true`: KIS answers buying power, in won (매수가능조회). */
  static constexpr bool has_buying_power = true;

  /**
   * @brief `true`: KIS also answers how many shares are affordable; reach it through
   *   `session.client()`, since the session asks only for the amount.
   */
  static constexpr bool has_buying_power_quantity = true;

  /** @brief The sellable quantity comes with each holdings row (`ord_psbl_qty`), at no extra request. */
  static constexpr SellableSource sellable_source = SellableSource::HOLDINGS;

  /**
   * @brief `false`: KIS publishes no commission schedule for domestic stocks; a backtest uses the
   *   commission you state.
   */
  static constexpr bool has_commission_schedule = false;

  /** @brief `true`: KIS publishes instrument reference data. */
  static constexpr bool has_reference_data = true;

  /** @brief `false`: one instrument per reference-data request. */
  static constexpr bool has_batch_reference_data = false;

  /** @brief `true`: KIS publishes the trading calendar from a date on. */
  static constexpr bool has_calendar = true;

  /** @brief `true`: one calendar request covers a span of dates. */
  static constexpr bool has_calendar_range = true;

  /** @brief `false`: KIS publishes no session hours. */
  static constexpr bool has_calendar_sessions = false;

  /**
   * @brief Refuses a calendar request for a market other than Korea's (`unsupported_market`) or
   *   with an invalid date (`invalid_date`).
   * @param country The market.
   * @param from The first date.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
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

  /**
   * @brief Tests whether KIS publishes bars at an interval.
   * @param interval The interval.
   * @return `true`: KIS publishes every interval.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return true;
  }

  /**
   * @brief Refuses a bar request this broker cannot answer, before any I/O.
   * @param query The query.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return kis::unsupported_bar_interval(query.interval_);
  }

  /**
   * @brief Makes an order reference from KIS's.
   * @param order KIS's reference.
   * @return The reference.
   */
  static Ref make_ref(const KisOrderRef& order) {
    Ref ref;
    ref.primary_ = order.order_number_;
    ref.secondary_ = order.branch_number_;
    return ref;
  }

  /**
   * @brief Returns KIS's reference from an order reference.
   * @param ref The reference.
   * @return KIS's reference.
   */
  static KisOrderRef from_ref(const Ref& ref) {
    KisOrderRef order;
    order.order_number_ = ref.primary_;
    order.branch_number_ = ref.secondary_;
    return order;
  }

  /**
   * @brief Refuses an order KIS cannot place as described (one with a client order id; see
   *   `kis::unsupported_client_order_id`).
   * @param order The order.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    return kis::unsupported_client_order_id(order);
  }

  /**
   * @brief Normalizes a KIS acknowledgement into an acknowledgement and a reference.
   * @param raw The client's result.
   * @return The result, or an error.
   */
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
   * @brief Converts KIS's amendable-order list into working orders.
   *
   * Each reference is the row's own order number (`odno`), not `orgn_odno` (the order an amendment
   * replaced; see `KisOpenOrder::original_order_`).
   *
   * @param raw The client's result.
   * @return The working orders, or the error.
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
   * @brief Converts an amendment or cancellation into KIS's form.
   *
   * KIS requires a positive whole quantity even for a full cancellation, so an instruction without a
   * quantity is refused (`cancel_all()` fills it from the amendable quantity). KIS re-sends the whole
   * shape, so an instruction whose condition or session is unset is refused (`incomplete_shape`)
   * unless `resting_` supplies them (`reprice()`, `replace_with()` and `cancel_all()` do).
   * `confirm_high_value_` is ignored.
   *
   * @param ref The order's reference.
   * @param instruction The instruction.
   * @return KIS's amendment, or an `INVALID_ARGUMENT` error.
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

  /**
   * @brief Converts a normalized modification into KIS's own amendment.
   * @param ref The order's reference.
   * @param instruction The amendment or cancellation.
   * @return The result, or an error.
   */
  static Result<KisAmendment> to_replacement(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction);
  }

  /**
   * @brief Converts a normalized cancellation into KIS's own amendment.
   * @param ref The order's reference.
   * @param instruction The amendment or cancellation.
   * @return The result, or an error.
   */
  static Result<KisAmendment> to_cancellation(
    const Ref& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction);
  }

  /**
   * @brief Converts an order-history query into KIS's date range.
   *
   * KIS requires both dates and returns every page; a filter it cannot apply (a row limit, a cursor,
   * an instrument) is refused rather than ignored.
   *
   * @param query The query.
   * @return The range, or an `INVALID_ARGUMENT` error.
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

/** @brief The order-related capabilities shared by KIS's asynchronous and blocking traits. */
struct Capabilities {
  /** @brief `false`: the account comes from configuration, not a listing. */
  static constexpr bool has_account_discovery = false;

  /** @brief `false`: no idempotency key for individual customers. */
  static constexpr bool has_client_order_id = false;

  /** @brief `true`: a stop-limit order's trigger is sent as `CNDT_PRIC`, on placement and amendment. */
  static constexpr bool has_trigger_price = true;

  /** @brief `false`: a broker that takes orders; see `BrokerCaps::data_only`. */
  static constexpr bool data_only = false;

  /** @brief `true`: KIS lists amendable orders (on demo, from today's order history). */
  static constexpr bool has_working_orders = true;

  /**
   * @brief `true`: KIS reports the amendable quantity (`psbl_qty`; on demo the remaining quantity,
   *   which can differ while an amendment is pending).
   */
  static constexpr bool has_amendable_quantity = true;

  /**
   * @brief `false`: the amendable-order list shows only open orders (`PENDING` or `PARTIAL_FILLED`);
   *   read an order's outcome from holdings or the order history.
   */
  static constexpr bool working_orders_state_terminal = false;

  /** @brief `false`: no inquiry of one order by number. */
  static constexpr bool has_order_by_id = false;

  /** @brief `true`: part of an order can be cancelled. */
  static constexpr bool has_partial_cancel = true;

  /** @brief `true`: an order-history query must give a date range. */
  static constexpr bool history_requires_range = true;

  /** @brief `false`: the client reads every page itself and exposes no cursor. */
  static constexpr bool has_history_cursor = false;

  /**
   * @brief `false`: an order-history query with a row limit is refused (`limit_unsupported`) before
   *   sending; KIS returns every page.
   */
  static constexpr bool has_history_limit = false;

  /** @brief `false`: an order-history query cannot filter by instrument. */
  static constexpr bool has_history_symbol_filter = false;
};

} // namespace traits_detail
} // namespace kis

/**
 * @brief Returns the venue a KIS order actually reaches.
 * @param asked The order's venue.
 * @return `KRX` if `asked` is `UNSPECIFIED` (KIS's documented default), otherwise `asked`.
 */
inline constexpr Venue kis_effective_route(Venue asked) noexcept {
  return asked == Venue::UNSPECIFIED ? Venue::KRX : asked;
}

/** @brief The `BrokerSession` adapter for `kis::KisClient`. */
template <>
struct BrokerTraits<kis::KisClient>
: kis::traits_detail::Normalizers, kis::traits_detail::Capabilities {
  /**
   * @brief Where an order carrying `asked` actually goes; see `kis_effective_route`.
   * @param asked The order's venue.
   * @return The venue.
   */
  static constexpr Venue effective_route(Venue asked) noexcept {
    return kis_effective_route(asked);
  }

  /** @brief The client type. */
  using Client = kis::KisClient;

  /** @brief The configuration type. */
  using Config = kis::KisConfig;

  /** @brief The account type. */
  using Account = kis::KisAccount;

  /** @brief The broker tag. */
  using BrokerTag = kis::Broker;

  /** @brief The order acknowledgement type. */
  using PlaceAck = kis::KisOrderAck;

  /** @brief The amendment type. */
  using Amendment = kis::KisAmendment;

  /** @brief The order-history query type. */
  using HistoryQuery = kis::KisHistoryRange;

  /** @brief The working-order result type. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** @brief The broker's name. */
  static constexpr std::string_view name = kis::Broker::name;

  /** @brief `false`: requests complete through a completion token. */
  static constexpr bool blocking = false;

  /**
   * @brief Returns this client's pacing statistics, or null when it is unpaced.
   * @param client The client.
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /**
   * @brief Forwards a reference-data request for one instrument.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param subject The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto instrument(Client& client, InstrumentId subject, Token&& token) {
    return client.async_instrument(std::move(subject), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a calendar request (the market argument is ignored; `validate_calendar` checks
   *   it).
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param from The first date (today in Korea if empty).
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto calendar(Client& client, Country, std::optional<Date> from, Token&& token) {
    return client.async_calendar(
      from ? *from : kis::today_in_seoul(), false, std::forward<Token>(token)
    );
  }

  /**
   * @brief Forwards a price-bound request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto price_limits(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_price_limits(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a recent-execution request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto trades(Client& client, InstrumentId instrument, int count, Token&& token) {
    return client.async_trades(std::move(instrument), count, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order book request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto depth(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_depth(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a bar request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param query The query.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a quote request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a buying-power request for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param currency The currency.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto buying_power(Client& client, const Account& account, Currency currency, Token&& token) {
    return client.async_buying_power(currency, account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards a holdings request for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto holdings(Client& client, const Account& account, Token&& token) {
    return client.async_holdings(account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order submission for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param order The order.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto place_order(Client& client, const Account& account, OrderRequest order, Token&& token) {
    return client.async_place_order(account, std::move(order), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order modification for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto replace_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_replace_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order cancellation for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment,
    Token&& token
  ) {
    return client.async_cancel_order(account, std::move(amendment), std::forward<Token>(token));
  }

  /**
   * @brief Forwards an amendable-order inquiry for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_open_orders(account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards an order-history inquiry for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param query The query.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
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

/** @brief The `BlockingSession` adapter for `kis::KisBlockingClient`. */
template <>
struct BrokerTraits<kis::KisBlockingClient>
: kis::traits_detail::Normalizers, kis::traits_detail::Capabilities {
  /**
   * @brief Where an order carrying `asked` actually goes; see `kis_effective_route`.
   * @param asked The order's venue.
   * @return The venue.
   */
  static constexpr Venue effective_route(Venue asked) noexcept {
    return kis_effective_route(asked);
  }

  /** @brief The client type. */
  using Client = kis::KisBlockingClient;

  /** @brief The configuration type. */
  using Config = kis::KisConfig;

  /** @brief The account type. */
  using Account = kis::KisAccount;

  /** @brief The broker tag. */
  using BrokerTag = kis::Broker;

  /** @brief The order acknowledgement type. */
  using PlaceAck = kis::KisOrderAck;

  /** @brief The amendment type. */
  using Amendment = kis::KisAmendment;

  /** @brief The order-history query type. */
  using HistoryQuery = kis::KisHistoryRange;

  /** @brief The working-order result type. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** @brief The broker's name. */
  static constexpr std::string_view name = kis::Broker::name;

  /** @brief `true`: requests return their result directly. */
  static constexpr bool blocking = true;

  /**
   * @brief Returns this client's pacing statistics, or null when it is unpaced.
   * @param client The client.
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  static const network::RateLimitStats* rate_limit_stats(const Client& client) {
    return client.rate_limit_stats();
  }

  /**
   * @brief Returns static reference data for one instrument.
   * @param client The client.
   * @param subject The instrument.
   * @return The result, or an error.
   */
  static Result<Instrument> instrument(Client& client, InstrumentId subject) {
    return client.instrument(std::move(subject));
  }

  /**
   * @brief Returns the calendar, supplying today when no date was given.
   * @param client The client.
   * @param from The first date (today in Korea if empty).
   * @return The result, or an error.
   */
  static Result<TradingCalendar> calendar(Client& client, Country, std::optional<Date> from) {
    return client.calendar(from ? *from : kis::today_in_seoul());
  }

  /**
   * @brief Returns today's price bounds.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<PriceLimits> price_limits(Client& client, InstrumentId instrument) {
    return client.price_limits(std::move(instrument));
  }

  /**
   * @brief Returns recent executions.
   * @param client The client.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @return The result, or an error.
   */
  static Result<std::vector<TradePrint>> trades(
    Client& client,
    InstrumentId instrument,
    int count
  ) {
    return client.trades(std::move(instrument), count);
  }

  /**
   * @brief Returns the order book.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<DepthSnapshot> depth(Client& client, InstrumentId instrument) {
    return client.depth(std::move(instrument));
  }

  /**
   * @brief Returns bars, paging inside the client.
   * @param client The client.
   * @param query The query.
   * @return The result, or an error.
   */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    return client.bars(std::move(query));
  }

  /**
   * @brief Returns a quote.
   * @param client The client.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return client.quote(std::move(instrument));
  }

  /**
   * @brief Returns holdings for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<HoldingsSnapshot> holdings(Client& client, const Account& account) {
    return client.holdings(account);
  }

  /**
   * @brief Returns what the bound account may spend in `currency`.
   * @param client The client.
   * @param account The bound account.
   * @param currency The currency.
   * @return The result, or an error.
   */
  static Result<BuyingPower> buying_power(
    Client& client,
    const Account& account,
    Currency currency
  ) {
    return client.buying_power(currency, account);
  }

  /**
   * @brief Submits an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param order The order.
   * @return The result, or an error.
   */
  static Result<PlaceAck> place_order(Client& client, const Account& account, OrderRequest order) {
    return client.place_order(account, std::move(order));
  }

  /**
   * @brief Modifies an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  static Result<PlaceAck> replace_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.replace_order(account, std::move(amendment));
  }

  /**
   * @brief Cancels an order for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  static Result<PlaceAck> cancel_order(
    Client& client,
    const Account& account,
    Amendment amendment
  ) {
    return client.cancel_order(account, std::move(amendment));
  }

  /**
   * @brief Returns the amendable orders for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.open_orders(account);
  }

  /**
   * @brief Returns order history for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @param query The query.
   * @return The result, or an error.
   */
  static Result<OrderPage> order_history(
    Client& client,
    const Account& account,
    HistoryQuery query
  ) {
    return client.order_history(account, query);
  }
};

} // namespace algotradelib
