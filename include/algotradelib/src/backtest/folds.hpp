#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief Fold ranges for rolling and expanding windows and walk-forward tests: positions in a time index the
 *   caller supplies, never bars or durations.
 */

/** @brief A half-open range of positions, `[begin_, end_)`. */
struct IndexRange {
  /** @brief The first position. */
  std::size_t begin_{0};
  /** @brief One past the last position. */
  std::size_t end_{0};
};

/** @brief The lengths of a fold, in positions. */
struct FoldLengths {
  /** @brief The training range's length (for `Expanding`, the first fold's); positive. */
  std::size_t train_{0};
  /** @brief The test range's length; zero makes each fold a plain window, `train_`. */
  std::size_t test_{0};
  /** @brief How far each fold's test range starts after the previous one's; positive. */
  std::size_t step_{0};
  /** @brief The positions of history every range has before it. */
  std::size_t warmup_{0};
};

/** @brief One fold: its training and test ranges, and where each one's history starts. */
struct Fold {
  /** @brief The fold's place, from 0, in order. */
  std::size_t index_{0};
  /** @brief The training range. */
  IndexRange train_;
  /** @brief The test range, `test_` long; empty at its scheduled start when `test_` is zero. */
  IndexRange test_;
  /** @brief The first position of the training range's history: `train_.begin_ - warmup_`. */
  std::size_t train_warmup_{0};
  /** @brief The first position of the test range's history: `test_.begin_ - warmup_`. */
  std::size_t test_warmup_{0};
};

/** @brief Training on the `train_` positions just before each test range. */
struct Rolling {
  /**
   * @brief Returns the training range of the test range starting at `test_begin`.
   * @param test_begin The test range's first position.
   * @param lengths The fold lengths.
   * @return `[test_begin - train_, test_begin)`.
   */
  IndexRange train(std::size_t test_begin, const FoldLengths& lengths) const {
    return IndexRange{test_begin - lengths.train_, test_begin};
  }
};

/** @brief Training on every position from the end of the first history to each test range. */
struct Expanding {
  /**
   * @brief Returns the training range of the test range starting at `test_begin`.
   * @param test_begin The test range's first position.
   * @param lengths The fold lengths.
   * @return `[warmup_, test_begin)`.
   */
  IndexRange train(std::size_t test_begin, const FoldLengths& lengths) const {
    return IndexRange{lengths.warmup_, test_begin};
  }
};

/**
 * @brief Whether `Scheme` can choose a fold's training range: `true` when a `const Scheme` has a member
 *   `train(std::size_t, const FoldLengths&)` returning something convertible to `IndexRange`.
 * @tparam Scheme The scheme type.
 */
template <typename Scheme, typename = void>
struct is_window_scheme : std::false_type {};

/** @brief A scheme with the member `train` that `folds` calls. */
template <typename Scheme>
struct is_window_scheme<
  Scheme,
  std::enable_if_t<std::is_convertible_v<
    decltype(std::declval<const Scheme&>().train(std::size_t{}, std::declval<const FoldLengths&>())), IndexRange
  >>
> : std::true_type {};

/**
 * @brief Lists the folds over a time index of `count` positions.
 *
 * Fold `k` tests `[t_k, t_k + test_)` with `t_k = warmup_ + train_ + k * step_`, for every `k` whose test range
 * fits; a shorter last test range is dropped, so every fold tests the same length. The training range is
 * `scheme.train(t_k, lengths)`. Chaining the test ranges into one curve needs `step_ == test_`; any positive
 * step is allowed.
 *
 * @tparam Scheme The training scheme: `Rolling`, `Expanding`, or one of your own with the member
 *   `IndexRange train(std::size_t test_begin, const FoldLengths& lengths) const`.
 * @param count The number of positions in the time index.
 * @param lengths The fold lengths.
 * @param scheme The training scheme.
 * @return The folds, in order.
 * @throws std::invalid_argument if `train_` or `step_` is zero, `warmup_ + train_ + test_` exceeds `count`, or the
 *   scheme returns a range that starts before `warmup_`, is empty, or ends after the test range starts.
 */
template <typename Scheme = Rolling>
std::vector<Fold> folds(std::size_t count, const FoldLengths& lengths, const Scheme& scheme = Scheme{}) {
  static_assert(
    is_window_scheme<Scheme>::value,
    "a fold scheme needs the member IndexRange train(std::size_t test_begin, const FoldLengths& lengths) const"
  );
  if (lengths.train_ == 0 || lengths.step_ == 0) {
    throw std::invalid_argument("folds need a positive train_ and step_");
  }
  const auto& [train, test, step, warmup] = lengths;
  if (warmup > count || train > count - warmup || test > count - warmup - train) {
    throw std::invalid_argument(
      "folds: warmup_ + train_ + test_ (" + std::to_string(warmup) + " + " + std::to_string(train) + " + " +
      std::to_string(test) + ") exceeds the " + std::to_string(count) + " positions"
    );
  }
  std::vector<Fold> out;
  for (std::size_t begin = warmup + train;; begin += step) {
    const IndexRange range = scheme.train(begin, lengths);
    if (range.begin_ < warmup || range.begin_ >= range.end_ || range.end_ > begin) {
      throw std::invalid_argument(
        "folds: the scheme's training range [" + std::to_string(range.begin_) + ", " + std::to_string(range.end_) +
        ") is empty, starts before warmup_ " + std::to_string(warmup) + ", or ends after the test start " +
        std::to_string(begin)
      );
    }
    out.push_back(Fold{out.size(), range, IndexRange{begin, begin + test}, range.begin_ - warmup, begin - warmup});
    if (step > count - begin || test > count - begin - step) {
      break;
    }
  }
  return out;
}

} // namespace backtest
} // namespace algotradelib
