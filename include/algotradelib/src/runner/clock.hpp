#pragma once

#include <chrono>
#include <thread>

namespace algotradelib {
namespace runner {

/**
 * Where a live loop gets the time, and how it waits.
 *
 * A seam rather than a direct call to `std::chrono`, for the same reason the runner takes an
 * executor: the behaviour worth testing is the behaviour that depends on time passing, and a test
 * that genuinely waits is a test nobody runs. `ManualClock` makes a six-hour trading session an
 * instant.
 *
 * **Two clocks, deliberately, because they answer different questions.** This is the part worth
 * reading before implementing another one:
 *
 * - **`now()` returns a `system_clock::time_point`**, because it is what gets *stamped* on events,
 *   and it has to agree with `Context::now()` and with the timestamps brokers send. It is wall
 *   clock, so it can jump: NTP corrects it, an operator can set it, and daylight saving moves it.
 * - **`wait_for()` takes a duration and is measured on `steady_clock`**, because it answers "how
 *   long until the next poll" and that must not be affected by any of the above. A cadence measured
 *   on the wall clock will, the moment NTP steps the clock backwards, either sleep for the step or
 *   spin as fast as the loop can run — against a broker that is rate-limiting it.
 *
 * Getting this backwards is a real and quiet bug, which is why the two are separate calls with
 * separate types rather than one `sleep_until(now() + interval)`.
 */
class SteadyClock {
public:
  /** Returns the current wall-clock time, for stamping what the loop observes. */
  std::chrono::system_clock::time_point now() const noexcept {
    return std::chrono::system_clock::now();
  }

  /**
   * Blocks for `duration`, measured monotonically.
   *
   * A non-positive duration returns immediately rather than yielding, so a loop that has fallen
   * behind its cadence catches up instead of pausing once per overdue poll.
   */
  void wait_for(std::chrono::nanoseconds duration) const {
    if (duration <= std::chrono::nanoseconds::zero()) {
      return;
    }
    std::this_thread::sleep_for(duration);
  }
};

/**
 * A clock a test drives by hand, which never waits.
 *
 * `wait_for` advances the clock instead of sleeping, so a loop under test runs at the speed of the
 * CPU while believing whatever the test wants it to believe about elapsed time. That is what makes
 * a cadence assertion exact: the test can say "after four minutes, three quote polls and one bar
 * poll happened" and have it be a statement about arithmetic rather than about scheduling luck.
 *
 * `waits_` counts calls, and it is the assertion that distinguishes a loop that paced itself from
 * one that spun. A loop that polls the right number of times while never waiting is still wrong.
 */
class ManualClock {
public:
  /** Constructs a clock reading `start`. */
  explicit ManualClock(std::chrono::system_clock::time_point start = {}) : now_(start) {}

  /** Returns the time the test has advanced to. */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /** Advances the clock without pretending to block. */
  void wait_for(std::chrono::nanoseconds duration) noexcept {
    ++waits_;
    if (duration > std::chrono::nanoseconds::zero()) {
      now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
      waited_ += duration;
    }
  }

  /** Moves the clock forward directly, as an event arriving out of band would. */
  void advance(std::chrono::nanoseconds duration) noexcept {
    now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
  }

  /** Returns how many times the loop asked to wait. */
  std::size_t waits() const noexcept { return waits_; }

  /** Returns the total duration the loop asked to wait for. */
  std::chrono::nanoseconds waited() const noexcept { return waited_; }

private:
  std::chrono::system_clock::time_point now_;
  std::size_t waits_{0};
  std::chrono::nanoseconds waited_{std::chrono::nanoseconds::zero()};
};

} // namespace runner
} // namespace algotradelib
