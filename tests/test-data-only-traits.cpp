#include "data_only_vendor.hpp"

#include <algotradelib/broker>
#include <algotradelib/live>
#include <algotradelib/runner>

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <optional>

// The check an out-of-tree adapter asserts once: members found through the base count.
ATL_ASSERT_BROKER(vendor::BlockingClient);
ATL_ASSERT_BROKER(vendor::AsyncClient);

static_assert(!BrokerCaps<vendor::BlockingClient>::has_working_orders, "a data vendor lists no orders");
static_assert(BrokerCaps<vendor::BlockingClient>::data_only, "the base marks the client as a data source");
static_assert(BrokerCaps<vendor::AsyncClient>::data_only, "in either shape");
static_assert(!BrokerCaps<vendor::TradingStandIn>::data_only, "a declaration in the specialization hides the base's");
static_assert(!strategy::NoCaps::data_only, "the library's own capability sets state the flag");
static_assert(!strategy::AllCaps::data_only, "a source that takes no order is not \"everything\"");
static_assert(BrokerCaps<vendor::BlockingClient>::sellable_source == SellableSource::NONE, "nor holdings");
// Neither question is asked of a vendor, which answers neither, and both read as the answer that cannot mislead.
static_assert(!BrokerCaps<vendor::BlockingClient>::working_orders_state_terminal, "no listing, no terminal rows");
static_assert(
  BrokerCaps<vendor::BlockingClient>::sellable_inquiry_basis.coverage_ == SellableBasis::Coverage::UNESTABLISHED,
  "no inquiry, no basis"
);

namespace {

const InstrumentId kInstrument{Country::US, Venue::UNSPECIFIED, "AAPL"};

/** Checks an error is the data-only refusal, which the runner reads as nothing sent. */
void check_refused_locally(const BrokerError& error) {
  CHECK_FALSE(error.reached_broker_);
  CHECK(error.category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK(error.broker_code_ == "data_only");
  CHECK(runner::outcome_of(error) == runner::SendOutcome::REFUSED_LOCALLY);
}

/** A strategy that only counts what it is fed. */
struct CountsBars {
  /** Bars fed so far. */
  std::size_t bars_seen_{0};

  /** Counts a bar. */
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) { ++bars_seen_; }
};

/** A handler that cannot be copied, to show a refusal moves its handler rather than copying it. */
struct MoveOnlyRecorder {
  /** Where the completion lands. */
  std::shared_ptr<std::optional<BrokerError>> seen_;

  MoveOnlyRecorder(std::shared_ptr<std::optional<BrokerError>> seen) : seen_(std::move(seen)) {}
  MoveOnlyRecorder(MoveOnlyRecorder&&) = default;
  MoveOnlyRecorder& operator=(MoveOnlyRecorder&&) = default;
  MoveOnlyRecorder(const MoveOnlyRecorder&) = delete;
  MoveOnlyRecorder& operator=(const MoveOnlyRecorder&) = delete;

  /** Records the error a refusal completes with. */
  template <typename T>
  void operator()(Result<T> result) {
    REQUIRE_FALSE(result);
    *seen_ = result.error();
  }
};

} // namespace

TEST_CASE("a blocking data-only session serves bars and quotes", "[data-only]") {
  auto client = std::make_shared<vendor::BlockingClient>();
  BlockingSession<vendor::BlockingClient> session(client, data_only::NoAccount{});
  BarQuery query;
  query.instrument_ = kInstrument;
  auto bars = session.bars(query);
  REQUIRE(bars);
  CHECK(bars.value().bars_.size() == 3);
  CHECK(client->bar_calls_ == 1);
  CHECK(session.quote(kInstrument).value().last_price_ == Decimal64::parse("100.5"));
  CHECK(session.rate_limit_stats() == nullptr);
  query.interval_ = BarInterval::ONE_MINUTE;
  check_refused_locally(session.bars(query).error());
  CHECK(client->bar_calls_ == 1);
}

TEST_CASE("every blocking order verb refuses locally", "[data-only]") {
  BlockingSession<vendor::BlockingClient> session(
    std::make_shared<vendor::BlockingClient>(), data_only::NoAccount{}
  );
  OrderRequest order;
  order.instrument_ = kInstrument;
  order.side_ = OrderSide::BUY;
  order.quantity_ = Decimal64::parse("1");
  const BlockingSession<vendor::BlockingClient>::Ref ref;
  check_refused_locally(session.place(order).error());
  check_refused_locally(session.replace(ref, AmendInstruction{}).error());
  check_refused_locally(session.cancel(ref, AmendInstruction{}).error());
  check_refused_locally(session.holdings().error());
  check_refused_locally(session.order_history(OrderHistoryQuery{}).error());
}

TEST_CASE("every raw inherited thunk refuses when called directly", "[data-only]") {
  // A session's validators and converters refuse first, so the raw thunks are never reached through
  // one; a caller that skips the session must get the same answer.
  using Traits = BrokerTraits<vendor::BlockingClient>;
  vendor::BlockingClient client;
  data_only::NoAccount account;
  check_refused_locally(Traits::holdings(client, account).error());
  check_refused_locally(Traits::place_order(client, account, OrderRequest{}).error());
  check_refused_locally(Traits::replace_order(client, account, data_only::NoAmendment{}).error());
  check_refused_locally(Traits::cancel_order(client, account, data_only::NoAmendment{}).error());
  check_refused_locally(Traits::order_history(client, account, data_only::NoHistoryQuery{}).error());
  check_refused_locally(*Traits::validate_place(OrderRequest{}));
  check_refused_locally(Traits::to_replacement(OrderRef<vendor::Tag>{}, AmendInstruction{}).error());
  check_refused_locally(Traits::to_cancellation(OrderRef<vendor::Tag>{}, AmendInstruction{}).error());
  check_refused_locally(Traits::to_history_query(account, OrderHistoryQuery{}).error());
  // An acknowledgement cannot exist; one passed in anyway is refused, not normalized into an order.
  check_refused_locally(Traits::normalize_place(Result<data_only::NoAck>(data_only::NoAck{})).error());
  check_refused_locally(Traits::normalize_place(Result<data_only::NoAck>(data_only::refusal())).error());
}

TEST_CASE("an asynchronous raw refusal completes once and after the call returns", "[data-only]") {
  using Traits = BrokerTraits<vendor::AsyncClient>;
  net::io_context io;
  vendor::AsyncClient client(io.get_executor());
  data_only::NoAccount account;
  int completions = 0;
  std::optional<BrokerError> seen;
  const auto record = [&](auto result) {
    ++completions;
    REQUIRE_FALSE(result);
    seen = result.error();
  };
  Traits::holdings(client, account, net::bind_executor(io.get_executor(), record));
  Traits::place_order(client, account, OrderRequest{}, net::bind_executor(io.get_executor(), record));
  Traits::replace_order(client, account, data_only::NoAmendment{}, net::bind_executor(io.get_executor(), record));
  Traits::cancel_order(client, account, data_only::NoAmendment{}, net::bind_executor(io.get_executor(), record));
  Traits::order_history(client, account, data_only::NoHistoryQuery{}, net::bind_executor(io.get_executor(), record));
  // Posted, so nothing has completed inside the calls.
  CHECK(completions == 0);
  io.run();
  CHECK(completions == 5);
  REQUIRE(seen.has_value());
  check_refused_locally(*seen);

  // A move-only handler is moved once, not copied.
  io.restart();
  auto moved = std::make_shared<std::optional<BrokerError>>();
  Traits::holdings(client, account, net::bind_executor(io.get_executor(), MoveOnlyRecorder(moved)));
  io.run();
  REQUIRE(moved->has_value());
  check_refused_locally(**moved);
}

TEST_CASE("every asynchronous session order verb refuses locally", "[data-only]") {
  net::io_context io;
  BrokerSession<vendor::AsyncClient> session(
    std::make_shared<vendor::AsyncClient>(io.get_executor()), data_only::NoAccount{}
  );
  const BrokerSession<vendor::AsyncClient>::Ref ref;
  std::vector<BrokerError> errors;
  const auto record = [&](auto result) {
    REQUIRE_FALSE(result);
    errors.push_back(result.error());
  };
  // Bound to the io_context, so every completion runs inside `run()`: a callback naming no executor
  // completes on the system executor's threads instead.
  const auto on_io = [&] { return net::bind_executor(io.get_executor(), record); };
  session.async_place(OrderRequest{}, on_io());
  session.async_replace(ref, AmendInstruction{}, on_io());
  session.async_cancel(ref, AmendInstruction{}, on_io());
  session.async_holdings(on_io());
  session.async_order_history(OrderHistoryQuery{}, on_io());
  CHECK(errors.empty());
  io.run();
  REQUIRE(errors.size() == 5);
  for (const auto& error : errors) {
    check_refused_locally(error);
  }

  // Bars through the same session, and a refusal through a future.
  io.restart();
  BarQuery query;
  query.instrument_ = kInstrument;
  auto bars = session.async_bars(query, net::use_future);
  auto holdings = session.async_holdings(net::use_future);
  io.run();
  CHECK(bars.get().value().bars_.size() == 3);
  check_refused_locally(holdings.get().error());
}

TEST_CASE("a data-only session serves a source whose trading session is another broker", "[data-only]") {
  // The use the helper is for: the account through one session, bars from the vendor. The trading side is
  // a stand-in that declares itself not data-only (`TradingStandIn`, below); only its type matters, and
  // the bars must come from the `MarketData` session.
  using Trading = BlockingSession<vendor::TradingStandIn>;
  using Data = BlockingSession<vendor::BlockingClient>;
  auto trading_client = std::make_shared<vendor::TradingStandIn>();
  auto data_client = std::make_shared<vendor::BlockingClient>();
  Trading trading(trading_client, data_only::NoAccount{});
  Data data(data_client, data_only::NoAccount{});
  live::SessionSource<Trading, runner::MemoryJournal, Data> source(trading, data, {kInstrument});
  source.set_bar_query(BarInterval::ONE_DAY, 10);
  auto observing = live::bind_observer(trading, CountsBars{});
  CHECK(source.poll(runner::PollKind::BARS, observing) == runner::PollOutcome::UPDATED);
  CHECK(observing.strategy().bars_seen_ == 3);
  CHECK(data_client->bar_calls_ == 1);
  CHECK(trading_client->bar_calls_ == 0);
}

TEST_CASE("a refusal to a callback naming no executor still completes once", "[data-only]") {
  // `BrokerSession` binds the sink it gives a thunk to the callback's associated executor: the system
  // executor before Asio 1.37 (Boost 1.90) and the inline executor from it, which cannot take posted work.
  // Either way the refusal completes, once, and not inside the initiating call.
  net::io_context io;
  BrokerSession<vendor::AsyncClient> session(
    std::make_shared<vendor::AsyncClient>(io.get_executor()), data_only::NoAccount{}
  );
  auto done = std::make_shared<std::promise<BrokerError>>();
  auto future = done->get_future();
  auto calls = std::make_shared<std::atomic<int>>(0);
  session.async_holdings([done, calls](Result<HoldingsSnapshot> result) {
    if (calls->fetch_add(1) == 0) {
      done->set_value(result.error());
    }
  });
  REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  check_refused_locally(future.get());
  CHECK(calls->load() == 1);
}

TEST_CASE("a refusal through an erased strand executor completes on that strand", "[data-only]") {
  // An `any_completion_executor` cannot be posted to even when it wraps a strand, so the refusal takes the
  // fallback path; the strand must still be where it completes, or a callback races state the caller
  // confined to it.
  net::io_context io;
  auto guard = net::make_work_guard(io);
  auto strand = net::make_strand(io);
  const net::any_completion_executor erased(strand);
  vendor::AsyncClient client(io.get_executor());
  data_only::NoAccount account;
  BrokerSession<vendor::AsyncClient> session(
    std::make_shared<vendor::AsyncClient>(io.get_executor()), data_only::NoAccount{}
  );
  int completions = 0;
  bool on_strand = true;
  const auto record = [&](auto result) {
    ++completions;
    on_strand = on_strand && strand.running_in_this_thread();
    CHECK_FALSE(result);
  };
  BrokerTraits<vendor::AsyncClient>::holdings(client, account, net::bind_executor(erased, record));
  session.async_holdings(net::bind_executor(erased, record));
  auto moved = std::make_shared<std::optional<BrokerError>>();
  BrokerTraits<vendor::AsyncClient>::holdings(
    client, account, net::bind_executor(erased, MoveOnlyRecorder(moved))
  );
  // Only this thread runs the io_context, so nothing bound to the strand can have run yet.
  CHECK(completions == 0);
  CHECK_FALSE(moved->has_value());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((completions < 2 || !moved->has_value()) && std::chrono::steady_clock::now() < deadline) {
    io.run_one_for(std::chrono::milliseconds(100));
  }
  CHECK(completions == 2);
  CHECK(on_strand);
  REQUIRE(moved->has_value());
  check_refused_locally(**moved);
}
