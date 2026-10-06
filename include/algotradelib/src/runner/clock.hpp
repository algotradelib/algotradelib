#pragma once

#include <chrono>
#include <thread>

namespace algotradelib {
namespace runner {

/**
 * @brief The real clock a live loop uses: wall-clock time to stamp events, monotonic time to wait.
 *
 * `now()` is wall-clock time (it can jump when NTP or an operator sets the clock), because events
 * are stamped with it and must agree with brokers' timestamps. `wait_for()` is measured on
 * `steady_clock`, so a poll cadence is unaffected by such jumps. A custom clock for a loop needs
 * the same two calls; `ManualClock` is the test version.
 */
class SteadyClock {
public:
  /**
   * @brief Returns the current wall-clock time.
   * @return `std::chrono::system_clock::now()`.
   */
  std::chrono::system_clock::time_point now() const noexcept {
    return std::chrono::system_clock::now();
  }

  /**
   * @brief Blocks for a duration, measured on the monotonic clock.
   * @param duration How long to wait; zero or negative returns at once.
   */
  void wait_for(std::chrono::nanoseconds duration) const {
    if (duration <= std::chrono::nanoseconds::zero()) {
      return;
    }
    std::this_thread::sleep_for(duration);
  }
};

/**
 * @brief A clock for tests: `wait_for` advances it instead of sleeping.
 *
 * A loop under test runs instantly while seeing whatever elapsed time the test chooses. `waits()`
 * counts the waits, so a test can tell a loop that paced itself from one that spun.
 */
class ManualClock {
public:
  /**
   * @brief Constructs a clock.
   * @param start The time it reads.
   */
  explicit ManualClock(std::chrono::system_clock::time_point start = {}) : now_(start) {}

  /**
   * @brief Returns the current time.
   * @return The time the clock has been advanced to.
   */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Advances the clock by a duration instead of waiting, and counts the wait.
   * @param duration The duration; zero or negative counts the wait without advancing.
   */
  void wait_for(std::chrono::nanoseconds duration) noexcept {
    ++waits_;
    if (duration > std::chrono::nanoseconds::zero()) {
      now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
      waited_ += duration;
    }
  }

  /**
   * @brief Advances the clock without counting a wait.
   * @param duration The duration.
   */
  void advance(std::chrono::nanoseconds duration) noexcept {
    now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
  }

  /**
   * @brief Returns how many times `wait_for` was called.
   * @return The count.
   */
  std::size_t waits() const noexcept { return waits_; }

  /**
   * @brief Returns the total positive duration passed to `wait_for`.
   * @return The sum.
   */
  std::chrono::nanoseconds waited() const noexcept { return waited_; }

private:
  std::chrono::system_clock::time_point now_;
  std::size_t waits_{0};
  std::chrono::nanoseconds waited_{std::chrono::nanoseconds::zero()};
};

} // namespace runner
} // namespace algotradelib
