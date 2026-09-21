#pragma once

#include "./session_executor.hpp"

#include "../runner/runner.hpp"

#include <cstddef>
#include <utility>

namespace algotradelib {
namespace live {

/**
 * Binding a strategy to a session in one call.
 *
 * `Runner` takes four template parameters because each is a genuine axis: the strategy, the broker's
 * capabilities, its compile-time tag, and where instructions are sent. Three of those are determined
 * by the session, though, so writing them out at a call site is repetition rather than expression —
 * and repetition that can be got subtly wrong, since nothing stops a caller pairing one broker's tag
 * with another's capabilities.
 *
 * These two factories read all three off the session, so a binding is one line and the mismatch is
 * unexpressible.
 */

/** The runner type a session-backed binding produces. */
template <typename Strategy, typename Session>
using BoundRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  SessionExecutor<Session>
>;

/** The runner type an observing binding produces: the same, sending nowhere. */
template <typename Strategy, typename Session>
using ObservingRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  runner::RecordingExecutor<typename Session::BrokerTag>
>;

/**
 * Binds `strategy` to `session`, so instructions reach the broker.
 *
 * **This can place orders.** `bind_observer` is the same pipeline with nowhere to send.
 *
 * The session is borrowed and must outlive the runner: `SessionExecutor` holds it by pointer, because
 * a runner uses a connection it does not own.
 */
template <typename Strategy, typename Session>
BoundRunner<Strategy, Session> bind(
  Session& session,
  Strategy strategy,
  strategy::RiskLimits limits = {},
  std::size_t series_capacity = 512
) {
  return BoundRunner<Strategy, Session>(
    std::move(strategy),
    std::move(limits),
    series_capacity,
    SessionExecutor<Session>(session)
  );
}

/**
 * Binds `strategy` for observation: real capabilities, real broker tag, and no session to send to.
 *
 * The session is taken only to read its types, which is the point — an observing runner is
 * instantiated with the *same* `Caps` and `BrokerTag` as the live one, so a capability-gated strategy
 * compiles the same branch in both and observing cannot diverge from trading.
 *
 * Nothing can be sent because there is nothing to send *through*, rather than because a check
 * declines to. That is a stronger arrangement than halting the guard, which would leave the send path
 * present and report every decision as a refusal.
 */
template <typename Strategy, typename Session>
ObservingRunner<Strategy, Session> bind_observer(
  Session& session,
  Strategy strategy,
  strategy::RiskLimits limits = {},
  std::size_t series_capacity = 512
) {
  (void)session;
  return ObservingRunner<Strategy, Session>(
    std::move(strategy), std::move(limits), series_capacity, {}
  );
}

} // namespace live
} // namespace algotradelib
