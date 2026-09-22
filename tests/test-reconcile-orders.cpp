#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;

namespace runner = algotradelib::runner;

namespace {

using Clock = std::chrono::system_clock;
using runner::IndeterminacyLedger;
using runner::JournalAction;
using runner::JournalEntry;
using runner::JournalOutcome;
using runner::MatchVerdict;
using runner::MatchWindow;
using runner::OpenInstruction;

/** Builds a UTC instant without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute, int second = 0) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) +
    std::chrono::minutes(minute) + std::chrono::seconds(second);
}

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

/** The instant every instruction in these tests was recorded at. */
Clock::time_point sent_at() { return utc(2026, 3, 30, 1, 0, 0); }

/** An unanswered placement, which is what a process killed mid-send leaves behind. */
OpenInstruction placement(
  const InstrumentId& id = samsung(),
  const char* quantity = "10",
  const char* price = "70000"
) {
  JournalEntry entry;
  entry.id_ = "run-1";
  entry.action_ = JournalAction::PLACE;
  entry.outcome_ = JournalOutcome::PENDING;
  entry.recorded_at_ = sent_at();
  entry.instrument_ = id;
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::LIMIT;
  entry.quantity_ = Decimal64::parse(quantity);
  entry.price_ = Decimal64::parse(price);

  OpenInstruction instruction;
  instruction.entry_ = entry;
  instruction.opened_at_ = entry.recorded_at_;
  return instruction;
}

/** A cancellation naming an order the broker already issued. */
OpenInstruction cancellation(const char* order_id, const InstrumentId& id = samsung()) {
  auto instruction = placement(id);
  instruction.entry_.action_ = JournalAction::CANCEL;
  instruction.entry_.order_id_ = order_id;
  return instruction;
}

/** A row as a broker's listing reports it. */
OrderSnapshot row(
  const char* order_id,
  const InstrumentId& id = samsung(),
  const char* quantity = "10",
  const char* price = "70000",
  std::optional<Clock::time_point> ordered_at = sent_at()
) {
  OrderSnapshot snapshot;
  snapshot.order_id_ = order_id;
  snapshot.instrument_ = id;
  snapshot.side_ = OrderSide::BUY;
  snapshot.type_ = OrderType::LIMIT;
  snapshot.status_ = OrderStatus::PENDING;
  snapshot.quantity_ = Decimal64::parse(quantity);
  snapshot.price_ = Decimal64::parse(price);
  snapshot.ordered_at_ = ordered_at;
  return snapshot;
}

/** An instant past which absence is evidence, given the default window. */
Clock::time_point after_the_window() { return sent_at() + std::chrono::minutes(10); }

/** An instant at which absence still means nothing. */
Clock::time_point inside_the_window() { return sent_at() + std::chrono::seconds(5); }

/**
 * Somewhere for a sweep to record its resolutions when the test is about the verdict.
 *
 * A journal is a required argument rather than an optional one, deliberately: a sweep that clears
 * the ledger without writing down why leaves the instruction owed on disk, so every caller has to
 * say where the record goes. Tests that care about the record use their own.
 */
runner::MemoryJournal sink;

/** A journal that refuses to record, so the write-back failure path is reachable. */
class FailingSink {
public:
  bool append(const JournalEntry&) { return false; }
  runner::JournalReplay replay() const { return {}; }
};

} // namespace

// ---------------------------------------------------------------------------
// A placement, matched on attributes
// ---------------------------------------------------------------------------

TEST_CASE("one corresponding row matches a placement", "[reconcile]") {
  const auto match =
    runner::match_against(placement(), {row("KRX-1")}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
  REQUIRE(match.order_id_ == "KRX-1");
  REQUIRE(match.candidates_ == 1);
  REQUIRE(match.instrument_ == samsung());
}

TEST_CASE("a row for another instrument is not a match", "[reconcile]") {
  const auto match =
    runner::match_against(placement(), {row("KRX-1", hynix())}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(match.candidates_ == 0);
}

TEST_CASE("side, type, quantity and price must all agree", "[reconcile]") {
  auto wrong_side = row("KRX-1");
  wrong_side.side_ = OrderSide::SELL;
  REQUIRE(
    runner::match_against(placement(), {wrong_side}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );

  auto wrong_type = row("KRX-1");
  wrong_type.type_ = OrderType::MARKET;
  REQUIRE(
    runner::match_against(placement(), {wrong_type}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );

  REQUIRE(
    runner::match_against(placement(), {row("KRX-1", samsung(), "11")}, after_the_window())
        .verdict_ == MatchVerdict::NOT_PLACED
  );
  REQUIRE(
    runner::match_against(placement(), {row("KRX-1", samsung(), "10", "70100")},
      after_the_window())
        .verdict_ == MatchVerdict::NOT_PLACED
  );
}

TEST_CASE("a quantity of a different scale is still the same quantity", "[reconcile]") {
  // `Decimal64::operator==` compares the coefficient and the scale, so a broker reporting `10.00`
  // where the journal wrote `10` would fail an equality test while denoting the same number. A
  // matcher using it would report NOT_PLACED for an order sitting in the listing.
  const auto match =
    runner::match_against(placement(), {row("KRX-1", samsung(), "10.00", "70000.0")},
      after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
}

TEST_CASE("a market order matches a row with no price", "[reconcile]") {
  auto instruction = placement();
  instruction.entry_.type_ = OrderType::MARKET;
  instruction.entry_.price_.reset();

  auto market_row = row("KRX-1");
  market_row.type_ = OrderType::MARKET;
  market_row.price_.reset();

  REQUIRE(
    runner::match_against(instruction, {market_row}, after_the_window()).verdict_ ==
      MatchVerdict::MATCHED
  );
  // One side carrying a price and the other not is a different order, not a near miss.
  REQUIRE(
    runner::match_against(instruction, {row("KRX-1")}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

TEST_CASE("absence inside the window decides nothing", "[reconcile]") {
  const auto match = runner::match_against(placement(), {}, inside_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::UNDECIDED);
  REQUIRE_FALSE(runner::match_verdict_resolves(match.verdict_));
}

TEST_CASE("absence after the window means the order does not exist", "[reconcile]") {
  const auto match = runner::match_against(placement(), {}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(runner::match_verdict_resolves(match.verdict_));
}

TEST_CASE("the window closes later for an instruction nobody answered", "[reconcile]") {
  // A `PENDING` line with a terminal companion brackets the send exactly. One without has to be
  // bounded by how long a request could have been outstanding, so its window closes later.
  auto answered = placement();
  answered.answered_ = true;
  answered.closed_at_ = sent_at();

  const MatchWindow window;
  const auto unanswered_closes =
    runner::instruction_sent_by(placement(), window) + window.clock_skew_ + window.listing_delay_;
  const auto answered_closes =
    runner::instruction_sent_by(answered, window) + window.clock_skew_ + window.listing_delay_;
  REQUIRE(answered_closes < unanswered_closes);

  const auto between = answered_closes + std::chrono::seconds(1);
  REQUIRE(runner::match_against(answered, {}, between).verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(runner::match_against(placement(), {}, between).verdict_ == MatchVerdict::UNDECIDED);
}

TEST_CASE("a row stamped outside the window is not this instruction", "[reconcile]") {
  const auto stale = row("KRX-1", samsung(), "10", "70000", sent_at() - std::chrono::hours(2));
  REQUIRE(
    runner::match_against(placement(), {stale}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );
}

TEST_CASE("clock skew is allowed in both directions", "[reconcile]") {
  // The journal's stamp is local and the broker's is its own. A skew allowance too narrow does not
  // fail loudly: it reports NOT_PLACED for an order that exists, which is the worst outcome here.
  const auto early = row("KRX-1", samsung(), "10", "70000", sent_at() - std::chrono::seconds(20));
  const auto late = row("KRX-2", samsung(), "10", "70000", sent_at() + std::chrono::seconds(70));
  REQUIRE(
    runner::match_against(placement(), {early}, after_the_window()).verdict_ ==
      MatchVerdict::MATCHED
  );
  REQUIRE(
    runner::match_against(placement(), {late}, after_the_window()).verdict_ ==
      MatchVerdict::MATCHED
  );

  MatchWindow narrow;
  narrow.clock_skew_ = std::chrono::seconds(1);
  REQUIRE(
    runner::match_against(placement(), {early}, after_the_window(), narrow).verdict_ ==
      MatchVerdict::NOT_PLACED
  );
}

TEST_CASE("a row with no timestamp is judged on attributes alone", "[reconcile]") {
  // Excluding it would report NOT_PLACED for an order sitting in the listing.
  const auto undated = row("KRX-1", samsung(), "10", "70000", std::nullopt);
  REQUIRE(
    runner::match_against(placement(), {undated}, after_the_window()).verdict_ ==
      MatchVerdict::MATCHED
  );
}

// ---------------------------------------------------------------------------
// Ambiguity
// ---------------------------------------------------------------------------

TEST_CASE("two candidate rows resolve nothing", "[reconcile]") {
  const auto match =
    runner::match_against(placement(), {row("KRX-1"), row("KRX-2")}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
  REQUIRE(match.candidates_ == 2);
  REQUIRE(match.order_id_.empty());
  REQUIRE_FALSE(runner::match_verdict_resolves(match.verdict_));
}

TEST_CASE("a cancellation can never be ambiguous", "[reconcile]") {
  // The identity match the design needs, and the only one available: a cancellation's journal line
  // carries the broker's own order id from the moment it is written, and an order id is unique in a
  // listing. Asserted rather than assumed.
  const std::vector<OrderSnapshot> rows = {
    row("KRX-1"), row("KRX-2"), row("KRX-3", samsung(), "10", "70000"),
  };
  const auto match = runner::match_against(cancellation("KRX-2"), rows, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
  REQUIRE(match.candidates_ == 1);
  REQUIRE(match.order_id_ == "KRX-2");
}

TEST_CASE("a cancellation whose order has left the listing is resolved", "[reconcile]") {
  const auto match =
    runner::match_against(cancellation("KRX-9"), {row("KRX-1")}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(runner::match_verdict_resolves(match.verdict_));
}

TEST_CASE("a cancellation is not matched by attributes", "[reconcile]") {
  // Identical in every attribute, different in id. Falling back to attributes here would resolve a
  // cancellation against an order it never named.
  const auto match =
    runner::match_against(cancellation("KRX-9"), {row("KRX-1")}, inside_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::UNDECIDED);
  REQUIRE(match.candidates_ == 0);
}

// ---------------------------------------------------------------------------
// Sweeping the ledger
// ---------------------------------------------------------------------------

TEST_CASE("a sweep clears what the listing settles", "[reconcile]") {
  IndeterminacyLedger ledger;
  ledger.mark(samsung(), "an order request failed in transit");

  const auto report =
    runner::reconcile_against({placement()}, {row("KRX-1")}, after_the_window(), ledger, sink);
  REQUIRE(report.settled());
  REQUIRE(report.resolved_.size() == 1);
  REQUIRE(report.resolved_.front() == samsung());
  REQUIRE_FALSE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("an ambiguous instruction leaves its instrument withheld", "[reconcile]") {
  IndeterminacyLedger ledger;
  ledger.mark(samsung(), "an order request failed in transit");

  const auto report = runner::reconcile_against(
    {placement()}, {row("KRX-1"), row("KRX-2")}, after_the_window(), ledger, sink
  );
  REQUIRE(report.ambiguous_);
  REQUIRE_FALSE(report.settled());
  REQUIRE(report.withheld_.front() == samsung());
  REQUIRE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("an instrument clears only when every instruction for it is decided", "[reconcile]") {
  IndeterminacyLedger ledger;
  ledger.mark(samsung(), "an order request failed in transit");

  auto matched = placement();
  auto pending = placement(samsung(), "20", "70000");
  pending.entry_.id_ = "run-2";

  // The first instruction is answered by the listing; the second is not, and the window is still
  // open. Clearing on the first would lift the hold with a question still outstanding.
  const auto report = runner::reconcile_against(
    {matched, pending}, {row("KRX-1")}, inside_the_window(), ledger, sink
  );
  REQUIRE(report.matches_.size() == 2);
  REQUIRE(report.matches_[0].verdict_ == MatchVerdict::MATCHED);
  REQUIRE(report.matches_[1].verdict_ == MatchVerdict::UNDECIDED);
  REQUIRE_FALSE(report.settled());
  REQUIRE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("a sweep does not touch a mark the journal knows nothing about", "[reconcile]") {
  IndeterminacyLedger ledger;
  ledger.mark(hynix(), "an operator marked this by hand");

  const auto report =
    runner::reconcile_against({placement()}, {row("KRX-1")}, after_the_window(), ledger, sink);
  REQUIRE(report.resolved_.size() == 1);
  REQUIRE(ledger.requires_reconcile(hynix()));
}

TEST_CASE("verdict names are stable", "[reconcile]") {
  REQUIRE(runner::match_verdict_name(MatchVerdict::MATCHED) == "matched");
  REQUIRE(runner::match_verdict_name(MatchVerdict::NOT_PLACED) == "not_placed");
  REQUIRE(runner::match_verdict_name(MatchVerdict::AMBIGUOUS) == "ambiguous");
  REQUIRE(runner::match_verdict_name(MatchVerdict::UNDECIDED) == "undecided");
}

// ---------------------------------------------------------------------------
// Recovery: what a previous process left owed
// ---------------------------------------------------------------------------

namespace {

using runner::JournalingExecutor;
using runner::ManualClock;
using runner::MemoryJournal;
using runner::RecordingExecutor;
using runner::SendOutcome;

namespace strategy = algotradelib::strategy;

/** An executor that fails on purpose, so the indeterminacy path needs no outage. */
template <typename Tag>
class FailingExecutor {
public:
  SendOutcome place(const strategy::PlannedOrder&) { return outcome_; }
  SendOutcome cancel(const WorkingOrder<Tag>&) { return outcome_; }
  SendOutcome outcome_{SendOutcome::INDETERMINATE};
};

strategy::PlannedOrder buy_ten(const InstrumentId& id = samsung()) {
  strategy::PlannedOrder order;
  order.instrument_ = id;
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");
  return order;
}

/** A journal line as it stands in a file. */
JournalEntry entry_at(
  const char* id,
  JournalOutcome outcome,
  const InstrumentId& instrument = samsung(),
  Clock::time_point at = sent_at()
) {
  JournalEntry entry;
  entry.id_ = id;
  entry.outcome_ = outcome;
  entry.instrument_ = instrument;
  entry.recorded_at_ = at;
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::LIMIT;
  entry.quantity_ = Decimal64::parse("10");
  entry.price_ = Decimal64::parse("70000");
  return entry;
}

} // namespace

TEST_CASE("a journal with nothing owed recovers into an empty ledger", "[reconcile][recover]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    entry_at("run-1", JournalOutcome::PENDING),
    entry_at("run-1", JournalOutcome::ACCEPTED),
  };

  IndeterminacyLedger ledger;
  const auto report = runner::recover_into(replay, ledger);
  REQUIRE(report.clean());
  REQUIRE(ledger.empty());
}

TEST_CASE("an unanswered instruction is recovered into the ledger", "[reconcile][recover]") {
  runner::JournalReplay replay;
  replay.entries_ = {entry_at("run-1", JournalOutcome::PENDING, hynix())};

  IndeterminacyLedger ledger;
  const auto report = runner::recover_into(replay, ledger);
  REQUIRE_FALSE(report.clean());
  REQUIRE(report.marked_.size() == 1);
  REQUIRE(report.marked_.front() == hynix());
  // The whole point: the restarted runner refuses to send for this instrument until the broker's
  // own order list says what happened.
  REQUIRE(ledger.requires_reconcile(hynix()));
}

TEST_CASE("an instruction answered in transit is recovered too", "[reconcile][recover]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    entry_at("run-1", JournalOutcome::PENDING),
    entry_at("run-1", JournalOutcome::INDETERMINATE),
  };

  IndeterminacyLedger ledger;
  const auto report = runner::recover_into(replay, ledger);
  REQUIRE(report.marked_.size() == 1);
  REQUIRE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("a truncated line with no instrument is a warning, not a mark", "[reconcile][recover]") {
  runner::JournalReplay replay;
  replay.truncated_tail_ = true;
  replay.detail_ = "the final journal line was truncated and named no instrument";

  IndeterminacyLedger ledger;
  const auto report = runner::recover_into(replay, ledger);
  REQUIRE(report.truncated_);
  REQUIRE(report.unattributable_);
  REQUIRE(report.marked_.empty());
  // Nothing specific can be withheld. The loop's own startup gate covers it: a runner sends nothing
  // at all until it has read the broker's order list once.
  REQUIRE(ledger.empty());
}

TEST_CASE("a crash and a restart, end to end", "[reconcile][recover]") {
  // The story the feature exists for, with no broker and no filesystem: a runner sends, the process
  // dies before the outcome is known, a new process starts with an empty ledger, and recovery plus
  // one order-list read is what decides whether trading may resume.
  MemoryJournal journal;
  {
    JournalingExecutor<FailingExecutor<runner::DryTag>, MemoryJournal, ManualClock> executor{
      FailingExecutor<runner::DryTag>{}, journal, ManualClock{sent_at()}
    };
    REQUIRE(executor.place(buy_ten()) == SendOutcome::INDETERMINATE);
  }

  // A fresh process. Its ledger knows nothing, which is exactly the hazard.
  IndeterminacyLedger ledger;
  REQUIRE(ledger.empty());

  const auto recovery = runner::recover_into(journal, ledger);
  REQUIRE(recovery.marked_.size() == 1);
  REQUIRE(ledger.requires_reconcile(samsung()));

  // The first sweep finds nothing and settles nothing, because absence this soon means nothing.
  const auto early =
    runner::reconcile_against(recovery.open_, {}, inside_the_window(), ledger, sink);
  REQUIRE_FALSE(early.settled());
  REQUIRE(ledger.requires_reconcile(samsung()));

  // A later sweep finds the order. It existed after all, and the instrument may trade again.
  const auto later =
    runner::reconcile_against(recovery.open_, {row("KRX-1")}, after_the_window(), ledger, sink);
  REQUIRE(later.settled());
  REQUIRE(later.matches_.front().order_id_ == "KRX-1");
  REQUIRE_FALSE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("recovering twice marks an instrument once", "[reconcile][recover]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    entry_at("run-1", JournalOutcome::PENDING),
    entry_at("run-2", JournalOutcome::PENDING),
  };

  IndeterminacyLedger ledger;
  const auto report = runner::recover_into(replay, ledger);
  REQUIRE(report.open_.size() == 2);
  REQUIRE(report.marked_.size() == 1);
  REQUIRE(ledger.pending().size() == 1);
}

// ---------------------------------------------------------------------------
// Resolutions are written back, so a restart does not re-ask a settled question
// ---------------------------------------------------------------------------

TEST_CASE("a resolved instruction is closed in the journal", "[reconcile][recover]") {
  runner::MemoryJournal journal;
  journal.append(entry_at("run-1", JournalOutcome::PENDING));

  IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(journal, ledger);
  REQUIRE(ledger.requires_reconcile(samsung()));

  const auto report = runner::reconcile_against(
    recovery.open_, {row("KRX-1")}, after_the_window(), ledger, journal
  );
  REQUIRE(report.settled());
  REQUIRE(report.recorded_ == 1);
  REQUIRE_FALSE(report.unrecorded_);

  // The terminal line closes the pending one by id, in the broker's own vocabulary: the order is in
  // the listing, so it exists.
  REQUIRE(journal.records().size() == 2);
  REQUIRE(journal.records().back().id_ == "run-1");
  REQUIRE(journal.records().back().outcome_ == JournalOutcome::ACCEPTED);
  REQUIRE(journal.records().back().order_id_ == "KRX-1");

  // A second restart finds nothing owed. Before the write-back this recovered and withheld again,
  // forever, on every restart.
  IndeterminacyLedger fresh;
  const auto again = runner::recover_into(journal, fresh);
  REQUIRE(again.clean());
  REQUIRE(fresh.empty());
}

TEST_CASE("an order that never existed is closed as a refusal", "[reconcile][recover]") {
  runner::MemoryJournal journal;
  journal.append(entry_at("run-1", JournalOutcome::PENDING));

  IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(journal, ledger);
  const auto report =
    runner::reconcile_against(recovery.open_, {}, after_the_window(), ledger, journal);
  REQUIRE(report.matches_.front().verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(journal.records().back().outcome_ == JournalOutcome::REJECTED);

  IndeterminacyLedger fresh;
  REQUIRE(runner::recover_into(journal, fresh).clean());
}

TEST_CASE("repeated sweeps do not contradict each other", "[reconcile][recover]") {
  // Without the write-back, a later sweep replayed the unchanged journal and could report the same
  // instruction `UNDECIDED` after an earlier sweep had already cleared the ledger on `MATCHED` --
  // leaving the report inconsistent with the actual gate.
  runner::MemoryJournal journal;
  journal.append(entry_at("run-1", JournalOutcome::PENDING));

  IndeterminacyLedger ledger;
  auto open = runner::unresolved_instructions(journal.replay());
  runner::recover_into(journal, ledger);

  const auto first =
    runner::reconcile_against(open, {row("KRX-1")}, after_the_window(), ledger, journal);
  REQUIRE(first.settled());

  // The next sweep replays the journal, as a source does, and finds the question closed.
  open = runner::unresolved_instructions(journal.replay());
  REQUIRE(open.empty());
  const auto second = runner::reconcile_against(open, {}, after_the_window(), ledger, journal);
  REQUIRE(second.matches_.empty());
  REQUIRE(second.settled());
  REQUIRE_FALSE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("a resolution that cannot be recorded leaves the hold standing", "[reconcile][recover]") {
  // The same discipline as writing before a send, applied to the other end. Clearing memory while
  // failing to record why would resolve the instruction here and leave it owed on disk.
  runner::MemoryJournal source;
  source.append(entry_at("run-1", JournalOutcome::PENDING));

  IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(source, ledger);

  FailingSink sink;
  const auto report = runner::reconcile_against(
    recovery.open_, {row("KRX-1")}, after_the_window(), ledger, sink
  );
  REQUIRE(report.matches_.front().verdict_ == MatchVerdict::MATCHED);
  REQUIRE(report.unrecorded_);
  REQUIRE(report.recorded_ == 0);
  REQUIRE_FALSE(report.settled());
  REQUIRE(ledger.requires_reconcile(samsung()));
}
