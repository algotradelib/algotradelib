// expect: a final strategy cannot be checked for a handler the runner would skip
// Deliberately does not compile. `strategy-refuses-a-final-strategy` builds this file and passes only when
// the diagnostic is that a final strategy cannot be checked. The case: the
// fill handler takes its context by value, which was called with a copy before the context became
// move-only and would now be skipped in silence, since a final class's handler names cannot be looked up.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe final {
  int fills = 0;
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx ctx, const strategy::Fill&) { ++fills; static_cast<void>(ctx); }
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
