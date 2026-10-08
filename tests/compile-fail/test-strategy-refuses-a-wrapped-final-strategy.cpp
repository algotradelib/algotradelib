// expect: a final strategy cannot be checked for a handler the runner would skip
// Deliberately does not compile. `strategy-refuses-a-wrapped-final-strategy` builds this file and passes only
// when the diagnostic is that a final strategy cannot be checked. The outer strategy is valid and forwards
// through `runner::dispatch` to a `final` inner one, whose handler names cannot be looked up: without the
// refusal in `dispatch` itself, the inner handlers would never be called.
#include <algotradelib/runner>

using namespace algotradelib;

struct Inner final {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
};

struct Outer {
  Inner inner_;
  template <class Ctx> void on_bar(Ctx& ctx, const strategy::BarEvent& bar) { runner::dispatch::bar(inner_, ctx, bar); }
};

ATL_ASSERT_STRATEGY(Outer, strategy::MinimalContext);
