#pragma once

#include "./broker_caps.hpp"
#include "./broker_session.hpp"
#include "./broker_traits.hpp"

#include <boost/asio/use_future.hpp>

#include <optional>
#include <vector>

namespace algotradelib {

/**
 * Compiles every port verb a client's capabilities claim.
 *
 * `ATL_ASSERT_BROKER` checks a traits specialization when a session is built, and it checks an
 * asynchronous thunk **by name only**: the handler a session hands a thunk is built around the
 * caller's own handler, whose executor type is the caller's choice and which may be move-only, so no
 * stand-in handler can probe a call without misjudging some thunk that works. That leaves a thunk
 * with the wrong signature undetected until its verb is first called -- which, for a capability no
 * caller happens to use yet, can be never. `BrokerTraits<kis::KisClient>` claimed reference data for
 * eight slices with no thunk behind it for exactly that reason.
 *
 * This closes the gap by doing what a caller would: it calls every claimed verb, through the real
 * session, with a plain callback, a move-only callback and `use_future`, and discards nothing but the
 * calls themselves -- the functions are compiled and never run. A capability whose verb does not
 * compile for one of those tokens fails the build here, with the compiler's own diagnostic at the
 * thunk.
 *
 * **An adapter author asserts it once, in any one source file of their own** -- a test is the natural
 * place, since it instantiates the whole broker:
 *
 * @code
 * ATL_ASSERT_BROKER_VERBS(my::Client);
 * ATL_ASSERT_BROKER_VERBS(my::BlockingClient);
 * @endcode
 *
 * The library asserts it for every shipped client in `tests/test-capability-conformance.cpp`.
 */
namespace conformance {

/** A completion handler naming no executor, which is what a plain callback is. */
struct PlainCallback {
  /** Accepts and discards any completion argument. */
  template <typename T>
  void operator()(T) const {}
};

/** A plain callback that cannot be copied, which a thunk that copies its token would not accept. */
struct MoveOnlyCallback {
  /** Constructs an empty callback. */
  MoveOnlyCallback() = default;
  /** Moves a callback. */
  MoveOnlyCallback(MoveOnlyCallback&&) = default;
  /** Moves a callback. */
  MoveOnlyCallback& operator=(MoveOnlyCallback&&) = default;
  MoveOnlyCallback(const MoveOnlyCallback&) = delete;
  MoveOnlyCallback& operator=(const MoveOnlyCallback&) = delete;

  /** Accepts and discards any completion argument. */
  template <typename T>
  void operator()(T) {}
};

/** The instrument every verb is called with; its value is irrelevant, since nothing runs. */
inline InstrumentId probe_instrument() { return InstrumentId{Country::KR, Venue::COMBINED, "000000"}; }

/**
 * Calls every verb an asynchronous session's capabilities claim, with a fresh `Token` each time so
 * a move-only one is moved into each verb rather than copied. Compiled, never run.
 */
template <typename Client, typename Token>
void call_claimed_async_verbs(
  BrokerSession<Client>& session, const typename BrokerSession<Client>::Ref& ref
) {
  using Caps = BrokerCaps<Client>;
  session.async_quote(probe_instrument(), Token{});
  session.async_holdings(Token{});
  session.async_bars(BarQuery{}, Token{});
  session.async_place(OrderRequest{}, Token{});
  session.async_replace(ref, AmendInstruction{}, Token{});
  session.async_cancel(ref, AmendInstruction{}, Token{});
  session.async_order_history(OrderHistoryQuery{}, Token{});
  if constexpr (Caps::has_buying_power) {
    session.async_buying_power(Currency{}, Token{});
  }
  if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
    session.async_sellable_quantity(probe_instrument(), Token{});
  }
  if constexpr (Caps::has_commission_schedule) {
    session.async_commissions(Token{});
  }
  if constexpr (Caps::has_working_orders) {
    session.async_working_orders(Token{});
  }
  if constexpr (Caps::has_reference_data) {
    session.async_instrument(probe_instrument(), Token{});
  }
  if constexpr (Caps::has_calendar) {
    session.async_calendar(Country::KR, std::nullopt, Token{});
  }
  if constexpr (Caps::has_price_limits) {
    session.async_price_limits(probe_instrument(), Token{});
  }
  if constexpr (Caps::has_trade_prints) {
    session.async_trades(probe_instrument(), 1, Token{});
  }
  if constexpr (Caps::has_batch_prices) {
    session.async_prices(std::vector<InstrumentId>{probe_instrument()}, Token{});
  }
  if constexpr (Caps::has_depth) {
    session.async_depth(probe_instrument(), Token{});
  }
  if constexpr (Caps::has_order_by_id) {
    session.async_order(ref, Token{});
  }
  if constexpr (Caps::has_account_discovery) {
    async_accounts(session.client(), Token{});
  }
}

/** The blocking counterpart: every verb a blocking session's capabilities claim. Compiled, never run. */
template <typename Client>
void call_claimed_blocking_verbs(
  BlockingSession<Client>& session, const typename BlockingSession<Client>::Ref& ref
) {
  using Caps = BrokerCaps<Client>;
  static_cast<void>(session.quote(probe_instrument()));
  static_cast<void>(session.holdings());
  static_cast<void>(session.bars(BarQuery{}));
  static_cast<void>(session.place(OrderRequest{}));
  static_cast<void>(session.replace(ref, AmendInstruction{}));
  static_cast<void>(session.cancel(ref, AmendInstruction{}));
  static_cast<void>(session.order_history(OrderHistoryQuery{}));
  if constexpr (Caps::has_buying_power) {
    static_cast<void>(session.buying_power(Currency{}));
  }
  if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
    static_cast<void>(session.sellable_quantity(probe_instrument()));
  }
  if constexpr (Caps::has_commission_schedule) {
    static_cast<void>(session.commissions());
  }
  if constexpr (Caps::has_working_orders) {
    static_cast<void>(session.working_orders());
  }
  if constexpr (Caps::has_reference_data) {
    static_cast<void>(session.instrument(probe_instrument()));
  }
  if constexpr (Caps::has_calendar) {
    static_cast<void>(session.calendar(Country::KR));
  }
  if constexpr (Caps::has_price_limits) {
    static_cast<void>(session.price_limits(probe_instrument()));
  }
  if constexpr (Caps::has_trade_prints) {
    static_cast<void>(session.trades(probe_instrument(), 1));
  }
  if constexpr (Caps::has_batch_prices) {
    static_cast<void>(session.prices(std::vector<InstrumentId>{probe_instrument()}));
  }
  if constexpr (Caps::has_depth) {
    static_cast<void>(session.depth(probe_instrument()));
  }
  if constexpr (Caps::has_order_by_id) {
    static_cast<void>(session.order(ref));
  }
  if constexpr (Caps::has_account_discovery) {
    static_cast<void>(session.accounts());
    static_cast<void>(list_accounts(session.client()));
  }
}

/**
 * Instantiates the verb callers for `Client` by taking their addresses, which compiles them.
 *
 * `constexpr` so `ATL_ASSERT_BROKER_VERBS` can read it in a `static_assert` at any scope; the value
 * is always true, and what matters is the instantiation it forces.
 */
template <typename Client>
constexpr bool claimed_verbs_compile() {
  if constexpr (BrokerTraits<Client>::blocking) {
    static_cast<void>(&call_claimed_blocking_verbs<Client>);
  } else {
    static_cast<void>(&call_claimed_async_verbs<Client, PlainCallback>);
    static_cast<void>(&call_claimed_async_verbs<Client, MoveOnlyCallback>);
    static_cast<void>(&call_claimed_async_verbs<Client, boost::asio::use_future_t<>>);
  }
  return true;
}

} // namespace conformance

/**
 * Compiles every port verb `Client`'s capabilities claim, through the real session, and fails the
 * build if one does not compile. See `algotradelib::conformance`.
 *
 * Usable at namespace, class and block scope. Stronger than `ATL_ASSERT_BROKER`, which it does not
 * replace: that one names a missing member in its own words, this one reports a wrong signature in
 * the compiler's.
 */
#define ATL_ASSERT_BROKER_VERBS(Client)                                                  \
  static_assert(                                                                         \
    ::algotradelib::conformance::claimed_verbs_compile<Client>(),                        \
    "every port verb this client's capabilities claim must compile"                       \
  )

} // namespace algotradelib
