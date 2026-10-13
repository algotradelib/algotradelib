#include <algotradelib/runner>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

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

  /** A double reaches no broker, so it is exempt from having to have read the account. */
  static constexpr bool reaches_a_broker = false;
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

  /** A double reaches no broker, so it is exempt from having to have read the account. */
  static constexpr bool reaches_a_broker = false;
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

TEST_CASE("a placement is found by its id whatever was written about it since", "[journal]") {
  // What a later sweep needs of a placement an earlier one closed without finding its order: the
  // instruction, and the span of its send.
  auto failed = line("a-1", JournalOutcome::INDETERMINATE);
  failed.recorded_at_ += std::chrono::seconds(7);
  auto cancel = line("a-3", JournalOutcome::PENDING);
  cancel.action_ = runner::JournalAction::CANCEL;
  auto damaged = line("a-4", JournalOutcome::PENDING);
  damaged.requires_operator_ = true;
  runner::JournalReplay replay;
  replay.entries_ = {
    line("a-1", JournalOutcome::PENDING),
    failed,
    line("a-1", JournalOutcome::RESOLVED),
    line("a-2", JournalOutcome::INDETERMINATE, hynix()),
    line("a-2", JournalOutcome::RESOLVED, hynix()),
    cancel,
    damaged,
    line("a-5", JournalOutcome::PENDING),
    // An answer alone says nothing about when its instruction was sent.
    line("a-6", JournalOutcome::ACCEPTED),
  };
  const auto found = runner::placements_of(replay, {"a-1", "a-2", "a-3", "a-4", "a-6", "a-9"});
  REQUIRE(found.size() == 2);
  CHECK(found[0].entry_.id_ == "a-1");
  CHECK(found[0].entry_.outcome_ == JournalOutcome::PENDING);
  CHECK(found[0].opened_at_ == utc(2026, 3, 30, 0, 30));
  CHECK(found[0].closed_at_ == utc(2026, 3, 30, 0, 30) + std::chrono::seconds(7));
  CHECK(found[0].answered_);
  // Its first line was lost: the answer brackets the send alone.
  CHECK(found[1].entry_.instrument_ == hynix());
  CHECK(found[1].opened_at_ == found[1].closed_at_);
  CHECK(runner::placements_of(replay, {}).empty());
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

// ---------------------------------------------------------------------------
// The decorator: the entry exists before the send is attempted
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The file: append, flush, read back
// ---------------------------------------------------------------------------

namespace {

/**
 * Writes `text` to `path` exactly, trailing newline included or omitted.
 *
 * The truncation tests write real files rather than faking a journal, because what is being tested
 * is what a partially completed write actually looks like on disk. A mock would assert the reader
 * against an idea of a torn line instead of against one.
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

/**
 * Returns `line` as a release before checksums would have written it: the same bytes without the
 * final `crc` member.
 *
 * The tests below that model a line written *before some field existed* also model one written before
 * the checksum existed, because that is the only kind of line that can lack the field. Editing a
 * checksummed line instead is damage, which the checksum now catches -- see the checksum tests.
 */
std::string without_checksum(std::string line) {
  const auto at = line.rfind(",\"crc\":\"");
  REQUIRE(at != std::string::npos);
  line.erase(at);
  line += '}';
  return line;
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

TEST_CASE("a damaged value makes a line unattributable, not a different order", "[journal]") {
  // **Every one of these parsed successfully before, into a different order.**
  // The lenient readers exist so a line written before a field existed still
  // reads, and their defaults are a fact about such a line. Applying them to a
  // token the line *does* carry turns damage into evidence: recovery matches a
  // lookalike, records its id and clears the hold on an order still resting.
  //
  // An unreadable value now yields an entry with no instrument, which is what
  // `unattributable_journal_entry` is for -- recovery turns that into a global
  // halt, because a guessed instrument is not the key the runner trades under
  // and withholding it protects nothing.
  const auto damage = [](std::string_view field, std::string_view from, std::string_view to) {
    auto line = without_checksum(runner::journal_entry_to_line(sample_entry()));
    const std::string needle = std::string("\"") + std::string(field) + "\":\"" +
      std::string(from) + "\"";
    const auto at = line.find(needle);
    REQUIRE(at != std::string::npos);
    line.replace(at, needle.size(),
      std::string("\"") + std::string(field) + "\":\"" + std::string(to) + "\"");
    return line;
  };

  struct Case {
    const char* field;
    const char* from;
    const char* to;
  };
  const Case cases[] = {
    {"country", "KR", "K?"},
    {"venue", "COMBINED", "COMBIN?D"},
    {"side", "SELL", "SEL?"},
    {"type", "BEST_LIMIT", "BEST_LIM?T"},
    {"time_in_force", "IOC", "IO?"},
    {"session", "AFTER_MARKET", "AFTER_MARK?T"},
  };

  for (const auto& one : cases) {
    INFO("damaged field: " << one.field);
    const auto parsed = runner::journal_entry_from_line(damage(one.field, one.from, one.to));
    REQUIRE(parsed.has_value());
    // No instrument: that is what makes the halt global rather than a
    // withholding of whatever the damage happened to spell.
    REQUIRE(parsed->instrument_.symbol_.empty());
    REQUIRE(parsed->requires_operator_);
    // And specifically not the order the damage would otherwise have named.
    REQUIRE_FALSE(parsed->instrument_ == sample_entry().instrument_);
  }

  // The undamaged line still reads as itself, so the assertions above are about
  // the damage rather than about the reader refusing everything.
  const auto intact = runner::journal_entry_from_line(
    runner::journal_entry_to_line(sample_entry())
  );
  REQUIRE(intact.has_value());
  REQUIRE(intact->instrument_ == sample_entry().instrument_);
  REQUIRE_FALSE(intact->requires_operator_);
}

TEST_CASE("a torn line's wrong-typed prefix field is damage, not an absent one", "[journal]") {
  // `salvage_string` recognizes `"key":"value"` and nothing else, so `null` and
  // `7` read as *absent* -- and a truncated line's absent fields legitimately
  // take defaults, because truncation removes the tail. A fragment reading
  // `"country":null,…,"symbol":"AAPL"` was therefore salvaged as a **Korean**
  // instrument, marked operator-only on that guessed key, while the real US
  // instrument stayed free to trade. `requires_operator_` does not protect a
  // key nobody is trading under.
  const auto torn = R"({"id":"run-1","action":"place","outcome":"pending","country":null,)"
                    R"("venue":"COMBINED","symbol":"AAPL","side":"BUY","quan)";
  const auto salvaged = runner::salvage_journal_line(torn);
  REQUIRE(salvaged.has_value());
  REQUIRE(salvaged->requires_operator_);
  REQUIRE(salvaged->instrument_.symbol_.empty());
  // Specifically not the instrument the guess would have named.
  REQUIRE_FALSE((salvaged->instrument_.country_ == Country::KR &&
    salvaged->instrument_.symbol_ == "AAPL"));

  // A fragment that simply *ends* before those keys still takes its defaults,
  // because that is what a torn line is.
  const auto plain = R"({"id":"run-2","action":"place","outcome":"pending",)"
                     R"("symbol":"005930","side":"BUY","quan)";
  const auto ordinary = runner::salvage_journal_line(plain);
  REQUIRE(ordinary.has_value());
  REQUIRE(ordinary->requires_operator_);
  REQUIRE(ordinary->instrument_.symbol_ == "005930");
  REQUIRE(ordinary->instrument_.country_ == Country::KR);

  // **And a key whose value the truncation cut off mid-write is the ordinary
  // shape of a torn line, not damage.** The first version of this check asked
  // only whether the key was *mentioned*, which made every fragment's own last
  // field look damaged and turned every torn line into a global halt. The comma
  // is the tell: a value followed by one finished being written, so failing to
  // read it is damage; a value with nothing after it is where the writer
  // stopped.
  const auto cut_mid_value = R"({"id":"run-3","action":"place","outcome":"pending",)"
                             R"("country":"KR","venue":"COMBINED","symbol":"005930","side":"BU)";
  const auto cut = runner::salvage_journal_line(cut_mid_value);
  REQUIRE(cut.has_value());
  REQUIRE(cut->requires_operator_);
  REQUIRE(cut->instrument_.symbol_ == "005930");
  REQUIRE(cut->side_ == OrderSide::BUY);

  // The same key, written to completion and unreadable, is still damage.
  const auto completed = R"({"id":"run-4","action":"place","outcome":"pending",)"
                         R"("country":"KR","venue":"COMBINED","side":null,)"
                         R"("symbol":"005930","quan)";
  const auto damaged_side = runner::salvage_journal_line(completed);
  REQUIRE(damaged_side.has_value());
  REQUIRE(damaged_side->instrument_.symbol_.empty());
}

TEST_CASE("a line the writer emits is readable, and unresolvable", "[journal]") {
  // **These two shapes are written by this library itself.**
  // `JournalingExecutor` appends the pending line *before* the binding runs, so
  // an order the binding then refuses locally -- a cancellation of a
  // reservation whose broker id is unknown, a limit with no price -- is already
  // on disk by the time anything could object.
  //
  // Making the reader reject them turned an ordinary local
  // refusal into a global halt on the next restart: both lines became
  // `~damaged` operator-only entries with fresh ids, so the terminal line could
  // no longer close its pending partner. The reader reads them now; refusing to
  // *conclude* from one is `match_against`'s job.
  auto cancel = sample_entry();
  cancel.action_ = runner::JournalAction::CANCEL;
  cancel.order_id_.clear();
  const auto read_cancel =
    runner::journal_entry_from_line(runner::journal_entry_to_line(cancel));
  REQUIRE(read_cancel.has_value());
  REQUIRE(read_cancel->instrument_ == cancel.instrument_);
  REQUIRE_FALSE(read_cancel->requires_operator_);

  auto unpriced = sample_entry();
  unpriced.type_ = OrderType::LIMIT;
  unpriced.price_.reset();
  const auto read_unpriced =
    runner::journal_entry_from_line(runner::journal_entry_to_line(unpriced));
  REQUIRE(read_unpriced.has_value());
  REQUIRE(read_unpriced->instrument_ == unpriced.instrument_);

  // And a damaged *terminal* line keeps its own outcome, so it still closes the
  // damaged pending line it belongs to. Forcing PENDING left both open and
  // halted a runner over an instruction that had already resolved.
  auto terminal = sample_entry();
  terminal.outcome_ = runner::JournalOutcome::REJECTED;
  auto damaged = without_checksum(runner::journal_entry_to_line(terminal));
  const auto at = damaged.find("\"side\":\"SELL\"");
  REQUIRE(at != std::string::npos);
  damaged.replace(at, std::string("\"side\":\"SELL\"").size(), "\"side\":\"SEL?\"");
  const auto read_damaged = runner::journal_entry_from_line(damaged);
  REQUIRE(read_damaged.has_value());
  REQUIRE(read_damaged->instrument_.symbol_.empty());
  REQUIRE(read_damaged->outcome_ == runner::JournalOutcome::REJECTED);
}

// ---------------------------------------------------------------------------
// Truncation: the case the format exists for
// ---------------------------------------------------------------------------

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

/** Withdraws every resting order it can see, so a cancellation reaches the executor. */
struct WithdrawAll {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent&) {
    for (const auto& working : ctx.portfolio().working_) {
      ctx.cancel(working.ref_, "withdraw-all");
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

TEST_CASE("a terminal journal failure stops the runner too", "[journal]") {
  // The broker already answered, so the outcome passed back stays the broker's -- saying otherwise
  // would be a lie about an order that exists. But the journal is broken, and the earlier code
  // recorded that only on the executor, where nothing read it, so the runner did not stop.
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

TEST_CASE(
  "a cancellation of a market order the broker priced survives writer to reader",
  "[journal][file][reconcile]"
) {
  // The same case as the path it actually travels rather than as a hand-built
  // `OpenInstruction`: the direct test shows the matcher tolerates a price-inconsistent cancellation, but
  // only this one shows such a line is *written* in the first place, accepted by the reader, and
  // still resolvable. Without it the guard could be protecting a case that never occurs.
  //
  // `JournalingExecutor::cancel` copies `type_` and `price_` straight off the working order, and a
  // working order is read back from the broker -- so a `MARKET` order the broker reports a price
  // for puts an inconsistency in the line with nothing wrong anywhere. The cancellation is matched
  // on `order_id_` alone, so that inconsistency is not evidence about anything.
  const TempJournal path("cancel-priced-market");

  auto working = resting("KRX-1");
  working.order_.type_ = OrderType::MARKET;
  working.order_.price_ = Decimal64::parse("70000");

  {
    runner::FileJournal journal(path.str(), "test:account");
    REQUIRE(journal.open());
    ObservingExecutor<runner::DryTag> inner;
    inner.cancel_outcome_ = SendOutcome::INDETERMINATE;
    runner::JournalingExecutor<ObservingExecutor<runner::DryTag>, runner::FileJournal,
      runner::ManualClock>
      executor{inner, journal, runner::ManualClock{utc(2026, 3, 30, 1, 0)}};
    // No answer came back, which is what leaves the instruction open across the restart.
    REQUIRE(executor.cancel(working) == SendOutcome::INDETERMINATE);
  }

  // A new process, reading the file its predecessor wrote.
  const auto replay = runner::read_journal_file(path.str());
  REQUIRE(replay.intact());
  const auto open = runner::unresolved_instructions(replay);
  REQUIRE(open.size() == 1);
  REQUIRE(open.front().entry_.action_ == runner::JournalAction::CANCEL);
  // The reader kept both, so the inconsistency really is present at the matcher.
  REQUIRE(open.front().entry_.type_ == OrderType::MARKET);
  REQUIRE(open.front().entry_.price_.has_value());
  REQUIRE(open.front().entry_.order_id_ == "KRX-1");

  const auto now = utc(2026, 3, 30, 1, 10);

  OrderSnapshot listed;
  listed.order_id_ = "KRX-1";
  listed.instrument_ = samsung();
  listed.side_ = OrderSide::BUY;
  listed.type_ = OrderType::MARKET;
  listed.time_in_force_ = TimeInForce::DAY;
  listed.session_ = OrderSession::REGULAR;
  listed.status_ = OrderStatus::PENDING;
  listed.quantity_ = Decimal64::parse("10");
  listed.price_ = Decimal64::parse("70000");

  // Present: the named order is still there, so the cancellation did not take.
  REQUIRE(runner::match_against(open.front(), {listed}, now).verdict_ ==
    runner::MatchVerdict::MATCHED);
  // Absent, past the window: it is gone, and the instrument may trade again. Before the fix both
  // of these were NEEDS_OPERATOR and the instrument was withheld for ever.
  REQUIRE(runner::match_against(open.front(), {}, now).verdict_ ==
    runner::MatchVerdict::NOT_PLACED);
}

// ---------------------------------------------------------------------------
// One journal, one account
// ---------------------------------------------------------------------------

TEST_CASE("a journal must name an account", "[journal][file]") {
  const TempJournal path("scope-empty");
  runner::FileJournal journal(path.str(), "");
  REQUIRE_FALSE(journal.open());
  REQUIRE_FALSE(journal.rejection().empty());
}

TEST_CASE("one journal has one writer: a second opener is refused while the first is open", "[journal][file]") {
  const TempJournal path("one-writer");
  {
    runner::FileJournal first(path.str(), "test:account");
    REQUIRE(first.open());
    runner::FileJournal second(path.str(), "test:account");
    REQUIRE_FALSE(second.open());
    REQUIRE(second.rejection().find("one journal, one writer") != std::string::npos);
    REQUIRE(first.append(sample_entry()));
  }
  runner::FileJournal third(path.str(), "test:account");
  REQUIRE(third.open());
  REQUIRE(third.replay().entries_.size() == 1);
}

TEST_CASE("a moved journal keeps its writer's lock", "[journal][file]") {
  const TempJournal path("one-writer-moved");
  std::optional<runner::FileJournal> first;
  first.emplace(path.str(), "test:account");
  runner::FileJournal moved(std::move(*first));
  // The moved-from journal is gone before anyone else tries: a lock it still held would hide a move
  // that dropped it.
  first.reset();
  REQUIRE(moved.open());
  runner::FileJournal second(path.str(), "test:account");
  REQUIRE_FALSE(second.open());
  REQUIRE(moved.append(sample_entry()));
}

#if !defined(_WIN32)
TEST_CASE("a journal another process holds is refused", "[journal][file]") {
  const TempJournal path("one-writer-process");
  int ready[2];
  int release[2];
  REQUIRE(::pipe(ready) == 0);
  REQUIRE(::pipe(release) == 0);
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The child holds the journal until the parent has tried, then leaves without unwinding into
    // Catch2. Each side closes the ends it does not use, so either one dying is an end of file.
    ::close(ready[0]);
    ::close(release[1]);
    try {
      runner::FileJournal holder(path.str(), "test:account");
      const char held = holder.open() ? 'y' : 'n';
      (void)!::write(ready[1], &held, 1);
      char done = 0;
      (void)!::read(release[0], &done, 1);
    } catch (...) {
      ::_exit(1);
    }
    ::_exit(0);
  }
  ::close(ready[1]);
  ::close(release[0]);
  char held = 0;
  const auto got = ::read(ready[0], &held, 1);
  bool refused = false;
  std::string why;
  {
    runner::FileJournal parent(path.str(), "test:account");
    refused = !parent.open();
    why = parent.rejection();
  }
  const char done = 'x';
  (void)!::write(release[1], &done, 1);
  int status = 0;
  ::waitpid(child, &status, 0);
  ::close(ready[0]);
  ::close(release[1]);
  REQUIRE(got == 1);
  REQUIRE(held == 'y');
  REQUIRE(refused);
  REQUIRE(why.find("one journal, one writer") != std::string::npos);
  // The child is gone, and its lock with it.
  runner::FileJournal after(path.str(), "test:account");
  REQUIRE(after.open());
}
#endif

TEST_CASE("a file with no header is not ours to write to", "[journal][file]") {
  // Ownership cannot be established, and that is not a question to guess at while holding a file
  // that may name another account's live orders.
  const TempJournal path("scope-headerless");
  write_file(path.str(), runner::journal_entry_to_line(sample_entry()) + "\n");

  runner::FileJournal journal(path.str(), "test:account");
  REQUIRE_FALSE(journal.open());
  REQUIRE(journal.rejection().find("no header") != std::string::npos);
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

// ---------------------------------------------------------------------------
// Stop-limit orders
// ---------------------------------------------------------------------------

TEST_CASE("a damaged trigger makes a line unattributable, and salvage keeps a readable one", "[journal][trigger]") {
  auto stop = sample_entry();
  stop.type_ = OrderType::STOP_LIMIT;
  stop.trigger_price_ = Decimal64::parse("69500");
  auto line = runner::journal_entry_to_line(stop);
  const std::string needle = "\"trigger\":\"69500\"";
  const auto at = line.find(needle);
  REQUIRE(at != std::string::npos);

  // On a pre-checksum line, where the value reader is what catches it; a checksummed line with the
  // same change is unreadable outright, which the integrity tests pin.
  auto damaged = without_checksum(line);
  damaged.replace(at, needle.size(), "\"trigger\":\"69?00\"");
  std::optional<runner::JournalEntry> parsed;
  REQUIRE_NOTHROW(parsed = runner::journal_entry_from_line(damaged));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->instrument_.symbol_.empty());
  REQUIRE(parsed->requires_operator_);

  // Torn just after the trigger: salvage recovers it, and the entry is still an operator's.
  const auto torn = line.substr(0, at + needle.size());
  const auto salvaged = runner::salvage_journal_line(torn);
  REQUIRE(salvaged.has_value());
  REQUIRE(salvaged->requires_operator_);
  REQUIRE(salvaged->trigger_price_ == Decimal64::parse("69500"));
}

// ---------------------------------------------------------------------------
// Line integrity
// ---------------------------------------------------------------------------

namespace {

/** A line carrying every field a recovery match reads, so damage anywhere in it has something to change. */
JournalEntry checked_entry() {
  auto entry = sample_entry();
  entry.type_ = OrderType::STOP_LIMIT;
  entry.trigger_price_ = Decimal64::parse("69500");
  entry.order_id_ = "0001569139";
  entry.outcome_ = JournalOutcome::ACCEPTED;
  return entry;
}

/** Whether `read` is what damage must become: nothing, or an entry only an operator may resolve. */
bool reads_as_damage(const std::optional<JournalEntry>& read) {
  return !read.has_value() || (read->requires_operator_ && read->instrument_.symbol_.empty());
}

} // namespace

TEST_CASE("no single changed byte reads as an ordinary line", "[journal][integrity]") {
  // **The property the checksum exists for**, stated over the whole line rather than a few chosen
  // fields: every position, two replacements each. A value damaged into another valid value --
  // `005930` into `005931`, a real instrument -- was once read as ordinary.
  const auto line = runner::journal_entry_to_line(checked_entry());
  std::size_t ordinary = 0;
  for (std::size_t at = 0; at < line.size(); ++at) {
    for (const char replacement : {'1', 'x'}) {
      if (line[at] == replacement) {
        continue;
      }
      auto damaged = line;
      damaged[at] = replacement;
      if (!reads_as_damage(runner::journal_entry_from_line(damaged))) {
        ++ordinary;
        INFO("position " << at << " -> " << replacement << ": " << damaged);
        CHECK(false);
      }
    }
  }
  CHECK(ordinary == 0);

  // And the ones a reader of values alone could never see, by name.
  const auto changed = [&line](std::string_view from, std::string_view to) {
    auto damaged = line;
    const auto at = damaged.find(from);
    REQUIRE(at != std::string::npos);
    damaged.replace(at, from.size(), to);
    return runner::journal_entry_from_line(damaged);
  };
  CHECK(reads_as_damage(changed("\"005930\"", "\"005931\"")));
  CHECK(reads_as_damage(changed("\"0001569139\"", "\"0001569138\"")));
  CHECK(reads_as_damage(changed("\"crc\"", "\"crd\"")));
}

TEST_CASE("a changed terminal line halts recovery rather than being dropped as an orphan", "[journal][integrity]") {
  // Read as a damaged entry, a terminal line carries an id suffixed
  // `~damaged`, pairs with nothing and is dropped -- leaving its intact pending partner open as an
  // ordinary instruction a sweep may resolve. A changed line is unreadable instead, and that halts.
  auto pending = checked_entry();
  pending.outcome_ = JournalOutcome::PENDING;
  pending.order_id_.clear();
  auto terminal = checked_entry();
  auto changed = runner::journal_entry_to_line(terminal);
  const auto at = changed.find("\"accepted\"");
  REQUIRE(at != std::string::npos);
  changed.replace(at, 10, "\"rejected\"");

  runner::JournalBytes bytes;
  bytes.status_ = runner::JournalReadStatus::READ;
  bytes.text_ = runner::journal_entry_to_line(pending) + "\n" + changed + "\n";
  const auto replay = runner::parse_journal_bytes(bytes);
  CHECK(replay.unreadable_lines_ == 1);
  runner::IndeterminacyLedger ledger;
  const auto recovery = runner::recover_into(replay, ledger);
  CHECK(recovery.unreadable_lines_ == 1);
  CHECK_FALSE(recovery.clean());

  // A key no writer produced, on a line whose checksum is *correct* for it: unreadable too.
  auto body = without_checksum(runner::journal_entry_to_line(terminal));
  body.insert(body.size() - 1, ",\"note\":\"x\"");
  const auto crc = runner::journal_detail::checksum_text(runner::journal_detail::line_checksum(body));
  body.pop_back();
  const auto annotated = body + ",\"crc\":\"" + crc + "\"}";
  REQUIRE(runner::journal_detail::checksum_matches(annotated));
  CHECK_FALSE(runner::journal_entry_from_line(annotated).has_value());
}

TEST_CASE("an entry overwritten by a newline or by the header is counted, not skipped", "[journal][integrity]") {
  // A blank line and a second header were skipped, so an entry replaced by
  // either vanished from recovery with nothing counted -- and its terminal companion became an orphan.
  const auto header = runner::journal_header_line("test:account");
  auto pending = checked_entry();
  pending.outcome_ = JournalOutcome::PENDING;
  pending.order_id_.clear();
  const auto terminal = runner::journal_entry_to_line(checked_entry());

  const auto recovered = [](const std::string& text) {
    runner::JournalBytes bytes;
    bytes.status_ = runner::JournalReadStatus::READ;
    bytes.text_ = text;
    const auto replay = runner::parse_journal_bytes(bytes);
    runner::IndeterminacyLedger ledger;
    return std::make_pair(replay.unreadable_lines_, runner::recover_into(replay, ledger).clean());
  };

  const auto blanked = recovered(header + "\n\n" + terminal + "\n");
  CHECK(blanked.first == 1);
  CHECK_FALSE(blanked.second);

  const auto duplicated = recovered(header + "\n" + header + "\n" + terminal + "\n");
  CHECK(duplicated.first == 1);
  CHECK_FALSE(duplicated.second);

  // What this writer does produce is still clean: a header, then a pending and its answer.
  const auto healthy =
    recovered(header + "\n" + runner::journal_entry_to_line(pending) + "\n" + terminal + "\n");
  CHECK(healthy.first == 0);
  CHECK(healthy.second);
}
