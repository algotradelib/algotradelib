#pragma once

#include <algotradelib/network>

namespace algotradelib {
namespace tossinvest {

/** Blocking HTTP transport retained for the compatibility authentication API. */
using TossHttpClient = network::HttpClient;

} // namespace tossinvest
} // namespace algotradelib
