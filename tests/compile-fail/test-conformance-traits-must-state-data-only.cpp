// expect: no member named 'data_only' in [^\n]*SilentAboutDataOnly|'data_only' is not a member of [^\n]*SilentAboutDataOnly
// Deliberately does not compile. `conformance-traits-must-state-data-only` builds this file and passes
// only when the diagnostic names `data_only`: an adapter whose traits do not say whether its client takes
// orders is not admitted where orders are sent. The boundary is the traits, read through `BrokerCaps`;
// `test-conformance-caps-must-state-data-only.cpp` covers a capability set written by hand.
#include <algotradelib/live>

#include "mock_broker.hpp"

namespace probe {

/** The in-memory client under an adapter that answers everything except `data_only`. */
struct SilentAboutDataOnly : vendor::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::SilentAboutDataOnly> : mock_port::Members, mock_port::Capabilities {
  static constexpr bool working_orders_state_terminal = true;
};

} // namespace algotradelib

static_assert(sizeof(algotradelib::live::SessionExecutor<algotradelib::BrokerSession<probe::SilentAboutDataOnly>>) > 0);
