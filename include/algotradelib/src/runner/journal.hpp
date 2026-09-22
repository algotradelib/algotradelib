#pragma once

#include "./clock.hpp"
#include "./execution.hpp"

#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * Which kind of instruction a journal line describes.
 *
 * Both are recorded, because both leave the same question behind when the process dies in the
 * middle of one. A placement killed in flight may have created an order nobody is watching; a
 * cancellation killed in flight may have withdrawn an order the restarted runner still believes is
 * resting, or may not have. `IndeterminacyLedger` already treats the two identically once the
 * failure is *observed* — `Runner::send_cancel` marks on an indeterminate cancellation exactly as
 * `settle()` does on an indeterminate placement — so a journal that recorded only placements would
 * lose half of what the ledger already knows how to hold.
 *
 * They differ in how the question is *answered*, which is why the distinction is stored rather than
 * inferred. See `reconcile_orders.hpp`: a cancellation names the broker's own order id, so matching
 * it against a listing is an identity. A placement has no such handle, and matching it is a
 * heuristic over attributes.
 */
enum class JournalAction {
  /** A new order was about to be sent, or was sent. */
  PLACE,
  /** A withdrawal of a resting order was about to be sent, or was sent. */
  CANCEL,
};

/** Returns a stable lowercase name for `action`, as written to the journal. */
inline constexpr std::string_view journal_action_name(JournalAction action) noexcept {
  switch (action) {
    case JournalAction::PLACE:
      return "place";
    case JournalAction::CANCEL:
      return "cancel";
  }
  return "unknown";
}

/** Returns the action `text` names, or nothing when it names none. */
inline std::optional<JournalAction> journal_action_from(std::string_view text) noexcept {
  if (text == "place") {
    return JournalAction::PLACE;
  }
  if (text == "cancel") {
    return JournalAction::CANCEL;
  }
  return std::nullopt;
}

/**
 * What a journal line says about an instruction's fate.
 *
 * `SendOutcome` with one extra value, and the extra value is the whole point. `PENDING` is written
 * *before* the send is attempted, so it is the only record that exists during the window this
 * feature was built for: the interval in which the bytes may be on the wire and the process may
 * die. The three terminal values are what a second line later says happened.
 *
 * A `PENDING` line with no terminal companion is therefore not a bug in the journal. It is the
 * signal — it means a process stopped between the write and the answer, and the order's existence
 * is unknown.
 */
enum class JournalOutcome {
  /** Recorded before the send. Whether the instruction exists is not yet known. */
  PENDING,
  /** The broker recorded the instruction. */
  ACCEPTED,
  /** The broker refused the instruction. */
  REJECTED,
  /** The request failed in transit. Whether the instruction exists is unknown. */
  INDETERMINATE,
};

/** Returns a stable lowercase name for `outcome`, as written to the journal. */
inline constexpr std::string_view journal_outcome_name(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::PENDING:
      return "pending";
    case JournalOutcome::ACCEPTED:
      return "accepted";
    case JournalOutcome::REJECTED:
      return "rejected";
    case JournalOutcome::INDETERMINATE:
      return "indeterminate";
  }
  return "unknown";
}

/** Returns the outcome `text` names, or nothing when it names none. */
inline std::optional<JournalOutcome> journal_outcome_from(std::string_view text) noexcept {
  if (text == "pending") {
    return JournalOutcome::PENDING;
  }
  if (text == "accepted") {
    return JournalOutcome::ACCEPTED;
  }
  if (text == "rejected") {
    return JournalOutcome::REJECTED;
  }
  if (text == "indeterminate") {
    return JournalOutcome::INDETERMINATE;
  }
  return std::nullopt;
}

/** Returns the journal outcome that records `outcome`. */
inline constexpr JournalOutcome journal_outcome_of(SendOutcome outcome) noexcept {
  switch (outcome) {
    case SendOutcome::ACCEPTED:
      return JournalOutcome::ACCEPTED;
    case SendOutcome::REJECTED:
      return JournalOutcome::REJECTED;
    case SendOutcome::INDETERMINATE:
      return JournalOutcome::INDETERMINATE;
  }
  return JournalOutcome::INDETERMINATE;
}

/** Returns whether `outcome` closes the question a `PENDING` line opened. */
inline constexpr bool journal_outcome_is_terminal(JournalOutcome outcome) noexcept {
  return outcome != JournalOutcome::PENDING;
}

/**
 * One line of the journal: an instruction, and what was known about it when the line was written.
 *
 * **Every line carries the whole instruction, not just a delta.** A terminal line repeats the
 * instrument, side, quantity and price that its `PENDING` predecessor already gave. That is
 * deliberate redundancy on an append-only file, and it buys two things that matter more than the
 * bytes it costs:
 *
 * - **The file stays readable with `tail`.** The plan for this feature names that as the primary
 *   operator interface during an incident, and a line that says only `{"id":"...","outcome":
 *   "indeterminate"}` sends the operator scrolling backwards through a day's trading to find out
 *   which instrument it was about.
 * - **A partially damaged file degrades gracefully.** If the `PENDING` line is the one lost to a
 *   truncated write, a self-describing terminal line still names the instrument.
 *
 * `recorded_at_` is the **local** clock, from the runner's own clock seam. It is not the broker's,
 * and the difference between the two is exactly what `reconcile_orders.hpp` has to widen its
 * matching window to absorb.
 */
struct JournalEntry {
  /**
   * Ties a terminal line to the `PENDING` line it answers.
   *
   * Locally generated and meaningless to any broker. It is not an idempotency key and must never
   * be used as one: no broker has seen it, so it promises nothing about a duplicate.
   */
  std::string id_;
  JournalAction action_{JournalAction::PLACE};
  JournalOutcome outcome_{JournalOutcome::PENDING};
  std::chrono::system_clock::time_point recorded_at_{};
  InstrumentId instrument_;
  OrderSide side_{OrderSide::BUY};
  OrderType type_{OrderType::LIMIT};
  Decimal64 quantity_;
  std::optional<Decimal64> price_;
  /**
   * The broker's own order id, where one is known.
   *
   * Set on a `CANCEL` line from the moment it is written, because a cancellation names an order
   * that already exists. Set on a `PLACE` line only once the broker has acknowledged it — which is
   * to say, only on a line that already resolves the question, never on one that opens it.
   */
  std::string order_id_;
  /** Why a terminal line says what it says, for an operator reading the file. */
  std::string detail_;
};

/**
 * Everything a journal held, and everything about it that could not be read.
 *
 * The second half is not an afterthought. A journal exists to be read after a crash, and a crash is
 * the one event that can leave the file's last line half-written. Returning damage as *data* rather
 * than as an exception or a silent skip is what lets recovery treat it the way it must be treated:
 * as evidence that something was in flight.
 */
struct JournalReplay {
  /** Every line that parsed, in file order. */
  std::vector<JournalEntry> entries_;
  /**
   * Lines that did not parse, which on an append-only file means a write that did not finish.
   *
   * Counted rather than discarded. A journal with an unreadable line is a journal that cannot
   * account for everything, and a caller that ignores this has quietly re-opened the window the
   * whole feature closes.
   */
  std::size_t unreadable_lines_{0};
  /** Whether the file's final line was incomplete, which is the signature of a kill mid-append. */
  bool truncated_tail_{false};
  /** What was wrong, for a report an operator reads. */
  std::string detail_;

  /** Returns whether every line was read. */
  bool intact() const noexcept { return unreadable_lines_ == 0 && !truncated_tail_; }
};

/**
 * One instruction the journal cannot account for, and the span in which it may have reached the
 * broker.
 *
 * Two different situations, and collapsing them would lose the one piece of information matching
 * depends on:
 *
 * - **Unanswered.** A `PENDING` line with no terminal companion. The process stopped between the
 *   write and the answer, so the span's end is unknown and has to be bounded by how long a request
 *   could have been outstanding.
 * - **Answered `INDETERMINATE`.** Both lines are present, and together they bracket the send
 *   exactly: the request began at one and failed at the other. A narrower, exact span.
 *
 * The second is easy to overlook, and overlooking it is a live bug rather than a missed
 * optimization. A runner that marks its ledger on a transport failure and is then restarted begins
 * with an empty ledger; unless recovery reads that pair back out of the journal, the restart
 * silently forgets an order whose existence is unknown -- which is the exact failure the journal was
 * written to prevent, arriving by a different door.
 */
struct OpenInstruction {
  /** The instruction, as its `PENDING` line recorded it. */
  JournalEntry entry_;
  /** When the send began, from the `PENDING` line. */
  std::chrono::system_clock::time_point opened_at_{};
  /** When the send is known to have failed, where a terminal line says so. */
  std::optional<std::chrono::system_clock::time_point> closed_at_;
  /** Whether a terminal line answered, and answered "unknown". */
  bool answered_{false};
  /** What the journal says went wrong, for a report an operator reads. */
  std::string detail_;
};

/**
 * Returns every instruction whose outcome nobody knows, in the order it was opened.
 *
 * Two kinds qualify, and the second is the one worth stating: a `PENDING` line with no answer, and
 * a `PENDING` line answered `INDETERMINATE`. An `ACCEPTED` or `REJECTED` answer resolves an entry
 * and it disappears from this list, because both are knowledge -- the order exists, or it does not.
 *
 * A terminal line with no `PENDING` predecessor can occur when a journal is read from a file whose
 * beginning was rotated or hand-edited away. An orphaned `INDETERMINATE` is still owed and is
 * reported; an orphaned `ACCEPTED` or `REJECTED` carries its own answer and is not.
 *
 * Linear in lines times open instructions rather than hash-based, because this runs once per order
 * list read over a file holding one session's instructions, and a `std::map` here would trade a
 * measurable allocation for an immeasurable saving.
 */
inline std::vector<OpenInstruction> unresolved_instructions(const JournalReplay& replay) {
  std::vector<OpenInstruction> open;
  for (const auto& entry : replay.entries_) {
    if (!journal_outcome_is_terminal(entry.outcome_)) {
      OpenInstruction instruction;
      instruction.entry_ = entry;
      instruction.opened_at_ = entry.recorded_at_;
      instruction.detail_ = entry.detail_;
      open.push_back(std::move(instruction));
      continue;
    }

    bool paired = false;
    for (auto at = open.begin(); at != open.end(); ++at) {
      if (at->entry_.id_ != entry.id_) {
        continue;
      }
      paired = true;
      if (entry.outcome_ == JournalOutcome::INDETERMINATE) {
        at->answered_ = true;
        at->closed_at_ = entry.recorded_at_;
        at->detail_ = entry.detail_;
        // The broker issued no reference, so the placement keeps the one its pending line had.
        at->entry_.order_id_ = entry.order_id_.empty() ? at->entry_.order_id_ : entry.order_id_;
      } else {
        open.erase(at);
      }
      break;
    }
    if (!paired && entry.outcome_ == JournalOutcome::INDETERMINATE) {
      OpenInstruction instruction;
      instruction.entry_ = entry;
      instruction.opened_at_ = entry.recorded_at_;
      instruction.closed_at_ = entry.recorded_at_;
      instruction.answered_ = true;
      instruction.detail_ = entry.detail_;
      open.push_back(std::move(instruction));
    }
  }
  return open;
}

/**
 * A journal that remembers nothing past the process, for tests and for dry runs.
 *
 * Useful precisely because it is *not* the durable one: every behaviour of the decorator, the
 * matcher and recovery can be driven through it with no filesystem at all, leaving `FileJournal`'s
 * own tests to cover only what a real file adds — flushing, `fsync`, and what a truncated line
 * looks like when a kernel rather than a test made it.
 *
 * It is also the honest default. A runner configured with this one journals into memory and loses
 * it on exit, which is exactly the pre-existing behaviour of `IndeterminacyLedger`; nothing is
 * silently promised.
 */
class MemoryJournal {
public:
  /** Records one line. Always succeeds: memory does not run out of disk. */
  bool append(const JournalEntry& entry) {
    entries_.push_back(entry);
    return true;
  }

  /** Returns everything recorded. Never reports damage: memory does not tear. */
  JournalReplay replay() const {
    JournalReplay replay;
    replay.entries_ = entries_;
    return replay;
  }

  /** Returns the lines recorded so far, in order. */
  const std::vector<JournalEntry>& records() const noexcept { return entries_; }

  /** Forgets everything, as a fresh process would. */
  void clear() noexcept { entries_.clear(); }

private:
  std::vector<JournalEntry> entries_;
};

/**
 * Wraps an executor so that every instruction is durably recorded before it is attempted.
 *
 * **The ordering is the feature.** Recording after the send would produce a file that is correct
 * about everything except the one case it exists for: a process killed while the request is on the
 * wire. So `place()` appends a `PENDING` line, waits for that line to be durable, and only then
 * calls the inner executor.
 *
 * **A decorator rather than a change to `Runner`.** `Runner::settle()` already holds the rules
 * about what an indeterminate outcome obliges, and those rules are tested by 454 tests that know
 * nothing about journals. Wrapping the executor adds the durability without touching any of it,
 * and it composes with `RecordingExecutor`, so every behaviour below is exercisable with no broker
 * and no network. It also means a **backtest journals nothing** — not because a flag disables it,
 * but because a backtest never wraps.
 *
 * The inner executor's own return value is passed through unchanged. Journaling observes; it never
 * decides. A journal that could change an outcome would be a second place where order safety is
 * implemented, and there is exactly one.
 *
 * **Except in one case, and it is the case that proves the rule.** If the `PENDING` line cannot be
 * made durable — a full disk, a revoked permission, a path that stopped existing — the send does
 * not happen. Sending anyway would produce the exact situation the journal exists to prevent, with
 * the added insult that the runner would believe itself protected. The instruction is reported as
 * `REJECTED`, which is the truthful classification: nothing was sent, so the order does not exist
 * and nothing needs reconciling. `last_journal_error()` says why, because a `REJECTED` with no
 * broker behind it is otherwise indistinguishable from a broker's refusal.
 */
template <typename Inner, typename Journal, typename Clock = SteadyClock>
class JournalingExecutor {
public:
  /**
   * Wraps `inner`, recording into `journal`.
   *
   * The journal is borrowed rather than owned, for the same reason a session is: recovery reads it
   * at startup, before any runner exists, and the same file is then appended to for the rest of the
   * process. One journal outlives the executor that writes to it.
   */
  JournalingExecutor(Inner inner, Journal& journal, Clock clock = {})
  : inner_(std::move(inner)), journal_(&journal), clock_(std::move(clock)) {
    prefix_ = default_prefix();
  }

  /**
   * Overrides the prefix that makes line ids unique.
   *
   * Ids must not collide across restarts, because a restarted process appends to the file its
   * predecessor wrote, and a repeated id would let a stale terminal line close a fresh `PENDING`
   * one. The default prefix is the microsecond the executor was constructed, which distinguishes
   * runs of a real process; a test that wants stable output sets its own.
   */
  void set_id_prefix(std::string prefix) { prefix_ = std::move(prefix); }

  /** Places one order, recording the attempt before making it. */
  SendOutcome place(const strategy::PlannedOrder& order) {
    JournalEntry entry;
    entry.id_ = next_id();
    entry.action_ = JournalAction::PLACE;
    entry.outcome_ = JournalOutcome::PENDING;
    entry.recorded_at_ = clock_.now();
    entry.instrument_ = order.instrument_;
    entry.side_ = order.side_;
    entry.type_ = order.type_;
    entry.quantity_ = order.quantity_;
    entry.price_ = order.price_;
    if (!journal_->append(entry)) {
      return refuse_unjournaled();
    }

    const auto outcome = inner_.place(order);

    entry.outcome_ = journal_outcome_of(outcome);
    entry.recorded_at_ = clock_.now();
    entry.order_id_ = accepted_order_id(outcome);
    entry.detail_ = failure_detail(outcome);
    journal_->append(entry);
    return outcome;
  }

  /**
   * Cancels one working order, recording the attempt before making it.
   *
   * Templated on the tag rather than fixed by the class, so the decorator needs no broker parameter
   * of its own and cannot be instantiated with one that disagrees with the executor it wraps.
   */
  template <typename BrokerTag>
  SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    JournalEntry entry;
    entry.id_ = next_id();
    entry.action_ = JournalAction::CANCEL;
    entry.outcome_ = JournalOutcome::PENDING;
    entry.recorded_at_ = clock_.now();
    entry.instrument_ = working.order_.instrument_;
    entry.side_ = working.order_.side_;
    entry.type_ = working.order_.type_;
    entry.quantity_ = working.order_.quantity_;
    entry.price_ = working.order_.price_;
    // Recorded from the start, unlike a placement's: a cancellation names an order that already
    // exists, and that name is what turns its recovery into an identity match rather than a guess.
    entry.order_id_ = working.ref_.primary_;
    if (!journal_->append(entry)) {
      return refuse_unjournaled();
    }

    const auto outcome = inner_.cancel(working);

    entry.outcome_ = journal_outcome_of(outcome);
    entry.recorded_at_ = clock_.now();
    entry.detail_ = failure_detail(outcome);
    journal_->append(entry);
    return outcome;
  }

  /** Returns the wrapped executor, so a caller can read what it was handed. */
  const Inner& inner() const noexcept { return inner_; }
  Inner& inner() noexcept { return inner_; }

  /** Returns the journal being written to. */
  const Journal& journal() const noexcept { return *journal_; }

  /**
   * Returns why the last instruction was refused without being sent, or empty when none was.
   *
   * Cleared by the next instruction that is journaled successfully, so it describes the most recent
   * attempt rather than accumulating. A caller that sees repeated refusals here is looking at a
   * storage problem, not a trading one, and should stop the runner rather than retry.
   */
  const std::string& last_journal_error() const noexcept { return journal_error_; }

  /** Returns how many instructions were refused because they could not be journaled. */
  std::size_t unjournaled_refusals() const noexcept { return refusals_; }

  /** Forwards the reference the broker issued for the last accepted order, where the inner has one. */
  template <typename Executor = Inner>
  auto last_ref() const noexcept -> decltype(std::declval<const Executor&>().last_ref()) {
    return inner_.last_ref();
  }

  /** Forwards why the last instruction failed, where the inner executor reports it. */
  template <typename Executor = Inner>
  auto last_error() const noexcept -> decltype(std::declval<const Executor&>().last_error()) {
    return inner_.last_error();
  }

private:
  /** Refuses an instruction that could not be recorded, without attempting it. */
  SendOutcome refuse_unjournaled() {
    ++refusals_;
    journal_error_ = "the order journal could not be written, so nothing was sent";
    return SendOutcome::REJECTED;
  }

  /** Returns a line id unique within this process and, by its prefix, across restarts. */
  std::string next_id() { return prefix_ + '-' + std::to_string(++sequence_); }

  /** Returns a prefix derived from the moment of construction. */
  std::string default_prefix() const {
    const auto since = clock_.now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since).count();
    return std::to_string(static_cast<std::int64_t>(micros));
  }

  /**
   * Returns the broker's id for an accepted order, where the inner executor reports one.
   *
   * Detected rather than required, because `RecordingExecutor` has no references to report and must
   * still be wrappable — it is what every journal test runs against.
   */
  std::string accepted_order_id(SendOutcome outcome) const {
    if (outcome != SendOutcome::ACCEPTED) {
      return {};
    }
    return accepted_order_id_impl(0);
  }

  template <typename Executor = Inner>
  auto accepted_order_id_impl(int) const
    -> decltype(std::declval<const Executor&>().last_ref(), std::string()) {
    const auto& ref = inner_.last_ref();
    return ref ? ref->primary_ : std::string();
  }

  std::string accepted_order_id_impl(...) const { return {}; }

  /** Returns what the inner executor said went wrong, where it says anything. */
  std::string failure_detail(SendOutcome outcome) const {
    if (outcome == SendOutcome::ACCEPTED) {
      return {};
    }
    return failure_detail_impl(0);
  }

  template <typename Executor = Inner>
  auto failure_detail_impl(int) const
    -> decltype(std::declval<const Executor&>().last_error(), std::string()) {
    const auto& error = inner_.last_error();
    return error ? error->message_ : std::string();
  }

  std::string failure_detail_impl(...) const { return {}; }

  Inner inner_;
  Journal* journal_;
  Clock clock_;
  std::string prefix_;
  std::string journal_error_;
  std::size_t refusals_{0};
  std::uint64_t sequence_{0};
};

} // namespace runner
} // namespace algotradelib
