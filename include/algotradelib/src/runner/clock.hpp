#pragma once

#include <chrono>
#include <thread>

namespace algotradelib {
namespace runner {

/**
 * @brief The real clock a live loop uses: wall-clock time to stamp events, monotonic time to wait
 *   and to measure how long something took.
 *
 * `now()` is wall-clock time (it can jump when NTP or an operator sets the clock), because events
 * are stamped with it and must agree with brokers' timestamps. `wait_for()` is measured on
 * `steady_clock`, so a sleep is as long as it was asked to be whatever happens to the wall clock, and
 * `monotonic_now()` reads that clock, which is what a bounded wait is measured on
 * (`PollingLoop::set_closing_wait`). When a poll is next due is still a wall-clock time. A custom
 * clock for a loop needs the same three calls; `ManualClock` is the test version.
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
   * @brief Returns a reading that only moves forward, for measuring elapsed time.
   * @return `std::chrono::steady_clock::now()`.
   */
  std::chrono::steady_clock::time_point monotonic_now() const noexcept {
    return std::chrono::steady_clock::now();
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
 * @brief A clock that reads another one, so that two holders (a loop and its journal) read one time.
 *
 * Clocks are taken by value, and a copy of a `ManualClock` does not follow the original: a journal
 * given one copy and a loop given another would stamp lines at the starting time while the loop
 * moves on. Give both a `ClockReference` to the same clock instead.
 *
 * @tparam Clock The clock read; it must outlive every copy of the reference.
 */
template <typename Clock>
class ClockReference {
public:
  /**
   * @brief Refers to a clock.
   * @param clock The clock; borrowed, and must outlive every copy of this reference.
   */
  explicit ClockReference(Clock& clock) noexcept : clock_(&clock) {}

  /**
   * @brief Returns the referenced clock's wall-clock time.
   * @return Its `now()`.
   */
  std::chrono::system_clock::time_point now() const { return clock_->now(); }

  /**
   * @brief Returns the referenced clock's monotonic reading.
   * @return Its `monotonic_now()`.
   */
  std::chrono::steady_clock::time_point monotonic_now() const { return clock_->monotonic_now(); }

  /**
   * @brief Waits on the referenced clock.
   * @param duration How long.
   */
  void wait_for(std::chrono::nanoseconds duration) { clock_->wait_for(duration); }

private:
  Clock* clock_;
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
   * @brief Returns the monotonic reading: how far `wait_for` and `advance` have moved the clock.
   * @return The reading; `jump` does not move it.
   */
  std::chrono::steady_clock::time_point monotonic_now() const noexcept { return monotonic_; }

  /**
   * @brief Advances the clock by a duration instead of waiting, and counts the wait.
   * @param duration The duration; zero or negative counts the wait without advancing.
   */
  void wait_for(std::chrono::nanoseconds duration) noexcept {
    ++waits_;
    if (duration > std::chrono::nanoseconds::zero()) {
      now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
      monotonic_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
      waited_ += duration;
    }
  }

  /**
   * @brief Advances the clock without counting a wait: time that passed, as inside a slow call.
   * @param duration The duration; zero or negative does nothing to the monotonic reading.
   */
  void advance(std::chrono::nanoseconds duration) noexcept {
    now_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
    if (duration > std::chrono::nanoseconds::zero()) {
      monotonic_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
    }
  }

  /**
   * @brief Moves the wall-clock reading alone, forward or back, as an operator or NTP setting the clock does.
   * @param duration The jump.
   */
  void jump(std::chrono::nanoseconds duration) noexcept {
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
  std::chrono::steady_clock::time_point monotonic_{};
  std::size_t waits_{0};
  std::chrono::nanoseconds waited_{std::chrono::nanoseconds::zero()};
};

} // namespace runner
} // namespace algotradelib
