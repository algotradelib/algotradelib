#pragma once

#include "../strategy/concept.hpp"

namespace algotradelib {
namespace runner {

/**
 * Calls a strategy's handler when it has one, and costs nothing when it does not.
 *
 * Every handler is optional, so a runner cannot simply call them: a strategy that reacts only to
 * bars must not be required to write five empty functions, and a runner must not be written twice
 * for strategies that do and do not define `on_quote`.
 *
 * Each function below is the `if constexpr` that resolves that, in one place. An absent handler
 * compiles to nothing — no branch, no vtable, no empty call — which is what lets the vanilla
 * strategies stay small and lets the runner stay one implementation.
 *
 * These take the strategy by reference and mutate it, because a strategy holds its own state.
 */
namespace dispatch {

/** Calls `on_start` if `strategy` defines one. */
template <typename S, typename Ctx>
void start(S& strategy, Ctx& context) {
  if constexpr (strategy::strategy_detail::has_on_start<S, Ctx>::value) {
    strategy.on_start(context);
  } else {
    (void)strategy;
    (void)context;
  }
}

/** Calls `on_stop` if `strategy` defines one. */
template <typename S, typename Ctx>
void stop(S& strategy, Ctx& context) {
  if constexpr (strategy::strategy_detail::has_on_stop<S, Ctx>::value) {
    strategy.on_stop(context);
  } else {
    (void)strategy;
    (void)context;
  }
}

/** Calls `on_bar` if `strategy` defines one. */
template <typename S, typename Ctx>
void bar(S& strategy, Ctx& context, const strategy::BarEvent& event) {
  if constexpr (strategy::strategy_detail::has_on_bar<S, Ctx>::value) {
    strategy.on_bar(context, event);
  } else {
    (void)strategy;
    (void)context;
    (void)event;
  }
}

/** Calls `on_quote` if `strategy` defines one. */
template <typename S, typename Ctx>
void quote(S& strategy, Ctx& context, const strategy::QuoteEvent& event) {
  if constexpr (strategy::strategy_detail::has_on_quote<S, Ctx>::value) {
    strategy.on_quote(context, event);
  } else {
    (void)strategy;
    (void)context;
    (void)event;
  }
}

/** Calls `on_fill` if `strategy` defines one. */
template <typename S, typename Ctx>
void fill(S& strategy, Ctx& context, const strategy::Fill& event) {
  if constexpr (strategy::strategy_detail::has_on_fill<S, Ctx>::value) {
    strategy.on_fill(context, event);
  } else {
    (void)strategy;
    (void)context;
    (void)event;
  }
}

/** Calls `on_order_update` if `strategy` defines one. */
template <typename S, typename Ctx>
void order_update(S& strategy, Ctx& context, const OrderSnapshot& event) {
  if constexpr (strategy::strategy_detail::has_on_order_update<S, Ctx>::value) {
    strategy.on_order_update(context, event);
  } else {
    (void)strategy;
    (void)context;
    (void)event;
  }
}

/** Calls `on_phase` if `strategy` defines one. */
template <typename S, typename Ctx>
void phase(S& strategy, Ctx& context, const strategy::MarketPhase& event) {
  if constexpr (strategy::strategy_detail::has_on_phase<S, Ctx>::value) {
    strategy.on_phase(context, event);
  } else {
    (void)strategy;
    (void)context;
    (void)event;
  }
}

} // namespace dispatch
} // namespace runner
} // namespace algotradelib
