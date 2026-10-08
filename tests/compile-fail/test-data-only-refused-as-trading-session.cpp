// expect: SessionSource's first session is the trading session
// Deliberately does not compile. `data-only-refused-as-trading-session` builds this file and passes only when
// the diagnostic is SessionSource's assertion: the first session is read for holdings and orders, which a
// market-data source does not have.
#include "data_only_vendor.hpp"

#include <algotradelib/live>

void wire(BlockingSession<vendor::BlockingClient>& session) {
  live::SessionSource<BlockingSession<vendor::BlockingClient>> source(session, {});
  static_cast<void>(source);
}
