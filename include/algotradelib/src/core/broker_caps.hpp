#pragma once

#include "./broker_traits.hpp"

namespace algotradelib {

/**
 * Compile-time feature set of one broker client.
 *
 * Each constant answers whether the broker *implements* an operation, which a
 * strategy gates on with `if constexpr` so one source file compiles against
 * every broker. Every specialization must answer every question explicitly; a
 * defaulted answer would let a new broker silently claim a feature it lacks.
 *
 * Capability is not availability. Whether a *configuration* may call an
 * implemented operation stays a run-time error: KIS implements the
 * modifiable-order inquiry but does not offer it to demo accounts, which is an
 * `ErrorCategory::INVALID_ARGUMENT` at run time. Encoding a per-credential
 * restriction here would make one binary claim different things for a live and a
 * demo key.
 */
template <typename Client>
struct BrokerCaps {
  /** Traits specialization these capabilities are read from. */
  using Traits = BrokerTraits<Client>;

  /** Whether the broker lists selectable accounts over the network. */
  static constexpr bool has_account_discovery = Traits::has_account_discovery;

  /** Whether the broker accepts a caller-supplied idempotency key on an order. */
  static constexpr bool has_client_order_id = Traits::has_client_order_id;

  /** Whether the broker publishes the orders still open to amendment. */
  static constexpr bool has_working_orders = Traits::has_working_orders;

  /** Whether the broker reports the quantity still amendable on an open order. */
  static constexpr bool has_amendable_quantity = Traits::has_amendable_quantity;

  /** Whether the broker retrieves one order by its identifier. */
  static constexpr bool has_order_by_id = Traits::has_order_by_id;

  /** Whether the broker can cancel part of an order rather than all of it. */
  static constexpr bool has_partial_cancel = Traits::has_partial_cancel;

  /** Whether an order-history inquiry requires an explicit calendar range. */
  static constexpr bool history_requires_range = Traits::history_requires_range;

  /** Whether an order-history inquiry exposes a caller-visible cursor. */
  static constexpr bool has_history_cursor = Traits::has_history_cursor;

  /** Whether an order-history inquiry can filter by instrument. */
  static constexpr bool has_history_symbol_filter = Traits::has_history_symbol_filter;
};

} // namespace algotradelib
