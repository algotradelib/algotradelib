// expect: distinct names
// Deliberately does not compile. `with-broker-refuses-duplicate-names` builds this file and passes only when the
// diagnostic is with_broker's assertion: two clients of one name would dispatch to whichever is listed first, as
// a broker's asynchronous and blocking clients would.
#include <algotradelib/broker>

#include <string_view>

namespace probe {
struct First {};
struct Second {};
} // namespace probe

template <>
struct algotradelib::BrokerTraits<probe::First> {
  static constexpr std::string_view name = "same";
};

template <>
struct algotradelib::BrokerTraits<probe::Second> {
  static constexpr std::string_view name = "same";
};

int dispatch(std::string_view name) {
  using Listed = algotradelib::type_list<probe::First, probe::Second>;
  return algotradelib::with_broker<Listed>(name, [](auto) {
    return 1;
  }).value_or(0);
}
