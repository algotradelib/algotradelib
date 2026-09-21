#pragma once

/**
 * Declares that this translation unit has reached the transport layer.
 *
 * Read by `tests/test-strategy-isolation.cpp` to assert that the strategy surface does not. A
 * strategy must name no broker and perform no I/O, and that property degrades silently: one
 * convenience include in a strategy header would couple every strategy to a vendor, and nothing
 * would fail until someone built the same source against the other broker. Declaring the layer
 * here makes the assertion an architectural statement rather than a search for a third-party
 * macro that happens to come along for the ride.
 */
#define ATL_TRANSPORT_LAYER_PRESENT 1

#include <algotradelib/core>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {
namespace network {

/**
 * Traffic class of one request, for admission and for queue priority.
 *
 * The class is a property of the *operation*, assigned by the broker adapter that
 * builds the request, never by the caller: a strategy cannot label its own chart
 * polling as trading traffic.
 */
enum class RateLimitGroup {
  /** Order placement, modification, and cancellation. */
  TRADING,
  /** Access-token issue and refresh. */
  AUTH,
  /** Holdings, working orders, buying power. */
  ACCOUNT,
  /** Quotes, candles, order book, trade prints. */
  MARKET_DATA,
  /** Instrument reference data and trading calendars. */
  REFERENCE,
};

/**
 * Returns the admission priority of `group`; a larger value is served first.
 *
 * `AUTH` ranks above `TRADING` because every other request waits behind a token
 * refresh, so delaying the refresh delays a cancellation too. It cannot starve
 * trading traffic: both adapters coalesce refreshes, so at most one is ever in
 * flight.
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

/** Returns a stable lowercase name for `group`, for statistics and logs. */
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
  return "unknown";
}

/** Number of distinct rate-limit groups, for fixed-size per-group storage. */
inline constexpr std::size_t rate_limit_group_count = 5;

/** Returns a dense index for `group`, for fixed-size per-group arrays. */
inline constexpr std::size_t rate_limit_group_index(RateLimitGroup group) noexcept {
  return static_cast<std::size_t>(group);
}

/**
 * How much request capacity one broker connection may spend.
 *
 * `burst_` is how many requests may be issued back to back from idle and
 * `per_second_` is the sustained rate. Both brokers state their limits per
 * second, so both are whole numbers and the pacing arithmetic stays integral.
 *
 * `trading_reserve_` is the part of `burst_` that only `TRADING` and `AUTH` may
 * spend. It is the whole point of grouping traffic: a saturated polling loop must
 * not be the reason a cancellation waits. A reserve of zero disables the
 * reservation without disabling the pacing.
 *
 * The defaults are deliberately conservative. KIS's per-key allowance is an
 * account-level portal setting that its documentation does not state, so the
 * library cannot assert a number; it can only start low and let a caller raise it.
 */
struct RateLimitBudget {
  int burst_{8};
  int per_second_{8};
  int trading_reserve_{2};
};

/**
 * Returns how much of the burst allowance `group` may spend under `budget`.
 *
 * `TRADING` and `AUTH` may spend all of it; every other group stops at the
 * reserve, which is what keeps capacity available for an order or a cancellation.
 */
inline constexpr int usable_burst(const RateLimitBudget& budget, RateLimitGroup group) noexcept {
  if (group == RateLimitGroup::TRADING || group == RateLimitGroup::AUTH) {
    return budget.burst_;
  }
  const int reserved = budget.burst_ - budget.trading_reserve_;
  return reserved > 0 ? reserved : 1;
}

/**
 * Paces requests by the generic cell rate algorithm, using integer arithmetic.
 *
 * The bucket stores one virtual schedule rather than a token count, so admission
 * is two integer comparisons and an addition with no floating point and no
 * periodic refill task. A request is admitted when the schedule has advanced to
 * within the caller's burst tolerance of now; admitting one pushes the schedule
 * forward by exactly one interval.
 *
 * `usable_burst` is supplied per call rather than fixed per bucket, which is how
 * one shared schedule implements a reservation: a caller allowed less of the
 * burst reaches its limit earlier while trading traffic can still draw on all of
 * it.
 *
 * The clock is a template parameter so tests drive time directly instead of
 * sleeping. The bucket performs no I/O and owns no timer; waiting belongs to the
 * caller.
 */
template <typename Clock = std::chrono::steady_clock>
class BasicRateLimitBucket {
public:
  /** Clock this bucket measures time with. */
  using ClockType = Clock;

  /** Instant type of `ClockType`. */
  using TimePoint = typename Clock::time_point;

  /** Duration used for all pacing arithmetic. */
  using Duration = std::chrono::nanoseconds;

  /**
   * Constructs a bucket allowing `burst` requests from idle and `per_second`
   * sustained.
   *
   * @throws std::invalid_argument if either value is below one.
   */
  BasicRateLimitBucket(int burst, int per_second) { resize(burst, per_second); }

  /** Constructs a bucket from a budget's burst and rate. */
  explicit BasicRateLimitBucket(const RateLimitBudget& budget)
  : BasicRateLimitBucket(budget.burst_, budget.per_second_) {}

  /**
   * Changes the allowance without discarding the schedule already accumulated.
   *
   * @throws std::invalid_argument if either value is below one.
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

  /** Returns how long a request of `allowance` burst must wait, zero if none. */
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

  /** Returns whether a request of `allowance` burst may be issued now. */
  bool ready(TimePoint now, int allowance) const {
    return delay_until(now, allowance) == Duration::zero();
  }

  /**
   * Admits one request of `allowance` burst, advancing the schedule.
   *
   * Returns false and changes nothing when the request must wait.
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
   * Holds admission for at least `duration`, measured for full burst allowance.
   *
   * Used for a rejection the client's own pacing cannot prevent — a broker's
   * per-endpoint ledger cap, shared with every other customer — where the remedy
   * is to wait rather than to shrink this client's budget. A group spending less
   * of the burst waits correspondingly longer, so the reservation still favours
   * trading traffic while the hold is in force.
   *
   * A hold never shortens an existing one.
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
   * Applies a broker-reported allowance and holds admission until it refills.
   *
   * This is the correction path for a rejection that *is* this client's own
   * fault: the configured budget was larger than the broker's, so the budget
   * shrinks and admission pauses. `refill` may be zero when the broker reported
   * no hint, in which case one interval is used.
   */
  void observe_limit(TimePoint now, int burst, int per_second, Duration refill) {
    resize(burst, per_second);
    cool_down(now, refill > Duration::zero() ? refill : interval_);
  }

  /** Returns how many requests of `allowance` burst could be issued right now. */
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

  /** Returns the configured burst allowance. */
  int burst() const noexcept { return burst_; }

  /** Returns the configured sustained rate in requests per second. */
  int per_second() const noexcept { return per_second_; }

  /** Returns the minimum spacing between two admitted requests. */
  Duration interval() const noexcept { return interval_; }

private:
  /** Returns how far ahead of now the schedule may run for `allowance`. */
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

/** Request pacing on the steady clock. */
using RateLimitBucket = BasicRateLimitBucket<>;

/** Counters and current pacing state for one traffic group. */
struct RateLimitGroupStats {
  std::uint64_t admitted_{0};
  std::uint64_t delayed_{0};
  std::uint64_t shed_{0};
  std::chrono::nanoseconds waited_{0};
};

/**
 * What the pacer has done, per group, plus what the broker last reported.
 *
 * A polling strategy needs this to know whether it is asking for more capacity
 * than it has, and it is how a human confirms the trading reserve is working.
 */
struct RateLimitStats {
  RateLimitGroupStats groups_[rate_limit_group_count];
  int burst_{0};
  int per_second_{0};
  int available_{0};
  std::uint64_t broker_rejections_{0};

  /** Returns the counters for one group. */
  RateLimitGroupStats& group(RateLimitGroup value) {
    return groups_[rate_limit_group_index(value)];
  }

  /** Returns the counters for one group. */
  const RateLimitGroupStats& group(RateLimitGroup value) const {
    return groups_[rate_limit_group_index(value)];
  }
};

/**
 * Paces one broker connection and records what it did.
 *
 * The controller is the stateful, real-time half of the pacer: it owns the
 * schedule for one application key, reads the steady clock, and keeps the
 * per-group counters. All pacing arithmetic stays in `RateLimitBucket`, which
 * takes its instants as arguments and is therefore tested exactly; the controller
 * is exercised end to end against the local TLS fixture instead.
 *
 * It is not synchronized. Every caller is the owning transport's strand, which is
 * also what serializes the requests being paced.
 */
class RateLimitController {
public:
  /** Duration used for all pacing arithmetic. */
  using Duration = RateLimitBucket::Duration;

  /** Constructs a controller for one application key's allowance. */
  explicit RateLimitController(RateLimitBudget budget = {})
  : budget_(budget), bucket_(budget) {
    stats_.burst_ = budget.burst_;
    stats_.per_second_ = budget.per_second_;
  }

  /** Returns how long a request of `group` must wait, zero if it may go now. */
  Duration delay_until(RateLimitGroup group) const {
    return bucket_.delay_until(RateLimitBucket::ClockType::now(), usable_burst(budget_, group));
  }

  /**
   * Admits one request of `group`, advancing the schedule.
   *
   * Returns false when the request must still wait, in which case nothing is
   * consumed and nothing is recorded as admitted.
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

  /** Records that a request of `group` had to wait `waited`. */
  void record_delay(RateLimitGroup group, Duration waited) {
    auto& counters = stats_.group(group);
    ++counters.delayed_;
    counters.waited_ += waited;
  }

  /** Records that a request of `group` was refused admission locally. */
  void record_shed(RateLimitGroup group) { ++stats_.group(group).shed_; }

  /**
   * Holds admission for `duration` without changing the configured allowance.
   *
   * For a broker refusal this client's own pacing could not have prevented,
   * where the capacity is shared with other customers and the remedy is to wait.
   * Shrinking the budget here would be wrong: the allowance was never the
   * problem, so the client would pace itself slower for no benefit.
   *
   * A refusal is counted globally rather than per group. At the point a broker
   * refusal is recognized the request's group is no longer in hand, and
   * inventing an attribution would be worse than not reporting one; the per-group
   * counters that matter for pacing — admitted, delayed, and shed — are recorded
   * where the group *is* known.
   */
  void hold(Duration duration) {
    ++stats_.broker_rejections_;
    bucket_.cool_down(RateLimitBucket::ClockType::now(), duration);
  }

  /**
   * Shrinks the allowance to what the broker reported and then holds.
   *
   * Only ever called with a number a broker actually stated. This module applies
   * allowances; it does not invent them. A broker that reports no allowance is
   * handled by `hold()`, and deciding what to do about a budget that keeps being
   * refused belongs to whoever configured it, not to the transport.
   */
  void shrink(int burst, int per_second, Duration retry) {
    ++stats_.broker_rejections_;
    apply_budget(burst, per_second);
    bucket_.observe_limit(RateLimitBucket::ClockType::now(), burst, per_second, retry);
  }

  /** Returns the allowance currently in force. */
  const RateLimitBudget& budget() const noexcept { return budget_; }

  /** Returns what the pacer has admitted, delayed, shed, and been refused. */
  const RateLimitStats& stats() const noexcept { return stats_; }

private:
  /** Records a new allowance, keeping the reserve smaller than the burst. */
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
