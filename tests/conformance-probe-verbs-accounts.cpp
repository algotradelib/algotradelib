// Deliberately does not compile. `conformance-verbs-reject-unwalkable-account-rows` builds this file
// and passes only on the failed comparison inside `to_account_info`'s range-for. The adapter passes
// `ATL_ASSERT_BROKER`, which checks only that a blocking listing answers a `Result`; the rows are
// `ATL_ASSERT_BROKER_VERBS`' to check, because it calls the real `list_accounts`.
#include <algotradelib/broker>
#include <algotradelib/kiwoom>

#include <string>

namespace probe {

struct Row {
  std::string number_;
};
struct End {};
/** A cursor that compares only as an rvalue, which a range-for -- holding it by name -- cannot use. */
struct Cursor {
  const Row* at_;
  const Row& operator*() const { return *at_; }
  Cursor& operator++() {
    ++at_;
    return *this;
  }
  friend bool operator!=(Cursor&& cursor, End&&) { return cursor.at_ != nullptr; }
};
struct Rows {
  Cursor begin() const { return Cursor{nullptr}; }
  End end() const { return {}; }
  std::size_t size() const { return 0; }
};

/** A blocking Kiwoom client in every respect but its account listing. */
struct Client : algotradelib::kiwoom::KiwoomBlockingClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::Client> : BrokerTraits<kiwoom::KiwoomBlockingClient> {
  static Result<probe::Rows> accounts(Client&) { return Result<probe::Rows>(probe::Rows{}); }
  static std::string display_of(const probe::Row& row) { return row.number_; }
  static std::string type_of(const probe::Row&) { return "cash"; }
  static Account selector_of(const probe::Row&) { return Account{}; }
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::Client);
ATL_ASSERT_BROKER_VERBS(probe::Client);
