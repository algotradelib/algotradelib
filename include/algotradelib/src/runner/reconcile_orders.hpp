#pragma once

#include "./execution.hpp"
#include "./journal.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * @brief What the broker's order list says about one instruction of unknown outcome.
 *
 * A missing row means `NOT_PLACED` only after the matching window has closed (the listing may lag
 * an accepted write); before that it is `UNDECIDED`. `NOT_PLACED` is a finding about the rows supplied,
 * not the broker's word that no order exists: an overlapping runner then forgets the placement, and a
 * serialized runner (`Runner::serialized`) keeps it owed.
 */
enum class MatchVerdict {
  /**
   * @brief Exactly one row corresponds; the order exists.
   *
   * For a cancellation: the named order is in the listing supplied (which may include history, so
   * not necessarily resting).
   */
  MATCHED,
  /**
   * @brief No row corresponds and the window has closed: no order was found.
   *
   * For a cancellation: the named order is not in the listing supplied.
   */
  NOT_PLACED,
  /**
   * @brief Several rows could correspond, a row that fits is another placement's, or a row that fits
   *   cannot be confirmed; the instrument stays withheld until a later sweep or an operator decides.
   */
  AMBIGUOUS,
  /** @brief No row yet, and the window is still open; wait for the next sweep. */
  UNDECIDED,
  /**
   * @brief The instruction came from a damaged journal line, or its line cannot be matched
   *   (`unresolvable_reason`); only an operator may resolve it.
   */
  NEEDS_OPERATOR,
};

/**
 * @brief Returns a verdict's lowercase name, such as `"not_placed"`, for logs.
 * @param verdict The verdict.
 * @return The name.
 */
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

/**
 * @brief Tests whether a verdict settles the question.
 * @param verdict The verdict.
 * @return `true` for `MATCHED` and `NOT_PLACED`.
 */
inline constexpr bool match_verdict_resolves(MatchVerdict verdict) noexcept {
  return verdict == MatchVerdict::MATCHED || verdict == MatchVerdict::NOT_PLACED;
}

/**
 * @brief The time allowances used when matching journalled instructions against a broker listing.
 *
 * Too narrow a window reports `NOT_PLACED` for an order that exists, which is the dangerous error;
 * the defaults are generous.
 */
struct MatchWindow {
  /** @brief How far the broker's clock may differ from this machine's, either way. */
  std::chrono::seconds clock_skew_{30};
  /** @brief How long a request could have been in flight when no answering line records its end. */
  std::chrono::seconds maximum_request_{60};
  /** @brief How long the broker's listing may lag an order it accepted. */
  std::chrono::seconds listing_delay_{30};
};

/** @brief One instruction's verdict, with context for the operator. */
struct OrderMatch {
  /** @brief The verdict. */
  MatchVerdict verdict_{MatchVerdict::UNDECIDED};
  /** @brief The instruction's instrument. */
  InstrumentId instrument_;
  /** @brief The journal line id. */
  std::string entry_id_;
  /** @brief The broker's order id, on `MATCHED`. */
  std::string order_id_;
  /** @brief How many rows could correspond (one or more on `AMBIGUOUS`). */
  std::size_t candidates_{0};
  /** @brief What was decided and why, for the operator. */
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
 * @brief Returns the last instant an instruction could have reached the broker.
 * @param instruction The instruction.
 * @param window The allowances.
 * @return When its answering line was written, or its `PENDING` time plus `maximum_request_`.
 */
inline std::chrono::system_clock::time_point instruction_sent_by(
  const OpenInstruction& instruction, const MatchWindow& window
) {
  return instruction.closed_at_ ?
    *instruction.closed_at_ :
    instruction.opened_at_ + window.maximum_request_;
}

/**
 * @brief Tests whether a missing row may now be read as "not placed".
 * @param instruction The instruction.
 * @param now The current instant.
 * @param window The allowances.
 * @return `true` once `now` is past `instruction_sent_by` plus the clock skew and listing delay.
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

/**
 * @brief What one attribute of a broker row says about an instruction.
 *
 * `ABSENT` (nothing recorded to compare) neither confirms a match nor allows an absence verdict.
 */
enum class Evidence {
  /** @brief The row agrees. */
  CONFIRMS,
  /** @brief The row disagrees; it is not this order. */
  CONTRADICTS,
  /** @brief Not enough was recorded to say. */
  ABSENT,
};

/** @brief `Evidence`, under the name used for the route comparison. */
using RouteEvidence = Evidence;

/**
 * @brief Compares an enumerated attribute of a row with the instruction's.
 * @tparam Enum The enumeration (it must have an `UNKNOWN` value).
 * @param row_value The row's value.
 * @param entry_value The instruction's value.
 * @return `ABSENT` if either is `UNKNOWN`; otherwise `CONFIRMS` or `CONTRADICTS`.
 */
template <typename Enum>
inline Evidence attribute_evidence(Enum row_value, Enum entry_value) noexcept {
  if (row_value == Enum::UNKNOWN || entry_value == Enum::UNKNOWN) {
    return Evidence::ABSENT;
  }
  return row_value == entry_value ? Evidence::CONFIRMS : Evidence::CONTRADICTS;
}

/**
 * @brief Compares a row's trigger price with the instruction's.
 *
 * | Row | Entry | Evidence |
 * | --- | --- | --- |
 * | `t` | `t` | `CONFIRMS` |
 * | `t` | `u` | `CONTRADICTS` |
 * | none | none | `CONFIRMS` |
 * | none | `t` | `ABSENT` (the broker may not have reported it) |
 * | `t` | none | `ABSENT` (the broker's row contradicts its own type) |
 *
 * @param row_trigger The row's trigger.
 * @param entry_trigger The instruction's trigger.
 * @return The evidence.
 */
inline Evidence trigger_evidence(
  const std::optional<Decimal64>& row_trigger,
  const std::optional<Decimal64>& entry_trigger
) {
  if (!row_trigger) {
    return entry_trigger ? Evidence::ABSENT : Evidence::CONFIRMS;
  }
  if (!entry_trigger) {
    return Evidence::ABSENT;
  }
  return reconcile_detail::same_value(*row_trigger, *entry_trigger) ?
    Evidence::CONFIRMS :
    Evidence::CONTRADICTS;
}

/**
 * @brief Compares a row's order time with the window in which the instruction was sent.
 *
 * A row with no time is judged on its other attributes (`CONFIRMS` here), because every KIS
 * real-account working-order row lacks a date. So an identical order placed through another channel, while this
 * runner's own order is not listed, can be matched to it; when both are listed the result is
 * `AMBIGUOUS`. A row whose date the adapter assumed (`OrderSnapshot::ordered_date_assumed_`) may be
 * another day's order at the same time of day, so it establishes nothing (`ABSENT`), whether or not its
 * time could be read.
 *
 * @param instruction The instruction.
 * @param row The row.
 * @param window The allowances.
 * @return `ABSENT` if the row's date was assumed; otherwise `CONFIRMS` if it has no time or its time is
 *   inside the widened send window, and `CONTRADICTS` if not.
 */
inline Evidence time_evidence(
  const OpenInstruction& instruction, const OrderSnapshot& row, const MatchWindow& window
) {
  if (row.ordered_date_assumed_) {
    return Evidence::ABSENT;
  }
  if (!row.ordered_at_) {
    return Evidence::CONFIRMS;
  }
  const auto begin = instruction.opened_at_ - window.clock_skew_;
  const auto end = instruction_sent_by(instruction, window) + window.clock_skew_;
  return (*row.ordered_at_ >= begin && *row.ordered_at_ <= end) ? Evidence::CONFIRMS
                                                                : Evidence::CONTRADICTS;
}

/**
 * @brief Compares a row's venue with the route the instruction recorded.
 *
 * The recorded route is where the order actually went (an unrouted KIS order reaches KRX).
 *
 * @param instruction The instruction.
 * @param row The row.
 * @return `ABSENT` if the line predates routes or the row names no venue; `CONFIRMS` if the
 *   instruction asked for no venue (as at Toss) or the venues agree; otherwise `CONTRADICTS`.
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
 * @brief Tests whether a row is established to be an instruction's order.
 *
 * Matched by attributes, since no broker lists a client order id: the instrument, side and type
 * (both known), quantity and price must match, and time in force, session, trigger, route and time
 * must each `CONFIRMS`.
 *
 * @param instruction The instruction.
 * @param row The row.
 * @param window The allowances.
 * @return `true` if every attribute confirms.
 */
inline bool row_corresponds(
  const OpenInstruction& instruction, const OrderSnapshot& row, const MatchWindow& window
) {
  const auto& entry = instruction.entry_;
  if (!(row.instrument_ == entry.instrument_)) {
    return false;
  }
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
  if (!reconcile_detail::same_value(row.quantity_, entry.quantity_)) {
    return false;
  }
  if (!reconcile_detail::same_value(row.price_, entry.price_)) {
    return false;
  }
  // A match needs every attribute established, not merely not contradicted.
  for (const auto evidence : {
         attribute_evidence(row.time_in_force_, entry.time_in_force_),
         attribute_evidence(row.session_, entry.session_),
         trigger_evidence(row.trigger_price_, entry.trigger_price_),
         route_evidence(instruction, row),
         time_evidence(instruction, row, window),
       }) {
    if (evidence != Evidence::CONFIRMS) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Tests whether a row might be an instruction's order although some attributes could not be
 *   read (such as an order type outside the broker's published table).
 *
 * Such a row makes the verdict `AMBIGUOUS` (an operator decides) rather than `NOT_PLACED`. It
 * compares instrument, side, quantity and time, and the price when both order types are known, and
 * rules the row out on any positive disagreement.
 *
 * @param instruction The instruction.
 * @param row The row.
 * @param window The allowances.
 * @return `true` if some attribute is unknown and nothing disagrees; `false` for a row that fully
 *   corresponds (see `row_corresponds`) or disagrees.
 */
inline bool row_might_correspond(
  const OpenInstruction& instruction, const OrderSnapshot& row, const MatchWindow& window
) {
  const auto& entry = instruction.entry_;
  // Anything that positively disagrees rules the row out however much else is
  // silent -- an unknown-code row at a venue the instruction did not use is not
  // this order, and letting it force an ambiguity would stall recovery for
  // nothing.
  for (const auto evidence : {
         attribute_evidence(row.type_, entry.type_),
         attribute_evidence(row.time_in_force_, entry.time_in_force_),
         attribute_evidence(row.session_, entry.session_),
         trigger_evidence(row.trigger_price_, entry.trigger_price_),
         route_evidence(instruction, row),
         time_evidence(instruction, row, window),
       }) {
    if (evidence == Evidence::CONTRADICTS) {
      return false;
    }
  }

  const bool anything_silent =
    attribute_evidence(row.type_, entry.type_) == Evidence::ABSENT ||
    attribute_evidence(row.time_in_force_, entry.time_in_force_) == Evidence::ABSENT ||
    attribute_evidence(row.session_, entry.session_) == Evidence::ABSENT ||
    trigger_evidence(row.trigger_price_, entry.trigger_price_) == Evidence::ABSENT ||
    route_evidence(instruction, row) == Evidence::ABSENT ||
    time_evidence(instruction, row, window) == Evidence::ABSENT;
  if (!anything_silent) {
    // Everything comparable was compared, so `row_corresponds` has already
    // given the definitive answer and this predicate has nothing to add.
    return false;
  }

  if (!(row.instrument_ == entry.instrument_) || row.side_ != entry.side_) {
    return false;
  }
  if (!reconcile_detail::same_value(row.quantity_, entry.quantity_)) {
    return false;
  }
  // Compare what survived. A row whose *basis* is unreadable has no price
  // either, because the parser discards it -- but one whose only silence is a
  // route or a timestamp still has both, and a row differing there is plainly
  // another order rather than an ambiguity worth withholding an instrument for.
  if (attribute_evidence(row.type_, entry.type_) != Evidence::ABSENT) {
    if (!reconcile_detail::same_value(row.price_, entry.price_)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Returns why an instruction cannot be resolved from a broker listing.
 *
 * A cancellation with no order id, or a placement whose recorded price or trigger price does not fit
 * its type (missing where required, or present where not taken), would otherwise read as absent.
 * Such an instruction needs an operator (`match_against` returns `NEEDS_OPERATOR`).
 *
 * @param entry The journalled instruction.
 * @return The reason, or an empty string if it can be matched.
 */
inline std::string unresolvable_reason(const JournalEntry& entry) {
  if (entry.action_ == JournalAction::CANCEL && entry.order_id_.empty()) {
    return "a cancellation was journalled without the order id it names, so no row can be "
           "identified as its target; an operator must say what became of it";
  }
  // **Placements only.** A cancellation is matched on the broker id alone --
  // `row.order_id_ == entry.order_id_` and nothing else -- so its copy of the
  // order's shape is never compared and an inconsistency in it cannot mislead
  // anything. Applying the check to every action withheld an instrument whose
  // identity the journal *had* recorded, for a cancellation that resolves
  // without trouble: the shape is copied from the working order, so a
  // reservation carrying an unpriced `LIMIT` or a reported `MARKET` price puts
  // it there without the cancellation caring.
  if (entry.action_ == JournalAction::PLACE && entry.type_ != OrderType::UNKNOWN) {
    const bool priced = carries_caller_price(entry.type_);
    if (priced && !entry.price_) {
      return "a price-bearing order was journalled without its price, so no row can be matched "
             "on it; an operator must say what became of it";
    }
    if (!priced && entry.price_) {
      return "an order whose price the market sets was journalled carrying one, so no row can "
             "be matched on it; an operator must say what became of it";
    }
    // The trigger's twin of the price rule, for the same reason: a line the writer emits before
    // the binding could refuse it, which no row can be matched against.
    const bool triggered = carries_trigger_price(entry.type_);
    if (triggered && !entry.trigger_price_) {
      return "a stop-limit order was journalled without its trigger price, so no row can be "
             "matched on it; an operator must say what became of it";
    }
    if (!triggered && entry.trigger_price_) {
      return "an order that takes no trigger price was journalled carrying one, so no row can be "
             "matched on it; an operator must say what became of it";
    }
  }
  return {};
}

/**
 * @brief Returns the placements a journal records an order id for, each with the span of its send.
 *
 * The id is the one an `ACCEPTED` line (the broker's reply) or a `RESOLVED` line (a sweep's name)
 * records. The span runs from the placement's first line to the line that answered the send
 * (`ACCEPTED`, `REJECTED` or `INDETERMINATE`). A placement with no such id, a refused one and an
 * operator-only one are left out.
 *
 * @param replay The journal's contents.
 * @return The placements, with `entry_.order_id_` set to the recorded id.
 */
inline std::vector<OpenInstruction> recorded_placements(const JournalReplay& replay) {
  std::vector<OpenInstruction> found;
  std::unordered_map<std::string, std::size_t> found_at;
  std::vector<std::string> refused;
  for (const auto& entry : replay.entries_) {
    if (entry.action_ != JournalAction::PLACE || entry.requires_operator_) {
      continue;
    }
    auto known = found_at.find(entry.id_);
    if (known == found_at.end()) {
      OpenInstruction placement;
      placement.entry_ = entry;
      placement.entry_.order_id_.clear();
      placement.opened_at_ = entry.recorded_at_;
      known = found_at.emplace(entry.id_, found.size()).first;
      found.push_back(std::move(placement));
    }
    auto& placement = found[known->second];
    if (entry.outcome_ == JournalOutcome::REJECTED) {
      refused.push_back(entry.id_);
    }
    const bool answers_the_send = entry.outcome_ == JournalOutcome::ACCEPTED ||
      entry.outcome_ == JournalOutcome::REJECTED || entry.outcome_ == JournalOutcome::INDETERMINATE;
    if (answers_the_send && !placement.closed_at_) {
      placement.closed_at_ = entry.recorded_at_;
    }
    const bool names_the_order = entry.outcome_ == JournalOutcome::ACCEPTED || entry.outcome_ == JournalOutcome::RESOLVED;
    if (names_the_order && !entry.order_id_.empty() && placement.entry_.order_id_.empty()) {
      placement.entry_.order_id_ = entry.order_id_;
    }
  }
  found.erase(
    std::remove_if(
      found.begin(), found.end(),
      [&](const OpenInstruction& placement) {
        return placement.entry_.order_id_.empty() ||
          std::find(refused.begin(), refused.end(), placement.entry_.id_) != refused.end();
      }
    ),
    found.end()
  );
  return found;
}

/**
 * @brief Returns the recorded placement whose order a row is: the row carries the order id the journal
 *   records for it and fits that placement's own send (`row_corresponds` or `row_might_correspond`).
 *
 * The send window matters because some brokers reuse order numbers on another day.
 *
 * @param instruction The instruction being matched, which is never its own rival.
 * @param row The row.
 * @param recorded The placements with recorded ids (`recorded_placements`).
 * @param window The allowances.
 * @return The placement, or `nullptr`.
 */
inline const OpenInstruction* recorded_owner(
  const OpenInstruction& instruction,
  const OrderSnapshot& row,
  const std::vector<OpenInstruction>& recorded,
  const MatchWindow& window
) {
  if (row.order_id_.empty()) {
    return nullptr;
  }
  for (const auto& other : recorded) {
    if (other.entry_.id_ == instruction.entry_.id_ || other.entry_.order_id_ != row.order_id_) {
      continue;
    }
    if (row_corresponds(other, row, window) || row_might_correspond(other, row, window)) {
      return &other;
    }
  }
  return nullptr;
}

/**
 * @brief Decides what a broker listing says about one instruction of unknown outcome.
 *
 * A cancellation is matched by the broker order id it names (`AMBIGUOUS` only if two rows carry that
 * id); a placement by attributes (`row_corresponds`, `row_might_correspond`). An operator-only or
 * unresolvable (`unresolvable_reason`) instruction is `NEEDS_OPERATOR`.
 *
 * For a placement, a row that fits it but is the order of another placement in `recorded`
 * (`recorded_owner`) is no candidate, and makes the verdict `AMBIGUOUS` whatever else fits: an
 * operator decides, because the record may itself be a sweep's mistaken name.
 *
 * @param instruction The instruction.
 * @param rows The listing (working orders and history).
 * @param recorded The journal's placements with a recorded order id (`recorded_placements`).
 * @param now The current instant.
 * @param window The allowances.
 * @return The verdict.
 */
inline OrderMatch match_against(
  const OpenInstruction& instruction,
  const std::vector<OrderSnapshot>& rows,
  const std::vector<OpenInstruction>& recorded,
  std::chrono::system_clock::time_point now,
  const MatchWindow& window = {}
) {
  OrderMatch match;
  match.instrument_ = instruction.entry_.instrument_;
  match.entry_id_ = instruction.entry_.id_;

  // **An instruction whose own line cannot support a conclusion.** These are
  // written by this library -- `JournalingExecutor` appends the pending line
  // before the binding runs, so a locally refused order is on disk before
  // anything could object -- which is why the *reader* accepts them. What must
  // not happen is resolving one: each lacks precisely the field the matcher
  // would decide on, so a sweep would conclude from silence.
  //
  // An ordinary local refusal is unaffected: its own terminal line closes the
  // pending one before any of this is reached. Only an instruction still open
  // at recovery arrives here, and for those the honest answer is an operator.
  if (const auto reason = unresolvable_reason(instruction.entry_); !reason.empty()) {
    match.verdict_ = MatchVerdict::NEEDS_OPERATOR;
    match.detail_ = reason;
    return match;
  }
  if (instruction.entry_.requires_operator_) {
    // Checked before anything else, so no window arithmetic runs on a timestamp that was never
    // recorded. This is the entry the file-journal documentation calls "something nothing can ever
    // resolve", and this check is what makes that true rather than aspirational.
    match.verdict_ = MatchVerdict::NEEDS_OPERATOR;
    match.detail_ = instruction.entry_.detail_.empty() ?
      "recovered from a damaged journal line; only an operator can resolve it" :
      instruction.entry_.detail_;
    return match;
  }

  const OrderSnapshot* found = nullptr;
  std::size_t unreadable = 0;
  std::size_t claimed = 0;
  const OrderSnapshot* claimed_row = nullptr;
  const OpenInstruction* claimant = nullptr;
  if (instruction.entry_.action_ == JournalAction::CANCEL) {
    for (const auto& row : rows) {
      if (!instruction.entry_.order_id_.empty() && row.order_id_ == instruction.entry_.order_id_) {
        ++match.candidates_;
        found = &row;
      }
    }
  } else {
    for (const auto& row : rows) {
      const bool fits = row_corresponds(instruction, row, window);
      // Counted separately, because these are not candidates for a *match* -- the
      // library could not read their shape, so it cannot claim one is this order.
      // They are candidates against concluding the order does not exist.
      const bool might_fit = row_might_correspond(instruction, row, window);
      if (!fits && !might_fit) {
        continue;
      }
      // Another placement's order by the journal's own record: neither this order nor evidence
      // that this order is missing.
      if (const auto* owner = recorded_owner(instruction, row, recorded, window)) {
        ++claimed;
        claimed_row = &row;
        claimant = owner;
        continue;
      }
      if (fits) {
        ++match.candidates_;
        found = &row;
      }
      if (might_fit) {
        ++unreadable;
      }
    }
  }

  if (claimed > 0) {
    // Not `MATCHED`: the row is another placement's. Not `NOT_PLACED` either, even when nothing else
    // fits, and not `MATCHED` on another row that fits: the record may be a sweep's mistaken name,
    // and then the claimed row is this order and the other row is the claimant's.
    match.verdict_ = MatchVerdict::AMBIGUOUS;
    const auto others = match.candidates_ + unreadable;
    match.candidates_ = others + claimed;
    match.detail_ = "order " + claimed_row->order_id_ + " in the broker's listing fits this instruction, and the "
      "journal records it for another placement (" + claimant->entry_.id_ + ")" +
      (claimed > 1 ? ", as it does " + std::to_string(claimed - 1) + " more such row(s)" : std::string()) +
      (others > 0 ? "; " + std::to_string(others) + " other row(s) fit as well" : std::string()) +
      "; nothing was resolved";
    return match;
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
    // Says which it is. An operator reading "unrecognized order code" about a
    // row whose code was perfectly readable, and whose *route* was the unknown,
    // is being sent to look at the wrong field.
    match.detail_ = std::to_string(unreadable) +
      " order(s) in the broker's listing match this instruction on instrument, side, quantity "
      "and time, but this library cannot confirm which order they are: an order code it does "
      "not recognize, a route nobody recorded, or both" +
      (found != nullptr ? ", and another row matches it fully" : "") +
      "; nothing was resolved. Each row carries the broker's own code as shape_code_ and its "
      "venue as venue_";
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
    "no matching order was found among the rows read, within the matching window";
  return match;
}

/**
 * @brief Decides what a broker listing says about one instruction, with no other placement's
 *   recorded order to set aside: every row that fits is a candidate.
 *
 * For a listing already filtered of other placements' rows. A sweep over the journal's open
 * instructions should pass `recorded_placements` (the other overload), as `reconcile_against` does.
 *
 * @param instruction The instruction.
 * @param rows The listing (working orders and history).
 * @param now The current instant.
 * @param window The allowances.
 * @return The verdict.
 */
inline OrderMatch match_against(
  const OpenInstruction& instruction,
  const std::vector<OrderSnapshot>& rows,
  std::chrono::system_clock::time_point now,
  const MatchWindow& window = {}
) {
  return match_against(instruction, rows, {}, now, window);
}

/**
 * @brief Removes the rows that could be another placement's order as well.
 *
 * A row could be another placement's when `row_corresponds` says it is that placement's order (the
 * same instrument, side and shape, dated inside that placement's own send window: from its `PENDING`
 * line to the answer of that send, `ACCEPTED` or `INDETERMINATE`, or `maximum_request_` after the
 * `PENDING` line where no such answer was written), or when `row_might_correspond` says it may be:
 * nothing disagrees and something could not be compared, such as the route of a line written before
 * routes were recorded. Whatever order id the journal records for that placement plays no part: a
 * sweep may have given it another order's number. A placement the broker or the binding refused
 * (`REJECTED`) has no order and claims no row.
 *
 * For a search that acts only on finding an order (`live::SessionSource` uses it where it looks again
 * for an order a serialized runner still owes): what is left can be matched to the instruction with
 * no other placement in the journal as a rival, and an order sent beside another of the same shape
 * stays unfound. It is not for a sweep whose negative verdict is acted on.
 *
 * @param rows The rows to filter.
 * @param replay The journal's contents.
 * @param matching The instructions about to be matched, which are not rivals of each other here.
 * @param window The allowances.
 */
inline void drop_rows_fitting_another_placement(
  std::vector<OrderSnapshot>& rows,
  const JournalReplay& replay,
  const std::vector<OpenInstruction>& matching,
  const MatchWindow& window = {}
) {
  // The other placements on the instruments being matched, each with the span of its send.
  std::vector<OpenInstruction> others;
  std::unordered_map<std::string, std::size_t> other_of;
  std::vector<std::string> refused;
  for (const auto& entry : replay.entries_) {
    if (entry.action_ != JournalAction::PLACE) {
      continue;
    }
    const bool on_an_instrument_matched = std::any_of(
      matching.begin(), matching.end(),
      [&](const OpenInstruction& instruction) { return instruction.entry_.instrument_ == entry.instrument_; }
    );
    const bool being_matched = std::any_of(matching.begin(), matching.end(), [&](const OpenInstruction& instruction) {
      return instruction.entry_.id_ == entry.id_;
    });
    if (!on_an_instrument_matched || being_matched) {
      continue;
    }
    if (entry.outcome_ == JournalOutcome::REJECTED) {
      refused.push_back(entry.id_);
    }
    auto known = other_of.find(entry.id_);
    if (known == other_of.end()) {
      OpenInstruction other;
      other.entry_ = entry;
      other.opened_at_ = entry.recorded_at_;
      known = other_of.emplace(entry.id_, others.size()).first;
      others.push_back(std::move(other));
    }
    auto& other = others[known->second];
    const bool answers_the_send = entry.outcome_ == JournalOutcome::ACCEPTED ||
      entry.outcome_ == JournalOutcome::REJECTED || entry.outcome_ == JournalOutcome::INDETERMINATE;
    if (answers_the_send && !other.closed_at_) {
      other.closed_at_ = entry.recorded_at_;
    }
  }
  rows.erase(
    std::remove_if(
      rows.begin(), rows.end(),
      [&](const OrderSnapshot& row) {
        return std::any_of(others.begin(), others.end(), [&](const OpenInstruction& other) {
          return std::find(refused.begin(), refused.end(), other.entry_.id_) == refused.end() &&
            (row_corresponds(other, row, window) || row_might_correspond(other, row, window));
        });
      }
    ),
    rows.end()
  );
}

/** @brief What one sweep of the broker's order list settled, and what is still withheld. */
struct ReconcileReport {
  /** @brief One verdict per instruction, in journal order. */
  std::vector<OrderMatch> matches_;
  /** @brief Instruments cleared; they may trade again. */
  std::vector<InstrumentId> resolved_;
  /** @brief Instruments still withheld (their reasons are in `matches_`). */
  std::vector<InstrumentId> withheld_;
  /** @brief Whether any instruction was `AMBIGUOUS`. */
  bool ambiguous_{false};
  /** @brief How many resolutions were written to the journal. */
  std::size_t recorded_{0};
  /**
   * @brief Whether a resolution could not be written to the journal; its instrument then stays
   *   withheld.
   */
  bool unrecorded_{false};

  /**
   * @brief Tests whether the sweep settled everything.
   * @return `true` if nothing is still withheld.
   */
  bool settled() const noexcept { return withheld_.empty(); }
};

/**
 * @brief Makes the `RESOLVED` journal line that records a sweep's verdict.
 * @param instruction The instruction.
 * @param match The verdict.
 * @param now When it was resolved.
 * @return The line.
 */
inline JournalEntry resolution_entry(
  const OpenInstruction& instruction,
  const OrderMatch& match,
  std::chrono::system_clock::time_point now
) {
  auto entry = instruction.entry_;
  // RESOLVED records what the listing showed, not the broker's answer to the instruction.
  entry.outcome_ = JournalOutcome::RESOLVED;
  entry.recorded_at_ = now;
  entry.requires_operator_ = false;
  entry.order_id_ = match.order_id_;
  entry.detail_ = "resolved by reading the broker's order list: " + match.detail_;
  return entry;
}

/**
 * @brief Resolves what a broker listing settles, records it in the journal, and clears the ledger
 *   for each instrument whose instructions are all decided.
 *
 * An instrument is cleared only when every instruction for it is `MATCHED` or `NOT_PLACED`, and
 * only after the resolution is written. Ledger entries the journal knows nothing about are left.
 * A row the journal records as another placement's order is set aside (`match_against`).
 *
 * @tparam Journal The journal type.
 * @param open The unresolved instructions.
 * @param rows The listing, which must be complete.
 * @param replay The journal's contents, which `open` was taken from.
 * @param now The current instant.
 * @param ledger The ledger to clear.
 * @param journal The journal to record resolutions in.
 * @param window The allowances.
 * @param unplaced_note Text added to the journal detail of a placement found `NOT_PLACED`: what the
 *   caller does with that finding.
 * @return The report.
 */
template <typename Journal>
ReconcileReport reconcile_against(
  const std::vector<OpenInstruction>& open,
  const std::vector<OrderSnapshot>& rows,
  const JournalReplay& replay,
  std::chrono::system_clock::time_point now,
  IndeterminacyLedger& ledger,
  Journal& journal,
  const MatchWindow& window = {},
  std::string_view unplaced_note = {}
) {
  ReconcileReport report;
  report.matches_.reserve(open.size());
  const auto recorded = recorded_placements(replay);

  std::vector<InstrumentId> undecided;
  std::vector<InstrumentId> seen;
  for (const auto& instruction : open) {
    auto match = match_against(instruction, rows, recorded, now, window);
    const auto instrument = match.instrument_;
    if (match.verdict_ == MatchVerdict::AMBIGUOUS) {
      report.ambiguous_ = true;
    }
    if (match.verdict_ == MatchVerdict::NOT_PLACED && instruction.entry_.action_ == JournalAction::PLACE) {
      match.detail_ += unplaced_note;
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
 * @brief Resolves what a broker listing settles, reading the journal's contents from `journal`; see
 *   the other overload.
 * @tparam Journal The journal type; it must provide `replay()`.
 * @param open The unresolved instructions.
 * @param rows The listing, which must be complete.
 * @param now The current instant.
 * @param ledger The ledger to clear.
 * @param journal The journal to read and to record resolutions in.
 * @param window The allowances.
 * @param unplaced_note Text added to the journal detail of a placement found `NOT_PLACED`.
 * @return The report.
 */
template <typename Journal>
ReconcileReport reconcile_against(
  const std::vector<OpenInstruction>& open,
  const std::vector<OrderSnapshot>& rows,
  std::chrono::system_clock::time_point now,
  IndeterminacyLedger& ledger,
  Journal& journal,
  const MatchWindow& window = {},
  std::string_view unplaced_note = {}
) {
  return reconcile_against(open, rows, journal.replay(), now, ledger, journal, window, unplaced_note);
}

/**
 * @brief Makes a report that decides nothing, for a sweep that could not see a complete listing.
 *
 * Matching a partial listing could read a missing row as `NOT_PLACED`.
 *
 * @param open The unresolved instructions.
 * @param detail Why nothing was decided.
 * @return A report with every instruction `UNDECIDED` and withheld.
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

/**
 * @brief Tests whether any instruction is a placement a listing could decide (which needs history,
 *   not only working orders).
 * @param open The instructions.
 * @return `true` if any is a `PLACE` that is not operator-only.
 */
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

/**
 * @brief Tests whether two rows are established copies of one order: the same order id and
 *   instrument, and the same order time on both.
 * @param a The first row.
 * @param b The second row.
 * @return `true` only if all three are stated and equal.
 */
inline bool exact_copy(const OrderSnapshot& a, const OrderSnapshot& b) {
  return !a.order_id_.empty() && a.order_id_ == b.order_id_ && a.instrument_ == b.instrument_ &&
    a.ordered_at_ && b.ordered_at_ && *a.ordered_at_ == *b.ordered_at_;
}

/**
 * @brief Tests whether a dated history row is the order a listing row whose date was assumed shows: the
 *   same order id and instrument at the same time of day, nothing either row states disagrees (side,
 *   quantity, type, time in force, session, trigger, venue), and the price is stated alike on both
 *   (both absent, or equal).
 * @param assumed The listing row (`OrderSnapshot::ordered_date_assumed_`).
 * @param row The history row.
 * @return `true` if the history row dates the listed order.
 */
inline bool dates_listed_order(const OrderSnapshot& assumed, const OrderSnapshot& row) {
  if (row.ordered_date_assumed_ || !row.ordered_at_ || !assumed.ordered_at_ || assumed.order_id_.empty() ||
      row.order_id_ != assumed.order_id_ || !(row.instrument_ == assumed.instrument_)) {
    return false;
  }
  // Both stamped from a date and the same `HHmmss` in one zone, so one order's two rows differ by whole days.
  if ((*assumed.ordered_at_ - *row.ordered_at_) % std::chrono::hours(24) != std::chrono::hours(0)) {
    return false;
  }
  // A number reused on another day can belong to a different order at the same time of day.
  const bool venues_differ = row.venue_ != Venue::UNSPECIFIED && assumed.venue_ != Venue::UNSPECIFIED &&
    row.venue_ != assumed.venue_;
  return row.side_ == assumed.side_ &&
    reconcile_detail::same_value(row.quantity_, assumed.quantity_) &&
    reconcile_detail::same_value(row.price_, assumed.price_) &&
    attribute_evidence(row.type_, assumed.type_) != Evidence::CONTRADICTS &&
    attribute_evidence(row.time_in_force_, assumed.time_in_force_) != Evidence::CONTRADICTS &&
    attribute_evidence(row.session_, assumed.session_) != Evidence::CONTRADICTS &&
    trigger_evidence(row.trigger_price_, assumed.trigger_price_) != Evidence::CONTRADICTS &&
    !venues_differ;
}

/**
 * @brief Adds history rows to a working-order listing, dropping established copies (`exact_copy`) and
 *   listing rows a history row dates; for brokers whose history takes no range (Kiwoom, Toss).
 *
 * A listing row whose date was assumed (`OrderSnapshot::ordered_date_assumed_`) is dropped where a
 * history row dates the same order (`dates_listed_order`): the history row stands for it, on its own
 * evidence, and nothing of the listing row is carried over. An uncertain row is kept, at the cost of a
 * possible `AMBIGUOUS`. An adapter should stamp `ordered_at_` the same way on working and history rows
 * of one order, or both are kept.
 *
 * @param into The listing, extended in place.
 * @param rows The history rows.
 */
inline void merge_dated_history(
  std::vector<OrderSnapshot>& into,
  const std::vector<OrderSnapshot>& rows
) {
  std::vector<OrderSnapshot> listing;
  listing.reserve(into.size() + rows.size());
  for (auto& listed : into) {
    const bool dated_by_history = listed.ordered_date_assumed_ &&
      std::any_of(rows.begin(), rows.end(), [&](const OrderSnapshot& row) { return dates_listed_order(listed, row); });
    if (!dated_by_history) {
      listing.push_back(std::move(listed));
    }
  }
  into = std::move(listing);
  const auto listed = into.size();
  for (const auto& row : rows) {
    bool known = false;
    for (std::size_t at = 0; at < listed; ++at) {
      // A listing row still assumed is no copy of anything: the dated row is the evidence, and both stay.
      if (!into[at].ordered_date_assumed_ && exact_copy(into[at], row)) {
        known = true;
        break;
      }
    }
    if (!known) {
      into.push_back(row);
    }
  }
}

/**
 * @brief Adds rows to a listing, skipping any whose order id is already there.
 * @param into The listing, extended in place.
 * @param rows The rows to add.
 */
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

/**
 * @brief Returns the UTC calendar date of an instant.
 * @param at The instant.
 * @return The date.
 */
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
 * @brief Returns the date range an order-history inquiry must cover for some instructions.
 *
 * Widened by a day at each end, since the instants are UTC and brokers date orders in their own
 * market's time.
 *
 * @param open The instructions; must not be empty.
 * @param window The allowances.
 * @return The first and last date.
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

/** @brief What a journal showed was left owed by the process that wrote it. */
struct RecoveryReport {
  /** @brief Instruments marked in the ledger, withheld until the order list resolves them. */
  std::vector<InstrumentId> marked_;
  /** @brief The instructions of unknown outcome. */
  std::vector<OpenInstruction> open_;
  /** @brief Whether the last line was torn. */
  bool truncated_{false};
  /**
   * @brief Whether something was in flight that names no instrument (a torn line too short to name
   *   one); `recover_runner` halts the runner for it.
   */
  bool unattributable_{false};
  /** @brief Complete lines that did not parse. */
  std::size_t unreadable_lines_{0};
  /** @brief Whether the journal exists and could not be read; it may hold orders left in flight. */
  bool unreadable_file_{false};
  /** @brief What was found, for the operator. */
  std::string detail_;

  /**
   * @brief Tests whether nothing was owed or damaged.
   * @return `true` if no instrument was marked and nothing was torn, unparsed or unreadable.
   */
  bool clean() const noexcept {
    return marked_.empty() && !truncated_ && !unattributable_ && unreadable_lines_ == 0 &&
      !unreadable_file_;
  }
};

/**
 * @brief Marks the ledger for every instrument a journal left an instruction of unknown outcome for.
 *
 * It halts nothing. Prefer `recover_runner`; with a bare ledger, stop trading yourself if the report
 * has `unattributable_`, `unreadable_lines_` or `unreadable_file_` set: then the ledger can be empty
 * while the journal's contents are unknown.
 *
 * @param replay The journal's contents.
 * @param ledger The ledger to mark.
 * @return The report.
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

/**
 * @brief Reads a journal and marks the ledger for what it left owed; see the other overload.
 * @tparam Journal The journal type.
 * @param journal The journal.
 * @param ledger The ledger to mark.
 * @return The report.
 */
template <typename Journal>
RecoveryReport recover_into(const Journal& journal, IndeterminacyLedger& ledger) {
  return recover_into(journal.replay(), ledger);
}

/**
 * @brief Recovers a journal into a runner at startup: marks each instrument left owed, restores a
 *   serialized runner's orders not yet accounted for (`Runner::restore_obligations`), and halts the
 *   runner when damage names no instrument.
 *
 * The runner is halted (`LOCAL_FAILURE`) when a torn line names no instrument, a complete line does
 * not parse, or the file cannot be read. An operator reads the broker's order list and calls
 * `Runner::resume_after_local_failure`.
 *
 * @tparam Runner The runner type.
 * @tparam Journal The journal type.
 * @param runner_instance The runner.
 * @param journal The journal.
 * @return The report.
 */
template <typename Runner, typename Journal>
RecoveryReport recover_runner(Runner& runner_instance, const Journal& journal) {
  const auto replay = journal.replay();
  auto report = recover_into(replay, runner_instance.ledger());
  // A serialized runner also gets back every order it may still owe (`Runner::restore_obligations`),
  // and places nothing until this has run.
  runner_instance.restore_obligations(replay);
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
