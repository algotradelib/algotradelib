// expect: a record fed to a strategy that declares no on_data would vanish
// variants: 1 2
// Deliberately does not compile. `strategy-refuses-a-record-without-on-data-<n>` builds this file and passes
// only when the diagnostic is the refusal of a record handed to a strategy without `on_data`, through
// `Runner::feed` (1) or `Backtester::feed` (2): the record must not be dropped in silence.
#include <algotradelib/backtest>
#include <algotradelib/runner>

using namespace algotradelib;

struct BarOnly {
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}
};

void feeds_a_record(const strategy::DataEvent<int>& record) {
#if ATL_PROBE_VARIANT == 1
  runner::DryRunner<BarOnly> dry;
  dry.feed(record);
#elif ATL_PROBE_VARIANT == 2
  backtest::Backtester<BarOnly> engine(BarOnly{}, backtest::ReplayConfig{});
  engine.feed(record);
#endif
}
