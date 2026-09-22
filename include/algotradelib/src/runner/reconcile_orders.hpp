#pragma once

#include "./execution.hpp"
#include "./journal.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * What the broker's own order list says about one instruction the journal cannot account for.
 *
 * Four values rather than two, and the pair most easily conflated is `NOT_PLACED` and `UNDECIDED`.
 * Getting that one wrong is how a live order goes unwatched: the absence of a row is evidence of
 * absence only *after* enough time has passed for the broker's listing to reflect a write it
 * accepted. Until then, absence means nothing at all.
 */
enum class MatchVerdict {
  /** Exactly one row corresponds. The instruction's outcome is known. */
  MATCHED,
  /**
   * No row corresponds and the window has closed. The order does not exist.
   *
   * On a cancellation this reads oddly and means the right thing: the order the instruction named
   * is no longer in the listing, so it is not resting, which is the state the question was about.
   */
  NOT_PLACED,
  /**
   * More than one row could correspond.
   *
   * **Report, never guess.** Resolving on a coin flip would either leave a live order unwatched or
   * double a position, and no amount of heuristic refinement turns two indistinguishable rows into
   * one answer. The instrument stays withheld until an operator says which it was.
   */
  AMBIGUOUS,
  /** No row yet, and the window is still open. Wait for the next sweep. */
  UNDECIDED,
  /**
   * The instruction came from a damaged journal line, so no sweep may decide it.
   *
   * Not a weaker `AMBIGUOUS`. An ambiguous instruction has too many candidate rows; this one has
   * untrustworthy *criteria* -- a salvaged line has no recorded timestamp, and may be missing side,
   * type, quantity or price. Matching on what survived would be matching on a guess, and the window
   * arithmetic would resolve it to `NOT_PLACED` immediately, which is the unsafe direction.
   */
  NEEDS_OPERATOR,
};

/** Returns a stable lowercase name for `verdict`, for logs and structured reporting. */
inline constexpr std::string_view match_verdict_name(MatchVerdict verdict) noexcept {
  switch (verdict) {
    case MatchVerdict::MATCHED:
      return "matched";
    case MatchVerdict::NOT_PLACED:
      return "not_placed";
    case MatchVerdict::AMBIGUOUS:
      return "ambiguous";
    case MatchVerdict::UNDECIDED:
      return "undecided";
    case MatchVerdict::NEEDS_OPERATOR:
      return "needs_operator";
  }
  return "unknown";
}

/** Returns whether `verdict` settles the question, so the instrument may trade again. */
inline constexpr bool match_verdict_resolves(MatchVerdict verdict) noexcept {
  return verdict == MatchVerdict::MATCHED || verdict == MatchVerdict::NOT_PLACED;
}

/**
 * How much slack attribute matching allows, in three units rather than one number.
 *
 * They are three because they absorb three unrelated things, and a single "tolerance" would have to
 * be set to the largest of them — widening the window for reasons that do not apply.
 *
 * **`clock_skew_` is about whose clock.** A journal entry is stamped by the runner's clock; an
 * `OrderSnapshot::ordered_at_` is stamped by the broker's. The two agree only as well as NTP makes
 * them agree, and KIS reports `HHMMSS` in its own market's day. A narrow allowance here does not
 * fail loudly — it silently reports `NOT_PLACED` for an order that exists, which is the worst
 * outcome in this whole design. The default is generous on purpose.
 *
 * **`maximum_request_` is about how long the bytes could have been in the air.** It bounds the span
 * of an instruction whose `PENDING` line was never answered, because the process died before it
 * could be. Where a terminal line *does* exist, the span is exact and this is not used.
 *
 * **`listing_delay_` is about the broker's own bookkeeping.** An accepted write does not appear in
 * an inquiry instantly. This is how long to keep saying `UNDECIDED` before concluding that a row
 * which has not appeared is not going to.
 */
struct MatchWindow {
  /** How far the broker's clock may differ from this one, in either direction. */
  std::chrono::seconds clock_skew_{30};
  /** How long a request could have been outstanding when no terminal line says otherwise. */
  std::chrono::seconds maximum_request_{60};
  /** How long the broker's listing may lag a write it accepted. */
  std::chrono::seconds listing_delay_{30};
};

/** One instruction's verdict, and enough context for an operator to act on it. */
struct OrderMatch {
  MatchVerdict verdict_{MatchVerdict::UNDECIDED};
  /** The instruction this answers. */
  InstrumentId instrument_;
  /** The journal line id, so a report points at the file. */
  std::string entry_id_;
  /** The broker's order id, on `MATCHED`. */
  std::string order_id_;
  /** How many rows could correspond. Above one on `AMBIGUOUS`, zero on the other two. */
  std::size_t candidates_{0};
  /** What was decided and why, in a sentence an operator reads. */
  std::string detail_;
};

namespace reconcile_detail {

/**
 * Returns whether two decimals denote the same number.
 *
 * Not `operator==`, which compares the coefficient *and the scale*, so a broker reporting `10.00`
 * where the journal wrote `10` would not match. That is correct behaviour for a value type whose
 * scale is part of what a broker said, and exactly wrong for a quantity comparison.
 */
inline bool same_value(const Decimal64& lhs, const Decimal64& rhs) {
  return !decimal_less(lhs, rhs) && !decimal_less(rhs, lhs);
}

/** Returns whether two optional decimals are both absent or denote the same number. */
inline bool same_value(
  const std::optional<Decimal64>& lhs, const std::optional<Decimal64>& rhs
) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return lhs.has_value() == rhs.has_value();
  }
  return same_value(*lhs, *rhs);
}

} // namespace reconcile_detail

/**
 * Returns the last instant at which `instruction` could have reached the broker.
 *
 * Exact where a terminal line bracketed the send, and bounded by `maximum_request_` where the
 * process died before one could be written.
 */
inline std::chrono::system_clock::time_point instruction_sent_by(
  const OpenInstruction& instruction, const MatchWindow& window
) {
  return instruction.closed_at_ ?
    *instruction.closed_at_ :
    instruction.opened_at_ + window.maximum_request_;
}

/**
 * Returns whether enough time has passed that a row which has not appeared is not going to.
 *
 * The one predicate standing between `UNDECIDED` and `NOT_PLACED`, and therefore between "keep
 * watching" and "declare the order never existed".
 */
inline bool match_window_closed(
  const OpenInstruction& instruction,
  std::chrono::system_clock::time_point now,
  const MatchWindow& window
) {
  return now >= instruction_sent_by(instruction, window) + window.clock_skew_ +
    window.listing_delay_;
}

/**
 * Returns whether `row` could be the order `instruction` describes.
 *
 * **Attributes, because there is nothing better, on either broker.** The plan for this work
 * expected to branch here on `BrokerCaps::has_client_order_id` and match on that key where it
 * exists — an identity, needing no window and admitting no ambiguity. That branch cannot be
 * written, and the reason is worth recording so nobody tries again: Toss accepts a
 * `clientOrderId` on the order POST and echoes it on the **acknowledgement**, but its order
 * *listing* row carries no such field (`components/schemas/Order` in the published specification
 * has `orderId` and no client id). KIS has no equivalent at all outside the corporate-only
 * `gt_uid`. So the key is an idempotency guarantee on the write path, not a recovery handle on the
 * read path, and every instruction needing a match is one for which no identity was ever returned.
 *
 * `ordered_at_` is the broker's clock and the instruction's span is this one's, so the comparison
 * is widened by `clock_skew_` at both ends. A row with no timestamp at all is admitted on
 * attributes alone rather than excluded: excluding it would report `NOT_PLACED` for an order that
 * is sitting in the listing.
 */
inline bool row_corresponds(
  const OpenInstruction& instruction, const OrderSnapshot& row, const MatchWindow& window
) {
  const auto& entry = instruction.entry_;
  if (!(row.instrument_ == entry.instrument_)) {
    return false;
  }
  if (row.side_ != entry.side_ || row.type_ != entry.type_) {
    return false;
  }
  if (!reconcile_detail::same_value(row.quantity_, entry.quantity_)) {
    return false;
  }
  if (!reconcile_detail::same_value(row.price_, entry.price_)) {
    return false;
  }
  if (!row.ordered_at_) {
    return true;
  }
  const auto begin = instruction.opened_at_ - window.clock_skew_;
  const auto end = instruction_sent_by(instruction, window) + window.clock_skew_;
  return *row.ordered_at_ >= begin && *row.ordered_at_ <= end;
}

/**
 * Decides what the broker's order list says about one unresolved instruction.
 *
 * **One matcher serves every broker.** Both adapters already normalize their listings into
 * `OrderSnapshot`, which carries the instrument, side, type, quantity, price and `ordered_at_` this
 * needs, and both populate `ordered_at_`. So the per-broker field lists this feature was specified
 * against — KIS's `pdno` / `sll_buy_dvsn_cd` / `ord_tmd`, Toss's `symbol` / `side` / `createdAt` —
 * are documentation of what the normalizer preserves, not a specification for this code.
 *
 * **A cancellation is matched by identity, a placement by attributes.** That asymmetry is not a
 * preference; it is what the two instructions know. A cancellation names an order that already
 * exists, so its journal line carries the broker's own id from the moment it is written, and at
 * most one row can carry that id — `AMBIGUOUS` is unreachable on this path, and a test asserts it
 * rather than assuming it. A placement has no such handle, because no broker returns one in a
 * listing.
 *
 * **Second granularity is already mitigated, and it is worth knowing why.** KIS reports `ord_tmd`
 * as `HHMMSS`, so two identical orders in the same second would be indistinguishable. They cannot
 * both be outstanding: once an instrument is marked, `IndeterminacyLedger::requires_reconcile`
 * withholds *everything* for it, so a second identical order cannot be sent while the first is
 * unresolved. The serialization this matcher would otherwise need already exists, as a consequence
 * of the withholding rule rather than as something added for it.
 */
inline OrderMatch match_against(
  const OpenInstruction& instruction,
  const std::vector<OrderSnapshot>& rows,
  std::chrono::system_clock::time_point now,
  const MatchWindow& window = {}
) {
  OrderMatch match;
  match.instrument_ = instruction.entry_.instrument_;
  match.entry_id_ = instruction.entry_.id_;

  if (instruction.entry_.requires_operator_) {
    // Checked before anything else, so no window arithmetic runs on a timestamp that was never
    // recorded. This is the entry the file-journal documentation calls "something nothing can ever
    // resolve", and this branch is what makes that true rather than aspirational.
    match.verdict_ = MatchVerdict::NEEDS_OPERATOR;
    match.detail_ = instruction.entry_.detail_.empty() ?
      "recovered from a damaged journal line; only an operator can resolve it" :
      instruction.entry_.detail_;
    return match;
  }

  const OrderSnapshot* found = nullptr;
  if (instruction.entry_.action_ == JournalAction::CANCEL) {
    for (const auto& row : rows) {
      if (!instruction.entry_.order_id_.empty() && row.order_id_ == instruction.entry_.order_id_) {
        ++match.candidates_;
        found = &row;
      }
    }
  } else {
    for (const auto& row : rows) {
      if (row_corresponds(instruction, row, window)) {
        ++match.candidates_;
        found = &row;
      }
    }
  }

  if (match.candidates_ > 1) {
    match.verdict_ = MatchVerdict::AMBIGUOUS;
    match.detail_ = std::to_string(match.candidates_) +
      " orders in the broker's listing could be this instruction; nothing was resolved";
    return match;
  }
  if (match.candidates_ == 1) {
    match.verdict_ = MatchVerdict::MATCHED;
    match.order_id_ = found->order_id_;
    match.detail_ = instruction.entry_.action_ == JournalAction::CANCEL ?
      "the order named by the cancellation is still in the broker's listing" :
      "the broker's listing carries this order";
    return match;
  }
  if (!match_window_closed(instruction, now, window)) {
    match.verdict_ = MatchVerdict::UNDECIDED;
    match.detail_ =
      "no matching order yet, and not enough time has passed for the listing to show one";
    return match;
  }
  match.verdict_ = MatchVerdict::NOT_PLACED;
  match.detail_ = instruction.entry_.action_ == JournalAction::CANCEL ?
    "the order named by the cancellation is no longer in the broker's listing" :
    "no matching order appeared within the window; the order does not exist";
  return match;
}

/** What one sweep of the broker's order list settled, and what it left owed. */
struct ReconcileReport {
  /** One verdict per unresolved instruction, in journal order. */
  std::vector<OrderMatch> matches_;
  /** Instruments cleared, so they may trade again. */
  std::vector<InstrumentId> resolved_;
  /** Instruments still withheld, and why they are in `matches_`. */
  std::vector<InstrumentId> withheld_;
  /** Whether any instruction could correspond to more than one row. */
  bool ambiguous_{false};

  /** Returns whether the sweep settled everything the journal owed. */
  bool settled() const noexcept { return withheld_.empty(); }
};

/**
 * Resolves what the broker's order list settles, and leaves the rest withheld.
 *
 * **An instrument is cleared only when every instruction for it is decided.** Resolving on the
 * first `MATCHED` while a second instruction for the same instrument is still `UNDECIDED` would
 * lift the hold with a question still open, and the ledger is keyed by instrument precisely because
 * that hold is what stops a second order being sent into an unknown state.
 *
 * Only what the journal accounts for is touched. A mark the ledger holds for an instrument this
 * journal knows nothing about — one made before journaling was configured, or by an operator —
 * stays exactly where it is.
 */
inline ReconcileReport reconcile_against(
  const std::vector<OpenInstruction>& open,
  const std::vector<OrderSnapshot>& rows,
  std::chrono::system_clock::time_point now,
  IndeterminacyLedger& ledger,
  const MatchWindow& window = {}
) {
  ReconcileReport report;
  report.matches_.reserve(open.size());

  std::vector<InstrumentId> undecided;
  std::vector<InstrumentId> seen;
  for (const auto& instruction : open) {
    auto match = match_against(instruction, rows, now, window);
    const auto instrument = match.instrument_;
    if (match.verdict_ == MatchVerdict::AMBIGUOUS) {
      report.ambiguous_ = true;
    }
    if (!match_verdict_resolves(match.verdict_)) {
      undecided.push_back(instrument);
    }
    report.matches_.push_back(std::move(match));

    bool known = false;
    for (const auto& id : seen) {
      if (id == instrument) {
        known = true;
        break;
      }
    }
    if (!known) {
      seen.push_back(instrument);
    }
  }

  for (const auto& instrument : seen) {
    bool blocked = false;
    for (const auto& id : undecided) {
      if (id == instrument) {
        blocked = true;
        break;
      }
    }
    if (blocked) {
      report.withheld_.push_back(instrument);
      continue;
    }
    ledger.resolve_after_reading_orders(instrument);
    report.resolved_.push_back(instrument);
  }
  return report;
}

/** What a journal left owed by the process that wrote it. */
struct RecoveryReport {
  /** Instruments marked, so nothing is sent for them until the order list resolves them. */
  std::vector<InstrumentId> marked_;
  /** The instructions the journal could not account for. */
  std::vector<OpenInstruction> open_;
  /** Whether the journal's final line was cut off mid-write. */
  bool truncated_{false};
  /**
   * Whether something was in flight that the journal cannot attribute to an instrument.
   *
   * A truncated line too short to name a symbol. Nothing specific can be withheld, so this is a
   * warning rather than a mark — and the loop's own `ORDER_LIST_UNREAD` gate is what covers it: a
   * runner refuses to send *anything at all* until it has read the broker's order list once,
   * whatever the journal says.
   */
  bool unattributable_{false};
  /** Complete lines that were not written by this library. */
  std::size_t unreadable_lines_{0};
  /** What was found, in a sentence an operator reads. */
  std::string detail_;

  /** Returns whether the journal left nothing owed and nothing damaged. */
  bool clean() const noexcept {
    return marked_.empty() && !truncated_ && !unattributable_ && unreadable_lines_ == 0;
  }
};

/**
 * Marks the ledger for everything a journal left unaccounted for.
 *
 * The restart half of the design, and it is independent of the loop's startup gate rather than a
 * duplicate of it. The gate is about *this process never having looked*; recovery is about *a
 * previous process having left something owed*. A runner needs both: the gate catches an order in
 * flight at the instant of a kill, whose `PENDING` line may not even have been written, and
 * recovery catches everything that was.
 */
inline RecoveryReport recover_into(const JournalReplay& replay, IndeterminacyLedger& ledger) {
  RecoveryReport report;
  report.truncated_ = replay.truncated_tail_;
  report.unreadable_lines_ = replay.unreadable_lines_;
  report.detail_ = replay.detail_;
  report.open_ = unresolved_instructions(replay);

  for (const auto& instruction : report.open_) {
    const auto& instrument = instruction.entry_.instrument_;
    if (instrument.symbol_.empty()) {
      report.unattributable_ = true;
      continue;
    }
    const auto marked = ledger.requires_reconcile(instrument);
    ledger.mark(
      instrument,
      instruction.detail_.empty() ?
        "a previous process left this instruction unresolved in the order journal" :
        instruction.detail_
    );
    if (!marked) {
      report.marked_.push_back(instrument);
    }
  }
  if (report.truncated_ && report.open_.empty()) {
    report.unattributable_ = true;
  }
  return report;
}

/** Marks the ledger for everything `journal` left unaccounted for. */
template <typename Journal>
RecoveryReport recover_into(const Journal& journal, IndeterminacyLedger& ledger) {
  return recover_into(journal.replay(), ledger);
}

/**
 * Recovers `journal` into `runner_instance`, halting it outright when the damage names nothing.
 *
 * **The entry point a program should use**, rather than `recover_into` with a bare ledger, because
 * one of the outcomes cannot be expressed as a ledger mark at all.
 *
 * A journal whose torn line was too short to yield a symbol says *something was in flight* and
 * cannot say for what. There is no instrument to withhold, so withholding one is not an option and
 * withholding everything is the only honest response. Leaving it to the loop's startup gate is not
 * enough: that gate lifts after a single successful order-list read, and a read cannot answer a
 * question nobody can pose -- there is no instrument to look for. So this halts the runner, and an
 * operator clears it with `resume_after_local_failure` once they have read the broker's list
 * themselves.
 */
template <typename Runner, typename Journal>
RecoveryReport recover_runner(Runner& runner_instance, const Journal& journal) {
  auto report = recover_into(journal.replay(), runner_instance.ledger());
  if (report.unattributable_) {
    runner_instance.halt_locally(
      report.detail_.empty() ?
        "the order journal was damaged in a way that names no instrument" :
        report.detail_
    );
  }
  return report;
}

} // namespace runner
} // namespace algotradelib
