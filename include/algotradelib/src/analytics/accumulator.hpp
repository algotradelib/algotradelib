#pragma once

#include "./numeric.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace algotradelib {
namespace analytics {

/**
 * Statistics whose window is all of history, kept in constant space.
 *
 * These are the counterpart to `rolling.hpp`, and the division between them is the point.
 * A trailing statistic needs the last `n` values, so `SeriesBuffer` stores them. A
 * statistic over *all* history — a cumulative return, an all-time high, the worst drawdown
 * so far — does not need the history stored at all, only a few numbers updated, so storing
 * it would spend `O(n)` memory answering a question that needs `O(1)`. That is why this
 * library offers no unbounded buffer: these are what it would have been for.
 *
 * Each accumulator takes one observation at a time through `update`, which is exactly what a
 * live runner has to give it, and each reports the same number an evaluation pass over a
 * finished series would. An unobserved value is skipped rather than folded in, matching the
 * masking convention the vectorized functions keep, and every observer returns `missing()`
 * before the accumulator has seen enough to answer.
 *
 * Like every other stateful type here, an accumulator is owned by one caller and is not
 * synchronized.
 */

/**
 * A sum that does not lose the small terms to the large ones.
 *
 * Uses Neumaier's compensated summation: the rounding error of each addition is carried in a
 * second variable and added back at the end. The cost is one extra `Scalar` and three flops
 * per update; what it buys is a sum of a million small returns that is still accurate when
 * the running total has grown far larger than any one of them, which is precisely the shape
 * a live accumulator has. A naive `total += value` loses roughly one digit per factor of ten
 * between the total and the addend, silently.
 */
class RunningSum {
public:
  /** Folds one observation into the sum, skipping an unobserved one. */
  void update(Scalar value) {
    if (!observed(value)) {
      return;
    }
    ++count_;
    const Scalar total = sum_ + value;
    // The compensation term takes the part of the smaller operand that the addition
    // rounded away, which requires knowing which operand was smaller.
    if (std::abs(sum_) >= std::abs(value)) {
      compensation_ += (sum_ - total) + value;
    } else {
      compensation_ += (value - total) + sum_;
    }
    sum_ = total;
  }

  /** Returns how many observations have been folded in. */
  std::size_t count() const noexcept { return count_; }

  /** Returns the sum, or `missing()` before any observation. */
  Scalar total() const { return count_ == 0 ? missing() : sum_ + compensation_; }

private:
  std::size_t count_{0};
  Scalar sum_{0};
  Scalar compensation_{0};
};

/**
 * A running mean and variance by Welford's method.
 *
 * Welford updates the mean and the sum of squared deviations from *the running mean*, so no
 * intermediate ever holds a sum of squares. That is what makes it usable on price levels:
 * the one-pass `E[x²] - E[x]²` form subtracts two nearly equal large numbers and loses most
 * of its significant digits, and can even return a negative variance. `rolling_stdev` avoids
 * the same trap by taking two passes over each window, which an accumulator cannot do
 * because it never sees the values twice.
 *
 * The variance is the **sample** one, dividing by `count - 1`, matching `rolling_stdev` so
 * that a running figure and a trailing one mean the same thing.
 */
class RunningMoments {
public:
  /** Folds one observation into the moments, skipping an unobserved one. */
  void update(Scalar value) {
    if (!observed(value)) {
      return;
    }
    ++count_;
    const Scalar delta = value - mean_;
    mean_ += delta / static_cast<Scalar>(count_);
    // Deliberately the product of the deviation from the *old* mean and the deviation from
    // the *new* one. Squaring either alone is a different, and wrong, recurrence.
    squared_ += delta * (value - mean_);
  }

  /** Returns how many observations have been folded in. */
  std::size_t count() const noexcept { return count_; }

  /** Returns the running mean, or `missing()` before any observation. */
  Scalar mean() const { return count_ == 0 ? missing() : mean_; }

  /** Returns the sample variance, or `missing()` before the second observation. */
  Scalar variance() const {
    return count_ < 2 ? missing() : squared_ / static_cast<Scalar>(count_ - 1);
  }

  /** Returns the sample standard deviation, or `missing()` before the second observation. */
  Scalar stdev() const {
    const Scalar spread = variance();
    return observed(spread) ? std::sqrt(spread) : spread;
  }

private:
  std::size_t count_{0};
  Scalar mean_{0};
  Scalar squared_{0};
};

/** The smallest and largest value seen so far. */
class RunningExtremes {
public:
  /** Folds one observation in, skipping an unobserved one. */
  void update(Scalar value) {
    if (!observed(value)) {
      return;
    }
    if (count_ == 0 || value < smallest_) {
      smallest_ = value;
    }
    if (count_ == 0 || largest_ < value) {
      largest_ = value;
    }
    ++count_;
  }

  /** Returns how many observations have been folded in. */
  std::size_t count() const noexcept { return count_; }

  /** Returns the smallest value seen, or `missing()` before any observation. */
  Scalar smallest() const { return count_ == 0 ? missing() : smallest_; }

  /** Returns the largest value seen, or `missing()` before any observation. */
  Scalar largest() const { return count_ == 0 ? missing() : largest_; }

private:
  std::size_t count_{0};
  Scalar smallest_{0};
  Scalar largest_{0};
};

/**
 * The worst peak-to-trough decline of a level series, and the decline in force now.
 *
 * Takes a **level** — an equity curve, a cumulative return expressed as a growth factor, a
 * price — not a series of period returns. Feeding it returns measures the drawdown of the
 * return series itself, which is a different and mostly meaningless quantity.
 *
 * Both figures are reported as a non-negative fraction of the running peak: `0.2` means a
 * fifth below the high-water mark. The sign convention is stated because both are in use and
 * a mixed-up one turns a risk limit into its opposite.
 *
 * A peak at or below zero leaves the fraction unobserved rather than dividing by it. An
 * equity curve that reaches zero has not drawn down by some percentage; it is gone, and a
 * ratio would be a fabrication.
 */
class RunningDrawdown {
public:
  /** Folds one level in, skipping an unobserved one. */
  void update(Scalar level) {
    if (!observed(level)) {
      return;
    }
    if (count_ == 0 || peak_ < level) {
      peak_ = level;
    }
    ++count_;
    const Scalar decline = relative(level);
    if (observed(decline)) {
      if (!observed(worst_) || worst_ < decline) {
        worst_ = decline;
      }
    } else if (!observed(worst_)) {
      // Carries the reason forward: undefined, not merely absent.
      worst_ = decline;
    }
    current_ = decline;
  }

  /** Returns how many levels have been folded in. */
  std::size_t count() const noexcept { return count_; }

  /** Returns the running high-water mark, or `missing()` before any observation. */
  Scalar peak() const { return count_ == 0 ? missing() : peak_; }

  /** Returns the decline from the peak in force now, as a non-negative fraction. */
  Scalar drawdown() const { return count_ == 0 ? missing() : current_; }

  /** Returns the worst decline seen so far, as a non-negative fraction. */
  Scalar max_drawdown() const { return count_ == 0 ? missing() : worst_; }

private:
  Scalar relative(Scalar level) const {
    if (peak_ <= Scalar{0}) {
      // Levels arrived, and no ratio exists for them. `undefined()` rather than `missing()`, so a
      // caller can tell an equity curve that reached zero from one that has not started.
      return math::undefined();
    }
    return (peak_ - level) / peak_;
  }

  std::size_t count_{0};
  Scalar peak_{0};
  // Both start unobserved rather than at zero. Reporting a worst drawdown of zero before any
  // level has arrived would be a claim that nothing had gone wrong.
  Scalar current_{missing()};
  Scalar worst_{missing()};
};

/**
 * An exponential moving average a live loop advances one observation at a time.
 *
 * This exists because of the caution in `ema`: an exponential average's value depends on
 * every observation that fed it, so recomputing it over a trailing window does *not*
 * reproduce what a live loop would have produced — the window's oldest row becomes a fresh
 * seed each time it advances. A strategy that fits an EMA in a backtest over frames and then
 * trades it from a `SeriesBuffer` is trading a slightly different indicator, and the
 * difference does not announce itself. Keeping the level in an accumulator removes the
 * question: the same recurrence runs in both places.
 *
 * Same convention as `ema`: `alpha = 2 / (span + 1)`, seeded with the first observed value.
 * `min_observations` suppresses the region where the seed still dominates.
 */
class RunningEma {
public:
  /**
   * Constructs an average with the given span.
   *
   * @throws std::invalid_argument if `span` is zero, or if more observations are required
   * than could distinguish the result from its seed.
   */
  explicit RunningEma(std::size_t span, std::size_t min_observations = 0)
  : min_observations_(min_observations),
    alpha_(Scalar{2} / (static_cast<Scalar>(span) + Scalar{1})) {
    if (span == 0) {
      throw std::invalid_argument("running ema span must be at least one observation");
    }
  }

  /** Folds one observation in, skipping an unobserved one. */
  void update(Scalar value) {
    if (!observed(value)) {
      return;
    }
    level_ = count_ == 0 ? value : level_ + alpha_ * (value - level_);
    ++count_;
  }

  /** Returns how many observations have been folded in. */
  std::size_t count() const noexcept { return count_; }

  /** Returns the smoothing factor the span implies. */
  Scalar alpha() const noexcept { return alpha_; }

  /** Returns the level, or `missing()` until enough observations have arrived. */
  Scalar value() const {
    return count_ == 0 || count_ < min_observations_ ? missing() : level_;
  }

private:
  std::size_t count_{0};
  std::size_t min_observations_{0};
  Scalar alpha_;
  Scalar level_{0};
};

} // namespace analytics
} // namespace algotradelib
