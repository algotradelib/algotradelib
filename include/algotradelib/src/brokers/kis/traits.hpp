#pragma once

#include "./client.hpp"

#include "../../core/broker_session.hpp"
#include "../../core/broker_traits.hpp"

#include <optional>
#include <string>
#include <utility>

namespace algotradelib {

namespace kis::traits_detail {

/** Normalized broker name carried by every KIS order reference. */
inline constexpr std::string_view broker_name = "kis";

/**
 * Pure conversions shared by the asynchronous and blocking KIS traits.
 *
 * Holding them in one place is what keeps `BrokerSession<KisClient>` and
 * `BlockingSession<KisBlockingClient>` from drifting apart in how they normalize
 * the same KIS response.
 */
struct Normalizers {
  /** Returns an opaque reference carrying KIS's order number and branch code. */
  static OrderRef make_ref(const KisOrderRef& order) {
    OrderRef ref;
    ref.broker_ = broker_name;
    ref.primary_ = order.order_number_;
    ref.secondary_ = order.branch_number_;
    return ref;
  }

  /** Returns KIS's own reference form from an opaque reference. */
  static KisOrderRef from_ref(const OrderRef& ref) {
    KisOrderRef order;
    order.order_number_ = ref.primary_;
    order.branch_number_ = ref.secondary_;
    return order;
  }

  /** Returns an error when `ref` was not issued for KIS. */
  static std::optional<BrokerError> check_ref(const OrderRef& ref) {
    if (ref.broker_ == broker_name) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "foreign_order_reference";
    error.message_ = "order reference was not issued by the KIS adapter";
    return error;
  }

  /**
   * Rejects an order KIS cannot carry out as described.
   *
   * KIS documents no client-supplied idempotency key for individual customers,
   * so a supplied key is refused rather than dropped: a caller who set one would
   * otherwise believe a resubmission was safe when KIS offers no such guarantee.
   *
   * `OrderRequest::confirm_high_value_` has no KIS counterpart and is ignored.
   * It gates an order at Toss rather than describing one, so ignoring it cannot
   * make KIS transmit an order the caller did not describe.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest& order) {
    if (!order.client_order_id_) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.broker_code_ = "idempotency_key_unsupported";
    error.message_ =
      "KIS offers no client-supplied idempotency key for individual customers; clear "
      "client_order_id_ and reconcile through an order inquiry after a transport failure";
    return error;
  }

  /** Normalizes a KIS acknowledgement into an acknowledgement and a reference. */
  static Result<PlacedOrder> normalize_place(Result<KisOrderAck> raw) {
    if (!raw) {
      return Result<PlacedOrder>(std::move(raw.error()));
    }
    PlacedOrder placed;
    placed.ack_ = std::move(raw.value().ack_);
    placed.ref_ = make_ref(raw.value().order_);
    return Result<PlacedOrder>(std::move(placed));
  }

  /**
   * Normalizes one page of KIS amendable orders.
   *
   * KIS reports both the row's own order number and the original order number an
   * amendment must address. Both are preserved: `ref_` identifies the row and
   * `amend_ref_` is set only where KIS's original order number differs, so a
   * caller uses `amend_reference()` without choosing between them.
   */
  static Result<WorkingOrders> normalize_working_orders(Result<KisOpenOrderPage> raw) {
    if (!raw) {
      return Result<WorkingOrders>(std::move(raw.error()));
    }
    WorkingOrders orders;
    orders.received_at_ = raw.value().received_at_;
    orders.orders_.reserve(raw.value().orders_.size());
    for (auto& open_order : raw.value().orders_) {
      WorkingOrder working;
      working.order_ = std::move(open_order.order_);
      working.ref_ = make_ref(open_order.order_ref_);
      const auto& original = open_order.original_order_.order_number_;
      if (!original.empty() && original != open_order.order_ref_.order_number_) {
        working.amend_ref_ = make_ref(open_order.original_order_);
      }
      working.amendable_quantity_ = open_order.amendable_quantity_;
      orders.orders_.push_back(std::move(working));
    }
    return Result<WorkingOrders>(std::move(orders));
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
    const OrderRef& ref,
    const AmendInstruction& instruction
  ) {
    if (auto invalid = check_ref(ref)) {
      return Result<KisAmendment>(std::move(*invalid));
    }
    if (!instruction.quantity_) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.broker_code_ = "quantity_required";
      error.message_ =
        "KIS requires an explicit quantity on a modification or cancellation, including a full "
        "cancellation; read the amendable quantity from working orders first";
      return Result<KisAmendment>(std::move(error));
    }

    KisAmendment amendment;
    amendment.order_ = from_ref(ref);
    amendment.type_ = instruction.shape_.type_;
    amendment.quantity_ = *instruction.quantity_;
    amendment.price_ = instruction.shape_.price_;
    amendment.all_remaining_ = instruction.all_remaining_;
    return Result<KisAmendment>(std::move(amendment));
  }

  /** Converts a normalized modification into KIS's own amendment. */
  static Result<KisAmendment> to_replacement(
    const OrderRef& ref,
    const AmendInstruction& instruction
  ) {
    return to_amendment(ref, instruction);
  }

  /** Converts a normalized cancellation into KIS's own amendment. */
  static Result<KisAmendment> to_cancellation(
    const OrderRef& ref,
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

  /** KIS publishes its modifiable and cancellable orders, except on demo accounts. */
  static constexpr bool has_working_orders = true;

  /** KIS reports `psbl_qty`, the quantity still amendable. */
  static constexpr bool has_amendable_quantity = true;

  /** KIS has no single-order inquiry by order number. */
  static constexpr bool has_order_by_id = false;

  /** KIS cancels a stated quantity through `QTY_ALL_ORD_YN`. */
  static constexpr bool has_partial_cancel = true;

  /** KIS requires an inclusive calendar range on an order-history inquiry. */
  static constexpr bool history_requires_range = true;

  /** KIS follows its own continuation cursors and exposes none. */
  static constexpr bool has_history_cursor = false;

  /** This adapter sends an empty `PDNO` and exposes no instrument filter. */
  static constexpr bool has_history_symbol_filter = false;
};

} // namespace kis::traits_detail

/** Broker port adaptation for the asynchronous KIS client. */
template <>
struct BrokerTraits<kis::KisClient>
: kis::traits_detail::Normalizers, kis::traits_detail::Capabilities {
  /** Concrete client this specialization adapts. */
  using Client = kis::KisClient;

  /** KIS application credentials and environment selection. */
  using Config = kis::KisConfig;

  /** KIS account selector. */
  using Account = kis::KisAccount;

  /** Acknowledgement KIS returns for an order instruction. */
  using PlaceAck = kis::KisOrderAck;

  /** KIS modification and cancellation instruction. */
  using Amendment = kis::KisAmendment;

  /** KIS order-history selector. */
  using HistoryQuery = kis::KisHistoryRange;

  /** KIS amendable-order result. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kis::traits_detail::broker_name;

  /** This client completes through a completion token. */
  static constexpr bool blocking = false;

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
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
  /** Concrete client this specialization adapts. */
  using Client = kis::KisBlockingClient;

  /** KIS application credentials and environment selection. */
  using Config = kis::KisConfig;

  /** KIS account selector. */
  using Account = kis::KisAccount;

  /** Acknowledgement KIS returns for an order instruction. */
  using PlaceAck = kis::KisOrderAck;

  /** KIS modification and cancellation instruction. */
  using Amendment = kis::KisAmendment;

  /** KIS order-history selector. */
  using HistoryQuery = kis::KisHistoryRange;

  /** KIS amendable-order result. */
  using WorkingOrdersResult = kis::KisOpenOrderPage;

  /** Normalized broker name. */
  static constexpr std::string_view name = kis::traits_detail::broker_name;

  /** This client drives its own event loop and returns a result directly. */
  static constexpr bool blocking = true;

  /** Returns a quote. */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return client.quote(std::move(instrument));
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
