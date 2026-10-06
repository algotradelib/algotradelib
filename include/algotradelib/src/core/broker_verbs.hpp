#pragma once

#include "./broker_caps.hpp"
#include "./broker_session.hpp"
#include "./broker_traits.hpp"

#include <boost/asio/use_future.hpp>

#include <optional>
#include <vector>

namespace algotradelib {

/**
 * @brief The compile-time check behind `ATL_ASSERT_BROKER(Client)`.
 *
 * It calls every operation the client's capabilities claim, through a real session, with a plain
 * callback, a move-only callback and `use_future` (or directly, for a blocking client), and reads
 * every capability constant. Nothing runs. A missing member, a thunk with the wrong signature or
 * one that copies its completion token, or an unanswered capability fails the build with the
 * compiler's own message. Assert it once, in any one source file (a test is a good place):
 *
 * @code
 * ATL_ASSERT_BROKER(my::Client);
 * ATL_ASSERT_BROKER(my::BlockingClient);
 * @endcode
 *
 * It cannot prove run-time behaviour: that an asynchronous call completes exactly once and on the
 * right executor, or that a claimed capability is true of the broker.
 */
namespace conformance {

/** @brief A completion handler with no associated executor (a plain callback). */
struct PlainCallback {
  /**
   * @brief Accepts and discards any completion argument.
   * @tparam T The argument type.
   */
  template <typename T>
  void operator()(T) const {}
};

/** @brief A plain callback that cannot be copied. */
struct MoveOnlyCallback {
  /** @brief Constructs the callback. */
  MoveOnlyCallback() = default;
  /** @brief Move constructor. */
  MoveOnlyCallback(MoveOnlyCallback&&) = default;
  /**
   * @brief Move assignment.
   * @return `*this`.
   */
  MoveOnlyCallback& operator=(MoveOnlyCallback&&) = default;
  MoveOnlyCallback(const MoveOnlyCallback&) = delete;
  MoveOnlyCallback& operator=(const MoveOnlyCallback&) = delete;

  /**
   * @brief Accepts and discards any completion argument.
   * @tparam T The argument type.
   */
  template <typename T>
  void operator()(T) {}
};

/**
 * @brief Returns the placeholder instrument the checks call operations with.
 * @return An arbitrary instrument; nothing runs.
 */
inline InstrumentId probe_instrument() { return InstrumentId{Country::KR, Venue::COMBINED, "000000"}; }

/**
 * @brief Calls every operation an asynchronous session's capabilities claim; compiled, never run.
 * @tparam Client The broker client type.
 * @tparam Token The completion token type; a fresh one is moved into each call.
 * @param session The session.
 * @param ref An order reference.
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

/**
 * @brief Calls every operation a blocking session's capabilities claim; compiled, never run.
 * @tparam Client The broker client type.
 * @param session The session.
 * @param ref An order reference.
 */
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
 * @brief Compiles the operation checks for a client.
 * @tparam Client The broker client type.
 * @return Always `true`; a failing check is a compile error.
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
 * @brief Fails the build unless a broker client conforms: every operation its capabilities claim
 *   compiles through a real session, and every capability is answered.
 *
 * Usable at namespace, class and block scope. Use it in one source file per client, since it
 * compiles every claimed operation three times. See `algotradelib::conformance`.
 *
 * @param Client The broker client type.
 */
#define ATL_ASSERT_BROKER(Client)                                                        \
  static_assert(                                                                         \
    ::algotradelib::conformance::claimed_verbs_compile<Client>() &&                      \
      ::algotradelib::answers_every_capability<Client>(),                                \
    "every port verb this client's capabilities claim must compile, and every capability " \
    "must be answered"                                                                   \
  )

} // namespace algotradelib
