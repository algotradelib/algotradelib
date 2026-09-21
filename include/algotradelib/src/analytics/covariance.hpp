#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * Covariance estimation, and the repair its own gaps make necessary.
 *
 * Deliberately *not* a regression header. Fitting `y` on `X` is one call to a decomposition
 * -- `math::ColPivQr(x).solve(y)`, with `y` a vector or a matrix -- and everything that
 * carries actual judgement happens before it: which predictors, over what estimation window,
 * how targets align to lagged features, what to do with an incomplete row. Those are
 * modelling decisions that belong to a strategy, not defaults a library should pick. See
 * `math/eigen.hpp` for the decompositions and which to reach for.
 *
 * What *is* here is knowledge about market data that is not a per-strategy choice: how to
 * estimate a covariance matrix when instruments have different gaps, how to weight recent
 * observations, and how to repair the result when the estimator's own deletion policy leaves
 * it indefinite.
 */

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

  // Symmetrized first, because the projection is defined for a symmetric matrix and an
  // input that is only nearly symmetric has to be told what it meant. Then read through a
  // self-adjoint view, so the solver takes one triangle and assumes the other instead of
  // re-examining both.
  const math::Matrix symmetric = (matrix + matrix.transpose()) * Scalar{0.5};
  const math::SelfAdjointEigen decomposed(symmetric.selfadjointView<math::LOWER>());
  if (decomposed.info() != math::SUCCESS) {
    throw std::runtime_error("eigendecomposition did not converge; the input is not finite");
  }

  math::Vector clipped = decomposed.eigenvalues();
  for (math::Index at = 0; at < clipped.size(); ++at) {
    if (clipped[at] < floor) {
      clipped[at] = floor;
    }
  }
  // Reassembled as `V diag(clipped) V^T`. Written as one expression so Eigen evaluates it
  // without a named intermediate for the scaled eigenvectors.
  return decomposed.eigenvectors() * clipped.asDiagonal() *
    decomposed.eigenvectors().transpose();
}

} // namespace analytics
} // namespace algotradelib
