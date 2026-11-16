// expect: deleted function[^\n]*measure_input
// Deliberately does not compile. `measure-input-refuses-a-temporary-result` builds this file and passes only when
// the diagnostic is the deleted overload: an input built from a temporary `ReplayResult` would refer to a result
// that is gone before any measure reads it.
#include <algotradelib/backtest>

using namespace algotradelib;

double reads_a_temporary(const backtest::NextBarFillModel& fills) {
  const auto in = backtest::measure_input(backtest::ReplayResult{}, fills);
  return in.periods_per_year_;
}
