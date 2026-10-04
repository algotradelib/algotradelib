#include <algotradelib/broker>
#include <algotradelib/live>
#include <algotradelib/runner>

#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <optional>

using namespace algotradelib;

namespace vendor {

/** A market-data vendor's identity, as an out-of-tree adapter declares one. */
struct Tag {
  /** Normalized name. */
  static constexpr std::string_view name = "vendor";
};

/** What the vendor needs to connect; nothing here. */
struct Config {};

/** One scripted series, as a vendor would answer any bar query. */
inline BarSeries scripted_bars(const InstrumentId& instrument) {
  BarSeries series;
  series.instrument_ = instrument;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::OPEN;
  for (int day = 0; day < 3; ++day) {
    Bar bar;
    bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (20000 + day));
    bar.open_ = Decimal64::parse("100");
    bar.high_ = Decimal64::parse("101");
    bar.low_ = Decimal64::parse("99");
    bar.close_ = Decimal64::parse("100.5");
    bar.volume_ = Decimal64::parse("1000");
    series.bars_.push_back(bar);
  }
  return series;
}

/** A blocking vendor client: bars and quotes, nothing about any account. */
struct BlockingClient {
  /** How many bar requests reached the client. */
  int bar_calls_{0};
};

/** An asynchronous vendor client: completes on its own executor, as a real client completes on its strand. */
class AsyncClient {
public:
  /** Constructs a client whose completions run on `executor`. */
  explicit AsyncClient(net::any_io_executor executor) : executor_(std::move(executor)) {}

  /** Answers a bar request with the scripted series. */
  template <typename Token>
  auto async_bars(BarQuery query, Token&& token) {
    return net::async_initiate<Token, void(Result<BarSeries>)>(
      [this, query = std::move(query)](auto&& handler) mutable {
        net::post(executor_, [handler = std::forward<decltype(handler)>(handler), query]() mutable {
          std::move(handler)(Result<BarSeries>(scripted_bars(query.instrument_)));
        });
      },
      token
    );
  }

  /** Answers a quote request. */
  template <typename Token>
  auto async_quote(InstrumentId instrument, Token&& token) {
    return net::async_initiate<Token, void(Result<QuoteSnapshot>)>(
      [this, instrument = std::move(instrument)](auto&& handler) mutable {
        net::post(executor_, [handler = std::forward<decltype(handler)>(handler), instrument]() mutable {
          QuoteSnapshot quote;
          quote.instrument_ = instrument;
          quote.last_price_ = Decimal64::parse("100.5");
          std::move(handler)(Result<QuoteSnapshot>(quote));
        });
      },
      token
    );
  }

private:
  net::any_io_executor executor_;
};

} // namespace vendor

/** The blocking vendor's traits: what it serves, and the account side from the base. */
template <>
struct algotradelib::BrokerTraits<vendor::BlockingClient> : algotradelib::DataOnlyTraits<vendor::Tag> {
  /** The adapted client. */
  using Client = vendor::BlockingClient;
  /** The vendor's configuration. */
  using Config = vendor::Config;
  /** Normalized name. */
  static constexpr std::string_view name = vendor::Tag::name;
  /** A blocking facade. */
  static constexpr bool blocking = true;

  /** Daily bars only. */
  static constexpr bool supports_interval(BarInterval interval) { return interval == BarInterval::ONE_DAY; }

  /** Refuses an interval it does not serve. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return data_only::refusal();
  }

  /** Answers a quote. */
  static Result<QuoteSnapshot> quote(Client&, InstrumentId instrument) {
    QuoteSnapshot quote;
    quote.instrument_ = std::move(instrument);
    quote.last_price_ = Decimal64::parse("100.5");
    return Result<QuoteSnapshot>(quote);
  }

  /** Answers bars. */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    ++client.bar_calls_;
    return Result<BarSeries>(vendor::scripted_bars(query.instrument_));
  }
};

/** The asynchronous vendor's traits. */
template <>
struct algotradelib::BrokerTraits<vendor::AsyncClient> : algotradelib::DataOnlyTraits<vendor::Tag> {
  /** The adapted client. */
  using Client = vendor::AsyncClient;
  /** The vendor's configuration. */
  using Config = vendor::Config;
  /** Normalized name. */
  static constexpr std::string_view name = vendor::Tag::name;
  /** Completes through a token. */
  static constexpr bool blocking = false;

  /** Daily bars only. */
  static constexpr bool supports_interval(BarInterval interval) { return interval == BarInterval::ONE_DAY; }

  /** Refuses an interval it does not serve. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return data_only::refusal();
  }

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a bar request. */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }
};

// The two checks an out-of-tree adapter asserts once: members found through the base count.
ATL_ASSERT_BROKER(vendor::BlockingClient);
ATL_ASSERT_BROKER(vendor::AsyncClient);
ATL_ASSERT_BROKER_VERBS(vendor::BlockingClient);
ATL_ASSERT_BROKER_VERBS(vendor::AsyncClient);

static_assert(!BrokerCaps<vendor::BlockingClient>::has_working_orders, "a data vendor lists no orders");
static_assert(BrokerCaps<vendor::BlockingClient>::sellable_source == SellableSource::NONE, "nor holdings");

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

TEST_CASE("an executor bound to a data-only session reports a local refusal", "[data-only]") {
  BlockingSession<vendor::BlockingClient> session(
    std::make_shared<vendor::BlockingClient>(), data_only::NoAccount{}
  );
  live::SessionExecutor<BlockingSession<vendor::BlockingClient>> executor(session);
  strategy::PlannedOrder planned;
  planned.instrument_ = kInstrument;
  planned.side_ = OrderSide::BUY;
  planned.quantity_ = Decimal64::parse("1");
  const auto sent = executor.place(planned);
  CHECK(sent == runner::SendOutcome::REFUSED_LOCALLY);
}

TEST_CASE("a data-only trading session keeps the startup gate and a recovered hold", "[data-only]") {
  // Bound as the trading session by mistake: the listing fails rather than reading as empty, so the gate
  // stays held, and a hold recovered from the journal is not released by it.
  using Session = BlockingSession<vendor::BlockingClient>;
  Session session(std::make_shared<vendor::BlockingClient>(), data_only::NoAccount{});
  runner::MemoryJournal journal;
  runner::JournalEntry entry;
  entry.id_ = "run-1";
  entry.action_ = runner::JournalAction::PLACE;
  entry.outcome_ = runner::JournalOutcome::PENDING;
  entry.recorded_at_ = std::chrono::system_clock::now() - std::chrono::minutes(5);
  entry.instrument_ = kInstrument;
  entry.side_ = OrderSide::BUY;
  entry.type_ = OrderType::LIMIT;
  entry.quantity_ = Decimal64::parse("1");
  entry.price_ = Decimal64::parse("100");
  REQUIRE(journal.append(entry));

  auto observing = live::bind_observer(session, CountsBars{});
  REQUIRE_FALSE(runner::recover_runner(observing, journal).clean());
  REQUIRE(observing.ledger().requires_reconcile(kInstrument));

  live::SessionSource<Session, runner::MemoryJournal> source(session, {kInstrument});
  source.set_journal(journal);
  CHECK(source.read_order_list(observing) == runner::PollOutcome::FAILED);
  CHECK(observing.ledger().requires_reconcile(kInstrument));
  CHECK(source.poll(runner::PollKind::HOLDINGS, observing) == runner::PollOutcome::FAILED);
}

TEST_CASE("a data-only session serves a source whose trading session is another broker", "[data-only]") {
  // The use the helper is for: the account through one session, bars from the vendor. The trading side
  // here is the vendor too, for want of a broker without a network; what matters is that the bars come
  // from the `MarketData` session.
  using Session = BlockingSession<vendor::BlockingClient>;
  auto trading_client = std::make_shared<vendor::BlockingClient>();
  auto data_client = std::make_shared<vendor::BlockingClient>();
  Session trading(trading_client, data_only::NoAccount{});
  Session data(data_client, data_only::NoAccount{});
  live::SessionSource<Session, runner::MemoryJournal, Session> source(trading, data, {kInstrument});
  source.set_bar_query(BarInterval::ONE_DAY, 10);
  auto observing = live::bind_observer(trading, CountsBars{});
  CHECK(source.poll(runner::PollKind::BARS, observing) == runner::PollOutcome::UPDATED);
  CHECK(observing.strategy().bars_seen_ == 3);
  CHECK(data_client->bar_calls_ == 1);
  CHECK(trading_client->bar_calls_ == 0);
}

TEST_CASE("a refusal to a callback naming no executor still completes once", "[data-only]") {
  // `BrokerSession` binds the sink it gives a thunk to the callback's associated executor: the system
  // executor before Boost 1.92 and the inline executor from it, which cannot take posted work. Either way
  // the refusal completes, once, off the initiating call.
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
