// expect: call to deleted constructor of[^\n]*Context<|use of deleted function[^\n]*Context<
// Deliberately does not compile. `strategy-refuses-a-runner-context-by-value` builds this file and passes
// only when the diagnostic is that on_fill is declared and the runner cannot call it: the same handler as
// test-strategy-refuses-a-context-by-value.cpp, asserted against the runner's own context rather than a probe's.
#include <algotradelib/runner>

using namespace algotradelib;

struct Probe {
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx> void on_fill(Ctx ctx, const strategy::Fill&) { ctx.submit(strategy::TargetPosition{}); }
};

using RunnerContext = runner::Context<strategy::ProbeBroker, strategy::NoCaps>;
ATL_ASSERT_STRATEGY(Probe, RunnerContext);
