// expect: a bootstrap join needs the member double join
// Deliberately does not compile. `bootstrap-refuses-a-join-without-join` builds this file and passes only when the
// diagnostic is the `static_assert` in `resample_closes`, naming the member a join must have.
#include <algotradelib/backtest>

#include <vector>

using namespace algotradelib;

/** Names its member differently, so `resample_closes` cannot call it. */
struct JoinsWrongly {
  double chain(double level, double from, double value) const {
    return level * (value / from);
  }
};

std::size_t resamples() {
  const std::vector<Decimal64> closes{Decimal64(1, 0), Decimal64(2, 0)};
  return backtest::resample_closes(closes, PriceRule::unconstrained(), {0}, JoinsWrongly{}).size();
}
