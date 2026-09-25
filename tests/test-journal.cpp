#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
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

TEST_CASE("every order-shape name round-trips through the journal", "[journal]") {
  // Written out rather than looped over a range, so adding an enumerator and
  // not adding it to `order_type_name` fails here. The compiler will not catch
  // that: the switch has no `default:`, which is what makes -Wswitch report a
  // missing case, but the `return "UNKNOWN"` after it means the function still
  // compiles and the new basis is silently journalled as UNKNOWN.
  const OrderType types[] = {
    OrderType::UNKNOWN,
    OrderType::LIMIT,
    OrderType::MARKET,
    OrderType::CONDITIONAL_LIMIT,
    OrderType::BEST_LIMIT,
    OrderType::PRIMARY_LIMIT,
    OrderType::MID_PRICE,
    OrderType::STOP_LIMIT,
    OrderType::CLOSING_PRICE,
  };
  std::vector<std::string> names;
  for (const auto type : types) {
    const auto name = runner::journal_detail::order_type_name(type);
    REQUIRE(runner::journal_detail::order_type_from(name) == type);
    names.emplace_back(name);
  }
  // Two bases sharing a name would round-trip one of them into the other.
  const auto before = names.size();
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  REQUIRE(names.size() == before);

  const TimeInForce conditions[] = {
    TimeInForce::UNKNOWN,
    TimeInForce::DAY,
    TimeInForce::IMMEDIATE_OR_CANCEL,
    TimeInForce::FILL_OR_KILL,
  };
  for (const auto condition : conditions) {
    REQUIRE(
      runner::journal_detail::time_in_force_from(
        runner::journal_detail::time_in_force_name(condition)
      ) == condition
    );
  }

  const OrderSession sessions[] = {
    OrderSession::UNKNOWN,
    OrderSession::REGULAR,
    OrderSession::OPENING_AUCTION,
    OrderSession::CLOSING_AUCTION,
    OrderSession::PRE_MARKET,
    OrderSession::AFTER_HOURS,
    OrderSession::AFTER_HOURS_AUCTION,
    OrderSession::AFTER_MARKET,
    OrderSession::GTP,
  };
  for (const auto session : sessions) {
    REQUIRE(
      runner::journal_detail::order_session_from(
        runner::journal_detail::order_session_name(session)
      ) == session
    );
  }
}

TEST_CASE("a journal written before the shape fields reads as what it recorded", "[journal]") {
  // A line with no `time_in_force` and no `session` key is one this runner
  // wrote before those existed. Every order it could have recorded was a
  // regular-session day order, because that was the only shape either adapter
  // would send -- so reading them as UNKNOWN would make recovery refuse to
  // match orders it can match, and the answer to a failed match is to send
  // again.
  REQUIRE(runner::journal_detail::time_in_force_from("") == TimeInForce::DAY);
  REQUIRE(runner::journal_detail::order_session_from("") == OrderSession::REGULAR);
  // An unrecognized name is not an absent one, and must not be read as DAY.
  REQUIRE(runner::journal_detail::time_in_force_from("UNKNOWN") == TimeInForce::UNKNOWN);
  REQUIRE(runner::journal_detail::order_session_from("UNKNOWN") == OrderSession::UNKNOWN);
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
  REQUIRE(runner::unresolved_instructions(replay).empty());
}

TEST_CASE("a pending line with no answer stays unresolved", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-1", JournalOutcome::ACCEPTED),
    line("a-2", JournalOutcome::PENDING, hynix()),
  };
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().entry_.id_ == "a-2");
  REQUIRE(open.front().entry_.instrument_ == hynix());
}

TEST_CASE("a terminal line closes only the entry carrying its own id", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-2", JournalOutcome::PENDING, hynix()),
    line("a-2", JournalOutcome::ACCEPTED, hynix()),
  };
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().entry_.id_ == "a-1");
}

TEST_CASE("an answer of indeterminate resolves nothing", "[journal]") {
  // The case a restart most easily loses. `Runner` marks its ledger when a send fails in transit,
  // but that mark dies with the process; unless recovery reads the pair back out of the journal, a
  // restarted runner silently forgets an order whose existence is unknown.
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-1", JournalOutcome::INDETERMINATE),
  };
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().answered_);
  // Both lines are present, so the span in which the order may have reached the broker is exact
  // rather than bounded by a guess about how long a request can take.
  REQUIRE(open.front().opened_at_ == utc(2026, 3, 30, 0, 30));
  REQUIRE(open.front().closed_at_ == utc(2026, 3, 30, 0, 30));
}

TEST_CASE("an orphaned indeterminate line is still owed", "[journal]") {
  // A journal whose beginning was rotated or edited away. An orphaned `ACCEPTED` carries its own
  // answer; an orphaned `INDETERMINATE` carries the absence of one.
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::ACCEPTED),
    line("a-2", JournalOutcome::INDETERMINATE, hynix()),
  };
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().entry_.instrument_ == hynix());
}

TEST_CASE("unresolved entries keep the order they were opened in", "[journal]") {
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    line("a-2", JournalOutcome::PENDING, hynix()),
  };
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 2);
  REQUIRE(open[0].entry_.id_ == "a-1");
  REQUIRE(open[1].entry_.id_ == "a-2");
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
  // An answer of "unknown" is not an answer. Both lines are present, so the send's span is exact,
  // but the instruction is still owed -- and a restarted process must read that back out.
  const auto open = runner::unresolved_instructions(journal.replay());
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().answered_);
  REQUIRE(open.front().closed_at_.has_value());
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
  REQUIRE(runner::unresolved_instructions(journal.replay()).empty());
}

TEST_CASE("a journal entry stamps the clock the runner reads", "[journal]") {
  MemoryJournal journal;
  runner::ManualClock clock{utc(2026, 3, 30, 1, 0)};
  runner::JournalingExecutor<ObservingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{ObservingExecutor<runner::DryTag>{&journal}, journal, clock};

  executor.place(buy_ten());

  REQUIRE(journal.records().front().recorded_at_ == utc(2026, 3, 30, 1, 0));
}

TEST_CASE("a journal line records the venue the order was routed to", "[journal]") {
  // **Through `JournalingExecutor::place`, not by building the entry.** The
  // round-trip test above states a route on an entry it constructs itself, so
  // it pins the serializer and says nothing about the production copy --
  // verified, because deleting that copy left the whole suite green.
  //
  // The route is what lets recovery tell two orders in one symbol, side,
  // quantity, price and second apart. A line that loses it makes an unresolved
  // NXT placement and an unrelated KRX order the same row.
  MemoryJournal journal;
  runner::JournalingExecutor<ObservingExecutor<runner::DryTag>, MemoryJournal, runner::ManualClock>
    executor{ObservingExecutor<runner::DryTag>{&journal}, journal, runner::ManualClock{}};

  auto order = buy_ten();
  order.venue_ = Venue::NXT;
  order.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  order.session_ = OrderSession::AFTER_MARKET;
  executor.place(order);

  REQUIRE_FALSE(journal.records().empty());
  const auto& line = journal.records().front();
  REQUIRE(line.venue_ == Venue::NXT);
  // And the identity venue is still the instrument's, which is a different
  // field answering a different question.
  REQUIRE(line.instrument_.venue_ == samsung().venue_);
  // The rest of the shape travels by the same copy, so a regression in one is
  // usually a regression in all of them.
  REQUIRE(line.time_in_force_ == TimeInForce::IMMEDIATE_OR_CANCEL);
  REQUIRE(line.session_ == OrderSession::AFTER_MARKET);
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

// ---------------------------------------------------------------------------
// The file: append, flush, read back
// ---------------------------------------------------------------------------

namespace {

/**
 * Writes `text` to `path` exactly, trailing newline included or omitted.
 *
 * The truncation tests write real files rather than faking a journal, because what is being tested
 * is what a partially completed write actually looks like on disk. A mock would assert the reader
 * against my own idea of a torn line instead of against one.
 */
void write_file(const std::string& path, const std::string& text) {
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  REQUIRE(handle != nullptr);
  if (!text.empty()) {
    REQUIRE(std::fwrite(text.data(), 1, text.size(), handle) == text.size());
  }
  std::fclose(handle);
}

/** The account every journal in these tests belongs to. */
constexpr const char* kScope = "test:account";

/**
 * Writes a journal whose header is intact and whose body is whatever the test wants to break.
 *
 * The header is written before any instruction in a real journal, so a test that hand-builds a
 * damaged file has to do the same or it is testing a file no process could produce.
 */
void write_journal(const std::string& path, const std::string& body, const char* scope = kScope);

/** Returns the whole file as text. */
std::string read_file(const std::string& path) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  REQUIRE(handle != nullptr);
  std::string text;
  char buffer[1024];
  while (const auto read = std::fread(buffer, 1, sizeof(buffer), handle)) {
    text.append(buffer, read);
  }
  std::fclose(handle);
  return text;
}

/**
 * A journal path that exists only for the duration of one test.
 *
 * Removed on the way in *and* on the way out. Only the first is about correctness -- a test must
 * start from nothing -- but leaving the file behind puts an artifact in whatever directory the
 * binary was run from, and a run from the worktree root then offers it to `git add`. Cleaning up is
 * cheaper than remembering to ignore it.
 */
class TempJournal {
public:
  explicit TempJournal(const char* name)
  : path_(std::string("atl-test-journal-") + name + ".jsonl") {
    std::remove(path_.c_str());
  }

  ~TempJournal() { std::remove(path_.c_str()); }

  TempJournal(const TempJournal&) = delete;
  TempJournal& operator=(const TempJournal&) = delete;

  /** Returns the path, so a test reads like it holds a string. */
  operator const std::string&() const noexcept { return path_; }
  const std::string& str() const noexcept { return path_; }

private:
  std::string path_;
};

void write_journal(const std::string& path, const std::string& body, const char* scope) {
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  REQUIRE(handle != nullptr);
  const auto header = runner::journal_header_line(scope) + "\n";
  REQUIRE(std::fwrite(header.data(), 1, header.size(), handle) == header.size());
  if (!body.empty()) {
    REQUIRE(std::fwrite(body.data(), 1, body.size(), handle) == body.size());
  }
  std::fclose(handle);
}

JournalEntry sample_entry() {
  JournalEntry entry;
  entry.id_ = "run-1";
  entry.action_ = JournalAction::PLACE;
  entry.outcome_ = JournalOutcome::PENDING;
  entry.recorded_at_ = utc(2026, 3, 30, 1, 15);
  entry.instrument_ = samsung();
  entry.side_ = OrderSide::SELL;
  entry.type_ = OrderType::BEST_LIMIT;
  entry.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  entry.session_ = OrderSession::AFTER_MARKET;
  entry.venue_ = Venue::NXT;
  entry.quantity_ = Decimal64::parse("10");
  entry.price_ = Decimal64::parse("70000");
  return entry;
}

} // namespace

TEST_CASE("a journal entry survives a round trip through a line", "[journal][file]") {
  const auto original = sample_entry();
  const auto parsed = runner::journal_entry_from_line(runner::journal_entry_to_line(original));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->id_ == original.id_);
  REQUIRE(parsed->action_ == original.action_);
  REQUIRE(parsed->outcome_ == original.outcome_);
  REQUIRE(parsed->instrument_ == original.instrument_);
  REQUIRE(parsed->side_ == original.side_);
  REQUIRE(parsed->type_ == original.type_);
  REQUIRE(parsed->time_in_force_ == original.time_in_force_);
  REQUIRE(parsed->session_ == original.session_);
  // The route, which is not `instrument_.venue_`: recovery compares both, and
  // an entry that lost the route cannot tell two orders in one symbol apart.
  REQUIRE(parsed->venue_ == original.venue_);
  REQUIRE(parsed->quantity_ == original.quantity_);
  REQUIRE(parsed->price_ == original.price_);
  // Exactly, not approximately: the matching window is measured against this.
  REQUIRE(parsed->recorded_at_ == original.recorded_at_);
}

TEST_CASE("an absent price round-trips as absent, not as zero", "[journal][file]") {
  auto entry = sample_entry();
  entry.price_.reset();
  entry.type_ = OrderType::MARKET;
  const auto parsed = runner::journal_entry_from_line(runner::journal_entry_to_line(entry));
  REQUIRE(parsed.has_value());
  REQUIRE_FALSE(parsed->price_.has_value());
  REQUIRE(parsed->type_ == OrderType::MARKET);
}

TEST_CASE("a line this library did not write is not an entry", "[journal][file]") {
  REQUIRE_FALSE(runner::journal_entry_from_line("").has_value());
  REQUIRE_FALSE(runner::journal_entry_from_line("not json at all").has_value());
  // Well-formed JSON, but not a journal line. Accepting it would manufacture an entry with a
  // default instrument, which is a claim about an order that was never made.
  REQUIRE_FALSE(runner::journal_entry_from_line(R"({"hello":"world"})").has_value());
}

TEST_CASE("appends accumulate, one whole line each", "[journal][file]") {
  const TempJournal path("append");
  {
    runner::FileJournal journal(path.str(), "test:account");
    REQUIRE(journal.open());
    auto entry = sample_entry();
    REQUIRE(journal.append(entry));
    entry.outcome_ = JournalOutcome::ACCEPTED;
    entry.order_id_ = "KRX-99";
    REQUIRE(journal.append(entry));
  }

  const auto text = read_file(path.str());
  REQUIRE(text.back() == '\n');
  // The header, then one line per record. The header is written when the journal is first claimed,
  // before any instruction can be contemplated, which is what makes ownership legible even if the
  // very first order tears.
  REQUIRE(std::count(text.begin(), text.end(), '\n') == 3);
  REQUIRE(runner::journal_header_scope(text.substr(0, text.find('\n'))) == "test:account");

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.intact());
  REQUIRE(replay.entries_.size() == 2);
  REQUIRE(replay.entries_[1].order_id_ == "KRX-99");
  REQUIRE(runner::unresolved_instructions(replay).empty());
}

TEST_CASE("a restarted process appends to its predecessor's file", "[journal][file]") {
  const TempJournal path("restart");
  {
    runner::FileJournal first(path.str(), "test:account");
    REQUIRE(first.append(sample_entry()));
  }
  {
    runner::FileJournal second(path.str(), "test:account");
    auto entry = sample_entry();
    entry.id_ = "run-2";
    REQUIRE(second.append(entry));
    REQUIRE(second.replay().entries_.size() == 2);
  }
}

TEST_CASE("an absent journal is intact and empty, not an error", "[journal][file]") {
  const TempJournal path("absent");
  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.intact());
  REQUIRE(replay.entries_.empty());
  REQUIRE(replay.detail_.empty());
}

// ---------------------------------------------------------------------------
// Truncation: the case the format exists for
// ---------------------------------------------------------------------------

TEST_CASE("a truncated final line is reported and treated as unresolved", "[journal][file]") {
  const TempJournal path("truncated");

  auto complete = sample_entry();
  complete.id_ = "run-1";
  auto resolved = complete;
  resolved.outcome_ = JournalOutcome::ACCEPTED;

  auto torn = sample_entry();
  torn.id_ = "run-2";
  torn.instrument_ = hynix();
  const auto torn_line = runner::journal_entry_to_line(torn);

  // A process killed mid-append leaves the prefix of its last line and no newline. Cut at two
  // thirds so the instrument is still legible, which is the case worth salvaging.
  write_journal(
    path.str(),
    runner::journal_entry_to_line(complete) + "\n" +
      runner::journal_entry_to_line(resolved) + "\n" +
      torn_line.substr(0, torn_line.size() * 2 / 3)
  );

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.truncated_tail_);
  REQUIRE_FALSE(replay.intact());
  REQUIRE(replay.detail_.find("000660") != std::string::npos);

  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().entry_.instrument_ == hynix());
  // Its id is rewritten so that nothing can ever close it: the `outcome` field of a torn line may
  // itself be the part that was cut off.
  REQUIRE(open.front().entry_.id_ != "run-2");
  REQUIRE(open.front().entry_.outcome_ == JournalOutcome::PENDING);
  // And flagged, which is what actually makes it unresolvable. Without this the entry's missing
  // timestamp reads as the epoch, so the window is long closed and no real row can fall inside it:
  // the very first sweep would call it `not_placed` and clear the ledger.
  REQUIRE(open.front().entry_.requires_operator_);
}

TEST_CASE("a truncated line is never allowed to resolve a pending one", "[journal][file]") {
  const TempJournal path("truncated-terminal");

  auto pending = sample_entry();
  pending.id_ = "run-1";
  auto terminal = pending;
  terminal.outcome_ = JournalOutcome::ACCEPTED;
  terminal.order_id_ = "KRX-1";
  const auto terminal_line = runner::journal_entry_to_line(terminal);

  // The line that was cut off is the one that *answers* the pending line. A reader that trusted
  // its legible prefix would mark the order resolved on the strength of a write that never
  // finished.
  write_journal(
    path.str(),
    runner::journal_entry_to_line(pending) + "\n" +
      terminal_line.substr(0, terminal_line.size() * 2 / 3)
  );

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.truncated_tail_);
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 2);
  REQUIRE(open[0].entry_.id_ == "run-1");
  REQUIRE(open[1].entry_.instrument_ == samsung());
}

TEST_CASE("a truncated line naming no instrument is still reported", "[journal][file]") {
  const TempJournal path("truncated-early");
  write_journal(path.str(), runner::journal_entry_to_line(sample_entry()) + "\n" + R"({"id":"run-2","ac)");

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.truncated_tail_);
  REQUIRE(replay.entries_.size() == 1);
  REQUIRE(replay.detail_.find("something was in flight") != std::string::npos);
}

TEST_CASE("a complete line that is not a journal entry is counted", "[journal][file]") {
  const TempJournal path("foreign");
  write_journal(
    path.str(), runner::journal_entry_to_line(sample_entry()) + "\n" + R"({"hello":"world"})" + "\n"
  );

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE_FALSE(replay.truncated_tail_);
  REQUIRE(replay.unreadable_lines_ == 1);
  REQUIRE(replay.entries_.size() == 1);
  REQUIRE_FALSE(replay.intact());
}

// ---------------------------------------------------------------------------
// A journal that cannot be written stops the send
// ---------------------------------------------------------------------------

namespace {

/** A journal that refuses to record, standing in for a full disk. */
class FailingJournal {
public:
  bool append(const JournalEntry&) {
    ++appends_;
    return false;
  }
  runner::JournalReplay replay() const { return {}; }

  /** How many records were offered, so a test can prove a send was gated rather than attempted. */
  std::size_t appends() const noexcept { return appends_; }

private:
  std::size_t appends_{0};
};

} // namespace

TEST_CASE("an instruction that cannot be journaled is not sent", "[journal]") {
  FailingJournal journal;
  runner::JournalingExecutor<runner::RecordingExecutor<runner::DryTag>, FailingJournal,
    runner::ManualClock>
    executor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};

  // `UNAVAILABLE` rather than `REJECTED`, and the distinction is the whole point: both mean the
  // order does not exist, but a rejection is the broker's answer about one instruction while this
  // is the local machinery failing, which says nothing about the instruction and everything about
  // whether the runner should still be trading.
  REQUIRE(executor.place(buy_ten()) == SendOutcome::UNAVAILABLE);
  REQUIRE_FALSE(runner::send_outcome_reached_broker(SendOutcome::UNAVAILABLE));
  // Two outcomes leave the broker unasked, and both must answer the same here -- the question is
  // whether a request went out, which is what a rate-limit window and a request count depend on.
  REQUIRE_FALSE(runner::send_outcome_reached_broker(SendOutcome::REFUSED_LOCALLY));
  REQUIRE(runner::send_outcome_reached_broker(SendOutcome::ACCEPTED));
  REQUIRE(runner::send_outcome_reached_broker(SendOutcome::REJECTED));
  REQUIRE(runner::send_outcome_reached_broker(SendOutcome::INDETERMINATE));
  REQUIRE(executor.inner().placed().empty());
  REQUIRE(executor.unjournaled_refusals() == 1);
  REQUIRE_FALSE(executor.last_journal_error().empty());

  REQUIRE(executor.cancel(resting("BROKER-1")) == SendOutcome::UNAVAILABLE);
  REQUIRE(executor.inner().canceled().empty());
  REQUIRE(executor.unjournaled_refusals() == 2);
}

TEST_CASE("the first journal failure is the one reported", "[journal]") {
  // A journal that failed once will fail again. Overwriting with the latest reason would make an
  // operator chase the last symptom rather than the first cause.
  FailingJournal journal;
  runner::JournalingExecutor<runner::RecordingExecutor<runner::DryTag>, FailingJournal,
    runner::ManualClock>
    executor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};

  executor.place(buy_ten());
  const auto first = executor.last_journal_error();
  executor.place(buy_ten(hynix()));
  REQUIRE(executor.last_journal_error() == first);
}

namespace {

/** A journal that records a fixed number of lines and then refuses, until it is repaired. */
class HalfFailingJournal {
public:
  bool append(const JournalEntry& entry) {
    if (repaired_) {
      entries_.push_back(entry);
      return true;
    }
    if (accepted_ >= limit_) {
      return false;
    }
    ++accepted_;
    entries_.push_back(entry);
    return true;
  }

  /** Stands in for an operator emptying the disk. */
  void repair() { repaired_ = true; }

  runner::JournalReplay replay() const {
    runner::JournalReplay replay;
    replay.entries_ = entries_;
    return replay;
  }

  std::size_t limit_{1};

  /** Everything recorded, so a test can count sends that actually happened. */
  const std::vector<JournalEntry>& records() const noexcept { return entries_; }

private:
  std::size_t accepted_{0};
  bool repaired_{false};
  std::vector<JournalEntry> entries_;
};

} // namespace

TEST_CASE("a terminal line that cannot be written is reported, not swallowed", "[journal]") {
  // The send already happened, so refusing it now would be a lie. What is lost is the *answer*,
  // which leaves the instruction looking unresolved to the next recovery -- one wasted
  // reconciliation, which is the safe direction. The failure is still recorded so the runner stops.
  HalfFailingJournal journal;
  runner::JournalingExecutor<runner::RecordingExecutor<runner::DryTag>, HalfFailingJournal,
    runner::ManualClock>
    executor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}};

  REQUIRE(executor.place(buy_ten()) == SendOutcome::ACCEPTED);
  REQUIRE(executor.inner().placed().size() == 1);
  REQUIRE_FALSE(executor.last_journal_error().empty());
  // The pending line stands alone, so recovery treats the instruction as owed.
  REQUIRE(runner::unresolved_instructions(journal.replay()).size() == 1);
}

TEST_CASE("a journal that could not be opened reports it", "[journal][file]") {
  runner::FileJournal journal("atl-test-journal-missing-directory/nowhere.jsonl", "test:account");
  REQUIRE_FALSE(journal.open());
  REQUIRE_FALSE(journal.append(sample_entry()));
}

// ---------------------------------------------------------------------------
// A broken journal stops the runner, rather than producing a refusal per bar
// ---------------------------------------------------------------------------

namespace {

/** A strategy that asks for a fixed position every bar, so every bar produces an instruction. */
struct HoldTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
  }
};

/** A second instrument, so a batch can carry more than one instruction. */
Instrument hynix_reference() {
  Instrument reference;
  reference.id_ = hynix();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/** Withdraws every resting order it can see, so a cancellation reaches the executor. */
struct WithdrawAll {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "withdraw-all");
    }
  }
};

/** Asks for a position in two instruments on every bar. */
struct HoldTwo {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    for (const auto& id : {samsung(), hynix()}) {
      strategy::TargetPosition target;
      target.instrument_ = id;
      target.target_quantity_ = Decimal64::parse("10");
      ctx.submit(target);
    }
  }
};

Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

strategy::BarEvent bar_at(int day) {
  Bar bar;
  bar.timestamp_ = utc(2026, 3, day, 6, 30);
  bar.high_ = Decimal64::parse("70000");
  bar.low_ = bar.high_;
  bar.close_ = bar.high_;
  bar.volume_ = Decimal64::parse("1000000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar);
}

using BrokenExecutor = runner::JournalingExecutor<
  runner::RecordingExecutor<runner::DryTag>, FailingJournal, runner::ManualClock>;

} // namespace

TEST_CASE("a broken journal stops the runner rather than refusing per bar", "[journal]") {
  FailingJournal journal;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, BrokenExecutor> dry{
    HoldTen{},
    {},
    512,
    BrokenExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto report = dry.feed(bar_at(30));
  const auto before_appends = journal.appends();

  // Reported as what it is, in its own bucket. Not `rejected_`, which would read as the broker
  // declining an instruction it never saw.
  REQUIRE(report.unavailable_.size() == 1);
  REQUIRE(report.rejected_.empty());
  REQUIRE(report.would_place_.empty());
  REQUIRE(report.unavailable_.front().reason_ == runner::WithheldReason::LOCAL_FAILURE);
  REQUIRE_FALSE(report.unavailable_.front().detail_.empty());

  // Sticky: the next bar does not get to try again and produce a second mystery refusal.
  REQUIRE(dry.local_failure().has_value());

  // And the instrument is *not* withheld. Nothing was attempted, so nothing is in doubt about the
  // position -- marking it would withhold it for the wrong reason.
  REQUIRE(dry.ledger().empty());

  // The next bar is refused without reaching the executor at all. The earlier version of this test
  // asserted only that the flag existed and never fed a second bar, so it passed while `settle()`
  // went on calling `place` for every instruction after the first failure.
  const auto second = dry.feed(bar_at(31));
  REQUIRE(second.unavailable_.size() == 1);
  REQUIRE(second.would_place_.empty());
  REQUIRE(journal.appends() == before_appends);

  dry.resume_after_local_failure();
  REQUIRE_FALSE(dry.local_failure().has_value());
}

TEST_CASE("a batch stops at its first journal failure", "[journal]") {
  // One instruction per instrument in a single `settle()`. A journal that failed on the first will
  // fail on the rest, so carrying on produces one unexplained refusal per instrument.
  FailingJournal journal;
  runner::Runner<HoldTwo, strategy::NoCaps, runner::DryTag, BrokenExecutor> dry{
    HoldTwo{},
    {},
    512,
    BrokenExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_instrument(hynix_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));
  dry.context().set_last_price(hynix(), Decimal64::parse("70000"));

  const auto report = dry.feed(bar_at(30));
  REQUIRE(report.unavailable_.size() == 2);
  REQUIRE(report.would_place_.empty());
  // Only the first instruction reached the journal; the second was gated before the send.
  REQUIRE(journal.appends() == 1);
}

TEST_CASE("a terminal journal failure stops the runner too", "[journal]") {
  // The broker already answered, so the outcome passed back stays the broker's -- saying otherwise
  // would be a lie about an order that exists. But the journal is broken, and the earlier code
  // recorded that only on the executor, where nothing read it. The comment claimed the runner
  // stopped; it did not.
  HalfFailingJournal journal;
  using HalfExecutor = runner::JournalingExecutor<
    runner::RecordingExecutor<runner::DryTag>, HalfFailingJournal, runner::ManualClock>;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, HalfExecutor> dry{
    HoldTen{},
    {},
    512,
    HalfExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto first = dry.feed(bar_at(30));
  // The order really was sent and really was accepted.
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE(first.unavailable_.empty());
  // And the runner is now halted, so nothing further goes out.
  REQUIRE(dry.local_failure().has_value());

  const auto second = dry.feed(bar_at(31));
  REQUIRE(second.unavailable_.size() == 1);
  REQUIRE(second.would_place_.empty());
}

TEST_CASE("a halted runner sends no cancellation either", "[journal]") {
  FailingJournal journal;
  runner::Runner<WithdrawAll, strategy::NoCaps, runner::DryTag, BrokenExecutor> dry{
    WithdrawAll{},
    {},
    512,
    BrokenExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.halt_locally("the order journal could not be written");

  strategy::Portfolio<runner::DryTag> portfolio;
  WorkingOrder<runner::DryTag> resting_order;
  resting_order.order_.instrument_ = samsung();
  resting_order.order_.side_ = OrderSide::BUY;
  resting_order.order_.type_ = OrderType::LIMIT;
  resting_order.order_.quantity_ = Decimal64::parse("10");
  resting_order.order_.price_ = Decimal64::parse("70000");
  resting_order.ref_.primary_ = "KRX-1";
  portfolio.working_.push_back(resting_order);
  dry.context().set_portfolio(portfolio);

  const auto report = dry.feed(bar_at(30));

  // Reported in its own bucket, not as a withdrawal that happened and not as one the snapshot
  // could not describe.
  REQUIRE(report.unavailable_cancel_.size() == 1);
  REQUIRE(report.would_cancel_.empty());
  REQUIRE(report.unresolved_cancel_.empty());
  REQUIRE(journal.appends() == 0);
}

// ---------------------------------------------------------------------------
// A damaged journal must not repair itself into a wrong answer
// ---------------------------------------------------------------------------

TEST_CASE("a salvaged entry is never resolved by a sweep", "[journal][file]") {
  const TempJournal path("salvage-verdict");
  auto torn = sample_entry();
  torn.id_ = "run-2";
  const auto torn_line = runner::journal_entry_to_line(torn);
  write_journal(path.str(), torn_line.substr(0, torn_line.size() * 2 / 3));

  const auto replay = runner::read_journal_file(path.str());
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);

  runner::IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(replay, ledger);
  REQUIRE(recovery.marked_.size() == 1);

  // Years later, with a listing that plainly does not contain it. Before the flag existed this
  // returned `not_placed` and cleared the ledger -- resolving on missing data, in the one direction
  // that can leave a live order unwatched.
  MemoryJournal sink;
  const auto swept = runner::reconcile_against(
    recovery.open_, {}, utc(2030, 1, 1, 0, 0), ledger, sink
  );
  // Nothing was written back either, because nothing was decided.
  REQUIRE(swept.recorded_ == 0);
  REQUIRE(swept.matches_.front().verdict_ == runner::MatchVerdict::NEEDS_OPERATOR);
  REQUIRE_FALSE(runner::match_verdict_resolves(runner::MatchVerdict::NEEDS_OPERATOR));
  REQUIRE_FALSE(swept.settled());
  REQUIRE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("opening a torn journal terminates the fragment before appending", "[journal][file]") {
  // Appending straight onto a line with no newline glues the next object to the fragment. The
  // combined text then carries a newline, so the following replay reads one complete unreadable
  // line -- losing the fragment *and* the new instruction with it.
  const TempJournal path("torn-append");
  auto torn = sample_entry();
  torn.id_ = "run-1";
  const auto torn_line = runner::journal_entry_to_line(torn);
  write_journal(path.str(), torn_line.substr(0, torn_line.size() * 2 / 3));

  auto fresh = sample_entry();
  fresh.id_ = "run-2";
  fresh.instrument_ = hynix();
  {
    runner::FileJournal journal(path.str(), "test:account");
    REQUIRE(journal.open());
    REQUIRE_FALSE(journal.repaired().empty());
    REQUIRE(journal.append(fresh));
  }

  const auto replay = runner::read_journal_file(path.str());
  // The fragment is now a complete line that does not parse, counted rather than silently dropped.
  REQUIRE(replay.unreadable_lines_ == 1);
  REQUIRE_FALSE(replay.truncated_tail_);

  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 2);
  // The salvaged stand-in for the fragment, still operator-only.
  REQUIRE(open[0].entry_.requires_operator_);
  REQUIRE(open[0].entry_.instrument_ == samsung());
  // And the new instruction, intact rather than swallowed by the fragment.
  REQUIRE(open[1].entry_.id_ == "run-2");
  REQUIRE(open[1].entry_.instrument_ == hynix());
  REQUIRE_FALSE(open[1].entry_.requires_operator_);
}

TEST_CASE("a repair survives a second restart", "[journal][file]") {
  const TempJournal path("torn-twice");
  auto torn = sample_entry();
  const auto torn_line = runner::journal_entry_to_line(torn);
  write_journal(path.str(), torn_line.substr(0, torn_line.size() * 2 / 3));

  {
    runner::FileJournal first(path.str(), "test:account");
    REQUIRE(first.open());
  }
  {
    // Nothing left to repair: the file is well formed now.
    runner::FileJournal second(path.str(), "test:account");
    REQUIRE(second.open());
    REQUIRE(second.repaired().empty());
  }

  runner::IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(runner::read_journal_file(path.str()), ledger);
  REQUIRE(recovery.open_.size() == 1);
  REQUIRE(recovery.open_.front().entry_.requires_operator_);
  REQUIRE(ledger.requires_reconcile(samsung()));
}

TEST_CASE("a torn line naming nothing halts the runner outright", "[journal][file]") {
  // There is no instrument to withhold, so withholding one is not an option and withholding
  // everything is the only honest response. The loop's startup gate is not enough on its own: it
  // lifts after one order-list read, and a read cannot answer a question nobody can pose.
  const TempJournal path("torn-nameless");
  write_journal(path.str(), R"({"id":"run-2","ac)");

  {
    runner::FileJournal journal(path.str(), "test:account");
    REQUIRE(journal.open());
  }

  runner::DryRunner<HoldTen> dry;
  const auto recovery = runner::recover_runner(dry, runner::FileJournal(path.str(), "test:account"));
  REQUIRE(recovery.unattributable_);
  REQUIRE(dry.local_failure().has_value());
  REQUIRE(dry.ledger().empty());
}

// ---------------------------------------------------------------------------
// One journal, one account
// ---------------------------------------------------------------------------

TEST_CASE("a journal records which account it belongs to", "[journal][file]") {
  const TempJournal path("scope");
  {
    runner::FileJournal journal(path.str(), "kis:live:12345678-01");
    REQUIRE(journal.open());
    // Stamped by the journal rather than the caller, so a record cannot reach the file without one.
    REQUIRE(journal.append(sample_entry()));
  }
  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.entries_.front().scope_ == "kis:live:12345678-01");
}

TEST_CASE("a journal refuses to open for a different account", "[journal][file]") {
  // Reusing a path across accounts is an easy mistake and not a harmless one: symbol, side,
  // quantity and price can all agree across two accounts trading the same instrument, so a sweep
  // would reconcile one account's unresolved instruction against the other's rows and clear a hold
  // for an order that is still resting.
  const TempJournal path("scope-clash");
  {
    runner::FileJournal journal(path.str(), "kis:live:12345678-01");
    REQUIRE(journal.append(sample_entry()));
  }

  runner::FileJournal other(path.str(), "toss:live:99");
  REQUIRE_FALSE(other.open());
  REQUIRE_FALSE(other.rejection().empty());
  REQUIRE(other.rejection().find("kis:live:12345678-01") != std::string::npos);
  // And it writes nothing, so the first account's journal is left exactly as it was.
  REQUIRE_FALSE(other.append(sample_entry()));
  REQUIRE(runner::read_journal_file(path.str()).entries_.size() == 1);
}

TEST_CASE("a journal reopens for the account that wrote it", "[journal][file]") {
  const TempJournal path("scope-same");
  {
    runner::FileJournal journal(path.str(), "kis:live:12345678-01");
    REQUIRE(journal.append(sample_entry()));
  }
  runner::FileJournal again(path.str(), "kis:live:12345678-01");
  REQUIRE(again.open());
  REQUIRE(again.rejection().empty());
}

TEST_CASE("the account check runs before the torn-tail repair", "[journal][file]") {
  // The repair writes to the file. Doing it before the account is checked would modify a journal
  // that turns out not to be ours.
  const TempJournal path("scope-torn");
  auto entry = sample_entry();
  entry.scope_ = "kis:live:12345678-01";
  const auto line = runner::journal_entry_to_line(entry);
  const auto torn = runner::journal_entry_to_line(entry);
  write_journal(path.str(), line + "\n" + torn.substr(0, torn.size() * 2 / 3));
  const auto before = read_file(path.str());

  runner::FileJournal other(path.str(), "toss:live:99");
  REQUIRE_FALSE(other.open());
  REQUIRE(read_file(path.str()) == before);
}

TEST_CASE("the first record tearing cannot hide the account", "[journal][file]") {
  // The hole the header closes. Reading ownership from the records leaves it exactly where it
  // matters most: `scope` is one field among many on a record, so a process killed partway through
  // writing the *first* instruction leaves a file whose only line is torn and unscoped. Another
  // account could then claim the path and reconcile the first account's instruction against its own
  // rows.
  const TempJournal path("scope-first-tear");
  auto entry = sample_entry();
  const auto line = runner::journal_entry_to_line(entry);
  // Cut before the trailing `scope` field ever reaches the file.
  const auto torn = line.substr(0, line.find("\"scope\""));
  REQUIRE(torn.find("scope") == std::string::npos);
  write_journal(path.str(), torn, "kis:live:12345678-01");

  runner::FileJournal other(path.str(), "toss:live:99");
  REQUIRE_FALSE(other.open());
  REQUIRE(other.rejection().find("kis:live:12345678-01") != std::string::npos);

  // The owner still opens it, and the torn record is repaired into its journal.
  runner::FileJournal owner(path.str(), "kis:live:12345678-01");
  REQUIRE(owner.open());
  REQUIRE_FALSE(owner.repaired().empty());
}

TEST_CASE("a journal must name an account", "[journal][file]") {
  const TempJournal path("scope-empty");
  runner::FileJournal journal(path.str(), "");
  REQUIRE_FALSE(journal.open());
  REQUIRE_FALSE(journal.rejection().empty());
}

TEST_CASE("a file with no header is not ours to write to", "[journal][file]") {
  // Ownership cannot be established, and that is not a question to guess at while holding a file
  // that may name another account's live orders.
  const TempJournal path("scope-headerless");
  write_file(path.str(), runner::journal_entry_to_line(sample_entry()) + "\n");

  runner::FileJournal journal(path.str(), "test:account");
  REQUIRE_FALSE(journal.open());
  REQUIRE(journal.rejection().find("no header") != std::string::npos);
}

TEST_CASE("the journal owns the scope field, and overwrites a caller's", "[journal][file]") {
  // A caller-supplied scope that disagreed with the file's would put a record in this journal
  // claiming to belong to another account.
  const TempJournal path("scope-overwrite");
  runner::FileJournal journal(path.str(), "test:account");
  auto entry = sample_entry();
  entry.scope_ = "somebody:else";
  REQUIRE(journal.append(entry));

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.entries_.front().scope_ == "test:account");
}

TEST_CASE("a repair interrupted before its stand-in halts the next process", "[journal][file]") {
  // `finish_repair` terminates a torn fragment and then records a stand-in for it. A process killed
  // between those two leaves the fragment as a *complete* unreadable line, so the next open sees no
  // torn tail and performs no repair -- and before this, `unreadable_lines_` only made `clean()`
  // false and had no safety effect, so the runner traded on.
  const TempJournal path("repair-interrupted");
  auto entry = sample_entry();
  const auto line = runner::journal_entry_to_line(entry);
  // The fragment, terminated, with no stand-in after it: exactly what the interruption leaves.
  write_journal(path.str(), line.substr(0, line.size() * 2 / 3) + "\n");

  const auto replay = runner::read_journal_file(path.str());
  REQUIRE_FALSE(replay.truncated_tail_);
  REQUIRE(replay.unreadable_lines_ == 1);
  REQUIRE(replay.entries_.empty());

  runner::DryRunner<HoldTen> dry;
  const auto recovery = runner::recover_runner(dry, runner::FileJournal(path.str(), "test:account"));
  REQUIRE(recovery.unreadable_lines_ == 1);
  REQUIRE_FALSE(recovery.clean());
  // Nothing can be withheld specifically, so everything is.
  REQUIRE(dry.ledger().empty());
  REQUIRE(dry.local_failure().has_value());
}

TEST_CASE("an intact journal does not halt recovery", "[journal][file]") {
  const TempJournal path("recover-clean");
  {
    runner::FileJournal journal(path.str(), "test:account");
    auto entry = sample_entry();
    REQUIRE(journal.append(entry));
    entry.outcome_ = JournalOutcome::ACCEPTED;
    REQUIRE(journal.append(entry));
  }
  runner::DryRunner<HoldTen> dry;
  const auto recovery = runner::recover_runner(dry, runner::FileJournal(path.str(), "test:account"));
  REQUIRE(recovery.clean());
  REQUIRE_FALSE(dry.local_failure().has_value());
}

// ---------------------------------------------------------------------------
// A journal that cannot be read is not an empty journal
// ---------------------------------------------------------------------------

TEST_CASE("a missing journal and an unreadable one are different things", "[journal][file]") {
  // Collapsing them is a live hazard: a first start owes nothing, while a journal that *exists* and
  // cannot be read may hold every order a predecessor left in flight.
  const TempJournal path("read-status");
  REQUIRE(runner::read_journal_bytes(path.str()).status_ == runner::JournalReadStatus::MISSING);

  {
    runner::FileJournal journal(path.str(), "test:account");
    REQUIRE(journal.append(sample_entry()));
  }
  REQUIRE(runner::read_journal_bytes(path.str()).status_ == runner::JournalReadStatus::READ);
}

TEST_CASE("an unreadable journal is parsed as unreadable, not as empty", "[journal][file]") {
  // Driven through the parse seam rather than by arranging a file permission, because what is being
  // tested is the decision, not the platform's enforcement of a mode bit.
  runner::JournalBytes bytes;
  bytes.status_ = runner::JournalReadStatus::UNREADABLE;
  bytes.detail_ = "the order journal at /some/path exists and could not be opened for reading";

  const auto replay = runner::parse_journal_bytes(bytes);
  REQUIRE_FALSE(replay.readable());
  REQUIRE_FALSE(replay.intact());
  REQUIRE(replay.entries_.empty());
  REQUIRE(replay.detail_ == bytes.detail_);
  // Crucially not mistaken for a file with no header, which would be a different refusal.
  REQUIRE_FALSE(replay.has_header_);
}

TEST_CASE("recovery halts when the journal cannot be read", "[journal][file]") {
  runner::JournalBytes bytes;
  bytes.status_ = runner::JournalReadStatus::UNREADABLE;
  bytes.detail_ = "the order journal exists and could not be opened for reading";

  runner::DryRunner<HoldTen> dry;
  const auto recovery = runner::recover_into(runner::parse_journal_bytes(bytes), dry.ledger());
  REQUIRE(recovery.unreadable_file_);
  REQUIRE_FALSE(recovery.clean());

  // And through the entry point a program actually uses.
  struct UnreadableJournal {
    runner::JournalReplay replay() const {
      runner::JournalBytes bytes;
      bytes.status_ = runner::JournalReadStatus::UNREADABLE;
      bytes.detail_ = "the order journal exists and could not be opened for reading";
      return runner::parse_journal_bytes(bytes);
    }
  };
  runner::DryRunner<HoldTen> halted;
  const auto report = runner::recover_runner(halted, UnreadableJournal{});
  REQUIRE(report.unreadable_file_);
  REQUIRE(halted.local_failure().has_value());
  // Nothing specific can be withheld, so everything is.
  REQUIRE(halted.ledger().empty());
}

TEST_CASE("a missing journal recovers clean", "[journal][file]") {
  runner::JournalBytes bytes;
  bytes.status_ = runner::JournalReadStatus::MISSING;

  runner::DryRunner<HoldTen> dry;
  const auto recovery = runner::recover_into(runner::parse_journal_bytes(bytes), dry.ledger());
  REQUIRE(recovery.clean());
  REQUIRE_FALSE(recovery.unreadable_file_);
}

// ---------------------------------------------------------------------------
// Resuming after the storage is fixed
// ---------------------------------------------------------------------------

namespace {

/** A journal that refuses a fixed number of times and then works. */
class RepairableJournal {
public:
  bool append(const JournalEntry& entry) {
    if (failures_remaining_ > 0) {
      --failures_remaining_;
      return false;
    }
    entries_.push_back(entry);
    return true;
  }

  runner::JournalReplay replay() const {
    runner::JournalReplay replay;
    replay.entries_ = entries_;
    return replay;
  }

  const std::vector<JournalEntry>& records() const noexcept { return entries_; }

  std::size_t failures_remaining_{1};

private:
  std::vector<JournalEntry> entries_;
};

using RepairableExecutor = runner::JournalingExecutor<
  runner::RecordingExecutor<runner::DryTag>, RepairableJournal, runner::ManualClock>;

} // namespace

TEST_CASE("resuming after a repaired journal actually resumes", "[journal]") {
  // The API is documented as the way back from a storage failure, and before this it could not get
  // there: the executor keeps its first error forever, so the runner read that stale reason after
  // the very next send and halted again immediately. The old test asserted only that the flag was
  // cleared and never sent again.
  RepairableJournal journal;
  journal.failures_remaining_ = 1;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, RepairableExecutor> dry{
    HoldTen{},
    {},
    512,
    RepairableExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto failed = dry.feed(bar_at(30));
  REQUIRE(failed.unavailable_.size() == 1);
  REQUIRE(dry.local_failure().has_value());

  // The operator fixes the disk and resumes.
  dry.resume_after_local_failure();
  REQUIRE_FALSE(dry.local_failure().has_value());

  // Two successful sends, not one. The second is what the stale-error bug broke.
  const auto first = dry.feed(bar_at(31));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE_FALSE(dry.local_failure().has_value());

  const auto second = dry.feed(bar_at(32));
  REQUIRE(second.would_place_.size() == 1);
  REQUIRE_FALSE(dry.local_failure().has_value());
}

TEST_CASE("resuming after a terminal-write failure also resumes", "[journal]") {
  // The other half: the broker answered, the journal could not record the answer, the runner
  // halted. A repair and a resume must get it trading again.
  HalfFailingJournal journal;
  journal.limit_ = 1;
  using HalfExecutor = runner::JournalingExecutor<
    runner::RecordingExecutor<runner::DryTag>, HalfFailingJournal, runner::ManualClock>;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, HalfExecutor> dry{
    HoldTen{},
    {},
    512,
    HalfExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  dry.feed(bar_at(30));
  REQUIRE(dry.local_failure().has_value());

  dry.executor().inner();  // the send really did happen
  journal.repair();
  dry.resume_after_local_failure();

  const auto first = dry.feed(bar_at(31));
  REQUIRE(first.would_place_.size() == 1);
  REQUIRE_FALSE(dry.local_failure().has_value());

  const auto second = dry.feed(bar_at(32));
  REQUIRE(second.would_place_.size() == 1);
  REQUIRE_FALSE(dry.local_failure().has_value());
}

TEST_CASE("resuming without fixing anything halts again on the next send", "[journal]") {
  RepairableJournal journal;
  journal.failures_remaining_ = 5;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, RepairableExecutor> dry{
    HoldTen{},
    {},
    512,
    RepairableExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  dry.feed(bar_at(30));
  REQUIRE(dry.local_failure().has_value());
  dry.resume_after_local_failure();

  // Acknowledging a failure is not the same as fixing it.
  const auto again = dry.feed(bar_at(31));
  REQUIRE(again.unavailable_.size() == 1);
  REQUIRE(dry.local_failure().has_value());
}

TEST_CASE("a journal failure is a hold on the guard, not a flag beside it", "[journal]") {
  // The structural half of the fix. Before this, `local_failure()` was enforced by two
  // hand-written checks inside `settle()`; a send path that forgot to write one was ungated. It is
  // now a hold on the guard, so the one function every order already passes through refuses.
  RepairableJournal journal;
  journal.failures_remaining_ = 1;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, RepairableExecutor> dry{
    HoldTen{},
    {},
    512,
    RepairableExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  const auto failed = dry.feed(bar_at(30));
  REQUIRE(failed.unavailable_.size() == 1);
  // The report still says what it always said: not a guard rejection, but a withheld order naming
  // the local failure and carrying the reason an operator has to act on.
  REQUIRE(failed.refused_.empty());
  REQUIRE(failed.unavailable_.front().reason_ == runner::WithheldReason::LOCAL_FAILURE);
  REQUIRE_FALSE(failed.unavailable_.front().detail_.empty());

  // And the guard itself knows, with no `PollingLoop` anywhere near this runner.
  REQUIRE(dry.guard().halted());
  REQUIRE(dry.guard().halted(strategy::HaltReason::LOCAL_FAILURE));
  REQUIRE(dry.guard().blocking_reason());
  REQUIRE(*dry.guard().blocking_reason() == strategy::HaltReason::LOCAL_FAILURE);
  REQUIRE(dry.guard().hold_detail(strategy::HaltReason::LOCAL_FAILURE) == *dry.local_failure());

  // `RepairableJournal` stops failing once its budget is spent, which is the repair here.
  dry.resume_after_local_failure();
  REQUIRE_FALSE(dry.guard().halted());
}

TEST_CASE("resuming a journal failure does not resume an operator's halt", "[journal]") {
  // The interleaving, at the level it actually bites: an operator stops trading by hand while the
  // journal is broken, the storage is fixed, and the documented resume runs. Their hold stands.
  RepairableJournal journal;
  journal.failures_remaining_ = 1;
  runner::Runner<HoldTen, strategy::NoCaps, runner::DryTag, RepairableExecutor> dry{
    HoldTen{},
    {},
    512,
    RepairableExecutor{runner::RecordingExecutor<runner::DryTag>{}, journal, runner::ManualClock{}}
  };
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(samsung(), Decimal64::parse("70000"));

  dry.feed(bar_at(30));
  REQUIRE(dry.local_failure().has_value());

  dry.guard().halt();  // the operator, having seen the alert, stops trading themselves
  dry.resume_after_local_failure();

  REQUIRE_FALSE(dry.local_failure().has_value());
  REQUIRE(dry.guard().halted());
  REQUIRE(dry.guard().halted(strategy::HaltReason::OPERATOR));

  // So the next bar is refused by the guard, and refused as the *operator's* halt rather than as a
  // journal failure that no longer exists.
  const auto next = dry.feed(bar_at(31));
  REQUIRE(next.would_place_.empty());
  REQUIRE(next.refused_.size() == 1);
  REQUIRE(next.refused_.front().reason_ == strategy::GuardReason::TRADING_HALTED);
  REQUIRE(next.refused_.front().halt_reason_);
  REQUIRE(*next.refused_.front().halt_reason_ == strategy::HaltReason::OPERATOR);

  dry.guard().resume();
  const auto after = dry.feed(bar_at(32));
  REQUIRE(after.would_place_.size() == 1);
}
