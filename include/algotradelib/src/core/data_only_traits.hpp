#pragma once

#include "./broker_traits.hpp"

#include <algotradelib/network>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_executor.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace algotradelib {

/**
 * @brief Placeholder types and the refusal used by `DataOnlyTraits`.
 *
 * No value of the placeholder types is ever produced: every member that would produce one refuses.
 */
namespace data_only {

/** @brief The (empty) account a data-only session binds. */
struct NoAccount {};
/** @brief Placeholder order acknowledgement; never produced. */
struct NoAck {};
/** @brief Placeholder amendment; never produced. */
struct NoAmendment {};
/** @brief Placeholder history query; never produced. */
struct NoHistoryQuery {};
/** @brief Placeholder working-order listing; never produced. */
struct NoWorkingOrders {};

/**
 * @brief Returns the error every account-side member of `DataOnlyTraits` answers with.
 *
 * `INVALID_ARGUMENT`, broker code `data_only`, and `reached_broker_ = false`: the runner reads it as
 * refused locally (nothing was sent, nothing to reconcile), not as an order of unknown outcome.
 *
 * @return The error.
 */
inline BrokerError refusal() {
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "data_only";
  error.message_ = "a market-data source holds no account and takes no order";
  return error;
}

/// \cond ATL_DATA_ONLY_DETAIL
namespace detail {

/**
 * Whether work can be posted to `Executor`.
 *
 * False for an executor that only runs work inline, and also for a type-erased completion executor
 * (`any_completion_executor`) whose erased properties do not include `blocking.never`, even when it wraps
 * an `io_context` or a strand. So false says "not a posting target", never "no execution context".
 */
template <typename Executor, typename = void>
struct postable : std::false_type {};

template <typename Executor>
struct postable<
  Executor, std::void_t<decltype(net::post(std::declval<const Executor&>(), std::declval<void (*)()>()))>
> : std::true_type {};

} // namespace detail
/// \endcond

/**
 * @brief Completes an asynchronous call with `refusal()`, posted through the handler's associated
 *   executor.
 *
 * Never invoked inside the initiating call (though on a multi-threaded executor it may run before
 * that call returns); the handler is moved once and called once, so it may be move-only. A handler
 * bound to a strand completes on that strand.
 *
 * @tparam Out The result type of the call.
 * @tparam Handler The completion handler type.
 * @param handler The handler, called with `Result<Out>(refusal())`.
 */
template <typename Out, typename Handler>
void post_refusal(Handler&& handler) {
  auto executor = net::get_associated_executor(handler, net::system_executor());
  auto complete = [handler = std::forward<Handler>(handler)]() mutable {
    std::move(handler)(Result<Out>(refusal()));
  };
  if constexpr (detail::postable<decltype(executor)>::value) {
    net::post(executor, std::move(complete));
  } else {
    net::post(net::system_executor(), net::bind_executor(executor, std::move(complete)));
  }
}

} // namespace data_only

/**
 * @brief A base for the `BrokerTraits` of a market-data client that cannot trade.
 *
 * Inherit it and declare only what the vendor serves:
 *
 * @code
 * template <>
 * struct algotradelib::BrokerTraits<my::Client> : algotradelib::DataOnlyTraits<my::Tag> {
 *   using Client = my::Client;
 *   using Config = my::Config;
 *   static constexpr std::string_view name = my::Tag::name;
 *   static constexpr bool blocking = true;
 *   static constexpr bool supports_interval(BarInterval interval);
 *   static std::optional<BrokerError> validate_bars(const BarQuery& query);
 *   static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument);
 *   static Result<BarSeries> bars(Client& client, BarQuery query);
 * };
 * ATL_ASSERT_BROKER(my::Client);  // in one source file
 * @endcode
 *
 * An asynchronous client takes the completion token as the last argument. To serve more (a
 * calendar, reference data, price limits), declare that capability `true` and add its thunk.
 *
 * Every account-side member (holdings, place, replace, cancel, history) refuses before any I/O
 * with `data_only::refusal()`; none returns success, since an empty holdings or history would be
 * read as facts about an account. So:
 * - as a backtest source, or as the market-data session of a `live::SessionSource`, it is complete;
 * - as the trading session of `live::SessionExecutor` or `live::SessionSource` (and so in
 *   `live::bind` and `live::bind_journaled`) it does not compile;
 * - `place` or `holdings` called directly on its session fail at run time;
 * - `live_program::trade` and `observe` (`examples/live_program.hpp`) also need reference data
 *   (`has_reference_data`).
 *
 * The account-side members ignore their client, account, request, reference and instruction
 * arguments, which are unnamed.
 *
 * @note A member you declare hides every inherited member of that name, overloads included
 *   (declaring a blocking `holdings` hides the asynchronous one); re-expose with
 *   `using DataOnlyTraits<Tag>::holdings;`.
 * @warning Not a base for a trading adapter that is still incomplete.
 *
 * @tparam Tag The vendor's tag type.
 */
template <typename Tag>
struct DataOnlyTraits {
  /** @brief The vendor's tag type. */
  using BrokerTag = Tag;
  /** @brief No account; the session binds an empty one. */
  using Account = data_only::NoAccount;
  /** @brief Never produced: placement refuses. */
  using PlaceAck = data_only::NoAck;
  /** @brief Never produced: amendment refuses. */
  using Amendment = data_only::NoAmendment;
  /** @brief Never produced: history refuses. */
  using HistoryQuery = data_only::NoHistoryQuery;
  /** @brief Never produced: working orders are not claimed. */
  using WorkingOrdersResult = data_only::NoWorkingOrders;

  /** @brief `true`: the client is a market-data source; see `BrokerCaps::data_only`. */
  static constexpr bool data_only = true;

  /// \cond ATL_DATA_ONLY_CAPABILITIES
  // Every capability is false until the vendor's own specialization says otherwise.
  static constexpr bool has_account_discovery = false;
  static constexpr bool has_client_order_id = false;
  static constexpr bool has_trigger_price = false;
  static constexpr bool has_working_orders = false;
  static constexpr bool has_amendable_quantity = false;
  static constexpr bool has_order_by_id = false;
  static constexpr bool has_partial_cancel = false;
  static constexpr bool history_requires_range = false;
  static constexpr bool has_history_cursor = false;
  static constexpr bool has_history_limit = false;
  static constexpr bool has_history_symbol_filter = false;
  static constexpr bool has_depth = false;
  static constexpr bool has_price_limits = false;
  static constexpr bool has_trade_prints = false;
  static constexpr bool has_batch_prices = false;
  static constexpr bool has_buying_power = false;
  static constexpr bool has_buying_power_quantity = false;
  static constexpr SellableSource sellable_source = SellableSource::NONE;
  static constexpr bool has_commission_schedule = false;
  static constexpr bool has_reference_data = false;
  static constexpr bool has_batch_reference_data = false;
  static constexpr bool has_calendar = false;
  static constexpr bool has_calendar_range = false;
  static constexpr bool has_calendar_sessions = false;
  /// \endcond

  /**
   * @brief Reports that the client runs no rate limiter; declare your own if it does.
   * @tparam Client The client type.
   * @return `nullptr`.
   */
  template <typename Client>
  static const network::RateLimitStats* rate_limit_stats(const Client&) {
    return nullptr;
  }

  /**
   * @brief Refuses every order before it is built.
   * @return `data_only::refusal()`.
   */
  static std::optional<BrokerError> validate_place(const OrderRequest&) { return data_only::refusal(); }

  /**
   * @brief Passes the refusal through (an acknowledgement is refused too).
   * @param raw The placement result.
   * @return A refusal.
   */
  static Result<PlacedOrder<Tag>> normalize_place(Result<PlaceAck> raw) {
    return Result<PlacedOrder<Tag>>(raw ? data_only::refusal() : std::move(raw.error()));
  }

  /**
   * @brief Refuses an amendment.
   * @return `data_only::refusal()`.
   */
  static Result<Amendment> to_replacement(const OrderRef<Tag>&, const AmendInstruction&) {
    return Result<Amendment>(data_only::refusal());
  }

  /**
   * @brief Refuses a cancellation.
   * @return `data_only::refusal()`.
   */
  static Result<Amendment> to_cancellation(const OrderRef<Tag>&, const AmendInstruction&) {
    return Result<Amendment>(data_only::refusal());
  }

  /**
   * @brief Refuses a history query.
   * @return `data_only::refusal()`.
   */
  static Result<HistoryQuery> to_history_query(const Account&, const OrderHistoryQuery&) {
    return Result<HistoryQuery>(data_only::refusal());
  }

  /**
   * @brief Refuses holdings.
   * @tparam Client The client type.
   * @return The refusal.
   */
  template <typename Client>
  static Result<HoldingsSnapshot> holdings(Client&, Account&) {
    return Result<HoldingsSnapshot>(data_only::refusal());
  }

  /**
   * @brief Refuses holdings, asynchronously.
   * @tparam Client The client type.
   * @tparam Handler The completion handler type.
   * @param handler Called once, through its associated executor, with the refusal.
   */
  template <typename Client, typename Handler>
  static void holdings(Client&, Account&, Handler&& handler) {
    data_only::post_refusal<HoldingsSnapshot>(std::forward<Handler>(handler));
  }

  /**
   * @brief Refuses a placement.
   * @tparam Client The client type.
   * @return The refusal.
   */
  template <typename Client>
  static Result<PlaceAck> place_order(Client&, Account&, OrderRequest) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /**
   * @brief Refuses a placement, asynchronously.
   * @tparam Client The client type.
   * @tparam Handler The completion handler type.
   * @param handler Called once, through its associated executor, with the refusal.
   */
  template <typename Client, typename Handler>
  static void place_order(Client&, Account&, OrderRequest, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /**
   * @brief Refuses a replacement.
   * @tparam Client The client type.
   * @return The refusal.
   */
  template <typename Client>
  static Result<PlaceAck> replace_order(Client&, Account&, Amendment) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /**
   * @brief Refuses a replacement, asynchronously.
   * @tparam Client The client type.
   * @tparam Handler The completion handler type.
   * @param handler Called once, through its associated executor, with the refusal.
   */
  template <typename Client, typename Handler>
  static void replace_order(Client&, Account&, Amendment, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /**
   * @brief Refuses a cancellation.
   * @tparam Client The client type.
   * @return The refusal.
   */
  template <typename Client>
  static Result<PlaceAck> cancel_order(Client&, Account&, Amendment) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /**
   * @brief Refuses a cancellation, asynchronously.
   * @tparam Client The client type.
   * @tparam Handler The completion handler type.
   * @param handler Called once, through its associated executor, with the refusal.
   */
  template <typename Client, typename Handler>
  static void cancel_order(Client&, Account&, Amendment, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /**
   * @brief Refuses order history.
   * @tparam Client The client type.
   * @return The refusal.
   */
  template <typename Client>
  static Result<OrderPage> order_history(Client&, Account&, HistoryQuery) {
    return Result<OrderPage>(data_only::refusal());
  }

  /**
   * @brief Refuses order history, asynchronously.
   * @tparam Client The client type.
   * @tparam Handler The completion handler type.
   * @param handler Called once, through its associated executor, with the refusal.
   */
  template <typename Client, typename Handler>
  static void order_history(Client&, Account&, HistoryQuery, Handler&& handler) {
    data_only::post_refusal<OrderPage>(std::forward<Handler>(handler));
  }
};

} // namespace algotradelib
