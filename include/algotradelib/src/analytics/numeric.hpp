#pragma once

#include "../math/matrix.hpp"

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Re-exports `math::Scalar`, `math::missing` and `math::observed` into `analytics`.
 */
// Kept apart from conversion.hpp so indicator headers do not include the trading types.

/** @brief The floating-point type analytics computes in; see `math::Scalar`. */
using math::Scalar;

/**
 * @brief Returns the value that stands for "no observation"; see `math::missing`.
 * @return A quiet `NaN`.
 */
using math::missing;

/**
 * @brief Tests whether a value is an observation; see `math::observed`.
 * @param value The value to test.
 * @return `false` for any `NaN`; `true` otherwise.
 */
using math::observed;

} // namespace analytics
} // namespace algotradelib
