#pragma once

#include "./execution.hpp"
#include "./journal.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <ctime>
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

namespace shape_detail {

/** Returns whether neither side of a shape comparison is `UNKNOWN`. */
template <typename Enum>
inline constexpr bool both_known(Enum left, Enum right) noexcept {
  return left != Enum::UNKNOWN && right != Enum::UNKNOWN;
}

} // namespace shape_detail

/** What a broker row's venue says about the instruction's requested route. */
enum class RouteEvidence {
  /** The row was routed where the instruction asked, or the instruction asked for nothing. */
  CONFIRMS,
  /** The row went somewhere else. It is not this order. */
  CONTRADICTS,
  /** Nobody recorded enough to say. Not a match, and not an absence either. */
  ABSENT,
};

/**
 * Classifies what `row`'s venue says about `instruction`'s requested route.
 *
 * **Silence is not confirmation, and that distinction is the whole function.**
 * Treating an unrecorded route as agreement let a legacy journal line -- written
 * before the route was recorded, for an order that had explicitly gone to NXT --
 * be resolved by the only row in the listing, a KRX order that was somebody
 * else's. The hold cleared and the NXT order was still out there. The same
 * followed for a *new* NXT instruction whenever KIS reported a venue this
 * library does not recognize, because that reads as `UNSPECIFIED` too.
 *
 * An instruction that asked for **no** venue is the case that still confirms.
 * That is not silence: it is a recorded fact, and it is every Toss order, whose
 * listings carry no venue at all and never could. There is nothing to verify, so
 * the other attributes decide.
 *
 * **A residual gap, stated rather than left to be found.** Nothing on the
 * strategy path sets `PlannedOrder::venue_`, so *every* order a strategy places
 * records `UNSPECIFIED` -- and at KIS such an order still goes somewhere, KRX,
 * by the broker's documented default for an omitted field. This function
 * therefore confirms a KIS row reporting NXT against an instruction that in fact
 * went to KRX. The harm needs three things at once: this runner's own order
 * absent from the listing, a lookalike at another venue matching instrument,
 * side, quantity, price and the clock-skew window, and no third row. With both
 * rows present there are two candidates and the verdict is `AMBIGUOUS`.
 *
 * Closing it means recording where the order *actually* went rather than what
 * was asked, and only the adapter knows that -- the journal line is written
 * before the send and is broker-neutral. That is a change to the journal's
 * contract and belongs in a branch that is looking at it; see
 * `plan/todo/journal-effective-route.md`. Until then, a caller who wants the
 * comparison to be exact states `OrderRequest::venue_`, and gets it.
 */
inline RouteEvidence route_evidence(
  const OpenInstruction& instruction, const OrderSnapshot& row
) {
  const auto& asked = instruction.entry_.venue_;
  if (!asked) {
    // The line predates the field, so what it asked for is unknowable.
    return RouteEvidence::ABSENT;
  }
  if (*asked == Venue::UNSPECIFIED) {
    // It asked for nothing, so nothing about the row's venue can contradict it.
    return RouteEvidence::CONFIRMS;
  }
  if (row.venue_ == Venue::UNSPECIFIED) {
    // It asked for a venue and the broker did not say where the order went.
    return RouteEvidence::ABSENT;
  }
  return row.venue_ == *asked ? RouteEvidence::CONFIRMS : RouteEvidence::CONTRADICTS;
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
  // The whole shape, not just the basis. This compared `type_` alone, which was
  // vacuous while KIS reported `DAY` for everything it could normalize: there
  // was one condition and one session, so they could not disagree. Now they
  // can, and a resting day order would otherwise be accepted as the
  // immediate-or-cancel order this runner recorded sending -- which is recovery
  // attributing one order to another, the failure the journal exists to
  // prevent.
  if (row.side_ != entry.side_) {
    return false;
  }
  // A known basis on both sides must agree. An `UNKNOWN` on either side is not
  // a disagreement and is not a correspondence either: it is a row this
  // library could not classify, and `row_might_correspond` below is what stops
  // it being read as absence.
  if (!shape_detail::both_known(row.type_, entry.type_)) {
    return false;
  }
  if (row.type_ != entry.type_) {
    return false;
  }
  // A field the broker did not report cannot rule a row out. `UNKNOWN` on
  // either side is absence of information, not disagreement, and this follows
  // the rule already stated below for `ordered_at_`: a row admitted on the
  // attributes that *are* known is the safe direction, because excluding it
  // reports `NOT_PLACED` for an order sitting in the listing, and the runner's
  // answer to `NOT_PLACED` is to send again.
  if (shape_detail::both_known(row.time_in_force_, entry.time_in_force_) &&
      row.time_in_force_ != entry.time_in_force_) {
    return false;
  }
  if (shape_detail::both_known(row.session_, entry.session_) &&
      row.session_ != entry.session_) {
    return false;
  }
  // Where the order went. Two orders in one symbol, side, quantity, price and
  // second differ only by this. `route_evidence` below says whether this row's
  // venue confirms the instruction, contradicts it, or says nothing; only the
  // first two are decided here, and the third is what `row_might_correspond`
  // picks up.
  if (route_evidence(instruction, row) == RouteEvidence::CONTRADICTS) {
    return false;
  }
  if (route_evidence(instruction, row) == RouteEvidence::ABSENT) {
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
 * Returns whether `row` could be this instruction's order despite a shape this
 * library could not read.
 *
 * **Why a second, weaker predicate rather than a looser first one.** An order
 * whose `ORD_DVSN` is outside the published table normalizes to `UNKNOWN` in
 * all three shape fields, and — because an unknown basis is not one
 * `carries_caller_price` admits — with no price either. Three of the six
 * attributes `row_corresponds` compares are therefore missing, so it cannot
 * say yes. Letting it say *no* is the dangerous half: no row corresponds, the
 * window closes, the verdict is `NOT_PLACED`, the hold clears, and the runner's
 * answer to `NOT_PLACED` is to send the order again — beside the one already
 * resting under a code this release had not seen.
 *
 * So a row like that is neither a match nor an absence. It is reported as
 * `AMBIGUOUS`, which is the verdict this file already reserves for "report,
 * never guess", and which withholds the instrument until an operator says
 * which it was. `shape_code_` carries the broker's own code for them to read.
 *
 * This compares what survives an unreadable shape: the instrument, the side,
 * the quantity and the time window. It deliberately does **not** compare the
 * price, which such a row does not carry.
 */
inline bool row_might_correspond(
  const OpenInstruction& instruction, const OrderSnapshot& row, const MatchWindow& window
) {
  const auto& entry = instruction.entry_;
  // A route the instruction asked for and the row denies rules it out however
  // unreadable its shape is -- an unknown-code KRX row is not an NXT order, and
  // letting it force an ambiguity would stall recovery for no reason.
  if (route_evidence(instruction, row) == RouteEvidence::CONTRADICTS) {
    return false;
  }
  const bool shape_unreadable =
    row.type_ == OrderType::UNKNOWN || entry.type_ == OrderType::UNKNOWN;
  const bool route_unknown = route_evidence(instruction, row) == RouteEvidence::ABSENT;
  if (!shape_unreadable && !route_unknown) {
    // Everything that could be compared was, so `row_corresponds` has already
    // given the definitive answer and this predicate has nothing to add.
    return false;
  }
  if (!(row.instrument_ == entry.instrument_) || row.side_ != entry.side_) {
    return false;
  }
  if (!reconcile_detail::same_value(row.quantity_, entry.quantity_)) {
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
  std::size_t unreadable = 0;
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
    // Counted separately, because these are not candidates for a *match* -- the
    // library could not read their shape, so it cannot claim one is this order.
    // They are candidates against concluding the order does not exist.
    for (const auto& row : rows) {
      if (row_might_correspond(instruction, row, window)) {
        ++unreadable;
      }
    }
  }

  if (match.candidates_ > 1) {
    match.verdict_ = MatchVerdict::AMBIGUOUS;
    match.detail_ = std::to_string(match.candidates_) +
      " orders in the broker's listing could be this instruction; nothing was resolved";
    return match;
  }
  if (unreadable > 0) {
    // Report, never guess -- and **whether or not a readable row also matched**.
    // A row whose order code this release does not publish is not evidence that
    // the order is missing, so `NOT_PLACED` would clear the hold and send it
    // again beside the one already resting. But it is equally not evidence that
    // some *other* row is the order: with one readable row matching every
    // attribute and one unreadable row matching every attribute that survives
    // an unreadable shape, the unreadable one may be the instruction and the
    // readable one another order on the same account. Resolving to the readable
    // one would attribute the wrong order and clear the hold on it, which is
    // the same harm by a different route.
    match.verdict_ = MatchVerdict::AMBIGUOUS;
    match.candidates_ += unreadable;
    match.order_id_.clear();
    match.detail_ = std::to_string(unreadable) +
      " order(s) in the broker's listing match this instruction on instrument, side, quantity "
      "and time but carry an order code this library does not recognize" +
      (found != nullptr ? ", and another row matches it fully" : "") +
      "; nothing was resolved. The broker's own code is on each row as shape_code_";
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
  /** How many resolutions were written back to the journal. */
  std::size_t recorded_{0};
  /**
   * Whether a resolution could not be recorded, so its ledger entry was deliberately left standing.
   *
   * The same discipline as writing before a send, applied to the other end: a sweep that cleared
   * the ledger without recording why would resolve the instruction in memory and leave it owed on
   * disk, so the next restart would withhold the instrument all over again.
   */
  bool unrecorded_{false};

  /** Returns whether the sweep settled everything the journal owed. */
  bool settled() const noexcept { return withheld_.empty(); }
};

/** Returns the terminal line that records how a sweep resolved `instruction`. */
inline JournalEntry resolution_entry(
  const OpenInstruction& instruction,
  const OrderMatch& match,
  std::chrono::system_clock::time_point now
) {
  auto entry = instruction.entry_;
  // `RESOLVED`, not `ACCEPTED` or `REJECTED`. Those are answers the broker gave to the instruction;
  // this is what its listing showed afterwards, and for a cancellation the two can be opposites -- a
  // matched row means the target order is still resting, which is the withdrawal *not* having
  // happened. What was observed lives in `order_id_` and `detail_`.
  entry.outcome_ = JournalOutcome::RESOLVED;
  entry.recorded_at_ = now;
  entry.requires_operator_ = false;
  entry.order_id_ = match.order_id_;
  entry.detail_ = "resolved by reading the broker's order list: " + match.detail_;
  return entry;
}

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
template <typename Journal>
ReconcileReport reconcile_against(
  const std::vector<OpenInstruction>& open,
  const std::vector<OrderSnapshot>& rows,
  std::chrono::system_clock::time_point now,
  IndeterminacyLedger& ledger,
  Journal& journal,
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
    if (match_verdict_resolves(match.verdict_)) {
      // Written before the ledger is cleared, for the same reason a placement is written before it
      // is sent: the durable record has to be the conservative one. A sweep that cleared memory and
      // failed to record why would leave the instruction owed on disk, and the next restart would
      // withhold the instrument again for a question that had already been answered.
      if (journal.append(resolution_entry(instruction, match, now))) {
        ++report.recorded_;
      } else {
        report.unrecorded_ = true;
        undecided.push_back(instrument);
        match.detail_ += " (not recorded: the journal could not be written, so the hold stands)";
      }
    } else {
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

/**
 * Returns a report that decides nothing, leaving every instruction owed.
 *
 * What a sweep must produce when it could not see everything it needed to. The alternative -- run
 * the matcher over a partial listing -- is worse than doing nothing at all: after the window
 * closes, a missing row reads as `NOT_PLACED`, so an incomplete view resolves in the one direction
 * that clears the ledger for an order that may exist.
 */
inline ReconcileReport decide_nothing(
  const std::vector<OpenInstruction>& open, std::string detail
) {
  ReconcileReport report;
  report.matches_.reserve(open.size());
  for (const auto& instruction : open) {
    OrderMatch match;
    match.verdict_ = MatchVerdict::UNDECIDED;
    match.instrument_ = instruction.entry_.instrument_;
    match.entry_id_ = instruction.entry_.id_;
    match.detail_ = detail;
    report.matches_.push_back(std::move(match));

    bool known = false;
    for (const auto& id : report.withheld_) {
      if (id == instruction.entry_.instrument_) {
        known = true;
        break;
      }
    }
    if (!known) {
      report.withheld_.push_back(instruction.entry_.instrument_);
    }
  }
  return report;
}

/** Returns whether any of `open` is a placement, which is what needs a lifecycle-wide listing. */
inline bool any_placement(const std::vector<OpenInstruction>& open) {
  for (const auto& instruction : open) {
    // Operator-only entries are excluded, because no listing can decide them: `match_against`
    // returns `NEEDS_OPERATOR` before it looks at a single row. Including them bought pointless
    // pagination on Toss and a guaranteed rejected inquiry on KIS, on every sweep, forever.
    if (instruction.entry_.action_ == JournalAction::PLACE &&
        !instruction.entry_.requires_operator_) {
      return true;
    }
  }
  return false;
}

/** Adds rows to `into`, skipping any order id already there. */
inline void merge_rows(std::vector<OrderSnapshot>& into, const std::vector<OrderSnapshot>& rows) {
  for (const auto& row : rows) {
    bool known = false;
    for (const auto& held : into) {
      // Deduplicated by the broker's own id, because an order that is still open appears in *both*
      // listings. Two copies of one row would read as two candidates, and the matcher would report
      // `AMBIGUOUS` for an instruction that is not ambiguous at all.
      if (!row.order_id_.empty() && held.order_id_ == row.order_id_) {
        known = true;
        break;
      }
    }
    if (!known) {
      into.push_back(row);
    }
  }
}

/** Returns the UTC calendar date `at` falls on. */
inline Date date_of(std::chrono::system_clock::time_point at) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(at);
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  Date date;
  date.year_ = utc.tm_year + 1900;
  date.month_ = static_cast<unsigned>(utc.tm_mon + 1);
  date.day_ = static_cast<unsigned>(utc.tm_mday);
  return date;
}

/**
 * Returns the calendar range an order-history inquiry must cover for `open`.
 *
 * Widened by a day at each end, and the reason is not caution for its own sake: these instants are
 * UTC, a broker's trading day is its own market's, and KIS reports order times in Korean local
 * time. A range that is a day too wide returns rows that do not match and costs nothing; a range
 * that is an hour too narrow misses the order and reports `NOT_PLACED` for one that exists.
 */
inline std::pair<Date, Date> history_range_for(
  const std::vector<OpenInstruction>& open, const MatchWindow& window
) {
  auto earliest = open.front().opened_at_;
  auto latest = instruction_sent_by(open.front(), window);
  for (const auto& instruction : open) {
    if (instruction.opened_at_ < earliest) {
      earliest = instruction.opened_at_;
    }
    const auto sent_by = instruction_sent_by(instruction, window);
    if (sent_by > latest) {
      latest = sent_by;
    }
  }
  return {
    date_of(earliest - std::chrono::hours(24)),
    date_of(latest + std::chrono::hours(24)),
  };
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
  /**
   * Whether the journal file exists and could not be read at all.
   *
   * Nothing about its contents is known, so nothing specific can be withheld -- and unlike a
   * missing journal, this one may hold every order a predecessor left in flight.
   */
  bool unreadable_file_{false};
  /** What was found, in a sentence an operator reads. */
  std::string detail_;

  /** Returns whether the journal left nothing owed and nothing damaged. */
  bool clean() const noexcept {
    return marked_.empty() && !truncated_ && !unattributable_ && unreadable_lines_ == 0 &&
      !unreadable_file_;
  }
};

/**
 * Marks the ledger for everything a journal left unaccounted for.
 *
 * **Prefer `recover_runner`.** This form takes a bare ledger, and one of the outcomes -- damage that
 * names no instrument -- cannot be expressed as a ledger mark at all, so a caller using this one has
 * to notice `unattributable_` and halt trading itself. `recover_runner` does that for you.
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
  report.unreadable_file_ = !replay.readable();
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
 *
 * **A complete line that does not parse halts it too**, for the same reason and one more. A journal
 * this library wrote contains no such line, so its presence means either that something else edited
 * the file or that a repair was itself interrupted -- `finish_repair` terminates a torn fragment and
 * then records a stand-in for it, and a process killed between those leaves the fragment as a
 * complete, unreadable line with no torn tail for the next open to notice. Either way the file
 * cannot account for what was in flight, and an unreadable line that only made `clean()` false had
 * no safety effect at all.
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
  if (report.unreadable_lines_ > 0) {
    runner_instance.halt_locally(
      "the order journal holds " + std::to_string(report.unreadable_lines_) +
      " line(s) this library did not write, so it cannot account for what was in flight"
    );
  }
  if (report.unreadable_file_) {
    // Nothing about the file's contents is known, and unlike a missing journal this one may hold
    // every order a predecessor left in flight. There is nothing to withhold specifically.
    runner_instance.halt_locally(
      report.detail_.empty() ?
        "the order journal exists and could not be read" :
        report.detail_
    );
  }
  return report;
}

} // namespace runner
} // namespace algotradelib
