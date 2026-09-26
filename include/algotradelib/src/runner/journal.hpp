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
 * Deliberately coarser than `SendOutcome`, plus `PENDING`, which is the whole point: it is written
 * *before* the send is attempted, so it is the only record that exists during the window this
 * feature was built for -- the interval in which the bytes may be on the wire and the process may
 * die. Every other value is terminal, and what a second line later says happened.
 *
 * Coarser because a journal answers one question: **does the order exist?** Who decided that it
 * does not -- the broker, or the binding that never asked -- is a `SendOutcome` distinction, and
 * the report carries it. Collapsing them here keeps a recovery sweep reading one thing.
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
  /**
   * The instruction was refused, so it does not exist.
   *
   * **Not necessarily by the broker.** A binding that refuses to send an instruction it cannot
   * express -- an idempotency key at a broker that accepts none -- writes this line too, having
   * asked nobody, and so does an inner executor whose own machinery refused. What every one of them
   * shares is the only thing a recovery sweep needs: the order does not exist, and nothing needs
   * reconciling. A report says who decided for the first two -- `RejectReason` on a placement,
   * `by_broker_` on a cancellation -- and puts the third in `unavailable_` instead, so a journal
   * line alone never attributes a refusal.
   */
  REJECTED,
  /** The request failed in transit. Whether the instruction exists is unknown. */
  INDETERMINATE,
  /**
   * A sweep read the broker's own order list and closed the question.
   *
   * Its own value rather than a reuse of `ACCEPTED` or `REJECTED`, because those record the
   * instruction's own fate at the moment it was attempted, and a sweep learns something different:
   * what the listing shows now. For a placement the two nearly coincide -- the order is there or it is not. For a
   * cancellation they can read as the exact opposite of the truth: a matched row means the target
   * order is **still resting**, so recording that as `ACCEPTED` would say the withdrawal succeeded
   * when the listing says it did not.
   *
   * What was actually observed is in `order_id_` -- set when a row was found, empty when none was
   * -- and spelled out in `detail_`.
   */
  RESOLVED,
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
    case JournalOutcome::RESOLVED:
      return "resolved";
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
  if (text == "resolved") {
    return JournalOutcome::RESOLVED;
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
    case SendOutcome::UNAVAILABLE:
      // Recorded as a refusal, because that is what it is from the order's point of view: nothing
      // was sent, so the order does not exist and nothing needs reconciling. This decorator never
      // reaches here for its own refusals -- those return before a terminal line is written -- but
      // an inner executor with local machinery of its own can produce one, and recording it as
      // `INDETERMINATE` would withhold an instrument for an order that was never attempted.
      return JournalOutcome::REJECTED;
    case SendOutcome::REFUSED_LOCALLY:
      // The same reasoning, and the same answer: nothing was sent, so the order does not exist.
      // The journal records what happened to the order, not who decided it.
      return JournalOutcome::REJECTED;
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
  /**
   * The order's shape, as all three normalized facts.
   *
   * All three, because recovery matches a recorded instruction against a
   * broker's listing on these attributes: an immediate-or-cancel order and a
   * day order of the same basis, quantity and price are different orders, and a
   * journal that recorded only the basis could not tell recovery which one it
   * had sent.
   */
  OrderType type_{OrderType::LIMIT};
  TimeInForce time_in_force_{TimeInForce::DAY};
  OrderSession session_{OrderSession::REGULAR};
  /**
   * The venue this order actually reaches, where the binding can say.
   *
   * Recorded beside the shape and **not** on `instrument_`, which is identity.
   * Recovery matches a recorded instruction against a broker's listing on
   * attributes, and two orders in one symbol, side, quantity, price and second
   * differ only by where they went: without this, an unresolved NXT placement
   * and an unrelated KRX order are indistinguishable, and recovery attributes
   * one to the other and clears the hold.
   *
   * **Optional, so an old line is distinguishable from one that asked for
   * nothing.** `Venue::UNSPECIFIED` -- the default -- means this runner asked
   * for no particular venue, which is a recorded fact rather than an absence: a
   * Toss order can never ask for one, so recovery has nothing to verify and the
   * other attributes decide.
   *
   * Empty means the line **predates this field**, so what the order asked for is
   * unknowable. Only the file journal's reader produces that, and only when the
   * line carries no `route` key at all. A row is then not attributed to the
   * instruction on the strength of a venue nobody recorded -- it is reported as
   * ambiguous instead, because the order may have gone somewhere this row did
   * not.
   */
  std::optional<Venue> venue_{Venue::UNSPECIFIED};
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
  /**
   * Whether only a person may resolve this instruction.
   *
   * Set on an entry recovered from a damaged line, where some of the fields matching depends on are
   * missing or untrustworthy. Such an entry must never be resolved by a sweep, and the reason is
   * specific rather than precautionary: a salvaged entry has no recorded timestamp, so its matching
   * window sits at the epoch. Every window test would pass instantly and no real row could fall
   * inside it, so the very first sweep would call it `NOT_PLACED` and clear the ledger -- resolving,
   * on the strength of missing data, in the one direction that can leave a live order unwatched.
   *
   * Carried in the file rather than derived at read time, because a damaged tail is repaired into a
   * complete record on the next open, and the flag has to survive that.
   */
  bool requires_operator_{false};
  /**
   * Which broker and account this instruction belongs to.
   *
   * Opaque to this library and meaningful to the operator who chose it -- `"kis:live:12345678-01"`,
   * say. It exists because every other field in this record is about an *order*, and matching an
   * order without knowing whose account it is on is how one account's unresolved instruction gets
   * reconciled against another account's rows: the symbol, side, quantity and price can all agree
   * across two accounts trading the same instrument, and the sweep would then clear a hold for an
   * order the first account still has resting.
   *
   * Stamped by the journal rather than by the caller, so a record cannot be written without one.
   */
  std::string scope_;
};

/**
 * Whether a journal's bytes were obtained at all, before anything in them was parsed.
 *
 * The distinction `MISSING` and `UNREADABLE` draw is the whole point, and collapsing them is a live
 * hazard: a runner starting for the first time has no journal and owes nothing, while a journal that
 * *exists* and cannot be read may hold every order its predecessor left in flight. Treating the
 * second as the first lets a process claim a file it never read and trade on an empty ledger.
 */
enum class JournalReadStatus {
  /** No such file. A first start, and genuinely nothing owed. */
  MISSING,
  /** The bytes were read in full. */
  READ,
  /** The file exists and could not be read, so nothing about its contents is known. */
  UNREADABLE,
};

/** Returns a stable lowercase name for `status`. */
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
  /**
   * The broker and account the file's header declares, when it has one.
   *
   * A header rather than a field on the first record, because a record can tear. The scope is the
   * one thing that must be legible even when the first instruction ever written was cut in half --
   * otherwise another account can open the path, and a sweep can reconcile one account's
   * instruction against the other's rows.
   */
  std::string scope_;
  /** Whether a header was found at all. A non-empty file without one is not ours to write to. */
  bool has_header_{false};
  /**
   * Whether the bytes were obtained, and if not, why that matters.
   *
   * `READ` by default, because a journal held in memory always is. Only the file reader reports
   * anything else.
   */
  JournalReadStatus status_{JournalReadStatus::READ};

  /** Returns whether the file could be read at all. */
  bool readable() const noexcept { return status_ != JournalReadStatus::UNREADABLE; }

  /** Returns whether every line was read. */
  bool intact() const noexcept {
    return readable() && unreadable_lines_ == 0 && !truncated_tail_;
  }
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
  MemoryJournal() = default;

  /** Constructs a journal that stamps `scope` on everything it records. */
  explicit MemoryJournal(std::string scope) : scope_(std::move(scope)) {}

  /** Records one line. Always succeeds: memory does not run out of disk. */
  bool append(const JournalEntry& entry) {
    entries_.push_back(entry);
    if (entries_.back().scope_.empty()) {
      entries_.back().scope_ = scope_;
    }
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

  /** Returns the broker and account this journal belongs to. */
  const std::string& scope() const noexcept { return scope_; }

private:
  std::vector<JournalEntry> entries_;
  std::string scope_;
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
 * `SendOutcome::UNAVAILABLE`: nothing was sent, so the order does not exist and nothing needs
 * reconciling, and it is deliberately distinct from a broker's `REJECTED`, which says something
 * about one instruction rather than about whether the runner should still be trading at all.
 * `Runner` turns it into a sticky condition the loop gates on.
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

  /**
   * Returns the venue the bound executor says this order will reach.
   *
   * Detected rather than required: only a broker binding can answer it, and a
   * replay executor or a test double has no venue to speak of. The fallback is
   * the venue the caller asked for, which is the most that can be known without
   * a broker.
   */
  template <typename E = Inner>
  auto route_of(const strategy::PlannedOrder& order, int) const
    -> decltype(std::declval<const E&>().effective_route(order.venue_)) {
    return inner_.effective_route(order.venue_);
  }

  Venue route_of(const strategy::PlannedOrder& order, long) const { return order.venue_; }

  Venue effective_route_of(const strategy::PlannedOrder& order) const {
    return route_of(order, 0);
  }

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
    entry.time_in_force_ = order.time_in_force_;
    entry.session_ = order.session_;
    // **Where the order will actually go, not what the caller asked for.**
    // Nothing on the strategy path sets `venue_`, so every strategy-placed order
    // asks for `UNSPECIFIED` -- and at KIS that order still reaches KRX, by the
    // broker's documented default for an omitted exchange field. Recording the
    // request would put a positive "no venue was reached" claim in the line, and
    // recovery confirms a row from *any* venue against one of those: a lone NXT
    // lookalike could be attributed to an order that went to KRX.
    //
    // The binding is the only thing that knows, and asking it here keeps the
    // ordering `ai/order-journal` chose -- `effective_route` is a pure function
    // of the order, so the durable line is still appended before any network
    // I/O. An executor that does not answer (a replay, a test double) leaves the
    // requested value, which is all it could mean there.
    entry.venue_ = effective_route_of(order);
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
    if (!journal_->append(entry)) {
      // The send already happened, so refusing it now would be a lie. What is lost is the *answer*,
      // which leaves the instruction looking unresolved to the next recovery -- the safe direction,
      // costing one reconciliation. The failure is still recorded so the runner stops.
      note_failure("the order journal could not record an instruction's outcome");
    }
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
    if (!journal_->append(entry)) {
      note_failure("the order journal could not record an instruction's outcome");
    }
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
   * **Kept rather than cleared**, and the first reason rather than the latest. A journal that failed
   * once will fail again, and a caller that sees this is looking at a storage problem rather than a
   * trading one: the runner should stop, not retry. `Runner` treats it exactly that way, turning the
   * first `UNAVAILABLE` into a sticky condition the loop gates on.
   */
  const std::string& last_journal_error() const noexcept { return journal_error_; }

  /** Returns how many instructions were refused because they could not be journaled. */
  std::size_t unjournaled_refusals() const noexcept { return refusals_; }

  /**
   * Returns how many journal writes have failed, counting every one.
   *
   * A *generation*, not a status. `last_journal_error()` keeps the first reason forever, which is
   * right for diagnosis and useless for recovery: a runner watching only that would re-halt on the
   * next send after an operator fixed the storage and resumed, because the stale reason is still
   * there. Comparing this count against the one already acknowledged is what distinguishes a new
   * failure from one that has been dealt with.
   */
  std::size_t journal_failures() const noexcept { return failures_; }

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
    note_failure("the order journal could not be written, so nothing was sent");
    return SendOutcome::UNAVAILABLE;
  }

  /**
   * Records why the journal failed, keeping the first reason and counting every failure.
   *
   * The two halves answer different questions. The reason is for a person reading a report, and the
   * first one is the useful one. The count is for the runner, which needs to know whether anything
   * has gone wrong *since* it last acknowledged a problem.
   */
  void note_failure(std::string detail) {
    ++failures_;
    if (journal_error_.empty()) {
      journal_error_ = std::move(detail);
    }
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
  std::size_t failures_{0};
  std::uint64_t sequence_{0};
};

} // namespace runner
} // namespace algotradelib
