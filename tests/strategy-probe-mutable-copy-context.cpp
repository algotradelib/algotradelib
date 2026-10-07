// Deliberately does not compile. `strategy-refuses-a-mutable-copy-context` builds this file and passes only
// when the diagnostic is that a strategy context must not be copyable. This context's copy constructor takes
// a mutable reference, so `std::is_copy_constructible` says no; but `dispatch` passes a mutable lvalue, which
// it does copy, and the by-value handler's submission would be lost.
#include <algotradelib/strategy>

using namespace algotradelib;

struct MutableCopyContext {
  int submitted = 0;
  MutableCopyContext() = default;
  MutableCopyContext(MutableCopyContext& other) : submitted(other.submitted) {}
  void submit(strategy::TargetPosition) { ++submitted; }
};

struct Probe {
  template <class Ctx> void on_bar(Ctx ctx, const strategy::BarEvent&) { ctx.submit(strategy::TargetPosition{}); }
};

ATL_ASSERT_STRATEGY(Probe, MutableCopyContext);
