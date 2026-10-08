// expect: SessionExecutor sends orders, and this session's client is a market-data source
// Deliberately does not compile. `data-only-refused-as-executor` builds this file and passes only when the
// diagnostic is SessionExecutor's assertion: a market-data source wired where orders are sent is a
// program that would run and trade nothing, so it must not build.
#include "data_only_vendor.hpp"

#include <algotradelib/live>

void wire(BlockingSession<vendor::BlockingClient>& session) {
  live::SessionExecutor<BlockingSession<vendor::BlockingClient>> executor(session);
  static_cast<void>(executor);
}
