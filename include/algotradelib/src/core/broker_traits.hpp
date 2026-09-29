#pragma once

#include "./calendar.hpp"
#include "./instrument.hpp"
#include "./market_data.hpp"
#include "./types.hpp"

#include <cstddef>
#include <functional>
#include <iterator>
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

/**
 * Detects that the traits declare a member called NAME, of any kind and any signature.
 *
 * Used for every **asynchronous** thunk, and deliberately no stronger. A thunk taking a completion
 * token is handed whatever `BrokerSession::translate` builds around the *caller's* handler: bound to
 * the caller's executor, whose type is the caller's choice, and possibly move-only. No single
 * stand-in handler models all of those, so probing a call with one either rejects a thunk that works
 * for the caller's actual handler -- one that requires a concrete executor type, say -- or, where the
 * thunk's return type is deduced, fails with a hard error inside its body before any named message.
 * A lookup that is ambiguous exactly when the traits declare the name instantiates no body and can
 * reject nothing that works. What it cannot see is the signature; that is compiled where the verb is
 * first called, and for the shipped adapters by `tests/test-capability-conformance.cpp`, which calls
 * every claimed verb with a plain, a move-only and a `use_future` token.
 *
 * A `final` traits class cannot be derived from for the lookup, so it is read as declaring the name:
 * the check then passes and the verb's own call site reports any absence.
 */
#define ATL_DETECT_NAME(NAME)                                                            \
  struct NAME##_name_fallback {                                                          \
    int NAME;                                                                            \
  };                                                                                     \
  template <typename B>                                                                  \
  struct NAME##_name_probe : B, NAME##_name_fallback {};                                 \
  template <typename T, typename = void>                                                 \
  struct names_##NAME##_lookup : std::true_type {};                                      \
  template <typename T>                                                                  \
  struct names_##NAME##_lookup<                                                          \
    T, std::void_t<decltype(&NAME##_name_probe<BrokerTraits<T>>::NAME)>                  \
  > : std::false_type {};                                                                \
  template <typename T>                                                                  \
  struct names_##NAME                                                                    \
  : std::conditional_t<                                                                  \
      std::is_final_v<BrokerTraits<T>>, std::true_type, names_##NAME##_lookup<T>         \
    > {}

ATL_DETECT_NAME(quote);
ATL_DETECT_NAME(buying_power);
ATL_DETECT_NAME(sellable_quantity);
ATL_DETECT_NAME(commissions);
ATL_DETECT_NAME(working_orders);
ATL_DETECT_NAME(instrument);
ATL_DETECT_NAME(calendar);
ATL_DETECT_NAME(price_limits);
ATL_DETECT_NAME(trades);
ATL_DETECT_NAME(prices);
ATL_DETECT_NAME(depth);
ATL_DETECT_NAME(order_by_id);
ATL_DETECT_NAME(accounts);

#undef ATL_DETECT_NAME

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

/** Detects the asynchronous quote thunk, by name; see `names_quote`. */
template <typename T>
using has_async_quote = names_quote<T>;

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

/*
 * Capability conformance.
 *
 * A capability constant is a promise that the matching port verb compiles, and nothing checked it:
 * every gated verb sits inside an `if constexpr` of a member template, so a verb nobody calls has
 * never been compiled. `BrokerTraits<kis::KisClient>` claimed reference data for eight slices with
 * no `instrument` thunk behind it. The detectors below read each constant only once it is known to
 * be declared, and ask for the thunk behind it: a blocking thunk in the full shape the blocking
 * session calls it, an asynchronous one by name (see `ATL_DETECT_NAME` for why no stronger).
 */

/** Detects each capability constant `BrokerCaps` reads without a default. */
#define ATL_DETECT_CONSTANT(NAME, TYPE)                                                  \
  template <typename T, typename = void>                                                 \
  struct declares_##NAME : std::false_type {};                                           \
  template <typename T>                                                                  \
  struct declares_##NAME<T, std::void_t<decltype(TYPE(BrokerTraits<T>::NAME))>>          \
  : std::true_type {}

ATL_DETECT_CONSTANT(has_account_discovery, bool);
ATL_DETECT_CONSTANT(has_client_order_id, bool);
ATL_DETECT_CONSTANT(has_working_orders, bool);
ATL_DETECT_CONSTANT(has_amendable_quantity, bool);
ATL_DETECT_CONSTANT(has_order_by_id, bool);
ATL_DETECT_CONSTANT(has_partial_cancel, bool);
ATL_DETECT_CONSTANT(history_requires_range, bool);
ATL_DETECT_CONSTANT(has_history_cursor, bool);
ATL_DETECT_CONSTANT(has_history_limit, bool);
ATL_DETECT_CONSTANT(has_history_symbol_filter, bool);
ATL_DETECT_CONSTANT(has_depth, bool);
ATL_DETECT_CONSTANT(has_price_limits, bool);
ATL_DETECT_CONSTANT(has_trade_prints, bool);
ATL_DETECT_CONSTANT(has_batch_prices, bool);
ATL_DETECT_CONSTANT(has_buying_power, bool);
ATL_DETECT_CONSTANT(has_buying_power_quantity, bool);
ATL_DETECT_CONSTANT(sellable_source, SellableSource);
ATL_DETECT_CONSTANT(has_commission_schedule, bool);
ATL_DETECT_CONSTANT(has_reference_data, bool);
ATL_DETECT_CONSTANT(has_batch_reference_data, bool);
ATL_DETECT_CONSTANT(has_calendar, bool);
ATL_DETECT_CONSTANT(has_calendar_range, bool);
ATL_DETECT_CONSTANT(has_calendar_sessions, bool);

#undef ATL_DETECT_CONSTANT

/** Detects the per-interval bar predicate. */
template <typename T, typename = void>
struct declares_supports_interval : std::false_type {};
template <typename T>
struct declares_supports_interval<
  T, std::void_t<decltype(bool(BrokerTraits<T>::supports_interval(std::declval<BarInterval>())))>
> : std::true_type {};

/** Whether the client is a blocking facade; false when the flag is undeclared. */
template <typename T>
constexpr bool is_blocking_client() {
  if constexpr (has_blocking_flag<T>::value) {
    return BrokerTraits<T>::blocking;
  } else {
    return false;
  }
}

/** Placeholder for the bound account, passed as the non-const lvalue a session holds. */
struct AccountArg {};

/**
 * Resolves `AccountArg` to the traits' account selector; every other argument type is kept.
 *
 * Where the traits declare no `Account` the placeholder is left unresolved, which no thunk accepts,
 * so the detector answers false instead of failing outside its substitution context -- the missing
 * `Account` is already reported on its own.
 */
template <typename T, typename A, typename = void>
struct thunk_arg {
  using type = A;
};
template <typename T>
struct thunk_arg<T, AccountArg, std::void_t<typename BrokerTraits<T>::Account>> {
  using type = typename BrokerTraits<T>::Account&;
};

/** The argument types a session passes to a thunk after the client, in order. */
template <typename... A>
struct ThunkArgs {};

/**
 * Detects one gated thunk in the blocking shape: the arguments the blocking session passes, and a
 * return a `Result<OUT>` can be built from, which is what the blocking session returns it as. No
 * handler is involved, so here the whole call can be checked. The asynchronous shape is detected by
 * name; see `ATL_DETECT_NAME`.
 */
#define ATL_DETECT_THUNK(NAME, OUT)                                                      \
  template <typename T, typename Args, typename = void>                                  \
  struct has_blocking_##NAME##_in : std::false_type {};                                  \
  template <typename T, typename... A>                                                   \
  struct has_blocking_##NAME##_in<                                                       \
    T, ThunkArgs<A...>,                                                                  \
    std::enable_if_t<std::is_convertible_v<                                              \
      decltype(BrokerTraits<T>::NAME(                                                    \
        std::declval<T&>(), std::declval<typename thunk_arg<T, A>::type>()...            \
      )),                                                                                \
      Result<OUT>                                                                        \
    >>                                                                                   \
  > : std::true_type {}

ATL_DETECT_THUNK(buying_power, BuyingPower);
ATL_DETECT_THUNK(sellable_quantity, Decimal64);
ATL_DETECT_THUNK(commissions, CommissionSchedule);
ATL_DETECT_THUNK(working_orders, typename BrokerTraits<T>::WorkingOrdersResult);
ATL_DETECT_THUNK(instrument, Instrument);
ATL_DETECT_THUNK(calendar, TradingCalendar);
ATL_DETECT_THUNK(price_limits, PriceLimits);
ATL_DETECT_THUNK(trades, std::vector<TradePrint>);
ATL_DETECT_THUNK(prices, std::vector<QuoteSnapshot>);
ATL_DETECT_THUNK(depth, DepthSnapshot);
ATL_DETECT_THUNK(order_by_id, OrderSnapshot);

#undef ATL_DETECT_THUNK

/**
 * Detects the working-order normalizer the session applies to every listing, answering what the
 * session returns: `Result<WorkingOrders<BrokerTag>>`.
 */
template <typename T, typename = void>
struct has_normalize_working_orders : std::false_type {};
template <typename T>
struct has_normalize_working_orders<
  T,
  std::enable_if_t<std::is_convertible_v<
    decltype(BrokerTraits<T>::normalize_working_orders(
      std::declval<Result<typename BrokerTraits<T>::WorkingOrdersResult>>()
    )),
    Result<WorkingOrders<typename BrokerTraits<T>::BrokerTag>>
  >>
> : std::true_type {};

/**
 * Whether `R` is usable the way the session uses a pre-flight answer: tested for truth, and when
 * true, dereferenced and passed *by value* as the `BrokerError` it completes with -- so the
 * dereference must convert implicitly, not merely construct one.
 */
template <typename R, typename = void>
struct is_refusal : std::false_type {};
template <typename R>
struct is_refusal<
  R,
  std::enable_if_t<
    std::is_constructible_v<bool, R&> &&
    std::is_convertible_v<decltype(std::move(*std::declval<R&>())), BrokerError>
  >
> : std::true_type {};

/** Detects the calendar pre-flight check the session runs before any request. */
template <typename T, typename = void>
struct has_validate_calendar : std::false_type {};
template <typename T>
struct has_validate_calendar<
  T,
  std::enable_if_t<is_refusal<decltype(BrokerTraits<T>::validate_calendar(
    std::declval<Country&>(), std::declval<std::optional<Date>&>()
  ))>::value>
> : std::true_type {};

/** The item range of a `Result<Items>`; absent for any other type, so a mismatch stays a substitution failure. */
template <typename R>
struct listed_items {};
template <typename Items>
struct listed_items<Result<Items>> {
  using type = Items;
};

namespace account_rows {

using std::begin;
using std::end;

/**
 * The row `to_account_info` binds, `const auto& item : raw.value()`, found as a range-for finds it --
 * and declared only where the whole loop is well-formed: `begin`, `end`, the comparison between
 * them, the increment and the dereference.
 */
template <typename Items>
auto row_of(Items& items) -> decltype(
  static_cast<void>(begin(items) != end(items)),
  static_cast<void>(++std::declval<decltype(begin(items))&>()),
  std::declval<const std::remove_reference_t<decltype(*begin(items))>&>()
);

}  // namespace account_rows

/**
 * Whether a listing's items go through `to_account_info`, as it is written: `size()` for the
 * reservation, a range-for over the items, a default-constructed `AccountInfo` whose three members
 * are assigned from `display_of`, `type_of` and `selector_of`, and a move into the result.
 */
template <typename T, typename Items, typename = void>
struct normalizes_account_items : std::false_type {};
template <typename T, typename Items>
struct normalizes_account_items<
  T, Items,
  std::enable_if_t<
    std::is_convertible_v<decltype(std::declval<Items&>().size()), std::size_t> &&
    std::is_default_constructible_v<typename BrokerTraits<T>::Account> &&
    std::is_move_constructible_v<typename BrokerTraits<T>::Account> &&
    std::is_assignable_v<
      std::string&,
      decltype(BrokerTraits<T>::display_of(account_rows::row_of(std::declval<Items&>())))
    > &&
    std::is_assignable_v<
      std::string&,
      decltype(BrokerTraits<T>::type_of(account_rows::row_of(std::declval<Items&>())))
    > &&
    std::is_assignable_v<
      typename BrokerTraits<T>::Account&,
      decltype(BrokerTraits<T>::selector_of(account_rows::row_of(std::declval<Items&>())))
    >
  >
> : std::true_type {};

/** Detects the blocking account listing, and that its rows normalize. */
template <typename T, typename = void>
struct has_blocking_accounts : std::false_type {};
template <typename T>
struct has_blocking_accounts<
  T,
  std::enable_if_t<normalizes_account_items<
    T, typename listed_items<decltype(BrokerTraits<T>::accounts(std::declval<T&>()))>::type
  >::value>
> : std::true_type {};

/**
 * Whether a thunk is present in the shape the client's `blocking` flag selects.
 *
 * `Async` is the name detector for the verb and `Blocking` its full blocking-shape detector.
 */
template <typename T, typename Async, typename Blocking>
constexpr bool thunk_present() {
  if constexpr (is_blocking_client<T>()) {
    return Blocking::value;
  } else {
    return Async::value;
  }
}

/**
 * Defines `serves_<NAME><T>`: true when the capability is unclaimed or undeclared, or when it is
 * claimed and every member the gated verb calls is present.
 *
 * Undeclared reads as served here because the missing constant is reported on its own, by name;
 * reporting a thunk as well would bury that message under a second one about the same omission.
 */
#define ATL_SERVES(NAME, CLAIMED, PRESENT)                                               \
  template <typename T>                                                                  \
  constexpr bool serves_##NAME##_impl() {                                                \
    if constexpr (!(CLAIMED)) {                                                          \
      return true;                                                                       \
    } else {                                                                             \
      return PRESENT;                                                                    \
    }                                                                                    \
  }                                                                                      \
  template <typename T>                                                                  \
  inline constexpr bool serves_##NAME = serves_##NAME##_impl<T>()

/** Whether a declared boolean capability is claimed; false while undeclared. */
#define ATL_CLAIMS(NAME) (declares_##NAME<T>::value && claimed_##NAME<T>())

/** Reads each boolean capability only once it is known to exist. */
#define ATL_CLAIMED(NAME)                                                                \
  template <typename T>                                                                  \
  constexpr bool claimed_##NAME() {                                                      \
    if constexpr (declares_##NAME<T>::value) {                                           \
      return bool(BrokerTraits<T>::NAME);                                                \
    } else {                                                                             \
      return false;                                                                      \
    }                                                                                    \
  }

ATL_CLAIMED(has_buying_power)
ATL_CLAIMED(has_commission_schedule)
ATL_CLAIMED(has_working_orders)
ATL_CLAIMED(has_reference_data)
ATL_CLAIMED(has_calendar)
ATL_CLAIMED(has_price_limits)
ATL_CLAIMED(has_trade_prints)
ATL_CLAIMED(has_batch_prices)
ATL_CLAIMED(has_depth)
ATL_CLAIMED(has_order_by_id)
ATL_CLAIMED(has_account_discovery)

/** Whether the traits say the sellable figure comes from a separate inquiry. */
template <typename T>
constexpr bool claimed_sellable_inquiry() {
  if constexpr (declares_sellable_source<T>::value) {
    return BrokerTraits<T>::sellable_source == SellableSource::INQUIRY;
  } else {
    return false;
  }
}

ATL_SERVES(
  buying_power, ATL_CLAIMS(has_buying_power),
  (thunk_present<
    T, names_buying_power<T>,
    has_blocking_buying_power_in<T, ThunkArgs<AccountArg, Currency&>>
  >())
);
ATL_SERVES(
  sellable_quantity, claimed_sellable_inquiry<T>(),
  (thunk_present<
    T, names_sellable_quantity<T>,
    has_blocking_sellable_quantity_in<T, ThunkArgs<AccountArg, InstrumentId>>
  >())
);
ATL_SERVES(
  commissions, ATL_CLAIMS(has_commission_schedule),
  (thunk_present<
    T, names_commissions<T>,
    has_blocking_commissions_in<T, ThunkArgs<AccountArg>>
  >())
);
ATL_SERVES(
  working_orders, ATL_CLAIMS(has_working_orders),
  (thunk_present<
    T, names_working_orders<T>,
    has_blocking_working_orders_in<T, ThunkArgs<AccountArg>>
  >() && has_normalize_working_orders<T>::value)
);
ATL_SERVES(
  reference_data, ATL_CLAIMS(has_reference_data),
  (thunk_present<
    T, names_instrument<T>,
    has_blocking_instrument_in<T, ThunkArgs<InstrumentId>>
  >())
);
ATL_SERVES(
  calendar, ATL_CLAIMS(has_calendar),
  (thunk_present<
    T, names_calendar<T>,
    has_blocking_calendar_in<T, ThunkArgs<Country&, std::optional<Date>&>>
  >() && has_validate_calendar<T>::value)
);
ATL_SERVES(
  price_limits, ATL_CLAIMS(has_price_limits),
  (thunk_present<
    T, names_price_limits<T>,
    has_blocking_price_limits_in<T, ThunkArgs<InstrumentId>>
  >())
);
ATL_SERVES(
  trade_prints, ATL_CLAIMS(has_trade_prints),
  (thunk_present<
    T, names_trades<T>,
    has_blocking_trades_in<T, ThunkArgs<InstrumentId, int&>>
  >())
);
ATL_SERVES(
  batch_prices, ATL_CLAIMS(has_batch_prices),
  (thunk_present<
    T, names_prices<T>,
    has_blocking_prices_in<T, ThunkArgs<std::vector<InstrumentId>>>
  >())
);
ATL_SERVES(
  depth, ATL_CLAIMS(has_depth),
  (thunk_present<
    T, names_depth<T>,
    has_blocking_depth_in<T, ThunkArgs<InstrumentId>>
  >())
);
ATL_SERVES(
  order_by_id, ATL_CLAIMS(has_order_by_id),
  (thunk_present<
    T, names_order_by_id<T>,
    has_blocking_order_by_id_in<T, ThunkArgs<AccountArg, const std::string&>>
  >())
);
ATL_SERVES(
  account_discovery, ATL_CLAIMS(has_account_discovery),
  (thunk_present<T, names_accounts<T>, has_blocking_accounts<T>>())
);

#undef ATL_CLAIMED
#undef ATL_CLAIMS
#undef ATL_SERVES

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

  /// \cond ATL_CONFORMANCE_ASSERTIONS
  // One named assertion per capability constant; hidden from Doxygen, which reads each macro
  // invocation as an undocumented member.
#define ATL_REQUIRE_CONSTANT(NAME, TYPE)                                                 \
  static_assert(                                                                         \
    port_detail::declares_##NAME<Client>::value,                                         \
    "BrokerTraits<Client>::" #NAME " is missing: declare static constexpr " #TYPE " " #NAME \
    "; every capability is answered explicitly, so a broker never claims one by default"  \
  )

  ATL_REQUIRE_CONSTANT(has_account_discovery, bool);
  ATL_REQUIRE_CONSTANT(has_client_order_id, bool);
  ATL_REQUIRE_CONSTANT(has_working_orders, bool);
  ATL_REQUIRE_CONSTANT(has_amendable_quantity, bool);
  ATL_REQUIRE_CONSTANT(has_order_by_id, bool);
  ATL_REQUIRE_CONSTANT(has_partial_cancel, bool);
  ATL_REQUIRE_CONSTANT(history_requires_range, bool);
  ATL_REQUIRE_CONSTANT(has_history_cursor, bool);
  ATL_REQUIRE_CONSTANT(has_history_limit, bool);
  ATL_REQUIRE_CONSTANT(has_history_symbol_filter, bool);
  ATL_REQUIRE_CONSTANT(has_depth, bool);
  ATL_REQUIRE_CONSTANT(has_price_limits, bool);
  ATL_REQUIRE_CONSTANT(has_trade_prints, bool);
  ATL_REQUIRE_CONSTANT(has_batch_prices, bool);
  ATL_REQUIRE_CONSTANT(has_buying_power, bool);
  ATL_REQUIRE_CONSTANT(has_buying_power_quantity, bool);
  ATL_REQUIRE_CONSTANT(sellable_source, SellableSource);
  ATL_REQUIRE_CONSTANT(has_commission_schedule, bool);
  ATL_REQUIRE_CONSTANT(has_reference_data, bool);
  ATL_REQUIRE_CONSTANT(has_batch_reference_data, bool);
  ATL_REQUIRE_CONSTANT(has_calendar, bool);
  ATL_REQUIRE_CONSTANT(has_calendar_range, bool);
  ATL_REQUIRE_CONSTANT(has_calendar_sessions, bool);

#undef ATL_REQUIRE_CONSTANT
  /// \endcond

  static_assert(
    port_detail::declares_supports_interval<Client>::value,
    "BrokerTraits<Client>::supports_interval(BarInterval) is missing: declare a static constexpr "
    "predicate answering which bar intervals the broker publishes"
  );

  // A claimed capability is a verb a caller may compile against, so the thunk behind it must
  // exist: for a blocking facade in the whole shape the blocking session calls it, and for an
  // asynchronous client by name, since its handler type is the caller's choice.
  static_assert(
    port_detail::serves_buying_power<Client>,
    "BrokerTraits<Client>::has_buying_power is true but buying_power(client, account, currency"
    "[, token]) is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_sellable_quantity<Client>,
    "BrokerTraits<Client>::sellable_source is INQUIRY but sellable_quantity(client, account, "
    "instrument[, token]) is missing or has the wrong signature: define it, or name another source"
  );
  static_assert(
    port_detail::serves_commissions<Client>,
    "BrokerTraits<Client>::has_commission_schedule is true but commissions(client, account"
    "[, token]) is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_working_orders<Client>,
    "BrokerTraits<Client>::has_working_orders is true but working_orders(client, account[, token]) "
    "or normalize_working_orders(Result<WorkingOrdersResult>) -> Result<WorkingOrders<BrokerTag>> "
    "is missing or has the wrong signature: define both, or declare the capability false"
  );
  static_assert(
    port_detail::serves_reference_data<Client>,
    "BrokerTraits<Client>::has_reference_data is true but instrument(client, instrument[, token]) "
    "is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_calendar<Client>,
    "BrokerTraits<Client>::has_calendar is true but calendar(client, country, from[, token]) or "
    "validate_calendar(country, from) is missing or has the wrong signature -- validate_calendar "
    "answers something that tests true on a refusal and dereferences to a BrokerError, such as "
    "std::optional<BrokerError>: define both, or declare the capability false"
  );
  static_assert(
    port_detail::serves_price_limits<Client>,
    "BrokerTraits<Client>::has_price_limits is true but price_limits(client, instrument[, token]) "
    "is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_trade_prints<Client>,
    "BrokerTraits<Client>::has_trade_prints is true but trades(client, instrument, count"
    "[, token]) is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_batch_prices<Client>,
    "BrokerTraits<Client>::has_batch_prices is true but prices(client, instruments[, token]) is "
    "missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_depth<Client>,
    "BrokerTraits<Client>::has_depth is true but depth(client, instrument[, token]) is missing or "
    "has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_order_by_id<Client>,
    "BrokerTraits<Client>::has_order_by_id is true but order_by_id(client, account, id[, token]) "
    "is missing or has the wrong signature: define it, or declare the capability false"
  );
  static_assert(
    port_detail::serves_account_discovery<Client>,
    "BrokerTraits<Client>::has_account_discovery is true but accounts(client[, token]) is missing "
    "or has the wrong signature, or a blocking client's rows do not pass through display_of, "
    "type_of and selector_of: define them, or declare the capability false"
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
