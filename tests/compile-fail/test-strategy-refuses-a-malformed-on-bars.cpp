// expect: no matching (member )?function for call to[^\n]*on_bars
// Deliberately does not compile. `strategy-refuses-a-malformed-on-bars` builds this file and passes only
// when the diagnostic is the call to `on_bars`: it takes the slice as a mutable reference, which the
// runner's call does not bind to. Read as absent instead, the strategy would never decide.
#include <algotradelib/strategy>

using namespace algotradelib;

struct MutableSlice {
  template <class Ctx> void on_bars(Ctx&, strategy::BarSlice&) {}
};

ATL_ASSERT_STRATEGY(MutableSlice, strategy::MinimalContext);
