#pragma once

#include "./types.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
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
  std::string display_number_;
  std::string type_;
  typename BrokerTraits<Client>::Account selector_;
};

namespace port_detail {

/** Always false, but dependent, so a `static_assert` fires only on instantiation. */
template <typename T>
constexpr bool dependent_false = false;

/** Completion handler shape used only to detect a trait thunk's presence. */
template <typename T>
using Sink = std::function<void(Result<T>)>;

#define ATL_DETECT_TYPE(NAME, MEMBER)                                                    \
  template <typename T, typename = void>                                                 \
  struct NAME : std::false_type {};                                                      \
  template <typename T>                                                                  \
  struct NAME<T, std::void_t<typename BrokerTraits<T>::MEMBER>> : std::true_type {}

ATL_DETECT_TYPE(has_account_type, Account);
ATL_DETECT_TYPE(has_broker_tag, BrokerTag);
ATL_DETECT_TYPE(has_config_type, Config);
ATL_DETECT_TYPE(has_place_ack_type, PlaceAck);
ATL_DETECT_TYPE(has_amendment_type, Amendment);
ATL_DETECT_TYPE(has_history_query_type, HistoryQuery);
ATL_DETECT_TYPE(has_working_orders_type, WorkingOrdersResult);

#undef ATL_DETECT_TYPE

/** Detects the broker name constant. */
template <typename T, typename = void>
struct has_name : std::false_type {};
template <typename T>
struct has_name<T, std::void_t<decltype(std::string_view(BrokerTraits<T>::name))>>
: std::true_type {};

/** Detects the `blocking` classification constant. */
template <typename T, typename = void>
struct has_blocking_flag : std::false_type {};
template <typename T>
struct has_blocking_flag<T, std::void_t<decltype(bool(BrokerTraits<T>::blocking))>>
: std::true_type {};

/** Detects the asynchronous quote thunk, which takes a completion token. */
template <typename T, typename = void>
struct has_async_quote : std::false_type {};
template <typename T>
struct has_async_quote<
  T,
  std::void_t<
    decltype(
      BrokerTraits<T>::quote(
        std::declval<T&>(), std::declval<InstrumentId>(), std::declval<Sink<QuoteSnapshot>>()
      )
    )
  >
> : std::true_type {};

/** Detects the blocking quote thunk, which returns a result directly. */
template <typename T, typename = void>
struct has_blocking_quote : std::false_type {};
template <typename T>
struct has_blocking_quote<
  T,
  std::void_t<
    decltype(BrokerTraits<T>::quote(std::declval<T&>(), std::declval<InstrumentId>()))
  >
> : std::true_type {};

/** Detects the place-order normalizer. */
template <typename T, typename = void>
struct has_normalize_place : std::false_type {};
template <typename T>
struct has_normalize_place<
  T,
  std::void_t<
    decltype(
      BrokerTraits<T>::normalize_place(
        std::declval<Result<typename BrokerTraits<T>::PlaceAck>>()
      )
    )
  >
> : std::true_type {};

} // namespace port_detail

/**
 * Reports which mandatory trait member is absent, by name.
 *
 * Instantiating this type is what produces the diagnostic. `ATL_ASSERT_BROKER`
 * instantiates it; the message names the missing member and the instantiation
 * context names the client, because a C++17 `static_assert` message must be a
 * string literal.
 *
 * A compiler stops instantiating this body at its first failed assertion, so a
 * specialization missing several members is reported one member at a time: each
 * fix reveals the next. That is still a named member rather than a template
 * instantiation backtrace, which is the point.
 */
template <typename Client>
struct BrokerConformance {
  static_assert(
    port_detail::has_account_type<Client>::value,
    "BrokerTraits<Client>::Account is missing: declare the broker's account selector type"
  );
  static_assert(
    port_detail::has_broker_tag<Client>::value,
    "BrokerTraits<Client>::BrokerTag is missing: declare the broker's compile-time tag type, "
    "shared by its asynchronous and blocking clients"
  );
  static_assert(
    port_detail::has_config_type<Client>::value,
    "BrokerTraits<Client>::Config is missing: declare the broker's configuration type"
  );
  static_assert(
    port_detail::has_place_ack_type<Client>::value,
    "BrokerTraits<Client>::PlaceAck is missing: declare the client's order acknowledgement type"
  );
  static_assert(
    port_detail::has_amendment_type<Client>::value,
    "BrokerTraits<Client>::Amendment is missing: declare the client's amendment type"
  );
  static_assert(
    port_detail::has_history_query_type<Client>::value,
    "BrokerTraits<Client>::HistoryQuery is missing: declare the client's history query type"
  );
  static_assert(
    port_detail::has_working_orders_type<Client>::value,
    "BrokerTraits<Client>::WorkingOrdersResult is missing: declare the client's working-order "
    "result type"
  );
  static_assert(
    port_detail::has_name<Client>::value,
    "BrokerTraits<Client>::name is missing: declare a static constexpr std::string_view name"
  );
  static_assert(
    port_detail::has_blocking_flag<Client>::value,
    "BrokerTraits<Client>::blocking is missing: declare a static constexpr bool classifying the "
    "client as an asynchronous client (false) or a blocking facade (true)"
  );
  static_assert(
    port_detail::has_async_quote<Client>::value || port_detail::has_blocking_quote<Client>::value,
    "BrokerTraits<Client>::quote(client, instrument[, token]) is missing or has the wrong "
    "signature"
  );
  static_assert(
    port_detail::has_normalize_place<Client>::value,
    "BrokerTraits<Client>::normalize_place(Result<PlaceAck>) is missing or has the wrong signature"
  );

  /** Always true; reading it forces the assertions above to be checked. */
  static constexpr bool value = true;
};

/**
 * Asserts that `Client` has a conforming `BrokerTraits` specialization.
 *
 * Usable at namespace, class, and block scope. A missing member is reported by
 * name instead of as a template instantiation backtrace; see
 * `BrokerConformance` for why they are reported one at a time.
 */
#define ATL_ASSERT_BROKER(Client)                                                        \
  static_assert(                                                                         \
    ::algotradelib::BrokerConformance<Client>::value,                                     \
    "Client does not satisfy the algotradelib broker port; see the messages above"        \
  )

} // namespace algotradelib
