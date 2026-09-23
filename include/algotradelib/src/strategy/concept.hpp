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
 * every runner — dry-run, live against either broker, and later a backtest. A handler written
 * against one concrete context type compiles for that runner and silently excludes the others,
 * which is the failure `ATL_ASSERT_STRATEGY` is built to catch.
 */

namespace strategy_detail {

#define ATL_DETECT_HANDLER_1(NAME, METHOD)                                               \
  template <typename S, typename Ctx, typename = void>                                   \
  struct NAME : std::false_type {};                                                      \
  template <typename S, typename Ctx>                                                    \
  struct NAME<                                                                           \
    S,                                                                                   \
    Ctx,                                                                                 \
    std::void_t<decltype(std::declval<S&>().METHOD(std::declval<Ctx&>()))>                \
  > : std::true_type {}

#define ATL_DETECT_HANDLER_2(NAME, METHOD, EVENT)                                        \
  template <typename S, typename Ctx, typename = void>                                   \
  struct NAME : std::false_type {};                                                      \
  template <typename S, typename Ctx>                                                    \
  struct NAME<                                                                           \
    S,                                                                                   \
    Ctx,                                                                                 \
    std::void_t<                                                                         \
      decltype(                                                                          \
        std::declval<S&>().METHOD(std::declval<Ctx&>(), std::declval<const EVENT&>())     \
      )                                                                                  \
    >                                                                                    \
  > : std::true_type {}

ATL_DETECT_HANDLER_1(has_on_start, on_start);
ATL_DETECT_HANDLER_1(has_on_stop, on_stop);
ATL_DETECT_HANDLER_2(has_on_bar, on_bar, BarEvent);
ATL_DETECT_HANDLER_2(has_on_quote, on_quote, QuoteEvent);
ATL_DETECT_HANDLER_2(has_on_fill, on_fill, Fill);
ATL_DETECT_HANDLER_2(has_on_order_update, on_order_update, OrderSnapshot);
ATL_DETECT_HANDLER_2(has_on_phase, on_phase, MarketPhase);

#undef ATL_DETECT_HANDLER_1
#undef ATL_DETECT_HANDLER_2

/** Whether `S` defines any handler at all for `Ctx`. */
template <typename S, typename Ctx>
constexpr bool has_any_handler =
  has_on_start<S, Ctx>::value || has_on_stop<S, Ctx>::value || has_on_bar<S, Ctx>::value ||
  has_on_quote<S, Ctx>::value || has_on_fill<S, Ctx>::value ||
  has_on_order_update<S, Ctx>::value || has_on_phase<S, Ctx>::value;

/**
 * Whether every handler `S` defines for `Ctx` it also defines for `Other`.
 *
 * This is the genericity test. A handler taking a concrete context type is detected for that
 * context and not for any other, so comparing two unrelated contexts catches it. A handler
 * templated on its context is detected for both.
 */
template <typename S, typename Ctx, typename Other>
constexpr bool handlers_are_generic =
  (!has_on_start<S, Ctx>::value || has_on_start<S, Other>::value) &&
  (!has_on_stop<S, Ctx>::value || has_on_stop<S, Other>::value) &&
  (!has_on_bar<S, Ctx>::value || has_on_bar<S, Other>::value) &&
  (!has_on_quote<S, Ctx>::value || has_on_quote<S, Other>::value) &&
  (!has_on_fill<S, Ctx>::value || has_on_fill<S, Other>::value) &&
  (!has_on_order_update<S, Ctx>::value || has_on_order_update<S, Other>::value) &&
  (!has_on_phase<S, Ctx>::value || has_on_phase<S, Other>::value);

} // namespace strategy_detail

/** Capability set in which the broker implements nothing optional. */
struct NoCaps {
  static constexpr bool has_account_discovery = false;
  static constexpr bool has_client_order_id = false;
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
  static constexpr bool has_reference_data = false;
  static constexpr bool has_batch_reference_data = false;
  static constexpr bool has_calendar = false;
  static constexpr bool has_calendar_range = false;
  static constexpr bool has_calendar_sessions = false;
  static constexpr bool has_buying_power = false;
  static constexpr bool has_buying_power_quantity = false;
  static constexpr bool has_commission_schedule = false;
  /**
   * Nowhere, which is the honest answer for a broker this type stands in for.
   *
   * `NONE` rather than `HOLDINGS`: a stand-in that claimed the figure arrives with the positions
   * would have a runner skip the inquiry and then find nothing there.
   */
  static constexpr SellableSource sellable_source = SellableSource::NONE;
  static constexpr bool supports_interval(BarInterval) { return false; }
};

/** Capability set in which the broker implements everything optional. */
struct AllCaps {
  static constexpr bool has_account_discovery = true;
  static constexpr bool has_client_order_id = true;
  static constexpr bool has_working_orders = true;
  static constexpr bool has_amendable_quantity = true;
  static constexpr bool has_order_by_id = true;
  static constexpr bool has_partial_cancel = true;
  static constexpr bool history_requires_range = true;
  static constexpr bool has_history_cursor = true;
  static constexpr bool has_history_limit = true;
  static constexpr bool has_history_symbol_filter = true;
  static constexpr bool has_depth = true;
  static constexpr bool has_price_limits = true;
  static constexpr bool has_trade_prints = true;
  static constexpr bool has_batch_prices = true;
  static constexpr bool has_reference_data = true;
  static constexpr bool has_batch_reference_data = true;
  static constexpr bool has_calendar = true;
  static constexpr bool has_calendar_range = true;
  static constexpr bool has_calendar_sessions = true;
  static constexpr bool supports_interval(BarInterval) { return true; }
};

/** Compile-time tag standing in for a broker in a probe context. */
struct ProbeBroker {
  static constexpr std::string_view name = "probe";
};

/**
 * A complete context that does nothing, for checking a strategy compiles.
 *
 * Two uses, and the second is the interesting one. It is a usable stub for tests — intents land
 * in `submitted_` where a test can read them. And it is the reference context
 * `ATL_ASSERT_STRATEGY` compares against to prove a strategy's handlers are generic.
 *
 * Parameterized on the capability set so both branches of a capability gate get type-checked.
 * `MinimalContext` has every capability absent and `FullContext` has every one present, so a
 * strategy asserted against both has compiled the `if constexpr` branch it takes for a broker
 * that offers everything *and* the one it takes for a broker that offers nothing. Probing one
 * setting alone would leave the other branch unchecked until a broker bound to it.
 *
 * It deliberately does not provide the series accessors a real runner has — `bars`, `frame`,
 * `panel`. Those return analytics types and therefore need a linear-algebra backend, which this
 * layer does not require: a strategy trading on prices and positions alone should not have to
 * acquire one. A strategy that reads series is checked when its runner instantiates it.
 */
template <typename CapsType>
class ProbeContextT {
public:
  using Broker = ProbeBroker;
  using caps = CapsType;

  /** A log that records rather than writes, so a test can assert on what a strategy said. */
  class Log {
  public:
    void note(std::string_view message) { lines_.emplace_back(message); }
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
  const std::vector<TargetPosition>& targets() const noexcept { return targets_; }
  const std::vector<CancelIntent<ProbeBroker>>& canceled() const noexcept { return canceled_; }

  /** Mutable state a test arranges before invoking a handler. */
  std::chrono::system_clock::time_point now_{};
  MarketPhase phase_{MarketPhase::OPEN};
  TradingCalendar calendar_;
  Portfolio<ProbeBroker> portfolio_;
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
 * Instantiating this type produces the diagnostic, the same arrangement `BrokerConformance` uses
 * and for the same reason: a C++17 `static_assert` message must be a string literal, so the
 * client type has to arrive through the instantiation context rather than the message.
 */
template <typename S, typename Ctx>
struct StrategyConformance {
  static_assert(
    strategy_detail::has_any_handler<S, Ctx>,
    "This type defines no strategy handler that the runner can call. Expected at least one of "
    "on_start, on_bar, on_quote, on_fill, on_order_update, on_phase, or on_stop, each taking "
    "the context by reference as its first parameter. A handler with a different signature is "
    "not detected, so check the parameter types: on_bar takes (Ctx&, const BarEvent&)"
  );
  static_assert(
    strategy_detail::handlers_are_generic<S, Ctx, MinimalContext> &&
      strategy_detail::handlers_are_generic<S, Ctx, FullContext>,
    "A handler is written against one concrete context type rather than as a template. It would "
    "compile for this runner and silently exclude every other, so a strategy could not be "
    "dry-run or backtested without being rewritten. Declare each handler as "
    "`template <class Ctx> void on_bar(Ctx&, const BarEvent&)`"
  );

  static constexpr bool value = true;
};

/**
 * Asserts that `S` is a strategy the runner for `Ctx` can drive.
 *
 * Checks two things a misspelling or a stray concrete type would otherwise hide until much later:
 * that at least one handler is detected, and that every handler detected is also detected for two
 * unrelated probe contexts, which is what proves it is a template and therefore portable.
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
