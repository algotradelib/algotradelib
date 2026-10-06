#pragma once

#include "../strategy/concept.hpp"

namespace algotradelib {
namespace runner {

/**
 * The calls a runner makes to a strategy's handlers: `strategy::dispatch`, which calls a handler when the
 * strategy declares its name. Defined beside the strategy surface so `ATL_ASSERT_STRATEGY` can compile the
 * same calls; named here for the runners and for a wrapper forwarding to an inner strategy.
 */
namespace dispatch = strategy::dispatch;

} // namespace runner
} // namespace algotradelib
