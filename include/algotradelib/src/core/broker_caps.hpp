#pragma once

#include "./broker_traits.hpp"
#include "./calendar.hpp"
#include "./instrument.hpp"
#include "./market_data.hpp"

namespace algotradelib {

namespace caps_detail {

/**
 * Reads `T::working_orders_state_terminal` where `T::has_working_orders` is true, and answers false where
 * it is not: the question is asked only of a broker that lists working orders, so only that broker must
 * answer it. `T` is a traits specialization or a hand-written capability set.
 */
template <typename T>
constexpr bool terminal_working_state() {
  if constexpr (T::has_working_orders) {
    return T::working_orders_state_terminal;
  } else {
    return false;
  }
}

/**
 * Reads `T::sellable_inquiry_basis` where `T::sellable_source` is `INQUIRY`, and answers an unestablished
 * basis where it is not: no inquiry is made there, so no figure needs one. `T` is a traits
 * specialization or a hand-written capability set.
 */
template <typename T>
constexpr SellableBasis inquiry_basis() {
  if constexpr (T::sellable_source == SellableSource::INQUIRY) {
    return T::sellable_inquiry_basis;
  } else {
    return SellableBasis{};
  }
}

}  // namespace caps_detail

/**
 * @brief What one broker client implements, as compile-time constants.
 *
 * Gate broker-specific code with `if constexpr` on these, so one strategy compiles against every
 * broker. A traits specialization must answer every constant (none has a default);
 * `working_orders_state_terminal` and `sellable_inquiry_basis` only where the inquiry they describe
 * exists. `ATL_ASSERT_BROKER` reads them all.
 *
 * A capability says the broker implements an operation, not that every account may call it: KIS
 * does not offer its modifiable-order inquiry to demo accounts, so its adapter answers that read
 * from the daily order inquiry there instead.
 *
 * @tparam Client The broker client type.
 */
template <typename Client>
struct BrokerCaps {
  /** @brief The traits specialization the constants are read from. */
  using Traits = BrokerTraits<Client>;

  /** @brief Whether the broker lists the selectable accounts. */
  static constexpr bool has_account_discovery = Traits::has_account_discovery;

  /** @brief Whether the broker accepts an idempotency key (`OrderRequest::client_order_id_`). */
  static constexpr bool has_client_order_id = Traits::has_client_order_id;

  /**
   * @brief Whether the broker lists the orders still open to amendment. A listing shows only what the
   *   broker's API lists (at Toss, not orders of types placed outside it).
   */
  static constexpr bool has_working_orders = Traits::has_working_orders;

  /** @brief Whether the broker reports the quantity still amendable on an open order. */
  static constexpr bool has_amendable_quantity = Traits::has_amendable_quantity;

  /**
   * @brief Whether the working-order listing can show a terminal status (filled, cancelled, ...).
   *
   * `false` for KIS, Toss and Kiwoom: their listings show only open orders. Then an order that
   * leaves the listing has no terminal `on_order_update`; read its outcome from holdings or an order
   * inquiry instead of waiting for one. `false` here when `has_working_orders` is `false`.
   */
  static constexpr bool working_orders_state_terminal = caps_detail::terminal_working_state<Traits>();

  /**
   * @brief Whether the adapter sends trigger prices (stop-limit orders).
   *
   * When `false`, `BrokerSession` refuses a placement or modification carrying a trigger
   * (`unsupported_trigger_price`) before the adapter runs; cancellations are unaffected. `true` for
   * KIS and Kiwoom, `false` for Toss.
   */
  static constexpr bool has_trigger_price = Traits::has_trigger_price;

  /**
   * @brief Whether the client is a market-data source that takes no orders (built on
   *   `DataOnlyTraits`).
   *
   * Binding such a client as the trading session of `live::SessionExecutor` or `live::SessionSource`
   * does not compile. Calling `place` or `holdings` on its session directly fails at run time.
   */
  static constexpr bool data_only = Traits::data_only;

  /** @brief Whether the broker can look up one order by its id. */
  static constexpr bool has_order_by_id = Traits::has_order_by_id;

  /** @brief Whether the broker can cancel part of an order. */
  static constexpr bool has_partial_cancel = Traits::has_partial_cancel;

  /** @brief Whether an order-history inquiry needs a date range (`OrderHistoryQuery::from_`). */
  static constexpr bool history_requires_range = Traits::history_requires_range;

  /**
   * @brief Whether an order-history answer can carry a cursor to the next page.
   *
   * When `false`, the adapter returns the whole history in one answer.
   */
  static constexpr bool has_history_cursor = Traits::has_history_cursor;

  /**
   * @brief Whether an order-history inquiry accepts a row limit (`OrderHistoryQuery::limit_`).
   *
   * `false` for KIS, which refuses a query with a limit (`INVALID_ARGUMENT`, before any I/O).
   */
  static constexpr bool has_history_limit = Traits::has_history_limit;

  /** @brief Whether an order-history inquiry can filter by symbol. */
  static constexpr bool has_history_symbol_filter = Traits::has_history_symbol_filter;

  /** @brief Whether the broker publishes the order book (`depth`). */
  static constexpr bool has_depth = Traits::has_depth;

  /** @brief Whether the broker publishes the day's price limits. */
  static constexpr bool has_price_limits = Traits::has_price_limits;

  /** @brief Whether the broker publishes recent trades. */
  static constexpr bool has_trade_prints = Traits::has_trade_prints;

  /** @brief Whether the broker quotes several instruments in one request. */
  static constexpr bool has_batch_prices = Traits::has_batch_prices;

  /** @brief Whether the broker reports buying power. */
  static constexpr bool has_buying_power = Traits::has_buying_power;

  /**
   * @brief Whether the broker can also say how many shares are affordable.
   *
   * `true` for KIS (per instrument and order type), `false` for Toss. The port returns only the
   * amount; reach the quantity through `session.client()`.
   */
  static constexpr bool has_buying_power_quantity = Traits::has_buying_power_quantity;

  /**
   * @brief Where the library gets the sellable quantity: `HOLDINGS` costs nothing, `INQUIRY` one
   *   request per instrument sold, `NONE` means it is not available.
   */
  static constexpr SellableSource sellable_source = Traits::sellable_source;

  /**
   * @brief What a sellable figure from the per-instrument inquiry covers.
   *
   * Set only where `sellable_source` is `INQUIRY` (unestablished otherwise). If unestablished,
   * checked sells are refused.
   */
  static constexpr SellableBasis sellable_inquiry_basis = caps_detail::inquiry_basis<Traits>();

  /** @brief Whether the broker publishes the account's commission rates. */
  static constexpr bool has_commission_schedule = Traits::has_commission_schedule;

  /** @brief Whether the broker publishes instrument reference data. */
  static constexpr bool has_reference_data = Traits::has_reference_data;

  /**
   * @brief Whether one reference-data request can cover several instruments.
   *
   * Toss takes up to 200 symbols; KIS one. The port asks for one; use `session.client()` for more.
   */
  static constexpr bool has_batch_reference_data = Traits::has_batch_reference_data;

  /** @brief Whether the broker publishes the trading calendar. */
  static constexpr bool has_calendar = Traits::has_calendar;

  /**
   * @brief Whether one calendar request covers a span of dates.
   *
   * KIS answers from a base date onward; Toss answers the requested day and the trading day either
   * side.
   */
  static constexpr bool has_calendar_range = Traits::has_calendar_range;

  /**
   * @brief Whether the broker publishes intraday session hours (only Toss); the library does not
   *   expose them yet.
   */
  static constexpr bool has_calendar_sessions = Traits::has_calendar_sessions;

  /**
   * @brief Tests whether the broker publishes bars of an interval.
   *
   * KIS and Toss both publish minute and daily bars; only KIS publishes weekly, monthly and yearly.
   * An interval a broker does not publish is refused, not aggregated from shorter bars.
   *
   * @param interval The interval.
   * @return `true` if the broker publishes it.
   */
  static constexpr bool supports_interval(BarInterval interval) {
    return Traits::supports_interval(interval);
  }
};

/**
 * @brief Reads every capability constant of a capability set, so a missing one fails to compile here.
 *
 * @tparam Caps `BrokerCaps<Client>`, or a hand-written set such as `strategy::NoCaps`.
 * @return Always `true`.
 */
// A constant added to `BrokerCaps` must be added here.
template <typename Caps>
constexpr bool reads_every_capability() {
  constexpr bool read[] = {
    Caps::has_account_discovery,
    Caps::has_client_order_id,
    Caps::has_working_orders,
    Caps::has_amendable_quantity,
    Caps::working_orders_state_terminal,
    Caps::has_trigger_price,
    Caps::data_only,
    Caps::has_order_by_id,
    Caps::has_partial_cancel,
    Caps::history_requires_range,
    Caps::has_history_cursor,
    Caps::has_history_limit,
    Caps::has_history_symbol_filter,
    Caps::has_depth,
    Caps::has_price_limits,
    Caps::has_trade_prints,
    Caps::has_batch_prices,
    Caps::has_buying_power,
    Caps::has_buying_power_quantity,
    Caps::sellable_source == SellableSource::INQUIRY,
    Caps::sellable_inquiry_basis.coverage_ == SellableBasis::Coverage::UNESTABLISHED,
    Caps::has_commission_schedule,
    Caps::has_reference_data,
    Caps::has_batch_reference_data,
    Caps::has_calendar,
    Caps::has_calendar_range,
    Caps::has_calendar_sessions,
  };
  static_cast<void>(read);
  // Named, not called: the predicate need not be usable in a constant expression to be conforming.
  static_cast<void>(&Caps::supports_interval);
  return true;
}

/**
 * @brief Reads every capability of a client's traits (see `reads_every_capability`).
 * @tparam Client The broker client type.
 * @return Always `true`; a missing capability fails to compile.
 */
template <typename Client>
constexpr bool answers_every_capability() {
  return reads_every_capability<BrokerCaps<Client>>();
}

} // namespace algotradelib
