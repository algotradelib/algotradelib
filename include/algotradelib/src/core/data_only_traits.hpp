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
 * The account side of a source that has none: a market-data vendor behind the broker port.
 *
 * Holds the stand-in types and the one refusal `DataOnlyTraits` answers with. Nothing ever holds a
 * value of these types: every member that would produce one refuses first.
 */
namespace data_only {

/** The account a data vendor does not have. A session still binds one. */
struct NoAccount {};
/** Stands in for an order acknowledgement no data vendor issues. */
struct NoAck {};
/** Stands in for an amendment no data vendor accepts. */
struct NoAmendment {};
/** Stands in for an order-history query no data vendor answers. */
struct NoHistoryQuery {};
/** Stands in for a working-order listing no data vendor publishes. */
struct NoWorkingOrders {};

/**
 * The refusal every account-side member answers: refused here, before any I/O.
 *
 * **`reached_broker_` is false, and that is the point of the whole helper.** `BrokerError`'s defaults are
 * `PROTOCOL` and `reached_broker_ = true`, which `runner::outcome_of` reads as *indeterminate*: an order
 * that may exist, an instrument withheld until a reconciliation says otherwise -- one a data vendor can
 * never give, since it has no listing and no history. Stated false, the same refusal reads as
 * `REFUSED_LOCALLY`: nothing exists, no broker send was counted, nothing needs reconciling.
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
 * Completes an asynchronous call with `refusal()`, posted, through the handler's associated executor.
 *
 * Posted, never invoked inside the initiating call -- not inline, not re-entrantly -- as `BrokerSession`
 * does for its own pre-flight refusals. (Posting does not order the completion after the initiating call
 * returns: another thread of the executor may run it first.) The handler is moved once and called once,
 * so a move-only one works.
 *
 * **When the associated executor takes no posted work.** `BrokerSession` binds the handler it gives a
 * thunk to `get_associated_executor(handler)`. For a plain callback that is Asio's default candidate:
 * `system_executor` before Asio 1.37 (Boost 1.90), `inline_executor` from it, which cannot be posted to. A
 * type-erased completion executor wrapping a strand cannot be posted to either, and there the strand still
 * matters. So the system executor is only the trampoline: what is posted to it is the completion **bound
 * to the associated executor**, and Asio dispatches it through that executor -- inline for the inline
 * default, onto the strand for a strand.
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
 * A base for the `BrokerTraits` of a client that reads market data and cannot trade.
 *
 * The broker port has no read-only form: every traits specialization declares the order-side types, and
 * every session calls `holdings`, `place`, `replace`, `cancel` and `order_history` without a capability gate.
 * A market-data vendor (EODHD, Twelve Data, Yahoo, ...) therefore had to write about sixty lines of
 * refusals to be used through the port, and the obvious way to write one -- `BrokerError{}` -- is the one
 * the runner reads as an order of unknown outcome (see `data_only::refusal`). This base writes them once.
 *
 * Inherit it from the specialization and declare only what the vendor serves:
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
 * ATL_ASSERT_BROKER(my::Client);  // in one source file, as for any adapter
 * @endcode
 *
 * An asynchronous client takes the completion token as its last argument, as any adapter does. A vendor
 * that serves more than bars and quotes -- a calendar, reference data, price limits -- declares that
 * constant `true` in its own specialization, which hides this one, and adds the thunk; the conformance
 * assertions then check the thunk exactly as they do for a broker.
 *
 * **What it gives, and what it does not.** Every account-side member refuses before any I/O, in both the
 * blocking and the asynchronous shape; **none answers success**. An empty holdings snapshot or an empty
 * order history would be a claim about an account the vendor does not have, and the runner reads both as
 * evidence: they would release the startup gate and the holds a restart recovers from its journal. So:
 *
 * - **As a backtest source, or as the `MarketData` of a `live::SessionSource`** beside a broker's trading
 *   session, a data-only session is complete: those read only `bars`, `quote` and the capabilities the
 *   vendor declares.
 * - **Where orders go, it does not compile.** `live::SessionExecutor` over a data-only session, and a
 *   `live::SessionSource` whose *trading* session is one, fail a `static_assert` naming the mistake:
 *   a program that would run and quietly trade nothing is not built. (`live::bind` and
 *   `live::bind_journaled` build a `SessionExecutor`, so they are refused the same way.)
 * - **Called directly, it refuses at run time.** A caller holding the session can still call `place` or
 *   `holdings` on it; each refuses before any I/O, as above. A data client written by hand without this
 *   base declares `static constexpr bool data_only = true` itself, as every traits specialization
 *   declares the flag.
 * - `live_program::trade` and `observe` in `examples/live_program.hpp` read reference data from the
 *   market-data session, so they need a vendor that serves it (`has_reference_data`); `SessionSource` does
 *   not.
 *
 * **A member declared in the specialization hides every base member of that name**, overloads included:
 * declaring a blocking `holdings` hides the inherited asynchronous one too. Re-expose what is still wanted
 * with `using DataOnlyTraits<Tag>::holdings;`. A capability constant declared there hides the base's `false`.
 *
 * Do not use it as a base for a trading adapter with members still to write: a broker that trades must
 * answer every one of these itself.
 */
template <typename Tag>
struct DataOnlyTraits {
  /** The vendor's compile-time identity, the tag every normalized result carries. */
  using BrokerTag = Tag;
  /** No account; the session binds an empty one. */
  using Account = data_only::NoAccount;
  /** Never produced: placement refuses. */
  using PlaceAck = data_only::NoAck;
  /** Never produced: amendment refuses. */
  using Amendment = data_only::NoAmendment;
  /** Never produced: history refuses. */
  using HistoryQuery = data_only::NoHistoryQuery;
  /** Never produced: the listing is not claimed. */
  using WorkingOrdersResult = data_only::NoWorkingOrders;

  /**
   * Marks the client as a market-data source, which `BrokerCaps::data_only` reads. `live::SessionExecutor`
   * and the trading session of `live::SessionSource` refuse it at compile time.
   */
  static constexpr bool data_only = true;

  /// \cond ATL_DATA_ONLY_CAPABILITIES
  // Every capability answered, and every one false: a data vendor claims none of the account side, and
  // none of the optional market data until its own specialization says so.
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

  /** Answers that no limiter runs inside the client. A vendor whose client paces may hide this. */
  template <typename Client>
  static const network::RateLimitStats* rate_limit_stats(const Client&) {
    return nullptr;
  }

  /** Refuses every order before it is built, so `place_order` is never reached through a session. */
  static std::optional<BrokerError> validate_place(const OrderRequest&) { return data_only::refusal(); }

  /** Passes a refusal through; an acknowledgement cannot exist, and would be refused too. */
  static Result<PlacedOrder<Tag>> normalize_place(Result<PlaceAck> raw) {
    return Result<PlacedOrder<Tag>>(raw ? data_only::refusal() : std::move(raw.error()));
  }

  /** Refuses an amendment, so `replace_order` is never reached through a session. */
  static Result<Amendment> to_replacement(const OrderRef<Tag>&, const AmendInstruction&) {
    return Result<Amendment>(data_only::refusal());
  }

  /** Refuses a cancellation, so `cancel_order` is never reached through a session. */
  static Result<Amendment> to_cancellation(const OrderRef<Tag>&, const AmendInstruction&) {
    return Result<Amendment>(data_only::refusal());
  }

  /** Refuses a history query, so `order_history` is never reached through a session. */
  static Result<HistoryQuery> to_history_query(const Account&, const OrderHistoryQuery&) {
    return Result<HistoryQuery>(data_only::refusal());
  }

  /** Refuses: there are no holdings to report, and an empty snapshot would claim there are none. */
  template <typename Client>
  static Result<HoldingsSnapshot> holdings(Client&, Account&) {
    return Result<HoldingsSnapshot>(data_only::refusal());
  }

  /** The asynchronous `holdings`: the refusal, posted. */
  template <typename Client, typename Handler>
  static void holdings(Client&, Account&, Handler&& handler) {
    data_only::post_refusal<HoldingsSnapshot>(std::forward<Handler>(handler));
  }

  /** Refuses a placement. */
  template <typename Client>
  static Result<PlaceAck> place_order(Client&, Account&, OrderRequest) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /** The asynchronous `place_order`: the refusal, posted. */
  template <typename Client, typename Handler>
  static void place_order(Client&, Account&, OrderRequest, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /** Refuses a replacement. */
  template <typename Client>
  static Result<PlaceAck> replace_order(Client&, Account&, Amendment) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /** The asynchronous `replace_order`: the refusal, posted. */
  template <typename Client, typename Handler>
  static void replace_order(Client&, Account&, Amendment, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /** Refuses a cancellation. */
  template <typename Client>
  static Result<PlaceAck> cancel_order(Client&, Account&, Amendment) {
    return Result<PlaceAck>(data_only::refusal());
  }

  /** The asynchronous `cancel_order`: the refusal, posted. */
  template <typename Client, typename Handler>
  static void cancel_order(Client&, Account&, Amendment, Handler&& handler) {
    data_only::post_refusal<PlaceAck>(std::forward<Handler>(handler));
  }

  /** Refuses: there is no history, and an empty page would claim nothing was ever sent. */
  template <typename Client>
  static Result<OrderPage> order_history(Client&, Account&, HistoryQuery) {
    return Result<OrderPage>(data_only::refusal());
  }

  /** The asynchronous `order_history`: the refusal, posted. */
  template <typename Client, typename Handler>
  static void order_history(Client&, Account&, HistoryQuery, Handler&& handler) {
    data_only::post_refusal<OrderPage>(std::forward<Handler>(handler));
  }
};

} // namespace algotradelib
