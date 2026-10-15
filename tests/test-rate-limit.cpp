#include <algotradelib/tossinvest>
#include <algotradelib/src/network/rate_limit.hpp>
#include <algotradelib/src/runner/execution.hpp>

#include "local_https_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace algotradelib;
using network::RateLimitBudget;
using network::RateLimitGroup;
using network::usable_burst;
using network::RateLimitBudget;

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

TEST_CASE(
  "pacing delays requests past the burst and records the wait",
  "[rate-limit][tls][pacing]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  network::HttpClientOptions options;
  options.rate_limit_ = RateLimitBudget{1, 50, 0};  // one now, then one every 20ms
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  int completed = 0;
  for (const std::string target : {"/one", "/two", "/three"}) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = RateLimitGroup::MARKET_DATA;
    client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
      REQUIRE(result);
      ++completed;
    });
  }

  const auto began = std::chrono::steady_clock::now();
  ioc.run();
  const auto elapsed = std::chrono::steady_clock::now() - began;

  REQUIRE(completed == 3);

  // Two of the three were held back, counted by the timer that held them. Exactly two arms here
  // because all three requests are queued before the event loop runs, so nothing arrives to make
  // `pump()` re-evaluate and rearm mid-wait.
  REQUIRE(client->admission_waits() == 2);

  // And time really passed, which is a different claim from the counters below and the reason this
  // *floor* stays. `delayed_` and `waited_` are what the pacer decided; an admission delay computed
  // too short would still record two delays and satisfy them, and would not survive this line. Load
  // can only make a floor truer; an upper bound would fail on a loaded host.
  REQUIRE(elapsed >= std::chrono::milliseconds(40));

  const auto* stats = client->rate_limits() ? &client->rate_limits()->stats() : nullptr;
  REQUIRE(stats != nullptr);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).admitted_ == 3);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).delayed_ == 2);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).waited_ > std::chrono::nanoseconds::zero());
}

TEST_CASE(
  "a trading request admitted ahead of a waiting poll is not charged the poll's wait",
  "[rate-limit][tls][pacing]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  // A burst of two, one of it reserved for trading, one request a second: the first poll goes at once,
  // the second waits, and a trading request arriving meanwhile goes on the reserve.
  network::HttpClientOptions options;
  options.rate_limit_ = RateLimitBudget{2, 1, 1};
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );
  using SteadyClock = std::chrono::steady_clock;
  const auto send = [&client](const std::string& target, RateLimitGroup group, auto handler) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = group;
    client->async_request(std::move(request), std::move(handler));
  };

  // The trading request is arranged only once the scenario stands: the first poll has completed (so the
  // connection is up and no request is on the wire) and, 300ms later, the second is still waiting.
  net::steady_timer later(ioc);
  SteadyClock::time_point trading_sent{};
  SteadyClock::time_point second_done{};
  send("/first-poll", RateLimitGroup::MARKET_DATA, [&](Result<network::HttpResponse> result) {
    REQUIRE(result);
    later.expires_after(std::chrono::milliseconds(300));
    later.async_wait([&](beast::error_code ec) {
      REQUIRE_FALSE(ec);
      const auto& waiting = client->rate_limits()->stats().group(RateLimitGroup::MARKET_DATA);
      REQUIRE(waiting.admitted_ == 1);
      REQUIRE(client->admission_waits() >= 1);
      trading_sent = SteadyClock::now();
      send("/order", RateLimitGroup::TRADING, [](Result<network::HttpResponse> sent) { REQUIRE(sent); });
    });
  });
  send("/second-poll", RateLimitGroup::MARKET_DATA, [&](Result<network::HttpResponse> result) {
    REQUIRE(result);
    second_done = SteadyClock::now();
  });
  ioc.run();

  const auto* stats = client->rate_limits() ? &client->rate_limits()->stats() : nullptr;
  REQUIRE(stats != nullptr);
  const auto& trading = stats->group(RateLimitGroup::TRADING);
  const auto& polling = stats->group(RateLimitGroup::MARKET_DATA);
  REQUIRE(trading.admitted_ == 1);
  REQUIRE(polling.admitted_ == 2);
  // The reserve worked: the trading request did not wait, and is not recorded as having waited.
  CHECK(trading.delayed_ == 0);
  CHECK(trading.waited_ == std::chrono::nanoseconds::zero());
  // The poll waited once, from just after the first poll completed. It completes after it is admitted, so
  // `second_done - trading_sent` exceeds any wait restarted when the trading request left; a wait counted
  // from its start, about 300ms before that send, exceeds it by 300ms less the poll's round trip.
  CHECK(polling.delayed_ == 1);
  CHECK(polling.waited_ > second_done - trading_sent);
}

TEST_CASE(
  "a rate-limited response is surfaced with its hints and never retried",
  "[rate-limit][tls][retry]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  network::HttpClientOptions options;
  options.rate_limit_ = RateLimitBudget{8, 8, 2};
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  // An order POST is the case that matters: a refused order must not be replayed.
  network::HttpRequest request;
  request.method_ = http::verb::post;
  request.target_ = "/rate-limited";
  request.body_ = "{}";
  request.group_ = RateLimitGroup::TRADING;

  std::optional<network::HttpResponse> response;
  client->async_request(std::move(request), [&response](Result<network::HttpResponse> result) {
    REQUIRE(result);
    response = std::move(result.value());
  });
  ioc.run();

  REQUIRE(response.has_value());
  REQUIRE(response->result_int() == 429);
  REQUIRE(server.requestCount("/rate-limited") == 1);

  // The transport surfaces the refusal and says only what HTTP says; which headers
  // carry meaning is the broker's contract, so the broker reads them.
  auto error = broker_detail::http_error(*response);
  REQUIRE(error.category_ == ErrorCategory::RATE_LIMIT);
  REQUIRE_FALSE(error.retry_after_.has_value());

  const auto delay = tossinvest::retry_after(*response);
  REQUIRE(delay.has_value());
  REQUIRE(*delay == std::chrono::seconds(1));

  const auto allowance = tossinvest::reported_rate_limit(*response);
  REQUIRE(allowance.has_value());
  REQUIRE(*allowance == 3);
}

TEST_CASE("a stated allowance that cannot be adopted leaves the budget in force") {
  network::RateLimitController controller(RateLimitBudget{8, 8, 2});
  REQUIRE_THROWS_AS(controller.shrink(0, 3, std::chrono::milliseconds(1)), std::invalid_argument);
  REQUIRE_THROWS_AS(controller.shrink(3, 0, std::chrono::milliseconds(1)), std::invalid_argument);
  // What `budget()` and `stats()` report is still what the bucket enforces.
  CHECK(controller.budget().burst_ == 8);
  CHECK(controller.budget().per_second_ == 8);
  CHECK(controller.budget().trading_reserve_ == 2);
  CHECK(controller.stats().burst_ == 8);
  CHECK(controller.stats().broker_rejections_ == 0);
}

TEST_CASE("a stated allowance is adopted and an unstated one is waited out") {
  network::RateLimitController controller(RateLimitBudget{8, 8, 2});
  REQUIRE(controller.budget().per_second_ == 8);
  REQUIRE(controller.stats().broker_rejections_ == 0);

  // A broker that states its allowance is believed over the configured one.
  controller.shrink(3, 3, std::chrono::milliseconds(1));
  REQUIRE(controller.budget().burst_ == 3);
  REQUIRE(controller.budget().per_second_ == 3);
  REQUIRE(controller.budget().trading_reserve_ == 2);
  REQUIRE(controller.stats().broker_rejections_ == 1);

  // A stated allowance below the reserve keeps the reserve usable.
  controller.shrink(1, 1, std::chrono::milliseconds(1));
  REQUIRE(controller.budget().burst_ == 1);
  REQUIRE(controller.budget().trading_reserve_ == 0);
  REQUIRE(usable_burst(controller.budget(), RateLimitGroup::MARKET_DATA) == 1);

  // A refusal that states nothing is waited out, and the configured budget stands.
  // Reducing a stated budget from a guess would be a silent downgrade.
  network::RateLimitController shared(RateLimitBudget{8, 8, 2});
  shared.hold(std::chrono::milliseconds(1));
  REQUIRE(shared.budget().per_second_ == 8);
  REQUIRE(shared.budget().burst_ == 8);
  REQUIRE(shared.stats().broker_rejections_ == 1);
}

TEST_CASE(
  "a request that would wait past its deadline fails before the wire",
  "[rate-limit][tls][deadline]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  network::HttpClientOptions options;
  // Long enough to connect wherever "localhost" is tried first. On GitHub's Windows runner a one-second
  // timeout failed while connecting, consistent with `::1` (where the server does not listen) being tried
  // first.
  options.timeout_ = std::chrono::seconds(10);
  // The broker's one-minute hold below starts now, so neither request can start inside a
  // ten-second deadline.
  options.rate_limit_ = RateLimitBudget{1, 1, 0};
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );
  client->rate_limits()->shrink(1, 1, std::chrono::seconds(60));

  std::vector<BrokerError> errors;
  int succeeded = 0;
  for (const std::string target : {"/first", "/second"}) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = RateLimitGroup::MARKET_DATA;
    client->async_request(
      std::move(request),
      [&errors, &succeeded](Result<network::HttpResponse> result) {
        if (result) {
          ++succeeded;
        } else {
          errors.push_back(result.error());
        }
      }
    );
  }

  ioc.run();

  // Neither reached the wire: the hold is in force from the first evaluation.
  REQUIRE(succeeded == 0);
  REQUIRE(errors.size() == 2);
  for (const auto& error : errors) {
    REQUIRE(error.category_ == ErrorCategory::CANCELLED);
    REQUIRE(error.broker_code_ == "admission_deadline");
    // And the error says the request never went, which is what stops a runner withholding the
    // instrument to reconcile an order that was dropped before it was sent. `CANCELLED` alone maps
    // to `INDETERMINATE`, which would be the wrong answer here in both directions.
    REQUIRE_FALSE(error.reached_broker_);
  }
  REQUIRE(server.requestCount("/first") == 0);
  REQUIRE(client->rate_limits()->stats().group(RateLimitGroup::MARKET_DATA).shed_ == 2);
}

TEST_CASE("a full queue sheds with a structured error rather than growing") {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  network::HttpClientOptions options;
  options.queue_limit_ = 2;
  options.rate_limit_ = RateLimitBudget{1, 1, 0};
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  int shed = 0;
  for (const std::string target : {"/a", "/b", "/c", "/d"}) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = RateLimitGroup::REFERENCE;
    client->async_request(std::move(request), [&shed](Result<network::HttpResponse> result) {
      if (!result && result.error().broker_code_ == "queue_full") {
        // Refused at admission, so nothing was sent: `TRANSPORT` would otherwise read as a request
        // that may be on the wire.
        REQUIRE_FALSE(result.error().reached_broker_);
        ++shed;
      }
    });
  }

  client->close();
  ioc.run();

  REQUIRE(shed == 2);
  REQUIRE(client->rate_limits()->stats().group(RateLimitGroup::REFERENCE).shed_ == 2);
}

TEST_CASE("a connection that never opened never sent the order", "[http][tls][provenance]") {
  // A resolve, connect, or handshake failure happens before `start_write()`, so an order has
  // certainly not been sent. `TRANSPORT` alone maps to `INDETERMINATE`: the runner would spend the
  // order's rate-limit slot and withhold the instrument to reconcile an order that does not exist.
  //
  // Deterministic without a server: a port nothing is listening on cannot complete a connection,
  // and the failure therefore cannot happen after a write.
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "127.0.0.1", test::closed_loopback_port(),
    network::HttpClientOptions{}
  );

  std::optional<BrokerError> failure;
  network::HttpRequest request;
  request.method_ = http::verb::post;
  request.target_ = "/orders";
  request.group_ = RateLimitGroup::TRADING;
  client->async_request(
    std::move(request),
    [&failure](Result<network::HttpResponse> result) {
      if (!result) {
        failure = result.error();
      }
    }
  );

  ioc.run();
  client->close();

  REQUIRE(failure);
  // A POST is never replayed, so this is the first and only attempt, and it never left.
  REQUIRE_FALSE(failure->reached_broker_);
  REQUIRE(runner::outcome_of(*failure) == runner::SendOutcome::REFUSED_LOCALLY);
}
