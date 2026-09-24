#include <algotradelib/kis>
#include <algotradelib/tossinvest>
#include <algotradelib/src/network/rate_limit.hpp>
#include <algotradelib/src/runner/execution.hpp>

#include "local_https_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

  // This key's own per-second allowance. Documented in KIS's example repository
  // rather than in the API workbook, which carries only the ledger code.
  REQUIRE(kis_rate_limit_layer("EGW00201") == KisRateLimitLayer::PERSONAL);

  // The ledger's per-endpoint cap, shared with every other customer.
  REQUIRE(kis_rate_limit_layer("EGW00215") == KisRateLimitLayer::LEDGER);

  // A scheduled trading-control window is not congestion and must not be waited out
  // as though it were.
  REQUIRE(kis_rate_limit_layer("EGW00317") == KisRateLimitLayer::NONE);

  // An ordinary declared failure, and an absent code, stay unclassified.
  REQUIRE(kis_rate_limit_layer("APBK0013") == KisRateLimitLayer::NONE);
  REQUIRE(kis_rate_limit_layer("") == KisRateLimitLayer::NONE);
}


TEST_CASE(
  "a trading request is served before a queued polling backlog",
  "[rate-limit][tls][priority]"
) {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  std::vector<std::string> bodies;
  const auto send = [&](const std::string& target, RateLimitGroup group) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = group;
    client->async_request(std::move(request), [&bodies](Result<network::HttpResponse> result) {
      REQUIRE(result);
      bodies.push_back(result.value().body());
    });
  };

  // A polling loop fills the queue, and only then does a cancellation arrive.
  send("/poll-1", RateLimitGroup::MARKET_DATA);
  send("/poll-2", RateLimitGroup::MARKET_DATA);
  send("/poll-3", RateLimitGroup::MARKET_DATA);
  send("/cancel", RateLimitGroup::TRADING);
  send("/poll-4", RateLimitGroup::MARKET_DATA);

  ioc.run();

  REQUIRE(bodies.size() == 5);
  REQUIRE(bodies.front() == "/cancel");

  // Equal priority keeps arrival order, so the backlog is otherwise untouched.
  REQUIRE(
    bodies == std::vector<std::string>{"/cancel", "/poll-1", "/poll-2", "/poll-3", "/poll-4"}
  );
  REQUIRE(server.connectionCount() == 1);
}

TEST_CASE(
  "an unpaced transport keeps strict arrival order",
  "[rate-limit][tls][priority]"
) {
  // One traffic class must behave exactly as the FIFO queue did before priority
  // existed, which is what keeps every merged transport guarantee intact.
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  std::vector<std::string> bodies;
  for (const std::string target : {"/a", "/b", "/c"}) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = RateLimitGroup::MARKET_DATA;
    client->async_request(std::move(request), [&bodies](Result<network::HttpResponse> result) {
      REQUIRE(result);
      bodies.push_back(result.value().body());
    });
  }

  ioc.run();
  REQUIRE(bodies == std::vector<std::string>{"/a", "/b", "/c"});
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

  // Two of the three had to wait a full interval each.
  REQUIRE(elapsed >= std::chrono::milliseconds(40));

  const auto* stats = client->rate_limits() ? &client->rate_limits()->stats() : nullptr;
  REQUIRE(stats != nullptr);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).admitted_ == 3);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).delayed_ == 2);
  REQUIRE(stats->group(RateLimitGroup::MARKET_DATA).waited_ > std::chrono::nanoseconds::zero());
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

TEST_CASE("an unspecified budget leaves the transport entirely unpaced") {
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);

  // No budget: not every API is metered, and one that is not must not be paced.
  network::HttpClientOptions options;
  REQUIRE_FALSE(options.rate_limit_.has_value());
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );

  REQUIRE(client->rate_limits() == nullptr);

  int completed = 0;
  for (const std::string target : {"/a", "/b", "/c", "/d", "/e", "/f"}) {
    network::HttpRequest request;
    request.target_ = target;
    request.group_ = RateLimitGroup::MARKET_DATA;
    client->async_request(std::move(request), [&completed](Result<network::HttpResponse> result) {
      REQUIRE(result);
      ++completed;
    });
  }

  ioc.run();

  // Six requests that a budget would have spaced out go straight through.
  //
  // **Asserted by the absent controller above, not by a stopwatch.** An earlier version of this test
  // also required the whole run to finish inside 500ms, which measured the machine rather than the
  // library: `rate_limits() == nullptr` already means there is no code path that could pace, so the
  // timing line added no claim about this transport and one large claim about how busy the host is.
  // It failed on loaded machines and pointed the reader at the rate limiter, which was never what
  // had broken.
  //
  // The opposite direction is safe and is used in the paced test above: a *floor* on elapsed time
  // says the pacer really waited, and load can only make it truer.
  REQUIRE(completed == 6);
  REQUIRE(server.connectionCount() == 1);
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
  options.timeout_ = std::chrono::seconds(1);
  // One request per five seconds, so the second one cannot start inside a
  // one-second deadline however long it waits.
  options.rate_limit_ = RateLimitBudget{1, 1, 0};
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port(), options
  );
  client->rate_limits()->shrink(1, 1, std::chrono::seconds(5));

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
    ioc.get_executor(), context, "localhost", "1", network::HttpClientOptions{}
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

TEST_CASE("a GET retried after failing to connect still never reached anybody", "[http][tls]") {
  // A GET is replayed once after a transport failure, and the replay is what makes this worth
  // pinning: the retry flag counts an attempt, not a send. Both attempts here fail to *connect*,
  // so no byte ever left, and reading the retry as evidence would report an untouched broker as
  // one that may hold the request.
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", "1", network::HttpClientOptions{}
  );

  std::optional<BrokerError> failure;
  network::HttpRequest request;
  request.method_ = http::verb::get;
  request.target_ = "/quotes";
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
  REQUIRE_FALSE(failure->reached_broker_);
}

TEST_CASE(
  "closing a client while it is still connecting cancels rather than crashes",
  "[http][tls][close]"
) {
  // Regression: `close()` completes every queued request and releases the stream,
  // but the resolve, connect, and handshake continuations advanced the connection
  // unconditionally, so one arriving after a close dereferenced a released stream.
  // A paced client makes this ordinary rather than rare: it can sit waiting for
  // admission for as long as its budget says, and a caller may close it meanwhile.
  test::LocalHttpsServer server;
  net::io_context ioc;
  ssl::context context(ssl::context::tls_client);
  test::trustLocalhostCertificate(context);
  auto client = std::make_shared<network::AsyncHttpClient>(
    ioc.get_executor(), context, "localhost", server.port()
  );

  std::vector<BrokerError> errors;
  int succeeded = 0;
  for (const std::string target : {"/one", "/two"}) {
    network::HttpRequest request;
    request.target_ = target;
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

  client->close();
  ioc.run();

  REQUIRE(succeeded == 0);
  REQUIRE(errors.size() == 2);
  for (const auto& error : errors) {
    REQUIRE(error.category_ == ErrorCategory::CANCELLED);
    // Closed before the loop ever ran, so neither request was on the wire, and a caller must be
    // able to tell: `CANCELLED` alone reads as `INDETERMINATE`, withholding an instrument and
    // owing a reconciliation for an order that was never sent. The one entry that *may* have been
    // in flight is the front, and `close()` reads that before disconnecting rather than after --
    // afterwards the state always reads "not in flight", which would claim an order on the wire
    // provably does not exist.
    REQUIRE_FALSE(error.reached_broker_);
  }
  REQUIRE(server.requestCount("/one") == 0);
}
