#pragma once

#include <string>

namespace algotradelib {

/**
 * @brief Describes one broker client to the library: its types, capabilities, call thunks and
 *   normalizers.
 *
 * Declared but never defined. To add a broker, specialize it for your client type in your own
 * code; no library file changes. A specialization holds only:
 *
 * - associated types and constants: `Client`, `Config`, `Account`, `BrokerTag`, `PlaceAck`,
 *   `Amendment`, `HistoryQuery`, `WorkingOrdersResult`, `name`, and the capability constants that
 *   `BrokerCaps` reads;
 * - call thunks that forward to the client's own functions, with the broker's own argument types;
 * - normalizers and converters, which hold no state and perform no I/O.
 *
 * A missing member is a compile error where it is first used. Assert
 * `ATL_ASSERT_BROKER(Client)` (`broker_verbs.hpp`) in one source file to compile every operation and
 * read every capability at once; it does not call `broker_name()` or `rate_limit_stats()`.
 *
 * | Member | Required |
 * |---|---|
 * | `Account`, `BrokerTag`, `Config`, `PlaceAck`, `Amendment`, `HistoryQuery`, `WorkingOrdersResult` | always |
 * | `name`, `blocking`, `data_only`, `has_trigger_price`, `supports_interval(BarInterval)` | always |
 * | every other `BrokerCaps` constant (`true` or `false`), and `sellable_source`, except the two below | always; none has a default |
 * | `working_orders_state_terminal` | where `has_working_orders` |
 * | `sellable_inquiry_basis` | where `sellable_source` is `INQUIRY` |
 * | `quote`, `bars`, `validate_bars`, `holdings`, `validate_place`, `place_order`, `replace_order`, `cancel_order`, `order_history`, `normalize_place`, `to_replacement`, `to_cancellation`, `to_history_query`, `rate_limit_stats` | always: every session calls them |
 * | `buying_power`, `sellable_quantity`, `commissions`, `working_orders` and `normalize_working_orders`, `instrument`, `calendar` and `validate_calendar`, `price_limits`, `trades`, `prices`, `depth`, `order_by_id`, `accounts` with `selector_of`, `display_of` and `type_of` | where the capability that gates it is claimed |
 *
 * A blocking client (`blocking = true`) returns each result; an asynchronous one takes a
 * completion handler as its last argument. A market-data vendor can inherit `DataOnlyTraits<Tag>`,
 * which answers the account side.
 *
 * @tparam Client The broker client type.
 */
template <typename Client>
struct BrokerTraits;

/**
 * @brief One account from a broker's account list.
 *
 * @tparam Client The broker client type.
 */
template <typename Client>
struct AccountInfo {
  /** @brief The account number as the broker displays it (usually masked); for display only. */
  std::string display_number_;
  /** @brief The account type as the broker reported it; for display only. */
  std::string type_;
  /** @brief The value to bind a `BrokerSession` to this account with; its type is broker-specific. */
  typename BrokerTraits<Client>::Account selector_;
};

namespace port_detail {

/** Always false, but dependent, so a `static_assert` fires only on instantiation. */
template <typename T>
constexpr bool dependent_false = false;

} // namespace port_detail

} // namespace algotradelib
