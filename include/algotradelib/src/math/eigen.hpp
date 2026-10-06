#pragma once

#include "./scalar.hpp"

#include <Eigen/Dense>

#include <cstddef>

namespace algotradelib {
namespace math {

/**
 * @file
 * @brief The `math` types and decompositions, implemented with Eigen.
 *
 * Include `math/matrix.hpp` rather than this header. Write against the `math::` names
 * (`math::SeriesView`, `math::ColPivQr`, ...) rather than the Eigen types behind them.
 *
 * The decompositions are Eigen's own classes under `math::` names; use their Eigen interfaces
 * directly:
 *
 * ```cpp
 * // One factorization, many right-hand sides; `b` may be a vector or a matrix.
 * const math::ColPivQr qr(a);
 * const math::Matrix coefficients = qr.solve(b);
 * if (qr.rank() < a.cols()) { ... }  // the columns were not independent
 *
 * // LLT for a symmetric positive-definite system, LDLT when it is only semi-definite,
 * // Svd for the minimum-norm solution, HouseholderQr when pivoting is not needed.
 * const math::Llt chol(normal);
 * if (chol.info() == math::SUCCESS) { x = chol.solve(b); }
 * ```
 *
 * `Matrix::ldlt()`, `Matrix::colPivHouseholderQr()` and the other member forms work too, as
 * does `A.fullPivLu().solve(b)` for a square system where no structure is known.
 */
// The only header that names Eigen in a public signature; a second backend is a sibling header.

/**
 * @brief A dense, column-major matrix of `Scalar`.
 *
 * Frames put time on the rows and one series per column, so a column binds to `SeriesView`
 * without a copy.
 */
using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor>;

/** @brief A dense column vector of `Scalar`. */
using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

/**
 * @brief A read-only reference to one series.
 *
 * Binds without copying to a `Vector` or a column of a `Matrix`, and to an expression
 * (which is evaluated into a temporary). Functions over one series take this type.
 */
using SeriesView = Eigen::Ref<const Vector>;

/** @brief A writable reference to one series, such as a `Vector` or a column of a `Matrix`. */
using MutableSeriesView = Eigen::Ref<Vector>;

/** @brief A read-only reference to a whole matrix. */
using MatrixView = Eigen::Ref<const Matrix>;

/** @brief The signed index type of rows and columns. */
using Index = Eigen::Index;

/**
 * @brief Makes a vector filled with `missing()`.
 *
 * @param count The number of entries.
 * @return A vector of `count` unobserved values.
 */
inline Vector unobserved(std::size_t count) {
  return Vector::Constant(static_cast<Index>(count), missing());
}

/**
 * @brief Makes a matrix filled with `missing()`.
 *
 * @param rows The number of rows.
 * @param columns The number of columns.
 * @return A `rows` by `columns` matrix of unobserved values.
 */
inline Matrix unobserved(std::size_t rows, std::size_t columns) {
  return Matrix::Constant(static_cast<Index>(rows), static_cast<Index>(columns), missing());
}

/**
 * @brief A series split into zero-filled values and a presence mask, for sums that skip gaps.
 *
 * Made by `observed_parts()`. Over any range, `values_.segment(first, length).sum()` is the sum
 * of the observed entries and `present_.segment(first, length).sum()` is how many there are.
 * Deviations from a mean work the same way, because an absent entry contributes exactly zero:
 *
 * ```cpp
 * const Scalar squared = (parts.values_.segment(first, length) -
 *                         mean * parts.present_.segment(first, length)).squaredNorm();
 * ```
 */
struct Observed {
  /** @brief The input, with every unobserved entry replaced by `0`. */
  Vector values_;
  /** @brief `1` where the input is observed and `0` where it is not. */
  Vector present_;
};

/**
 * @brief Splits a series into zero-filled values and a presence mask.
 *
 * @param values The series; any `NaN` counts as unobserved.
 * @return The two parts; see `Observed`.
 */
inline Observed observed_parts(SeriesView values) {
  // `x == x` is false exactly for NaN, and vectorizes to a single compare.
  const auto present = (values.array() == values.array());
  Observed parts;
  parts.values_ = present.select(values, Scalar{0});
  parts.present_ = present.template cast<Scalar>();
  return parts;
}

/**
 * @brief Replaces every unobserved entry of a series.
 *
 * @param values The series; any `NaN` counts as unobserved.
 * @param filler The value to put in place of each unobserved entry.
 * @return A copy of `values` with no `NaN`.
 */
inline Vector observed_or(SeriesView values, Scalar filler) {
  return (values.array() == values.array()).select(values, filler);
}

/**
 * @brief Combines two presence masks: `1` only where both series are observed.
 *
 * Use it to restrict a co-moment (covariance, correlation) and both series' deviations to the
 * same rows; otherwise a correlation can fall outside `[-1, 1]`.
 *
 * @param left The first series' `Observed::present_`.
 * @param right The second series' `Observed::present_`, of the same length.
 * @return The elementwise product of the two masks.
 */
inline Vector both_present(const Vector& left, const Vector& right) {
  return left.cwiseProduct(right);
}

/**
 * @brief Selects the lower triangle, as in `matrix.selfadjointView<math::LOWER>()`.
 *
 * A self-adjoint view tells a decomposition to read one triangle and assume the other.
 */
inline constexpr unsigned int LOWER = Eigen::Lower;
/** @brief Selects the upper triangle. */
inline constexpr unsigned int UPPER = Eigen::Upper;
/** @brief Selects the lower triangle with an implicit unit diagonal. */
inline constexpr unsigned int UNIT_LOWER = Eigen::UnitLower;
/** @brief Selects the upper triangle with an implicit unit diagonal. */
inline constexpr unsigned int UNIT_UPPER = Eigen::UnitUpper;

/** @brief Cholesky decomposition of a symmetric positive-definite matrix. Check `info()`. */
using Llt = Eigen::LLT<Matrix>;

/** @brief Pivoting Cholesky (LDLT) decomposition of a symmetric, possibly semi-definite matrix. */
using Ldlt = Eigen::LDLT<Matrix>;

/** @brief Householder QR decomposition: the fastest QR, with no rank information. */
using HouseholderQr = Eigen::HouseholderQR<Matrix>;

/**
 * @brief Column-pivoting Householder QR: a least-squares solution and a rank.
 *
 * A good default for a regression on market data, where nearly collinear columns are common;
 * check `rank()` against the number of columns.
 */
using ColPivQr = Eigen::ColPivHouseholderQR<Matrix>;

/** @brief Full-pivoting Householder QR: slower than `ColPivQr`, with the most reliable rank. */
using FullPivQr = Eigen::FullPivHouseholderQR<Matrix>;

/** @brief LU decomposition with full pivoting, for a square system with no known structure. */
using FullPivLu = Eigen::FullPivLU<Matrix>;

/**
 * @brief Divide-and-conquer SVD: minimum-norm least-squares solutions and singular values.
 *
 * Construct it with `thin_svd()`, which requests the thin factors `solve()` needs on every
 * supported Eigen release.
 */
#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
using Svd = Eigen::BDCSVD<Matrix, Eigen::ComputeThinU | Eigen::ComputeThinV>;
#else
using Svd = Eigen::BDCSVD<Matrix>;
#endif

/**
 * @brief Computes a thin SVD, ready to `solve()`.
 *
 * @param a The matrix to decompose.
 * @return The decomposition of `a`.
 */
inline Svd thin_svd(const Matrix& a) {
#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
  return Svd(a);
#else
  return Svd(a, Eigen::ComputeThinU | Eigen::ComputeThinV);
#endif
}

/** @brief Eigenvalues (ascending) and eigenvectors of a symmetric matrix. Check `info()`. */
using SelfAdjointEigen = Eigen::SelfAdjointEigenSolver<Matrix>;

/** @brief The value a decomposition's `info()` returns on success. */
inline constexpr auto SUCCESS = Eigen::Success;

} // namespace math
} // namespace algotradelib
