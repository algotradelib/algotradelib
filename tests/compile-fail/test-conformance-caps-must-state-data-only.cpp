// expect: no member named 'data_only' in [^\n]*SilentCaps|'data_only' is not a member of [^\n]*SilentCaps
// Deliberately does not compile. `conformance-caps-must-state-data-only` builds this file and passes only
// when the diagnostic names `data_only`: a hand-written capability set that does not say whether its
// client takes orders is not admitted where orders are sent.
#include <algotradelib/live>

#include "mock_broker.hpp"

namespace probe {

/** A capability set written by hand that does not say whether its client takes orders. */
struct SilentCaps {};

/** The two names `SessionExecutor` reads from a session before anything else. */
struct Session {
  using Caps = SilentCaps;
  using BrokerTag = vendor::Broker;
};

} // namespace probe

static_assert(sizeof(algotradelib::live::SessionExecutor<probe::Session>) > 0);
