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
  }
  return "unknown";
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
};

/** Returns a stable lowercase name for `reason`. */
inline constexpr std::string_view withheld_reason_name(WithheldReason reason) noexcept {
  switch (reason) {
    case WithheldReason::RECONCILE_REQUIRED:
      return "reconcile_required";
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
  SendOutcome cancel(const OrderRef<BrokerTag>& ref) {
    canceled_.push_back(ref);
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
