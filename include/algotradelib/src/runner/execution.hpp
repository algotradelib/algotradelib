#pragma once

#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <string>
#include <type_traits>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * What became of one instruction handed to a broker, and what the runner owes as a result.
 *
 * This is where `order-core`'s hardest rule lives. An order POST is not idempotent on either
 * supported broker, and a request that fails *in transit* has an outcome nobody knows: the broker
 * may have recorded the order and lost the reply, or never received it at all. Retrying is how a
 * position gets doubled; assuming failure is how a live order goes unwatched.
 *
 * So a send does not have two outcomes. `INDETERMINATE` obliges the runner to stop and read the
 * broker's own order list before it may act on that instrument again, and `REFUSED_LOCALLY`
 * separates the binding's own refusal from the broker's, because only one of them is a request the
 * broker counted.
 */
enum class SendOutcome {
  /** The broker recorded the instruction. The order exists. */
  ACCEPTED,
  /**
   * The broker refused the instruction. The order does not exist.
   *
   * Safe, and distinct from a transport failure precisely because it is knowable: a refusal is an
   * answer, so nothing needs reconciling.
   */
  REJECTED,
  /**
   * The request failed in transit. Whether the order exists is unknown.
   *
   * Nothing further may be sent for that instrument until the broker's order list has been read.
   */
  INDETERMINATE,
  /**
   * A local precondition failed, so nothing was sent and the broker was never asked.
   *
   * Distinct from `REJECTED` even though both mean the order does not exist, because they oblige
   * opposite responses. A rejection is the broker's *answer*: the instruction was wrong, the rest of
   * the session is fine, and the loop should carry on. This is the local machinery failing -- the
   * order journal could not be written, say -- which says nothing about the instruction and
   * everything about whether the runner should still be trading. Every subsequent order would fail
   * the same way, and each one would look like a broker refusal.
   *
   * So it is **sticky and fatal to sending**: the runner records it and the loop gates on it, rather
   * than reporting one refusal per bar until somebody reads the logs closely enough to notice that
   * no broker was involved.
   */
  UNAVAILABLE,
  /**
   * The binding itself refused the instruction, so nothing was sent and no request was counted.
   *
   * It is a state of its own because every other one carries a claim that is false here.
   * `REJECTED` says the *broker* refused, which obliges the runner to count a request against the
   * rate limit -- the broker counted one, after all -- and spending budget for a request nobody
   * made can refuse a later order that would have been allowed. `UNAVAILABLE` says the local
   * machinery is broken, which is sticky and fatal to sending; this is not, because the next
   * instruction may be perfectly sendable. `INDETERMINATE` says the position is in doubt, and
   * nothing is in doubt when nothing left the process.
   *
   * What it means for the order is exactly what `REJECTED` means -- it does not exist, nothing
   * needs reconciling -- and what it means for the session is exactly what `ACCEPTED` means: carry
   * on. It says only that the instruction asked the binding for something the binding cannot do:
   * an idempotency key at a broker that accepts none, or a withdrawal for an order the broker has
   * no reference for.
   */
  REFUSED_LOCALLY,
};

/** Returns a stable lowercase name for `outcome`, for logs and structured reporting. */
inline constexpr std::string_view send_outcome_name(SendOutcome outcome) noexcept {
  switch (outcome) {
    case SendOutcome::ACCEPTED:
      return "accepted";
    case SendOutcome::REJECTED:
      return "rejected";
    case SendOutcome::INDETERMINATE:
      return "indeterminate";
    case SendOutcome::UNAVAILABLE:
      return "unavailable";
    case SendOutcome::REFUSED_LOCALLY:
      return "refused_locally";
  }
  return "unknown";
}

/**
 * Returns what an order-writing failure means for the order's existence.
 *
 * The single most consequential function in the runner, because it decides whether the position is
 * known. Four categories leave the outcome unknown, and three of them are not obvious:
 *
 * - **`TRANSPORT`** is the expected one. The connection failed, and whether the bytes arrived is
 *   exactly what is not known.
 * - **`CANCELLED`** is a *local* abandonment — a deadline expired, or the session closed — and says
 *   nothing about whether the request had already gone out. Treating a local timeout as a failure
 *   is how an order nobody is watching ends up resting on an exchange.
 * - **`PROTOCOL`** means the broker answered and the answer could not be parsed. That answer may
 *   well have been an acknowledgement. This is the trap: the request plainly succeeded at the
 *   transport level, so it reads like a client-side problem, and the order may be live.
 * - **An HTTP 5xx** is the broker's own infrastructure failing, which can happen after the
 *   instruction reached the matching engine. A 4xx is the broker declining to act, which is an
 *   answer.
 *
 * Everything else is a refusal: authentication fails before processing, a rate limit is refused
 * before processing, and an invalid argument is either rejected locally or declined by the broker.
 * In each case the order does not exist.
 *
 * The asymmetry is deliberate. Calling an indeterminate outcome a rejection risks an unwatched live
 * order and a doubled position; calling a rejection indeterminate costs one reconciliation. Only
 * one of those can lose money.
 */
inline SendOutcome outcome_of(const BrokerError& error) {
  if (!error.reached_broker_) {
    // **Checked before the category, because it outranks every one of them.** An error saying the
    // request never went out settles both questions this function exists to answer: the instruction
    // does not exist, and no request was counted. No category settles either on its own --
    // `INVALID_ARGUMENT` is both a broker's refusal and a client's own validation, `AUTHENTICATION`
    // is also a token refresh that failed before the order was ever built, and `TRANSPORT` on that
    // token request leaves nothing in doubt about an order nobody sent.
    return SendOutcome::REFUSED_LOCALLY;
  }
  switch (error.category_) {
    case ErrorCategory::TRANSPORT:
    case ErrorCategory::CANCELLED:
    case ErrorCategory::PROTOCOL:
      return SendOutcome::INDETERMINATE;
    case ErrorCategory::HTTP:
      return error.http_status_ >= 500 ? SendOutcome::INDETERMINATE : SendOutcome::REJECTED;
    case ErrorCategory::AUTHENTICATION:
    case ErrorCategory::RATE_LIMIT:
      return SendOutcome::REJECTED;
    case ErrorCategory::INVALID_ARGUMENT:
      // Reaching here means the broker was asked and refused. A client's own validation carries the
      // same category and is separated above by `reached_broker_`, which defaults to true -- so a
      // client that says nothing is read as having sent, over-counting its own budget rather than
      // exceeding the broker's.
      return SendOutcome::REJECTED;
  }
  // A category this library does not recognize is treated as unknown rather than as a refusal,
  // for the same asymmetry: the cost of being wrong is not the same in both directions.
  return SendOutcome::INDETERMINATE;
}

/**
 * Returns whether `error` says the broker could not be reached, as opposed to the binding declining.
 *
 * Both map to `SendOutcome::REFUSED_LOCALLY` -- the request was never written, so nothing exists and
 * nothing was counted -- and they differ in what comes next. A client's own validation
 * (`INVALID_ARGUMENT`: a price off the grid, a key the broker cannot carry) will refuse the same
 * instruction again, so it is final. Anything else that never left -- a DNS, connect or TLS failure
 * (`TRANSPORT`), a closed client or a request whose rate-limit admission outlived its deadline
 * (`CANCELLED`), a token request that failed (`AUTHENTICATION`, or
 * `PROTOCOL` for one that could not be parsed) -- is about the connection, and the same instruction
 * may go once the broker answers again. `Runner` keeps such an instruction for a later pass.
 */
inline bool broker_unreachable(const BrokerError& error) noexcept {
  return !error.reached_broker_ && error.category_ != ErrorCategory::INVALID_ARGUMENT;
}

/**
 * Returns whether `error` is a broker turning a request away for its rate limit.
 *
 * `SendOutcome::REJECTED`, rightly: the broker answered and processed nothing (KIS's `EGW00201`, "too
 * many requests per second", and `EGW00215`, when answered with HTTP 200; the KIS adapter leaves an
 * order answered with a non-2xx status unclassified, so it stays indeterminate). But the refusal is about the moment, not the instruction,
 * so `Runner` keeps the instruction for a later pass, as it keeps one the broker could not be reached
 * for (`broker_unreachable`).
 */
inline bool broker_rate_limited(const BrokerError& error) noexcept {
  return error.reached_broker_ && error.category_ == ErrorCategory::RATE_LIMIT;
}

/**
 * Returns whether `outcome` means the broker was asked at all.
 *
 * Two outcomes mean it was not, and they differ in everything except that: `UNAVAILABLE` is the
 * local machinery failing and stops the runner sending, while `REFUSED_LOCALLY` is the binding
 * declining one instruction it cannot express. Both leave the broker unasked, which is the only
 * question this answers -- and the one a rate-limit window and a request count depend on.
 */
inline constexpr bool send_outcome_reached_broker(SendOutcome outcome) noexcept {
  return outcome != SendOutcome::UNAVAILABLE && outcome != SendOutcome::REFUSED_LOCALLY;
}

namespace runner_detail {

/**
 * Whether `Executor` can put an order in front of a real broker.
 *
 * Declared by the executor, because it is the executor that knows. It decides one thing: whether a
 * runner built on it must have read the account before it places. That requirement has to hold for
 * every live path, and there are more of those than the two binding factories --
 * `doxygen/vignettes/binding-a-broker.md` shows a `Runner<..., SessionExecutor<Session>>` assembled
 * by hand, and a rule installed only by the factories would hold for their callers and nobody else.
 *
 * **The default is `true`, and an executor opts *out*.** That is the opposite of how a detection
 * trait beside it usually reads, and it is deliberate: the two directions fail differently. An
 * unmarked executor assumed harmless is a consumer's own adapter -- one whose `place` forwards to
 * `SessionExecutor` or straight to a broker client -- silently exempted from the rule this whole
 * branch exists for, and it fails by sending a real order sized against an account nobody read. An
 * unmarked executor assumed live is a test double that refuses until it is given a stamped portfolio
 * or says `allow_absent_portfolio()`, and it fails by being noticed at once. Only one of those two
 * is discoverable, so silence means live.
 *
 * `RecordingExecutor` and `backtest::QueueingExecutor` therefore say `false` explicitly: a dry run
 * and a replay have no holdings inquiry that could have failed, and a replay stamps its own
 * portfolio. `JournalingExecutor` forwards whatever it wraps, because recording an order does not
 * change who receives it.
 */
template <typename Executor, typename = void>
struct sends_to_a_broker : std::true_type {};

template <typename Executor>
struct sends_to_a_broker<Executor, std::void_t<decltype(Executor::reaches_a_broker)>>
  : std::bool_constant<Executor::reaches_a_broker> {};

} // namespace runner_detail

/** Why a planned order was held back rather than sent or refused. */
enum class WithheldReason {
  /**
   * An earlier instruction for this instrument failed in transit.
   *
   * The runner does not know whether that order exists, so sending another could double the
   * position. It withholds until the broker's order list resolves the question.
   */
  RECONCILE_REQUIRED,
  /**
   * Local machinery could not send, so the broker was never asked.
   *
   * Nothing is in doubt about the position -- nothing was attempted -- but nothing further will
   * succeed either until whatever broke is fixed.
   */
  LOCAL_FAILURE,
};

/** Returns a stable lowercase name for `reason`. */
inline constexpr std::string_view withheld_reason_name(WithheldReason reason) noexcept {
  switch (reason) {
    case WithheldReason::RECONCILE_REQUIRED:
      return "reconcile_required";
    case WithheldReason::LOCAL_FAILURE:
      return "local_failure";
  }
  return "unknown";
}

/**
 * Why a whole reconciliation plan was held back before any of it was sent.
 *
 * Separate from `WithheldReason` because the two answer different questions. That one is about a
 * *planned order*; this one is about a plan, which may contain no order at all -- a target of zero
 * against an account that is already flat plans only withdrawals -- and whose danger is precisely in
 * the half that is not an order.
 */
enum class WithheldPlanReason {
  /**
   * No holdings snapshot has ever been read, so the plan was computed from an account nobody has
   * seen.
   *
   * The dangerous case, because an empty `Portfolio` reads as *flat*: a target of zero is then
   * already satisfied, every open row on the instrument counts as opposing it, and the plan withdraws
   * orders that may be the only thing protecting a real holding.
   */
  PORTFOLIO_ABSENT,
  /** The snapshot the plan was computed from is older than the stated policy allows. */
  PORTFOLIO_STALE,
  /**
   * Something is holding placements, so the plan's replacement half cannot go out.
   *
   * Withdrawing without replacing is not the instruction the strategy stated. `Runner::settle` sends
   * a plan's cancellations before its placements are ever guard-checked, so without this the
   * cancellation is already gone by the time the refusal is known -- and a reprice becomes a bare
   * withdrawal, which is how a resting protective order disappears.
   */
  PLACEMENT_HELD,
  /**
   * The working-order listing arrived after the holdings the plan was sized against.
   *
   * A fill landing between the two reads leaves the listing before it reaches the holdings, so the
   * account reads as though the order never existed and a target buys again. Withheld until a holdings
   * read newer than the listing; a `TargetPosition` is retained and planned again then
   * (`Runner::retry_retained_targets`, which also retries targets withheld as absent or stale), an
   * `OrderIntent` is not. See
   * `strategy::Portfolio::listing_newer_than_holdings`.
   */
  PORTFOLIO_SUPERSEDED,
};

/** Returns a stable lowercase name for `reason`. */
inline constexpr std::string_view withheld_plan_reason_name(WithheldPlanReason reason) noexcept {
  switch (reason) {
    case WithheldPlanReason::PORTFOLIO_ABSENT:
      return "portfolio_absent";
    case WithheldPlanReason::PORTFOLIO_STALE:
      return "portfolio_stale";
    case WithheldPlanReason::PORTFOLIO_SUPERSEDED:
      return "portfolio_superseded";
    case WithheldPlanReason::PLACEMENT_HELD:
      return "placement_held";
  }
  return "unknown";
}

/**
 * One reconciliation plan the runner declined to act on, and what it would have done.
 *
 * Reported instead of, not beside, the plan's own orders: the entry exists so that a withheld plan
 * is *visible* rather than silent, and reporting its halves separately would suggest each was
 * decided on its own when the whole point is that they were decided together.
 *
 * Templated on the broker tag because `not_canceled_` names real order references.
 */
template <typename BrokerTag>
struct WithheldPlan {
  /** Why the plan was held back. */
  WithheldPlanReason reason_{WithheldPlanReason::PORTFOLIO_ABSENT};
  /** The instrument the plan was for. */
  InstrumentId instrument_;
  /** The stating intent's own tag, so an operator can find which instruction this was. */
  std::string tag_;
  /** The rows the plan would have withdrawn, and did not. */
  std::vector<OrderRef<BrokerTag>> not_canceled_;
  /** How many orders the plan would have placed, and did not. Zero for a withdrawal-only plan. */
  std::size_t not_placed_{0};
  /** What the runner knows about why, including the blocking hold when there was one. */
  std::string detail_;
};

/**
 * The broker's own account of one refusal, kept so a count has something to chase.
 *
 * A figure tells an operator that orders are being refused; it does not tell them why, and the
 * cheapest useful answer is the last one. Structured rather than a formatted string so a reader of
 * many symbols can find which instrument it was about, and so a caller can match on the code without
 * parsing prose.
 *
 * **`message_` is the broker's, and this library does not bound its length.** One record is kept
 * rather than a list, so a run refused ten thousand times costs one of them -- but the one it costs
 * is whatever the broker wrote.
 */
struct BrokerRefusal {
  /** The broker's own code for the refusal, where it published one. */
  std::string code_;
  /** The broker's own message, unbounded in length and unedited. */
  std::string message_;
  /** Which instrument the refused instruction was for. */
  InstrumentId instrument_;
  /** The strategy's own label on the intent that produced it, for attribution. */
  std::string tag_;
};

/** One order the runner declined to send, and why. */
struct WithheldOrder {
  /** Why the runner declined to send it. A local refusal, never a broker's. */
  WithheldReason reason_{WithheldReason::RECONCILE_REQUIRED};
  /** The order that was not sent. */
  strategy::PlannedOrder order_;
  /** What the runner knows about why the state is unresolved. */
  std::string detail_;
};

/**
 * The instruments whose order state is unknown, and what is owed for each.
 *
 * Deliberately a separate, broker-free type. The behaviour it encodes is the most important in the
 * branch and the hardest to exercise against a real broker — a transport failure mid-POST is not
 * something a test can ask a broker for — so it is a plain value that a test drives directly.
 *
 * **Nothing clears an entry except reading the broker's own order list.** Not a timeout, not a
 * later success, and not a subsequent bar. A timeout would resume trading on a guess; a later
 * success on a *different* instruction says nothing about the one that vanished.
 *
 * **It does not survive the process, and that is a real limitation rather than an oversight to
 * discover later.** The entries live in memory. Three consequences, in increasing order of how much
 * they matter:
 *
 * 1. Killing a runner sends nothing further, because there is no runner. But it is **not a
 *    flatten**: an order the broker already accepted stays live on the exchange, which knows nothing
 *    about a process that died.
 * 2. A mark made a moment before termination is lost with the rest of the ledger.
 * 3. Worse, an order *in flight* when the process is killed never reaches the classification at all.
 *    A default `SIGINT` handler terminates immediately, so no error path runs, `outcome_of` is never
 *    called, and nothing records that the outcome is unknown.
 *
 * So after any abnormal termination, the broker's order list has to be read before trading resumes,
 * and a restarted runner cannot infer from an empty ledger that nothing is owed. That is automatic
 * now: `PollingLoop` sends nothing until it has read the order list once, and a `FileJournal` bound
 * through `live::bind_journaled` carries the instructions themselves across the process boundary so
 * `recover_runner` can withhold what is owed and resolve it against the broker's own listings.
 */
class IndeterminacyLedger {
public:
  /** Records that an instruction for `id` failed with an unknown outcome. */
  void mark(const InstrumentId& id, std::string detail) {
    for (auto& entry : pending_) {
      if (entry.instrument_ == id) {
        // Kept as the first reason rather than the latest. The earliest unresolved instruction is
        // the one whose existence is in doubt; a later failure adds no information about it.
        return;
      }
    }
    pending_.push_back(Entry{id, std::move(detail)});
  }

  /** Returns whether anything may be sent for `id`. */
  bool requires_reconcile(const InstrumentId& id) const {
    for (const auto& entry : pending_) {
      if (entry.instrument_ == id) {
        return true;
      }
    }
    return false;
  }

  /** Returns why `id` is unresolved, or an empty view when it is not. */
  std::string_view detail(const InstrumentId& id) const {
    for (const auto& entry : pending_) {
      if (entry.instrument_ == id) {
        return entry.detail_;
      }
    }
    return {};
  }

  /**
   * Clears `id` after the broker's own order list has been read for it.
   *
   * Named for what it requires rather than for what it does, because calling it without having
   * read that list is the one way to defeat the whole mechanism.
   */
  void resolve_after_reading_orders(const InstrumentId& id) {
    for (auto at = pending_.begin(); at != pending_.end(); ++at) {
      if (at->instrument_ == id) {
        pending_.erase(at);
        return;
      }
    }
  }

  /** Returns every instrument awaiting reconciliation. */
  std::vector<InstrumentId> pending() const {
    std::vector<InstrumentId> ids;
    ids.reserve(pending_.size());
    for (const auto& entry : pending_) {
      ids.push_back(entry.instrument_);
    }
    return ids;
  }

  /** Returns whether anything at all is unresolved. */
  bool empty() const noexcept { return pending_.empty(); }

private:
  struct Entry {
    InstrumentId instrument_;
    std::string detail_;
  };

  std::vector<Entry> pending_;
};

/**
 * An executor that sends nothing and records what it was given.
 *
 * What a dry run uses, and the default. Every instruction is `ACCEPTED`, because a dry run has no
 * transport that could fail — which is also why the indeterminacy path is tested with an executor
 * that fails on purpose rather than by waiting for a real outage.
 */
template <typename BrokerTag>
class RecordingExecutor {
public:

  /**
   * A dry run reaches no broker, so it is exempt from having to have read the account.
   *
   * Stated rather than left to a default, because the default is `true`: see `sends_to_a_broker` for
   * why silence has to mean live.
   */
  static constexpr bool reaches_a_broker = false;
  /** Accepts a placement and records it. */
  SendOutcome place(const strategy::PlannedOrder& order) {
    placed_.push_back(order);
    return SendOutcome::ACCEPTED;
  }

  /** Accepts a cancellation and records it. */
  SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    canceled_.push_back(working.ref_);
    return SendOutcome::ACCEPTED;
  }

  /** Everything this executor was asked to place, in order. */
  const std::vector<strategy::PlannedOrder>& placed() const noexcept { return placed_; }

  /** Everything this executor was asked to cancel, in order. */
  const std::vector<OrderRef<BrokerTag>>& canceled() const noexcept { return canceled_; }

private:
  std::vector<strategy::PlannedOrder> placed_;
  std::vector<OrderRef<BrokerTag>> canceled_;
};

} // namespace runner
} // namespace algotradelib
