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
  // What a real adapter produces. Both brokers set all three from their own
  // tables for any code they recognize, and leave all three `UNKNOWN` only for
  // one they do not -- so a fixture that left these at the struct's defaults was
  // modelling a row no broker sends, and modelling it as the *unreadable* case
  // the matcher now refuses to attribute.
  snapshot.time_in_force_ = TimeInForce::DAY;
  snapshot.session_ = OrderSession::REGULAR;
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

} // namespace

// ---------------------------------------------------------------------------
// A placement, matched on attributes
// ---------------------------------------------------------------------------

TEST_CASE("an instruction whose own line cannot decide anything needs an operator") {
  // The other half of the writer/reader split. These lines are readable, and
  // each lacks precisely the field the matcher would decide on -- so resolving
  // one would conclude from silence, and a closed window would report a
  // still-resting order as gone.
  SECTION("a cancellation with no target id") {
    auto instruction = cancellation("KRX-1");
    instruction.entry_.order_id_.clear();
    const auto match = runner::match_against(instruction, {row("KRX-1")}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::NEEDS_OPERATOR);
    REQUIRE(match.order_id_.empty());
    // With the id it names, it resolves -- so this is about the missing id.
    REQUIRE(
      runner::match_against(cancellation("KRX-1"), {row("KRX-1")}, after_the_window()).verdict_ ==
        MatchVerdict::MATCHED
    );
  }

  SECTION("a priced basis journalled without its price") {
    auto instruction = placement();
    instruction.entry_.type_ = OrderType::LIMIT;
    instruction.entry_.price_.reset();
    REQUIRE(
      runner::match_against(instruction, {row("KRX-1")}, after_the_window()).verdict_ ==
        MatchVerdict::NEEDS_OPERATOR
    );
  }

  SECTION("a market order journalled carrying one") {
    auto instruction = placement();
    instruction.entry_.type_ = OrderType::MARKET;
    instruction.entry_.price_ = Decimal64::parse("70000");
    REQUIRE(
      runner::match_against(instruction, {row("KRX-1")}, after_the_window()).verdict_ ==
        MatchVerdict::NEEDS_OPERATOR
    );
  }

  SECTION("a cancellation is judged on its target id, not on the shape it copied") {
    // A cancellation matches on `order_id_` alone, so its copy of the order's
    // shape is never compared and an inconsistency there cannot mislead
    // anything. The shape is copied from the working order, so a reservation
    // carrying an unpriced LIMIT -- or a MARKET the broker reported a price for
    // -- puts one there without the cancellation caring. Applying the placement
    // check to it withheld an instrument whose identity the journal had
    // recorded, for a cancellation that resolves without trouble.
    auto unpriced = cancellation("KRX-1");
    unpriced.entry_.type_ = OrderType::LIMIT;
    unpriced.entry_.price_.reset();
    const auto match = runner::match_against(unpriced, {row("KRX-1")}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
    REQUIRE(match.order_id_ == "KRX-1");

    auto priced_market = cancellation("KRX-1");
    priced_market.entry_.type_ = OrderType::MARKET;
    priced_market.entry_.price_ = Decimal64::parse("70000");
    REQUIRE(
      runner::match_against(priced_market, {row("KRX-1")}, after_the_window()).order_id_ ==
        "KRX-1"
    );

    // And absence still concludes, rather than withholding for ever.
    REQUIRE(
      runner::match_against(unpriced, {}, after_the_window()).verdict_ ==
        MatchVerdict::NOT_PLACED
    );

    // The empty-target guard still applies to a cancellation, because that one
    // is about the field the matcher actually uses.
    auto nameless = cancellation("KRX-1");
    nameless.entry_.order_id_.clear();
    REQUIRE(
      runner::match_against(nameless, {row("KRX-1")}, after_the_window()).verdict_ ==
        MatchVerdict::NEEDS_OPERATOR
    );
  }

  SECTION("an ordinary market order still resolves") {
    // The rule is about the *combination*, not about market orders.
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
  }
}

TEST_CASE("recovery tells two orders apart by the venue each was routed to") {
  // Two orders in one symbol, side, quantity, price and second differ only by
  // where they went. Without the route on the journal line and in the matcher,
  // an unresolved NXT placement and an unrelated KRX order are the same row to
  // recovery: it attributes one to the other and clears the hold on an order
  // that has not appeared.
  auto instruction = placement();
  instruction.entry_.venue_ = Venue::NXT;

  auto krx = row("KRX-1");
  krx.venue_ = Venue::KRX;
  auto nxt = row("NXT-1");
  nxt.venue_ = Venue::NXT;

  SECTION("a listing carrying only the other venue's order is not a match") {
    const auto match = runner::match_against(instruction, {krx}, after_the_window());
    REQUIRE(match.verdict_ != MatchVerdict::MATCHED);
    REQUIRE(match.order_id_.empty());
  }

  SECTION("a listing carrying both resolves to the one that was routed there") {
    const auto match = runner::match_against(instruction, {krx, nxt}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
    REQUIRE(match.order_id_ == "NXT-1");
  }

  SECTION("a journal line written before the route existed resolves no plausible row") {
    // **Silence is not confirmation.** A line with no `route` key was written
    // before the field existed, so what its order asked for is unknowable -- and
    // the order may well have been the NXT one. Attributing it to the only row
    // in the listing is how a KRX order belonging to somebody else cleared the
    // hold on an NXT order that was still outstanding.
    auto legacy = placement();
    legacy.entry_.venue_.reset();
    const auto match = runner::match_against(legacy, {krx}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
    REQUIRE(match.order_id_.empty());
    // And not NOT_PLACED either: the row may be the order, so re-sending would
    // be the other half of the same mistake.
    REQUIRE(match.verdict_ != MatchVerdict::NOT_PLACED);

    // **But an empty listing is still an absence, and needs no route to be one.**
    // "Resolves nothing" is about rows that *could* be the order; a complete
    // listing carrying nothing that resembles it says the order is not there
    // whatever venue it was headed for. Withholding on that would strand a
    // legacy line forever.
    REQUIRE(
      runner::match_against(legacy, {}, after_the_window()).verdict_ ==
        MatchVerdict::NOT_PLACED
    );

    // And a row that differs on an attribute the unknown route does not hide is
    // not a plausible row either, so it does not withhold. The quantity is
    // compared whatever the shape says, so it proves nothing about the guard --
    // these differ only in the *price* and the *basis*, which are readable here
    // and are exactly what a row with an unreadable shape would have lost.
    auto repriced = row("KRX-9", samsung(), "10", "71000");
    REQUIRE(
      runner::match_against(legacy, {repriced}, after_the_window()).verdict_ ==
        MatchVerdict::NOT_PLACED
    );

    auto other_basis = row("KRX-8");
    other_basis.type_ = OrderType::MARKET;
    other_basis.price_.reset();
    REQUIRE(
      runner::match_against(legacy, {other_basis}, after_the_window()).verdict_ ==
        MatchVerdict::NOT_PLACED
    );

    // A row differing in neither still withholds, so the two above are about the
    // attributes they name rather than about the legacy line.
    REQUIRE(
      runner::match_against(legacy, {row("KRX-7")}, after_the_window()).verdict_ ==
        MatchVerdict::AMBIGUOUS
    );
  }

  SECTION("an instruction that asked for no venue is not silence") {
    // The default, and every Toss order: the runner recorded that it asked for
    // nothing, so there is nothing to verify and the other attributes decide.
    // Toss listings carry no venue at all and never could.
    auto routeless = placement();
    REQUIRE(routeless.entry_.venue_ == std::optional<Venue>(Venue::UNSPECIFIED));
    auto row_without_venue = row("TOSS-1");
    const auto match =
      runner::match_against(routeless, {row_without_venue}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
    REQUIRE(match.order_id_ == "TOSS-1");
  }

  SECTION("a route the broker did not report cannot confirm one that was asked for") {
    // KIS reports a venue this library does not recognize, or none at all, and
    // `reported_venue` yields UNSPECIFIED for both. That is silence about a
    // route the instruction *did* ask for, so the row is plausible and
    // unconfirmable rather than a match.
    auto silent = row("KRX-9");
    REQUIRE(silent.venue_ == Venue::UNSPECIFIED);
    const auto match = runner::match_against(instruction, {silent}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
    REQUIRE(match.order_id_.empty());
  }

  SECTION("both KIS directions resolve when the broker does report the route") {
    REQUIRE(runner::match_against(instruction, {nxt}, after_the_window()).order_id_ == "NXT-1");

    auto to_krx = placement();
    to_krx.entry_.venue_ = Venue::KRX;
    REQUIRE(runner::match_against(to_krx, {krx}, after_the_window()).order_id_ == "KRX-1");
    // And each refuses the other's row outright rather than reporting ambiguity,
    // because a stated route that differs is evidence, not silence.
    REQUIRE(
      runner::match_against(to_krx, {nxt}, after_the_window()).verdict_ ==
        MatchVerdict::NOT_PLACED
    );
  }
}

TEST_CASE("an order code this release cannot read is not read as an absent order") {
  // A broker may add an `ORD_DVSN` after this release. It parses to `UNKNOWN`
  // in all three shape fields and -- because an unknown basis is not one
  // `carries_caller_price` admits -- with no price either. Three of the six
  // attributes the matcher compares are missing, so it cannot say this row is
  // the order. Saying it is *absent* is the dangerous half: the window closes,
  // the verdict is NOT_PLACED, the hold clears, and the runner sends the order
  // again beside the one already resting.
  auto instruction = placement();

  auto unreadable = row("KRX-1");
  unreadable.type_ = OrderType::UNKNOWN;
  unreadable.time_in_force_ = TimeInForce::UNKNOWN;
  unreadable.session_ = OrderSession::UNKNOWN;
  unreadable.price_.reset();
  unreadable.shape_code_ = Code<8>("99");

  const auto match = runner::match_against(instruction, {unreadable}, after_the_window());
  // After the window, and still not NOT_PLACED.
  REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
  REQUIRE(match.candidates_ == 1);
  REQUIRE(match.verdict_ != MatchVerdict::NOT_PLACED);

  // With no such row at all, the same instruction and window does conclude the
  // order is absent -- so the assertion above is about the row rather than
  // about the window.
  REQUIRE(
    runner::match_against(instruction, {}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );

  // And a readable row that genuinely differs still concludes absence, so the
  // tolerance is for unreadable rows and not for every row.
  auto different = row("KRX-2");
  different.side_ = OrderSide::SELL;
  REQUIRE(
    runner::match_against(instruction, {different}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );
}

TEST_CASE("an unreadable row that is plainly a different order is still an absence") {
  auto instruction = placement();
  auto unreadable = row("KRX-1", samsung(), "999");
  unreadable.type_ = OrderType::UNKNOWN;
  unreadable.time_in_force_ = TimeInForce::UNKNOWN;
  unreadable.session_ = OrderSession::UNKNOWN;
  unreadable.price_.reset();
  // Quantity still discriminates, and it survives an unreadable shape, so the
  // weaker predicate is not "admit anything".
  REQUIRE(
    runner::match_against(instruction, {unreadable}, after_the_window()).verdict_ ==
      MatchVerdict::NOT_PLACED
  );
}

TEST_CASE("a row for another instrument is not a match", "[reconcile]") {
  const auto match =
    runner::match_against(placement(), {row("KRX-1", hynix())}, after_the_window());
  REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
  REQUIRE(match.candidates_ == 0);
}

// ---------------------------------------------------------------------------
// Ambiguity
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Sweeping the ledger
// ---------------------------------------------------------------------------

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

TEST_CASE("a placement found absent says what was read, and what its caller does with that", "[reconcile]") {
  IndeterminacyLedger ledger;
  runner::MemoryJournal journal;
  auto cancel = cancellation("KRX-9");
  cancel.entry_.id_ = "run-2";
  const auto report = runner::reconcile_against(
    {placement(), cancel}, {}, after_the_window(), ledger, journal, MatchWindow{}, " (kept owed)"
  );
  REQUIRE(report.matches_.size() == 2);
  REQUIRE(report.matches_[0].verdict_ == MatchVerdict::NOT_PLACED);
  // No claim that the order does not exist: a list can lag an order it accepted.
  CHECK(
    report.matches_[0].detail_ ==
    "no matching order was found among the rows read, within the matching window (kept owed)"
  );
  CHECK(report.matches_[1].detail_.find("kept owed") == std::string::npos);
  REQUIRE(journal.records().size() == 2);
  CHECK(journal.records()[0].detail_.find("(kept owed)") != std::string::npos);
  // Said of nothing unless the caller says it, and of no other verdict.
  CHECK(runner::match_against(placement(), {}, after_the_window()).detail_.find("owed") == std::string::npos);
  const auto found = runner::reconcile_against(
    {placement()}, {row("KRX-1")}, after_the_window(), ledger, journal, MatchWindow{}, " (kept owed)"
  );
  REQUIRE(found.matches_.front().verdict_ == MatchVerdict::MATCHED);
  CHECK(found.matches_.front().detail_.find("kept owed") == std::string::npos);
}

TEST_CASE("a row the journal attributes to another placement is not a candidate", "[reconcile]") {
  // An order sent just before one whose outcome is unknown, of the same shape: its row is inside the
  // second one's window, and taken for it the second would carry the first one's number for good.
  auto first_pending = placement().entry_;
  first_pending.id_ = "run-0";
  auto first_accepted = first_pending;
  first_accepted.outcome_ = JournalOutcome::ACCEPTED;
  first_accepted.order_id_ = "KRX-1";
  first_accepted.recorded_at_ += std::chrono::seconds(1);
  runner::JournalReplay replay;
  replay.entries_ = {first_pending, first_accepted, placement().entry_};

  SECTION("its dated row is dropped; an undated one, another instrument's and another day's are kept") {
    auto next_day = row("KRX-1");
    next_day.ordered_at_ = sent_at() + std::chrono::hours(24);
    std::vector<OrderSnapshot> rows{
      row("KRX-1"), row("KRX-2"), row("KRX-1", samsung(), "10", "70000", std::nullopt), row("KRX-1", hynix()), next_day,
    };
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].order_id_ == "KRX-2");
    CHECK_FALSE(rows[1].ordered_at_.has_value());
    CHECK(rows[2].instrument_ == hynix());
    CHECK(rows[3].ordered_at_ == next_day.ordered_at_);
  }

  SECTION("a cancellation's line names the order it withdraws, and owns no row") {
    auto cancel = cancellation("KRX-2").entry_;
    cancel.id_ = "run-9";
    replay.entries_.push_back(cancel);
    std::vector<OrderSnapshot> rows{row("KRX-2")};
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    CHECK(rows.size() == 1);
  }

  SECTION("a line written later gives a name and does not widen the send's window") {
    // Round 1 of the review: yesterday's order, named by a sweep and confirmed today, and today's order
    // carrying the same number, sent seconds after that recovery.
    const auto today = sent_at() + std::chrono::hours(24);
    auto yesterday = placement().entry_;
    yesterday.id_ = "run-0";
    auto named_today = yesterday;
    named_today.outcome_ = JournalOutcome::RESOLVED;
    named_today.order_id_ = "KRX-1";
    named_today.recorded_at_ = today;
    auto confirmed_today = named_today;
    confirmed_today.outcome_ = JournalOutcome::CONFIRMED;
    replay.entries_ = {yesterday, named_today, confirmed_today};
    auto reused = row("KRX-1");
    reused.ordered_at_ = today + std::chrono::seconds(10);
    std::vector<OrderSnapshot> rows{reused, row("KRX-1")};
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    // Today's order with the reused number stays; yesterday's own row goes.
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().ordered_at_ == reused.ordered_at_);
  }

  SECTION("a name an earlier sweep gave wrongly keeps the row from its real placement, which stays unfound") {
    // The conservative direction, and why this filter is not for a sweep whose negative verdict is acted
    // on: the row is not lent to a second placement while the journal says it is the first one's.
    auto wrongly = first_accepted;
    wrongly.outcome_ = JournalOutcome::RESOLVED;
    replay.entries_ = {first_pending, wrongly, placement().entry_};
    std::vector<OrderSnapshot> rows{row("KRX-1")};
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    CHECK(rows.empty());
  }

  SECTION("the instruction being matched keeps its own row") {
    auto named = placement();
    named.entry_.id_ = "run-0";
    std::vector<OrderSnapshot> rows{row("KRX-1")};
    runner::drop_rows_of_other_placements(rows, replay, {named});
    CHECK(rows.size() == 1);
  }

  SECTION("so the later order is found absent, or matched to its own row, never to the earlier one") {
    std::vector<OrderSnapshot> rows{row("KRX-1")};
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    CHECK(runner::match_against(placement(), rows, after_the_window()).verdict_ == MatchVerdict::NOT_PLACED);
    rows = {row("KRX-1"), row("KRX-2")};
    runner::drop_rows_of_other_placements(rows, replay, {placement()});
    const auto match = runner::match_against(placement(), rows, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
    CHECK(match.order_id_ == "KRX-2");
  }
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

  /** A double reaches no broker, so it is exempt from having to have read the account. */
  static constexpr bool reaches_a_broker = false;
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

TEST_CASE("recovery tells two stops apart by their triggers", "[reconcile][trigger]") {
  auto instruction = placement();
  instruction.entry_.type_ = OrderType::STOP_LIMIT;
  instruction.entry_.trigger_price_ = Decimal64::parse("69500");

  const auto stop_row = [](const char* id, std::optional<const char*> trigger) {
    auto snapshot = row(id);
    snapshot.type_ = OrderType::STOP_LIMIT;
    if (trigger) {
      snapshot.trigger_price_ = Decimal64::parse(*trigger);
    }
    return snapshot;
  };

  SECTION("the same trigger matches") {
    const auto match =
      runner::match_against(instruction, {stop_row("KRX-1", "69500")}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::MATCHED);
    REQUIRE(match.order_id_ == "KRX-1");
  }

  SECTION("a different trigger is a different order") {
    const auto match =
      runner::match_against(instruction, {stop_row("KRX-1", "69800")}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
    REQUIRE(match.candidates_ == 0);
  }

  SECTION("a stop whose trigger the broker did not report is a candidate, not a match or an absence") {
    // Excluding it would report NOT_PLACED for a stop that is resting, and the runner would send
    // it again; matching it would let any lookalike clear the hold.
    const auto match =
      runner::match_against(instruction, {stop_row("KRX-1", std::nullopt)}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
    REQUIRE(match.candidates_ == 1);
    REQUIRE(match.order_id_.empty());
  }

  SECTION("a limit row that also reports a trigger is withheld, not read as an absence") {
    // What the readers record when a broker reports a positive trigger on a non-stop row.
    // It is the broker contradicting its own code, not proof of another order: reading it as one
    // reported this runner's own limit NOT_PLACED and cleared the hold for a second send.
    auto plain = placement();
    auto limit_with_trigger = row("KRX-1");
    limit_with_trigger.trigger_price_ = Decimal64::parse("69500");
    const auto match = runner::match_against(plain, {limit_with_trigger}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::AMBIGUOUS);
    REQUIRE(match.candidates_ == 1);
  }

  SECTION("a stop row is still not a plain order's match") {
    auto plain = placement();
    const auto match =
      runner::match_against(plain, {stop_row("KRX-1", "69500")}, after_the_window());
    REQUIRE(match.verdict_ == MatchVerdict::NOT_PLACED);
  }
}

TEST_CASE("a stop journalled without its trigger, or a limit with one, needs an operator", "[reconcile][trigger]") {
  JournalEntry missing = placement().entry_;
  missing.type_ = OrderType::STOP_LIMIT;
  CHECK_FALSE(runner::unresolvable_reason(missing).empty());

  JournalEntry stray = placement().entry_;
  stray.trigger_price_ = Decimal64::parse("69500");
  CHECK_FALSE(runner::unresolvable_reason(stray).empty());

  JournalEntry stop = missing;
  stop.trigger_price_ = Decimal64::parse("69500");
  CHECK(runner::unresolvable_reason(stop).empty());
}
