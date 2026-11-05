// expect: a strategy declaring on_data names the record type it takes
// Deliberately does not compile. `strategy-refuses-on-data-without-data-type` builds this file and passes only
// when the diagnostic is the missing `data_type`: without it the handler's body could not be checked.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
  template <class Ctx>
  void on_data(Ctx&, const strategy::DataEvent<int>&) {}
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
