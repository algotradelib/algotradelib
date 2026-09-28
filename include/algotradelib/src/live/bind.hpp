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
  BoundRunner<Strategy, Session> bound(
    std::move(strategy),
    std::move(limits),
    series_capacity,
    SessionExecutor<Session>(session)
  );
  // **A runner that can reach a broker requires an account it has actually read.**
  //
  // Set here rather than on every `Runner`, because this is where the distinction is knowable: a
  // `DryRunner` and a replay have no holdings inquiry that could have failed, and a replay stamps
  // its own portfolio, so requiring it of them would refuse every order in a backtest to close a
  // hole neither has. A session-backed runner has exactly that inquiry, and an empty `Portfolio` is
  // the permissive answer to every ceiling it is about to be measured against.
  //
  // Not only for `runner::PollingLoop`: that one sets this itself each iteration, but driving a
  // bound runner by hand is a supported path -- `examples/example-strategy-live.cpp` does -- and it
  // is the path with no loop to hold anything. A run that genuinely reads no holdings says
  // `allow_absent_portfolio()` on the runner.
  bound.require_known_portfolio(true);
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
 */
template <typename Strategy, typename Session, typename Journal>
JournaledRunner<Strategy, Session, Journal> bind_journaled(
  Session& session,
  Journal& journal,
  Strategy strategy,
  strategy::RiskLimits limits = {},
  std::size_t series_capacity = 512
) {
  JournaledRunner<Strategy, Session, Journal> bound(
    std::move(strategy),
    std::move(limits),
    series_capacity,
    runner::JournalingExecutor<SessionExecutor<Session>, Journal>(
      SessionExecutor<Session>(session), journal
    )
  );
  // The same contract as `bind`, for the same reason: this one can reach a broker too.
  bound.require_known_portfolio(true);
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
  std::size_t series_capacity = 512
) {
  (void)session;
  return ObservingRunner<Strategy, Session>(
    std::move(strategy), std::move(limits), series_capacity, {}
  );
}

} // namespace live
} // namespace algotradelib
