#pragma once

#include <algotradelib/network>

namespace algotradelib {
namespace kiwoom {

/** Blocking HTTP transport retained for the compatibility authentication API. */
using KiwoomHttpClient = network::HttpClient;

} // namespace kiwoom
} // namespace algotradelib
