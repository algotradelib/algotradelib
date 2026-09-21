#pragma once

#include "../math/matrix.hpp"

namespace algotradelib {
namespace analytics {

/**
 * The numeric vocabulary, re-exported into this namespace.
 *
 * Kept apart from `conversion.hpp` on purpose. The conversion header is the bridge to the
 * trading domain and therefore needs `<algotradelib/core>`; a numeric kernel over a series
 * of scalars needs nothing from the trading domain at all, and pulling `Decimal64`,
 * `InstrumentId`, and the order types into every indicator header to reach three
 * using-declarations would be paying for a dependency none of them has.
 */

/** The numeric type this layer computes in, from the math module. */
using math::Scalar;

/** The value standing for "no observation". */
using math::missing;

/** Returns whether a value carries an observation. */
using math::observed;

} // namespace analytics
} // namespace algotradelib
