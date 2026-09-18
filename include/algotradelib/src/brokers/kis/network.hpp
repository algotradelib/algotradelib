#pragma once

#include <algotradelib/network>

namespace algotradelib {
namespace kis {

/** Blocking HTTP transport retained for the compatibility authentication API. */
using KisHttpClient = network::HttpClient;

} // namespace kis
} // namespace algotradelib
