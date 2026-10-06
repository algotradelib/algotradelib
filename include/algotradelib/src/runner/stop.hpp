#pragma once

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace algotradelib {
namespace runner {

/**
 * @brief What an operator asks for when stopping a running loop.
 *
 * | Intent | Stops sending | Requests cancellation of resting orders | Requests closing orders |
 * | --- | --- | --- | --- |
 * | `QUIESCE` | yes | no | no |
 * | `WITHDRAW` | yes | yes | no |
 * | `FLATTEN` | yes | yes | yes |
 *
 * Only "stops sending" is certain. Cancellations and closing orders are requests: an accepted
 * cancellation can still be rejected later, and an accepted closing order may not have filled.
 * `runner::LoopReport` counts what was accepted and `closing_complete()` says whether everything was;
 * the broker's order list and holdings say what is left.
 */
enum class StopIntent {
  /**
   * @brief Stop sending new orders; nothing is cancelled or closed. The default, and what a signal
   *   means.
   */
  QUIESCE,
  /**
   * @brief Quiesce, then ask the broker to cancel the working orders.
   *
   * Each cancellation can itself have an unknown outcome; read the order list afterwards.
   */
  WITHDRAW,
  /**
   * @brief Withdraw, then ask the broker to close the positions with market orders.
   *
   * Never what a signal means. Being flat is shown by a later holdings read, not by this intent.
   */
  FLATTEN,
};

/**
 * @brief Returns a lowercase name for an intent, as written in the control file (`"withdraw"`).
 * @param intent The intent.
 * @return The name.
 */
inline constexpr std::string_view stop_intent_name(StopIntent intent) noexcept {
  switch (intent) {
    case StopIntent::QUIESCE:
      return "quiesce";
    case StopIntent::WITHDRAW:
      return "withdraw";
    case StopIntent::FLATTEN:
      return "flatten";
  }
  return "unknown";
}

/**
 * @brief Parses an intent's name.
 * @param text `"quiesce"`, `"withdraw"` or `"flatten"`.
 * @return The intent, or `std::nullopt` for any other text.
 */
inline std::optional<StopIntent> stop_intent_from(std::string_view text) noexcept {
  if (text == "quiesce") {
    return StopIntent::QUIESCE;
  }
  if (text == "withdraw") {
    return StopIntent::WITHDRAW;
  }
  if (text == "flatten") {
    return StopIntent::FLATTEN;
  }
  return std::nullopt;
}

/**
 * @brief Tests whether one intent does everything another would.
 * @param held The intent in force.
 * @param candidate The intent requested.
 * @return `true` if `held` is at least as strong (`QUIESCE` < `WITHDRAW` < `FLATTEN`).
 */
inline constexpr bool stop_intent_covers(StopIntent held, StopIntent candidate) noexcept {
  return static_cast<int>(held) >= static_cast<int>(candidate);
}

/**
 * @brief A stop request the loop polls; safe to set from a signal handler.
 *
 * Two requests keep the stronger intent: a `FLATTEN` followed by a routine `SIGTERM` stays a
 * `FLATTEN`.
 */
class StopSource {
public:
  /**
   * @brief Requests a stop, keeping whichever of this and an earlier intent is stronger.
   *
   * Async-signal-safe (a lock-free atomic compare-exchange).
   *
   * @param intent The intent.
   */
  void request(StopIntent intent) noexcept {
    const int desired = static_cast<int>(intent);
    int current = requested_.load(std::memory_order_relaxed);
    while (current < desired) {
      if (requested_.compare_exchange_weak(
            current, desired, std::memory_order_release, std::memory_order_relaxed)) {
        return;
      }
    }
  }

  /**
   * @brief Tests whether a stop has been requested.
   * @return `true` once `request` has been called (until `clear`).
   */
  bool requested() const noexcept {
    return requested_.load(std::memory_order_acquire) >= 0;
  }

  /**
   * @brief Returns the requested intent.
   * @return The intent, or `std::nullopt` if no stop has been requested.
   */
  std::optional<StopIntent> intent() const noexcept {
    const int held = requested_.load(std::memory_order_acquire);
    if (held < 0) {
      return std::nullopt;
    }
    return static_cast<StopIntent>(held);
  }

  /** @brief Clears the request (for tests, or a supervisor deliberately resuming); the loop never does. */
  void clear() noexcept { requested_.store(-1, std::memory_order_release); }

private:
  // Negative means no request; otherwise a `StopIntent`.
  std::atomic<int> requested_{-1};

  // Asserted rather than assumed, because the signal-safety argument above rests entirely on it. If
  // this ever fails on some platform, `request()` is no longer callable from a handler and the
  // signal installer is unsound -- which is worth a compile error rather than a subtle deadlock in a
  // handler that fires once a month.
  static_assert(
    std::atomic<int>::is_always_lock_free,
    "StopSource requires a lock-free atomic<int> to stay signal-safe");
};

/**
 * @brief A control file an operator writes to stop a running loop with a given intent.
 *
 * ```sh
 * echo withdraw > /var/run/atl/stop
 * ```
 *
 * The first word of the file names the intent; a missing file is normal. Checked once per loop
 * iteration.
 *
 * @warning The file is never deleted. A word left in it stops the next process too, from its first
 *   iteration (a `withdraw` or `flatten` acting on the whole account before anything trades). Remove
 *   it before trading again.
 */
class FileStopChannel {
public:
  /**
   * @brief Constructs a channel.
   * @param path The file to watch; empty disables the channel.
   */
  explicit FileStopChannel(std::string path = {}) : path_(std::move(path)) {}

  /**
   * @brief Tests whether a path was configured.
   * @return `true` if the channel is enabled.
   */
  bool enabled() const noexcept { return !path_.empty(); }

  /**
   * @brief Returns the watched path.
   * @return The path.
   */
  const std::string& path() const noexcept { return path_; }

  /**
   * @brief Reads the file's first word.
   * @return The intent it names, or `std::nullopt` if the channel is disabled, the file is missing or
   *   empty, or the word names no intent (then `unrecognized()` returns the word).
   */
  std::optional<StopIntent> poll() {
    unrecognized_.clear();
    if (path_.empty()) {
      return std::nullopt;
    }
    std::FILE* handle = std::fopen(path_.c_str(), "rb");
    if (handle == nullptr) {
      return std::nullopt;
    }
    char buffer[64] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, handle);
    std::fclose(handle);

    std::string_view text(buffer, read);
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
      return std::nullopt;
    }
    text.remove_prefix(begin);
    const auto end = text.find_first_of(" \t\r\n");
    if (end != std::string_view::npos) {
      text = text.substr(0, end);
    }

    if (auto intent = stop_intent_from(text)) {
      return intent;
    }
    unrecognized_.assign(text);
    return std::nullopt;
  }

  /**
   * @brief Returns the word the last `poll` did not recognize.
   * @return The word, or empty if the last read was understood or found nothing.
   */
  const std::string& unrecognized() const noexcept { return unrecognized_; }

private:
  std::string path_;
  std::string unrecognized_;
};

} // namespace runner
} // namespace algotradelib
