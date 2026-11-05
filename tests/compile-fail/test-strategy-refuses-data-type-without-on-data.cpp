// expect: this strategy declares no on_data, so no record would reach it
// Deliberately does not compile. `strategy-refuses-data-type-without-on-data` builds this file and passes only
// when the diagnostic is the `data_type` with no handler: a misspelled `on_data` would otherwise take no record.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  using data_type = int;
  template <class Ctx>
  void on_bar(Ctx&, const strategy::BarEvent&) {}
  template <class Ctx>
  void on_dta(Ctx&, const strategy::DataEvent<int>&) {}
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
