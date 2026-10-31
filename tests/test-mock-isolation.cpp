// Asserts, at compile time, that the mock broker reaches no HTTP, WebSocket or TLS code.
//
// Same design as `test-runner-isolation.cpp`: this translation unit includes **only**
// `<algotradelib/mock>`, so every macro visible below arrived through it.
//
// `ATL_TRANSPORT_LAYER_PRESENT` is not checked: the core broker session includes
// `network/rate_limit.hpp` (a pacer's statistics, no I/O), which defines it. What a mock must never
// reach is a client that could contact a broker: Boost.Beast (the HTTP client and the WebSocket
// sessions are built on it) and OpenSSL.

#include <algotradelib/mock>

#if defined(BOOST_BEAST_VERSION)
#error "<algotradelib/mock> reached Boost.Beast, the HTTP and WebSocket clients. A mock broker sends nothing."
#endif

#if defined(OPENSSL_VERSION_NUMBER) || defined(OPENSSL_VERSION_TEXT)
#error "<algotradelib/mock> reached OpenSSL. A mock broker establishes no TLS connection."
#endif

#if !defined(ATL_BROKER_ADAPTER_PRESENT)
#error "<algotradelib/mock> is a broker: it must mark itself as one, so the strategy isolation tests see it."
#endif

int main() { return 0; }
