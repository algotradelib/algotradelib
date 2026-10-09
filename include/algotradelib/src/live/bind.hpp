#pragma once

#include "./session_executor.hpp"

#include "../runner/journal.hpp"
#include "../runner/runner.hpp"

#include <cstddef>
#include <utility>

namespace algotradelib {
namespace live {

/**
 * @file
 * @brief Factories that bind a strategy to a broker session in one call: `bind`, `bind_journaled`
 *   and `bind_observer`.
 *
 * Each reads the capabilities and broker tag off the session, so a runner cannot pair one broker's
 * tag with another's capabilities.
 */

/** @brief The number of trailing bars per instrument a bound runner keeps by default. */
inline constexpr std::size_t default_series_capacity = 512;

/**
 * @brief The runner type `bind` returns.
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 */
template <typename Strategy, typename Session>
using BoundRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  SessionExecutor<Session>
>;

/**
 * @brief The runner type `bind_observer` returns: the same capabilities and tag, sending nowhere.
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 */
template <typename Strategy, typename Session>
using ObservingRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  runner::RecordingExecutor<typename Session::BrokerTag>
>;

/**
 * @brief Binds a strategy to a broker session, so its instructions are sent to the broker.
 *
 * **This can place orders.** `bind_observer` is the same pipeline with nowhere to send. The runner
 * requires holdings to have been read before it places (see `Runner::allow_absent_portfolio` for a
 * run that reads none).
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 * @param session The session; borrowed, and must outlive the runner.
 * @param strategy The strategy.
 * @param limits The risk limits.
 * @param series_capacity How many trailing bars each cached series keeps.
 * @param default_route The venue for an order that names none, which is every order a strategy plans.
 *   `UNSPECIFIED` takes the broker's own default; Kiwoom has none, so **a Kiwoom runner places
 *   nothing until a route is given here**. See `SessionExecutor::default_route`.
 * @return The runner.
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

/**
 * @brief The runner type `bind_journaled` returns: the same pipeline, journaling before it sends.
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 * @tparam Journal The journal type (such as `runner::FileJournal`).
 */
template <typename Strategy, typename Session, typename Journal>
using JournaledRunner = runner::Runner<
  Strategy,
  typename Session::Caps,
  typename Session::BrokerTag,
  runner::JournalingExecutor<SessionExecutor<Session>, Journal>
>;

/**
 * @brief Binds a strategy to a broker session, recording every instruction in a journal before it
 *   is sent.
 *
 * Use this rather than building a `runner::JournalingExecutor` yourself: `bind` makes its own
 * executor, so a journaling executor built beside it would never be used, and nothing would report
 * it.
 *
 * The runner is serialized (`runner::Runner::serialized`): one order per instrument until holdings
 * account for it. Call `runner::recover_runner` with the journal **before** trading, on every start:
 * it restores what a previous process left owed, and the runner places nothing until it has run. To
 * keep several orders on one instrument outstanding instead, call `allow_overlapping_orders` first.
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 * @tparam Journal The journal type.
 * @param session The session; borrowed, and must outlive the runner.
 * @param journal The journal; borrowed, and must outlive the runner.
 * @param strategy The strategy.
 * @param limits The risk limits.
 * @param series_capacity How many trailing bars each cached series keeps.
 * @param default_route As for `bind`; the journal records the route the order is sent with
 *   (`SessionExecutor::effective_route`, such as KRX for an unspecified KIS order).
 * @return The runner.
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
 * @brief Binds a strategy for observation: the session's capabilities and broker tag, and nothing
 *   to send through.
 *
 * A capability-gated strategy compiles the same branch as it would live. Nothing can be sent
 * because the runner has no executor that reaches a broker; its reports list what would have been
 * sent.
 *
 * @tparam Strategy The strategy type.
 * @tparam Session The broker session type.
 * @param session The session; only its type is used.
 * @param strategy The strategy.
 * @param limits The risk limits.
 * @param series_capacity How many trailing bars each cached series keeps.
 * @return The runner.
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
