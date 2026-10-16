// expect: declares on_bar or on_bars, not both
// Deliberately does not compile. `strategy-refuses-both-bar-handlers` builds this file and passes only when
// the diagnostic is the one-bar-handler rule: a runner delivers bars one way, so the other handler would
// never run, or both would and every decision would be taken twice.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Both {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_bars(Ctx&, const strategy::BarSlice&) {}
};

ATL_ASSERT_STRATEGY(Both, strategy::MinimalContext);
