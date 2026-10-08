// expect: call to deleted constructor of[^\n]*ProbeContextT|use of deleted function[^\n]*ProbeContextT
// Deliberately does not compile. `strategy-refuses-a-context-by-value` builds this file and passes only
// when the diagnostic is that on_fill is declared and the runner cannot call it. Taking the context by
// value, the handler would run on a copy and what it submitted would be lost; neither context can be
// copied, so it is not callable. The strategy also has a valid `on_bar`.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx ctx, const strategy::Fill&) { ctx.submit(strategy::TargetPosition{}); }
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
