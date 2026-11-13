#pragma once

#include "../core/calendar.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace algotradelib {
namespace network {

/**
 * @brief Where blocking broker clients share a market's trading calendar, across threads and processes.
 *
 * Give one to `KisBlockingClient`: KIS asks that its holiday inquiry be made at most once a day, and one inquiry takes up to 20 pages.
 * The client reuses a stored calendar received on the same day in Seoul; see `KisBlockingClient::calendar`.
 *
 * Each function returns storage failures, rather than throwing them, as `calendar_cache_error` (an allocation can still throw).
 * Stored contents that cannot be read as a calendar are no calendar, not an error.
 * `received_at_` is stored and returned as given.
 * An implementation must allow calls from several threads at once.
 */
class CalendarCache {
public:
  virtual ~CalendarCache() = default;

  /**
   * @brief Reads the calendar stored under a name.
   * @param name The name, such as `kis-kr`.
   * @return The calendar, nothing if none is stored or it cannot be read, or the cache's error.
   */
  virtual Result<std::optional<TradingCalendar>> load(const std::string& name) = 0;

  /**
   * @brief Stores a calendar under a name, replacing what was stored.
   * @param name The name.
   * @param calendar The calendar.
   * @return The cache's error, or nothing.
   */
  virtual std::optional<BrokerError> publish(const std::string& name, const TradingCalendar& calendar) = 0;
};

/**
 * @brief Returns a calendar cache's error: `TRANSPORT`, not sent to the broker, code `calendar_cache`.
 * @param reason Fixed text.
 * @return The error.
 */
inline BrokerError calendar_cache_error(std::string_view reason) {
  BrokerError error;
  error.category_ = ErrorCategory::TRANSPORT;
  error.reached_broker_ = false;
  error.broker_code_ = "calendar_cache";
  error.message_ = std::string(reason);
  return error;
}

} // namespace network
} // namespace algotradelib

#if !defined(_WIN32)

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace algotradelib {
namespace network {

/**
 * @brief A `CalendarCache` in a private folder: one file per name. POSIX only; not provided on Windows.
 *
 * The folder is created (mode `0700`) if it does not exist; its parent must.
 * For name `N` the cache keeps `N.calendar`: JSON with the format (`1`), the country, `received_at_ns` (nanoseconds since the epoch), `unreadable_days` and the days as `[YYYYMMDD, open]` pairs in their order.
 * The folder and the file must be owned by the user running the program and give no permission to group or others;
 * a cache that finds otherwise returns its error and leaves them as they were.
 * A symbolic link at the folder's path or at the file's is not followed, and descriptors are closed on `exec`.
 * A file over 256 KiB, or one that is not well formed, reads as no calendar.
 *
 * Writers take no lock: each writes a temporary file of its own, `N.calendar.<process>-<count>.tmp`, flushes it and renames it over `N.calendar`, so a reader sees a whole file and the last writer's calendar stays.
 * A writer stopped before its rename can leave its temporary file; it is never read and may be removed.
 */
class FileCalendarCache : public CalendarCache {
public:
  /**
   * @brief Constructs a cache; nothing is created or opened until the first call.
   * @param folder The folder.
   */
  explicit FileCalendarCache(std::string folder) : folder_(std::move(folder)) {}

  /**
   * @brief Reads the name's calendar file.
   * @param name The name.
   * @return The calendar; nothing if there is no file or it is not well formed; or the cache's error.
   */
  Result<std::optional<TradingCalendar>> load(const std::string& name) override {
    Descriptor folder;
    if (auto error = open_folder(name, folder)) {
      return *error;
    }
    Descriptor file;
    if (auto error = open_file(folder, name, file)) {
      return *error;
    }
    if (file.get() < 0) {
      return std::optional<TradingCalendar>();
    }
    std::string text;
    char buffer[4096];
    while (text.size() <= MAXIMUM_FILE) {
      const auto count = ::read(file.get(), buffer, sizeof(buffer));
      if (count == 0) {
        return parse(text);
      }
      if (count < 0 && errno != EINTR) {
        return calendar_cache_error("calendar cache: the file could not be read");
      }
      if (count > 0) {
        text.append(buffer, static_cast<std::size_t>(count));
      }
    }
    return std::optional<TradingCalendar>();
  }

  /**
   * @brief Replaces the name's calendar file.
   * @param name The name.
   * @param calendar The calendar.
   * @return The cache's error, or nothing; the old file is left as it was on failure.
   */
  std::optional<BrokerError> publish(const std::string& name, const TradingCalendar& calendar) override {
    Descriptor folder;
    if (auto error = open_folder(name, folder)) {
      return error;
    }
    Descriptor existing;
    if (auto error = open_file(folder, name, existing)) {
      return error;
    }
    auto temporary = write_temporary(folder, name, serialize(calendar));
    if (!temporary) {
      return std::move(temporary.error());
    }
    return rename_temporary(folder, name, temporary.value());
  }

private:
  friend struct FileCalendarCacheTestAccess;

  // Opened without following a link, closed on exec, and never blocking on a special file.
  static constexpr int FILE_FLAGS = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  // Larger calendar files are not well formed.
  static constexpr std::size_t MAXIMUM_FILE = 256 * 1024;

  // Owns a descriptor; negative for none.
  class Descriptor {
  public:
    explicit Descriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}

    ~Descriptor() {
      if (descriptor_ >= 0) {
        ::close(descriptor_);
      }
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    int get() const noexcept {
      return descriptor_;
    }

    void reset(int descriptor) noexcept {
      if (descriptor_ >= 0) {
        ::close(descriptor_);
      }
      descriptor_ = descriptor;
    }

  private:
    int descriptor_;
  };

  // Tests what the cache requires of everything it opens: the type, the user's own, nothing for group or others.
  static bool owned_privately(int descriptor, mode_t type) noexcept {
    struct stat status{};
    return ::fstat(descriptor, &status) == 0 && (status.st_mode & S_IFMT) == type && status.st_uid == ::geteuid() &&
      (status.st_mode & 077) == 0;
  }

  // Checks the name can name a file (letters, digits, `-` and `_`, at most 200), then creates the folder if it does not exist and opens it, checked.
  std::optional<BrokerError> open_folder(const std::string& name, Descriptor& folder) const {
    bool safe = !name.empty() && name.size() <= 200;
    for (const char ch : name) {
      safe = safe &&
        ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_');
    }
    if (!safe) {
      return calendar_cache_error("calendar cache: the name cannot name a file");
    }
    if (::mkdir(folder_.c_str(), 0700) != 0 && errno != EEXIST) {
      return calendar_cache_error("calendar cache: the folder could not be created");
    }
    folder.reset(::open(folder_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (folder.get() < 0) {
      struct stat status{};
      const bool link = ::lstat(folder_.c_str(), &status) == 0 && S_ISLNK(status.st_mode);
      return calendar_cache_error(
        link ? "calendar cache: the folder is a symbolic link" : "calendar cache: the folder could not be opened"
      );
    }
    if (!owned_privately(folder.get(), S_IFDIR)) {
      return calendar_cache_error("calendar cache: the folder is not private (owner or permissions)");
    }
    return std::nullopt;
  }

  // Opens the name's calendar file, checked; leaves `file` closed when there is none.
  static std::optional<BrokerError> open_file(const Descriptor& folder, const std::string& name, Descriptor& file) {
    file.reset(::openat(folder.get(), (name + ".calendar").c_str(), FILE_FLAGS | O_RDONLY));
    if (file.get() < 0 && errno == ENOENT) {
      return std::nullopt;
    }
    if (file.get() < 0) {
      return calendar_cache_error(
        errno == ELOOP ? "calendar cache: the file is a symbolic link" : "calendar cache: the file could not be opened"
      );
    }
    if (!owned_privately(file.get(), S_IFREG)) {
      return calendar_cache_error("calendar cache: the file is not private (owner, permissions or type)");
    }
    return std::nullopt;
  }

  // Returns a country's ISO 3166 code, as the file stores it.
  static const char* country_code(Country country) noexcept {
    switch (country) {
      case Country::KR:
        return "KR";
      case Country::US:
        return "US";
    }
    return "";
  }

  // Writes the calendar's file text.
  static std::string serialize(const TradingCalendar& calendar) {
    boost::json::array days;
    days.reserve(calendar.days_.size());
    for (const auto& day : calendar.days_) {
      const std::int64_t date =
        std::int64_t{day.date_.year_} * 10000 + std::int64_t{day.date_.month_} * 100 + std::int64_t{day.date_.day_};
      days.push_back(boost::json::array{date, day.open_});
    }
    boost::json::object document;
    document["format"] = 1;
    document["country"] = country_code(calendar.country_);
    document["received_at_ns"] =
      std::chrono::duration_cast<std::chrono::nanoseconds>(calendar.received_at_.time_since_epoch()).count();
    document["unreadable_days"] = calendar.unreadable_days_;
    document["days"] = std::move(days);
    return boost::json::serialize(document);
  }

  // Reads a calendar file's text; nothing unless every field is present, of its type and in its range.
  static std::optional<TradingCalendar> parse(const std::string& text) {
    boost::system::error_code ec;
    const auto document = boost::json::parse(text, ec);
    if (ec || !document.is_object()) {
      return std::nullopt;
    }
    const auto& object = document.as_object();
    const auto* format = object.if_contains("format");
    const auto* country = object.if_contains("country");
    const auto* received = object.if_contains("received_at_ns");
    const auto* unreadable = object.if_contains("unreadable_days");
    const auto* days = object.if_contains("days");
    if (
      !format || !format->is_int64() || format->as_int64() != 1 || !country || !country->is_string() || !received ||
      !received->is_int64() || !unreadable || !unreadable->is_int64() || !days || !days->is_array()
    ) {
      return std::nullopt;
    }
    TradingCalendar calendar;
    const auto named = std::find_if(all_countries.begin(), all_countries.end(), [country](Country candidate) {
      return country->as_string() == country_code(candidate);
    });
    if (named == all_countries.end()) {
      return std::nullopt;
    }
    calendar.country_ = *named;
    // A day of margin either side keeps the Seoul date of any accepted instant within the clock's range.
    const auto since_epoch = std::chrono::nanoseconds(received->as_int64());
    const auto margin = std::chrono::hours(24);
    if (
      since_epoch < std::chrono::nanoseconds::min() + margin || std::chrono::nanoseconds::max() - margin < since_epoch
    ) {
      return std::nullopt;
    }
    calendar.received_at_ = std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(since_epoch)
    );
    if (unreadable->as_int64() < 0 || static_cast<std::uint64_t>(unreadable->as_int64()) > SIZE_MAX) {
      return std::nullopt;
    }
    calendar.unreadable_days_ = static_cast<std::size_t>(unreadable->as_int64());
    calendar.days_.reserve(days->as_array().size());
    for (const auto& row : days->as_array()) {
      if (
        !row.is_array() || row.as_array().size() != 2 || !row.as_array()[0].is_int64() || !row.as_array()[1].is_bool()
      ) {
        return std::nullopt;
      }
      const auto number = row.as_array()[0].as_int64();
      if (number < 10101 || number > 99991231) {
        return std::nullopt;
      }
      const Date date{
        static_cast<int>(number / 10000),
        static_cast<unsigned>(number / 100 % 100),
        static_cast<unsigned>(number % 100),
      };
      if (!is_valid_date(date)) {
        return std::nullopt;
      }
      calendar.days_.push_back(TradingDay{date, row.as_array()[1].as_bool()});
    }
    return calendar;
  }

  // Creates a temporary file of the caller's own (a name taken by another is not opened), writes `text` to it and flushes it; returns its name.
  static Result<std::string> write_temporary(
    const Descriptor& folder, const std::string& name, const std::string& text
  ) {
    static std::atomic<unsigned long> counter{0};
    std::string temporary;
    Descriptor file;
    for (int attempt = 0; attempt < 100 && file.get() < 0; ++attempt) {
      temporary = name + ".calendar." + std::to_string(::getpid()) + '-' + std::to_string(counter++) + ".tmp";
      file.reset(::openat(folder.get(), temporary.c_str(), FILE_FLAGS | O_WRONLY | O_CREAT | O_EXCL, 0600));
      if (file.get() < 0 && errno != EEXIST) {
        break;
      }
    }
    if (file.get() < 0) {
      return calendar_cache_error("calendar cache: a temporary file could not be created");
    }
    bool written = true;
    for (std::size_t offset = 0; written && offset < text.size();) {
      const auto count = ::write(file.get(), text.data() + offset, text.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
      } else if (count == 0 || errno != EINTR) {
        written = false;
      }
    }
    if (!written || ::fsync(file.get()) != 0) {
      ::unlinkat(folder.get(), temporary.c_str(), 0);
      return calendar_cache_error("calendar cache: the calendar file could not be written");
    }
    return temporary;
  }

  // Renames the caller's own temporary file over the calendar file; removes it if the rename fails.
  static std::optional<BrokerError> rename_temporary(
    const Descriptor& folder, const std::string& name, const std::string& temporary
  ) {
    if (::renameat(folder.get(), temporary.c_str(), folder.get(), (name + ".calendar").c_str()) != 0) {
      ::unlinkat(folder.get(), temporary.c_str(), 0);
      return calendar_cache_error("calendar cache: the calendar file could not be written");
    }
    return std::nullopt;
  }

  std::string folder_;
};

} // namespace network
} // namespace algotradelib

#endif
