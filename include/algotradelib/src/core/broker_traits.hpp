#pragma once

#include "./calendar.hpp"
#include "./instrument.hpp"
#include "./market_data.hpp"
#include "./types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {

/**
 * Adaptation point between one concrete broker client and the broker port.
 *
 * The template is declared and never defined. Each supported client specializes
 * it in that broker's own directory, so this header never depends on a broker
 * and adding a broker edits no shared file. The dependency therefore points from
 * the port to the adapter, which is the direction the architecture requires.
 *
 * A specialization is deliberately *external* rather than a base class the
 * client inherits. Static inheritance (CRTP) shares an implementation downward
 * and cannot translate a vendor acknowledgement type into a normalized one;
 * every hook would still need the mapping layer that lives here.
 *
 * A specialization contributes exactly three kinds of member and nothing else:
 *
 * - associated types and constants: `Client`, `Config`, `Account`, `BrokerTag`,
 *   `PlaceAck`, `Amendment`, `HistoryQuery`, `WorkingOrdersResult`, `name`, and
 *   the capability constants read by `BrokerCaps`;
 * - raw call thunks forwarding to the client's own entry points, taking the
 *   broker's own argument types;
 * - pure normalizers and converters, which hold no state and perform no I/O.
 *
 * Normalizers are pure so that the asynchronous and blocking ports share them
 * verbatim. Completion-signature translation and executor propagation are
 * written once in `BrokerSession`, not per broker.
 *
 * **The contract.** Nothing checks a specialization when a session is built: a
 * missing member is reported by the compiler, naming this specialization and the
 * member, where it is first used. `ATL_ASSERT_BROKER(Client)` (`broker_verbs.hpp`)
 * uses all of it at once, and an adapter author asserts it in one source file.
 *
 * | Member | Required |
 * |---|---|
 * | `Account`, `BrokerTag`, `Config`, `PlaceAck`, `Amendment`, `HistoryQuery`, `WorkingOrdersResult` | always |
 * | `name`, `blocking`, `data_only`, `has_trigger_price`, `supports_interval(BarInterval)` | always |
 * | every other `BrokerCaps` constant, answered `true` or `false` | always; none has a default |
 * | `working_orders_state_terminal` | where `has_working_orders` |
 * | `sellable_inquiry_basis` | where `sellable_source` is `INQUIRY` |
 * | `quote`, `bars`, `validate_bars`, `holdings`, `validate_place`, `place_order`, `replace_order`, `cancel_order`, `order_history`, `normalize_place`, `to_replacement`, `to_cancellation`, `to_history_query`, `rate_limit_stats` | always: every session calls them |
 * | `buying_power`, `sellable_quantity`, `commissions`, `working_orders` and `normalize_working_orders`, `instrument`, `calendar` and `validate_calendar`, `price_limits`, `trades`, `prices`, `depth`, `order_by_id`, `accounts` with `selector_of`, `display_of` and `type_of` | where the capability that gates it is claimed |
 *
 * A blocking client (`blocking = true`) answers each call with a result; an
 * asynchronous one takes a completion handler as its last argument. A market-data
 * vendor inherits `DataOnlyTraits<Tag>`, which answers the account side.
 */
template <typename Client>
struct BrokerTraits;

/**
 * One brokerage account discovered through a broker that publishes a list.
 *
 * `selector_` is the value a `BrokerSession` binds; its type is broker-specific
 * because account selection is. `display_number_` and `type_` are for display
 * and are preserved as the broker reported them.
 */
template <typename Client>
struct AccountInfo {
  /** The account number as the broker displays it, usually masked. For display only. */
  std::string display_number_;
  /** The account type as the broker reported it. For display only. */
  std::string type_;
  /** The value a `BrokerSession` binds. Its type is broker-specific, because selection is. */
  typename BrokerTraits<Client>::Account selector_;
};

namespace port_detail {

/** Always false, but dependent, so a `static_assert` fires only on instantiation. */
template <typename T>
constexpr bool dependent_false = false;

} // namespace port_detail

} // namespace algotradelib
