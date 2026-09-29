// Deliberately does not compile. `conformance-account-rows-reject-an-rvalue-cursor` builds this file
// and passes only on the failed comparison inside `to_account_info`. It is the real call behind the
// `serves_account_discovery` verdict in test-capability-conformance.cpp: a cursor that compares only
// as an rvalue is rejected by the check, and this shows the session could not have used it either.
#include <algotradelib/broker>

#include <string>
#include <vector>

namespace probe {

struct Row {
  std::string number_;
};
struct End {};
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
struct Client {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::Client> {
  using Account = int;
  static constexpr bool blocking = true;
  static constexpr bool has_account_discovery = true;
  static Result<probe::Rows> accounts(probe::Client&) { return Result<probe::Rows>(probe::Rows{}); }
  static std::string display_of(const probe::Row& row) { return row.number_; }
  static std::string type_of(const probe::Row&) { return "cash"; }
  static int selector_of(const probe::Row&) { return 1; }
};

} // namespace algotradelib

void list(probe::Client& client) { static_cast<void>(algotradelib::list_accounts(client)); }
