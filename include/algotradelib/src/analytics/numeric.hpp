#pragma once

#include "../math/matrix.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

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

namespace constant_detail {

// Whether a deviation computed about `mean` over `count` observations is small enough to be
// rounding alone. Equal observations that are not exact binary fractions have a computed mean an
// ulp or so away from them (the mean of {0.1, 0.1, 0.1} is not 0.1), so their deviation comes out a
// few ulps of the mean rather than zero. The bound is a generous relative one, `8 * count * epsilon`
// of the mean's magnitude, against a summation error of about `count * epsilon / 2`.
inline bool within_rounding(Scalar deviation, Scalar mean, std::size_t count) {
  return deviation <=
    std::abs(mean) * static_cast<Scalar>(count) * Scalar{8} * std::numeric_limits<Scalar>::epsilon();
}

// Whether every observed row of `values` in `[first, first + length)` holds the same value;
// `present` is the presence mask `math::Observed` gives (`1` observed, `0` not).
inline bool all_equal(
  const math::Vector& values,
  const math::Vector& present,
  math::Index first,
  math::Index length
) {
  bool seen = false;
  Scalar reference{0};
  for (math::Index row = first; row < first + length; ++row) {
    if (present[row] == Scalar{0}) {
      continue;
    }
    if (!seen) {
      reference = values[row];
      seen = true;
    } else if (values[row] != reference) {
      return false;
    }
  }
  return true;
}

// Whether a window whose deviation about `mean` came out rounding-small holds equal observations,
// checked value by value. Each caller keeps its own exact-zero test beside it: this one never
// reads a zero as equality by itself, since a positive dispersion can underflow to a zero deviation.
inline bool rounded_constant(
  Scalar deviation,
  Scalar mean,
  std::size_t count,
  const math::Vector& values,
  const math::Vector& present,
  math::Index first,
  math::Index length
) {
  return within_rounding(deviation, mean, count) && all_equal(values, present, first, length);
}

} // namespace constant_detail

} // namespace analytics
} // namespace algotradelib
