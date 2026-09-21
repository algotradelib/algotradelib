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
 * Triangular and self-adjoint view modes.
 *
 * Aliased as plain constants so a caller writes `matrix.selfadjointView<math::LOWER>()`
 * without naming Eigen. A self-adjoint view is worth reaching for: it tells a decomposition
 * to read one triangle and assume the other, which halves the traffic and removes any
 * question of what a not-quite-symmetric input meant.
 */
inline constexpr unsigned int LOWER = Eigen::Lower;
inline constexpr unsigned int UPPER = Eigen::Upper;
inline constexpr unsigned int UNIT_LOWER = Eigen::UnitLower;
inline constexpr unsigned int UNIT_UPPER = Eigen::UnitUpper;

/**
 * The decompositions, as aliases rather than as wrappers.
 *
 * These are Eigen's own decomposition classes, named through this header so that callers
 * still write `math::` and a different backend could alias its own equivalents. That is the
 * honest extent of the seam here: for the numeric *vocabulary* the aliases really are
 * portable, because every backend has a dense matrix and a vector; for decompositions they
 * are a naming convention, because no two libraries agree on what a factorization object
 * exposes. Pretending otherwise by wrapping each one in a result struct would cost more than
 * it bought -- it would copy the factors out, discard the rank, the permutation, and the
 * conditioning the decomposition already knows, and allow only one right-hand side per
 * factorization.
 *
 * So callers use them directly, and get everything Eigen offers:
 *
 * ```cpp
 * // One factorization, many right-hand sides. `b` may be a vector or a matrix, and the
 * // solution comes back with the matching shape -- which is what a multivariate regression
 * // or a whole panel of targets needs.
 * const math::ColPivQr qr(a);
 * const math::Matrix coefficients = qr.solve(b);
 * if (qr.rank() < a.cols()) { ... }  // the columns were not independent
 *
 * // Or pick a different trade-off without a new function: LLT for a symmetric positive
 * // definite system, LDLT when it is only semi-definite, SVD when the answer must be the
 * // minimum-norm one, HouseholderQr when pivoting is not needed.
 * const math::Llt chol(normal);
 * if (chol.info() == math::SUCCESS) { x = chol.solve(b); }
 * ```
 *
 * `Matrix::ldlt()`, `Matrix::colPivHouseholderQr()` and the other member forms work too, as
 * does `A.fullPivLu().solve(b)` for a square system where no structure is known.
 */

/** Cholesky, for a symmetric positive-definite matrix. Check `info()`. */
using Llt = Eigen::LLT<Matrix>;

/** Cholesky with pivoting, for a symmetric matrix that may only be semi-definite. */
using Ldlt = Eigen::LDLT<Matrix>;

/** Householder QR. Fastest of the QRs, and gives no rank information. */
using HouseholderQr = Eigen::HouseholderQR<Matrix>;

/**
 * Column-pivoting Householder QR: a rank and a least-squares solution.
 *
 * The default choice for a design matrix of market data, where two nearly collinear columns
 * are the normal case rather than a pathology. The normal equations square the condition
 * number, which for such a matrix is the difference between a usable answer and noise, and
 * they cannot report a rank at all.
 */
using ColPivQr = Eigen::ColPivHouseholderQR<Matrix>;

/** Full-pivoting QR. Slower than `ColPivQr` and the most reliable rank of the three. */
using FullPivQr = Eigen::FullPivHouseholderQR<Matrix>;

/** LU with full pivoting, for a square system with no exploitable structure. */
using FullPivLu = Eigen::FullPivLU<Matrix>;

/**
 * Divide-and-conquer SVD: the minimum-norm least-squares solution, and singular values.
 *
 * An SVD needs its thin factors computed before it can solve, and *how* they are requested
 * changed between the supported Eigen releases: 3.4 takes runtime flags the current release
 * deprecates in favour of a class template argument. `thin_svd` picks the right one, which is
 * the only reason a factory exists here at all.
 *
 * Reach for it when the answer must be the minimum-norm solution of a rank-deficient system,
 * or when the conditioning is itself the question -- `singularValues()` answers it.
 */
#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
using Svd = Eigen::BDCSVD<Matrix, Eigen::ComputeThinU | Eigen::ComputeThinV>;
#else
using Svd = Eigen::BDCSVD<Matrix>;
#endif

/** Returns a thin SVD of `a`, ready to `solve()`, on either supported Eigen release. */
inline Svd thin_svd(const Matrix& a) {
#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
  return Svd(a);
#else
  return Svd(a, Eigen::ComputeThinU | Eigen::ComputeThinV);
#endif
}

/** Eigenvalues and eigenvectors of a symmetric matrix, ascending. Check `info()`. */
using SelfAdjointEigen = Eigen::SelfAdjointEigenSolver<Matrix>;

/** The value a decomposition's `info()` returns when it succeeded. */
inline constexpr auto SUCCESS = Eigen::Success;

} // namespace math
} // namespace algotradelib
