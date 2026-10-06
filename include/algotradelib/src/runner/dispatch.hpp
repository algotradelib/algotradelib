#pragma once

#include "../strategy/concept.hpp"

namespace algotradelib {
namespace runner {

/** @brief `strategy::dispatch` under the runner's namespace: calls a strategy's declared handlers. */
namespace dispatch = strategy::dispatch;

} // namespace runner
} // namespace algotradelib
