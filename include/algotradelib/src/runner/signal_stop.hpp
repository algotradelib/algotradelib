#pragma once

#include "./stop.hpp"

#include <atomic>
#include <csignal>

namespace algotradelib {
namespace runner {

/**
 * @brief Routes `SIGINT` and `SIGTERM` to a `StopSource` as a quiesce, and ignores `SIGHUP`; only
 *   when `install` is called.
 *
 * Both signals mean `StopIntent::QUIESCE`, never a flatten, since they arrive for routine reasons
 * (a reboot, a deploy, `Ctrl+C`). `SIGHUP` is ignored so a dropped SSH session does not stop a
 * process holding positions.
 *
 * | Situation | What arrives |
 * | --- | --- |
 * | `tmux send-keys -t sess:0.0 C-c` | `SIGINT` to the foreground process |
 * | `kill -TERM <pid>` | `SIGTERM` |
 * | host reboot under `systemd` | `SIGTERM`, then `SIGKILL` after `TimeoutStopSec` |
 * | `docker stop` | `SIGTERM`, then `SIGKILL` after ten seconds |
 *
 * @warning A supervisor's grace period must exceed one loop iteration plus one request timeout.
 *   Otherwise its `SIGKILL` can kill the process mid-request, leaving an order of unknown outcome
 *   that must be reconciled from the broker's order list before trading again.
 */
class SignalStop {
public:
  /**
   * @brief Installs the handlers: `SIGINT` and `SIGTERM` request a quiesce on `source`; `SIGHUP` is
   *   ignored.
   *
   * Only one source can be installed at a time.
   *
   * @param source The stop source; it must outlive the handlers (until `uninstall`).
   */
  static void install(StopSource& source) noexcept {
    slot().store(&source, std::memory_order_release);
    std::signal(SIGINT, &SignalStop::handle);
    std::signal(SIGTERM, &SignalStop::handle);
    // Ignored: SIGHUP's default disposition would terminate the process.
	#ifdef SIGHUP
    std::signal(SIGHUP, SIG_IGN);
	#endif
  }

  /** @brief Restores the default disposition of `SIGINT`, `SIGTERM` and `SIGHUP`, and forgets the source. */
  static void uninstall() noexcept {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
	#ifdef SIGHUP
    std::signal(SIGHUP, SIG_DFL);
	#endif
    slot().store(nullptr, std::memory_order_release);
  }

  /**
   * @brief Returns the installed source.
   * @return The source, or `nullptr` if none is installed.
   */
  static StopSource* installed() noexcept { return slot().load(std::memory_order_acquire); }

private:
  // The handler, which does exactly one thing.
  //
  // Everything async-signal-unsafe is absent by construction: no allocation, no locking, no I/O, no
  // `errno` use. It loads a pointer from a lock-free atomic and calls `StopSource::request`, which
  // is itself nothing but a compare-exchange on a lock-free atomic.
  static void handle(int) noexcept {
    if (StopSource* source = slot().load(std::memory_order_acquire)) {
      source->request(StopIntent::QUIESCE);
    }
  }

  // Where the handler finds its destination.
  //
  // Atomic because a handler can run on any thread at any time relative to `install`, and a
  // plain pointer read would be a data race. Function-local static inside an inline accessor keeps
  // this header-only with exactly one instance across translation units.
  static std::atomic<StopSource*>& slot() noexcept {
    static std::atomic<StopSource*> value{nullptr};
    return value;
  }
};

} // namespace runner
} // namespace algotradelib
