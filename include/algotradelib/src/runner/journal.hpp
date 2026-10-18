#pragma once

#include "./clock.hpp"
#include "./execution.hpp"

#include "../strategy/reconcile.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * @brief Which kind of instruction a journal line records.
 *
 * Recovery resolves them differently: a cancellation names the broker's order id, a placement is
 * matched by its attributes.
 */
enum class JournalAction {
  /** @brief A new order. */
  PLACE,
  /** @brief A cancellation of a resting order. */
  CANCEL,
};

/** @brief Every `JournalAction`, in declaration order. */
inline constexpr std::array<JournalAction, 2> all_journal_actions{JournalAction::PLACE, JournalAction::CANCEL};

/**
 * @brief Returns an action's name as written to the journal (`"place"`, `"cancel"`).
 * @param action The action.
 * @return The name; empty for a value that is not a `JournalAction`.
 */
inline constexpr std::string_view journal_action_name(JournalAction action) noexcept {
  switch (action) {
    case JournalAction::PLACE:
      return "place";
    case JournalAction::CANCEL:
      return "cancel";
  }
  return "";
}

static_assert(
  lists_every_value(all_journal_actions, [](JournalAction action) { return !journal_action_name(action).empty(); }),
  "all_journal_actions must list every JournalAction"
);

/**
 * @brief Tests whether recovery matches an instruction of this action by the broker's order id it names,
 *   rather than by its attributes.
 * @param action The action.
 * @return `true` for `CANCEL` (it names the order it cancels); `false` for `PLACE` (matched by instrument,
 *   side, shape, quantity and time).
 */
inline constexpr bool journal_action_matches_by_order_id(JournalAction action) noexcept {
  switch (action) {
    case JournalAction::CANCEL:
      return true;
    case JournalAction::PLACE:
      return false;
  }
  return false;
}

/**
 * @brief Parses an action's name.
 * @param text The name.
 * @return The action, or `std::nullopt` if `text` names none.
 */
inline std::optional<JournalAction> journal_action_from(std::string_view text) noexcept {
  return value_named(all_journal_actions, journal_action_name, text);
}

/**
 * @brief What a journal line says about an instruction.
 *
 * `PENDING` is written before the instruction is sent; a later line with any other outcome answers
 * it. A `PENDING` line with no answer means the process stopped while the request may have been in
 * flight, so the order's existence is unknown.
 */
enum class JournalOutcome {
  /** @brief Written before the send; whether the instruction exists is not yet known. */
  PENDING,
  /** @brief The broker accepted the instruction; it exists. */
  ACCEPTED,
  /**
   * @brief The instruction was refused, by the broker or before it was sent, so it does not exist.
   *
   * The journal does not record who refused; a pass's report does.
   */
  REJECTED,
  /**
   * @brief The outcome is unknown (`SendOutcome::INDETERMINATE`: a failure in transit, an unreadable
   *   reply, an HTTP 5xx, or a broker refusal reported as `PROTOCOL`).
   */
  INDETERMINATE,
  /**
   * @brief A recovery sweep read the broker's order list and closed the question.
   *
   * `order_id_` is set if a matching order was found and empty if none was; `detail_` says what was
   * seen. For a cancellation, a match means the named order was in the listing read (resting only if
   * that listing held working orders alone); it does not mean the cancellation completed.
   */
  RESOLVED,
  /**
   * @brief A serialized runner accounted for the placement: holdings showed what it filled, its
   *   history showed it ended with nothing filled, or an operator released it.
   *
   * `baseline_` is the instrument's holding afterwards, where holdings showed it; `detail_` says which.
   * See `Runner::allow_overlapping_orders`.
   */
  CONFIRMED,
  /**
   * @brief Evidence contradicted the placement (holdings moved past what it can explain), so only an
   *   operator may account for it (`Runner::release_obligations`). Written with `requires_operator_`.
   */
  HELD,
};

/** @brief Every `JournalOutcome`, in declaration order. */
inline constexpr std::array<JournalOutcome, 7> all_journal_outcomes{
  JournalOutcome::PENDING,
  JournalOutcome::ACCEPTED,
  JournalOutcome::REJECTED,
  JournalOutcome::INDETERMINATE,
  JournalOutcome::RESOLVED,
  JournalOutcome::CONFIRMED,
  JournalOutcome::HELD,
};

/**
 * @brief Returns an outcome's name as written to the journal (`"pending"`, ...).
 * @param outcome The outcome.
 * @return The name; empty for a value that is not a `JournalOutcome`.
 */
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
    case JournalOutcome::RESOLVED:
      return "resolved";
    case JournalOutcome::CONFIRMED:
      return "confirmed";
    case JournalOutcome::HELD:
      return "held";
  }
  return "";
}

static_assert(
  lists_every_value(all_journal_outcomes, [](JournalOutcome outcome) { return !journal_outcome_name(outcome).empty(); }),
  "all_journal_outcomes must list every JournalOutcome"
);

/**
 * @brief Parses an outcome's name.
 * @param text The name.
 * @return The outcome, or `std::nullopt` if `text` names none.
 */
inline std::optional<JournalOutcome> journal_outcome_from(std::string_view text) noexcept {
  return value_named(all_journal_outcomes, journal_outcome_name, text);
}

/**
 * @brief Maps a send outcome to the journal outcome that records it.
 * @param outcome The send outcome.
 * @return `ACCEPTED`, `INDETERMINATE`, or `REJECTED` for `REJECTED`, `UNAVAILABLE` and
 *   `REFUSED_LOCALLY`. For a placement, `REJECTED` means the order does not exist; for a
 *   cancellation, only that the request was refused or not sent, which says nothing about the
 *   named order's state (it may have filled).
 */
inline constexpr JournalOutcome journal_outcome_of(SendOutcome outcome) noexcept {
  switch (outcome) {
    case SendOutcome::ACCEPTED:
      return JournalOutcome::ACCEPTED;
    case SendOutcome::REJECTED:
      return JournalOutcome::REJECTED;
    case SendOutcome::INDETERMINATE:
      return JournalOutcome::INDETERMINATE;
    case SendOutcome::UNAVAILABLE:
      // Nothing was sent, so the order does not exist.
      return JournalOutcome::REJECTED;
    case SendOutcome::REFUSED_LOCALLY:
      // Nothing was sent, so the order does not exist.
      return JournalOutcome::REJECTED;
  }
  return JournalOutcome::INDETERMINATE;
}

/**
 * @brief Tests whether an outcome answers a `PENDING` line.
 * @param outcome The outcome.
 * @return `true` for every outcome but `PENDING`.
 */
inline constexpr bool journal_outcome_is_terminal(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::PENDING:
      return false;
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::REJECTED:
    case JournalOutcome::INDETERMINATE:
    case JournalOutcome::RESOLVED:
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return true;
  }
  return true;
}

/**
 * @brief Tests whether a line with this outcome answers the send itself (the broker's reply, or its
 *   absence), rather than a later account of it.
 * @param outcome The outcome.
 * @return `true` for `ACCEPTED`, `REJECTED` and `INDETERMINATE`.
 */
inline constexpr bool journal_outcome_answers_send(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::REJECTED:
    case JournalOutcome::INDETERMINATE:
      return true;
    case JournalOutcome::PENDING:
    case JournalOutcome::RESOLVED:
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return false;
  }
  return false;
}

/**
 * @brief Tests whether a line with this outcome may carry the broker's id for the order.
 * @param outcome The outcome.
 * @return `true` for `ACCEPTED` and `RESOLVED`.
 */
inline constexpr bool journal_outcome_names_order(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::RESOLVED:
      return true;
    case JournalOutcome::PENDING:
    case JournalOutcome::REJECTED:
    case JournalOutcome::INDETERMINATE:
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return false;
  }
  return false;
}

/**
 * @brief Tests whether a line with this outcome says the instruction was refused, so it does not exist.
 * @param outcome The outcome.
 * @return `true` for `REJECTED` only.
 */
inline constexpr bool journal_outcome_refuses(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::REJECTED:
      return true;
    case JournalOutcome::PENDING:
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::INDETERMINATE:
    case JournalOutcome::RESOLVED:
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return false;
  }
  return false;
}

/**
 * @brief Tests whether a line with this outcome is a serialized runner's closing account of a placement
 *   (it confirmed it, or held it for an operator), which repeats none of the instruction's shape.
 * @param outcome The outcome.
 * @return `true` for `CONFIRMED` and `HELD`.
 */
inline constexpr bool journal_outcome_is_closing_account(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return true;
    case JournalOutcome::PENDING:
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::REJECTED:
    case JournalOutcome::INDETERMINATE:
    case JournalOutcome::RESOLVED:
      return false;
  }
  return false;
}

/**
 * @brief Tests whether a line with this outcome is written with `requires_operator_` by design, so the flag
 *   is not damage.
 * @param outcome The outcome.
 * @return `true` for `HELD` only.
 */
inline constexpr bool journal_outcome_requires_operator(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::HELD:
      return true;
    case JournalOutcome::PENDING:
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::REJECTED:
    case JournalOutcome::INDETERMINATE:
    case JournalOutcome::RESOLVED:
    case JournalOutcome::CONFIRMED:
      return false;
  }
  return false;
}

/**
 * @brief Tests whether a line with this outcome leaves its instruction's fate unknown, so recovery must
 *   still settle it.
 * @param outcome The outcome.
 * @return `true` for `PENDING` (not answered yet) and `INDETERMINATE` (answered, outcome unknown);
 *   `false` for every outcome that settles it.
 */
inline constexpr bool journal_outcome_leaves_open(JournalOutcome outcome) noexcept {
  switch (outcome) {
    case JournalOutcome::PENDING:
    case JournalOutcome::INDETERMINATE:
      return true;
    case JournalOutcome::ACCEPTED:
    case JournalOutcome::REJECTED:
    case JournalOutcome::RESOLVED:
    case JournalOutcome::CONFIRMED:
    case JournalOutcome::HELD:
      return false;
  }
  return true;
}

/**
 * @brief One journal line: an instruction and what was known about it when the line was written.
 *
 * Every line repeats the whole instruction, so an answering line is readable on its own (with
 * `tail`) and survives the loss of its `PENDING` line. `recorded_at_` is the executor's clock (this
 * machine's, unless one was injected), not the broker's.
 */
struct JournalEntry {
  /**
   * @brief Links an answering line to its `PENDING` line.
   *
   * Generated locally; it is not an idempotency key and no broker has seen it.
   */
  std::string id_;
  /** @brief Whether the instruction is a placement or a cancellation. */
  JournalAction action_{JournalAction::PLACE};
  /** @brief What the line says about the instruction. */
  JournalOutcome outcome_{JournalOutcome::PENDING};
  /** @brief When the line was written (for `PENDING`, before the request went out). */
  std::chrono::system_clock::time_point recorded_at_{};
  /** @brief The instrument (identity; the routing venue is `venue_`). */
  InstrumentId instrument_;
  /** @brief The side. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The order type; recovery matches on type, time in force and session together. */
  OrderType type_{OrderType::LIMIT};
  /** @brief The time in force. */
  TimeInForce time_in_force_{TimeInForce::DAY};
  /** @brief The session. */
  OrderSession session_{OrderSession::REGULAR};
  /**
   * @brief The venue the order actually goes to, as the binding resolves it.
   *
   * `UNSPECIFIED` is a recorded fact (no venue was asked for, as always at Toss). Empty means the
   * route cannot be known from the line (an older line, an unrecognized value, or a damaged line);
   * recovery then reports a lookalike row as ambiguous rather than matching it.
   */
  std::optional<Venue> venue_{Venue::UNSPECIFIED};
  /** @brief The quantity, which recovery matches against. */
  Decimal64 quantity_;
  /** @brief The price, if the order has one. */
  std::optional<Decimal64> price_;
  /** @brief The trigger price of a stop-limit order; empty otherwise (and on lines that predate it). */
  std::optional<Decimal64> trigger_price_;
  /**
   * @brief On a serialized runner's placement, the instrument's net holding the order was decided
   *   against; on a `CONFIRMED` line, the holding afterwards, where holdings showed it.
   *
   * Empty on every line of an overlapping (or older) runner, which is how recovery tells them apart.
   */
  std::optional<Decimal64> baseline_;
  /**
   * @brief The broker's order id, if known: from the start on a `CANCEL` line, and only on an
   *   answering line for a `PLACE`.
   */
  std::string order_id_;
  /** @brief Why an answering line says what it says, for the operator. */
  std::string detail_;
  /**
   * @brief Whether only an operator may resolve this instruction.
   *
   * Set on an instruction recovered from a damaged line, whose matching fields are missing; a
   * sweep never resolves it. Written to the file, so it survives repair.
   */
  bool requires_operator_{false};
  /**
   * @brief The broker and account the instruction belongs to, such as `"kis:live:12345678-01"`.
   *
   * Opaque to the library; it keeps one account's instruction from being matched against another
   * account's orders. Stamped by the journal.
   */
  std::string scope_;
};

/**
 * @brief Whether a journal file could be read.
 *
 * A missing file means a first start with nothing owed; an unreadable one may hold orders left in
 * flight, and must not be treated as empty.
 */
enum class JournalReadStatus {
  /** @brief No such file: a first start, nothing owed. */
  MISSING,
  /** @brief The file was read in full. */
  READ,
  /** @brief The file exists and could not be read; its contents are unknown. */
  UNREADABLE,
};

/**
 * @brief Returns a status's lowercase name, such as `"unreadable"`.
 * @param status The status.
 * @return The name.
 */
inline constexpr std::string_view journal_read_status_name(JournalReadStatus status) noexcept {
  switch (status) {
    case JournalReadStatus::MISSING:
      return "missing";
    case JournalReadStatus::READ:
      return "read";
    case JournalReadStatus::UNREADABLE:
      return "unreadable";
  }
  return "unknown";
}

/**
 * @brief The contents of a journal, including what could not be read.
 *
 * Damage is returned as data: a torn last line is evidence that something was in flight.
 */
struct JournalReplay {
  /** @brief Every line that parsed, in file order. */
  std::vector<JournalEntry> entries_;
  /** @brief How many lines did not parse (on an append-only file, writes that did not finish). */
  std::size_t unreadable_lines_{0};
  /** @brief Whether the last line was incomplete, as after a kill during a write. */
  bool truncated_tail_{false};
  /** @brief What was wrong, for the operator. */
  std::string detail_;
  /** @brief The broker and account the file's header declares, if it has one. */
  std::string scope_;
  /** @brief Whether a header was found; a non-empty file without one is not written to. */
  bool has_header_{false};
  /** @brief Whether the file was read; `READ` for an in-memory journal. */
  JournalReadStatus status_{JournalReadStatus::READ};

  /**
   * @brief Tests whether the file could be read.
   * @return `false` only for `UNREADABLE`.
   */
  bool readable() const noexcept {
    switch (status_) {
      case JournalReadStatus::MISSING:
      case JournalReadStatus::READ:
        return true;
      case JournalReadStatus::UNREADABLE:
        return false;
    }
    return false;
  }

  /**
   * @brief Tests whether everything was read.
   * @return `true` if readable, with no unparsed lines and no torn last line.
   */
  bool intact() const noexcept {
    return readable() && unreadable_lines_ == 0 && !truncated_tail_;
  }
};

/**
 * @brief One instruction of unknown outcome, and the span in which it may have reached the broker.
 *
 * Either a `PENDING` line with no answer (the span's end is unknown), or one answered
 * `INDETERMINATE` (the two lines bracket the send exactly).
 */
struct OpenInstruction {
  /** @brief The instruction, as its first line recorded it (its `order_id_` may come from a later line). */
  JournalEntry entry_;
  /** @brief When the send began. */
  std::chrono::system_clock::time_point opened_at_{};
  /**
   * @brief When the send was answered: by an `INDETERMINATE` line in `unresolved_instructions`, by any
   *   answer in `recorded_placements`.
   */
  std::optional<std::chrono::system_clock::time_point> closed_at_;
  /** @brief Whether an `INDETERMINATE` line answered it. */
  bool answered_{false};
  /** @brief What the journal says went wrong. */
  std::string detail_;
};

/**
 * @brief Lists the instructions whose outcome is unknown, in the order they were opened.
 *
 * A `PENDING` line with no answer, or one answered `INDETERMINATE`, is listed; `ACCEPTED` and
 * `REJECTED` answers resolve it. An answer is paired with the first open line carrying its id, so
 * ids must be unique (see `JournalingExecutor::set_id_prefix`). An `INDETERMINATE` line whose `PENDING` line is missing (a rotated or
 * edited file) is listed too.
 *
 * @param replay The journal's contents.
 * @return The unresolved instructions.
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
      if (journal_outcome_leaves_open(entry.outcome_)) {
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
    if (!paired && journal_outcome_leaves_open(entry.outcome_)) {
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
 * @brief Returns the placements with the given journal ids, each with the span in which it may have
 *   reached the broker, whatever was written about them since.
 *
 * For a placement a sweep closed without finding its order and that a serialized runner still owes
 * (`Runner::obligations`): `unresolved_instructions` no longer lists it, and this gives a later sweep
 * the same instruction and span to match against.
 *
 * @param replay The journal's contents.
 * @param ids The journal ids wanted.
 * @return One instruction per id that has a `PENDING` or `INDETERMINATE` placement line, in file order;
 *   one recovered from a damaged line is left out.
 */
inline std::vector<OpenInstruction> placements_of(const JournalReplay& replay, const std::vector<std::string>& ids) {
  std::vector<OpenInstruction> found;
  for (const auto& entry : replay.entries_) {
    if (entry.action_ != JournalAction::PLACE || entry.requires_operator_ ||
        !journal_outcome_leaves_open(entry.outcome_) ||
        std::find(ids.begin(), ids.end(), entry.id_) == ids.end()) {
      continue;
    }
    auto known = std::find_if(found.begin(), found.end(), [&](const OpenInstruction& instruction) {
      return instruction.entry_.id_ == entry.id_;
    });
    if (known == found.end()) {
      OpenInstruction instruction;
      instruction.entry_ = entry;
      instruction.opened_at_ = entry.recorded_at_;
      instruction.detail_ = entry.detail_;
      found.push_back(std::move(instruction));
      known = std::prev(found.end());
    }
    if (journal_outcome_is_terminal(entry.outcome_)) {
      known->answered_ = true;
      known->closed_at_ = entry.recorded_at_;
      known->detail_ = entry.detail_;
    }
  }
  return found;
}

/**
 * @brief A journal held in memory, lost when the process exits; for tests and dry runs.
 */
class MemoryJournal {
public:
  /** @brief Constructs a journal with an empty scope. */
  MemoryJournal() = default;

  /**
   * @brief Constructs a journal.
   * @param scope The broker and account stamped on each line that has none.
   */
  explicit MemoryJournal(std::string scope) : scope_(std::move(scope)) {}

  /**
   * @brief Appends a line.
   * @param entry The line.
   * @return `true`.
   */
  bool append(const JournalEntry& entry) {
    entries_.push_back(entry);
    if (entries_.back().scope_.empty()) {
      entries_.back().scope_ = scope_;
    }
    return true;
  }

  /**
   * @brief Returns everything recorded.
   * @return The lines, with no damage.
   */
  JournalReplay replay() const {
    JournalReplay replay;
    replay.entries_ = entries_;
    return replay;
  }

  /**
   * @brief Returns the lines recorded.
   * @return The lines, in order.
   */
  const std::vector<JournalEntry>& records() const noexcept { return entries_; }

  /** @brief Forgets every line. */
  void clear() noexcept { entries_.clear(); }

  /**
   * @brief Returns the journal's broker and account.
   * @return The scope.
   */
  const std::string& scope() const noexcept { return scope_; }

private:
  std::vector<JournalEntry> entries_;
  std::string scope_;
};

/**
 * @brief An executor wrapper that records each instruction in a journal before sending it.
 *
 * `place` and `cancel` append a `PENDING` line, call the wrapped executor only if the append
 * succeeded, and append its outcome. The line is as durable as the journal makes it before
 * `append` returns: `FileJournal` flushes it to disk, `MemoryJournal` (for tests and dry runs)
 * loses it at exit. A journal of your own must write durably before returning success if it is to
 * be recovered from after a crash. The wrapped executor's outcome is returned unchanged, except
 * that **if the `PENDING` line cannot be written, nothing is sent** and the outcome is
 * `SendOutcome::UNAVAILABLE` (which stops the runner). If only the answering line fails, the send has
 * happened; the failure is recorded and the instruction will look unresolved to recovery.
 * A backtest does not use it.
 *
 * @tparam Inner The wrapped executor.
 * @tparam Journal The journal type (`MemoryJournal` or `FileJournal`).
 * @tparam Clock The clock lines are stamped with.
 */
template <typename Inner, typename Journal, typename Clock = SteadyClock>
class JournalingExecutor {
public:
  /** @brief Whether the wrapped executor reaches a broker. */
  static constexpr bool reaches_a_broker = runner_detail::sends_to_a_broker<Inner>::value;

  /**
   * @brief Wraps an executor.
   * @param inner The executor to wrap.
   * @param journal The journal; borrowed, and must outlive this executor.
   * @param clock The clock lines are stamped with; pass a `ClockReference` to share a loop's clock.
   */
  JournalingExecutor(Inner inner, Journal& journal, Clock clock = {})
  : inner_(std::move(inner)), journal_(&journal), clock_(std::move(clock)) {
    prefix_ = default_prefix();
  }

  /**
   * @brief Sets the prefix of line ids.
   *
   * Ids are the prefix, a hyphen and a counter starting at one (`<prefix>-1`), and recovery pairs lines by id, so the prefix
   * must differ between every executor run that shares a journal. The default is the wall-clock
   * construction time in microseconds (not the injected clock's), which is distinct only if no two
   * executors on the journal are made at the same instant.
   *
   * @param prefix The prefix.
   */
  void set_id_prefix(std::string prefix) { prefix_ = std::move(prefix); }

  /**
   * @brief Returns the venue the wrapped executor says an order will reach.
   *
   * Call it as `route_of(order, 0)`; the second argument selects this overload over the fallback,
   * and its value is ignored.
   *
   * @tparam E The wrapped executor type (deduced).
   * @param order The order.
   * @return The wrapped executor's `effective_route` for the order's venue.
   */
  template <typename E = Inner>
  auto route_of(const strategy::PlannedOrder& order, int) const
    -> decltype(std::declval<const E&>().effective_route(order.venue_)) {
    return inner_.effective_route(order.venue_);
  }

  /**
   * @brief Returns the venue an order asked for, when the wrapped executor cannot resolve routes
   *   (the fallback of `route_of(order, 0)`; the second argument's value is ignored).
   * @param order The order.
   * @return `order.venue_`.
   */
  Venue route_of(const strategy::PlannedOrder& order, long) const { return order.venue_; }

  /**
   * @brief Returns the venue to record for an order: the binding's route, or the venue asked for.
   * @param order The order.
   * @return The venue.
   */
  Venue effective_route_of(const strategy::PlannedOrder& order) const {
    return route_of(order, 0);
  }

  /**
   * @brief Records and places one order.
   * @param order The order.
   * @return The wrapped executor's outcome, or `UNAVAILABLE` if the `PENDING` line could not be written.
   */
  SendOutcome place(const strategy::PlannedOrder& order) { return place(order, std::nullopt); }

  /**
   * @brief Records and places one order, stating the holding it was decided against (a serialized
   *   `Runner` calls this).
   * @param order The order.
   * @param baseline The instrument's net holding, written on every line for the order.
   * @return The wrapped executor's outcome, or `UNAVAILABLE` if the `PENDING` line could not be written.
   */
  SendOutcome place(const strategy::PlannedOrder& order, std::optional<Decimal64> baseline) {
    JournalEntry entry;
    entry.id_ = next_id();
    entry.action_ = JournalAction::PLACE;
    entry.outcome_ = JournalOutcome::PENDING;
    entry.recorded_at_ = clock_.now();
    entry.instrument_ = order.instrument_;
    entry.side_ = order.side_;
    entry.type_ = order.type_;
    entry.time_in_force_ = order.time_in_force_;
    entry.session_ = order.session_;
    // Where the order will actually go (an unrouted KIS order reaches KRX), asked of the binding;
    // a pure function of the order, so the line is still written before any I/O.
    entry.venue_ = effective_route_of(order);
    entry.quantity_ = order.quantity_;
    entry.price_ = order.price_;
    entry.trigger_price_ = order.trigger_price_;
    entry.baseline_ = std::move(baseline);
    if (!journal_->append(entry)) {
      return refuse_unjournaled();
    }

    const auto outcome = inner_.place(order);

    entry.outcome_ = journal_outcome_of(outcome);
    entry.recorded_at_ = clock_.now();
    entry.order_id_ = accepted_order_id(outcome);
    entry.detail_ = failure_detail(outcome);
    last_entry_ = entry;
    if (!journal_->append(entry)) {
      // The send happened; only its answer is lost, so recovery will see it unresolved. Recorded so
      // the runner stops.
      note_failure("the order journal could not record an instruction's outcome");
    }
    return outcome;
  }

  /**
   * @brief Returns the answering line last written (or attempted) for a placement the wrapped executor
   *   was asked to place, sent or refused before sending.
   * @return The line, or `std::nullopt` before any placement was asked for.
   */
  const std::optional<JournalEntry>& last_entry() const noexcept { return last_entry_; }

  /**
   * @brief Appends a line the caller built, such as a serialized `Runner`'s `CONFIRMED` line.
   *
   * A failure is counted like any other journal failure (`journal_failures`).
   *
   * @param entry The line. It is stamped here with this executor's clock (`recorded_at_`), and its
   *   scope by the journal.
   * @return Whether it was written.
   */
  bool record(JournalEntry& entry) {
    entry.recorded_at_ = clock_.now();
    if (journal_->append(entry)) {
      return true;
    }
    note_failure("the order journal could not record that a placement was accounted for");
    return false;
  }

  /**
   * @brief Records and sends one cancellation.
   * @tparam BrokerTag The broker tag.
   * @param working The order to cancel.
   * @return The wrapped executor's outcome, or `UNAVAILABLE` if the `PENDING` line could not be written.
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
    entry.trigger_price_ = working.order_.trigger_price_;
    // Known from the start: a cancellation names an existing order.
    entry.order_id_ = working.ref_.primary_;
    if (!journal_->append(entry)) {
      return refuse_unjournaled();
    }

    const auto outcome = inner_.cancel(working);

    entry.outcome_ = journal_outcome_of(outcome);
    entry.recorded_at_ = clock_.now();
    entry.detail_ = failure_detail(outcome);
    if (!journal_->append(entry)) {
      note_failure("the order journal could not record an instruction's outcome");
    }
    return outcome;
  }

  /**
   * @brief Returns the wrapped executor.
   * @return The executor.
   */
  const Inner& inner() const noexcept { return inner_; }
  /**
   * @brief Returns the wrapped executor.
   * @return The executor.
   */
  Inner& inner() noexcept { return inner_; }

  /**
   * @brief Returns the journal.
   * @return The journal.
   */
  const Journal& journal() const noexcept { return *journal_; }

  /**
   * @brief Returns the first journal failure's description.
   * @return The description, or empty if no write has failed; never cleared.
   */
  const std::string& last_journal_error() const noexcept { return journal_error_; }

  /**
   * @brief Returns how many instructions were not sent because their `PENDING` line could not be written.
   * @return The count.
   */
  std::size_t unjournaled_refusals() const noexcept { return refusals_; }

  /**
   * @brief Returns how many journal writes have failed; `Runner` compares it with the count it last
   *   acknowledged to tell a new failure from an old one.
   * @return The count.
   */
  std::size_t journal_failures() const noexcept { return failures_; }

  /**
   * @brief Returns the wrapped executor's route for a requested venue.
   * @tparam Executor The wrapped executor type (deduced).
   * @param asked The venue asked for.
   * @return The venue it routes to.
   */
  template <typename Executor = Inner>
  auto requested_route(Venue asked) const noexcept
    -> decltype(std::declval<const Executor&>().requested_route(asked)) {
    return inner_.requested_route(asked);
  }

  /**
   * @brief Returns the reference of the last order the wrapped executor placed.
   * @tparam Executor The wrapped executor type (deduced).
   * @return The reference, if the wrapped executor reports one.
   */
  template <typename Executor = Inner>
  auto last_ref() const noexcept -> decltype(std::declval<const Executor&>().last_ref()) {
    return inner_.last_ref();
  }

  /**
   * @brief Returns why the last instruction failed, from the wrapped executor.
   * @tparam Executor The wrapped executor type (deduced).
   * @return The error, if the wrapped executor reports one.
   */
  template <typename Executor = Inner>
  auto last_error() const noexcept -> decltype(std::declval<const Executor&>().last_error()) {
    return inner_.last_error();
  }

private:
  // Refuses an instruction that could not be recorded, without attempting it.
  SendOutcome refuse_unjournaled() {
    ++refusals_;
    note_failure("the order journal could not be written, so nothing was sent");
    return SendOutcome::UNAVAILABLE;
  }

  // Records why the journal failed, keeping the first reason and counting every failure.
  //
  // The two halves answer different questions. The reason is for a person reading a report, and the
  // first one is the useful one. The count is for the runner, which needs to know whether anything
  // has gone wrong *since* it last acknowledged a problem.
  void note_failure(std::string detail) {
    ++failures_;
    if (journal_error_.empty()) {
      journal_error_ = std::move(detail);
    }
  }

  // Returns a line id unique within this process and, by its prefix, across restarts.
  std::string next_id() { return prefix_ + '-' + std::to_string(++sequence_); }

  // Returns a prefix derived from the moment of construction, on the wall clock rather than `clock_`:
  // the prefix tells process runs apart, and an injected clock (a test's, a replay's) may start two
  // runs at the same instant.
  std::string default_prefix() const {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since).count();
    return std::to_string(static_cast<std::int64_t>(micros));
  }

  // Returns the broker's id for an accepted order, where the inner executor reports one.
  //
  // Detected rather than required, because `RecordingExecutor` has no references to report and must
  // still be wrappable — it is what every journal test runs against.
  std::string accepted_order_id(SendOutcome outcome) const {
    switch (outcome) {
      case SendOutcome::ACCEPTED:
        return accepted_order_id_impl(0);
      case SendOutcome::REJECTED:
      case SendOutcome::INDETERMINATE:
      case SendOutcome::UNAVAILABLE:
      case SendOutcome::REFUSED_LOCALLY:
        return {};
    }
    return {};
  }

  template <typename Executor = Inner>
  auto accepted_order_id_impl(int) const
    -> decltype(std::declval<const Executor&>().last_ref(), std::string()) {
    const auto& ref = inner_.last_ref();
    return ref ? ref->primary_ : std::string();
  }

  std::string accepted_order_id_impl(...) const { return {}; }

  // Returns what the inner executor said went wrong, where it says anything.
  std::string failure_detail(SendOutcome outcome) const {
    switch (outcome) {
      case SendOutcome::ACCEPTED:
        return {};
      case SendOutcome::REJECTED:
      case SendOutcome::INDETERMINATE:
      case SendOutcome::UNAVAILABLE:
      case SendOutcome::REFUSED_LOCALLY:
        return failure_detail_impl(0);
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
  std::optional<JournalEntry> last_entry_;
  std::string prefix_;
  std::string journal_error_;
  std::size_t refusals_{0};
  std::size_t failures_{0};
  std::uint64_t sequence_{0};
};

} // namespace runner
} // namespace algotradelib
