#pragma once

#include "../core/broker_list.hpp"
#include "./kis/traits.hpp"
#include "./kiwoom/traits.hpp"
#include "./tossinvest/traits.hpp"

namespace algotradelib {

/**
 * @brief The library's blocking broker clients, for `with_broker`; including it parses all three adapters. A
 *   broker added later joins it: a consumer that keeps each broker in its own translation unit then adds one.
 */
using builtin_brokers = type_list<kis::KisBlockingClient, kiwoom::KiwoomBlockingClient, tossinvest::TossBlockingClient>;

} // namespace algotradelib
