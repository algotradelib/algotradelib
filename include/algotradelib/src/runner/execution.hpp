#pragma once

#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <string>
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
 * So the outcome has three values, not two, and the third obliges the runner to stop and read the
 * broker's own order list before it may act on that instrument again.
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
  switch (error.category_) {
    case ErrorCategory::TRANSPORT:
    case ErrorCategory::CANCELLED:
    case ErrorCategory::PROTOCOL:
      return SendOutcome::INDETERMINATE;
    case ErrorCategory::HTTP:
      return error.http_status_ >= 500 ? SendOutcome::INDETERMINATE : SendOutcome::REJECTED;
    case ErrorCategory::AUTHENTICATION:
    case ErrorCategory::RATE_LIMIT:
    case ErrorCategory::INVALID_ARGUMENT:
      return SendOutcome::REJECTED;
  }
  // A category this library does not recognize is treated as unknown rather than as a refusal,
  // for the same asymmetry: the cost of being wrong is not the same in both directions.
  return SendOutcome::INDETERMINATE;
}

/** Returns whether `outcome` means the broker was asked at all. */
inline constexpr bool send_outcome_reached_broker(SendOutcome outcome) noexcept {
  return outcome != SendOutcome::UNAVAILABLE;
}

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

/** One order the runner declined to send, and why. */
struct WithheldOrder {
  WithheldReason reason_{WithheldReason::RECONCILE_REQUIRED};
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
