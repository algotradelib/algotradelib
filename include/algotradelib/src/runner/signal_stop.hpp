#pragma once

#include "./stop.hpp"

#include <atomic>
#include <csignal>

namespace algotradelib {
namespace runner {

/**
 * Wires process signals to a `StopSource`, only when a program asks for it.
 *
 * **Nothing here happens unless called.** A header-only library that installed signal handlers on
 * inclusion would silently take over process-global state belonging to whatever program embeds it,
 * and would break any host that had its own handlers. So this is an explicit call, documented, and
 * entirely absent from every test of stop behaviour — `StopSource` is a plain value, so the loop's
 * stop paths are exercised without raising anything.
 *
 * **Why signals are the primary channel.** They already reach a detached process every way an
 * operator would want, with nothing to configure:
 *
 * | Situation | What arrives |
 * | --- | --- |
 * | `tmux send-keys -t sess:0.0 C-c` | `SIGINT` to the foreground process |
 * | `kill -TERM <pid>` | `SIGTERM`, tmux or not |
 * | host reboot under `systemd` | `SIGTERM`, then `SIGKILL` after `TimeoutStopSec` |
 * | `docker stop` | `SIGTERM`, then `SIGKILL` after ten seconds |
 *
 * That last column is the operational trap, and it is documented in the vignette rather than only
 * here: **the supervisor's grace period must exceed one loop iteration plus one request timeout.**
 * When it does not, the supervisor's `SIGKILL` converts the cleanest possible stop into the worst
 * one — a process killed mid-request, which manufactures exactly the indeterminate state that
 * requires reading the broker's order list before trading may resume. Silently, and precisely when
 * the operator believed they had shut down cleanly.
 *
 * ## The two decisions worth making deliberately
 *
 * **`SIGTERM` and `SIGINT` both quiesce, never flatten.** Either can arrive for entirely routine
 * reasons — a host reboot, a deploy, an operator's `Ctrl+C` — and turning a routine reboot into a
 * liquidation would be indefensible. An operator who wants to flatten says so, through the control
 * file or their own call to `StopSource::request`.
 *
 * **`SIGHUP` is ignored, not handled.** A dropped SSH connection must not stop a process holding
 * positions. That is `nohup` semantics, and it is the right default for this program: the default
 * disposition would terminate it outright, which is the level-5 stop, triggered by a flaky network.
 */
class SignalStop {
public:
  /**
   * Routes `SIGINT` and `SIGTERM` to `source` as a quiesce, and ignores `SIGHUP`.
   *
   * The source must outlive the process's use of signals; in practice it is a long-lived object
   * owned by `main`. Only one source can be installed at a time, because a handler receives nothing
   * but the signal number and so has to reach a single known destination — which is also why this is
   * a static function over a static pointer rather than an object with per-instance state.
   */
  static void install(StopSource& source) noexcept {
    slot().store(&source, std::memory_order_release);
    std::signal(SIGINT, &SignalStop::handle);
    std::signal(SIGTERM, &SignalStop::handle);
    // Explicitly ignored rather than left alone: the *default* disposition for SIGHUP terminates the
    // process, so doing nothing here would leave a dropped SSH session able to kill a runner that
    // holds positions.
    std::signal(SIGHUP, SIG_IGN);
  }

  /**
   * Restores the default disposition for the signals `install` took over.
   *
   * For a program that hands control back to a host with its own handlers, and for tests that must
   * not leave a process-global handler installed behind them.
   */
  static void uninstall() noexcept {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGHUP, SIG_DFL);
    slot().store(nullptr, std::memory_order_release);
  }

  /** Returns the source currently installed, or null when none is. */
  static StopSource* installed() noexcept { return slot().load(std::memory_order_acquire); }

private:
  /**
   * The handler, which does exactly one thing.
   *
   * Everything async-signal-unsafe is absent by construction: no allocation, no locking, no I/O, no
   * `errno` use. It loads a pointer from a lock-free atomic and calls `StopSource::request`, which
   * is itself nothing but a compare-exchange on a lock-free atomic.
   */
  static void handle(int) noexcept {
    if (StopSource* source = slot().load(std::memory_order_acquire)) {
      source->request(StopIntent::QUIESCE);
    }
  }

  /**
   * Where the handler finds its destination.
   *
   * Atomic because a handler can run on any thread at any time relative to `install`, and a
   * plain pointer read would be a data race. Function-local static inside an inline accessor keeps
   * this header-only with exactly one instance across translation units.
   */
  static std::atomic<StopSource*>& slot() noexcept {
    static std::atomic<StopSource*> value{nullptr};
    return value;
  }
};

} // namespace runner
} // namespace algotradelib
