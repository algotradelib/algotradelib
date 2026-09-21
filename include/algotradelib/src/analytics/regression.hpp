#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * Regression and covariance, the pieces a factor model or an optimizer consumes.
 *
 * Two conventions specific to this header, both of which exist because the alternative is a
 * silent error rather than a wrong number:
 *
 * 1. **No intercept is added for you.** A design matrix means exactly the columns it holds.
 *    `with_intercept` prepends a column of ones when one is wanted, which makes the choice
 *    visible at the call site. A library that added one silently would surprise a caller
 *    fitting a factor model with no intercept by construction, and one that never added one
 *    would let a caller forget — so the helper is named and the omission is documented.
 * 2. **A row is used only if every value in it is observed.** Listwise deletion, not
 *    pairwise: a regression needs whole rows, and quietly filling a gap with a mean or a
 *    zero would put a fabricated observation into the fit. Dropped rows come back as
 *    `missing()` in the fitted values and residuals, so those stay as long as the input and
 *    line up with the same index.
 */

/**
 * Returns `x` with a leading column of ones, the design matrix of a regression with
 * an intercept.
 *
 * Copies, because the result has a different shape than its input and there is nothing to
 * reference. Called once per fit, not per row.
 */
inline math::Matrix with_intercept(math::MatrixView x) {
  math::Matrix result(x.rows(), x.cols() + 1);
  result.col(0).setOnes();
  if (x.cols() > 0) {
    result.rightCols(x.cols()) = x;
  }
  return result;
}

/** What a fit produced, including what it could not do. */
struct Regression {
  /** One coefficient per design column, in the order the columns were given. */
  math::Vector coefficients_;
  /** The fitted value of each input row; `missing()` on a row the fit could not use. */
  math::Vector fitted_;
  /** Observed minus fitted, on the same rows. */
  math::Vector residuals_;
  /** Fraction of the variance of the used rows the fit explains. */
  Scalar r_squared_{0};
  /** How many whole rows took part. */
  std::size_t observations_{0};
  /** Numerical rank of the design matrix restricted to those rows. */
  std::size_t rank_{0};
  /**
   * Whether the design had full column rank.
   *
   * Worth checking. With this false the coefficients are one solution of infinitely many,
   * the fitted values are still meaningful, and the individual coefficients are not —
   * which is exactly the situation in which a factor model reports a large loading on a
   * factor that is really a combination of two others.
   */
  bool full_rank_{false};
};

namespace regression_detail {

/** Returns the input rows on which the response and every design column are observed. */
inline std::vector<math::Index> usable_rows(math::SeriesView y, math::MatrixView x) {
  std::vector<math::Index> rows;
  rows.reserve(static_cast<std::size_t>(y.size()));
  for (math::Index row = 0; row < y.size(); ++row) {
    if (!observed(y[row])) {
      continue;
    }
    bool whole = true;
    for (math::Index column = 0; column < x.cols(); ++column) {
      if (!observed(x(row, column))) {
        whole = false;
        break;
      }
    }
    if (whole) {
      rows.push_back(row);
    }
  }
  return rows;
}

/** Throws unless a response and a design matrix describe the same rows. */
inline void require_same_rows(math::SeriesView y, math::MatrixView x) {
  if (y.size() != x.rows()) {
    throw std::invalid_argument(
      "response and design matrix must have the same number of rows"
    );
  }
}

/**
 * Fills the fitted values, residuals, and R² of a completed fit.
 *
 * R² is computed against the mean of the rows that took part, not of the whole input. A
 * total sum of squares of zero -- a constant response -- leaves it unobserved: the fit
 * explains none of a variance that does not exist, and reporting either 0 or 1 would be a
 * claim about fit quality that the data cannot support.
 */
inline void score(
  Regression& fit,
  math::SeriesView y,
  math::MatrixView x,
  const std::vector<math::Index>& rows
) {
  const auto count = static_cast<std::size_t>(y.size());
  fit.fitted_ = math::unobserved(count);
  fit.residuals_ = math::unobserved(count);
  fit.observations_ = rows.size();

  Scalar sum{0};
  for (const math::Index row : rows) {
    Scalar prediction{0};
    for (math::Index column = 0; column < x.cols(); ++column) {
      prediction += fit.coefficients_[column] * x(row, column);
    }
    fit.fitted_[row] = prediction;
    fit.residuals_[row] = y[row] - prediction;
    sum += y[row];
  }

  if (rows.empty()) {
    fit.r_squared_ = missing();
    return;
  }
  const Scalar mean = sum / static_cast<Scalar>(rows.size());
  Scalar total{0};
  Scalar unexplained{0};
  for (const math::Index row : rows) {
    const Scalar centered = y[row] - mean;
    total += centered * centered;
    unexplained += fit.residuals_[row] * fit.residuals_[row];
  }
  fit.r_squared_ = total == Scalar{0} ? missing() : Scalar{1} - unexplained / total;
}

} // namespace regression_detail

/**
 * Fits `y` on the columns of `x` by ordinary least squares.
 *
 * Solved through the column-pivoting QR in `math::least_squares`, not through the normal
 * equations. On market data the distinction is not academic: two nearly collinear factor
 * columns are the normal case, the normal equations square the condition number, and the
 * QR also yields the rank, which is the only honest way to report that the factors were not
 * independent.
 *
 * No intercept is added; see `with_intercept`. Rows with any unobserved value are dropped,
 * and the fitted values and residuals carry `missing()` there so they stay aligned with the
 * input index.
 *
 * @throws std::invalid_argument if the response and design matrix disagree on row count, or
 * if the design has no columns.
 */
inline Regression ols(math::SeriesView y, math::MatrixView x) {
  regression_detail::require_same_rows(y, x);
  if (x.cols() == 0) {
    throw std::invalid_argument("design matrix must have at least one column");
  }

  const auto rows = regression_detail::usable_rows(y, x);
  Regression fit;
  if (rows.empty()) {
    fit.coefficients_ = math::unobserved(static_cast<std::size_t>(x.cols()));
    regression_detail::score(fit, y, x, rows);
    return fit;
  }

  math::Matrix design(static_cast<math::Index>(rows.size()), x.cols());
  math::Vector response(static_cast<math::Index>(rows.size()));
  for (std::size_t at = 0; at < rows.size(); ++at) {
    const auto target = static_cast<math::Index>(at);
    design.row(target) = x.row(rows[at]);
    response[target] = y[rows[at]];
  }

  const auto solved = math::least_squares(design, response);
  fit.coefficients_ = solved.coefficients_;
  fit.rank_ = static_cast<std::size_t>(solved.rank_);
  fit.full_rank_ = solved.full_rank_;
  regression_detail::score(fit, y, x, rows);
  return fit;
}

/**
 * Fits `y` on the columns of `x` with an L2 penalty of `penalty` on the coefficients.
 *
 * Solves `(XᵀX + penalty · I) b = Xᵀy` by Cholesky factorization. The normal equations are
 * acceptable *here*, unlike in `ols`, precisely because the penalty is what makes the system
 * well conditioned — that is the whole point of ridge, and a positive penalty guarantees the
 * matrix is positive definite even when `X` is rank deficient.
 *
 * **The penalty applies to every column, an intercept column included.** That is the
 * standard formulation and also a standard mistake: penalizing an intercept shrinks the
 * fitted level toward zero rather than toward the mean of the response, which biases every
 * prediction. Center the response and the columns and fit without an intercept, or accept the
 * bias knowingly. Nothing here can tell which column a caller meant as an intercept, so it is
 * documented rather than guessed at.
 *
 * `penalty` of zero is refused rather than silently becoming a badly conditioned `ols`.
 *
 * @throws std::invalid_argument on mismatched rows, an empty design, or a non-positive
 * penalty.
 * @throws std::runtime_error if the factorization fails, which with a positive penalty means
 * the inputs were not finite rather than that the system was singular.
 */
inline Regression ridge(math::SeriesView y, math::MatrixView x, Scalar penalty) {
  regression_detail::require_same_rows(y, x);
  if (x.cols() == 0) {
    throw std::invalid_argument("design matrix must have at least one column");
  }
  if (!(penalty > Scalar{0})) {
    throw std::invalid_argument("ridge penalty must be positive");
  }

  const auto rows = regression_detail::usable_rows(y, x);
  Regression fit;
  if (rows.empty()) {
    fit.coefficients_ = math::unobserved(static_cast<std::size_t>(x.cols()));
    regression_detail::score(fit, y, x, rows);
    return fit;
  }

  math::Matrix design(static_cast<math::Index>(rows.size()), x.cols());
  math::Vector response(static_cast<math::Index>(rows.size()));
  for (std::size_t at = 0; at < rows.size(); ++at) {
    const auto target = static_cast<math::Index>(at);
    design.row(target) = x.row(rows[at]);
    response[target] = y[rows[at]];
  }

  math::Matrix normal = design.transpose() * design;
  normal.diagonal().array() += penalty;
  const auto solved = math::solve_positive_definite(normal, design.transpose() * response);
  if (!solved.definite_) {
    throw std::runtime_error("ridge factorization failed; the inputs are not finite");
  }
  fit.coefficients_ = solved.values_;
  fit.rank_ = static_cast<std::size_t>(x.cols());
  fit.full_rank_ = true;
  regression_detail::score(fit, y, x, rows);
  return fit;
}

/**
 * Returns the sample covariance matrix of the columns of `observations`.
 *
 * Rows are observations, columns are variables, matching every frame and panel in this
 * layer. Divides by `count - 1`, consistent with `rolling_stdev` and `RunningMoments`.
 *
 * Unobserved values are handled **pairwise**: each entry uses the rows on which both of its
 * two variables are observed, and an entry with fewer than two such rows is `missing()`.
 * Pairwise rather than listwise because a panel of a few hundred instruments with scattered
 * gaps can easily have no complete row at all, which would make listwise deletion return
 * nothing.
 *
 * **The cost of that choice, stated plainly: the result need not be positive semi-definite.**
 * Different entries describe different samples, so the matrix can have negative eigenvalues,
 * and an optimizer handed one will happily report a portfolio with negative variance. That is
 * what `nearest_positive_definite` is for, and running it over this result is the expected
 * next step rather than a remedy for something that went wrong.
 */
inline math::Matrix covariance(math::MatrixView observations) {
  const math::Index variables = observations.cols();
  math::Matrix result = math::unobserved(
    static_cast<std::size_t>(variables), static_cast<std::size_t>(variables)
  );

  for (math::Index left = 0; left < variables; ++left) {
    for (math::Index right = left; right < variables; ++right) {
      std::size_t count = 0;
      Scalar sum_left{0};
      Scalar sum_right{0};
      for (math::Index row = 0; row < observations.rows(); ++row) {
        if (!observed(observations(row, left)) || !observed(observations(row, right))) {
          continue;
        }
        ++count;
        sum_left += observations(row, left);
        sum_right += observations(row, right);
      }
      if (count < 2) {
        continue;
      }
      const Scalar mean_left = sum_left / static_cast<Scalar>(count);
      const Scalar mean_right = sum_right / static_cast<Scalar>(count);
      Scalar product{0};
      for (math::Index row = 0; row < observations.rows(); ++row) {
        if (!observed(observations(row, left)) || !observed(observations(row, right))) {
          continue;
        }
        product += (observations(row, left) - mean_left) *
          (observations(row, right) - mean_right);
      }
      const Scalar entry = product / static_cast<Scalar>(count - 1);
      result(left, right) = entry;
      result(right, left) = entry;
    }
  }
  return result;
}

/**
 * Returns the exponentially weighted covariance matrix of the columns of `observations`.
 *
 * Weight `(1 - decay) · decay^k` on the observation `k` rows back from the last, normalized
 * over the weights actually used. `decay = 0.94` is RiskMetrics' daily figure, which is why
 * it is the default; `0.97` is their monthly one.
 *
 * Deviations are taken from the **weighted** mean of each pair's rows, not the unweighted
 * one. Mixing the two is a common shortcut that biases the result whenever the series has
 * any trend across the window, because the recent rows that dominate the weighting are not
 * centered on the mean being subtracted.
 *
 * Divides by the sum of weights, so the result is the weighted second moment rather than an
 * unbiased sample estimate; there is no `count - 1` correction that means anything once the
 * observations are unequally weighted. It carries the same pairwise caveat as `covariance`
 * and the same remedy.
 *
 * @throws std::invalid_argument unless `decay` lies strictly between 0 and 1. At 1 every
 * weight is equal and no rows are discounted; at 0 only the last row counts, and neither is
 * a decay.
 */
inline math::Matrix ewma_covariance(math::MatrixView observations, Scalar decay = Scalar{0.94}) {
  if (!(decay > Scalar{0}) || !(decay < Scalar{1})) {
    throw std::invalid_argument("ewma decay must lie strictly between 0 and 1");
  }

  const math::Index rows = observations.rows();
  const math::Index variables = observations.cols();
  math::Vector weights(rows);
  for (math::Index row = 0; row < rows; ++row) {
    // Row `rows - 1` is the most recent and carries the largest weight.
    weights[row] = std::pow(decay, static_cast<Scalar>(rows - 1 - row));
  }

  math::Matrix result = math::unobserved(
    static_cast<std::size_t>(variables), static_cast<std::size_t>(variables)
  );
  for (math::Index left = 0; left < variables; ++left) {
    for (math::Index right = left; right < variables; ++right) {
      Scalar total{0};
      Scalar sum_left{0};
      Scalar sum_right{0};
      std::size_t count = 0;
      for (math::Index row = 0; row < rows; ++row) {
        if (!observed(observations(row, left)) || !observed(observations(row, right))) {
          continue;
        }
        ++count;
        total += weights[row];
        sum_left += weights[row] * observations(row, left);
        sum_right += weights[row] * observations(row, right);
      }
      if (count < 2 || total == Scalar{0}) {
        continue;
      }
      const Scalar mean_left = sum_left / total;
      const Scalar mean_right = sum_right / total;
      Scalar product{0};
      for (math::Index row = 0; row < rows; ++row) {
        if (!observed(observations(row, left)) || !observed(observations(row, right))) {
          continue;
        }
        product += weights[row] * (observations(row, left) - mean_left) *
          (observations(row, right) - mean_right);
      }
      const Scalar entry = product / total;
      result(left, right) = entry;
      result(right, left) = entry;
    }
  }
  return result;
}

/**
 * Returns the nearest matrix to `matrix` whose eigenvalues are at least `floor`.
 *
 * The input is symmetrized as `(A + Aᵀ) / 2`, decomposed, its eigenvalues clipped up to
 * `floor`, and reassembled. "Nearest" is exact rather than approximate: among all symmetric
 * matrices with eigenvalues at or above `floor`, the clipped one is the unique minimizer of
 * the Frobenius distance.
 *
 * **What it does not preserve: the diagonal.** Clipping raises the trace, so a *correlation*
 * matrix comes back with a diagonal slightly above one. If the unit diagonal has to hold as
 * well, the projection is onto the intersection of two sets and takes Higham's alternating
 * iteration, which is a different and more expensive algorithm; the honest shortcut for a
 * correlation matrix is to rescale the result by its own diagonal afterwards. For a
 * *covariance* matrix the diagonals are variances with no constraint to preserve, which is
 * the case this is written for and the case a pairwise `covariance` produces.
 *
 * A `floor` of exactly zero gives a positive *semi*-definite result, which is enough for an
 * optimizer that only needs non-negative variances; a small positive floor is what makes it
 * invertible.
 *
 * @throws std::invalid_argument if `matrix` is not square or `floor` is negative.
 * @throws std::runtime_error if the eigendecomposition does not converge, which for a finite
 * symmetric matrix does not happen and therefore means the input was not finite.
 */
inline math::Matrix nearest_positive_definite(
  math::MatrixView matrix,
  Scalar floor = Scalar{0}
) {
  if (matrix.rows() != matrix.cols()) {
    throw std::invalid_argument("nearest positive definite needs a square matrix");
  }
  if (!(floor >= Scalar{0})) {
    throw std::invalid_argument("eigenvalue floor must not be negative");
  }

  const math::Matrix symmetric =
    (matrix + matrix.transpose()) * Scalar{0.5};
  const auto decomposed = math::symmetric_eigen(symmetric);
  if (!decomposed.converged_) {
    throw std::runtime_error("eigendecomposition did not converge; the input is not finite");
  }

  math::Vector clipped = decomposed.values_;
  for (math::Index at = 0; at < clipped.size(); ++at) {
    if (clipped[at] < floor) {
      clipped[at] = floor;
    }
  }
  return decomposed.vectors_ * clipped.asDiagonal() * decomposed.vectors_.transpose();
}

} // namespace analytics
} // namespace algotradelib
