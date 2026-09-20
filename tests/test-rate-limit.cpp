#include <algotradelib/kis>
#include <algotradelib/src/network/rate_limit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace algotradelib;
using network::RateLimitBudget;
using network::RateLimitGroup;
using network::usable_burst;

namespace {

/**
 * Clock whose instants the test supplies directly.
 *
 * Pacing is a timing behaviour, so it is the one thing that must not be tested
 * against real time: a sleeping test is both slow and flaky. The bucket takes
 * every instant as an argument, so this clock never has to advance itself.
 */
struct TestClock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<TestClock, duration>;
  static constexpr bool is_steady = true;

  /** Returns the epoch; tests pass instants explicitly instead. */
  static time_point now() { return time_point{}; }
};

using Bucket = network::BasicRateLimitBucket<TestClock>;
using Instant = TestClock::time_point;

/** Returns the epoch instant these tests measure from. */
Instant start() { return Instant{}; }

/** Returns `count` milliseconds as the bucket's duration type. */
std::chrono::nanoseconds ms(int count) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(count));
}

} // namespace

TEST_CASE("a bucket admits a full burst from idle and then paces") {
  Bucket bucket(4, 10);  // four back to back, then one every 100ms
  const auto now = start();

  REQUIRE(bucket.interval() == ms(100));
  REQUIRE(bucket.available(now, 4) == 4);

  for (int issued = 0; issued < 4; ++issued) {
    REQUIRE(bucket.try_acquire(now, 4));
  }

  // The burst is spent, so the fifth request waits exactly one interval.
  REQUIRE_FALSE(bucket.try_acquire(now, 4));
  REQUIRE(bucket.delay_until(now, 4) == ms(100));
  REQUIRE(bucket.available(now, 4) == 0);

  // One interval later it is admitted, and only one is.
  REQUIRE(bucket.try_acquire(now + ms(100), 4));
  REQUIRE_FALSE(bucket.try_acquire(now + ms(100), 4));
  REQUIRE(bucket.delay_until(now + ms(100), 4) == ms(100));
}

TEST_CASE("a reported delay is exact, not approximate") {
  Bucket bucket(1, 4);  // one every 250ms
  const auto now = start();

  REQUIRE(bucket.try_acquire(now, 1));
  REQUIRE(bucket.delay_until(now, 1) == ms(250));

  // A moment before the deadline the request still waits the remainder.
  REQUIRE(bucket.delay_until(now + ms(249), 1) == ms(1));
  REQUIRE_FALSE(bucket.try_acquire(now + ms(249), 1));

  // Exactly at the deadline it is admitted.
  REQUIRE(bucket.try_acquire(now + ms(250), 1));
}

TEST_CASE("sustained admissions match the configured rate") {
  Bucket bucket(1, 5);  // one every 200ms
  auto now = start();

  int admitted = 0;
  for (int step = 0; step <= 1000; step += 50) {
    if (bucket.try_acquire(now + ms(step), 1)) {
      ++admitted;
    }
  }

  // One second at five per second, plus the one admitted from idle at t=0.
  REQUIRE(admitted == 6);
}

TEST_CASE("idle time restores the burst but never more than the burst") {
  Bucket bucket(3, 10);
  const auto now = start();

  REQUIRE(bucket.try_acquire(now, 3));
  REQUIRE(bucket.try_acquire(now, 3));
  REQUIRE(bucket.try_acquire(now, 3));
  REQUIRE_FALSE(bucket.try_acquire(now, 3));

  // A long idle period cannot bank more than one burst of credit.
  const auto later = now + std::chrono::hours(1);
  REQUIRE(bucket.available(later, 3) == 3);
  REQUIRE(bucket.try_acquire(later, 3));
  REQUIRE(bucket.try_acquire(later, 3));
  REQUIRE(bucket.try_acquire(later, 3));
  REQUIRE_FALSE(bucket.try_acquire(later, 3));
}

TEST_CASE("the trading reserve cannot be spent by other traffic") {
  const RateLimitBudget budget{8, 8, 2};
  REQUIRE(usable_burst(budget, RateLimitGroup::TRADING) == 8);
  REQUIRE(usable_burst(budget, RateLimitGroup::AUTH) == 8);
  REQUIRE(usable_burst(budget, RateLimitGroup::MARKET_DATA) == 6);
  REQUIRE(usable_burst(budget, RateLimitGroup::ACCOUNT) == 6);
  REQUIRE(usable_burst(budget, RateLimitGroup::REFERENCE) == 6);

  Bucket bucket(budget);
  const auto now = start();
  const int polling = usable_burst(budget, RateLimitGroup::MARKET_DATA);
  const int trading = usable_burst(budget, RateLimitGroup::TRADING);

  // A polling loop saturates everything it is allowed to spend.
  for (int issued = 0; issued < polling; ++issued) {
    REQUIRE(bucket.try_acquire(now, polling));
  }
  REQUIRE_FALSE(bucket.try_acquire(now, polling));

  // The reserve is still there, and only trading traffic can reach it.
  REQUIRE(bucket.available(now, trading) == 2);
  REQUIRE(bucket.try_acquire(now, trading));
  REQUIRE(bucket.try_acquire(now, trading));
  REQUIRE_FALSE(bucket.try_acquire(now, trading));
}

TEST_CASE("a reserve of zero paces without reserving") {
  const RateLimitBudget budget{4, 4, 0};
  REQUIRE(usable_burst(budget, RateLimitGroup::MARKET_DATA) == 4);
  REQUIRE(usable_burst(budget, RateLimitGroup::TRADING) == 4);

  Bucket bucket(budget);
  const auto now = start();
  for (int issued = 0; issued < 4; ++issued) {
    REQUIRE(bucket.try_acquire(now, 4));
  }
  REQUIRE_FALSE(bucket.try_acquire(now, 4));
}

TEST_CASE("a reserve larger than the burst still admits other traffic") {
  // A misconfigured reserve must not deadlock every non-trading request.
  const RateLimitBudget budget{2, 2, 5};
  REQUIRE(usable_burst(budget, RateLimitGroup::MARKET_DATA) == 1);

  Bucket bucket(budget);
  const auto now = start();
  REQUIRE(bucket.try_acquire(now, usable_burst(budget, RateLimitGroup::MARKET_DATA)));
}

TEST_CASE("a hold delays every group and favours trading while it lasts") {
  Bucket bucket(4, 10);
  const auto now = start();

  bucket.cool_down(now, ms(500));

  // Trading waits exactly the reported hold.
  REQUIRE(bucket.delay_until(now, 4) == ms(500));
  REQUIRE_FALSE(bucket.try_acquire(now, 4));
  REQUIRE(bucket.try_acquire(now + ms(500), 4));

  // A group spending less of the burst waits longer, by the slots it may not use.
  Bucket other(4, 10);
  other.cool_down(now, ms(500));
  REQUIRE(other.delay_until(now, 2) == ms(700));
}

TEST_CASE("a hold never shortens one already in force") {
  Bucket bucket(1, 10);
  const auto now = start();

  bucket.cool_down(now, ms(900));
  bucket.cool_down(now, ms(100));
  REQUIRE(bucket.delay_until(now, 1) == ms(900));
}

TEST_CASE("a broker-reported limit shrinks the budget and pauses admission") {
  Bucket bucket(20, 20);
  const auto now = start();
  REQUIRE(bucket.try_acquire(now, 20));

  // The broker says the real allowance is far smaller and to retry in a second.
  bucket.observe_limit(now, 2, 2, std::chrono::seconds(1));
  REQUIRE(bucket.burst() == 2);
  REQUIRE(bucket.per_second() == 2);
  REQUIRE(bucket.interval() == ms(500));
  REQUIRE(bucket.delay_until(now, 2) == ms(1000));
  REQUIRE(bucket.try_acquire(now + ms(1000), 2));
}

TEST_CASE("a reported limit with no retry hint pauses for one interval") {
  Bucket bucket(4, 10);
  const auto now = start();

  bucket.observe_limit(now, 4, 10, std::chrono::nanoseconds::zero());
  REQUIRE(bucket.delay_until(now, 4) == ms(100));
}

TEST_CASE("an invalid allowance is refused at construction") {
  REQUIRE_THROWS_AS(Bucket(0, 10), std::invalid_argument);
  REQUIRE_THROWS_AS(Bucket(4, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(Bucket(-1, 10), std::invalid_argument);

  Bucket bucket(4, 10);
  REQUIRE_THROWS_AS(bucket.resize(4, -5), std::invalid_argument);
}

TEST_CASE("group priority orders trading ahead of polling") {
  using network::rate_limit_priority;
  REQUIRE(rate_limit_priority(RateLimitGroup::AUTH) > rate_limit_priority(RateLimitGroup::TRADING));
  REQUIRE(
    rate_limit_priority(RateLimitGroup::TRADING) > rate_limit_priority(RateLimitGroup::ACCOUNT)
  );
  REQUIRE(
    rate_limit_priority(RateLimitGroup::ACCOUNT) > rate_limit_priority(RateLimitGroup::MARKET_DATA)
  );
  REQUIRE(
    rate_limit_priority(RateLimitGroup::MARKET_DATA) >
    rate_limit_priority(RateLimitGroup::REFERENCE)
  );
  REQUIRE(network::rate_limit_group_name(RateLimitGroup::MARKET_DATA) == "market_data");
}

TEST_CASE("the KIS rate-limit classification table covers both layers") {
  using kis::KisRateLimitLayer;
  using kis::kis_rate_limit_layer;

  // The ledger's own per-endpoint cap, named by code.
  REQUIRE(
    kis_rate_limit_layer("EGW00215", "원장에서 허용 가능한 초당 거래건수를 초과하였습니다.") ==
    KisRateLimitLayer::LEDGER
  );

  // The same shortage without the ledger's wording is this key's own allowance.
  REQUIRE(
    kis_rate_limit_layer("", "초당 거래건수를 초과하였습니다.") == KisRateLimitLayer::PERSONAL
  );

  // The ledger wording alone is enough even with no code.
  REQUIRE(
    kis_rate_limit_layer("", "원장에서 허용 가능한 초당 거래건수를 초과하였습니다.") ==
    KisRateLimitLayer::LEDGER
  );

  // A scheduled trading-control window is not congestion and must not back off.
  REQUIRE(
    kis_rate_limit_layer("EGW00317", "09:00 ~ 09:30 시간 동안에는 조회하실 수 없습니다.") ==
    KisRateLimitLayer::NONE
  );

  // An ordinary declared failure stays unclassified.
  REQUIRE(kis_rate_limit_layer("APBK0013", "주문 전송 완료 되었습니다.") == KisRateLimitLayer::NONE);
  REQUIRE(kis_rate_limit_layer("", "") == KisRateLimitLayer::NONE);
}
