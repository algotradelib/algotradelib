// Asserts, in source and at compile time, that a dry run needs no broker and no transport.
//
// Same design as `test-strategy-isolation.cpp`: this translation unit includes **only**
// `<algotradelib/runner>`, so every macro visible below arrived through it and the check cannot be
// fooled by include order.
//
// The promise is stronger than a flag. A dry-run program links no TLS and no HTTP, so there is no
// symbol in the binary that could place an order — a flag can be set wrong, whereas a function
// that is not linked cannot be called. `LiveRunner` lives in `<algotradelib/live>` precisely
// because it does need a session, and keeping the two apart is what makes this assertable.

#include <algotradelib/runner>

#if defined(ATL_TRANSPORT_LAYER_PRESENT)
#error \
  "<algotradelib/runner> reached the transport layer. A dry run holds no session and must link no \
TLS: that is what makes it impossible for a dry-run binary to place an order. LiveRunner belongs \
in <algotradelib/live>."
#endif

#if defined(ATL_BROKER_ADAPTER_PRESENT)
#error "<algotradelib/runner> reached a broker adapter. A dry run binds no broker."
#endif

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_ASIO_HPP)
#error "<algotradelib/runner> reached Boost.Asio. A dry run performs no I/O."
#endif

#if defined(OPENSSL_VERSION_NUMBER) || defined(OPENSSL_VERSION_TEXT)
#error "<algotradelib/runner> reached OpenSSL. A dry run establishes no TLS connection."
#endif

#if !defined(ATL_STRATEGY_SURFACE_PRESENT)
#error \
  "the strategy surface was not reached, so the assertions in this file prove nothing about a \
runner that can actually drive a strategy."
#endif

namespace {

/** A strategy the dry runner can drive, using nothing but the strategy surface. */
struct Flat {
  template <class Ctx>
  void on_bar(Ctx& ctx, const algotradelib::strategy::BarEvent& bar) {
    algotradelib::strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = algotradelib::Decimal64::parse("0");
    ctx.submit(target);
  }
};

} // namespace

int main() {
  algotradelib::runner::DryRunner<Flat> runner;
  return runner.start().quiet() ? 0 : 1;
}
