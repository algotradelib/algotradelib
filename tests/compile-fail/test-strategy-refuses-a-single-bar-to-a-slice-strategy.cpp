// expect: this strategy declares on_bars, so its bars are delivered in slices
// variants: 1 2 3
// Deliberately does not compile. `strategy-refuses-a-single-bar-to-a-slice-strategy-<n>` builds this file
// and passes only when the diagnostic is the slice route's: a strategy declaring `on_bars` handed one
// `BarEvent` through `dispatch::bar` (1), `Runner::feed` (2) or `Runner::feed_warmup` (3). Without the
// refusal, a path still feeding single bars would skip the strategy in silence.
#include <algotradelib/runner>

using namespace algotradelib;

struct SliceOnly {
  template <class Ctx> void on_bars(Ctx&, const strategy::BarSlice&) {}
};

void feeds_one_bar(const strategy::BarEvent& bar) {
  runner::DryRunner<SliceOnly> dry;
#if ATL_PROBE_VARIANT == 1
  SliceOnly strategy;
  runner::dispatch::bar(strategy, dry.context(), bar);
#elif ATL_PROBE_VARIANT == 2
  dry.feed(bar);
#elif ATL_PROBE_VARIANT == 3
  dry.feed_warmup(bar);
#endif
}
