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

// Every shipped client, through the check a third-party adapter author runs. The plain callback is
// the shape that exposed Kiwoom's holdings refusal under a Boost whose default associated executor is
// `inline_executor`; the move-only one proves no shipped thunk copies its token.
ATL_ASSERT_BROKER(kis::KisClient);
ATL_ASSERT_BROKER(kis::KisBlockingClient);
ATL_ASSERT_BROKER(tossinvest::TossClient);
ATL_ASSERT_BROKER(tossinvest::TossBlockingClient);
ATL_ASSERT_BROKER(kiwoom::KiwoomClient);
ATL_ASSERT_BROKER(kiwoom::KiwoomBlockingClient);
ATL_ASSERT_BROKER(mock::MockClient);
ATL_ASSERT_BROKER(mock::MockBlockingClient);

// An adapter whose traits specialization is `final` and inherits no shipped broker's: nothing in the
// check derives from the traits, so it conforms like any other.
namespace conformance_fakes {
struct FinalTraitsClient : mock::MockClient {};
} // namespace conformance_fakes

namespace algotradelib {
template <>
struct BrokerTraits<conformance_fakes::FinalTraitsClient> final : mock_port::Members, mock_port::Capabilities {
  static constexpr bool data_only = false;
  static constexpr bool working_orders_state_terminal = false;
};
} // namespace algotradelib

ATL_ASSERT_BROKER(conformance_fakes::FinalTraitsClient);

// Account listings whose rows are walkable in unusual ways, walked for real by `list_accounts`.
namespace conformance_fakes {

/** A row type a broker might list accounts as. */
struct AccountRow {
  std::string number_;
};

/** The sentinel of the cursor range below. */
struct RowsEnd {};

/**
 * A cursor whose dereference accepts only lvalues -- valid for a range-for, which holds its iterator
 * in a named variable, and for `to_account_info`, which does too. Its comparison takes const
 * references: GCC rejects a range-for whose `!=` takes non-const ones, which Clang accepts.
 */
struct LvalueCursor {
  const AccountRow* at_;
  const AccountRow& operator*() & { return *at_; }
  LvalueCursor& operator++() {
    ++at_;
    return *this;
  }
  friend bool operator!=(const LvalueCursor& cursor, const RowsEnd&) { return cursor.at_ != nullptr; }
};

/** A range over `LvalueCursor`, empty. */
struct LvalueRows {
  LvalueCursor begin() const { return LvalueCursor{nullptr}; }
  RowsEnd end() const { return {}; }
  std::size_t size() const { return 0; }
};

/** A range with `begin`, `end` and `size` and no `value_type`, which a range-for accepts. */
struct AccountRange {
  const AccountRow* begin() const { return nullptr; }
  const AccountRow* end() const { return nullptr; }
  std::size_t size() const { return 0; }
};

struct AccountsWithoutValueType {};
struct AccountsOverLvalueCursor {};

} // namespace conformance_fakes

namespace algotradelib {

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

} // namespace algotradelib

TEST_CASE("account rows a range-for can walk are listed", "[port][conformance]") {
  using namespace conformance_fakes;
  // No `value_type`, and a cursor that works only as a named lvalue, which is how a range-for holds it.
  AccountsWithoutValueType plain;
  const auto listed_plain = list_accounts(plain);
  REQUIRE(listed_plain);
  CHECK(listed_plain.value().empty());
  AccountsOverLvalueCursor lvalue_only;
  const auto listed_lvalue = list_accounts(lvalue_only);
  REQUIRE(listed_lvalue);
  CHECK(listed_lvalue.value().empty());
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
