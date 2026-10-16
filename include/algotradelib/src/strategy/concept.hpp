#pragma once

#include "./event.hpp"
#include "./intent.hpp"
#include "./portfolio.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * @file
 * @brief What a strategy is, how a runner calls its handlers, and `ATL_ASSERT_STRATEGY`.
 *
 * A strategy is a plain struct whose handlers are member **templates** on the context type. Every
 * handler is optional; declare only the ones you need:
 *
 * ```cpp
 * struct MyStrategy {
 *   struct Params { std::size_t window_{20}; };
 *   Params params_;
 *
 *   template <class Ctx> void on_start(Ctx& ctx);
 *   template <class Ctx> void on_bar(Ctx& ctx, const BarEvent& bar);     // or on_bars, not both
 *   template <class Ctx> void on_quote(Ctx& ctx, const QuoteEvent& quote);
 *   template <class Ctx> void on_fill(Ctx& ctx, const Fill& fill);
 *   template <class Ctx> void on_order_update(Ctx& ctx, const OrderSnapshot& order);
 *   template <class Ctx> void on_phase(Ctx& ctx, const MarketPhase& phase);
 *   template <class Ctx> void on_stop(Ctx& ctx);
 * };
 * ```
 *
 * `on_bar` receives one bar at a time. A strategy that decides on several instruments together (a pair's
 * hedge ratio, a ranking across a universe) declares instead
 * `template <class Ctx> void on_bars(Ctx& ctx, const BarSlice& slice);`, called once per period end and
 * interval with every instrument's bar for it, after all of them are cached. A strategy declares one of
 * the two; declaring both does not compile, so neither runs in silence beside the other.
 *
 * Because the handlers are templates, the same strategy runs under every runner: a dry run, a
 * live session with any broker, and a backtest. Take the context by reference; a context cannot
 * be copied, so a handler that takes it by value does not compile.
 *
 * @warning `on_fill` is called in a backtest but never by the shipped live loop; see `Fill`.
 */

namespace strategy_detail {

/**
 * Detects that `S` declares a member named NAME: of any kind, signature or access, inherited included.
 *
 * Handlers are optional, so something has to decide whether to call one, and this is it: a declared name
 * is called, and a call that does not compile is the compiler's error at the call (see `dispatch`). The
 * name is what the author wrote; asking instead whether a particular call compiles would read a handler
 * with a wrong parameter as absent and skip it in silence. Looking NAME up in a class that derives from
 * `S` and from one declaring `int NAME` is ambiguous exactly when `S` declares it, which works for a
 * member template, where `&S::NAME` cannot name one.
 *
 * **A `final` class cannot be derived from**, and a non-class type has no members, so neither can be
 * looked into; `dispatch` and `StrategyConformance` refuse both rather than read them as declaring nothing.
 */
#define ATL_DETECT_DECLARED(NAME)                                                        \
  struct NAME##_fallback {                                                               \
    int NAME;                                                                            \
  };                                                                                     \
  template <typename S>                                                                  \
  struct NAME##_probe : S, NAME##_fallback {};                                           \
  template <typename S, typename = void>                                                 \
  struct NAME##_is_ambiguous : std::true_type {};                                        \
  template <typename S>                                                                  \
  struct NAME##_is_ambiguous<S, std::void_t<decltype(&NAME##_probe<S>::NAME)>>           \
  : std::false_type {};                                                                  \
  template <typename S>                                                                  \
  constexpr bool declares_##NAME = std::conjunction_v<                                   \
    std::is_class<S>, std::negation<std::is_final<S>>, NAME##_is_ambiguous<S>            \
  >

ATL_DETECT_DECLARED(on_start);
ATL_DETECT_DECLARED(on_stop);
ATL_DETECT_DECLARED(on_bar);
ATL_DETECT_DECLARED(on_bars);
ATL_DETECT_DECLARED(on_quote);
ATL_DETECT_DECLARED(on_fill);
ATL_DETECT_DECLARED(on_order_update);
ATL_DETECT_DECLARED(on_phase);

#undef ATL_DETECT_DECLARED

/** Whether `S` declares any handler at all. */
template <typename S>
constexpr bool declares_any_handler =
  declares_on_start<S> || declares_on_stop<S> || declares_on_bar<S> || declares_on_bars<S> ||
  declares_on_quote<S> || declares_on_fill<S> || declares_on_order_update<S> || declares_on_phase<S>;

/** Whether `S`'s handler names can be looked up: a class, and not `final`. */
template <typename S>
constexpr bool names_can_be_read = std::is_class_v<S> && !std::is_final_v<S>;

/** Refuses a strategy type whose handler names cannot be looked up, where it is dispatched. */
template <typename S>
void require_readable_names() {
  static_assert(
    names_can_be_read<S>,
    "a final strategy cannot be checked for a handler the runner would skip, because its member names "
    "cannot be looked up; remove `final` from the strategy class"
  );
}

/**
 * Refuses a strategy that declares both bar handlers. A runner delivers bars one way or the other, so the
 * second handler would never run, or both would and every decision would be taken twice.
 */
template <typename S>
void require_one_bar_handler() {
  static_assert(
    !(declares_on_bar<S> && declares_on_bars<S>),
    "a strategy declares on_bar or on_bars, not both: on_bar receives each bar, on_bars every "
    "instrument's bar of a period at once; keep the one this strategy decides with"
  );
}

} // namespace strategy_detail

/**
 * @brief Calls a strategy's handlers, skipping the ones it does not declare.
 *
 * A handler the strategy declares is always called; if its signature does not fit the call (a
 * non-const event, a missing event, a context taken by value), compilation fails here rather than
 * the handler being skipped. A wrapper strategy that forwards to an inner strategy through these
 * functions gets the same check for the inner one.
 */
namespace dispatch {

/**
 * @brief Calls `strategy.on_start(context)` if the strategy declares `on_start`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 */
template <typename S, typename Ctx>
void start(S& strategy, Ctx& context) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_start<S>) {
    strategy.on_start(context);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
  }
}

/**
 * @brief Calls `strategy.on_stop(context)` if the strategy declares `on_stop`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 */
template <typename S, typename Ctx>
void stop(S& strategy, Ctx& context) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_stop<S>) {
    strategy.on_stop(context);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
  }
}

/**
 * @brief Calls `strategy.on_bar(context, event)` if the strategy declares `on_bar`; otherwise does nothing.
 *
 * Does not compile for a strategy that declares `on_bars`: its bars go through `bars`, so a path that
 * still delivers one `BarEvent` at a time fails to build instead of skipping the strategy in silence.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param event The event passed to the handler.
 */
template <typename S, typename Ctx>
void bar(S& strategy, Ctx& context, const BarEvent& event) {
  strategy_detail::require_readable_names<S>();
  strategy_detail::require_one_bar_handler<S>();
  static_assert(
    strategy_detail::declares_on_bar<S> || !strategy_detail::declares_on_bars<S>,
    "this strategy declares on_bars, so its bars are delivered in slices: feed it through "
    "Runner::record and Runner::feed(const BarSlice&), not one BarEvent at a time"
  );
  if constexpr (strategy_detail::declares_on_bar<S>) {
    strategy.on_bar(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

/**
 * @brief Calls `strategy.on_bars(context, slice)` if the strategy declares `on_bars`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param slice The slice passed to the handler.
 */
template <typename S, typename Ctx>
void bars(S& strategy, Ctx& context, const BarSlice& slice) {
  strategy_detail::require_readable_names<S>();
  strategy_detail::require_one_bar_handler<S>();
  if constexpr (strategy_detail::declares_on_bars<S>) {
    strategy.on_bars(context, slice);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(slice);
  }
}

/**
 * @brief Calls `strategy.on_quote(context, event)` if the strategy declares `on_quote`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param event The event passed to the handler.
 */
template <typename S, typename Ctx>
void quote(S& strategy, Ctx& context, const QuoteEvent& event) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_quote<S>) {
    strategy.on_quote(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

/**
 * @brief Calls `strategy.on_fill(context, event)` if the strategy declares `on_fill`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param event The event passed to the handler.
 */
template <typename S, typename Ctx>
void fill(S& strategy, Ctx& context, const Fill& event) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_fill<S>) {
    strategy.on_fill(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

/**
 * @brief Calls `strategy.on_order_update(context, event)` if the strategy declares `on_order_update`; otherwise does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param event The event passed to the handler.
 */
template <typename S, typename Ctx>
void order_update(S& strategy, Ctx& context, const OrderSnapshot& event) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_order_update<S>) {
    strategy.on_order_update(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

/**
 * @brief Calls `strategy.on_phase(context, event)` if the strategy declares `on_phase`; otherwise
 *   does nothing.
 *
 * @tparam S The strategy type; a class that is not `final`.
 * @tparam Ctx The context type.
 * @param strategy The strategy.
 * @param context The context passed to the handler.
 * @param event The new phase, passed as a `const` reference.
 */
template <typename S, typename Ctx>
void phase(S& strategy, Ctx& context, const MarketPhase& event) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_phase<S>) {
    strategy.on_phase(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

} // namespace dispatch

namespace strategy_detail {

/** Makes every call a runner makes to `S` with context `Ctx`. Compiled by `StrategyConformance`, never run. */
template <typename S, typename Ctx>
void call_every_handler(
  S& strategy, Ctx& context, const BarEvent& bar, const BarSlice& slice, const QuoteEvent& quote,
  const Fill& fill, const OrderSnapshot& order, const MarketPhase& phase
) {
  dispatch::start(strategy, context);
  dispatch::stop(strategy, context);
  // The bar route the runner takes: slices for a strategy declaring `on_bars`, single bars otherwise.
  if constexpr (declares_on_bars<S>) {
    static_cast<void>(bar);
    dispatch::bars(strategy, context, slice);
  } else {
    static_cast<void>(slice);
    dispatch::bar(strategy, context, bar);
  }
  dispatch::quote(strategy, context, quote);
  dispatch::fill(strategy, context, fill);
  dispatch::order_update(strategy, context, order);
  dispatch::phase(strategy, context, phase);
}

/**
 * Compiles every handler call a runner would make, by taking the address of `call_every_handler`, so a
 * malformed handler fails here even when the program never reaches it. Always true.
 */
template <typename S, typename Ctx>
constexpr bool compiles_every_handler_call() {
  if constexpr (names_can_be_read<S>) {
    static_cast<void>(&call_every_handler<S, Ctx>);
  }
  return true;
}

} // namespace strategy_detail

/**
 * @brief A capability set in which the broker implements nothing optional.
 *
 * Inherit it in a test's capability set and override only what differs.
 */
struct NoCaps {
  /** @copydoc BrokerCaps::has_account_discovery */
  static constexpr bool has_account_discovery = false;
  /** @copydoc BrokerCaps::has_client_order_id */
  static constexpr bool has_client_order_id = false;
  /** @copydoc BrokerCaps::has_trigger_price */
  static constexpr bool has_trigger_price = false;
  /** @copydoc BrokerCaps::data_only */
  static constexpr bool data_only = false;
  /** @copydoc BrokerCaps::has_working_orders */
  static constexpr bool has_working_orders = false;
  /** @copydoc BrokerCaps::working_orders_state_terminal */
  static constexpr bool working_orders_state_terminal = false;
  /** @copydoc BrokerCaps::has_amendable_quantity */
  static constexpr bool has_amendable_quantity = false;
  /** @copydoc BrokerCaps::has_order_by_id */
  static constexpr bool has_order_by_id = false;
  /** @copydoc BrokerCaps::has_partial_cancel */
  static constexpr bool has_partial_cancel = false;
  /** @copydoc BrokerCaps::history_requires_range */
  static constexpr bool history_requires_range = false;
  /** @copydoc BrokerCaps::has_history_cursor */
  static constexpr bool has_history_cursor = false;
  /** @copydoc BrokerCaps::has_history_limit */
  static constexpr bool has_history_limit = false;
  /** @copydoc BrokerCaps::has_history_symbol_filter */
  static constexpr bool has_history_symbol_filter = false;
  /** @copydoc BrokerCaps::has_depth */
  static constexpr bool has_depth = false;
  /** @copydoc BrokerCaps::has_price_limits */
  static constexpr bool has_price_limits = false;
  /** @copydoc BrokerCaps::has_trade_prints */
  static constexpr bool has_trade_prints = false;
  /** @copydoc BrokerCaps::has_batch_prices */
  static constexpr bool has_batch_prices = false;
  /** @copydoc BrokerCaps::has_reference_data */
  static constexpr bool has_reference_data = false;
  /** @copydoc BrokerCaps::has_batch_reference_data */
  static constexpr bool has_batch_reference_data = false;
  /** @copydoc BrokerCaps::has_calendar */
  static constexpr bool has_calendar = false;
  /** @copydoc BrokerCaps::has_calendar_range */
  static constexpr bool has_calendar_range = false;
  /** @copydoc BrokerCaps::has_calendar_sessions */
  static constexpr bool has_calendar_sessions = false;
  /** @copydoc BrokerCaps::has_buying_power */
  static constexpr bool has_buying_power = false;
  /** @copydoc BrokerCaps::has_buying_power_quantity */
  static constexpr bool has_buying_power_quantity = false;
  /** @copydoc BrokerCaps::has_commission_schedule */
  static constexpr bool has_commission_schedule = false;
  /** @brief No sellable-quantity figure is available. */
  static constexpr SellableSource sellable_source = SellableSource::NONE;
  /** @brief Unestablished, since there is no inquiry. */
  static constexpr SellableBasis sellable_inquiry_basis{};
  /**
   * @brief Tests whether the broker serves bars of an interval (the unnamed argument).
   * @return `false` for every interval.
   */
  static constexpr bool supports_interval(BarInterval) { return false; }
};

/** @brief A capability set in which the broker implements everything optional. */
struct AllCaps {
  /** @copydoc BrokerCaps::has_account_discovery */
  static constexpr bool has_account_discovery = true;
  /** @copydoc BrokerCaps::has_client_order_id */
  static constexpr bool has_client_order_id = true;
  /** @copydoc BrokerCaps::has_trigger_price */
  static constexpr bool has_trigger_price = true;
  /** @brief `false`: this broker takes orders. */
  static constexpr bool data_only = false;
  /** @copydoc BrokerCaps::has_working_orders */
  static constexpr bool has_working_orders = true;
  /** @copydoc BrokerCaps::working_orders_state_terminal */
  static constexpr bool working_orders_state_terminal = true;
  /** @copydoc BrokerCaps::has_amendable_quantity */
  static constexpr bool has_amendable_quantity = true;
  /** @copydoc BrokerCaps::has_order_by_id */
  static constexpr bool has_order_by_id = true;
  /** @copydoc BrokerCaps::has_partial_cancel */
  static constexpr bool has_partial_cancel = true;
  /** @copydoc BrokerCaps::history_requires_range */
  static constexpr bool history_requires_range = true;
  /** @copydoc BrokerCaps::has_history_cursor */
  static constexpr bool has_history_cursor = true;
  /** @copydoc BrokerCaps::has_history_limit */
  static constexpr bool has_history_limit = true;
  /** @copydoc BrokerCaps::has_history_symbol_filter */
  static constexpr bool has_history_symbol_filter = true;
  /** @copydoc BrokerCaps::has_depth */
  static constexpr bool has_depth = true;
  /** @copydoc BrokerCaps::has_price_limits */
  static constexpr bool has_price_limits = true;
  /** @copydoc BrokerCaps::has_trade_prints */
  static constexpr bool has_trade_prints = true;
  /** @copydoc BrokerCaps::has_batch_prices */
  static constexpr bool has_batch_prices = true;
  /** @copydoc BrokerCaps::has_reference_data */
  static constexpr bool has_reference_data = true;
  /** @copydoc BrokerCaps::has_batch_reference_data */
  static constexpr bool has_batch_reference_data = true;
  /** @copydoc BrokerCaps::has_calendar */
  static constexpr bool has_calendar = true;
  /** @copydoc BrokerCaps::has_calendar_range */
  static constexpr bool has_calendar_range = true;
  /** @copydoc BrokerCaps::has_calendar_sessions */
  static constexpr bool has_calendar_sessions = true;
  /** @copydoc BrokerCaps::has_buying_power */
  static constexpr bool has_buying_power = true;
  /** @copydoc BrokerCaps::has_buying_power_quantity */
  static constexpr bool has_buying_power_quantity = true;
  /** @copydoc BrokerCaps::has_commission_schedule */
  static constexpr bool has_commission_schedule = true;
  /** @brief The sellable quantity comes from an inquiry. */
  static constexpr SellableSource sellable_source = SellableSource::INQUIRY;
  /** @brief The whole holding, with resting sells deducted (the basis the library assumes for Toss, unverified). */
  static constexpr SellableBasis sellable_inquiry_basis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  /**
   * @brief Tests whether the broker serves bars of an interval (the unnamed argument).
   * @return `true` for every interval.
   */
  static constexpr bool supports_interval(BarInterval) { return true; }
};

/** @brief The broker tag of a probe context; nothing it issues reaches a market. */
struct ProbeBroker {
  /** @brief The broker name an order reference prints. */
  static constexpr std::string_view name = "probe";
};

/**
 * @brief A context for testing a strategy without a runner: it records what the strategy asks for.
 *
 * Set the public fields (`now_`, `phase_`, `calendar_`, `portfolio_`, `instruments_`), call a
 * handler, then read `submitted()`, `targets()`, `canceled()` and `log().lines()`.
 * `ATL_ASSERT_STRATEGY(S, MinimalContext)` compiles a strategy's handlers against it.
 *
 * It has no series accessors (`bars`, `frame`, `panel`), so it needs no linear algebra backend;
 * a strategy that reads series is checked by the runner it is bound to.
 *
 * @tparam CapsType The broker capabilities the strategy sees as `Ctx::caps`; see `NoCaps` and
 *   `AllCaps`.
 */
template <typename CapsType>
class ProbeContextT {
public:
  /** @brief The broker tag; see `ProbeBroker`. */
  using Broker = ProbeBroker;
  /** @brief The broker capabilities the strategy sees. */
  using caps = CapsType;

  /** @brief Constructs an empty probe. */
  ProbeContextT() = default;
  /** @brief Deleted: like a runner's context, a probe cannot be copied. */
  ProbeContextT(const ProbeContextT&) = delete;
  /**
   * @brief Deleted: a probe cannot be copied.
   * @return Not callable.
   */
  ProbeContextT& operator=(const ProbeContextT&) = delete;
  /** @brief Move constructor: takes the other probe's state. */
  ProbeContextT(ProbeContextT&&) = default;
  /**
   * @brief Move assignment: takes the other probe's state.
   * @return `*this`.
   */
  ProbeContextT& operator=(ProbeContextT&&) = default;

  /** @brief A log that keeps each line in memory for a test to read. */
  class Log {
  public:
    /**
     * @brief Records one line.
     * @param message The line.
     */
    void note(std::string_view message) { lines_.emplace_back(message); }
    /**
     * @brief Returns the recorded lines.
     * @return The lines, in order.
     */
    const std::vector<std::string>& lines() const noexcept { return lines_; }

  private:
    std::vector<std::string> lines_;
  };

  /**
   * @brief Returns the current instant.
   * @return `now_`.
   */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Returns the market phase.
   * @return `phase_`.
   */
  MarketPhase phase() const noexcept { return phase_; }

  /**
   * @brief Returns the trading calendar.
   * @return `calendar_`.
   */
  const TradingCalendar& calendar() const noexcept { return calendar_; }

  /**
   * @brief Returns the portfolio.
   * @return `portfolio_`.
   */
  const Portfolio<ProbeBroker>& portfolio() const noexcept { return portfolio_; }

  /**
   * @brief Looks up an instrument's reference data in `instruments_`.
   *
   * @param id The instrument.
   * @return Its reference data, or `std::nullopt` if the test provided none.
   */
  std::optional<Instrument> instrument(const InstrumentId& id) const {
    for (const auto& known : instruments_) {
      if (known.id_ == id) {
        return known;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Records an order intent.
   * @param intent The intent, appended to `submitted()`.
   */
  void submit(OrderIntent intent) { submitted_.push_back(std::move(intent)); }

  /**
   * @brief Records a target position.
   * @param target The target, appended to `targets()`.
   */
  void submit(TargetPosition target) { targets_.push_back(std::move(target)); }

  /**
   * @brief Records a cancellation.
   * @param ref The order to cancel, appended to `canceled()`.
   */
  void cancel(const OrderRef<ProbeBroker>& ref) {
    canceled_.push_back(CancelIntent<ProbeBroker>{ref, {}});
  }

  /**
   * @brief Returns the log.
   * @return The recording log.
   */
  Log& log() noexcept { return log_; }

  /**
   * @brief Returns the order intents submitted.
   * @return The intents, in order.
   */
  const std::vector<OrderIntent>& submitted() const noexcept { return submitted_; }
  /**
   * @brief Returns the target positions submitted.
   * @return The targets, in order.
   */
  const std::vector<TargetPosition>& targets() const noexcept { return targets_; }
  /**
   * @brief Returns the cancellations requested.
   * @return The cancellations, in order.
   */
  const std::vector<CancelIntent<ProbeBroker>>& canceled() const noexcept { return canceled_; }

  /** @brief The instant `now()` returns. */
  std::chrono::system_clock::time_point now_{};
  /** @brief The phase `phase()` returns. */
  MarketPhase phase_{MarketPhase::OPEN};
  /** @brief The calendar `calendar()` returns. */
  TradingCalendar calendar_;
  /** @brief The portfolio `portfolio()` returns. */
  Portfolio<ProbeBroker> portfolio_;
  /** @brief The reference data `instrument()` searches. */
  std::vector<Instrument> instruments_;

private:
  std::vector<OrderIntent> submitted_;
  std::vector<TargetPosition> targets_;
  std::vector<CancelIntent<ProbeBroker>> canceled_;
  Log log_;
};

/** @brief A probe context whose broker implements nothing optional. */
using MinimalContext = ProbeContextT<NoCaps>;

/** @brief A probe context whose broker implements everything optional. */
using FullContext = ProbeContextT<AllCaps>;

/**
 * @brief The checks behind `ATL_ASSERT_STRATEGY`; instantiating it reports what is wrong with `S`.
 *
 * @tparam S The strategy type.
 * @tparam Ctx The context type the runner passes.
 */
template <typename S, typename Ctx>
struct StrategyConformance {
  // A strategy's handler names are looked up through a class derived from it, which a `final` class does
  // not allow; read any other way, a handler the runner cannot call could be skipped in silence.
  static_assert(
    strategy_detail::names_can_be_read<S>,
    "a final strategy cannot be checked for a handler the runner would skip, because its member names "
    "cannot be looked up; remove `final` from the strategy class"
  );
  // A handler taking its context by value would submit to a copy, and what it submitted would be lost.
  // Every context this library hands a strategy is move-only; a caller's own must be too -- from the
  // mutable lvalue `dispatch` passes as well as from a `const` one, since a copy constructor may take either.
  static_assert(
    !std::is_copy_constructible_v<Ctx> && !std::is_constructible_v<Ctx, Ctx&>,
    "a strategy context must not be copyable, so that a handler taking it by value does not compile "
    "(what it submitted would go to the copy and be lost); delete the context's copy constructor"
  );
  static_assert(
    strategy_detail::declares_any_handler<S>,
    "This type declares no strategy handler. Expected at least one of on_start, on_bar, on_bars, "
    "on_quote, on_fill, on_order_update, on_phase, or on_stop"
  );
  static_assert(
    !(strategy_detail::declares_on_bar<S> && strategy_detail::declares_on_bars<S>),
    "a strategy declares on_bar or on_bars, not both: on_bar receives each bar, on_bars every "
    "instrument's bar of a period at once; keep the one this strategy decides with"
  );

  /** @brief Always `true`; reading it compiles every handler call the runner makes with `Ctx`. */
  static constexpr bool value = strategy_detail::compiles_every_handler_call<S, Ctx>();
};

/**
 * @brief Asserts at compile time that `S` is a strategy a runner with context `Ctx` can drive.
 *
 * Checks that:
 * - `S` is a class and not `final`;
 * - `Ctx` cannot be copied, so a handler taking it by value does not compile;
 * - `S` declares at least one handler, and not both `on_bar` and `on_bars`;
 * - every handler `S` declares can be called the way a runner calls it with `Ctx`. Each handler's
 *   body is compiled for `Ctx`, including handlers for events the program never feeds.
 *
 * With `MinimalContext` or `FullContext` it checks handlers against a probe without series
 * accessors; a strategy that reads `bars()` is checked by its runner instead. Usable at
 * namespace, class and block scope.
 *
 * @param S The strategy type.
 * @param Ctx The context type.
 * @note Not detected: a handler that takes by value a copyable base class of the context.
 */
#define ATL_ASSERT_STRATEGY(S, Ctx)                                                      \
  static_assert(                                                                         \
    ::algotradelib::strategy::StrategyConformance<S, Ctx>::value,                         \
    "Strategy does not satisfy the algotradelib strategy surface; see the messages above" \
  )

} // namespace strategy
} // namespace algotradelib
