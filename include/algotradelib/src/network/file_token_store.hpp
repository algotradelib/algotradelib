#pragma once

#include "./token_store.hpp"

#if !defined(_WIN32)

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace algotradelib {
namespace network {

/**
 * @brief A `TokenStore` in a private folder: one token file and one lock file per identity. POSIX only; not
 *   provided on Windows.
 *
 * The folder is created (mode `0700`) if it does not exist; its parent must. For identity `ID` the store
 * keeps `ID.token` (JSON: the token and its expiry in nanoseconds since the epoch, replaced by writing
 * `ID.tmp`, flushing it and renaming it) and `ID.lock` (an `flock` lock, never removed). The folder and every
 * file must be owned by the user running the program and give no permission to group or others; a store
 * that finds otherwise refuses and leaves the file as it was. A symbolic link at the folder's path or at a
 * file's is not followed. Descriptors are closed on `exec`. A token file that is not well formed reads as no
 * token.
 *
 * Keep the folder on a local disk: `flock` over a network file system is unreliable. A lock held when the
 * process forks without `exec` is held by the child too, until the child closes the descriptor or exits.
 * The token is a bearer credential; the folder is as sensitive as the app secret.
 */
class FileTokenStore : public TokenStore {
public:
  /**
   * @brief Constructs a store; nothing is created or opened until the first call.
   * @param folder The folder.
   * @param lock_wait How long `lock` waits for another holder before it fails.
   */
  explicit FileTokenStore(std::string folder, std::chrono::milliseconds lock_wait = std::chrono::seconds(30))
  : folder_(std::move(folder)), lock_wait_(lock_wait) {}

  /**
   * @brief Takes the identity's lock, opening its lock file anew, so two callers in one process exclude each
   *   other as two processes do.
   * @param identity The identity.
   * @return The held lock, or the store's error (`lock not obtained in time` after the wait).
   */
  Result<std::unique_ptr<Lock>> lock(const std::string& identity) override {
    Descriptor folder;
    if (auto error = open_folder(identity, folder)) {
      return *error;
    }
    const auto name = identity + ".lock";
    Descriptor file(::openat(folder.get(), name.c_str(), FILE_FLAGS | O_RDWR | O_CREAT, 0600));
    if (file.get() < 0 && errno == ENOENT) {
      // A concurrent create that answers no-such-file is retried once.
      file.reset(::openat(folder.get(), name.c_str(), FILE_FLAGS | O_RDWR | O_CREAT, 0600));
    }
    if (auto error = check_file(file)) {
      return *error;
    }
    const auto deadline = std::chrono::steady_clock::now() + lock_wait_;
    while (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EINTR) {
        return token_store_error("token store: the lock file could not be locked");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return token_store_error("token store: lock not obtained in time");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::unique_ptr<Lock> held = std::make_unique<FileLock>(std::move(file));
    return Result<std::unique_ptr<Lock>>(std::move(held));
  }

  /**
   * @brief Reads the identity's token file.
   * @param identity The identity.
   * @return The token; nothing if there is no file or it is not well formed; or the store's error.
   */
  Result<std::optional<StoredToken>> load(const std::string& identity) override {
    Descriptor folder;
    if (auto error = open_folder(identity, folder)) {
      return *error;
    }
    std::optional<std::string> text;
    if (auto error = read_token(folder, identity, text)) {
      return *error;
    }
    return text ? parse(*text) : std::optional<StoredToken>();
  }

  /**
   * @brief Replaces the identity's token file, unless the stored token expires later.
   * @param identity The identity.
   * @param token The token.
   * @return The store's error, or nothing; the old file is left as it was on failure.
   */
  std::optional<BrokerError> publish(const std::string& identity, const StoredToken& token) override {
    Descriptor folder;
    if (auto error = open_folder(identity, folder)) {
      return error;
    }
    std::optional<std::string> text;
    if (auto error = read_token(folder, identity, text)) {
      return error;
    }
    if (text) {
      if (const auto stored = parse(*text); stored && stored->expires_at_ > token.expires_at_) {
        return std::nullopt;
      }
    }
    return write_token(folder, identity, token);
  }

  /**
   * @brief Removes the identity's token file if it holds the given token.
   * @param identity The identity.
   * @param access_token The token.
   * @return The store's error, or nothing.
   */
  std::optional<BrokerError> discard(const std::string& identity, const std::string& access_token) override {
    Descriptor folder;
    if (auto error = open_folder(identity, folder)) {
      return error;
    }
    std::optional<std::string> text;
    if (auto error = read_token(folder, identity, text)) {
      return error;
    }
    if (!text) {
      return std::nullopt;
    }
    if (const auto stored = parse(*text); !stored || stored->access_token_ != access_token) {
      return std::nullopt;
    }
    return unlink_token(folder, identity);
  }

  /**
   * @brief Removes the identity's token file; its lock file stays.
   * @param identity The identity.
   * @return The store's error, or nothing.
   */
  std::optional<BrokerError> remove(const std::string& identity) override {
    Descriptor folder;
    if (auto error = open_folder(identity, folder)) {
      return error;
    }
    return unlink_token(folder, identity);
  }

private:
  // Opened without following a link, closed on exec, and never blocking on a special file.
  static constexpr int FILE_FLAGS = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  // Larger token files are not well formed.
  static constexpr std::size_t MAXIMUM_TOKEN_FILE = 64 * 1024;

  // Owns a descriptor; negative for none.
  class Descriptor {
  public:
    explicit Descriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}

    ~Descriptor() {
      if (descriptor_ >= 0) {
        ::close(descriptor_);
      }
    }

    Descriptor(Descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    Descriptor& operator=(Descriptor&&) = delete;
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

  // The lock: closing its own open of the lock file releases it.
  class FileLock : public Lock {
  public:
    explicit FileLock(Descriptor file) noexcept : file_(std::move(file)) {}

  private:
    Descriptor file_;
  };

  // Tests whether an identity can name a file: letters, digits, `-` and `_`, at most 200.
  static bool file_safe(const std::string& identity) noexcept {
    if (identity.empty() || identity.size() > 200) {
      return false;
    }
    for (const char ch : identity) {
      const bool allowed =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
      if (!allowed) {
        return false;
      }
    }
    return true;
  }

  // Tests what the store requires of everything it opens: the type, the user's own, nothing for group or others.
  static bool owned_privately(int descriptor, mode_t type) noexcept {
    struct stat status{};
    return ::fstat(descriptor, &status) == 0 && (status.st_mode & S_IFMT) == type && status.st_uid == ::geteuid() &&
      (status.st_mode & 077) == 0;
  }

  // Creates the folder if it does not exist and opens it, checked.
  std::optional<BrokerError> open_folder(const std::string& identity, Descriptor& folder) const {
    if (!file_safe(identity)) {
      return token_store_error("token store: the identity cannot name a file");
    }
    if (::mkdir(folder_.c_str(), 0700) != 0 && errno != EEXIST) {
      return token_store_error("token store: the folder could not be created");
    }
    folder.reset(::open(folder_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (folder.get() < 0) {
      struct stat status{};
      const bool link = ::lstat(folder_.c_str(), &status) == 0 && S_ISLNK(status.st_mode);
      return token_store_error(
        link ? "token store: the folder is a symbolic link" : "token store: the folder could not be opened"
      );
    }
    if (!owned_privately(folder.get(), S_IFDIR)) {
      return token_store_error("token store: the folder is not private (owner or permissions)");
    }
    return std::nullopt;
  }

  // Checks a file just opened.
  static std::optional<BrokerError> check_file(const Descriptor& file) {
    if (file.get() < 0) {
      return token_store_error(
        errno == ELOOP ? "token store: a file is a symbolic link" : "token store: a file could not be opened"
      );
    }
    if (!owned_privately(file.get(), S_IFREG)) {
      return token_store_error("token store: a file is not private (owner, permissions or type)");
    }
    return std::nullopt;
  }

  // Reads the token file into `text`; leaves it empty when there is none.
  static std::optional<BrokerError> read_token(
    const Descriptor& folder, const std::string& identity, std::optional<std::string>& text
  ) {
    Descriptor file(::openat(folder.get(), (identity + ".token").c_str(), FILE_FLAGS | O_RDONLY));
    if (file.get() < 0 && errno == ENOENT) {
      return std::nullopt;
    }
    if (auto error = check_file(file)) {
      return error;
    }
    std::string contents;
    char buffer[4096];
    while (contents.size() <= MAXIMUM_TOKEN_FILE) {
      const auto count = ::read(file.get(), buffer, sizeof(buffer));
      if (count == 0) {
        text = std::move(contents);
        return std::nullopt;
      }
      if (count < 0 && errno != EINTR) {
        return token_store_error("token store: a file could not be read");
      }
      if (count > 0) {
        contents.append(buffer, static_cast<std::size_t>(count));
      }
    }
    text = std::string();
    return std::nullopt;
  }

  // Reads a token file's text; nothing unless it is a JSON object with a nonempty token and an expiry.
  static std::optional<StoredToken> parse(const std::string& text) {
    boost::system::error_code ec;
    const auto document = boost::json::parse(text, ec);
    if (ec || !document.is_object()) {
      return std::nullopt;
    }
    const auto* token = document.as_object().if_contains("access_token");
    const auto* expiry = document.as_object().if_contains("expires_at_ns");
    if (!token || !token->is_string() || token->as_string().empty() || !expiry || !expiry->is_int64()) {
      return std::nullopt;
    }
    const auto since_epoch = std::chrono::nanoseconds(expiry->as_int64());
    return StoredToken{
      std::string(token->as_string()),
      std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(since_epoch)
      ),
    };
  }

  // Writes the token to the temporary file, flushes it and renames it over the token file.
  static std::optional<BrokerError> write_token(
    const Descriptor& folder, const std::string& identity, const StoredToken& token
  ) {
    boost::json::object document;
    document["access_token"] = token.access_token_;
    document["expires_at_ns"] =
      std::chrono::duration_cast<std::chrono::nanoseconds>(token.expires_at_.time_since_epoch()).count();
    const auto text = boost::json::serialize(document);

    const auto temporary = identity + ".tmp";
    // A writer killed before its rename left this file; the lock is held, so nobody else writes it.
    ::unlinkat(folder.get(), temporary.c_str(), 0);
    Descriptor file(::openat(folder.get(), temporary.c_str(), FILE_FLAGS | O_WRONLY | O_CREAT | O_EXCL, 0600));
    bool written = file.get() >= 0;
    for (std::size_t offset = 0; written && offset < text.size();) {
      const auto count = ::write(file.get(), text.data() + offset, text.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
      } else if (count == 0 || errno != EINTR) {
        written = false;
      }
    }
    written = written && ::fsync(file.get()) == 0 &&
      ::renameat(folder.get(), temporary.c_str(), folder.get(), (identity + ".token").c_str()) == 0;
    if (!written) {
      if (file.get() >= 0) {
        ::unlinkat(folder.get(), temporary.c_str(), 0);
      }
      return token_store_error("token store: the token file could not be written");
    }
    return std::nullopt;
  }

  // Removes the token file; none is not an error.
  static std::optional<BrokerError> unlink_token(const Descriptor& folder, const std::string& identity) {
    if (::unlinkat(folder.get(), (identity + ".token").c_str(), 0) != 0 && errno != ENOENT) {
      return token_store_error("token store: the token file could not be removed");
    }
    return std::nullopt;
  }

  std::string folder_;
  std::chrono::milliseconds lock_wait_;
};

} // namespace network
} // namespace algotradelib

#endif
