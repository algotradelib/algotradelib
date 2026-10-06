// Deliberately does not compile. `strategy-refuses-an-unreached-handler` builds this file and passes only when
// the diagnostic is the call to `on_quote`. Nothing here feeds a quote, so only the assertion's compiling of
// every handler call finds the wrong event type -- before a runner that does feed quotes is built.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_quote(Ctx&, const strategy::BarEvent&) {}  // the wrong event
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
