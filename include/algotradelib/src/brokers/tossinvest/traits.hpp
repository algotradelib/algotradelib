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

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace tossinvest {

/**
 * @brief The Toss Securities broker tag: order references are typed by it, so a Toss reference
 *   works with both Toss clients and with no other broker.
 */
struct Broker {
  /** @brief The broker's name, `"tossinvest"`. */
  static constexpr std::string_view name = "tossinvest";
};

namespace traits_detail {

/** @brief A Toss order reference (`primary_` is Toss's order id). */
using Ref = OrderRef<tossinvest::Broker>;

/** @brief The capabilities and conversions shared by Toss's asynchronous and blocking traits. */
struct Normalizers {
  /** @brief `true`: Toss publishes the order book. */
  static constexpr bool has_depth = true;

  /** @brief `true`: Toss publishes today's price band. */
  static constexpr bool has_price_limits = true;

  /** @brief `true`: Toss publishes recent trades. */
  static constexpr bool has_trade_prints = true;

  /** @brief `true`: up to 200 instruments can be quoted in one request. */
  static constexpr bool has_batch_prices = true;

  /** @brief `true`: Toss answers buying power per currency (KRW, USD), on a cash basis. */
  static constexpr bool has_buying_power = true;

  /** @brief `false`: Toss reports an amount, not how many shares are affordable. */
  static constexpr bool has_buying_power_quantity = false;

  /**
   * @brief Holdings rows carry no sellable quantity; it is a separate inquiry, one request per
   *   instrument sold.
   */
  static constexpr SellableSource sellable_source = SellableSource::INQUIRY;

  /**
   * @brief What the sellable-quantity inquiry's answer covers: the whole holding, with this library
   *   deducting resting sell orders from it.
   *
   * Toss does not document whether its figure already excludes resting sells, so this is an
   * unverified assumption (`verified_` is `false`): deducting them errs toward refusing a sale rather
   * than selling the same shares twice.
   */
  static constexpr SellableBasis sellable_inquiry_basis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };

  /** @brief `true`: Toss publishes the account's commission rates. */
  static constexpr bool has_commission_schedule = true;

  /** @brief `true`: Toss publishes instrument reference data. */
  static constexpr bool has_reference_data = true;

  /** @brief `true`: several instruments per reference-data request. */
  static constexpr bool has_batch_reference_data = true;

  /** @brief `true`: Toss publishes the trading calendar for Korea and the US. */
  static constexpr bool has_calendar = true;

  /** @brief `false`: a calendar request covers only a date and the business days either side. */
  static constexpr bool has_calendar_range = false;

  /** @brief `true`: Toss publishes session hours, but this library does not expose them yet. */
  static constexpr bool has_calendar_sessions = true;

  /**
   * @brief Refuses a calendar request with an invalid date (the market argument is not checked).
   * @param from The date (empty: the broker's today).
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
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

  /**
   * @brief Tests whether Toss publishes bars at an interval.
   * @param interval The interval.
   * @return `true` for one-minute and one-day bars.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    static_cast<void>(interval);
    return interval == BarInterval::ONE_MINUTE || interval == BarInterval::ONE_DAY;
  }

  /**
   * @brief Refuses a bar request this broker cannot answer, before any I/O.
   * @param query The query.
   * @return An error if refused (nothing is sent), otherwise `std::nullopt`.
   */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    return tossinvest::unsupported_bar_interval(query.interval_);
  }

  /**
   * @brief Returns an opaque reference carrying Toss's order identifier.
   * @param order_id Toss's order id.
   * @return The reference.
   */
  static Ref make_ref(std::string order_id) {
    Ref ref;
    ref.primary_ = std::move(order_id);
    return ref;
  }

  /**
   * @brief Accepts every order here; the client refuses some orders Toss cannot take before sending (see
   *   `TossClient::async_place_order`). Toss honours `client_order_id_` as an idempotency key for ten
   *   minutes. The order argument is not used.
   * @return `std::nullopt`.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest&) { return std::nullopt; }

  /**
   * @brief Normalizes a Toss acknowledgement into an acknowledgement and a reference.
   * @param raw The client's result.
   * @return The result, or an error.
   */
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
   * @brief Converts Toss's `OPEN` listing into working orders. Toss reports no amendable quantity,
   *   so it is left empty, not zero.
   * @param raw The client's result.
   * @return The working orders, or the error.
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
   * @brief Converts an amendment into Toss's form.
   *
   * Toss requires a quantity for a Korean order and refuses one for a US order. It reports no
   * amendable quantity, so an amendment built with `reprice()` or `replace_with()` and no quantity
   * carries none, which Toss refuses for a Korean order: state the quantity there. Toss cannot change
   * the condition or session, so a change to either is refused, as is any amendment without
   * `resting_` (`unknown_resting_shape`).
   *
   * @param ref The order's reference.
   * @param instruction The amendment.
   * @return Toss's amendment, or an `INVALID_ARGUMENT` error.
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
   * @brief Converts a cancellation into Toss's form (the order id only).
   *
   * Toss cancels whole orders only, so a partial cancellation is refused rather than widened.
   *
   * @param ref The order's reference.
   * @param instruction The cancellation.
   * @return Toss's instruction, or an `INVALID_ARGUMENT` error.
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
   * @brief Converts an order-history query into a Toss `CLOSED` listing (ended orders; open ones are
   *   working orders). Every filter is supported.
   * @param account_sequence The account (`TossAccount::sequence_`).
   * @param query The query.
   * @return The listing query (never an error).
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

  /**
   * @brief Returns the `OPEN` listing query for an account.
   * @param account_sequence The account (`TossAccount::sequence_`).
   * @return The query.
   */
  static tossinvest::TossOrderQuery open_query(std::int64_t account_sequence) {
    tossinvest::TossOrderQuery query;
    query.account_sequence_ = account_sequence;
    query.scope_ = tossinvest::TossOrderScope::OPEN;
    return query;
  }

  /**
   * @brief Returns the account sequence a session binds.
   * @param account The account.
   * @return `account.sequence_`.
   */
  static std::int64_t selector_of(const tossinvest::TossAccount& account) {
    return account.sequence_;
  }

  /**
   * @brief Returns the account number as Toss displays it.
   * @param account The account.
   * @return The number.
   */
  static std::string display_of(const tossinvest::TossAccount& account) {
    return account.account_number_;
  }

  /**
   * @brief Returns the account type as Toss reports it.
   * @param account The account.
   * @return The type.
   */
  static std::string type_of(const tossinvest::TossAccount& account) {
    return account.account_type_;
  }
};

/** @brief The order-related capabilities shared by Toss's asynchronous and blocking traits. */
struct Capabilities {
  /** @brief `true`: Toss lists the accounts the credentials can use. */
  static constexpr bool has_account_discovery = true;

  /** @brief `true`: `client_order_id_` is an idempotency key for ten minutes. */
  static constexpr bool has_client_order_id = true;

  /**
   * @brief `false`: this client does not place Toss's conditional (trigger) orders, a separate API;
   *   `BrokerSession` refuses a trigger price before sending.
   */
  static constexpr bool has_trigger_price = false;

  /** @brief `false`: a broker that takes orders; see `BrokerCaps::data_only`. */
  static constexpr bool data_only = false;

  /**
   * @brief `true`: the `OPEN` listing returns open orders (only those placed as limit, market or
   *   closing-auction limit orders; see `TossClient::async_orders`).
   */
  static constexpr bool has_working_orders = true;

  /** @brief `false`: Toss reports no amendable quantity. */
  static constexpr bool has_amendable_quantity = false;

  /**
   * @brief `false`: the `OPEN` listing shows only open orders; an order's outcome comes from the
   *   `CLOSED` listing or `order_by_id`.
   */
  static constexpr bool working_orders_state_terminal = false;

  /** @brief `true`: one order can be read by id, in any state (with the same type restriction as listings). */
  static constexpr bool has_order_by_id = true;

  /** @brief `false`: Toss cancels whole orders only. */
  static constexpr bool has_partial_cancel = false;

  /** @brief `false`: an order-history query needs no date range. */
  static constexpr bool history_requires_range = false;

  /** @brief `true`: the `CLOSED` listing pages with a cursor. */
  static constexpr bool has_history_cursor = true;

  /** @brief `true`: an order-history query can limit rows per page. */
  static constexpr bool has_history_limit = true;

  /** @brief `true`: an order-history query can filter by instrument. */
  static constexpr bool has_history_symbol_filter = true;
};

} // namespace traits_detail
} // namespace tossinvest

/** @brief The `BrokerSession` adapter for `tossinvest::TossClient`. */
template <>
struct BrokerTraits<tossinvest::TossClient>
: tossinvest::traits_detail::Normalizers, tossinvest::traits_detail::Capabilities {
  /**
   * @brief Returns the venue a Toss order reaches: none in particular (Toss does not route by venue,
   *   and its listings carry none).
   *
   * The venue asked for is ignored.
   *
   * @return `Venue::UNSPECIFIED`.
   */
  static constexpr Venue effective_route(Venue) noexcept { return Venue::UNSPECIFIED; }

  /** @brief The client type. */
  using Client = tossinvest::TossClient;

  /** @brief The configuration type. */
  using Config = tossinvest::TossConfig;

  /** @brief The account type: the account sequence. */
  using Account = std::int64_t;

  /** @brief The broker tag. */
  using BrokerTag = tossinvest::Broker;

  /** @brief The order acknowledgement type. */
  using PlaceAck = OrderAck;

  /** @brief The amendment type. */
  using Amendment = tossinvest::TossAmendment;

  /** @brief The order-history query type. */
  using HistoryQuery = tossinvest::TossOrderQuery;

  /** @brief The working-order result type. */
  using WorkingOrdersResult = OrderPage;

  /** @brief The account-listing item type. */
  using AccountListItem = tossinvest::TossAccount;

  /** @brief The broker's name. */
  static constexpr std::string_view name = tossinvest::Broker::name;

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
   * @brief Forwards a reference-data request.
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
   * @brief Forwards a calendar request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param country The market.
   * @param from The date (empty: the broker's today).
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto calendar(Client& client, Country country, std::optional<Date> from, Token&& token) {
    return client.async_calendar(country, from, std::forward<Token>(token));
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
   * @param count The most trades to return, clamped to 1-50.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto trades(Client& client, InstrumentId instrument, int count, Token&& token) {
    return client.async_trades(std::move(instrument), count, std::forward<Token>(token));
  }

  /**
   * @brief Forwards a batch quote request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param instruments The instruments.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto prices(Client& client, std::vector<InstrumentId> instruments, Token&& token) {
    return client.async_prices(std::move(instruments), std::forward<Token>(token));
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
   * @brief Forwards an account listing request.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto accounts(Client& client, Token&& token) {
    return client.async_accounts(std::forward<Token>(token));
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
   * @brief Forwards a sellable-quantity request for one instrument.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto sellable_quantity(
    Client& client,
    const Account& account,
    InstrumentId instrument,
    Token&& token
  ) {
    return client.async_sellable_quantity(std::move(instrument), account, std::forward<Token>(token));
  }

  /**
   * @brief Forwards a commission-schedule request for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto commissions(Client& client, const Account& account, Token&& token) {
    return client.async_commissions(account, std::forward<Token>(token));
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
   * @brief Forwards an order cancellation, which transmits the identifier only.
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
    return client.async_cancel_order(
      account, std::move(amendment.order_id_), std::forward<Token>(token)
    );
  }

  /**
   * @brief Forwards an `OPEN` listing for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename Token>
  static auto working_orders(Client& client, const Account& account, Token&& token) {
    return client.async_orders(open_query(account), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a `CLOSED` listing for the bound account.
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
    static_cast<void>(account);
    return client.async_orders(std::move(query), std::forward<Token>(token));
  }

  /**
   * @brief Forwards a single-order inquiry for the bound account.
   * @tparam Token An Asio completion token.
   * @param client The client.
   * @param account The bound account.
   * @param order_id Toss's order id.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
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

/** @brief The `BlockingSession` adapter for `tossinvest::TossBlockingClient`. */
template <>
struct BrokerTraits<tossinvest::TossBlockingClient>
: tossinvest::traits_detail::Normalizers, tossinvest::traits_detail::Capabilities {
  /**
   * @brief Returns the venue a Toss order reaches: none in particular (Toss does not route by venue,
   *   and its listings carry none).
   *
   * The venue asked for is ignored.
   *
   * @return `Venue::UNSPECIFIED`.
   */
  static constexpr Venue effective_route(Venue) noexcept { return Venue::UNSPECIFIED; }

  /** @brief The client type. */
  using Client = tossinvest::TossBlockingClient;

  /** @brief The configuration type. */
  using Config = tossinvest::TossConfig;

  /** @brief The account type: the account sequence. */
  using Account = std::int64_t;

  /** @brief The broker tag. */
  using BrokerTag = tossinvest::Broker;

  /** @brief The order acknowledgement type. */
  using PlaceAck = OrderAck;

  /** @brief The amendment type. */
  using Amendment = tossinvest::TossAmendment;

  /** @brief The order-history query type. */
  using HistoryQuery = tossinvest::TossOrderQuery;

  /** @brief The working-order result type. */
  using WorkingOrdersResult = OrderPage;

  /** @brief The account-listing item type. */
  using AccountListItem = tossinvest::TossAccount;

  /** @brief The broker's name. */
  static constexpr std::string_view name = tossinvest::Broker::name;

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
   * @brief Returns the calendar for one market.
   * @param client The client.
   * @param country The market.
   * @param from The date (empty: the broker's today).
   * @return The result, or an error.
   */
  static Result<TradingCalendar> calendar(Client& client, Country country, std::optional<Date> from) {
    return client.calendar(country, from);
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
   * @param count The most trades to return, clamped to 1-50.
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
   * @brief Returns a quote for every instrument in one request.
   * @param client The client.
   * @param instruments The instruments.
   * @return The result, or an error.
   */
  static Result<std::vector<QuoteSnapshot>> prices(
    Client& client,
    std::vector<InstrumentId> instruments
  ) {
    return client.prices(std::move(instruments));
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
   * @brief Returns the broker's own account listing.
   * @param client The client.
   * @return The result, or an error.
   */
  static Result<std::vector<AccountListItem>> accounts(Client& client) {
    return client.accounts();
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
   * @brief Returns how much of `instrument` the bound account may sell.
   * @param client The client.
   * @param account The bound account.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  static Result<Decimal64> sellable_quantity(
    Client& client,
    const Account& account,
    InstrumentId instrument
  ) {
    return client.sellable_quantity(std::move(instrument), account);
  }

  /**
   * @brief Returns the commission rates in force for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<CommissionSchedule> commissions(Client& client, const Account& account) {
    return client.commissions(account);
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
   * @brief Cancels an order for the bound account, transmitting the identifier only.
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
    return client.cancel_order(account, std::move(amendment.order_id_));
  }

  /**
   * @brief Returns the `OPEN` listing for the bound account.
   * @param client The client.
   * @param account The bound account.
   * @return The result, or an error.
   */
  static Result<WorkingOrdersResult> working_orders(Client& client, const Account& account) {
    return client.orders(open_query(account));
  }

  /**
   * @brief Returns the `CLOSED` listing for the bound account.
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
    static_cast<void>(account);
    return client.orders(std::move(query));
  }

  /**
   * @brief Returns one order by id for the bound account; see `TossClient::async_order` for which
   *   orders can be read.
   * @param client The client.
   * @param account The bound account.
   * @param order_id Toss's order id.
   * @return The result, or an error.
   */
  static Result<OrderSnapshot> order_by_id(
    Client& client,
    const Account& account,
    std::string order_id
  ) {
    return client.order(account, std::move(order_id));
  }
};

} // namespace algotradelib
