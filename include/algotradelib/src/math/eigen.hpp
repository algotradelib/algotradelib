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

/**
 * The decompositions the layers above need, as free functions.
 *
 * These exist for the same reason the type aliases do. A regression needs a QR and a
 * covariance repair needs a symmetric eigendecomposition, and if the callers named
 * `Eigen::ColPivHouseholderQR` directly then Eigen would be spread across the analytics
 * layer and a second backend would be a rewrite rather than a sibling header. So the seam
 * carries the *operations* as well as the types, and stays the one place Eigen appears.
 *
 * Each reports failure in its result rather than throwing or returning something plausible.
 * A rank-deficient design matrix and a covariance matrix that is not positive definite are
 * both ordinary conditions in market data -- more instruments than observations, or a pair
 * of instruments that moved identically -- not exceptional ones.
 */

/** A least-squares solution together with what the decomposition learned about the fit. */
struct LeastSquares {
  Vector coefficients_;
  /** Numerical rank of the design matrix, from the pivoting. */
  Index rank_{0};
  /** Whether the design matrix had full column rank, so the solution is unique. */
  bool full_rank_{false};
};

/**
 * Solves `min ||a x - b||` by column-pivoting Householder QR.
 *
 * Column pivoting rather than the plain QR or the normal equations. `a` is a design matrix
 * of market data, where two columns being nearly collinear is the normal case rather than a
 * pathology -- two instruments in the same sector, or a factor that is almost a combination
 * of two others. The normal equations square the condition number, which for such a matrix
 * is the difference between a usable answer and noise; pivoting also produces the rank,
 * which is the only honest way to tell a caller that its factors were not independent.
 */
inline LeastSquares least_squares(MatrixView a, SeriesView b) {
  LeastSquares result;
  Eigen::ColPivHouseholderQR<Matrix> decomposition(a);
  result.rank_ = decomposition.rank();
  result.full_rank_ = result.rank_ == a.cols();
  result.coefficients_ = decomposition.solve(b);
  return result;
}

/** A solution of a symmetric positive-definite system, and whether it was one. */
struct PositiveDefiniteSolution {
  Vector values_;
  /** False when the Cholesky factorization failed, which means `a` was not definite. */
  bool definite_{false};
};

/**
 * Solves `a x = b` for symmetric positive-definite `a` by Cholesky factorization.
 *
 * Reports `definite_ == false` rather than throwing when the factorization fails, and leaves
 * `values_` empty. A caller that ignored the flag and used the values would be acting on a
 * solution to a system that has none.
 */
inline PositiveDefiniteSolution solve_positive_definite(MatrixView a, SeriesView b) {
  PositiveDefiniteSolution result;
  Eigen::LLT<Matrix> factorization(a);
  if (factorization.info() != Eigen::Success) {
    return result;
  }
  result.values_ = factorization.solve(b);
  result.definite_ = true;
  return result;
}

/** A symmetric eigendecomposition: `a == vectors_ * values_.asDiagonal() * vectors_^T`. */
struct SymmetricEigen {
  /** Eigenvalues in ascending order. */
  Vector values_;
  /** The corresponding orthonormal eigenvectors, one per column. */
  Matrix vectors_;
  bool converged_{false};
};

/**
 * Decomposes a symmetric matrix into its eigenvalues and eigenvectors.
 *
 * Only the lower triangle is read, so a matrix that is symmetric up to rounding is handled
 * without the caller having to symmetrize it first.
 */
inline SymmetricEigen symmetric_eigen(MatrixView a) {
  SymmetricEigen result;
  Eigen::SelfAdjointEigenSolver<Matrix> solver(a);
  if (solver.info() != Eigen::Success) {
    return result;
  }
  result.values_ = solver.eigenvalues();
  result.vectors_ = solver.eigenvectors();
  result.converged_ = true;
  return result;
}

} // namespace math
} // namespace algotradelib
