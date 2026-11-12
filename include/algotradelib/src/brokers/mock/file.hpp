#pragma once

#include "./market.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace algotradelib {
namespace mock {

/**
 * @brief A mock market that keeps its account in a file: every placement, cancellation and clock move is
 *   written before it is made, and opening the file again rebuilds the same market.
 *
 * For a program that runs once per command and trades the same account each time. Build it with the bars,
 * settings and reference data the market was first built with: the file's first line holds a digest of them
 * and of the rules the market fills by, and a file whose first line does not match is refused. The digest
 * covers every series in the order given with every bar, the starting cash and instant, the fill model (the
 * market-price rule, the limit price, the whole commission policy, the slippage and the sell tax; not the
 * tariff's name), the session window's times, the market's calendar and each instrument's reference data.
 * It detects a different market; it does not authenticate the file.
 *
 * Each later line records one change and its instant, never an answer: opening replays them, and the market
 * answers again as it did. A change is made only once its whole line is written and flushed to disk. A line
 * cut short (the process was killed while writing it) is never replayed, and opening removes it. Opening flushes
 * the file and its folder before the market takes a change, and refuses a file it cannot flush. A file refused as
 * another market's, of another format or rules, or damaged is left byte for byte as it was; an incomplete last line
 * or header may be removed before an open that then fails, such as one whose flush fails. When a
 * line cannot be written whole, the change is not made and the call answers as `Market::place` documents
 * (`state_not_written`); when it was written and flushing it failed, the change is not made here but
 * reopening may make it (`state_unconfirmed`). After either, the market refuses every placement,
 * cancellation and clock move until the file is opened again; reads answer as before. A market that halted
 * reopens halted, at the bar it halted on.
 *
 * Only a market whose settings fully describe it can be kept: a fill model whose market orders are priced by a
 * caller's own function is refused. Refused calls (an order the market refuses, a cancellation of no open
 * order) change nothing and are not recorded. The file is replayed whole each time it is opened, so opening
 * takes longer as its history grows. The client's scripted faults (`MockClient::fail_next`) are not kept.
 *
 * On POSIX the file is locked (`flock`) while it is open, so a second `MarketFile` on it, in any process, is
 * refused; a process that forks without `exec` shares the lock with its child. Keep it on a local disk. Windows
 * takes no lock and does not flush the file's folder. A file is opened only where the system clock counts in
 * the unit it was written with.
 *
 * Not copyable or movable: the market stays at one address while the file is open.
 */
class MarketFile {
public:
  /** @brief The file format's number, on its first line. */
  static constexpr std::int64_t FORMAT = 1;
  /**
   * @brief The number of the market's rules a file replays under, raised when a fill, cost, ledger, expiry or calendar
   *   rule changes.
   */
  static constexpr std::int64_t SEMANTICS = 1;

  /**
   * @brief Opens or creates the file, replays it into a market built as `Market`'s constructor builds one, and
   *   takes its lock.
   *
   * Never throws for the file: check `open()` and `rejection()`. A market whose file did not open refuses every
   * placement, cancellation and clock move (`state_not_written`).
   *
   * @param path The file; created if absent, in a folder that exists.
   * @param bars As `Market`'s constructor takes them.
   * @param settings As `Market`'s constructor takes them.
   * @param instruments As `Market`'s constructor takes them.
   * @throws std::invalid_argument as `Market`'s constructor does, before the file is touched.
   */
  MarketFile(
    std::string path, std::vector<BarSeries> bars, MarketSettings settings, std::vector<Instrument> instruments = {}
  )
  : market_(std::move(bars), std::move(settings), std::move(instruments)), path_(std::move(path)) {
    open_file();
    if (!opened_) {
      // A refused file is not held: its lock would refuse this program's own retry.
      handle_.reset();
    }
    market_.recorder_.record_ = [this](const Market::Change& change) {
      return record(change);
    };
  }

  /** @brief Closes the file, which releases its lock. */
  ~MarketFile() = default;

  MarketFile(const MarketFile&) = delete;
  MarketFile& operator=(const MarketFile&) = delete;

  /**
   * @brief Tests whether the file opened and replayed.
   * @return `true` if it did; changes are then recorded until one fails (`failed()`).
   */
  bool open() const noexcept {
    return opened_;
  }

  /**
   * @brief Tests whether a line could not be written or flushed since the file opened.
   * @return `true` if one could not; the market then refuses changes until the file is opened again.
   */
  bool failed() const noexcept {
    return failed_;
  }

  /**
   * @brief Returns why the file did not open.
   * @return The reason, in fixed words, or empty if it opened.
   */
  const std::string& rejection() const noexcept {
    return rejection_;
  }

  /**
   * @brief Returns the file's path.
   * @return The path.
   */
  const std::string& path() const noexcept {
    return path_;
  }

  /**
   * @brief Returns the kept market, for `MockClient` and `MarketClock`.
   * @return The market; copy it to try changes that are not recorded. Assigning to it or moving from it throws
   *   `std::logic_error`.
   */
  Market& market() noexcept {
    return market_;
  }

  /**
   * @brief Returns the kept market.
   * @return The market.
   */
  const Market& market() const noexcept {
    return market_;
  }

private:
  using Clock = std::chrono::system_clock;

  // A 64-bit FNV-1a digest, fed fixed-width integers and length-prefixed text, so two different inputs never feed it the
  // same bytes.
  class Digest {
  public:
    void add(std::int64_t value) {
      for (int at = 0; at < 64; at += 8) {
        state_ = (state_ ^ ((static_cast<std::uint64_t>(value) >> at) & 0xffU)) * 0x100000001b3ULL;
      }
    }

    void add(std::string_view text) {
      add(static_cast<std::int64_t>(text.size()));
      for (const char ch : text) {
        state_ = (state_ ^ static_cast<unsigned char>(ch)) * 0x100000001b3ULL;
      }
    }

    void add(const Decimal64& value) {
      add(value.coefficient());
      add(static_cast<std::int64_t>(value.scale()));
    }

    void add(const Date& date) {
      add(static_cast<std::int64_t>(date.year_));
      add(static_cast<std::int64_t>(date.month_));
      add(static_cast<std::int64_t>(date.day_));
    }

    void add(Clock::time_point at) {
      add(ticks(at));
    }

    void add(const InstrumentId& id) {
      add(static_cast<std::int64_t>(id.country_));
      add(static_cast<std::int64_t>(id.venue_));
      add(id.symbol_);
    }

    void add(const VenueKey& venue) {
      add(static_cast<std::int64_t>(venue.kind()));
      add(static_cast<std::int64_t>(venue.venue()));
      add(venue.raw_code().str());
    }

    void add(const StatedDate& date) {
      add(static_cast<std::int64_t>(date.state()));
      if (date.state() == StatedDate::State::STATED) {
        add(date.date());
      }
    }

    template <typename T>
    void add(const std::optional<T>& value) {
      add(static_cast<std::int64_t>(value.has_value()));
      if (value) {
        add(*value);
      }
    }

    std::string hex() const {
      std::string text(16, '0');
      for (std::size_t at = 0; at < 16; ++at) {
        text[15 - at] = "0123456789abcdef"[(state_ >> (4 * at)) & 0xfU];
      }
      return text;
    }

  private:
    std::uint64_t state_{0xcbf29ce484222325ULL};
  };

  static std::int64_t ticks(Clock::time_point at) {
    return static_cast<std::int64_t>(at.time_since_epoch().count());
  }

  static Clock::time_point instant(std::int64_t count) {
    return Clock::time_point(Clock::duration(count));
  }

  // The digest of everything a replay depends on; see the class.
  std::string fingerprint() const {
    Digest digest;
    digest.add(SEMANTICS);
    // The clock's unit, and the size of each enum a line names by number: a file is not read across them.
    digest.add(static_cast<std::int64_t>(Clock::period::num));
    digest.add(static_cast<std::int64_t>(Clock::period::den));
    for (
      const auto size :
      {all_countries.size(), all_venues.size(), all_order_sides.size(), all_order_types.size(),
        all_times_in_force.size(), all_order_sessions.size()}
    ) {
      digest.add(static_cast<std::int64_t>(size));
    }
    const auto& settings = market_.settings_;
    digest.add(settings.cash_.amount_);
    digest.add(settings.cash_.currency_.str());
    digest.add(settings.start_);
    const auto& fills = settings.fills_;
    digest.add(static_cast<std::int64_t>(*fills.library_market_price()));
    digest.add(static_cast<std::int64_t>(fills.limit_price()));
    digest.add(fills.slippage().basis_points_);
    digest.add(static_cast<std::int64_t>(fills.slippage().ticks_));
    digest.add(fills.sell_tax() ? std::optional<Decimal64>(fills.sell_tax()->rate_) : std::nullopt);
    digest.add(static_cast<std::int64_t>(fills.sell_tax() ? fills.sell_tax()->country_ : Country::KR));
    const auto& commission = fills.commission();
    digest.add(commission.stated_model().rate_);
    digest.add(commission.stated_model().minimum_);
    digest.add(commission.asserted_venue());
    digest.add(static_cast<std::int64_t>(commission.dating_market()));
    const auto& schedule = commission.schedule();
    // Zero for a stated policy, else one more than the schedule's rows.
    digest.add(static_cast<std::int64_t>(schedule ? schedule->rates_.size() + 1 : 0));
    for (std::size_t row = 0; schedule && row < schedule->rates_.size(); ++row) {
      const auto& rate = schedule->rates_[row];
      digest.add(static_cast<std::int64_t>(rate.country_));
      digest.add(rate.other_.str());
      digest.add(rate.venue_);
      digest.add(rate.rate_);
      digest.add(rate.institution_rate_);
      digest.add(rate.from_);
      digest.add(rate.until_);
      digest.add(static_cast<std::int64_t>(rate.from_unreadable_));
      digest.add(static_cast<std::int64_t>(rate.until_unreadable_));
    }
    digest.add(static_cast<std::int64_t>(settings.session_.open_.count()));
    digest.add(static_cast<std::int64_t>(settings.session_.close_.count()));
    digest.add(static_cast<std::int64_t>(settings.session_.utc_offset_.count()));
    const auto& calendar = market_.calendar_;
    digest.add(static_cast<std::int64_t>(calendar.country_));
    digest.add(static_cast<std::int64_t>(calendar.unreadable_days_));
    digest.add(static_cast<std::int64_t>(calendar.days_.size()));
    for (const auto& day : calendar.days_) {
      digest.add(day.date_);
      digest.add(static_cast<std::int64_t>(day.open_));
    }
    digest.add(static_cast<std::int64_t>(market_.listings_.size()));
    for (const auto& listing : market_.listings_) {
      const auto& series = listing.series_;
      digest.add(series.instrument_);
      digest.add(static_cast<std::int64_t>(series.interval_));
      digest.add(static_cast<std::int64_t>(series.stamp_));
      digest.add(series.currency_.str());
      digest.add(static_cast<std::int64_t>(series.adjusted_));
      digest.add(static_cast<std::int64_t>(series.bars_.size()));
      for (const auto& bar : series.bars_) {
        digest.add(bar.timestamp_);
        digest.add(bar.open_);
        digest.add(bar.high_);
        digest.add(bar.low_);
        digest.add(bar.close_);
        digest.add(bar.volume_);
        digest.add(bar.traded_value_);
      }
      const auto& reference = listing.reference_;
      digest.add(reference.id_);
      digest.add(reference.name_);
      digest.add(reference.english_name_);
      digest.add(reference.isin_);
      digest.add(static_cast<std::int64_t>(reference.segment_.known_));
      digest.add(reference.segment_.other_.str());
      digest.add(static_cast<std::int64_t>(reference.type_));
      digest.add(static_cast<std::int64_t>(reference.status_));
      digest.add(reference.currency_.str());
      digest.add(static_cast<std::int64_t>(reference.common_share_));
      digest.add(reference.listed_on_);
      digest.add(reference.delisted_on_);
      digest.add(reference.shares_outstanding_);
      digest.add(reference.lot_size_);
      digest.add(static_cast<std::int64_t>(reference.price_rule_.kind_));
      digest.add(reference.price_rule_.source_);
      digest.add(static_cast<std::int64_t>(reference.price_rule_.bands_.size()));
      for (const auto& band : reference.price_rule_.bands_) {
        digest.add(band.from_);
        digest.add(band.tick_);
      }
    }
    return digest.hex();
  }

  // The first line, newline included.
  std::string header_line() const {
    boost::json::object header;
    header["mock_market"] = FORMAT;
    header["semantics"] = SEMANTICS;
    header["fingerprint"] = fingerprint();
    return boost::json::serialize(header) + "\n";
  }

  // Closes a stream; never called for the null a failed open leaves behind.
  static void closer(std::FILE* handle) noexcept {
    std::fclose(handle);
  }

  // Opens, locks, checks and replays the file; on any refusal says why in `rejection_`. Only the repair of an
  // incomplete last line or header changes the file before a refusal.
  void open_file() {
    if (!market_.settings_.fills_.library_market_price()) {
      rejection_ = "a mock market whose market orders a caller's own function prices cannot be kept in a file";
      return;
    }
#if defined(_WIN32)
    handle_.reset(std::fopen(path_.c_str(), "a+b"));
#else
    const int descriptor = ::open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (descriptor < 0) {
      rejection_ = "the mock market's state file could not be opened";
      return;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      // Closed before the reason is stored, which may allocate and throw.
      const bool held_elsewhere = errno == EWOULDBLOCK;
      ::close(descriptor);
      rejection_ = held_elsewhere ?
        "the mock market's state file is open in another market (one at a time)" :
        "the mock market's state file could not be locked";
      return;
    }
    handle_.reset(::fdopen(descriptor, "a+b"));
    if (handle_ == nullptr) {
      ::close(descriptor);
    }
#endif
    if (handle_ == nullptr) {
      rejection_ = "the mock market's state file could not be opened";
      return;
    }
    // Unbuffered: a failed write leaves nothing behind for a later flush to complete.
    std::setvbuf(handle_.get(), nullptr, _IONBF, 0);
    std::string text;
    char buffer[65536];
    std::rewind(handle_.get());
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), handle_.get())) {
      text.append(buffer, count);
    }
    if (std::ferror(handle_.get()) != 0 || std::fseek(handle_.get(), 0, SEEK_END) != 0) {
      rejection_ = "the mock market's state file could not be read";
      return;
    }
    const auto header = header_line();
    const auto last = text.rfind('\n');
    if (last == std::string::npos) {
      // Empty, or a header cut short, before any change was made.
      if (header.compare(0, text.size(), text) != 0) {
        rejection_ = "the file is not a mock market's state file";
      } else if (!cut(0) || !write_whole(header) || !sync() || !sync_folder()) {
        rejection_ = "the mock market's state file could not be started";
      } else {
        opened_ = true;
      }
      return;
    }
    const auto first = text.find('\n');
    if (!check_header(std::string_view(text).substr(0, first + 1), header)) {
      return;
    }
    for (auto begin = first + 1; begin <= last;) {
      const auto end = text.find('\n', begin);
      if (!replay(std::string_view(text).substr(begin, end - begin))) {
        rejection_ = "the mock market's state file does not replay: a line is damaged or out of order";
        return;
      }
      begin = end + 1;
    }
    if (last + 1 < text.size() && !cut(last + 1)) {
      rejection_ = "the mock market's state file ends in a line cut short, which could not be removed";
      return;
    }
    // An earlier process may have stopped before either flush: what was replayed must be on the disk before it stands.
    if (!sync() || !sync_folder()) {
      rejection_ = "the mock market's state file could not be flushed to disk";
      return;
    }
    opened_ = true;
  }

  // Tests the first line against this market's; says why in `rejection_` when it differs.
  bool check_header(std::string_view line, const std::string& expected) {
    if (line == expected) {
      return true;
    }
    boost::system::error_code error;
    const auto value = boost::json::parse(line, error);
    const auto* fields = error ? nullptr : value.if_object();
    const auto format = fields ? integer(*fields, "mock_market") : std::nullopt;
    if (!format) {
      rejection_ = "the file is not a mock market's state file";
    } else if (*format != FORMAT) {
      rejection_ = "the mock market's state file is of another format";
    } else if (integer(*fields, "semantics") != SEMANTICS) {
      rejection_ = "the mock market's state file was written under other market rules";
    } else {
      rejection_ =
        "the mock market's state file belongs to another market: its bars, settings or reference data differ";
    }
    return false;
  }

  // Shortens the file to `size` bytes and flushes it.
  bool cut(std::size_t size) {
#if defined(_WIN32)
    const bool shortened = ::_chsize_s(::_fileno(handle_.get()), static_cast<long long>(size)) == 0;
#else
    const bool shortened = ::ftruncate(::fileno(handle_.get()), static_cast<off_t>(size)) == 0;
#endif
    return shortened && std::fseek(handle_.get(), 0, SEEK_END) == 0 && sync();
  }

  // Writes `line` at the end of the file; `false` unless every byte was.
  bool write_whole(const std::string& line) {
    return std::fwrite(line.data(), 1, line.size(), handle_.get()) == line.size() && std::fflush(handle_.get()) == 0;
  }

  // Asks the system to put the file's bytes on the disk.
  bool sync() {
#if defined(_WIN32)
    return ::_commit(::_fileno(handle_.get())) == 0;
#else
    return ::fsync(::fileno(handle_.get())) == 0;
#endif
  }

  // Puts a new file's name on the disk, so a crash cannot lose the file; Windows has no such call.
  bool sync_folder() const {
#if defined(_WIN32)
    return true;
#else
    const auto slash = path_.rfind('/');
    const auto folder = slash == std::string::npos ? std::string(".") : path_.substr(0, slash == 0 ? 1 : slash);
    const int descriptor = ::open(folder.c_str(), O_RDONLY | O_CLOEXEC);
    const bool synced = descriptor >= 0 && ::fsync(descriptor) == 0;
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    return synced;
#endif
  }

  // Writes and flushes the line for a change before the market makes it; what became of a line that failed.
  std::optional<MarketFileError::Kind> record(const Market::Change& change) {
    if (!opened_ || failed_) {
      return MarketFileError::Kind::NOT_WRITTEN;
    }
    boost::json::object entry;
    entry["at"] = ticks(market_.now());
    if (change.order_ != nullptr) {
      const auto& order = *change.order_;
      boost::json::object fields;
      fields["symbol"] = order.instrument_.symbol_;
      fields["country"] = static_cast<std::int64_t>(order.instrument_.country_);
      fields["venue"] = static_cast<std::int64_t>(order.instrument_.venue_);
      fields["side"] = static_cast<std::int64_t>(order.side_);
      fields["type"] = static_cast<std::int64_t>(order.type_);
      fields["time_in_force"] = static_cast<std::int64_t>(order.time_in_force_);
      fields["session"] = static_cast<std::int64_t>(order.session_);
      fields["route"] = static_cast<std::int64_t>(order.venue_);
      fields["quantity"] = decimal(order.quantity_);
      if (order.price_) {
        fields["price"] = decimal(*order.price_);
      }
      entry["place"] = std::move(fields);
    } else if (change.order_id_ != nullptr) {
      entry["cancel"] = *change.order_id_;
    } else {
      entry["advance"] = ticks(*change.to_);
    }
    if (!write_whole(boost::json::serialize(entry) + "\n")) {
      failed_ = true;
      return MarketFileError::Kind::NOT_WRITTEN;
    }
    if (!sync()) {
      failed_ = true;
      return MarketFileError::Kind::UNCONFIRMED;
    }
    return std::nullopt;
  }

  // Makes the change a line records; `false` if the line is damaged or the market does not take it as it did.
  bool replay(std::string_view line) {
    boost::system::error_code error;
    const auto value = boost::json::parse(line, error);
    const auto* entry = error ? nullptr : value.if_object();
    if (entry == nullptr || integer(*entry, "at") != ticks(market_.now())) {
      return false;
    }
    if (const auto* place = entry->if_contains("place")) {
      const auto order = order_from(*place);
      return order && market_.place(*order).has_value();
    }
    if (const auto* cancel = entry->if_contains("cancel")) {
      return cancel->is_string() && market_.cancel(std::string(cancel->as_string())).has_value();
    }
    const auto to = integer(*entry, "advance");
    if (!to || *to <= ticks(market_.now())) {
      return false;
    }
    try {
      market_.advance_to(instant(*to));
    } catch (const std::runtime_error&) {
      // The market halted on this move, as it did when the move was recorded.
      return market_.halted().has_value();
    }
    return true;
  }

  // Reads a placement's fields; nothing if any is missing or out of range.
  static std::optional<OrderRequest> order_from(const boost::json::value& value) {
    const auto* fields = value.if_object();
    const auto* symbol = fields ? fields->if_contains("symbol") : nullptr;
    if (symbol == nullptr || !symbol->is_string()) {
      return std::nullopt;
    }
    const auto country = enumerated<Country>(*fields, "country", all_countries.size());
    const auto venue = enumerated<Venue>(*fields, "venue", all_venues.size());
    const auto side = enumerated<OrderSide>(*fields, "side", all_order_sides.size());
    const auto type = enumerated<OrderType>(*fields, "type", all_order_types.size());
    const auto time_in_force = enumerated<TimeInForce>(*fields, "time_in_force", all_times_in_force.size());
    const auto session = enumerated<OrderSession>(*fields, "session", all_order_sessions.size());
    const auto route = enumerated<Venue>(*fields, "route", all_venues.size());
    const auto quantity = decimal_from(fields->if_contains("quantity"));
    const auto* price = fields->if_contains("price");
    const auto limit = decimal_from(price);
    if (
      !country || !venue || !side || !type || !time_in_force || !session || !route || !quantity || (price && !limit)
    ) {
      return std::nullopt;
    }
    OrderRequest order;
    order.instrument_ = InstrumentId{*country, *venue, std::string(symbol->as_string())};
    order.side_ = *side;
    order.type_ = *type;
    order.time_in_force_ = *time_in_force;
    order.session_ = *session;
    order.venue_ = *route;
    order.quantity_ = *quantity;
    order.price_ = limit;
    return order;
  }

  static std::optional<std::int64_t> integer(const boost::json::object& fields, std::string_view key) {
    const auto* value = fields.if_contains(key);
    return value != nullptr && value->is_int64() ? std::optional<std::int64_t>(value->as_int64()) : std::nullopt;
  }

  template <typename Enum>
  static std::optional<Enum> enumerated(const boost::json::object& fields, std::string_view key, std::size_t size) {
    const auto value = integer(fields, key);
    if (!value || *value < 0 || static_cast<std::size_t>(*value) >= size) {
      return std::nullopt;
    }
    return static_cast<Enum>(*value);
  }

  // A decimal as its coefficient and scale, which keep the scale as given.
  static boost::json::array decimal(const Decimal64& value) {
    return boost::json::array{value.coefficient(), static_cast<std::int64_t>(value.scale())};
  }

  // Reads a decimal `decimal` wrote; nothing if it is not one.
  static std::optional<Decimal64> decimal_from(const boost::json::value* value) {
    const auto* pair = value != nullptr ? value->if_array() : nullptr;
    if (pair == nullptr || pair->size() != 2 || !(*pair)[0].is_int64() || !(*pair)[1].is_int64()) {
      return std::nullopt;
    }
    const auto scale = (*pair)[1].as_int64();
    if (scale < 0 || scale > 255) {
      return std::nullopt;
    }
    return Decimal64((*pair)[0].as_int64(), static_cast<std::uint8_t>(scale));
  }

  Market market_;
  std::string path_;
  // The open file, closed with the market or when its opening throws: closing releases its lock.
  std::unique_ptr<std::FILE, void (*)(std::FILE*)> handle_{nullptr, &closer};
  std::string rejection_;
  bool opened_{false};
  bool failed_{false};
};

} // namespace mock
} // namespace algotradelib
