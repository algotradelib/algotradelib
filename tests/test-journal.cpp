#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;

namespace runner = algotradelib::runner;
namespace strategy = algotradelib::strategy;

namespace {

using Clock = std::chrono::system_clock;
using runner::JournalAction;
using runner::JournalEntry;
using runner::JournalOutcome;
using runner::MemoryJournal;
using runner::SendOutcome;

/** Builds a UTC instant without a zone database. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days =
    365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) +
    std::chrono::minutes(minute);
}

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

InstrumentId hynix() { return InstrumentId{Country::KR, Venue::COMBINED, "000660"}; }

strategy::PlannedOrder buy_ten(const InstrumentId& id = samsung(), const char* price = "70000") {
  strategy::PlannedOrder order;
  order.instrument_ = id;
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse(price);
  return order;
}

WorkingOrder<runner::DryTag> resting(const char* order_id) {
  WorkingOrder<runner::DryTag> working;
  working.order_.order_id_ = order_id;
  working.order_.instrument_ = samsung();
  working.order_.side_ = OrderSide::BUY;
  working.order_.type_ = OrderType::LIMIT;
  working.order_.quantity_ = Decimal64::parse("10");
  working.order_.price_ = Decimal64::parse("70000");
  working.ref_.primary_ = order_id;
  return working;
}

/**
 * An executor that reports a chosen outcome and asserts what the journal held when it was called.
 *
 * The observation hook is the point. Asserting that a `PENDING` line exists *after* `place()`
 * returns would pass even if the line were written after the socket, which is precisely the bug the
 * ordering rule exists to prevent. Capturing the journal's contents from inside the send is the
 * only way to state the requirement as a test.
 */
template <typename Tag>
class ObservingExecutor {
public:
  ObservingExecutor() = default;

  explicit ObservingExecutor(const MemoryJournal* journal) : journal_(journal) {}

  SendOutcome place(const strategy::PlannedOrder& order) {
    placed_.push_back(order);
    capture();
    return place_outcome_;
  }

  SendOutcome cancel(const WorkingOrder<Tag>& working) {
    canceled_.push_back(working.ref_);
    capture();
    return cancel_outcome_;
  }

  const std::vector<strategy::PlannedOrder>& placed() const noexcept { return placed_; }
  const std::vector<OrderRef<Tag>>& canceled() const noexcept { return canceled_; }

  /** What the journal held at the moment each send was attempted. */
  const std::vector<std::vector<JournalEntry>>& seen() const noexcept { return seen_; }

  SendOutcome place_outcome_{SendOutcome::ACCEPTED};
  SendOutcome cancel_outcome_{SendOutcome::ACCEPTED};

private:
  void capture() {
    if (journal_ != nullptr) {
      seen_.push_back(journal_->records());
    }
  }

  const MemoryJournal* journal_{nullptr};
  std::vector<strategy::PlannedOrder> placed_;
  std::vector<OrderRef<Tag>> canceled_;
  std::vector<std::vector<JournalEntry>> seen_;
};

/** An executor reporting a broker reference and an error message, as a session-backed one does. */
template <typename Tag>
class ReportingExecutor {
public:
  SendOutcome place(const strategy::PlannedOrder&) {
    if (place_outcome_ == SendOutcome::ACCEPTED) {
      last_ref_ = OrderRef<Tag>{};
      last_ref_->primary_ = issued_id_;
      last_error_.reset();
      return place_outcome_;
    }
    BrokerError error;
    error.category_ = ErrorCategory::TRANSPORT;
    error.message_ = message_;
    last_error_ = error;
    return place_outcome_;
  }

  SendOutcome cancel(const WorkingOrder<Tag>&) { return SendOutcome::ACCEPTED; }

  const std::optional<OrderRef<Tag>>& last_ref() const noexcept { return last_ref_; }
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

  SendOutcome place_outcome_{SendOutcome::ACCEPTED};
  std::string issued_id_{"BROKER-1"};
  std::string message_{"the connection was reset"};

private:
  std::optional<OrderRef<Tag>> last_ref_;
  std::optional<BrokerError> last_error_;
};

using Journaling =
  runner::JournalingExecutor<ObservingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>;

} // namespace

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("journal outcome names round-trip", "[journal]") {
  const JournalOutcome outcomes[] = {
    JournalOutcome::PENDING,
    JournalOutcome::ACCEPTED,
    JournalOutcome::REJECTED,
    JournalOutcome::INDETERMINATE,
  };
  for (const auto outcome : outcomes) {
    const auto name = runner::journal_outcome_name(outcome);
    REQUIRE(runner::journal_outcome_from(name) == outcome);
  }
  REQUIRE_FALSE(runner::journal_outcome_from("nonsense").has_value());
}

TEST_CASE("journal action names round-trip", "[journal]") {
  REQUIRE(runner::journal_action_from(runner::journal_action_name(JournalAction::PLACE)) ==
    JournalAction::PLACE);
  REQUIRE(runner::journal_action_from(runner::journal_action_name(JournalAction::CANCEL)) ==
    JournalAction::CANCEL);
  REQUIRE_FALSE(runner::journal_action_from("place-ish").has_value());
}

TEST_CASE("every send outcome has a journal outcome", "[journal]") {
  REQUIRE(runner::journal_outcome_of(SendOutcome::ACCEPTED) == JournalOutcome::ACCEPTED);
  REQUIRE(runner::journal_outcome_of(SendOutcome::REJECTED) == JournalOutcome::REJECTED);
  REQUIRE(
    runner::journal_outcome_of(SendOutcome::INDETERMINATE) == JournalOutcome::INDETERMINATE
  );
  REQUIRE_FALSE(runner::journal_outcome_is_terminal(JournalOutcome::PENDING));
  REQUIRE(runner::journal_outcome_is_terminal(JournalOutcome::REJECTED));
}

// ---------------------------------------------------------------------------
// Pairing lines into unresolved entries
// ---------------------------------------------------------------------------

namespace {

JournalEntry line(
  const char* id,
  JournalOutcome outcome,
  const InstrumentId& instrument = samsung()
) {
  JournalEntry entry;
  entry.id_ = id;
  entry.outcome_ = outcome;
  entry.instrument_ = instrument;
  entry.recorded_at_ = utc(2026, 3, 30, 0, 30);
  entry.quantity_ = Decimal64::parse("10");
  return entry;
}

} // namespace

TEST_CASE("a pending line answered by a terminal line is resolved", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-1", JournalOutcome::ACCEPTED),
  };
  REQUIRE(runner::unresolved_entries(replay).empty());
}

TEST_CASE("a pending line with no answer stays unresolved", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-1", JournalOutcome::ACCEPTED),
    line("a-2", JournalOutcome::PENDING, hynix()),
  };
  const auto open = runner::unresolved_entries(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().id_ == "a-2");
  REQUIRE(open.front().instrument_ == hynix());
}

TEST_CASE("a terminal line closes only the entry carrying its own id", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-2", JournalOutcome::PENDING, hynix()),
    line("a-2", JournalOutcome::INDETERMINATE, hynix()),
  };
  const auto open = runner::unresolved_entries(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().id_ == "a-1");
}

TEST_CASE("unresolved entries keep the order they were opened in", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-2", JournalOutcome::PENDING, hynix()),
  };
  const auto open = runner::unresolved_entries(replay);
  REQUIRE(open.size() == 2);
  REQUIRE(open[0].id_ == "a-1");
  REQUIRE(open[1].id_ == "a-2");
}

// ---------------------------------------------------------------------------
// The decorator: the entry exists before the send is attempted
// ---------------------------------------------------------------------------

TEST_CASE("a placement is journaled before it is attempted", "[journal]") {
  MemoryJournal journal;
  Journaling executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};
  executor.set_id_prefix("run");

  REQUIRE(executor.place(buy_ten()) == SendOutcome::ACCEPTED);

  // What the inner executor saw at the moment it was called: the pending line, already written.
  REQUIRE(executor.inner().seen().size() == 1);
  const auto& at_send = executor.inner().seen().front();
  REQUIRE(at_send.size() == 1);
  REQUIRE(at_send.front().outcome_ == JournalOutcome::PENDING);
  REQUIRE(at_send.front().action_ == JournalAction::PLACE);
  REQUIRE(at_send.front().instrument_ == samsung());
  REQUIRE(at_send.front().quantity_ == Decimal64::parse("10"));
}

TEST_CASE("a resolution is a second line, not an edit of the first", "[journal]") {
  MemoryJournal journal;
  Journaling executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};
  executor.set_id_prefix("run");

  executor.place(buy_ten());

  const auto& records = journal.records();
  REQUIRE(records.size() == 2);
  REQUIRE(records[0].outcome_ == JournalOutcome::PENDING);
  REQUIRE(records[1].outcome_ == JournalOutcome::ACCEPTED);
  REQUIRE(records[0].id_ == records[1].id_);
  // The terminal line repeats the instruction rather than referring to it, so `tail` on the file
  // tells an operator which instrument it is about.
  REQUIRE(records[1].instrument_ == samsung());
  REQUIRE(records[1].price_ == Decimal64::parse("70000"));
}

TEST_CASE("a failing send still leaves a terminal line", "[journal]") {
  MemoryJournal journal;
  Journaling executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};
  executor.inner().place_outcome_ = SendOutcome::INDETERMINATE;

  REQUIRE(executor.place(buy_ten()) == SendOutcome::INDETERMINATE);

  REQUIRE(journal.records().size() == 2);
  REQUIRE(journal.records()[1].outcome_ == JournalOutcome::INDETERMINATE);
  REQUIRE(runner::unresolved_entries(journal.replay()).empty());
}

TEST_CASE("the decorator returns the inner outcome unchanged", "[journal]") {
  MemoryJournal journal;
  Journaling executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};

  executor.inner().place_outcome_ = SendOutcome::REJECTED;
  REQUIRE(executor.place(buy_ten()) == SendOutcome::REJECTED);
  executor.inner().place_outcome_ = SendOutcome::ACCEPTED;
  REQUIRE(executor.place(buy_ten()) == SendOutcome::ACCEPTED);
  executor.inner().cancel_outcome_ = SendOutcome::INDETERMINATE;
  REQUIRE(executor.cancel(resting("BROKER-1")) == SendOutcome::INDETERMINATE);
}

TEST_CASE("a cancellation is journaled before it is attempted, naming its target", "[journal]") {
  MemoryJournal journal;
  Journaling executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};

  executor.cancel(resting("BROKER-7"));

  REQUIRE(executor.inner().seen().size() == 1);
  const auto& at_send = executor.inner().seen().front();
  REQUIRE(at_send.size() == 1);
  REQUIRE(at_send.front().action_ == JournalAction::CANCEL);
  REQUIRE(at_send.front().outcome_ == JournalOutcome::PENDING);
  // The order id is on the line from the start, because the order it names already exists. That is
  // what makes a cancellation's recovery an identity rather than a guess.
  REQUIRE(at_send.front().order_id_ == "BROKER-7");
}

TEST_CASE("line ids are unique within a run and distinguished across runs", "[journal]") {
  MemoryJournal journal;
  Journaling first{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};
  first.set_id_prefix("run-a");
  first.place(buy_ten());
  first.place(buy_ten(hynix()));

  Journaling second{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};
  second.set_id_prefix("run-b");
  second.place(buy_ten());

  const auto& records = journal.records();
  REQUIRE(records.size() == 6);
  REQUIRE(records[0].id_ == "run-a-1");
  REQUIRE(records[2].id_ == "run-a-2");
  // A restarted process appends to its predecessor's file, so a repeated id would let a stale
  // terminal line close a fresh pending one.
  REQUIRE(records[4].id_ == "run-b-1");
  REQUIRE(runner::unresolved_entries(journal.replay()).empty());
}

TEST_CASE("a journal entry stamps the clock the runner reads", "[journal]") {
  MemoryJournal journal;
  runner::ManualClock clock{utc(2026, 3, 30, 1, 0)};
  runner::JournalingExecutor<ObservingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{ObservingExecutor<runner::DryTag>{&journal}, journal, clock};

  executor.place(buy_ten());

  REQUIRE(journal.records().front().recorded_at_ == utc(2026, 3, 30, 1, 0));
}

TEST_CASE("an accepted placement records the reference the broker issued", "[journal]") {
  MemoryJournal journal;
  runner::JournalingExecutor<ReportingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{ReportingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};
  executor.inner().issued_id_ = "KRX-4412";

  REQUIRE(executor.place(buy_ten()) == SendOutcome::ACCEPTED);

  REQUIRE(journal.records().size() == 2);
  // The pending line has no id, because the order does not exist yet and inventing one would be a
  // claim about the broker.
  REQUIRE(journal.records()[0].order_id_.empty());
  REQUIRE(journal.records()[1].order_id_ == "KRX-4412");
}

TEST_CASE("a failed placement records what the executor said went wrong", "[journal]") {
  MemoryJournal journal;
  runner::JournalingExecutor<ReportingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{ReportingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};
  executor.inner().place_outcome_ = SendOutcome::INDETERMINATE;
  executor.inner().message_ = "the connection was reset";

  executor.place(buy_ten());

  REQUIRE(journal.records()[1].detail_ == "the connection was reset");
  REQUIRE(journal.records()[1].order_id_.empty());
}

TEST_CASE("an executor with nothing to report is still wrappable", "[journal]") {
  // `RecordingExecutor` has no `last_ref` and no `last_error`, and it is what a dry run uses. A
  // decorator that required them would be untestable without a session.
  MemoryJournal journal;
  runner::JournalingExecutor<runner::RecordingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};

  REQUIRE(executor.place(buy_ten()) == SendOutcome::ACCEPTED);
  REQUIRE(journal.records().size() == 2);
  REQUIRE(journal.records()[1].outcome_ == JournalOutcome::ACCEPTED);
  REQUIRE(journal.records()[1].order_id_.empty());
  REQUIRE(executor.inner().placed().size() == 1);
}
