// expect: (private|inaccessible)
// variants: 1 2
// Deliberately does not compile. `strategy-refuses-an-inaccessible-on-bars-<n>` builds this file and passes
// only when the diagnostic is the access check on `on_bars`: a private member (1) or one inherited through
// a private base (2). Either is declared, so the runner calls it, and the call must not compile.
#include <algotradelib/strategy>

using namespace algotradelib;

#if ATL_PROBE_VARIANT == 1
class Hidden {
  template <class Ctx> void on_bars(Ctx&, const strategy::BarSlice&) {}
};
#elif ATL_PROBE_VARIANT == 2
struct Base {
  template <class Ctx> void on_bars(Ctx&, const strategy::BarSlice&) {}
};
struct Hidden : private Base {};
#endif

ATL_ASSERT_STRATEGY(Hidden, strategy::MinimalContext);
