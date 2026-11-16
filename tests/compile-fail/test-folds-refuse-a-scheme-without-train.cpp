// expect: a fold scheme needs the member IndexRange train
// Deliberately does not compile. `folds-refuse-a-scheme-without-train` builds this file and passes only when the
// diagnostic is the `static_assert` in `folds`, naming the member a scheme must have.
#include <algotradelib/backtest>

using namespace algotradelib;

/** Names its member differently, so `folds` cannot call it. */
struct TrainsWrongly {
  backtest::IndexRange training(std::size_t test_begin, const backtest::FoldLengths&) const {
    return backtest::IndexRange{0, test_begin};
  }
};

std::size_t lists_folds() {
  return backtest::folds(100, backtest::FoldLengths{10, 5, 5, 0}, TrainsWrongly{}).size();
}
