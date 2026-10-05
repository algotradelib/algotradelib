// Deliberately does not compile. `strategy-refuses-a-mutable-phase` builds this file and passes
// only when the diagnostic is that on_phase is declared and the runner cannot call it. The strategy
// also has a valid `on_bar`, so it is a strategy by every other check, and the runner would
// otherwise skip the handler in silence.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_phase(Ctx&, strategy::MarketPhase&) {}  // the runner passes it const
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
