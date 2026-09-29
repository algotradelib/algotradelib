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

// Every shipped client, through the public check a third-party adapter author uses. The plain
// callback is the shape no other test used: under a Boost whose default associated executor is
// `inline_executor`, it is what exposed Kiwoom's holdings refusal; the move-only one proves no
// shipped thunk copies its token.
ATL_ASSERT_BROKER_VERBS(kis::KisClient);
ATL_ASSERT_BROKER_VERBS(kis::KisBlockingClient);
ATL_ASSERT_BROKER_VERBS(tossinvest::TossClient);
ATL_ASSERT_BROKER_VERBS(tossinvest::TossBlockingClient);
ATL_ASSERT_BROKER_VERBS(kiwoom::KiwoomClient);
ATL_ASSERT_BROKER_VERBS(kiwoom::KiwoomBlockingClient);
ATL_ASSERT_BROKER_VERBS(mock::MockClient);

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

/** A row type a broker might list accounts as. */
struct AccountRow {
  std::string number_;
};

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
struct BlockingClaimsWithWrongArity {};
struct RequiresTheInlineExecutor {};
struct ExplicitRefusal {};
struct AccountsWithoutValueType {};
struct AccountsWithoutEnd {};
struct AccountsOverLvalueCursor {};
struct AccountsOverRvalueCursor {};
struct AccountsNotAResult {};
struct AccountSelectorNotDefaultConstructible {};

/** A refusal whose dereference converts to `BrokerError` only explicitly. */
struct ExplicitlyAnError {
  explicit operator BrokerError() const { return BrokerError{}; }
};

/** A refusal answer shaped like `std::optional<ExplicitlyAnError>`. */
struct ExplicitRefusalAnswer {
  explicit operator bool() const { return false; }
  ExplicitlyAnError operator*() const { return {}; }
};

/** The sentinel of both cursor ranges below. */
struct RowsEnd {};

/**
 * A cursor whose comparison and dereference accept only lvalues -- valid for a range-for, which
 * holds its iterator and sentinel in named variables, and for `to_account_info`, which does too.
 */
struct LvalueCursor {
  const AccountRow* at_;
  const AccountRow& operator*() & { return *at_; }
  LvalueCursor& operator++() {
    ++at_;
    return *this;
  }
  friend bool operator!=(LvalueCursor& cursor, RowsEnd&) { return cursor.at_ != nullptr; }
};

/** A range over `LvalueCursor`, empty. */
struct LvalueRows {
  LvalueCursor begin() const { return LvalueCursor{nullptr}; }
  RowsEnd end() const { return {}; }
  std::size_t size() const { return 0; }
};

/** A cursor whose comparison accepts only rvalues, which no loop over named variables can use. */
struct RvalueCursor {
  const AccountRow* at_;
  const AccountRow& operator*() const { return *at_; }
  RvalueCursor& operator++() {
    ++at_;
    return *this;
  }
  friend bool operator!=(RvalueCursor&& cursor, RowsEnd&&) { return cursor.at_ != nullptr; }
};

/** A range over `RvalueCursor`. */
struct RvalueRows {
  RvalueCursor begin() const { return RvalueCursor{nullptr}; }
  RowsEnd end() const { return {}; }
  std::size_t size() const { return 0; }
};

/** An account selector with no default, which `AccountInfo`'s default construction needs. */
struct NoDefaultSelector {
  explicit NoDefaultSelector(int) {}
};


/** A range with `begin`, `end` and `size` and no `value_type`, which a range-for accepts. */
struct AccountRange {
  const AccountRow* begin() const { return nullptr; }
  const AccountRow* end() const { return nullptr; }
  std::size_t size() const { return 0; }
};

/** A listing with `begin` and `size` and no `end`, which a range-for cannot iterate. */
struct AccountRangeWithoutEnd {
  const AccountRow* begin() const { return nullptr; }
  std::size_t size() const { return 0; }
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

/** A blocking thunk missing the instrument argument the blocking session passes. */
template <>
struct BrokerTraits<conformance_fakes::BlockingClaimsWithWrongArity> {
  static constexpr bool blocking = true;
  static constexpr bool has_reference_data = true;
  static Result<Instrument> instrument(conformance_fakes::BlockingClaimsWithWrongArity&) {
    return Result<Instrument>(Instrument{});
  }
};

/**
 * A thunk constrained to the executor a plain callback carries under recent Boost. It works for that
 * caller and for no other, which a probe with any one stand-in handler would have to misjudge.
 */
template <>
struct BrokerTraits<conformance_fakes::RequiresTheInlineExecutor> {
  static constexpr bool blocking = false;
  static constexpr bool has_reference_data = true;
  template <
    typename Token,
    typename = std::enable_if_t<std::is_same_v<
      typename std::decay_t<Token>::executor_type, boost::asio::inline_executor
    >>
  >
  static void instrument(conformance_fakes::RequiresTheInlineExecutor&, InstrumentId, Token&&) {}
};

/** A calendar pre-flight whose refusal the session could not pass by value. */
template <>
struct BrokerTraits<conformance_fakes::ExplicitRefusal> {
  static constexpr bool blocking = true;
  static constexpr bool has_calendar = true;
  static conformance_fakes::ExplicitRefusalAnswer validate_calendar(Country, std::optional<Date>) {
    return {};
  }
  static Result<TradingCalendar> calendar(
    conformance_fakes::ExplicitRefusal&, Country, std::optional<Date>
  ) {
    return Result<TradingCalendar>(TradingCalendar{});
  }
};

/** Account discovery over a range with no `value_type`: `to_account_info` iterates it fine. */
template <>
struct BrokerTraits<conformance_fakes::AccountsWithoutValueType> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<conformance_fakes::AccountRange> accounts(conformance_fakes::AccountsWithoutValueType&) {
    return Result<conformance_fakes::AccountRange>(conformance_fakes::AccountRange{});
  }
  static std::string display_of(const conformance_fakes::AccountRow& row) { return row.number_; }
  static std::string type_of(const conformance_fakes::AccountRow&) { return "cash"; }
  static int selector_of(const conformance_fakes::AccountRow&) { return 1; }
};

/** Account discovery over a range a range-for cannot iterate. */
template <>
struct BrokerTraits<conformance_fakes::AccountsWithoutEnd> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<conformance_fakes::AccountRangeWithoutEnd> accounts(conformance_fakes::AccountsWithoutEnd&) {
    return Result<conformance_fakes::AccountRangeWithoutEnd>(conformance_fakes::AccountRangeWithoutEnd{});
  }
  static std::string display_of(const conformance_fakes::AccountRow& row) { return row.number_; }
  static std::string type_of(const conformance_fakes::AccountRow&) { return "cash"; }
  static int selector_of(const conformance_fakes::AccountRow&) { return 1; }
};

/** Account discovery over a range whose cursor works only as a named lvalue. */
template <>
struct BrokerTraits<conformance_fakes::AccountsOverLvalueCursor> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<conformance_fakes::LvalueRows> accounts(conformance_fakes::AccountsOverLvalueCursor&) {
    return Result<conformance_fakes::LvalueRows>(conformance_fakes::LvalueRows{});
  }
  static std::string display_of(const conformance_fakes::AccountRow& row) { return row.number_; }
  static std::string type_of(const conformance_fakes::AccountRow&) { return "cash"; }
  static int selector_of(const conformance_fakes::AccountRow&) { return 1; }
};

/** Account discovery over a range whose cursor compares only as an rvalue. */
template <>
struct BrokerTraits<conformance_fakes::AccountsOverRvalueCursor> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<conformance_fakes::RvalueRows> accounts(conformance_fakes::AccountsOverRvalueCursor&) {
    return Result<conformance_fakes::RvalueRows>(conformance_fakes::RvalueRows{});
  }
  static std::string display_of(const conformance_fakes::AccountRow& row) { return row.number_; }
  static std::string type_of(const conformance_fakes::AccountRow&) { return "cash"; }
  static int selector_of(const conformance_fakes::AccountRow&) { return 1; }
};

/** Account discovery answering rows directly, with no `Result` for a failure to travel in. */
template <>
struct BrokerTraits<conformance_fakes::AccountsNotAResult> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static std::vector<conformance_fakes::AccountRow> accounts(conformance_fakes::AccountsNotAResult&) {
    return {};
  }
};

/** Account discovery whose selector `AccountInfo` cannot default-construct. */
template <>
struct BrokerTraits<conformance_fakes::AccountSelectorNotDefaultConstructible> {
  using Account = conformance_fakes::NoDefaultSelector;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<std::vector<conformance_fakes::AccountRow>> accounts(
    conformance_fakes::AccountSelectorNotDefaultConstructible&
  ) {
    return Result<std::vector<conformance_fakes::AccountRow>>(
      std::vector<conformance_fakes::AccountRow>{}
    );
  }
  static std::string display_of(const conformance_fakes::AccountRow& row) { return row.number_; }
  static std::string type_of(const conformance_fakes::AccountRow&) { return "cash"; }
  static Account selector_of(const conformance_fakes::AccountRow&) { return Account{1}; }
};

} // namespace algotradelib

TEST_CASE("conformance never rejects an asynchronous thunk that works for its caller", "[port][conformance]") {
  // Detected by name, so neither a body that reads the token's executor nor a constraint to one
  // executor type is instantiated against a handler the caller would never pass.
  STATIC_REQUIRE(port_detail::serves_reference_data<conformance_fakes::ReadsTheTokensExecutor>);
  STATIC_REQUIRE(port_detail::serves_reference_data<conformance_fakes::RequiresTheInlineExecutor>);
}

TEST_CASE("conformance checks the shape the session uses, not only that a member exists", "[port][conformance]") {
  using namespace conformance_fakes;
  // `if (auto invalid = validate_calendar(...)) complete(std::move(*invalid))` -- a bool is testable
  // and cannot be dereferenced, so the session's calendar verb would not compile.
  STATIC_REQUIRE_FALSE(port_detail::serves_calendar<CalendarValidatesToBool>);
  // The session returns `Result<WorkingOrders<BrokerTag>>` from whatever this answers.
  STATIC_REQUIRE_FALSE(port_detail::serves_working_orders<WorkingOrdersNormalizeToTheWrongType>);
  // The refusal is passed by value to a `BrokerError` parameter, which an explicit conversion is not.
  STATIC_REQUIRE_FALSE(port_detail::serves_calendar<ExplicitRefusal>);
  // A blocking listing must answer a `Result` of rows; a bare vector is not what the session takes.
  STATIC_REQUIRE_FALSE(port_detail::serves_account_discovery<AccountsNotAResult>);
}

TEST_CASE("account rows are checked by the verbs check, not modelled by the early one", "[port][conformance]") {
  using namespace conformance_fakes;
  // **The stated limit, pinned.** `ATL_ASSERT_BROKER` checks that a blocking listing answers a
  // `Result` and stops there: every one of these passes it, and every one but the last two fails in
  // `to_account_info`. Modelling that range-for was tried for four review rounds and each round found
  // a requirement the model missed; `ATL_ASSERT_BROKER_VERBS` calls the real `list_accounts`, and
  // `conformance-verbs-reject-unwalkable-account-rows` shows it rejecting the rvalue cursor.
  STATIC_REQUIRE(port_detail::serves_account_discovery<BlockingAccountsWithoutRowNormalizers>);
  STATIC_REQUIRE(port_detail::serves_account_discovery<AccountSelectorNotDefaultConstructible>);
  STATIC_REQUIRE(port_detail::serves_account_discovery<AccountsWithoutEnd>);
  STATIC_REQUIRE(port_detail::serves_account_discovery<AccountsOverRvalueCursor>);
  STATIC_REQUIRE(port_detail::serves_account_discovery<AccountsWithoutValueType>);
  STATIC_REQUIRE(port_detail::serves_account_discovery<AccountsOverLvalueCursor>);

  // And the two that are walkable, walked for real: no `value_type`, and a cursor that works only as
  // a named lvalue, which is how a range-for holds it.
  AccountsWithoutValueType plain;
  const auto listed_plain = list_accounts(plain);
  REQUIRE(listed_plain);
  CHECK(listed_plain.value().empty());
  AccountsOverLvalueCursor lvalue_only;
  const auto listed_lvalue = list_accounts(lvalue_only);
  REQUIRE(listed_lvalue);
  CHECK(listed_lvalue.value().empty());
}

TEST_CASE("a claimed capability is served only by a thunk behind it", "[port][conformance]") {
  using namespace conformance_fakes;
  // The defect this branch fixed for KIS: the capability is claimed and nothing backs it.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<ClaimsWithoutThunk>);
  // A blocking facade is checked in the whole shape its session calls: the instrument argument...
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<BlockingClaimsWithWrongArity>);
  // ...and the type it returns.
  STATIC_REQUIRE_FALSE(port_detail::serves_reference_data<BlockingClaimsWithWrongResult>);

  // **The stated limit, pinned so it cannot change silently.** `ATL_ASSERT_BROKER` detects an
  // asynchronous thunk by name, so a wrong signature -- here the instrument argument missing, or a
  // blocking-shaped thunk on an asynchronous client -- passes it. `ATL_ASSERT_BROKER_VERBS` is what
  // rejects them; `conformance-verbs-reject-a-wrong-signature` builds that case and must fail.
  STATIC_REQUIRE(port_detail::serves_reference_data<ClaimsWithWrongArity>);
  STATIC_REQUIRE(port_detail::serves_reference_data<AsyncClaimsWithBlockingThunk>);

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
