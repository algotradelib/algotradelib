#pragma once

#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <chrono>
#include <string>
#include <type_traits>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * @brief What became of one instruction (a placement or a cancellation) an executor was asked to send.
 *
 * A request of unknown outcome (`INDETERMINATE`) is never replayed automatically: the runner places
 * nothing more for that instrument until the broker's order list is read and the instrument
 * resolved, though a named cancellation may still go. (A request known not to have reached the
 * broker, or refused for its rate limit, can be offered again; see
 * `Runner::retry_retained_targets`.)
 *
 * For a cancellation, `ACCEPTED` means the request was accepted, not that the order is gone, and a
 * refused cancellation leaves the order as it was (resting, or filled meanwhile).
 */
enum class SendOutcome {
  /** @brief The broker accepted the instruction (for a placement, the order exists). */
  ACCEPTED,
  /** @brief The broker refused the instruction (for a placement, the order does not exist); nothing needs reconciling. */
  REJECTED,
  /**
   * @brief The outcome is unknown (see `outcome_of`): the request failed in transit or was
   *   cancelled, the reply could not be read, the broker answered with an HTTP 5xx, or its answer is
   *   reported as `PROTOCOL`.
   *
   * At KIS (an unrecognized reply with `rt_cd` not `0`, such as insufficient balance), Toss (any
   * refusal but a rate limit) and Kiwoom (a `return_code` outside its documented error list) the broker's own
   * refusals are reported as `PROTOCOL`, so they land here, not in `REJECTED`. The exception is the
   * demo-server order refusals the KIS and Kiwoom clients recognize, which are `REJECTED`.
   * Nothing more is placed for that instrument until the broker's order list has been read and the
   * instrument resolved.
   */
  INDETERMINATE,
  /**
   * @brief Local machinery failed (such as the order journal not being writable), so nothing was sent.
   *
   * The runner stops sending (`strategy::HaltReason::LOCAL_FAILURE`) until
   * `Runner::resume_after_local_failure`, since every later order would fail the same way.
   */
  UNAVAILABLE,
  /**
   * @brief The binding refused the instruction before sending it (such as an idempotency key at a
   *   broker without one, or a cancellation of an order the broker has no reference for).
   *
   * No request was counted against the rate limit (for a placement, the order does not exist), and
   * the runner keeps trading.
   */
  REFUSED_LOCALLY,
};

/**
 * @brief Returns a lowercase name for an outcome, such as `"indeterminate"`, for logs.
 * @param outcome The outcome.
 * @return The name.
 */
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
 * @brief Classifies an error from an order request: did the order come to exist?
 *
 * - `REFUSED_LOCALLY` when `reached_broker_` is `false` (nothing was sent), whatever the category;
 * - `INDETERMINATE` for `TRANSPORT`, `CANCELLED` (a local timeout says nothing about whether the
 *   request went out), `PROTOCOL` (an unreadable reply may have been an acknowledgement; at KIS,
 *   Toss and Kiwoom this also covers most of the broker's own refusals), an HTTP status of 500 or
 *   more, and any unknown category;
 * - `REJECTED` for an HTTP status below 500, `AUTHENTICATION`, `RATE_LIMIT` and `INVALID_ARGUMENT`.
 *
 * Doubtful cases are classified as unknown, which costs a reconciliation, rather than as refused,
 * which could leave a live order unwatched.
 *
 * @param error The error.
 * @return The outcome.
 */
inline SendOutcome outcome_of(const BrokerError& error) {
  if (!error.reached_broker_) {
    // Checked first: a request that never went out settles the question whatever the category.
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
      // The broker was asked and refused (a client's own validation was caught above).
      return SendOutcome::REJECTED;
  }
  // An unrecognized category is treated as unknown, not refused.
  return SendOutcome::INDETERMINATE;
}

/**
 * @brief Tests whether an error means the broker could not be reached (as opposed to the binding
 *   refusing the instruction itself).
 *
 * Both are `REFUSED_LOCALLY`, but a connection problem (`TRANSPORT`, `CANCELLED`, a failed token
 * request) may clear, so `Runner` keeps the instruction for a later pass; a client's own
 * validation (`INVALID_ARGUMENT`) would refuse it again.
 *
 * @param error The error.
 * @return `true` if nothing was sent and the category is not `INVALID_ARGUMENT`.
 */
inline bool broker_unreachable(const BrokerError& error) noexcept {
  return !error.reached_broker_ && error.category_ != ErrorCategory::INVALID_ARGUMENT;
}

/**
 * @brief Tests whether an error is the broker's rate-limit refusal (such as KIS `EGW00201`).
 *
 * It is `REJECTED`, but the refusal is about the moment, so `Runner` keeps the instruction for a
 * later pass. (On an order, a KIS rate-limit refusal sent with an HTTP status of 500 or more is
 * `HTTP`, so it is `INDETERMINATE`.)
 *
 * @param error The error.
 * @return `true` if the request reached the broker and the category is `RATE_LIMIT`.
 */
inline bool broker_rate_limited(const BrokerError& error) noexcept {
  return error.reached_broker_ && error.category_ == ErrorCategory::RATE_LIMIT;
}

/**
 * @brief Tests whether an outcome means a request reached the broker.
 * @param outcome The outcome.
 * @return `false` for `UNAVAILABLE` and `REFUSED_LOCALLY`; `true` otherwise.
 */
inline constexpr bool send_outcome_reached_broker(SendOutcome outcome) noexcept {
  return outcome != SendOutcome::UNAVAILABLE && outcome != SendOutcome::REFUSED_LOCALLY;
}

namespace runner_detail {

// Whether `Executor` can put an order in front of a real broker.
//
// Declared by the executor, because it is the executor that knows. It decides one thing: whether a
// runner built on it must have read the account before it places. That requirement has to hold for
// every live path, and there are more of those than the two binding factories --
// `doxygen/vignettes/binding-a-broker.md` shows a `Runner<..., SessionExecutor<Session>>` assembled
// by hand, and a rule installed only by the factories would hold for their callers and nobody else.
//
// **The default is `true`, and an executor opts *out*.** That is the opposite of how a detection
// trait beside it usually reads, and it is deliberate: the two directions fail differently. An unmarked
// executor assumed harmless is a consumer's own adapter -- one whose `place` forwards to `SessionExecutor` or
// straight to a broker client -- silently exempted from the rule, and it fails by sending a real order sized
// against an account nobody read. An unmarked executor assumed live is a test double that refuses until it is
// given a stamped portfolio or says `allow_absent_portfolio()`, and it fails by being noticed at once. Only
// one of those two is discoverable, so silence means live.
//
// `RecordingExecutor` and `backtest::QueueingExecutor` therefore say `false` explicitly: a dry run
// and a replay have no holdings inquiry that could have failed, and a replay stamps its own
// portfolio. `JournalingExecutor` forwards whatever it wraps, because recording an order does not
// change who receives it.
template <typename Executor, typename = void>
struct sends_to_a_broker : std::true_type {};

template <typename Executor>
struct sends_to_a_broker<Executor, std::void_t<decltype(Executor::reaches_a_broker)>>
  : std::bool_constant<Executor::reaches_a_broker> {};

} // namespace runner_detail

/** @brief Why the runner held back a planned order without sending it. */
enum class WithheldReason {
  /**
   * @brief An earlier instruction for this instrument has an unknown outcome; nothing is sent until
   *   the broker's order list resolves it.
   */
  RECONCILE_REQUIRED,
  /** @brief Local machinery cannot send (see `SendOutcome::UNAVAILABLE`). */
  LOCAL_FAILURE,
  /**
   * @brief A serialized runner already has an order on this instrument that holdings have not yet
   *   accounted for (see `Runner::allow_overlapping_orders`).
   */
  AWAITING_CONFIRMATION,
};

/**
 * @brief Returns a lowercase name for a reason, such as `"reconcile_required"`, for logs.
 * @param reason The reason.
 * @return The name.
 */
inline constexpr std::string_view withheld_reason_name(WithheldReason reason) noexcept {
  switch (reason) {
    case WithheldReason::RECONCILE_REQUIRED:
      return "reconcile_required";
    case WithheldReason::LOCAL_FAILURE:
      return "local_failure";
    case WithheldReason::AWAITING_CONFIRMATION:
      return "awaiting_confirmation";
  }
  return "unknown";
}

/**
 * @brief Why the runner held back a whole reconciliation plan (cancellations and placements) before
 *   sending any of it.
 */
enum class WithheldPlanReason {
  /**
   * @brief No holdings have been read, so the plan was computed against an account that looks flat
   *   (and could cancel orders protecting a real holding).
   */
  PORTFOLIO_ABSENT,
  /** @brief The holdings the plan was computed from are older than the configured maximum age. */
  PORTFOLIO_STALE,
  /**
   * @brief A hold stops placements, so the plan's cancellations are held too (a reprice must not
   *   become a bare cancellation).
   */
  PLACEMENT_HELD,
  /**
   * @brief The working-order listing is newer than the holdings the plan was sized against (see
   *   `strategy::Portfolio::listing_newer_than_holdings`).
   *
   * A `TargetPosition` withheld for this, or as absent or stale, is kept and planned again once newer
   * holdings arrive (`Runner::retry_retained_targets`); an `OrderIntent` is not.
   */
  PORTFOLIO_SUPERSEDED,
  /**
   * @brief A serialized runner may not place on the instrument yet, so the plan's cancellations are
   *   held too: an order there is not yet accounted for, the runner was not recovered, or holdings
   *   cannot serve as the next order's baseline. The detail says which; a `TargetPosition` is kept
   *   and planned again after a newer holdings read. See `Runner::allow_overlapping_orders`.
   */
  AWAITING_CONFIRMATION,
};

/**
 * @brief Returns a lowercase name for a reason, such as `"portfolio_stale"`, for logs.
 * @param reason The reason.
 * @return The name.
 */
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
    case WithheldPlanReason::AWAITING_CONFIRMATION:
      return "awaiting_confirmation";
  }
  return "unknown";
}

/**
 * @brief One reconciliation plan the runner held back, and what it would have done.
 *
 * @tparam BrokerTag The broker tag of the order references.
 */
template <typename BrokerTag>
struct WithheldPlan {
  /** @brief Why the plan was held back. */
  WithheldPlanReason reason_{WithheldPlanReason::PORTFOLIO_ABSENT};
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The tag of the strategy's instruction. */
  std::string tag_;
  /** @brief The orders the plan would have cancelled. */
  std::vector<OrderRef<BrokerTag>> not_canceled_;
  /** @brief How many orders the plan would have placed. */
  std::size_t not_placed_{0};
  /** @brief A description of why, naming the hold if there was one. */
  std::string detail_;
};

/**
 * @brief What a serialized runner still owes on an instrument (`Runner::obligation_state`).
 */
enum class ObligationState {
  /** @brief Nothing: an order may be placed. */
  NONE,
  /** @brief An order the broker named, waiting for its history and holdings to account for it. */
  NAMED,
  /** @brief An order whose existence is unknown (its reply was lost and no sweep named it). */
  UNNAMED,
  /** @brief An order only an operator may account for (`Runner::release_obligations`). */
  OPERATOR,
};

/** @brief One order a serialized runner still owes (`Runner::obligations`). */
struct Obligation {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The id of the order's journal lines. */
  std::string journal_id_;
  /** @brief The broker's order id; empty while unnamed. */
  std::string order_id_;
  /** @brief The side. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The quantity holdings must still account for. */
  Decimal64 quantity_;
  /** @brief When it was sent, on the journal's clock. */
  std::chrono::system_clock::time_point sent_at_{};
  /** @brief Its state: `NAMED`, `UNNAMED` or `OPERATOR`. */
  ObligationState state_{ObligationState::NAMED};
};

/**
 * @brief Returns a lowercase name for a state, such as `"named"`, for logs.
 * @param state The state.
 * @return The name.
 */
inline constexpr std::string_view obligation_state_name(ObligationState state) noexcept {
  switch (state) {
    case ObligationState::NONE:
      return "none";
    case ObligationState::NAMED:
      return "named";
    case ObligationState::UNNAMED:
      return "unnamed";
    case ObligationState::OPERATOR:
      return "operator";
  }
  return "unknown";
}

/**
 * @brief The broker's own code and message for a refusal, kept for diagnosis.
 *
 * The runner keeps only the latest one. `message_` is the broker's text, unedited and of any length.
 */
struct BrokerRefusal {
  /** @brief The broker's error code, if it gave one. */
  std::string code_;
  /** @brief The broker's message. */
  std::string message_;
  /** @brief The instrument of the refused instruction. */
  InstrumentId instrument_;
  /** @brief The tag of the instruction. */
  std::string tag_;
};

/** @brief One order the runner held back without sending, and why. */
struct WithheldOrder {
  /** @brief Why it was held back. */
  WithheldReason reason_{WithheldReason::RECONCILE_REQUIRED};
  /** @brief The order. */
  strategy::PlannedOrder order_;
  /** @brief A description of why. */
  std::string detail_;
};

/**
 * @brief The instruments with an order of unknown outcome, where nothing may be placed until the
 *   broker's order list is read and the instrument resolved.
 *
 * Named cancellations are not blocked. Only `resolve_after_reading_orders` clears an entry; reading
 * the list alone does not. The ledger is in memory: it does not survive
 * the process, and an order in flight when a process is killed is never recorded. After an
 * abnormal exit, read the broker's order list before trading; `PollingLoop` does this before its
 * first send, and a `FileJournal` (`live::bind_journaled`) carries unresolved instructions across a
 * restart for `recover_runner`.
 *
 * @note Killing a runner does not cancel orders the broker already accepted.
 */
class IndeterminacyLedger {
public:
  /**
   * @brief Records an instrument's order of unknown outcome; an existing entry keeps its first detail.
   * @param id The instrument.
   * @param detail A description.
   */
  void mark(const InstrumentId& id, std::string detail) {
    for (auto& entry : pending_) {
      if (entry.instrument_ == id) {
        // Keep the first reason: the earliest unresolved instruction is the one in doubt.
        return;
      }
    }
    pending_.push_back(Entry{id, std::move(detail)});
  }

  /**
   * @brief Tests whether an instrument is waiting for reconciliation.
   * @param id The instrument.
   * @return `true` if nothing may be placed for it.
   */
  bool requires_reconcile(const InstrumentId& id) const {
    for (const auto& entry : pending_) {
      if (entry.instrument_ == id) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Returns why an instrument is unresolved.
   * @param id The instrument.
   * @return The detail, or an empty view if it is resolved.
   */
  std::string_view detail(const InstrumentId& id) const {
    for (const auto& entry : pending_) {
      if (entry.instrument_ == id) {
        return entry.detail_;
      }
    }
    return {};
  }

  /**
   * @brief Clears an instrument; call it only after reading the broker's order list for it.
   * @param id The instrument.
   */
  void resolve_after_reading_orders(const InstrumentId& id) {
    for (auto at = pending_.begin(); at != pending_.end(); ++at) {
      if (at->instrument_ == id) {
        pending_.erase(at);
        return;
      }
    }
  }

  /**
   * @brief Returns every instrument waiting for reconciliation.
   * @return The instruments.
   */
  std::vector<InstrumentId> pending() const {
    std::vector<InstrumentId> ids;
    ids.reserve(pending_.size());
    for (const auto& entry : pending_) {
      ids.push_back(entry.instrument_);
    }
    return ids;
  }

  /**
   * @brief Tests whether nothing is unresolved.
   * @return `true` if the ledger is empty.
   */
  bool empty() const noexcept { return pending_.empty(); }

private:
  struct Entry {
    InstrumentId instrument_;
    std::string detail_;
  };

  std::vector<Entry> pending_;
};

/**
 * @brief An executor that sends nothing and records what it was given (the dry run's default).
 *
 * Every instruction is `ACCEPTED`.
 *
 * @tparam BrokerTag The broker tag.
 */
template <typename BrokerTag>
class RecordingExecutor {
public:

  /** @brief `false`: a dry run reaches no broker, so the runner does not require a read account. */
  static constexpr bool reaches_a_broker = false;
  /**
   * @brief Records a placement.
   * @param order The order.
   * @return `ACCEPTED`.
   */
  SendOutcome place(const strategy::PlannedOrder& order) {
    placed_.push_back(order);
    return SendOutcome::ACCEPTED;
  }

  /**
   * @brief Records a cancellation.
   * @param working The order.
   * @return `ACCEPTED`.
   */
  SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    canceled_.push_back(working.ref_);
    return SendOutcome::ACCEPTED;
  }

  /**
   * @brief Returns every placement recorded.
   * @return The orders, in order.
   */
  const std::vector<strategy::PlannedOrder>& placed() const noexcept { return placed_; }

  /**
   * @brief Returns every cancellation recorded.
   * @return The references, in order.
   */
  const std::vector<OrderRef<BrokerTag>>& canceled() const noexcept { return canceled_; }

private:
  std::vector<strategy::PlannedOrder> placed_;
  std::vector<OrderRef<BrokerTag>> canceled_;
};

} // namespace runner
} // namespace algotradelib
