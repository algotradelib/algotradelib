// Asserts, in source and at compile time, that the strategy surface stays isolated.
//
// This translation unit includes **only** `<algotradelib/strategy>`. That is the whole design:
// with nothing else in the unit, every macro visible below arrived through the strategy surface,
// so the assertions cannot be fooled by include order and there is no compiler dependency output
// to parse. A violation fails the build rather than a test run.
//
// The property being guarded is the one that makes a strategy portable: it names no broker and
// performs no I/O, so one source compiles against every broker and runs unchanged under a
// dry-run, a live session, and later a backtest. It degrades silently — one convenience include
// in a strategy header would couple every strategy to a vendor, and nothing would fail until
// somebody built the same source against the other broker.

#include <algotradelib/strategy>

// --- The layers announce themselves; see ATL_TRANSPORT_LAYER_PRESENT in src/network. ---

#if defined(ATL_TRANSPORT_LAYER_PRESENT)
#error \
  "<algotradelib/strategy> reached the transport layer. A strategy performs no I/O: it receives \
events and emits intents, and the runner is the only component that holds a session. Move the \
include to the runner."
#endif

#if defined(ATL_BROKER_ADAPTER_PRESENT)
#error \
  "<algotradelib/strategy> reached a broker adapter. A strategy names no broker; it reaches a \
broker-specific feature through the context under a capability gate, never by including the \
adapter. Move the include to the runner."
#endif

// --- Third-party transport, checked independently of our own sentinels. ---
//
// Belt and braces: a future transport header that forgot its sentinel would still be caught
// here, because anything doing socket or TLS work pulls one of these in.

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_ASIO_HPP)
#error "<algotradelib/strategy> reached Boost.Asio. A strategy performs no I/O."
#endif

#if defined(BOOST_BEAST_VERSION)
#error "<algotradelib/strategy> reached Boost.Beast. A strategy speaks no HTTP or WebSocket."
#endif

#if defined(OPENSSL_VERSION_NUMBER) || defined(OPENSSL_VERSION_TEXT)
#error "<algotradelib/strategy> reached OpenSSL. A strategy establishes no TLS connection."
#endif

// --- A positive check, so the file cannot pass by including nothing useful. ---
//
// Without this, deleting the include above would make every assertion above vacuously true.

#if !defined(ATL_STRATEGY_SURFACE_PRESENT)
#error \
  "the strategy surface was not included, so the assertions in this file prove nothing. Check \
that <algotradelib/strategy> defines ATL_STRATEGY_SURFACE_PRESENT."
#endif

namespace {

/**
 * The surface is usable from here, which is the other half of the claim.
 *
 * Isolation is only worth having if what remains is enough to write a strategy with. This
 * compiles a handler that prices a limit order and submits it, using nothing but the strategy
 * headers.
 */
struct IsolatedStrategy {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    const auto reference = ctx.instrument(bar.instrument_);
    if (!reference) {
      return;
    }
    const auto limit = algotradelib::strategy::Price::rounded(
      bar.bar_.close_, reference->price_rule_, algotradelib::OrderSide::BUY
    );
    if (!limit) {
      return;
    }
    ctx.submit(
      algotradelib::strategy::OrderIntent::limit(
        bar.instrument_,
        algotradelib::OrderSide::BUY,
        algotradelib::strategy::SizeSpec::quantity(algotradelib::Decimal64::parse("1")),
        *limit
      ).with_tag("isolated")
    );
  }
};

} // namespace

ATL_ASSERT_STRATEGY(IsolatedStrategy, algotradelib::strategy::MinimalContext);
ATL_ASSERT_STRATEGY(IsolatedStrategy, algotradelib::strategy::FullContext);

int main() { return 0; }
