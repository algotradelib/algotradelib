// Asserts, at compile time, that the broker interface reaches no HTTP, WebSocket or TLS code.
//
// Same design as `test-runner-isolation.cpp`: the checks below follow an include of **only**
// `<algotradelib/broker>`, so every macro they see arrived through it.
//
// `BrokerSession`, `BlockingSession`, the traits and the conformance macros are generic over a
// broker client; the transport belongs to each adapter (`<algotradelib/kis>` and the others).
// Asio is allowed: the sessions' asynchronous calls take completion tokens and executors.
// Beast and OpenSSL are not. Reaching them once cost every program and test that binds a mock or a
// market-data vendor about 1,700 extra headers.
//
// After the checks, a vendor written the way a third party would (`data_only_vendor.hpp`, the broker
// interface and Asio only) is asserted conformant, which compiles both sessions' operations for
// it. The checks therefore cover a session that is actually built, not just a header that is parsed.

#include <algotradelib/broker>

#if defined(BOOST_BEAST_VERSION) || defined(BOOST_BEAST_CORE_DETAIL_CONFIG_HPP)
#error "<algotradelib/broker> reached Boost.Beast. The broker interface speaks no HTTP or WebSocket."
#endif

#if defined(OPENSSL_VERSION_NUMBER) || defined(OPENSSL_VERSION_TEXT)
#error "<algotradelib/broker> reached OpenSSL. The broker interface establishes no TLS connection."
#endif

#if defined(ATL_BROKER_ADAPTER_PRESENT)
#error "<algotradelib/broker> reached a broker adapter. Adapters include the interface, not the reverse."
#endif

#include "data_only_vendor.hpp"

ATL_ASSERT_BROKER(vendor::BlockingClient);
ATL_ASSERT_BROKER(vendor::AsyncClient);

int main() {
  auto client = std::make_shared<vendor::BlockingClient>();
  algotradelib::BlockingSession<vendor::BlockingClient> session(client, algotradelib::data_only::NoAccount{});
  // A data-only session refuses holdings before calling the vendor.
  return session.holdings() ? 1 : 0;
}
