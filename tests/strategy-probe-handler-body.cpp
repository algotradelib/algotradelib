// Deliberately does not compile. `strategy-compiles-handler-body-<n>` builds this file with
// ATL_PROBE_HANDLER = n (1 to 7) and passes only when the diagnostic is the error in that one handler's
// body. The handler's signature is valid and nothing feeds it an event: only the assertion's compiling of
// every handler call reaches the body, so dropping that handler's call from the assertion lets this build.
#include <algotradelib/strategy>

using namespace algotradelib;

struct Probe {
#if ATL_PROBE_HANDLER == 1
  template <class Ctx> void on_start(Ctx& ctx) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 2
  template <class Ctx> void on_stop(Ctx& ctx) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 3
  template <class Ctx> void on_bar(Ctx& ctx, const strategy::BarEvent&) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 4
  template <class Ctx> void on_quote(Ctx& ctx, const strategy::QuoteEvent&) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 5
  template <class Ctx> void on_fill(Ctx& ctx, const strategy::Fill&) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 6
  template <class Ctx> void on_order_update(Ctx& ctx, const OrderSnapshot&) { ctx.no_such_member(); }
#elif ATL_PROBE_HANDLER == 7
  template <class Ctx> void on_phase(Ctx& ctx, const strategy::MarketPhase&) { ctx.no_such_member(); }
#endif
};

ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
