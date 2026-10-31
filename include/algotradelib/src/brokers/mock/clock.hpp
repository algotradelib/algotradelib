#pragma once

#include "./market.hpp"

#include <chrono>

namespace algotradelib {
namespace mock {

/**
 * @brief The clock a loop waits on over a mock market: waiting moves the market.
 *
 * Give it to `runner::PollingLoop`, and through a `runner::ClockReference` to `live::bind_journaled`,
 * so the loop, its journal and the market read one time.
 */
class MarketClock {
public:
  /**
   * @brief Builds a clock over a market.
   * @param market The market; borrowed, and must outlive the clock.
   */
  explicit MarketClock(Market& market) : market_(&market) {}

  /**
   * @brief Returns the market's time.
   * @return `Market::now()`.
   */
  std::chrono::system_clock::time_point now() const noexcept { return market_->now(); }

  /**
   * @brief Returns a reading that moves with the market's time and never backwards.
   * @return The market's time as a steady reading.
   */
  std::chrono::steady_clock::time_point monotonic_now() const noexcept {
    return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(market_->now().time_since_epoch())
    );
  }

  /**
   * @brief Advances the market by a duration instead of sleeping.
   * @param duration How long; nothing moves if it is not positive.
   */
  void wait_for(std::chrono::nanoseconds duration) {
    if (duration.count() > 0) {
      market_->advance_to(market_->now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
    }
  }

private:
  Market* market_;
};

} // namespace mock
} // namespace algotradelib
