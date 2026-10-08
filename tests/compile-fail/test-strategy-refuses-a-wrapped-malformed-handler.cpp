// expect: no matching (member )?function for call to[^\n]*on_phase
// Deliberately does not compile. `strategy-refuses-a-wrapped-malformed-handler` builds this file and passes
// only when the diagnostic is the call to the inner strategy's `on_phase`. The outer strategy is valid and
// forwards through `runner::dispatch`, as a logging wrapper would; the inner one takes the phase
// as a mutable reference. Checking the outer type alone would have skipped the inner handler in silence.
#include <algotradelib/runner>

using namespace algotradelib;

struct Inner {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_phase(Ctx&, strategy::MarketPhase&) {}
};

struct Outer {
  Inner inner_;
  template <class Ctx> void on_bar(Ctx& ctx, const strategy::BarEvent& bar) { runner::dispatch::bar(inner_, ctx, bar); }
  template <class Ctx> void on_phase(Ctx& ctx, const strategy::MarketPhase& phase) {
    runner::dispatch::phase(inner_, ctx, phase);
  }
};

ATL_ASSERT_STRATEGY(Outer, strategy::MinimalContext);
