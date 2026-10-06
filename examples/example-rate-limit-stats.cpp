// Shows how a request budget is spent, and what the trading reserve protects.
//
// Entirely offline: it drives the pacer with its own clock and issues no broker
// request, so it needs no credentials and touches no network. The point is to
// make a budget's behaviour inspectable before a strategy depends on it.
//
//   example-rate-limit-stats [burst] [per-second] [trading-reserve]

#include <algotradelib/src/network/rate_limit.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

using algotradelib::network::RateLimitBudget;
using algotradelib::network::RateLimitGroup;
using algotradelib::network::rate_limit_group_name;
using algotradelib::network::usable_burst;

/** @brief Clock this example advances by hand, so the output is identical every run. */
struct WalkClock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<WalkClock, duration>;

  /** @brief Returns the epoch; the example supplies instants explicitly. */
  static time_point now() { return time_point{}; }
};

using Bucket = algotradelib::network::BasicRateLimitBucket<WalkClock>;

/** @brief Returns milliseconds as the pacer's duration type. */
std::chrono::nanoseconds ms(long long count) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(count));
}

/** @brief Returns a duration in milliseconds, for printing. */
long long as_ms(std::chrono::nanoseconds value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

/** @brief Reads one optional positive integer argument, or returns `fallback`. */
int argument(int argc, char** argv, int index, int fallback) {
  if (argc <= index) {
    return fallback;
  }
  const int value = std::atoi(argv[index]);
  return value >= 0 ? value : fallback;
}

/** @brief Prints how much of the burst each traffic class may spend. */
void print_allowances(const RateLimitBudget& budget) {
  std::cout << "budget: burst=" << budget.burst_ << " per_second=" << budget.per_second_
    << " trading_reserve=" << budget.trading_reserve_ << '\n';
  std::cout << "  interval between admissions: " << as_ms(Bucket(budget).interval()) << "ms\n";
  for (const auto group : {
    RateLimitGroup::AUTH,
    RateLimitGroup::TRADING,
    RateLimitGroup::ACCOUNT,
    RateLimitGroup::MARKET_DATA,
    RateLimitGroup::REFERENCE,
  }) {
    std::cout << "  " << std::left << std::setw(12) << rate_limit_group_name(group)
      << " burst allowance " << usable_burst(budget, group) << '\n';
  }
}

/** @brief Spends everything a polling loop is allowed, then shows what is left over. */
void print_reserve(const RateLimitBudget& budget) {
  Bucket bucket(budget);
  const auto now = WalkClock::now();
  const int polling = usable_burst(budget, RateLimitGroup::MARKET_DATA);
  const int trading = usable_burst(budget, RateLimitGroup::TRADING);

  int spent = 0;
  while (bucket.try_acquire(now, polling)) {
    ++spent;
  }

  std::cout << "\na saturated polling loop spends " << spent << " request(s) immediately\n";
  std::cout << "  its next request waits " << as_ms(bucket.delay_until(now, polling)) << "ms\n";
  std::cout << "  a cancellation waits " << as_ms(bucket.delay_until(now, trading)) << "ms";
  std::cout << " with " << bucket.available(now, trading) << " reserved slot(s) left\n";
}

/** @brief Shows what a broker refusal does to the budget, for each kind of refusal. */
void print_corrections(const RateLimitBudget& budget) {
  std::cout << "\nreactions to a broker refusal\n";

  algotradelib::network::RateLimitController shared(budget);
  shared.hold(std::chrono::seconds(1));
  std::cout << "  shared cap reached: allowance unchanged at " << shared.budget().per_second_
    << "/s, admission held\n";

  algotradelib::network::RateLimitController reported(budget);
  reported.shrink(2, 2, std::chrono::seconds(1));
  std::cout << "  broker reported its allowance: adopted " << reported.budget().per_second_
    << "/s\n";

  std::cout << "  broker reported no allowance: allowance unchanged, admission held\n";
  std::cout << "    (KIS states none, so its budget stays as configured and its"
    << " refusals are counted)\n";
}

/** @brief Walks a minute of polling to show the sustained rate the budget produces. */
void print_sustained(const RateLimitBudget& budget) {
  Bucket bucket(budget);
  const auto start = WalkClock::now();
  const int polling = usable_burst(budget, RateLimitGroup::MARKET_DATA);

  int admitted = 0;
  for (long long step = 0; step < 1000; step += 5) {
    if (bucket.try_acquire(start + ms(step), polling)) {
      ++admitted;
    }
  }
  std::cout << "\none second of polling as fast as allowed: " << admitted << " request(s)\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    RateLimitBudget budget;
    budget.burst_ = argument(argc, argv, 1, budget.burst_);
    budget.per_second_ = argument(argc, argv, 2, budget.per_second_);
    budget.trading_reserve_ = argument(argc, argv, 3, budget.trading_reserve_);

    print_allowances(budget);
    print_reserve(budget);
    print_sustained(budget);
    print_corrections(budget);
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
