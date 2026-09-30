#pragma once

#include "./session_executor.hpp"

#include "../runner/journal.hpp"
#include "../runner/reconcile_orders.hpp"
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

/**
 * The number of bars a bound runner keeps per instrument unless told otherwise.
 *
 * Named so a caller who states a later argument -- a default route -- need not restate the number.
 */
inline constexpr std::size_t default_series_capacity = 512;

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
 *
 * `default_route` is the venue an order that names none is sent to -- and a strategy's orders name
 * none. `UNSPECIFIED` takes the broker's own default, which Kiwoom does not have: **a Kiwoom runner
 * places nothing until a route is stated here.** See `SessionExecutor::default_route`.
 */
template <typename Strategy, typename Session>
BoundRunner<Strategy, Session> bind(
  Session& session,
  Strategy strategy,
  strategy::RiskLimits limits = {},
  std::size_t series_capacity = default_series_capacity,
  Venue default_route = Venue::UNSPECIFIED
) {
  BoundRunner<Strategy, Session> bound(
    std::move(strategy),
    std::move(limits),
    series_capacity,
    SessionExecutor<Session>(session, default_route)
  );
  // **A runner that can reach a broker requires an account it has actually read** -- and that is not
  // arranged here. It comes from `SessionExecutor::reaches_a_broker`, so a runner assembled by hand
  // carries it too: `doxygen/vignettes/binding-a-broker.md` shows exactly that, and a rule this
  // factory installed would hold for its own callers and for nobody else. A run that genuinely reads
  // no holdings says `allow_absent_portfolio()` on the runner.
  return bound;
}

/** The runner type a journaled binding produces: the same pipeline, recording before it sends. */
template <typename Strategy, typename Session, typename Journal>
using JournaledRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  runner::JournalingExecutor<SessionExecutor<Session>, Journal>
>;

/**
 * Binds `strategy` to `session` so that every instruction is recorded before it is sent.
 *
 * **Use this rather than wrapping an executor yourself.** `bind` constructs its own
 * `SessionExecutor` internally, so a `JournalingExecutor` built beside the runner it was meant for
 * is simply an unused variable: the runner goes on sending through the plain executor, the journal
 * stays empty, and nothing anywhere reports a problem. That failure is silent and total, which is
 * exactly the kind this factory exists to make unexpressible.
 *
 * Recovery is a separate call, and the order matters: `runner::recover_runner` must run against the
 * journal *before* the loop starts, so the ledger is marked for whatever a previous process left
 * owed. The journal is borrowed and must outlive the runner.
 *
 * `default_route` is as for `bind`, and the journal records the route it produces.
 */
template <typename Strategy, typename Session, typename Journal>
JournaledRunner<Strategy, Session, Journal> bind_journaled(
  Session& session,
  Journal& journal,
  Strategy strategy,
  strategy::RiskLimits limits = {},
  std::size_t series_capacity = default_series_capacity,
  Venue default_route = Venue::UNSPECIFIED
) {
  JournaledRunner<Strategy, Session, Journal> bound(
    std::move(strategy),
    std::move(limits),
    series_capacity,
    runner::JournalingExecutor<SessionExecutor<Session>, Journal>(
      SessionExecutor<Session>(session, default_route), journal
    )
  );
  // As in `bind`: the requirement travels with the executor, not with this factory.
  return bound;
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
  std::size_t series_capacity = default_series_capacity
) {
  (void)session;
  return ObservingRunner<Strategy, Session>(
    std::move(strategy), std::move(limits), series_capacity, {}
  );
}

} // namespace live
} // namespace algotradelib
