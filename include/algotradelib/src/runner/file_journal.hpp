#pragma once

#include "./journal.hpp"

#include <algotradelib/core>

#include <boost/json.hpp>

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
inline std::string string_field(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    return {};
  }
  return std::string(value->as_string());
}

/**
 * Returns the country `text` names, defaulting to `KR`.
 *
 * The journal's own wire vocabulary, deliberately spelled out here rather than derived from the
 * enumerator order. An enum's numeric value is an implementation detail that a reordering may
 * change; a file written by yesterday's build has to be readable by today's.
 */
inline Country country_from(std::string_view text) noexcept {
  return text == "US" ? Country::US : Country::KR;
}

/** Returns a stable name for `country`, as written to the journal. */
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

/** Returns the venue `text` names, defaulting to `UNSPECIFIED`. */
inline Venue venue_from(std::string_view text) noexcept {
  if (text == "KRX") {
    return Venue::KRX;
  }
  if (text == "NXT") {
    return Venue::NXT;
  }
  if (text == "COMBINED") {
    return Venue::COMBINED;
  }
  return Venue::UNSPECIFIED;
}

/** Returns a stable name for `side`, as written to the journal. */
inline constexpr std::string_view side_name(OrderSide side) noexcept {
  return side == OrderSide::SELL ? "SELL" : "BUY";
}

/** Returns the side `text` names, defaulting to `BUY`. */
inline OrderSide side_from(std::string_view text) noexcept {
  return text == "SELL" ? OrderSide::SELL : OrderSide::BUY;
}

/** Returns a stable name for `type`, as written to the journal. */
inline constexpr std::string_view order_type_name(OrderType type) noexcept {
  switch (type) {
    case OrderType::LIMIT:
      return "LIMIT";
    case OrderType::MARKET:
      return "MARKET";
    case OrderType::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

/** Returns the order type `text` names, defaulting to `UNKNOWN`. */
inline OrderType order_type_from(std::string_view text) noexcept {
  if (text == "LIMIT") {
    return OrderType::LIMIT;
  }
  if (text == "MARKET") {
    return OrderType::MARKET;
  }
  return OrderType::UNKNOWN;
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
  const auto at = row.if_contains("at");
  if (!action || !outcome || at == nullptr || !at->is_int64()) {
    // A well-formed JSON object that is not a journal line. Treated as damage rather than as an
    // empty entry, because an entry with a default instrument would be a claim about an order.
    return std::nullopt;
  }

  JournalEntry entry;
  entry.id_ = journal_detail::string_field(row, "id");
  entry.action_ = *action;
  entry.outcome_ = *outcome;
  entry.instrument_ = InstrumentId{
    journal_detail::country_from(journal_detail::string_field(row, "country")),
    journal_detail::venue_from(journal_detail::string_field(row, "venue")),
    journal_detail::string_field(row, "symbol"),
  };
  entry.side_ = journal_detail::side_from(journal_detail::string_field(row, "side"));
  entry.type_ = journal_detail::order_type_from(journal_detail::string_field(row, "type"));
  const auto quantity = journal_detail::string_field(row, "quantity");
  if (!quantity.empty()) {
    entry.quantity_ = Decimal64::parse(quantity);
  }
  const auto price = journal_detail::string_field(row, "price");
  if (!price.empty()) {
    entry.price_ = Decimal64::parse(price);
  }
  entry.recorded_at_ = journal_detail::instant_from(at->as_int64());
  entry.order_id_ = journal_detail::string_field(row, "order_id");
  entry.detail_ = journal_detail::string_field(row, "detail");
  const auto operator_only = row.if_contains("requires_operator");
  entry.requires_operator_ = operator_only != nullptr && operator_only->is_bool() &&
    operator_only->as_bool();
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
  entry.instrument_ = InstrumentId{
    journal_detail::country_from(journal_detail::salvage_string(line, "country").value_or("KR")),
    journal_detail::venue_from(
      journal_detail::salvage_string(line, "venue").value_or("UNSPECIFIED")
    ),
    *symbol,
  };
  entry.side_ =
    journal_detail::side_from(journal_detail::salvage_string(line, "side").value_or("BUY"));
  entry.type_ = journal_detail::order_type_from(
    journal_detail::salvage_string(line, "type").value_or("UNKNOWN")
  );
  if (const auto quantity = journal_detail::salvage_string(line, "quantity")) {
    entry.quantity_ = Decimal64::parse(*quantity);
  }
  if (const auto price = journal_detail::salvage_string(line, "price")) {
    entry.price_ = Decimal64::parse(*price);
  }
  entry.detail_ = "recovered from a truncated journal line; only an operator can resolve it";
  return entry;
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
inline JournalReplay read_journal_file(const std::string& path) {
  JournalReplay replay;
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(
    std::fopen(path.c_str(), "rb"), [](std::FILE* file) { return file ? std::fclose(file) : 0; }
  );
  if (handle == nullptr) {
    return replay;
  }

  std::string text;
  char buffer[4096];
  while (const auto read = std::fread(buffer, 1, sizeof(buffer), handle.get())) {
    text.append(buffer, read);
  }

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
      // the file was edited or written by something else; counted rather than ignored.
      ++replay.unreadable_lines_;
      if (replay.detail_.empty()) {
        replay.detail_ = "the journal contains a line this library did not write";
      }
    }
  }
  return replay;
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
