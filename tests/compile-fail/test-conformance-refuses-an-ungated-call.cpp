// expect: gate the call with BrokerCaps<Client>::has_depth
// Deliberately does not compile. `conformance-refuses-an-ungated-call` builds this file and passes only
// when the diagnostic is the session's own: a verb whose capability the broker does not claim must not
// compile for a caller who did not gate it. Without that assertion the verb's deduced return is `void`,
// and a call with a callback would compile and never complete.
#include "mock_broker.hpp"

#include <boost/asio/io_context.hpp>

using namespace algotradelib;

void ungated(BrokerSession<vendor::MockClient>& session) {
  session.async_depth(InstrumentId{}, [](Result<DepthSnapshot>) {});
}
