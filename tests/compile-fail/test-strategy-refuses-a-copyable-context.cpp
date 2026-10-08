// expect: a strategy context must not be copyable
// Deliberately does not compile. `strategy-refuses-a-copyable-context` builds this file and passes only when
// the diagnostic is that a strategy context must not be copyable. The context is a caller's own and can be
// copied, so the by-value handler would run on a copy and its submission would be lost.
#include <algotradelib/strategy>

using namespace algotradelib;

struct CopyContext {
  int submitted = 0;
  void submit(strategy::TargetPosition) { ++submitted; }
};

struct Probe {
  template <class Ctx> void on_bar(Ctx ctx, const strategy::BarEvent&) { ctx.submit(strategy::TargetPosition{}); }
};

ATL_ASSERT_STRATEGY(Probe, CopyContext);
