#pragma once

#include "./conversion.hpp"
#include "./frame.hpp"
#include "./time_index.hpp"

#include "../math/matrix.hpp"

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * The last `capacity` observations of a few fields, for a live loop.
 *
 * A frame is built once from a completed series; this is its counterpart on the other
 * side, fed one observation at a time as bars arrive and asked for a trailing window.
 * A strategy reading a moving average on every bar reads it from here.
 *
 * **No allocation happens in steady state.** Storage for twice the capacity is taken once
 * at construction and never grown, so a live loop does not allocate on the path that runs
 * on every tick.
 *
 * **A view is always one contiguous span**, which is the reason this is a linear buffer
 * that occasionally compacts rather than a ring. A ring never moves anything, but its
 * contents wrap, so every reader would face two pieces — and the whole point of
 * column-major storage is that a series is one run a computation can walk without a
 * stride. Paying an amortized single row move per push to keep that property is the better
 * trade.
 *
 * The amortized bound is exact rather than approximate: storage holds twice the capacity,
 * so a compaction can only happen once per `capacity` pushes, and each moves at most
 * `capacity` rows. That is one row moved per push, amortized, and `compactions()` exposes
 * the count so the bound can be asserted rather than asserted *about*.
 */
class RollingWindow {
public:
  /** Instant type observations are stamped with. */
  using Instant = TimeIndex::Instant;

  /**
   * Constructs a window holding `capacity` observations of `fields` values each.
   *
   * @throws std::invalid_argument if either is zero.
   */
  RollingWindow(std::size_t capacity, std::size_t fields = bar_field_count)
  : capacity_(capacity), fields_(fields) {
    if (capacity == 0 || fields == 0) {
      throw std::invalid_argument("a rolling window needs a capacity and at least one field");
    }
    // Twice the capacity is what makes the amortized bound hold; see the class comment.
    values_ = math::unobserved(capacity * 2, fields);
    stamps_.resize(capacity * 2);
  }

  /** Returns how many observations the window can hold. */
  std::size_t capacity() const noexcept { return capacity_; }

  /** Returns how many values each observation carries. */
  std::size_t fields() const noexcept { return fields_; }

  /** Returns how many observations the window currently holds. */
  std::size_t size() const noexcept { return count_; }

  /** Returns whether the window holds no observations. */
  bool empty() const noexcept { return count_ == 0; }

  /** Returns whether the window holds as many observations as it can. */
  bool full() const noexcept { return count_ == capacity_; }

  /** Returns how many times the buffer has been compacted. */
  std::size_t compactions() const noexcept { return compactions_; }

  /**
   * Appends one observation, dropping the oldest when the window is already full.
   *
   * @throws std::invalid_argument if `row` does not carry exactly `fields()` values, or if
   * `when` is not later than the newest observation held — a window whose instants were
   * not ascending would make every trailing computation over it meaningless.
   */
  void push(Instant when, math::SeriesView row) {
    if (static_cast<std::size_t>(row.size()) != fields_) {
      throw std::invalid_argument("a rolling window observation must carry every field");
    }
    if (count_ > 0 && !(newest_instant() < when)) {
      throw std::invalid_argument("a rolling window observation must be later than the last");
    }

    if (count_ == capacity_) {
      ++begin_;
      --count_;
    }
    if (begin_ + count_ == static_cast<std::size_t>(values_.rows())) {
      compact();
    }

    const auto row_index = static_cast<math::Index>(begin_ + count_);
    for (std::size_t field = 0; field < fields_; ++field) {
      values_(row_index, static_cast<math::Index>(field)) =
        row(static_cast<math::Index>(field));
    }
    stamps_[begin_ + count_] = when;
    ++count_;
  }

  /** Appends one observation from a plain sequence of values. */
  void push(Instant when, const std::vector<Scalar>& row) {
    if (row.size() != fields_) {
      throw std::invalid_argument("a rolling window observation must carry every field");
    }
    const math::Vector values = math::Vector::Map(row.data(), static_cast<math::Index>(row.size()));
    push(when, values);
  }

  /** Appends one bar's fields, in the order `BarField` enumerates them. */
  void push(const Bar& bar) {
    if (fields_ != bar_field_count) {
      throw std::invalid_argument("pushing a bar needs a window over every bar field");
    }
    math::Vector row = math::unobserved(bar_field_count);
    if (bar.open_) {
      row(static_cast<math::Index>(bar_field_index(BarField::OPEN))) = to_scalar(*bar.open_);
    }
    row(static_cast<math::Index>(bar_field_index(BarField::HIGH))) = to_scalar(bar.high_);
    row(static_cast<math::Index>(bar_field_index(BarField::LOW))) = to_scalar(bar.low_);
    row(static_cast<math::Index>(bar_field_index(BarField::CLOSE))) = to_scalar(bar.close_);
    row(static_cast<math::Index>(bar_field_index(BarField::VOLUME))) = to_scalar(bar.volume_);
    if (bar.traded_value_) {
      row(static_cast<math::Index>(bar_field_index(BarField::TRADED_VALUE))) =
        to_scalar(*bar.traded_value_);
    }
    push(bar.timestamp_, row);
  }

  /**
   * Returns one field's trailing values, oldest first, as a contiguous reference.
   *
   * References the window's own storage: nothing is copied to read a series, and the span
   * has no stride, which is what the compaction buys.
   */
  math::SeriesView column(std::size_t field) const {
    if (field >= fields_) {
      throw std::out_of_range("rolling window field is past the end");
    }
    return values_.col(static_cast<math::Index>(field))
      .segment(static_cast<math::Index>(begin_), static_cast<math::Index>(count_));
  }

  /** Returns one bar field's trailing values. */
  math::SeriesView column(BarField field) const { return column(bar_field_index(field)); }

  /** Returns one value, counting from the oldest observation held. */
  Scalar at(std::size_t position, std::size_t field) const {
    if (position >= count_ || field >= fields_) {
      throw std::out_of_range("rolling window position is past the end");
    }
    return values_(
      static_cast<math::Index>(begin_ + position), static_cast<math::Index>(field)
    );
  }

  /** Returns the instant of one observation, counting from the oldest held. */
  Instant instant(std::size_t position) const {
    if (position >= count_) {
      throw std::out_of_range("rolling window position is past the end");
    }
    return stamps_[begin_ + position];
  }

  /** Returns the instant of the newest observation. */
  Instant newest_instant() const {
    if (count_ == 0) {
      throw std::out_of_range("an empty rolling window has no newest observation");
    }
    return stamps_[begin_ + count_ - 1];
  }

  /** Discards every observation, keeping the storage already taken. */
  void clear() noexcept {
    begin_ = 0;
    count_ = 0;
  }

private:
  /**
   * Moves the held observations to the front of the storage.
   *
   * The source and destination never overlap. Compaction happens only when the held run
   * reaches the end of storage, so `begin_` is then `2 · capacity − count_`, and since
   * `count_` never exceeds `capacity_` that leaves `begin_ >= count_` — the destination
   * ends at or before where the source starts.
   */
  void compact() {
    const auto rows = static_cast<math::Index>(count_);
    const auto from = static_cast<math::Index>(begin_);
    values_.topRows(rows) = values_.middleRows(from, rows);
    for (std::size_t position = 0; position < count_; ++position) {
      stamps_[position] = stamps_[begin_ + position];
    }
    begin_ = 0;
    ++compactions_;
  }

  std::size_t capacity_{0};
  std::size_t fields_{0};
  std::size_t begin_{0};
  std::size_t count_{0};
  std::size_t compactions_{0};
  math::Matrix values_;
  std::vector<Instant> stamps_;
};

} // namespace analytics
} // namespace algotradelib
