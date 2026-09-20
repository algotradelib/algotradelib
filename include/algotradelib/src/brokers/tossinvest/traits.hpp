#pragma once

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

  /** Toss filters a listing by instrument. */
  static constexpr bool has_history_symbol_filter = true;
};

} // namespace traits_detail
} // namespace tossinvest

/** Broker port adaptation for the asynchronous Toss Securities client. */
template <>
struct BrokerTraits<tossinvest::TossClient>
: tossinvest::traits_detail::Normalizers, tossinvest::traits_detail::Capabilities {
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
