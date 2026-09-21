#pragma once

#include "./scalar.hpp"

#include <Eigen/Dense>

namespace algotradelib {
namespace analytics {

/**
 * The one header in this library that names the linear algebra backend.
 *
 * Everything public in the analytics layer is written against the aliases below, so a
 * caller writes `analytics::SeriesView` rather than an `Eigen::` type and a second
 * backend is a new definition of this header rather than a new public surface. That is
 * the whole reason the build option is named for the backend: it is expected to change.
 *
 * `Eigen::` may appear inside an implementation, but not in a signature a caller writes
 * against.
 */

/**
 * Dense two-dimensional storage, **column-major**.
 *
 * Column-major is load-bearing rather than a default taken. Every frame in this layer
 * puts time on the rows and a series on a column, so one series is contiguous and binds
 * to `SeriesView` with no copy and no stride. Row-major would make every indicator walk
 * a strided view of its own input.
 */
using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor>;

/** Dense one-dimensional storage. */
using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

/**
 * A read-only reference to one series, without copying it.
 *
 * Binds to a `Vector`, to a column of a `Matrix`, and to an expression. This is the
 * parameter type an indicator taking one series should accept — it is a reference, not
 * an adopter of foreign storage, which is what `Eigen::Map` is for.
 */
using SeriesView = Eigen::Ref<const Vector>;

/** A writable reference to one series, without copying it. */
using MutableSeriesView = Eigen::Ref<Vector>;

/** A read-only reference to a whole frame's storage, without copying it. */
using MatrixView = Eigen::Ref<const Matrix>;

/** Returns a vector of `count` values, every one of them unobserved. */
inline Vector unobserved(std::size_t count) {
  return Vector::Constant(static_cast<Eigen::Index>(count), missing());
}

} // namespace analytics
} // namespace algotradelib
