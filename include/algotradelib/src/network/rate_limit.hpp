#pragma once

/**
 * @brief Defined as 1 once a transport (network) header is included.
 *
 * The library's tests use it to check that the strategy headers include no transport.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include "../core/enum_values.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace algotradelib {
namespace network {

/**
 * @brief The traffic class of a request, for rate limiting and queue priority. The broker adapter
 *   that builds a request sets it.
 */
enum class RateLimitGroup {
  /**
   * @brief Order placement, amendment and cancellation.
   *   `HttpClientOptions::fresh_connection_for_orders_` applies to these requests.
   */
  TRADING,
  /** @brief Access-token issue and refresh. */
  AUTH,
  /** @brief Holdings, working orders, order history and buying power. */
  ACCOUNT,
  /** @brief Quotes, bars, order book and trades. */
  MARKET_DATA,
  /** @brief Instrument reference data and trading calendars. */
  REFERENCE,
};

/** @brief Every `RateLimitGroup`, in declaration order. */
inline constexpr std::array<RateLimitGroup, 5> all_rate_limit_groups{
  RateLimitGroup::TRADING,
  RateLimitGroup::AUTH,
  RateLimitGroup::ACCOUNT,
  RateLimitGroup::MARKET_DATA,
  RateLimitGroup::REFERENCE,
};

/**
 * @brief Returns a traffic class's queue priority.
 *
 * From highest: `AUTH` (every other request waits for a token), `TRADING`, `ACCOUNT`, `MARKET_DATA`,
 * `REFERENCE`.
 *
 * @param group The class.
 * @return The priority; larger is served first.
 */
inline constexpr int rate_limit_priority(RateLimitGroup group) noexcept {
  switch (group) {
    case RateLimitGroup::AUTH:
      return 50;
    case RateLimitGroup::TRADING:
      return 40;
    case RateLimitGroup::ACCOUNT:
      return 30;
    case RateLimitGroup::MARKET_DATA:
      return 20;
    case RateLimitGroup::REFERENCE:
      return 10;
  }
  return 0;
}

/**
 * @brief Returns a lowercase name for a traffic class, such as `"market_data"`, for logs.
 * @param group The class.
 * @return The name; empty for a value that is not a `RateLimitGroup`.
 */
inline constexpr std::string_view rate_limit_group_name(RateLimitGroup group) noexcept {
  switch (group) {
    case RateLimitGroup::AUTH:
      return "auth";
    case RateLimitGroup::TRADING:
      return "trading";
    case RateLimitGroup::ACCOUNT:
      return "account";
    case RateLimitGroup::MARKET_DATA:
      return "market_data";
    case RateLimitGroup::REFERENCE:
      return "reference";
  }
  return "";
}

static_assert(
  lists_every_value(all_rate_limit_groups, [](RateLimitGroup group) { return !rate_limit_group_name(group).empty(); }),
  "all_rate_limit_groups must list every RateLimitGroup"
);

/** @brief The number of traffic classes. */
inline constexpr std::size_t rate_limit_group_count = all_rate_limit_groups.size();

/**
 * @brief Returns a traffic class's index, for per-class arrays.
 * @param group The class.
 * @return An index below `rate_limit_group_count`.
 */
inline constexpr std::size_t rate_limit_group_index(RateLimitGroup group) noexcept {
  return static_cast<std::size_t>(group);
}

/**
 * @brief The request rate a connection may use.
 *
 * The defaults are a starting point, not a safe figure: a broker's real limit may be lower. KIS
 * states no per-key figure; on one account, eight a second was too many for its holiday calendar,
 * and a demo account refused reads less than about a second apart. The examples pace KIS at one a
 * second.
 */
struct RateLimitBudget {
  /** @brief How many requests may be sent back to back after an idle period; at least 1. */
  int burst_{8};
  /** @brief The sustained rate, in requests per second; at least 1. */
  int per_second_{8};
  /**
   * @brief The part of `burst_` only `AUTH` and `TRADING` requests may use, so polling cannot use up
   *   the capacity an order needs (a request already on the wire still goes first); zero disables
   *   the reservation.
   */
  int trading_reserve_{2};
};

/**
 * @brief Returns how much of the burst a traffic class may use.
 * @param budget The budget.
 * @param group The class.
 * @return `burst_` for `TRADING` and `AUTH`; otherwise `burst_ - trading_reserve_`, at least 1.
 */
inline constexpr int usable_burst(const RateLimitBudget& budget, RateLimitGroup group) noexcept {
  switch (group) {
    // An order, and the token it needs, may use the reserve.
    case RateLimitGroup::TRADING:
    case RateLimitGroup::AUTH:
      return budget.burst_;
    case RateLimitGroup::ACCOUNT:
    case RateLimitGroup::MARKET_DATA:
    case RateLimitGroup::REFERENCE:
      break;
  }
  const int reserved = budget.burst_ - budget.trading_reserve_;
  return reserved > 0 ? reserved : 1;
}

/**
 * @brief A rate limiter (the generic cell rate algorithm) that computes when a request may go; it
 *   neither waits nor reads the clock itself.
 *
 * Each call passes the current instant and an allowance: how much of the burst the caller may use
 * (see `usable_burst`). A caller with a smaller allowance reaches its limit earlier, so one schedule
 * can keep a reserve for trading.
 *
 * @tparam Clock The clock whose instants are passed in.
 */
template <typename Clock = std::chrono::steady_clock>
class BasicRateLimitBucket {
public:
  /** @brief The clock type. */
  using ClockType = Clock;

  /** @brief The clock's instant type. */
  using TimePoint = typename Clock::time_point;

  /** @brief The duration type of all results. */
  using Duration = std::chrono::nanoseconds;

  /**
   * @brief Constructs a bucket.
   * @param burst How many requests may go back to back after an idle period.
   * @param per_second The sustained rate.
   * @throws std::invalid_argument if either is below 1.
   */
  BasicRateLimitBucket(int burst, int per_second) { resize(burst, per_second); }

  /**
   * @brief Constructs a bucket from a budget's burst and rate.
   * @param budget The budget.
   * @throws std::invalid_argument if either is below 1.
   */
  explicit BasicRateLimitBucket(const RateLimitBudget& budget)
  : BasicRateLimitBucket(budget.burst_, budget.per_second_) {}

  /**
   * @brief Changes the burst and rate, keeping the current schedule.
   * @param burst The burst.
   * @param per_second The sustained rate.
   * @throws std::invalid_argument if either is below 1.
   */
  void resize(int burst, int per_second) {
    if (burst < 1) {
      throw std::invalid_argument("rate limit burst must be at least one request");
    }
    if (per_second < 1) {
      throw std::invalid_argument("rate limit rate must be at least one request per second");
    }
    burst_ = burst;
    per_second_ = per_second;
    interval_ = Duration(std::chrono::nanoseconds(std::chrono::seconds(1)).count() / per_second);
  }

  /**
   * @brief Returns how long a request must wait.
   * @param now The current instant.
   * @param allowance How much of the burst the caller may use (clamped to 1..`burst()`).
   * @return The wait, zero if it may go now.
   */
  Duration delay_until(TimePoint now, int allowance) const {
    if (!started_) {
      return Duration::zero();
    }
    const auto earliest = schedule_ - tolerance(allowance);
    if (now >= earliest) {
      return Duration::zero();
    }
    return std::chrono::duration_cast<Duration>(earliest - now);
  }

  /**
   * @brief Tests whether a request may go now.
   * @param now The current instant.
   * @param allowance How much of the burst the caller may use.
   * @return `true` if it need not wait.
   */
  bool ready(TimePoint now, int allowance) const {
    return delay_until(now, allowance) == Duration::zero();
  }

  /**
   * @brief Admits one request if it may go now.
   * @param now The current instant.
   * @param allowance How much of the burst the caller may use.
   * @return `true` if admitted (the schedule advances one interval); `false` if it must wait, with
   *   nothing changed.
   */
  bool try_acquire(TimePoint now, int allowance) {
    if (!ready(now, allowance)) {
      return false;
    }
    if (!started_) {
      schedule_ = now + interval_;
      started_ = true;
      return true;
    }
    schedule_ = (schedule_ > now ? schedule_ : now) + interval_;
    return true;
  }

  /**
   * @brief Holds every request for at least a duration (for a broker refusal that is not this
   *   client's pacing fault). A caller with a smaller allowance waits correspondingly longer.
   *
   * A hold never shortens an existing one.
   *
   * @param now The current instant.
   * @param duration The hold; a negative one counts as zero.
   */
  void cool_down(TimePoint now, Duration duration) {
    if (duration < Duration::zero()) {
      duration = Duration::zero();
    }
    const auto held = now + duration + tolerance(burst_);
    if (!started_ || held > schedule_) {
      schedule_ = held;
      started_ = true;
    }
  }

  /**
   * @brief Adopts a burst and rate the broker reported, then holds requests until it refills.
   * @param now The current instant.
   * @param burst The broker's burst.
   * @param per_second The broker's rate.
   * @param refill How long to hold; zero or less holds for one interval.
   * @throws std::invalid_argument if `burst` or `per_second` is below 1.
   */
  void observe_limit(TimePoint now, int burst, int per_second, Duration refill) {
    resize(burst, per_second);
    cool_down(now, refill > Duration::zero() ? refill : interval_);
  }

  /**
   * @brief Returns how many requests could go back to back now.
   * @param now The current instant.
   * @param allowance How much of the burst the caller may use.
   * @return The count, at most `allowance`.
   */
  int available(TimePoint now, int allowance) const {
    if (!started_) {
      return allowance;
    }
    const auto earliest = schedule_ - tolerance(allowance);
    if (now < earliest) {
      return 0;
    }
    const auto spare = std::chrono::duration_cast<Duration>(now - earliest);
    const auto slots = spare / interval_ + 1;
    return slots > allowance ? allowance : static_cast<int>(slots);
  }

  /**
   * @brief Returns the burst.
   * @return The burst.
   */
  int burst() const noexcept { return burst_; }

  /**
   * @brief Returns the sustained rate.
   * @return Requests per second.
   */
  int per_second() const noexcept { return per_second_; }

  /**
   * @brief Returns the spacing of the sustained rate.
   * @return One second divided by the rate.
   */
  Duration interval() const noexcept { return interval_; }

private:
  // Returns how far ahead of now the schedule may run for `allowance`.
  Duration tolerance(int allowance) const noexcept {
    const int slots = allowance < 1 ? 1 : (allowance > burst_ ? burst_ : allowance);
    return interval_ * (slots - 1);
  }

  int burst_{1};
  int per_second_{1};
  Duration interval_{std::chrono::seconds(1)};
  TimePoint schedule_{};
  bool started_{false};
};

/** @brief A rate-limit bucket on the steady clock. */
using RateLimitBucket = BasicRateLimitBucket<>;

/** @brief A rate limiter's counters for one traffic class. */
struct RateLimitGroupStats {
  /** @brief Requests admitted. */
  std::uint64_t admitted_{0};
  /** @brief Admitted requests that had to wait first (on a polling class, often the trading reserve at work). */
  std::uint64_t delayed_{0};
  /** @brief Requests refused locally: the queue was full, or the wait would have exceeded the timeout. */
  std::uint64_t shed_{0};
  /** @brief Total time admitted requests waited. */
  std::chrono::nanoseconds waited_{0};
};

/**
 * @brief A rate limiter's counters per traffic class, its current budget, and the broker's refusals
 *   (from `RateLimitController::stats`).
 */
struct RateLimitStats {
  /** @brief The counters per traffic class; read them with `group()`. */
  RateLimitGroupStats groups_[rate_limit_group_count];
  /** @brief The burst in force. */
  int burst_{0};
  /** @brief The sustained rate in force. */
  int per_second_{0};
  /** @brief How much of the burst was unspent just after the last admitted request (for that request's class); not updated otherwise. */
  int available_{0};
  /** @brief How many rate-limit refusals the broker sent; if it keeps rising, lower the budget. */
  std::uint64_t broker_rejections_{0};

  /**
   * @brief Returns the counters for one traffic class.
   * @param value The class.
   * @return The counters.
   */
  RateLimitGroupStats& group(RateLimitGroup value) {
    return groups_[rate_limit_group_index(value)];
  }

  /**
   * @brief Returns the counters for one traffic class.
   * @param value The class.
   * @return The counters.
   */
  const RateLimitGroupStats& group(RateLimitGroup value) const {
    return groups_[rate_limit_group_index(value)];
  }
};

/**
 * @brief The rate limiter of one connection (one app key): a `RateLimitBucket` on the steady clock,
 *   plus counters.
 *
 * Not synchronized: use it only from the owning client's strand (see
 * `AsyncHttpClient::rate_limits`).
 */
class RateLimitController {
public:
  /** @brief The duration type. */
  using Duration = RateLimitBucket::Duration;

  /**
   * @brief Constructs a controller.
   * @param budget The budget.
   * @throws std::invalid_argument if its burst or rate is below 1.
   */
  explicit RateLimitController(RateLimitBudget budget = {})
  : budget_(budget), bucket_(budget) {
    stats_.burst_ = budget.burst_;
    stats_.per_second_ = budget.per_second_;
  }

  /**
   * @brief Returns how long a request of a class must wait.
   * @param group The class.
   * @return The wait, zero if it may go now.
   */
  Duration delay_until(RateLimitGroup group) const {
    return bucket_.delay_until(RateLimitBucket::ClockType::now(), usable_burst(budget_, group));
  }

  /**
   * @brief Admits one request of a class if it may go now, and counts it.
   * @param group The class.
   * @return `true` if admitted; `false` if it must wait, with nothing changed.
   */
  bool acquire(RateLimitGroup group) {
    const auto now = RateLimitBucket::ClockType::now();
    if (!bucket_.try_acquire(now, usable_burst(budget_, group))) {
      return false;
    }
    ++stats_.group(group).admitted_;
    stats_.available_ = bucket_.available(now, usable_burst(budget_, group));
    return true;
  }

  /**
   * @brief Records that a request waited before admission.
   * @param group The class.
   * @param waited How long.
   */
  void record_delay(RateLimitGroup group, Duration waited) {
    auto& counters = stats_.group(group);
    ++counters.delayed_;
    counters.waited_ += waited;
  }

  /**
   * @brief Records that a request was refused unsent.
   * @param group The class.
   */
  void record_shed(RateLimitGroup group) { ++stats_.group(group).shed_; }

  /**
   * @brief Counts a broker refusal and holds every request for a duration, keeping the budget.
   *
   * For a refusal this client's pacing could not have prevented (capacity shared with other
   * customers). The refusal is counted in `RateLimitStats::broker_rejections_`, not per class.
   *
   * @param duration The hold.
   */
  void hold(Duration duration) {
    ++stats_.broker_rejections_;
    bucket_.cool_down(RateLimitBucket::ClockType::now(), duration);
  }

  /**
   * @brief Counts a broker refusal, adopts the burst and rate the broker reported, and holds.
   *
   * The trading reserve is reduced if it would not be smaller than the new burst.
   *
   * @param burst The broker's burst.
   * @param per_second The broker's rate.
   * @param retry How long to hold; zero or less holds for one interval.
   * @throws std::invalid_argument if `burst` or `per_second` is below 1.
   */
  void shrink(int burst, int per_second, Duration retry) {
    // The bucket validates first, so a refused figure leaves the reported budget the one in force.
    bucket_.observe_limit(RateLimitBucket::ClockType::now(), burst, per_second, retry);
    ++stats_.broker_rejections_;
    apply_budget(burst, per_second);
  }

  /**
   * @brief Returns the budget in force.
   * @return The budget.
   */
  const RateLimitBudget& budget() const noexcept { return budget_; }

  /**
   * @brief Returns the counters.
   * @return The statistics.
   */
  const RateLimitStats& stats() const noexcept { return stats_; }

private:
  // Records a new allowance, keeping the reserve smaller than the burst.
  void apply_budget(int burst, int per_second) {
    budget_.burst_ = burst;
    budget_.per_second_ = per_second;
    if (budget_.trading_reserve_ >= burst) {
      budget_.trading_reserve_ = burst > 1 ? burst - 1 : 0;
    }
    stats_.burst_ = burst;
    stats_.per_second_ = per_second;
  }

  RateLimitBudget budget_;
  RateLimitBucket bucket_;
  RateLimitStats stats_{};
};

} // namespace network
} // namespace algotradelib
