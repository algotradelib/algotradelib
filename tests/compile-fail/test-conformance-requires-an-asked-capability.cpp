// expect: no member named 'working_orders_state_terminal' in [^\n]*SilentAboutTerminal|'working_orders_state_terminal' is not a member of [^\n]*SilentAboutTerminal
// Deliberately does not compile. `conformance-requires-an-asked-capability` builds this file and passes
// only when the diagnostic names `working_orders_state_terminal`: an adapter that lists working orders is
// asked whether a row can state a terminal status, and must answer.
#include "mock_broker.hpp"

namespace probe {

/** The in-memory client, which lists working orders, under an adapter that does not answer. */
struct SilentAboutTerminal : vendor::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::SilentAboutTerminal> : mock_port::Members, mock_port::Capabilities {
  static constexpr bool data_only = false;
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::SilentAboutTerminal);
