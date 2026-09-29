#include <algotradelib/kis>
#include <algotradelib/kiwoom>
#include <algotradelib/tossinvest>

#include "mock_broker.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace {

/** A completion handler naming no executor, which is what a plain callback is. */
struct PlainCallback {
  /** Accepts and discards any completion argument. */
  template <typename T>
  void operator()(T) const {}
};

/** Returns the instrument used throughout these probes. */
InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/**
 * Calls every verb an asynchronous session's capabilities claim, with `token`.
 *
 * Never run. It is explicitly instantiated below, which compiles every call it makes, so a claimed
 * capability whose verb does not compile for this client and this token shape fails the build of
 * this file. That is the whole test: a verb inside an `if constexpr` of a member template is never
 * compiled until something calls it, and before this file nothing called most of them.
 */
template <typename Client, typename Token>
void call_every_claimed_verb(
  BrokerSession<Client>& session, typename BrokerSession<Client>::Ref ref, Token token
) {
  using Caps = BrokerCaps<Client>;
  session.async_quote(samsung(), token);
  session.async_holdings(token);
  session.async_bars(BarQuery{}, token);
  session.async_place(OrderRequest{}, token);
  session.async_replace(ref, AmendInstruction{}, token);
  session.async_cancel(ref, AmendInstruction{}, token);
  session.async_order_history(OrderHistoryQuery{}, token);
  if constexpr (Caps::has_buying_power) {
    session.async_buying_power(Currency{}, token);
  }
  if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
    session.async_sellable_quantity(samsung(), token);
  }
  if constexpr (Caps::has_commission_schedule) {
    session.async_commissions(token);
  }
  if constexpr (Caps::has_working_orders) {
    session.async_working_orders(token);
  }
  if constexpr (Caps::has_reference_data) {
    session.async_instrument(samsung(), token);
  }
  if constexpr (Caps::has_calendar) {
    session.async_calendar(Country::KR, std::nullopt, token);
  }
  if constexpr (Caps::has_price_limits) {
    session.async_price_limits(samsung(), token);
  }
  if constexpr (Caps::has_trade_prints) {
    session.async_trades(samsung(), 1, token);
  }
  if constexpr (Caps::has_batch_prices) {
    session.async_prices(std::vector<InstrumentId>{samsung()}, token);
  }
  if constexpr (Caps::has_depth) {
    session.async_depth(samsung(), token);
  }
  if constexpr (Caps::has_order_by_id) {
    session.async_order(ref, token);
  }
  if constexpr (Caps::has_account_discovery) {
    async_accounts(session.client(), token);
  }
}

/** The blocking counterpart: every verb a blocking session's capabilities claim. */
template <typename Client>
void call_every_claimed_blocking_verb(
  BlockingSession<Client>& session, const typename BlockingSession<Client>::Ref& ref
) {
  using Caps = BrokerCaps<Client>;
  static_cast<void>(session.quote(samsung()));
  static_cast<void>(session.holdings());
  static_cast<void>(session.bars(BarQuery{}));
  static_cast<void>(session.place(OrderRequest{}));
  static_cast<void>(session.replace(ref, AmendInstruction{}));
  static_cast<void>(session.cancel(ref, AmendInstruction{}));
  static_cast<void>(session.order_history(OrderHistoryQuery{}));
  if constexpr (Caps::has_buying_power) {
    static_cast<void>(session.buying_power(Currency{}));
  }
  if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
    static_cast<void>(session.sellable_quantity(samsung()));
  }
  if constexpr (Caps::has_commission_schedule) {
    static_cast<void>(session.commissions());
  }
  if constexpr (Caps::has_working_orders) {
    static_cast<void>(session.working_orders());
  }
  if constexpr (Caps::has_reference_data) {
    static_cast<void>(session.instrument(samsung()));
  }
  if constexpr (Caps::has_calendar) {
    static_cast<void>(session.calendar(Country::KR));
  }
  if constexpr (Caps::has_price_limits) {
    static_cast<void>(session.price_limits(samsung()));
  }
  if constexpr (Caps::has_trade_prints) {
    static_cast<void>(session.trades(samsung(), 1));
  }
  if constexpr (Caps::has_batch_prices) {
    static_cast<void>(session.prices(std::vector<InstrumentId>{samsung()}));
  }
  if constexpr (Caps::has_depth) {
    static_cast<void>(session.depth(samsung()));
  }
  if constexpr (Caps::has_order_by_id) {
    static_cast<void>(session.order(ref));
  }
  if constexpr (Caps::has_account_discovery) {
    static_cast<void>(session.accounts());
    static_cast<void>(list_accounts(session.client()));
  }
}

// Every shipped asynchronous client, with both a plain callback and a token that names an executor.
// The plain callback is the shape no other test used: under a Boost whose default associated
// executor is `inline_executor`, it is what exposed Kiwoom's holdings refusal.
#define ATL_INSTANTIATE_ASYNC(CLIENT, TOKEN)                                             \
  template void call_every_claimed_verb<CLIENT, TOKEN>(                                  \
    BrokerSession<CLIENT>&, BrokerSession<CLIENT>::Ref, TOKEN                            \
  )

ATL_INSTANTIATE_ASYNC(kis::KisClient, PlainCallback);
ATL_INSTANTIATE_ASYNC(kis::KisClient, boost::asio::use_future_t<>);
ATL_INSTANTIATE_ASYNC(tossinvest::TossClient, PlainCallback);
ATL_INSTANTIATE_ASYNC(tossinvest::TossClient, boost::asio::use_future_t<>);
ATL_INSTANTIATE_ASYNC(kiwoom::KiwoomClient, PlainCallback);
ATL_INSTANTIATE_ASYNC(kiwoom::KiwoomClient, boost::asio::use_future_t<>);
ATL_INSTANTIATE_ASYNC(mock::MockClient, PlainCallback);
ATL_INSTANTIATE_ASYNC(mock::MockClient, boost::asio::use_future_t<>);

#undef ATL_INSTANTIATE_ASYNC

template void call_every_claimed_blocking_verb<kis::KisBlockingClient>(
  BlockingSession<kis::KisBlockingClient>&, const BlockingSession<kis::KisBlockingClient>::Ref&
);
template void call_every_claimed_blocking_verb<tossinvest::TossBlockingClient>(
  BlockingSession<tossinvest::TossBlockingClient>&,
  const BlockingSession<tossinvest::TossBlockingClient>::Ref&
);
template void call_every_claimed_blocking_verb<kiwoom::KiwoomBlockingClient>(
  BlockingSession<kiwoom::KiwoomBlockingClient>&,
  const BlockingSession<kiwoom::KiwoomBlockingClient>::Ref&
);

} // namespace

TEST_CASE("every verb a shipped client's capabilities claim compiles", "[port][conformance]") {
  // Checked by the explicit instantiations above; reaching this line means they compiled.
  ATL_ASSERT_BROKER(kis::KisClient);
  ATL_ASSERT_BROKER(kis::KisBlockingClient);
  ATL_ASSERT_BROKER(tossinvest::TossClient);
  ATL_ASSERT_BROKER(tossinvest::TossBlockingClient);
  ATL_ASSERT_BROKER(kiwoom::KiwoomClient);
  ATL_ASSERT_BROKER(kiwoom::KiwoomBlockingClient);
  ATL_ASSERT_BROKER(mock::MockClient);
  SUCCEED("every claimed verb was compiled for every shipped client");
}

// Fakes for the detectors. Each answers only what the detector under test reads, so a verdict is
// about the one member that differs between them.
namespace conformance_fakes {

struct ClaimsWithoutThunk {};
struct ClaimsWithWrongArity {};
struct DeclinesWithoutThunk {};
struct ClaimsWithThunk {};
struct AsyncClaimsWithBlockingThunk {};
struct BlockingClaimsWithWrongResult {};
struct BlockingClaimsWithThunk {};
struct ReadsTheTokensExecutor {};
struct CalendarValidatesToBool {};
struct WorkingOrdersNormalizeToTheWrongType {};
struct BlockingAccountsWithoutRowNormalizers {};

/** A row type a broker might list accounts as. */
struct AccountRow {
  std::string number_;
};

} // namespace conformance_fakes

namespace algotradelib {

template <>
struct BrokerTraits<conformance_fakes::ClaimsWithoutThunk> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
};

template <>
struct BrokerTraits<conformance_fakes::ClaimsWithWrongArity> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
  template <typename Token>
  static void instrument(conformance_fakes::ClaimsWithWrongArity&, Token&&) {}
};

template <>
struct BrokerTraits<conformance_fakes::DeclinesWithoutThunk> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = false;
};

template <>
struct BrokerTraits<conformance_fakes::ClaimsWithThunk> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
  template <typename Token>
  static void instrument(conformance_fakes::ClaimsWithThunk&, InstrumentId, Token&&) {}
};

template <>
struct BrokerTraits<conformance_fakes::AsyncClaimsWithBlockingThunk> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
  static Result<Instrument> instrument(conformance_fakes::AsyncClaimsWithBlockingThunk&, InstrumentId) {
    return Result<Instrument>(Instrument{});
  }
};

template <>
struct BrokerTraits<conformance_fakes::BlockingClaimsWithWrongResult> {
  static constexpr bool blocking = true;
  static constexpr bool has_reference_data = true;
  static Result<QuoteSnapshot> instrument(conformance_fakes::BlockingClaimsWithWrongResult&, InstrumentId) {
    return Result<QuoteSnapshot>(QuoteSnapshot{});
  }
};

template <>
struct BrokerTraits<conformance_fakes::BlockingClaimsWithThunk> {
  static constexpr bool blocking = true;
  static constexpr bool has_reference_data = true;
  static Result<Instrument> instrument(conformance_fakes::BlockingClaimsWithThunk&, InstrumentId) {
    return Result<Instrument>(Instrument{});
  }
};

/**
 * A thunk written the way a third party would forward to an Asio client: templated on the token,
 * deduced return, and reading the token's executor in its body. Detection instantiates that body,
 * so a stand-in without an executor would hard-error here instead of reporting anything.
 */
template <>
struct BrokerTraits<conformance_fakes::ReadsTheTokensExecutor> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
  template <typename Token>
  static auto instrument(conformance_fakes::ReadsTheTokensExecutor&, InstrumentId, Token&& token) {
    auto executor = token.get_executor();
    return executor;
  }
};

/** A pre-flight check the session cannot use: it dereferences the answer into a `BrokerError`. */
template <>
struct BrokerTraits<conformance_fakes::CalendarValidatesToBool> {
  static constexpr bool blocking = true;
  static constexpr bool has_calendar = true;
  static bool validate_calendar(Country, std::optional<Date>) { return false; }
  static Result<TradingCalendar> calendar(
    conformance_fakes::CalendarValidatesToBool&, Country, std::optional<Date>
  ) {
    return Result<TradingCalendar>(TradingCalendar{});
  }
};

/** A working-order normalizer answering something other than what the session returns. */
template <>
struct BrokerTraits<conformance_fakes::WorkingOrdersNormalizeToTheWrongType> {
  struct Tag {};
  using BrokerTag = Tag;
  using Account = int;
  using WorkingOrdersResult = std::vector<OrderSnapshot>;
  static constexpr bool blocking = true;
  static constexpr bool has_working_orders = true;
  static Result<WorkingOrdersResult> working_orders(
    conformance_fakes::WorkingOrdersNormalizeToTheWrongType&, int&
  ) {
    return Result<WorkingOrdersResult>(WorkingOrdersResult{});
  }
  static Result<WorkingOrdersResult> normalize_working_orders(Result<WorkingOrdersResult> raw) {
    return raw;
  }
};

/** A blocking account listing whose rows nothing can turn into an `AccountInfo`. */
template <>
struct BrokerTraits<conformance_fakes::BlockingAccountsWithoutRowNormalizers> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<std::vector<conformance_fakes::AccountRow>> accounts(
    conformance_fakes::BlockingAccountsWithoutRowNormalizers&
  ) {
    return Result<std::vector<conformance_fakes::AccountRow>>(
      std::vector<conformance_fakes::AccountRow>{}
    );
  }
};

} // namespace algotradelib

TEST_CASE("conformance accepts a thunk that forwards the executor-bound handler the session passes", "[port][conformance]") {
  STATIC_REQUIRE(port_detail::serves_reference_data<conformance_fakes::ReadsTheTokensExecutor>);
}

TEST_CASE("conformance checks the shape the session uses, not only that a member exists", "[port][conformance]") {
  using namespace conformance_fakes;
  // `if (auto invalid = validate_calendar(...)) complete(std::move(*invalid))` -- a bool is testable
  // and cannot be dereferenced, so the session's calendar verb would not compile.
  STATIC_REQUIRE_FALSE(port_detail::serves_calendar<CalendarValidatesToBool>);
  // The session returns `Result<WorkingOrders<BrokerTag>>` from whatever this answers.
  STATIC_REQUIRE_FALSE(port_detail::serves_working_orders<WorkingOrdersNormalizeToTheWrongType>);
  // A blocking listing's rows go through display_of, type_of and selector_of, which are absent.
  STATIC_REQUIRE_FALSE(port_detail::serves_account_discovery<BlockingAccountsWithoutRowNormalizers>);
}

TEST_CASE("a claimed capability is served only by a thunk of the shape the session calls", "[port][conformance]") {
  using namespace conformance_fakes;
  // The defect this branch fixed for KIS: the capability is claimed and nothing backs it.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<ClaimsWithoutThunk>);
  // A member of that name is not enough; the session passes the instrument.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<ClaimsWithWrongArity>);
  // The flag selects the shape: an asynchronous client's session passes a token, so a blocking
  // thunk does not serve it however well it would serve a facade.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<AsyncClaimsWithBlockingThunk>);
  // A blocking facade returns what the session returns, so a thunk answering another type fails.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<BlockingClaimsWithWrongResult>);

  STATIC_REQUIRE(port_detail::serves_reference_data<ClaimsWithThunk>);
  STATIC_REQUIRE(port_detail::serves_reference_data<BlockingClaimsWithThunk>);
  // Declining a capability asks for nothing.
  STATIC_REQUIRE(port_detail::serves_reference_data<DeclinesWithoutThunk>);
}

TEST_CASE("an unanswered capability is detected, not read", "[port][conformance]") {
  using namespace conformance_fakes;
  STATIC_REQUIRE(port_detail::declares_has_reference_data<ClaimsWithoutThunk>::value);
  STATIC_REQUIRE_FALSE(port_detail::declares_has_depth<ClaimsWithoutThunk>::value);
  STATIC_REQUIRE_FALSE(port_detail::declares_sellable_source<ClaimsWithoutThunk>::value);
  STATIC_REQUIRE_FALSE(port_detail::declares_supports_interval<ClaimsWithoutThunk>::value);
  // An undeclared capability is reported as missing on its own, so it does not also report a
  // missing thunk -- one omission, one message.
  STATIC_REQUIRE(port_detail::serves_depth<ClaimsWithoutThunk>);
}

TEST_CASE("Kiwoom's holdings refusal completes later, once, for a plain callback", "[kiwoom][holdings]") {
  // A plain callback names no executor. Under Boost 1.92 its default associated executor is
  // `inline_executor`, and the refusal used to post to it, which did not compile.
  boost::asio::io_context ioc;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  kiwoom::KiwoomConfig config;
  config.holdings_venue_ = Venue::KRX;
  REQUIRE(config.holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED);
  BrokerSession<kiwoom::KiwoomClient> session(
    std::make_shared<kiwoom::KiwoomClient>(ioc.get_executor(), tls, config), kiwoom::KiwoomAccount{}
  );

  int calls = 0;
  std::optional<Result<HoldingsSnapshot>> answer;
  session.async_holdings([&](Result<HoldingsSnapshot> result) {
    ++calls;
    answer.emplace(std::move(result));
  });
  // Never inside the initiating call: a refusal completes the way a broker failure would.
  CHECK(calls == 0);

  // The io_context running the client counts the refusal as work, so run() waits for it.
  ioc.run();
  CHECK(calls == 1);
  REQUIRE(answer);
  REQUIRE_FALSE(*answer);
  CHECK(answer->error().broker_code_ == "holdings_scope_unestablished");
  CHECK_FALSE(answer->error().reached_broker_);
}

TEST_CASE("Kiwoom's holdings refusal keeps a separate caller context waiting for it", "[kiwoom][holdings]") {
  // The client runs on one io_context and the caller's handler is bound to another. If the caller's
  // context is run first it must not finish -- the refusal is outstanding work there -- or it stops
  // and the refusal, posted later, is never delivered without a restart nobody knows to make.
  boost::asio::io_context source;
  boost::asio::io_context caller;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  BrokerSession<kiwoom::KiwoomClient> session(
    std::make_shared<kiwoom::KiwoomClient>(source.get_executor(), tls, kiwoom::KiwoomConfig{}),
    kiwoom::KiwoomAccount{}
  );
  auto strand = boost::asio::make_strand(caller);

  int calls = 0;
  session.async_holdings(
    boost::asio::bind_executor(strand, [&](Result<HoldingsSnapshot> result) {
      ++calls;
      CHECK_FALSE(result);
    })
  );
  caller.poll();
  CHECK(calls == 0);
  CHECK_FALSE(caller.stopped());

  source.run();
  caller.run();
  CHECK(calls == 1);
}

TEST_CASE("Kiwoom's holdings refusal completes on the caller's strand", "[kiwoom][holdings]") {
  boost::asio::io_context ioc;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  kiwoom::KiwoomConfig config;
  BrokerSession<kiwoom::KiwoomClient> session(
    std::make_shared<kiwoom::KiwoomClient>(ioc.get_executor(), tls, config), kiwoom::KiwoomAccount{}
  );
  auto caller = boost::asio::make_strand(ioc);

  int calls = 0;
  bool on_caller_strand = false;
  session.async_holdings(
    boost::asio::bind_executor(caller, [&](Result<HoldingsSnapshot> result) {
      ++calls;
      on_caller_strand = caller.running_in_this_thread();
      CHECK_FALSE(result);
    })
  );
  CHECK(calls == 0);
  ioc.run();
  CHECK(calls == 1);
  CHECK(on_caller_strand);
}
