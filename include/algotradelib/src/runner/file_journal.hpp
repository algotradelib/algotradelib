#pragma once

#include "./journal.hpp"

#include <algotradelib/core>

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace algotradelib {
namespace runner {

namespace json = boost::json;

namespace journal_detail {

/**
 * Pushes a stream's buffered bytes out of the process and asks the kernel to write them.
 *
 * Two steps, because they close different windows and only the second one closes the one that
 * matters. `fflush` moves bytes out of the C library's buffer; if the process is killed after that,
 * the data still reaches the disk, because the kernel owns it. `fsync` waits for the kernel to
 * write it; if the *machine* loses power after that, the data is still there.
 *
 * **What this does not promise**, stated because the name suggests otherwise: on macOS, `fsync`
 * returns once the data reaches the drive, not once the drive has committed it to stable media.
 * `fcntl(F_FULLFSYNC)` is the stronger call and costs an order of magnitude more. The threat this
 * journal exists for is a *killed process* — `SIGKILL`, a crash, a supervisor restart — and `fflush`
 * alone already survives that. `fsync` is here to extend the guarantee to a kernel panic or a power
 * loss on every platform where it means that, and the gap on macOS is a real one an operator should
 * know about rather than a bug to be surprised by.
 */
inline bool flush_to_disk(std::FILE* handle) noexcept {
  if (std::fflush(handle) != 0) {
    return false;
  }
#if defined(_WIN32)
  return ::_commit(::_fileno(handle)) == 0;
#else
  return ::fsync(::fileno(handle)) == 0;
#endif
}

/** Returns the value of a JSON string field, or an empty string when it is absent or not one. */
/**
 * Returns whether the line carries `key` at all.
 *
 * **Presence and readability are different questions**, and `string_field`
 * answers neither on its own: it returns `""` for a key that is absent, for one
 * whose value is not a string, and for one whose value is an empty string. The
 * legacy defaults in this file are justified for the *first* of those -- a line
 * written before a field existed -- and applying them to the other two is the
 * same defect the strict readers were added to stop, wearing JSON's clothes: an
 * IOC line changed to `"time_in_force":""` or `null` reads as a day order.
 */
/**
 * Returns whether `micros` could be an instant this library wrote.
 *
 * **A bound, not a check.** It catches the demonstrated case -- an `at` changed
 * to `0`, which puts the matching window at the epoch so every real row falls
 * outside it and the sweep writes `NOT_PLACED` for an order that exists -- and
 * it does not catch a stamp that is merely an hour wrong, because nothing in a
 * line can. Distinguishing a valid value from a valid-but-changed one needs the
 * line to carry a digest of itself; see `plan/todo/journal-line-integrity.md`.
 *
 * The lower bound is 2020-01-01, comfortably before this library existed, and
 * the upper is a century out, which is far enough that clock skew and a
 * deliberately future-dated test cannot trip it while `0`, a negative, and a
 * value near the representable limit all do. The limit matters on its own:
 * `system_clock::duration` is finer than microseconds on some
 * implementations, so an extreme value overflows the conversion rather than
 * merely lying.
 */
inline bool plausible_instant(std::int64_t micros) noexcept {
  constexpr std::int64_t kYear2020 = 1577836800LL * 1000000LL;
  constexpr std::int64_t kCenturyOut = 4102444800LL * 1000000LL;
  return micros >= kYear2020 && micros <= kCenturyOut;
}

inline bool has_field(const json::object& row, std::string_view key) {
  return row.if_contains(key) != nullptr;
}

/** Returns the non-empty string `key` holds, or nothing when it holds no such thing. */
inline std::optional<std::string> readable_string(
  const json::object& row, std::string_view key
) {
  const auto value = row.if_contains(key);
  if (value == nullptr || !value->is_string() || value->as_string().empty()) {
    return std::nullopt;
  }
  return std::string(value->as_string());
}

inline std::string string_field(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    return {};
  }
  return std::string(value->as_string());
}

/**
 * Returns a stable name for `country`, as written to the journal.
 *
 * The journal's own wire vocabulary, deliberately spelled out here rather than derived from the
 * enumerator order. An enum's numeric value is an implementation detail that a reordering may
 * change; a file written by yesterday's build has to be readable by today's.
 */
inline constexpr std::string_view country_name(Country country) noexcept {
  return country == Country::US ? "US" : "KR";
}

/** Returns a stable name for `venue`, as written to the journal. */
inline constexpr std::string_view venue_name(Venue venue) noexcept {
  switch (venue) {
    case Venue::UNSPECIFIED:
      return "UNSPECIFIED";
    case Venue::KRX:
      return "KRX";
    case Venue::NXT:
      return "NXT";
    case Venue::COMBINED:
      return "COMBINED";
  }
  return "UNSPECIFIED";
}

/**
 * Returns the venue `text` names, or nothing when it names none this reader knows.
 *
 * The distinction matters only for the recorded **route**, and there it is the
 * difference between "the runner asked for no particular venue" and "this line
 * says something I cannot read". A damaged token, or one written by a newer
 * release, must not become the first: `UNSPECIFIED` is a positive assertion that
 * recovery confirms any row against, so reading `"NX?"` as it would let a KRX
 * lookalike clear the hold on an NXT order. That is the same
 * silence-to-confirmation failure two earlier rounds closed elsewhere.
 */
inline std::optional<Venue> known_venue_from(std::string_view text) noexcept {
  if (text == "KRX") {
    return Venue::KRX;
  }
  if (text == "NXT") {
    return Venue::NXT;
  }
  if (text == "COMBINED") {
    return Venue::COMBINED;
  }
  if (text == "UNSPECIFIED") {
    return Venue::UNSPECIFIED;
  }
  return std::nullopt;
}


/** Returns a stable name for `side`, as written to the journal. */
inline constexpr std::string_view side_name(OrderSide side) noexcept {
  return side == OrderSide::SELL ? "SELL" : "BUY";
}


/**
 * Returns a stable name for `type`, as written to the journal.
 *
 * **A new enumerator must be added here, and the compiler will not say so.**
 * The `switch` has no `default:`, which is the shape that makes `-Wswitch`
 * report a missing case -- but the `return` beneath it means the function still
 * compiles, and an unnamed basis would be written to the journal as `UNKNOWN`
 * and read back as `UNKNOWN`. A journal is what crash recovery reads, so that
 * is silent loss of the fact recovery matches on. The round-trip test over
 * every enumerator is the guard; the build is not.
 */
inline constexpr std::string_view order_type_name(OrderType type) noexcept {
  switch (type) {
    case OrderType::LIMIT:
      return "LIMIT";
    case OrderType::MARKET:
      return "MARKET";
    case OrderType::CONDITIONAL_LIMIT:
      return "CONDITIONAL_LIMIT";
    case OrderType::BEST_LIMIT:
      return "BEST_LIMIT";
    case OrderType::PRIMARY_LIMIT:
      return "PRIMARY_LIMIT";
    case OrderType::MID_PRICE:
      return "MID_PRICE";
    case OrderType::STOP_LIMIT:
      return "STOP_LIMIT";
    case OrderType::CLOSING_PRICE:
      return "CLOSING_PRICE";
    case OrderType::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

/** Returns the order type `text` names, defaulting to `UNKNOWN`. */
inline OrderType order_type_from(std::string_view text) noexcept {
  for (const auto type : {
         OrderType::LIMIT,
         OrderType::MARKET,
         OrderType::CONDITIONAL_LIMIT,
         OrderType::BEST_LIMIT,
         OrderType::PRIMARY_LIMIT,
         OrderType::MID_PRICE,
         OrderType::STOP_LIMIT,
         OrderType::CLOSING_PRICE,
       }) {
    if (text == order_type_name(type)) {
      return type;
    }
  }
  return OrderType::UNKNOWN;
}

/** Returns a stable name for `condition`, as written to the journal. */
inline constexpr std::string_view time_in_force_name(TimeInForce condition) noexcept {
  switch (condition) {
    case TimeInForce::DAY:
      return "DAY";
    case TimeInForce::IMMEDIATE_OR_CANCEL:
      return "IOC";
    case TimeInForce::FILL_OR_KILL:
      return "FOK";
    case TimeInForce::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

/**
 * Returns the condition `text` names, defaulting to `DAY`.
 *
 * `DAY` rather than `UNKNOWN`, and only here: a journal written before this
 * field existed has no `time_in_force` key at all, and every order it could
 * have recorded was a day order, because that was the only condition either
 * adapter would send. Reading those lines as `UNKNOWN` would make recovery
 * refuse to match orders it can match.
 */
inline TimeInForce time_in_force_from(std::string_view text) noexcept {
  if (text == "IOC") {
    return TimeInForce::IMMEDIATE_OR_CANCEL;
  }
  if (text == "FOK") {
    return TimeInForce::FILL_OR_KILL;
  }
  if (text == "UNKNOWN") {
    return TimeInForce::UNKNOWN;
  }
  return TimeInForce::DAY;
}

/** Returns a stable name for `session`, as written to the journal. */
inline constexpr std::string_view order_session_name(OrderSession session) noexcept {
  switch (session) {
    case OrderSession::REGULAR:
      return "REGULAR";
    case OrderSession::OPENING_AUCTION:
      return "OPENING_AUCTION";
    case OrderSession::CLOSING_AUCTION:
      return "CLOSING_AUCTION";
    case OrderSession::PRE_MARKET:
      return "PRE_MARKET";
    case OrderSession::AFTER_HOURS:
      return "AFTER_HOURS";
    case OrderSession::AFTER_HOURS_AUCTION:
      return "AFTER_HOURS_AUCTION";
    case OrderSession::AFTER_MARKET:
      return "AFTER_MARKET";
    case OrderSession::GTP:
      return "GTP";
    case OrderSession::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

/** Returns the session `text` names, defaulting to `REGULAR` for the reason above. */
inline OrderSession order_session_from(std::string_view text) noexcept {
  for (const auto session : {
         OrderSession::OPENING_AUCTION,
         OrderSession::CLOSING_AUCTION,
         OrderSession::PRE_MARKET,
         OrderSession::AFTER_HOURS,
         OrderSession::AFTER_HOURS_AUCTION,
         OrderSession::AFTER_MARKET,
         OrderSession::GTP,
         OrderSession::UNKNOWN,
       }) {
    if (text == order_session_name(session)) {
      return session;
    }
  }
  return OrderSession::REGULAR;
}

/**
 * Strict readers for a value a journal line actually carries.
 *
 * **A field that is absent and a field that is present but unreadable are not
 * the same thing, and the lenient readers below cannot tell them apart.** An
 * absent key means the line predates the field, and the documented default is
 * then a fact about what such a line could have recorded. A present token this
 * reader cannot parse means the line says something, and guessing what turns
 * damage into evidence: `"SELL"` damaged to `"SEL?"` reads as a buy, `"IOC"` to
 * `"IO?"` reads as a day order, and `"COMBINED"` to `"COMBIN?D"` moves the
 * order to a different instrument entirely. Recovery then matches a lookalike,
 * records its id and clears the hold on an order still at the exchange.
 *
 * So the strict readers answer `std::nullopt`, and `journal_entry_from_line`
 * turns that into an entry with **no instrument at all** -- which recovery
 * already treats as a global halt rather than withholding one instrument,
 * because a guessed instrument is not the key the runner is trading under.
 */
/**
 * Returns the decimal `text` holds, or nothing when it does not hold one.
 *
 * **`Decimal64::parse` throws**, and this reader's whole contract is to survive
 * a damaged file and report it. A quantity damaged to `"1?0"` would otherwise
 * raise `std::invalid_argument` out of `read_journal_file`, out of recovery, and
 * out of the runner's startup -- so a corrupt byte in the most load-bearing
 * field in the line takes the process down instead of producing the operator
 * halt the rest of this file is built to produce.
 *
 * It is also the same failure as the unreadable enumerations beside it, wearing
 * a different coat: a value that cannot be read must make the line
 * unattributable, not decide what happens next by escaping.
 */
inline std::optional<Decimal64> known_decimal_from(std::string_view text) noexcept {
  try {
    return Decimal64::parse(text);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

inline std::optional<Country> known_country_from(std::string_view text) noexcept {
  if (text == "KR") {
    return Country::KR;
  }
  if (text == "US") {
    return Country::US;
  }
  return std::nullopt;
}

inline std::optional<OrderSide> known_side_from(std::string_view text) noexcept {
  if (text == "BUY") {
    return OrderSide::BUY;
  }
  if (text == "SELL") {
    return OrderSide::SELL;
  }
  return std::nullopt;
}

inline std::optional<OrderType> known_order_type_from(std::string_view text) noexcept {
  for (const auto type : {
         OrderType::UNKNOWN,
         OrderType::LIMIT,
         OrderType::MARKET,
         OrderType::CONDITIONAL_LIMIT,
         OrderType::BEST_LIMIT,
         OrderType::PRIMARY_LIMIT,
         OrderType::MID_PRICE,
         OrderType::STOP_LIMIT,
         OrderType::CLOSING_PRICE,
       }) {
    if (text == order_type_name(type)) {
      return type;
    }
  }
  return std::nullopt;
}

inline std::optional<TimeInForce> known_time_in_force_from(std::string_view text) noexcept {
  for (const auto condition : {
         TimeInForce::UNKNOWN,
         TimeInForce::DAY,
         TimeInForce::IMMEDIATE_OR_CANCEL,
         TimeInForce::FILL_OR_KILL,
       }) {
    if (text == time_in_force_name(condition)) {
      return condition;
    }
  }
  return std::nullopt;
}

inline std::optional<OrderSession> known_order_session_from(std::string_view text) noexcept {
  for (const auto session : {
         OrderSession::UNKNOWN,
         OrderSession::REGULAR,
         OrderSession::OPENING_AUCTION,
         OrderSession::CLOSING_AUCTION,
         OrderSession::PRE_MARKET,
         OrderSession::AFTER_HOURS,
         OrderSession::AFTER_HOURS_AUCTION,
         OrderSession::AFTER_MARKET,
         OrderSession::GTP,
       }) {
    if (text == order_session_name(session)) {
      return session;
    }
  }
  return std::nullopt;
}


/** Returns the instant `micros` names, as microseconds since the epoch. */
inline std::chrono::system_clock::time_point instant_from(std::int64_t micros) noexcept {
  return std::chrono::system_clock::time_point{} + std::chrono::microseconds(micros);
}

/** Returns `value` as microseconds since the epoch. */
inline std::int64_t micros_of(std::chrono::system_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch()).count();
}

/**
 * Returns the text of a `"key":"value"` pair found by scanning, or nothing when it is not there.
 *
 * Deliberate, contained crudeness. It runs on exactly one input: a final line that a parser has
 * already refused, because the process died partway through writing it. Boost.JSON cannot help
 * there — an incomplete object is not a value, and `stream_parser` will not release one — so the
 * choice is between scraping what is legible and discarding a line that may be the only record
 * that an order exists.
 *
 * It is never used on a line that parsed. Anything it returns is reported as salvaged rather than
 * as read.
 */
inline std::optional<std::string> salvage_string(std::string_view line, std::string_view key) {
  std::string needle;
  needle.reserve(key.size() + 4);
  needle += '"';
  needle.append(key);
  needle += "\":\"";
  const auto at = line.find(needle);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  const auto begin = at + needle.size();
  const auto end = line.find('"', begin);
  if (end == std::string_view::npos) {
    // The value itself was cut off mid-write. Reporting what is there would invent a symbol.
    return std::nullopt;
  }
  return std::string(line.substr(begin, end - begin));
}

} // namespace journal_detail

/** The value of a journal header's `journal` field, identifying the format. */
inline constexpr std::string_view journal_format_name() noexcept {
  return "algotradelib-order-journal";
}

/**
 * Renders the header line that opens a journal and names the account it belongs to.
 *
 * Written once, before any instruction, and never again. Everything else in the file can tear; this
 * cannot, because it is complete before the first order is ever contemplated.
 */
inline std::string journal_header_line(std::string_view scope) {
  json::object row;
  row["journal"] = std::string(journal_format_name());
  row["version"] = 1;
  row["scope"] = std::string(scope);
  return json::serialize(row);
}

/** Returns the scope a header line declares, or nothing when the line is not a header. */
inline std::optional<std::string> journal_header_scope(std::string_view line) {
  boost::system::error_code error;
  const auto value = json::parse(line, error);
  if (error || !value.is_object()) {
    return std::nullopt;
  }
  const auto& row = value.as_object();
  const auto marker = row.if_contains("journal");
  if (marker == nullptr || !marker->is_string() ||
      std::string_view(marker->as_string()) != journal_format_name()) {
    return std::nullopt;
  }
  return journal_detail::string_field(row, "scope");
}

/** A journal file's bytes, and whether they were obtained at all. */
struct JournalBytes {
  JournalReadStatus status_{JournalReadStatus::MISSING};
  std::string text_;
  std::string detail_;
};

/** Renders one entry as the single JSON object that becomes one line of the journal. */
inline std::string journal_entry_to_line(const JournalEntry& entry) {
  json::object row;
  // Ordered so that the fields a damaged line most needs to yield come first. A truncated write
  // keeps its prefix, so putting the instrument ahead of the detail text is what makes salvage
  // work in practice rather than only in principle.
  row["id"] = entry.id_;
  row["action"] = std::string(journal_action_name(entry.action_));
  row["outcome"] = std::string(journal_outcome_name(entry.outcome_));
  row["country"] = std::string(journal_detail::country_name(entry.instrument_.country_));
  row["venue"] = std::string(journal_detail::venue_name(entry.instrument_.venue_));
  row["symbol"] = entry.instrument_.symbol_;
  row["side"] = std::string(journal_detail::side_name(entry.side_));
  row["type"] = std::string(journal_detail::order_type_name(entry.type_));
  row["time_in_force"] = std::string(journal_detail::time_in_force_name(entry.time_in_force_));
  row["session"] = std::string(journal_detail::order_session_name(entry.session_));
  // Distinct from `venue`, which is the instrument's identity venue. This is
  // where the order actually goes -- the binding resolves a request that named
  // no venue into the one the broker's default reaches -- and a line that omits
  // the key entirely is
  // one written before the field existed -- which is not the same as one that
  // asked for no particular venue, and recovery treats the two differently.
  if (entry.venue_) {
    row["route"] = std::string(journal_detail::venue_name(*entry.venue_));
  }
  row["quantity"] = entry.quantity_.str();
  if (entry.price_) {
    row["price"] = entry.price_->str();
  }
  // The authoritative instant, exact on every platform. `at_text` beside it is for the operator
  // reading the file with `tail`, is only second-resolution, and is never read back: one field is
  // the record and the other is the caption.
  row["at"] = journal_detail::micros_of(entry.recorded_at_);
  row["at_text"] = format_iso8601_utc(entry.recorded_at_);
  if (!entry.order_id_.empty()) {
    row["order_id"] = entry.order_id_;
  }
  if (!entry.detail_.empty()) {
    row["detail"] = entry.detail_;
  }
  if (entry.requires_operator_) {
    row["requires_operator"] = true;
  }
  if (!entry.scope_.empty()) {
    row["scope"] = entry.scope_;
  }
  return json::serialize(row);
}

/**
 * Returns the record that stands for a torn line too short to name anything.
 *
 * Carries no instrument, which is exactly what makes it dangerous and exactly what it has to say:
 * something was in flight and the journal cannot name it. Recovery turns this into a global halt
 * rather than withholding one instrument, because there is no instrument to withhold.
 */
inline JournalEntry unattributable_journal_entry(std::string_view line) {
  JournalEntry entry;
  entry.id_ = "unknown~truncated";
  entry.outcome_ = JournalOutcome::PENDING;
  entry.requires_operator_ = true;
  entry.detail_ = "a truncated journal line named no instrument; something was in flight: " +
    std::string(line.substr(0, line.size() < 120 ? line.size() : 120));
  return entry;
}

/** Parses one journal line, or reports nothing when it is not a complete journal object. */
inline std::optional<JournalEntry> journal_entry_from_line(std::string_view line) {
  boost::system::error_code error;
  const auto value = json::parse(line, error);
  if (error || !value.is_object()) {
    return std::nullopt;
  }
  const auto& row = value.as_object();

  const auto action = journal_action_from(journal_detail::string_field(row, "action"));
  const auto outcome = journal_outcome_from(journal_detail::string_field(row, "outcome"));
  // An operator-only line is exempt from the bound below, and deliberately: a
  // salvaged fragment records the epoch precisely *because* it has no
  // timestamp, and `requires_operator_` is what stops that epoch window
  // resolving it. Repairing such a line writes it back, so rejecting the epoch
  // here would make every repair unreadable on the next open. The bound is for
  // a line that claims to be ordinary and carries an instant it cannot have.
  const auto operator_only = row.if_contains("requires_operator");
  const bool marked = operator_only != nullptr && operator_only->is_bool() &&
    operator_only->as_bool();

  const auto at = row.if_contains("at");
  if (!action || !outcome || at == nullptr || !at->is_int64() ||
      (!marked && !journal_detail::plausible_instant(at->as_int64()))) {
    // A well-formed JSON object that is not a journal line. Treated as damage rather than as an
    // empty entry, because an entry with a default instrument would be a claim about an order.
    return std::nullopt;
  }

  // **A present value this reader cannot parse is damage, not a default.**
  // Each of these has a documented default for an *absent* key -- a line that
  // predates the field -- and applying that default to a token the line does
  // carry turns damage into evidence: a damaged `"SELL"` reads as a buy, a
  // damaged `"IOC"` as a day order, and a damaged `"COMBINED"` moves the entry
  // to an instrument the runner is not trading, so the ledger marks the wrong
  // key and the right one is never withheld at all.
  //
  // An unreadable value therefore produces an entry with **no instrument**,
  // which `unattributable_journal_entry` documents and recovery already turns
  // into a global halt. Marking a guessed instrument would not protect the real
  // one, because they are different keys.
  // Each field is asked two questions, because they have different answers.
  // *Is the key there?* -- if not, the line predates the field and its
  // documented default is a fact about such a line. *Is what it holds
  // readable?* -- if not, the line says something this release cannot use, and
  // guessing turns damage into evidence. `string_field` answers neither: it
  // flattens absent, non-string and empty into the same `""`.
  const auto read = [&row](std::string_view key) {
    return journal_detail::readable_string(row, key);
  };
  // Present but unusable, for any reason: not a string, an empty string, or a
  // token the reader could not parse. The first version of this asked only
  // whether the *string* was readable, which let `"IOC"` damaged to `"IO?"`
  // through -- that is a perfectly good string, and a perfectly bad condition.
  const auto unusable = [&row](std::string_view key, bool parsed) {
    return journal_detail::has_field(row, key) && !parsed;
  };

  // `std::optional::and_then` is C++23 and this library is C++17.
  const auto parsed = [&read](std::string_view key, auto reader) {
    using Result = decltype(reader(std::string_view{}));
    const auto text = read(key);
    return text ? reader(*text) : Result{};
  };

  const auto symbol = read("symbol");
  const auto country = parsed("country", journal_detail::known_country_from);
  const auto venue = parsed("venue", journal_detail::known_venue_from);
  const auto side = parsed("side", journal_detail::known_side_from);
  const auto type = parsed("type", journal_detail::known_order_type_from);
  const auto condition = parsed("time_in_force", journal_detail::known_time_in_force_from);
  const auto session = parsed("session", journal_detail::known_order_session_from);
  const auto quantity = parsed("quantity", journal_detail::known_decimal_from);
  const auto price = parsed("price", journal_detail::known_decimal_from);

  const bool damaged =
    // Identity and the fields every complete line carries: required outright.
    // A line this reader cannot attribute to an instrument must not be
    // attributed to a guess, and `quantity` at a silent zero is a claim about
    // an order's size.
    !symbol || !country || !venue || !side || !type || !quantity ||
    // Legacy-defaulted fields: absent is fine, present and unreadable is not.
    unusable("time_in_force", condition.has_value()) ||
    unusable("session", session.has_value()) ||
    // A price is absent for a market order and required for a basis that
    // carries one, so its presence is judged against the basis rather than
    // assumed either way.
    unusable("price", price.has_value());
  // **A price missing from a priced basis, and a cancellation with no target
  // id, are deliberately *not* damage here.** Both are lines this library's own
  // writer emits: `JournalingExecutor` appends the pending line *before* the
  // executor runs, so an order the binding then refuses locally -- a limit with
  // no price, a cancellation of a reservation whose broker id is not known --
  // has already been written by the time anyone could object. Rejecting them
  // turned an ordinary local refusal into a global halt on the next restart,
  // which is worse than the hazard the rejection was meant to close.
  //
  // The hazard is real and it lives one step later: such a line must not be
  // *resolved* on evidence it does not have. `runner::match_against` refuses to
  // conclude anything from one, which leaves it withheld for an operator while
  // its own terminal line still closes it when the refusal was ordinary.
  if (damaged) {
    auto unattributable = unattributable_journal_entry(line);
    const auto id = journal_detail::string_field(row, "id");
    unattributable.id_ = (id.empty() ? std::string("unknown") : id) + "~damaged";
    unattributable.action_ = *action;
    // Its own outcome, not `PENDING`. Both lines of a pair carry the same id, so
    // a damaged *terminal* line still closes its damaged pending partner --
    // forcing `PENDING` left both open and halted a runner over an instruction
    // that had already resolved.
    unattributable.outcome_ = *outcome;
    unattributable.recorded_at_ = journal_detail::instant_from(at->as_int64());
    unattributable.detail_ =
      "a journal line carries a value this release cannot read, so the order it describes cannot "
      "be identified; nothing may be traded until an operator resolves it: " +
      std::string(line.substr(0, line.size() < 160 ? line.size() : 160));
    return unattributable;
  }

  JournalEntry entry;
  entry.id_ = journal_detail::string_field(row, "id");
  entry.action_ = *action;
  entry.outcome_ = *outcome;
  entry.instrument_ = InstrumentId{*country, *venue, *symbol};
  entry.side_ = *side;
  entry.type_ = *type;
  // These two keep their documented defaults for an *absent* key only; a
  // present value that failed to parse was caught above.
  entry.time_in_force_ = condition ? *condition : TimeInForce::DAY;
  entry.session_ = session ? *session : OrderSession::REGULAR;
  // Absent key -> absent value, which is "this line predates the field" rather
  // than "no route was asked for". Recovery will not attribute a row to an
  // instruction whose route nobody recorded. The field defaults to a recorded
  // `UNSPECIFIED`, so this clears it rather than leaving the default standing.
  // An absent key means the line predates the field. An unrecognized value means
  // the line says something this reader cannot use. Both are "unknown", and
  // neither may become the `UNSPECIFIED` that recovery treats as a recorded
  // request for no venue.
  entry.venue_ = journal_detail::known_venue_from(journal_detail::string_field(row, "route"));
  // Damaged numbers land in the same place as damaged enumerations, for the
  // same reason -- and they used to land somewhere far worse, throwing out of
  // recovery entirely. See `known_decimal_from`.
  entry.quantity_ = *quantity;
  if (price) {
    entry.price_ = *price;
  }
  entry.recorded_at_ = journal_detail::instant_from(at->as_int64());
  entry.order_id_ = journal_detail::string_field(row, "order_id");
  entry.detail_ = journal_detail::string_field(row, "detail");
  entry.requires_operator_ = marked;
  entry.scope_ = journal_detail::string_field(row, "scope");
  return entry;
}

/**
 * Salvages what a damaged final line still says, as an entry nothing can resolve.
 *
 * Two rules, and both are about refusing to be clever with a line that is by definition incomplete:
 *
 * - **Its id is rewritten.** A damaged line must never close a `PENDING` line, because its own
 *   `outcome` field may itself be the part that was cut off. Suffixing the id guarantees it pairs
 *   with nothing.
 * - **Its outcome is `PENDING`, whatever the text said.** An unresolved entry costs one
 *   reconciliation; a wrongly resolved one risks an order nobody is watching. Only one of those
 *   can lose money, which is the same asymmetry `outcome_of` is built on.
 */
/**
 * Returns whether `line` carries `key` with a value that *finished being
 * written* and still could not be read.
 *
 * Three states, and a torn line has all three. `salvage_string` recognizes only
 * `"key":"value"`, so it answers nothing both for a key whose value is `null`,
 * a number or a bare token -- **damage** -- and for one the truncation cut off
 * -- **expected**. Telling them apart is what decides whether a default is a
 * fact about a torn line or a guess about a damaged one.
 *
 * A fragment reading `"country":null,"venue":"COMBINED","symbol":"AAPL"` was
 * salvaged as a *Korean* instrument, marked operator-only on that guessed key,
 * and left the real US instrument free to trade. But the first version of this
 * asked only whether the key was *mentioned*, which made the fragment's own
 * half-written final field -- the ordinary shape of truncation -- look like
 * damage too, and turned every torn line into a global halt.
 *
 * The comma is the tell: a value followed by one was written to completion, so
 * failing to read it is damage. A value with nothing after it is where the
 * writer stopped.
 */
inline bool salvage_damaged(std::string_view line, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const auto at = line.find(needle);
  if (at == std::string_view::npos) {
    return false;
  }
  return line.find(',', at + needle.size()) != std::string_view::npos;
}

inline std::optional<JournalEntry> salvage_journal_line(std::string_view line) {
  const auto symbol = journal_detail::salvage_string(line, "symbol");
  if (!symbol || symbol->empty()) {
    return std::nullopt;
  }
  JournalEntry entry;
  // The flag that makes the promise in this comment block true. Without it the entry's missing
  // timestamp reads as the epoch, every window test passes immediately, no real row falls inside
  // the window, and the first sweep resolves it `NOT_PLACED`.
  entry.requires_operator_ = true;
  const auto id = journal_detail::salvage_string(line, "id");
  entry.id_ = (id ? *id : std::string("unknown")) + "~truncated";
  entry.action_ =
    journal_action_from(journal_detail::salvage_string(line, "action").value_or("place"))
      .value_or(JournalAction::PLACE);
  entry.outcome_ = JournalOutcome::PENDING;
  // **Absent is a default; present and unreadable is damage** -- the same rule
  // the strict reader follows, and for the same reason. A truncated line is
  // missing its *tail*, so an absent field is expected and its default is a fact
  // about what the line could have said. A field that is there and cannot be
  // read is corruption in the middle, and guessing it hands this entry a
  // different instrument than the order had: `requires_operator_` would then
  // withhold a key the runner is not trading under, while the real one stays
  // open. Such a line becomes unattributable instead, carrying no instrument at
  // all, which recovery turns into a global halt.
  // A key the truncation removed takes its default; a key that is *there* and
  // unreadable is damage, and `salvage_string` cannot tell those apart on its
  // own -- it recognizes `"key":"value"` and nothing else, so `null` and `7`
  // both read as absent.
  const auto salvaged = [&line](std::string_view key, const char* fallback) {
    const auto text = journal_detail::salvage_string(line, key);
    if (text) {
      return std::optional<std::string>(*text);
    }
    return salvage_damaged(line, key) ? std::optional<std::string>()
                                                       : std::optional<std::string>(fallback);
  };
  const auto parsed = [&salvaged](std::string_view key, const char* fallback, auto reader) {
    using Result = decltype(reader(std::string_view{}));
    const auto text = salvaged(key, fallback);
    return text ? reader(*text) : Result{};
  };

  const auto country = parsed("country", "KR", journal_detail::known_country_from);
  const auto venue = parsed("venue", "UNSPECIFIED", journal_detail::known_venue_from);
  const auto side = parsed("side", "BUY", journal_detail::known_side_from);
  const auto type = parsed("type", "UNKNOWN", journal_detail::known_order_type_from);
  const auto condition =
    parsed("time_in_force", "DAY", journal_detail::known_time_in_force_from);
  const auto session = parsed("session", "REGULAR", journal_detail::known_order_session_from);
  if (!country || !venue || !side || !type || !condition || !session) {
    auto unattributable = unattributable_journal_entry(line);
    unattributable.id_ = entry.id_;
    unattributable.detail_ =
      "a truncated journal line carries a value this release cannot read, so the order it "
      "describes cannot be identified; nothing may be traded until an operator resolves it: " +
      std::string(line.substr(0, line.size() < 160 ? line.size() : 160));
    return unattributable;
  }
  entry.instrument_ = InstrumentId{*country, *venue, *symbol};
  entry.side_ = *side;
  entry.type_ = *type;
  entry.time_in_force_ = *condition;
  entry.session_ = *session;
  entry.venue_ = journal_detail::known_venue_from(
    journal_detail::salvage_string(line, "route").value_or("")
  );
  if (const auto quantity = journal_detail::salvage_string(line, "quantity")) {
    // A truncated line's last field is routinely half-written, so an unparseable
    // number here is expected rather than exceptional -- and it must not throw
    // out of a salvage path that exists to survive exactly this.
    if (const auto value = journal_detail::known_decimal_from(*quantity)) {
      entry.quantity_ = *value;
    }
  }
  if (const auto price = journal_detail::salvage_string(line, "price")) {
    if (const auto value = journal_detail::known_decimal_from(*price)) {
      entry.price_ = *value;
    }
  }
  entry.detail_ = "recovered from a truncated journal line; only an operator can resolve it";
  return entry;
}


/**
 * Reads a journal file into a replay, salvaging a truncated final line.
 *
 * A free function rather than a member so that recovery can read a journal a crashed predecessor
 * left behind without opening it for appending — which is what an operator's inspection tool wants,
 * and what a test asserting truncation behaviour needs.
 *
 * An absent file is an intact, empty journal, not an error. A runner starting for the first time
 * has no journal and owes nothing, and that is a different situation from a journal that exists and
 * cannot be read.
 */
inline JournalBytes read_journal_bytes(const std::string& path) {
  JournalBytes bytes;
  errno = 0;
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(
    std::fopen(path.c_str(), "rb"), [](std::FILE* file) { return file ? std::fclose(file) : 0; }
  );
  if (handle == nullptr) {
    if (errno == ENOENT) {
      bytes.status_ = JournalReadStatus::MISSING;
      return bytes;
    }
    // Every other reason -- a permission the process does not have, a directory where a file was
    // expected, a device error -- means the file exists and its contents are unknown. Reporting
    // that as "no journal" is how a process claims a path holding its predecessor's in-flight
    // orders and then trades on an empty ledger.
    bytes.status_ = JournalReadStatus::UNREADABLE;
    bytes.detail_ = "the order journal at " + path + " exists and could not be opened for reading";
    return bytes;
  }

  char buffer[4096];
  while (const auto read = std::fread(buffer, 1, sizeof(buffer), handle.get())) {
    bytes.text_.append(buffer, read);
  }
  if (std::ferror(handle.get()) != 0) {
    // A read that stopped partway looks exactly like an ordinary end of file to the loop above, so
    // the truncated tail it produces would be a fiction about where the journal ends.
    bytes.status_ = JournalReadStatus::UNREADABLE;
    bytes.detail_ = "the order journal at " + path + " could not be read to the end";
    bytes.text_.clear();
    return bytes;
  }
  bytes.status_ = JournalReadStatus::READ;
  return bytes;
}

/**
 * Parses journal bytes into a replay, salvaging a truncated final line.
 *
 * Split from the reading so the failure paths are drivable without a filesystem: a test hands this
 * an `UNREADABLE` status directly rather than arranging a permission the platform may or may not
 * enforce the same way twice.
 *
 * An absent file is an intact, empty journal, not an error. A runner starting for the first time has
 * no journal and owes nothing, and that is a different situation from a journal that exists and
 * cannot be read -- which is why the two arrive here as different statuses rather than as the same
 * empty result.
 */
inline JournalReplay parse_journal_bytes(const JournalBytes& bytes) {
  JournalReplay replay;
  replay.status_ = bytes.status_;
  if (bytes.status_ != JournalReadStatus::READ) {
    replay.detail_ = bytes.detail_;
    return replay;
  }

  const auto& text = bytes.text_;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    if (end == std::string::npos) {
      // Everything after the last newline is a write that never finished. This is the whole reason
      // the format is line-oriented, and dropping it silently would defeat the feature.
      replay.truncated_tail_ = true;
      const std::string_view tail(text.data() + begin, text.size() - begin);
      if (auto salvaged = salvage_journal_line(tail)) {
        replay.detail_ = "the final journal line was truncated; it named " +
          salvaged->instrument_.symbol_ + " and is treated as unresolved";
        replay.entries_.push_back(std::move(*salvaged));
      } else {
        replay.detail_ =
          "the final journal line was truncated and named no instrument; something was in flight";
      }
      break;
    }
    const std::string_view line(text.data() + begin, end - begin);
    begin = end + 1;
    if (line.empty()) {
      continue;
    }
    if (auto scope = journal_header_scope(line)) {
      // Not an instruction and not damage. Recorded so an opener can check ownership before it
      // writes anything at all.
      replay.has_header_ = true;
      replay.scope_ = std::move(*scope);
      continue;
    }
    if (auto entry = journal_entry_from_line(line)) {
      replay.entries_.push_back(std::move(*entry));
    } else {
      // A complete line that is not a journal entry. Not expected from this writer, so it means
      // the file was edited or written by something else, or a repair was interrupted; counted
      // rather than ignored, and recovery halts on it.
      ++replay.unreadable_lines_;
      if (replay.detail_.empty()) {
        replay.detail_ = "the journal contains a line this library did not write";
      }
    }
  }
  return replay;
}

/** Reads a journal file into a replay, reporting a file that exists and cannot be read. */
inline JournalReplay read_journal_file(const std::string& path) {
  return parse_journal_bytes(read_journal_bytes(path));
}

/**
 * An append-only journal on disk, one JSON object per line, durable before the call returns.
 *
 * **Why JSON Lines and not something with a schema.** The window being closed is "the process was
 * killed mid-write", so the format has to make a partial write *detectable and local*. A document
 * rewritten in place can be left corrupt as a whole; a line-oriented file can only lose its last
 * line, and the missing newline says so. Boost.JSON is already a required dependency, so this adds
 * nothing to the build, and an operator can read the file with `tail` during an incident — which is
 * the interface that matters at the moment it matters.
 *
 * **One journal, one writer.** Two processes appending to one file is not supported and is a larger
 * problem than a journal: two runners on one account will double positions regardless of what they
 * write down.
 *
 * **An append that fails is reported, not swallowed.** `append` returns whether the line is durable,
 * and `JournalingExecutor` refuses to send when it is not. A journal that silently lost a line
 * would be worse than no journal at all: it would make the runner believe the window was closed.
 */
class FileJournal {
public:
  /**
   * Opens `path` for appending, creating it when absent.
   *
   * Failure is not thrown. It is reported through `open()`, so a caller can decide — a runner that
   * cannot journal should refuse to trade, and that decision belongs to the program, not to a
   * constructor.
   */
  FileJournal(std::string path, std::string scope)
  : path_(std::move(path)), scope_(std::move(scope)), handle_(nullptr, &closer) {
    if (scope_.empty()) {
      // An unnamed journal cannot be checked against anything, so it would silently accept another
      // account's file -- which is the whole failure this exists to prevent.
      rejection_ = "an order journal must name the broker and account it belongs to";
      return;
    }
    if (!claim_file()) {
      return;
    }
    repair_torn_tail();
    handle_.reset(std::fopen(path_.c_str(), "ab"));
    if (!open()) {
      return;
    }
    if (needs_header_ && !write_header()) {
      return;
    }
    if (!pending_repair_.empty()) {
      finish_repair();
    }
  }

  /** Returns whether the file is open for appending. */
  bool open() const noexcept { return handle_ != nullptr; }

  /**
   * Returns what opening the file had to repair, or an empty view when nothing was wrong.
   *
   * A predecessor killed mid-append leaves a line with no newline. Appending straight onto it would
   * glue the next JSON object to the fragment, and the *combined* text would then carry a newline --
   * so the next replay would see one complete unreadable line rather than a torn tail, losing both
   * the fragment and the new instruction with it. Opening therefore terminates the fragment and
   * writes a salvaged record beside it before accepting anything new.
   */
  const std::string& repaired() const noexcept { return repaired_; }

  /** Returns the path being written. */
  const std::string& path() const noexcept { return path_; }

  /** Returns the broker and account this journal belongs to. */
  const std::string& scope() const noexcept { return scope_; }

  /**
   * Returns why the journal refused to open, or an empty view when it did not refuse.
   *
   * There is one reason: the file already holds records for a different account. Reusing a path
   * across accounts is an easy mistake -- a copied config, a renamed environment -- and it is not a
   * harmless one. The symbol, side, quantity and price of an order can all agree across two
   * accounts trading the same instrument, so a sweep would reconcile one account's unresolved
   * instruction against the other's rows and clear a hold for an order that is still resting.
   * Refusing to open is the only response that cannot be ignored.
   */
  const std::string& rejection() const noexcept { return rejection_; }

  /**
   * Appends one line and returns whether it reached the disk.
   *
   * The line and its terminating newline are written in one call, which is what makes the newline a
   * commit marker: a write cut short loses the tail of the buffer, so a line that ends in `\n` is a
   * line that was written whole.
   */
  bool append(const JournalEntry& entry) {
    if (!open()) {
      return false;
    }
    auto stamped = entry;
    // Overwritten, not filled in. A caller-supplied scope that disagreed with the file's would put
    // a record in this journal claiming to belong to another account, which is the confusion the
    // header exists to rule out. The journal owns this field.
    stamped.scope_ = scope_;
    auto line = journal_entry_to_line(stamped);
    line += '\n';
    const auto written = std::fwrite(line.data(), 1, line.size(), handle_.get());
    if (written != line.size()) {
      return false;
    }
    return journal_detail::flush_to_disk(handle_.get());
  }

  /**
   * Reads the whole journal back, reporting anything it could not read.
   *
   * Opened separately for reading rather than seeking the append handle, because this runs once at
   * startup and mixing a read position into a handle opened for appending is a portability trap
   * nobody needs to step in for a call made once per process.
   */
  JournalReplay replay() const { return read_journal_file(path_); }

private:
  /**
   * Returns whether this journal may write to the file, deciding ownership from the header alone.
   *
   * **The header, not the records, and that distinction is the fix.** Reading ownership from the
   * records leaves a hole exactly where it matters most: the scope is one field among many on a
   * record, so a process killed partway through writing the *first* instruction leaves a file whose
   * only line is torn and unscoped. Another account could then open the path, repair that line into
   * its own journal, and reconcile the first account's instruction against its own rows.
   *
   * A header is written before any instruction is contemplated, so it is complete or the file is
   * empty. Three cases, and the third is the strict one:
   *
   * - **Empty or absent.** Ours to claim; the header is written before the first append.
   * - **Header present.** The scopes must agree.
   * - **Non-empty with no header.** Refused. Something else wrote this file, or it predates the
   *   format, and either way its ownership cannot be established -- which is not a question to
   *   guess at while holding a file that may name another account's live orders.
   */
  bool claim_file() {
    const auto replay = read_journal_file(path_);
    if (!replay.readable()) {
      // Opening for *append* can succeed on a file that cannot be read -- a write-only file is the
      // plain case -- so without this the journal would claim a path whose contents it has never
      // seen, write a header into it, and report nothing owed.
      rejection_ = replay.detail_.empty() ?
        "the journal at " + path_ + " exists and cannot be read" :
        replay.detail_;
      return false;
    }
    const bool empty = !replay.has_header_ && replay.entries_.empty() &&
      replay.unreadable_lines_ == 0 && !replay.truncated_tail_;
    if (empty) {
      needs_header_ = true;
      return true;
    }
    if (!replay.has_header_) {
      rejection_ = "the journal at " + path_ +
        " has no header, so the account it belongs to cannot be established";
      return false;
    }
    if (replay.scope_ != scope_) {
      rejection_ = "the journal at " + path_ + " belongs to " + replay.scope_ + ", not to " +
        scope_;
      return false;
    }
    return true;
  }

  /** Writes and flushes the header, before any instruction can be recorded. */
  bool write_header() {
    auto line = journal_header_line(scope_);
    line += '\n';
    const auto written = std::fwrite(line.data(), 1, line.size(), handle_.get());
    if (written != line.size() || !journal_detail::flush_to_disk(handle_.get())) {
      handle_.reset();
      rejection_ = "the journal header at " + path_ + " could not be written";
      return false;
    }
    needs_header_ = false;
    return true;
  }

  /** Closes a stream, tolerating the null a failed open leaves behind. */
  static void closer(std::FILE* handle) noexcept {
    if (handle != nullptr) {
      std::fclose(handle);
    }
  }

  /**
   * Reads the file and works out what a torn final line needs, without writing anything yet.
   *
   * Split from the writing half because the file must be inspected *before* it is opened for
   * appending: once a stream is in append mode, everything written goes to the end, which is
   * precisely what must not happen until the fragment is terminated.
   */
  void repair_torn_tail() {
    const auto replay = read_journal_file(path_);
    if (!replay.truncated_tail_) {
      return;
    }
    const auto tail = read_torn_tail(path_);
    auto salvaged = salvage_journal_line(tail);
    pending_repair_ = salvaged ?
      journal_entry_to_line(*salvaged) :
      journal_entry_to_line(unattributable_journal_entry(tail));
    repaired_ = salvaged ?
      "a torn final line was terminated and salvaged as " + salvaged->instrument_.symbol_ :
      "a torn final line was terminated; it named no instrument";
  }

  /** Terminates the fragment and appends the record that stands for it, durably. */
  void finish_repair() {
    // The newline first and on its own: it is what turns the fragment into a complete -- if
    // unreadable -- line, so that the record written next is a line of its own rather than a
    // continuation of a half-written one.
    std::string repair = "\n";
    repair += pending_repair_;
    repair += '\n';
    const auto written = std::fwrite(repair.data(), 1, repair.size(), handle_.get());
    if (written != repair.size() || !journal_detail::flush_to_disk(handle_.get())) {
      // The repair itself failed, so the file is still torn. Refusing to be open is the only safe
      // answer: a caller that cannot journal must not trade, and `open()` is how it finds out.
      handle_.reset();
      repaired_ = "a torn final line could not be repaired";
      return;
    }
    pending_repair_.clear();
  }

  /** Returns the text after the file's last newline, which is the fragment a kill left behind. */
  static std::string read_torn_tail(const std::string& path) {
    std::unique_ptr<std::FILE, void (*)(std::FILE*)> reader(
      std::fopen(path.c_str(), "rb"), &closer
    );
    if (reader == nullptr) {
      return {};
    }
    std::string text;
    char buffer[4096];
    while (const auto read = std::fread(buffer, 1, sizeof(buffer), reader.get())) {
      text.append(buffer, read);
    }
    const auto last = text.rfind('\n');
    return last == std::string::npos ? text : text.substr(last + 1);
  }

  std::string path_;
  std::string scope_;
  std::unique_ptr<std::FILE, void (*)(std::FILE*)> handle_;
  std::string pending_repair_;
  std::string repaired_;
  std::string rejection_;
  bool needs_header_{false};
};

} // namespace runner
} // namespace algotradelib
