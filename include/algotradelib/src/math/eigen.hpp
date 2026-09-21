#pragma once

#include "./scalar.hpp"

#include <Eigen/Dense>

#include <cstddef>

namespace algotradelib {
namespace math {

/**
 * The linear algebra vocabulary, implemented by Eigen.
 *
 * This is the only header in the library that names Eigen. Everything above is written
 * against the aliases below, so a caller writes `math::SeriesView` rather than an
 * `Eigen::` type, and a second backend is a sibling of this header — `math/armadillo.hpp`,
 * or an implementation of our own — selected by `math/matrix.hpp` rather than a second
 * public surface.
 *
 * `Eigen::` may appear inside an implementation, but not in a signature a caller writes
 * against.
 */

/**
 * Dense two-dimensional storage, **column-major**.
 *
 * Column-major is load-bearing rather than a default taken. Every frame puts time on the
 * rows and a series on a column, so one series is contiguous and binds to `SeriesView`
 * with no copy and no stride. Row-major would make every computation walk a strided view
 * of its own input.
 */
using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor>;

/** Dense one-dimensional storage. */
using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

/**
 * A read-only reference to one series, without copying it.
 *
 * Binds to a `Vector`, to a column of a `Matrix`, and to an expression. This is the
 * parameter type a computation over one series should accept — it is a reference, not an
 * adopter of foreign storage, which is what `Eigen::Map` is for.
 */
using SeriesView = Eigen::Ref<const Vector>;

/** A writable reference to one series, without copying it. */
using MutableSeriesView = Eigen::Ref<Vector>;

/** A read-only reference to whole two-dimensional storage, without copying it. */
using MatrixView = Eigen::Ref<const Matrix>;

/** Index type the backend addresses storage with. */
using Index = Eigen::Index;

/** Returns a vector of `count` values, every one of them unobserved. */
inline Vector unobserved(std::size_t count) {
  return Vector::Constant(static_cast<Index>(count), missing());
}

/** Returns storage of `rows` by `columns`, every value unobserved. */
inline Matrix unobserved(std::size_t rows, std::size_t columns) {
  return Matrix::Constant(static_cast<Index>(rows), static_cast<Index>(columns), missing());
}

} // namespace math
} // namespace algotradelib
