// Deliberately does not compile. `strategy-refuses-a-wrong-update-event` builds this file and
// passes only when the diagnostic is that on_order_update is declared and the runner cannot call
// it. The strategy also has a valid `on_bar`, so it is a strategy by every other check, and the
// runner would otherwise skip the handler in silence.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_order_update(Ctx&, const strategy::QuoteEvent&) {}  // the wrong event
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
