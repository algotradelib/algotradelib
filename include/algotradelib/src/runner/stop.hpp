#pragma once

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace algotradelib {
namespace runner {

/**
 * What an operator means by "stop", which is three different things.
 *
 * Naming them separately matters more than whatever channel delivers them. An operator reaching for
 * a stop under pressure must not have to guess which one they are getting, because each *asks the
 * broker for* something materially different:
 *
 * | Intent | Stops sending | Requests cancellation of resting orders | Requests closing orders |
 * | --- | --- | --- | --- |
 * | `QUIESCE` | yes | no | no |
 * | `WITHDRAW` | yes | yes | no |
 * | `FLATTEN` | yes | yes | yes |
 *
 * **Only the first column is a fact about the exchange.** Not sending is entirely local and
 * therefore certain; the other two are requests, and a request that is accepted is not an outcome.
 * A cancellation the broker accepts can still be rejected afterwards and leave the order working,
 * and a closing order that is accepted has not necessarily filled. `runner::LoopReport` counts what
 * was accepted, `closing_complete()` says whether everything asked for was accepted, and the
 * broker's own order list and holdings are the only authority on what is actually left.
 *
 * The enumerators are ordered by escalation, and `stop_intent_covers` relies on that order, so a
 * new intent must be inserted at the position matching how much it does rather than appended.
 */
enum class StopIntent {
  /**
   * Stop sending new orders. Nothing is withdrawn and nothing is closed.
   *
   * The default everywhere, and what both stop signals mean. It is the only intent that leaves
   * nothing indeterminate -- the request in flight completes and gets classified -- and the only one
   * whose effect is entirely this process's to guarantee.
   */
  QUIESCE,
  /**
   * Quiesce, then ask the broker to cancel the working orders. No closing orders are placed.
   *
   * Each cancellation is itself an order-writing request that can fail indeterminately, so this
   * costs more than it looks like it does -- and an accepted one is a request the broker took, not
   * an order that has stopped resting. Read its order list afterwards.
   */
  WITHDRAW,
  /**
   * Withdraw, then ask the broker to close the positions with market orders.
   *
   * A trading decision taken under duress, and never what a signal means. Available because an
   * operator sometimes genuinely wants it, and explicit because nobody should reach it by accident.
   * An accepted closing order has not necessarily filled, so being flat is something a later
   * holdings snapshot shows rather than something this intent returns.
   */
  FLATTEN,
};

/** Returns a stable lowercase name for `intent`, for logs and for the control file. */
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

/** Returns the intent `text` names, or nothing when it names none. */
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

/** Returns whether `held` already does everything `candidate` would. */
inline constexpr bool stop_intent_covers(StopIntent held, StopIntent candidate) noexcept {
  return static_cast<int>(held) >= static_cast<int>(candidate);
}

/**
 * A stop request the loop polls, and the only thing a signal handler touches.
 *
 * Deliberately a plain value with no knowledge of signals, files, or loops. That is what lets every
 * stop path be tested without raising a signal and without a broker — the same reason
 * `IndeterminacyLedger` is broker-free.
 *
 * **`std::atomic<int>` rather than a bool**, because the intent has to arrive with the request; a
 * flag would say stop without saying which of the three. It is lock-free on every supported
 * platform and, critically, `std::atomic<int>::store` on a lock-free type is what a signal handler
 * is permitted to call. A `std::string` here would make the handler illegal.
 *
 * **An escalation, never a downgrade.** Two requests take the stronger one: an operator who asks to
 * flatten and then, a moment later, sends a routine `SIGTERM` must not have the flatten quietly
 * reduced to a quiesce. Escalating is also idempotent, so a supervisor sending `SIGTERM` twice
 * changes nothing.
 */
class StopSource {
public:
  /**
   * Requests a stop, keeping whichever intent does more.
   *
   * Safe to call from a signal handler. It is a compare-exchange loop rather than a bare store,
   * because it must not downgrade an intent already held, and that is permitted in a handler: the
   * restriction is on non-lock-free atomics and on everything that might allocate or lock, not on
   * the number of operations. The loop terminates -- it only retries when another thread wrote
   * concurrently, and each retry sees a value at least as large.
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

  /** Returns whether a stop has been requested. */
  bool requested() const noexcept {
    return requested_.load(std::memory_order_acquire) >= 0;
  }

  /** Returns the intent requested, or nothing when no stop has been asked for. */
  std::optional<StopIntent> intent() const noexcept {
    const int held = requested_.load(std::memory_order_acquire);
    if (held < 0) {
      return std::nullopt;
    }
    return static_cast<StopIntent>(held);
  }

  /**
   * Clears the request.
   *
   * For tests and for a supervisor that resumes a quiesced runner deliberately. Not called by the
   * loop, which exits rather than un-stopping itself.
   */
  void clear() noexcept { requested_.store(-1, std::memory_order_release); }

private:
  /** Negative means no request; otherwise a `StopIntent`. Signal handlers may only store here. */
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
 * A control file an operator writes to name an intent.
 *
 * The secondary channel, and the one that can distinguish all three intents without inventing a
 * signal for each. Scriptable, survives anything, and needs no open port:
 *
 * ```sh
 * echo withdraw > /var/run/atl/stop
 * ```
 *
 * Latency is bounded by the loop's cadence, which is acceptable for an instruction issued by hand.
 *
 * **A deliberately narrow reader.** It takes the first token of the first line, ignores surrounding
 * whitespace, and ignores a file it cannot open — an absent file is the normal case, not an error.
 * Unrecognized content is reported as unrecognized rather than guessed at, because guessing here
 * would pick a stop intent on an operator's behalf.
 *
 * **It never deletes the file.** A control file that consumed itself would make "did my stop take
 * effect?" unanswerable after the fact, and the loop exits anyway. So a word left in it is carried out
 * by the next process too, from its first iteration: a `withdraw` or `flatten` reads the order list and
 * acts on the whole account before anything trades. Remove it before trading again.
 */
class FileStopChannel {
public:
  /** Constructs a channel watching `path`. An empty path disables it. */
  explicit FileStopChannel(std::string path = {}) : path_(std::move(path)) {}

  /** Returns whether a path was configured. */
  bool enabled() const noexcept { return !path_.empty(); }

  /** Returns the path being watched. */
  const std::string& path() const noexcept { return path_; }

  /**
   * Reads the file and returns the intent it names.
   *
   * Returns nothing when the channel is disabled, the file is absent, the file is empty, or its
   * content names no intent. `unrecognized()` distinguishes the last case from the others, so a
   * loop can report a typo rather than silently ignoring a stop an operator believes they issued.
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

  /** Returns the last unrecognized token read, or empty when the last read was understood. */
  const std::string& unrecognized() const noexcept { return unrecognized_; }

private:
  std::string path_;
  std::string unrecognized_;
};

} // namespace runner
} // namespace algotradelib
