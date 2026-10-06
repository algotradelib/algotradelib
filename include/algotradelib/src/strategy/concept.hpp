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
 * What makes something a strategy, and how that is checked.
 *
 * A strategy is a plain struct with **template** member functions. Every handler is optional, so
 * a strategy that only reacts to bars writes only `on_bar` and costs nothing for the rest:
 *
 * ```cpp
 * struct MyStrategy {
 *   struct Params { std::size_t window_{20}; };
 *   Params params_;
 *
 *   template <class Ctx> void on_bar(Ctx& ctx, const BarEvent& bar) { ... }
 * };
 * ```
 *
 * The handlers are templates rather than concrete because that is what makes one source run under
 * every runner — dry-run, live against either broker, and a backtest. A handler written against one
 * concrete context type compiles for that runner and fails to compile for the others, where each is
 * built.
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
ATL_DETECT_DECLARED(on_quote);
ATL_DETECT_DECLARED(on_fill);
ATL_DETECT_DECLARED(on_order_update);
ATL_DETECT_DECLARED(on_phase);

#undef ATL_DETECT_DECLARED

/** Whether `S` declares any handler at all. */
template <typename S>
constexpr bool declares_any_handler =
  declares_on_start<S> || declares_on_stop<S> || declares_on_bar<S> || declares_on_quote<S> ||
  declares_on_fill<S> || declares_on_order_update<S> || declares_on_phase<S>;

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

} // namespace strategy_detail

/**
 * Calls a strategy's handler when it declares one, and costs nothing when it does not.
 *
 * Every handler is optional, so a runner cannot simply call them: a strategy that reacts only to bars
 * must not be required to write five empty functions. Each function below is the `if constexpr` that
 * resolves that, in one place: **a handler whose name the strategy declares is called**, and one it does
 * not declare compiles to nothing. A declared handler the call does not fit -- a non-const event, a wrong
 * or missing event, a context taken by value -- is therefore the compiler's error at the call, naming the
 * candidate, not a handler skipped in silence. A wrapper that forwards to an inner strategy through these
 * functions gets the same check on the inner one.
 *
 * These take the strategy by reference and mutate it, because a strategy holds its own state.
 * `runner::dispatch` names this namespace.
 */
namespace dispatch {

/** Calls `on_start` if `strategy` declares one. */
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

/** Calls `on_stop` if `strategy` declares one. */
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

/** Calls `on_bar` if `strategy` declares one. */
template <typename S, typename Ctx>
void bar(S& strategy, Ctx& context, const BarEvent& event) {
  strategy_detail::require_readable_names<S>();
  if constexpr (strategy_detail::declares_on_bar<S>) {
    strategy.on_bar(context, event);
  } else {
    static_cast<void>(strategy);
    static_cast<void>(context);
    static_cast<void>(event);
  }
}

/** Calls `on_quote` if `strategy` declares one. */
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

/** Calls `on_fill` if `strategy` declares one. */
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

/** Calls `on_order_update` if `strategy` declares one. */
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

/** Calls `on_phase` if `strategy` declares one, with the phase as a `const` reference. */
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
  S& strategy, Ctx& context, const BarEvent& bar, const QuoteEvent& quote, const Fill& fill,
  const OrderSnapshot& order, const MarketPhase& phase
) {
  dispatch::start(strategy, context);
  dispatch::stop(strategy, context);
  dispatch::bar(strategy, context, bar);
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
 * Capability set in which the broker implements nothing optional.
 *
 * Answers every `BrokerCaps` question, so a test's hand-written capability set can inherit it and state
 * only what differs.
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
  /**
   * Nowhere, which is the honest answer for a broker this type stands in for.
   *
   * `NONE` rather than `HOLDINGS`: a stand-in that claimed the figure arrives with the positions
   * would have a runner skip the inquiry and then find nothing there.
   */
  static constexpr SellableSource sellable_source = SellableSource::NONE;
  /** Unestablished; a capability set that makes the inquiry states its own. */
  static constexpr SellableBasis sellable_inquiry_basis{};
  /** @copydoc BrokerCaps::supports_interval(BarInterval) */
  static constexpr bool supports_interval(BarInterval) { return false; }
};

/** Capability set in which the broker implements everything optional. */
struct AllCaps {
  /** @copydoc BrokerCaps::has_account_discovery */
  static constexpr bool has_account_discovery = true;
  /** @copydoc BrokerCaps::has_client_order_id */
  static constexpr bool has_client_order_id = true;
  /** @copydoc BrokerCaps::has_trigger_price */
  static constexpr bool has_trigger_price = true;
  /** A broker that takes orders; a market-data source is not "everything". */
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
  /** By inquiry, the source that has a verb behind it. */
  static constexpr SellableSource sellable_source = SellableSource::INQUIRY;
  /** The basis Toss states: the whole holding, resting sells deducted, not measured. */
  static constexpr SellableBasis sellable_inquiry_basis{
    SellableBasis::Coverage::WHOLE_HOLDING,
    SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  /** @copydoc BrokerCaps::supports_interval(BarInterval) */
  static constexpr bool supports_interval(BarInterval) { return true; }
};

/** Compile-time tag standing in for a broker in a probe context. */
struct ProbeBroker {
  /** The tag's normalized name, which is what an order reference prints. */
  static constexpr std::string_view name = "probe";
};

/**
 * A complete context that does nothing, for checking a strategy compiles.
 *
 * A stub for tests: intents land in `submitted_` where a test can read them, and
 * `ATL_ASSERT_STRATEGY(S, MinimalContext)` compiles a strategy's handlers against it.
 *
 * Parameterized on the capability set: `MinimalContext` has every capability absent and
 * `FullContext` every one present, so a test that runs a strategy's handlers on both compiles the
 * `if constexpr` branch it takes for a broker that offers everything *and* the one it takes for a
 * broker that offers nothing.
 *
 * It deliberately does not provide the series accessors a real runner has — `bars`, `frame`,
 * `panel`. Those return analytics types and therefore need a linear-algebra backend, which this
 * layer does not require: a strategy trading on prices and positions alone should not have to
 * acquire one. A strategy that reads series is checked when its runner instantiates it.
 */
template <typename CapsType>
class ProbeContextT {
public:
  /** The stand-in broker tag; nothing a probe issues reaches a market. */
  using Broker = ProbeBroker;
  /** The capability set being probed against. */
  using caps = CapsType;

  /** Constructs an empty probe. */
  ProbeContextT() = default;
  /** Not copyable, like the runner's context, so a probe rejects a by-value handler as a runner would. */
  ProbeContextT(const ProbeContextT&) = delete;
  /** Not copyable; see the copy constructor. */
  ProbeContextT& operator=(const ProbeContextT&) = delete;
  /** Movable. */
  ProbeContextT(ProbeContextT&&) = default;
  /** Movable. */
  ProbeContextT& operator=(ProbeContextT&&) = default;

  /** A log that records rather than writes, so a test can assert on what a strategy said. */
  class Log {
  public:
    /** Records one line. */
    void note(std::string_view message) { lines_.emplace_back(message); }
    /** Returns everything recorded, in order. */
    const std::vector<std::string>& lines() const noexcept { return lines_; }

  private:
    std::vector<std::string> lines_;
  };

  /** Returns the current instant as the test set it. */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /** Returns the market phase as the test set it. */
  MarketPhase phase() const noexcept { return phase_; }

  /** Returns the trading calendar as the test set it. */
  const TradingCalendar& calendar() const noexcept { return calendar_; }

  /** Returns the portfolio as the test set it. */
  const Portfolio<ProbeBroker>& portfolio() const noexcept { return portfolio_; }

  /**
   * Returns reference data for `id`, or nothing when the test provided none.
   *
   * Nothing rather than a default, because the `PriceRule` on it is what a strategy needs to
   * build a limit price, and a defaulted rule would be an `UNKNOWN` grid that silently refused
   * every price. Absent says "ask the runner to load it"; a default would say "there is no grid".
   */
  std::optional<Instrument> instrument(const InstrumentId& id) const {
    for (const auto& known : instruments_) {
      if (known.id_ == id) {
        return known;
      }
    }
    return std::nullopt;
  }

  /** Records an order the strategy asked for. */
  void submit(OrderIntent intent) { submitted_.push_back(std::move(intent)); }

  /** Records a target position the strategy asked for. */
  void submit(TargetPosition target) { targets_.push_back(std::move(target)); }

  /** Records a cancellation the strategy asked for. */
  void cancel(const OrderRef<ProbeBroker>& ref) {
    canceled_.push_back(CancelIntent<ProbeBroker>{ref, {}});
  }

  /** Returns the recording log. */
  Log& log() noexcept { return log_; }

  /** What the strategy asked for, in order. */
  const std::vector<OrderIntent>& submitted() const noexcept { return submitted_; }
  /** Returns the targets the strategy stated, in order. */
  const std::vector<TargetPosition>& targets() const noexcept { return targets_; }
  /** Returns the cancellations the strategy asked for, in order. */
  const std::vector<CancelIntent<ProbeBroker>>& canceled() const noexcept { return canceled_; }

  /** Mutable state a test arranges before invoking a handler. */
  std::chrono::system_clock::time_point now_{};
  /** The phase a handler will see. */
  MarketPhase phase_{MarketPhase::OPEN};
  /** The calendar a handler will see. */
  TradingCalendar calendar_;
  /** The portfolio a handler will see. */
  Portfolio<ProbeBroker> portfolio_;
  /** Reference data a handler can look an instrument up in. */
  std::vector<Instrument> instruments_;

private:
  std::vector<OrderIntent> submitted_;
  std::vector<TargetPosition> targets_;
  std::vector<CancelIntent<ProbeBroker>> canceled_;
  Log log_;
};

/** A probe context in which the broker implements nothing optional. */
using MinimalContext = ProbeContextT<NoCaps>;

/** A probe context in which the broker implements everything optional. */
using FullContext = ProbeContextT<AllCaps>;

/**
 * Reports what is wrong with a strategy, by name.
 *
 * Instantiating this type produces the diagnostic: a C++17 `static_assert` message must be a string
 * literal, so the strategy type has to arrive through the instantiation context rather than the
 * message.
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
  // Every context this library hands a strategy is move-only; a caller's own must be too.
  static_assert(
    !std::is_copy_constructible_v<Ctx>,
    "a strategy context must not be copyable, so that a handler taking it by value does not compile "
    "(what it submitted would go to the copy and be lost); delete the context's copy constructor"
  );
  static_assert(
    strategy_detail::declares_any_handler<S>,
    "This type declares no strategy handler. Expected at least one of on_start, on_bar, on_quote, "
    "on_fill, on_order_update, on_phase, or on_stop"
  );

  /**
   * Always true. Reading it compiles every handler call the runner makes with `Ctx`: a declared handler
   * the call does not fit fails here, in the compiler's words, at `dispatch`.
   */
  static constexpr bool value = strategy_detail::compiles_every_handler_call<S, Ctx>();
};

/**
 * Asserts that `S` is a strategy the runner for `Ctx` can drive.
 *
 * - `S` is a class and not `final` (its handler names could not be looked up otherwise);
 * - `Ctx` cannot be copied, so a handler taking it by value does not compile (its submissions would go to
 *   a copy); the runner's contexts and the probe contexts are move-only;
 * - it declares at least one handler;
 * - **every handler it declares can be called as the runner calls it with `Ctx`**: the assertion compiles
 *   each call, and each handler template's body, for `Ctx`. A handler the call does not fit is the
 *   compiler's error at `dispatch`, naming the candidate: an `on_fill(Ctx&, Fill&)` with a non-const event,
 *   an `on_phase(Ctx&)` with no event, a context taken by value, a private member or a non-callable one of
 *   a handler's name, or a handler disabled with `enable_if` for this context. **Every declared handler's
 *   body is compiled for `Ctx` here**, including one for an event the program never feeds: a runner
 *   asserts this when it is instantiated.
 *
 * Asserted against `MinimalContext` or `FullContext`, it compiles the handlers against that stub, which
 * offers no series accessors: a strategy that reads `bars()` is checked by its runner instead. A strategy
 * is checked with the context each runner it is bound to passes; nothing here claims it runs on every one.
 *
 * Usable at namespace, class, and block scope.
 */
#define ATL_ASSERT_STRATEGY(S, Ctx)                                                      \
  static_assert(                                                                         \
    ::algotradelib::strategy::StrategyConformance<S, Ctx>::value,                         \
    "Strategy does not satisfy the algotradelib strategy surface; see the messages above" \
  )

} // namespace strategy
} // namespace algotradelib
