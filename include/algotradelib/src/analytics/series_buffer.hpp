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
 * @brief The most recent observations of a few fields, for a strategy fed one bar at a time.
 *
 * Holds up to `capacity()` observations. Until it is full, each `push` adds one; after that,
 * each `push` also drops the oldest. Check `full()` before computing an indicator whose window
 * is the capacity, so it is never computed from fewer observations than its window.
 *
 * `column()` returns the trailing values of one field, oldest first, as one contiguous view
 * of the buffer's own storage. Storage is allocated once at construction and never grows.
 * The trailing statistics themselves are the `rolling_*` functions in `rolling.hpp`.
 *
 * @note Not thread-safe: use one buffer from one strategy (one strand).
 */
class SeriesBuffer {
public:
  /** @brief The instant type observations are stamped with. */
  using Instant = TimeIndex::Instant;

  /**
   * @brief Constructs an empty buffer.
   *
   * @param capacity The most observations the buffer holds.
   * @param fields The number of values per observation; the default holds every `BarField`.
   * @throws std::invalid_argument if `capacity` or `fields` is zero.
   */
  SeriesBuffer(std::size_t capacity, std::size_t fields = bar_field_count)
  : capacity_(capacity), fields_(fields) {
    if (capacity == 0 || fields == 0) {
      throw std::invalid_argument("a series buffer needs a capacity and at least one field");
    }
    // Twice the capacity bounds compaction to one row move per push, amortized.
    values_ = math::unobserved(capacity * 2, fields);
    stamps_.resize(capacity * 2);
  }

  /**
   * @brief Returns the most observations the buffer holds.
   * @return The capacity given at construction.
   */
  std::size_t capacity() const noexcept { return capacity_; }

  /**
   * @brief Returns the number of values per observation.
   * @return The field count given at construction.
   */
  std::size_t fields() const noexcept { return fields_; }

  /**
   * @brief Returns the number of observations held.
   * @return A number from `0` to `capacity()`.
   */
  std::size_t size() const noexcept { return count_; }

  /**
   * @brief Tests whether the buffer is empty.
   * @return `true` if it holds no observations.
   */
  bool empty() const noexcept { return count_ == 0; }

  /**
   * @brief Tests whether the buffer holds `capacity()` observations.
   * @return `true` once it is full.
   */
  bool full() const noexcept { return count_ == capacity_; }

  /**
   * @brief Returns how many times the buffer moved its contents to the front of its storage.
   * @return The compaction count; at most one per `capacity()` pushes.
   */
  std::size_t compactions() const noexcept { return compactions_; }

  /**
   * @brief Appends one observation, dropping the oldest if the buffer is full.
   *
   * @param when The observation's instant; must be later than the newest one held.
   * @param row The observation's values, exactly `fields()` of them.
   * @throws std::invalid_argument if `row` has the wrong size or `when` is not later than
   *   the newest observation.
   */
  void push(Instant when, math::SeriesView row) {
    if (static_cast<std::size_t>(row.size()) != fields_) {
      throw std::invalid_argument("a series buffer observation must carry every field");
    }
    if (count_ > 0 && !(newest_instant() < when)) {
      throw std::invalid_argument("a series buffer observation must be later than the last");
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

  /**
   * @brief Appends one observation, dropping the oldest if the buffer is full.
   *
   * @param when The observation's instant; must be later than the newest one held.
   * @param row The observation's values, exactly `fields()` of them.
   * @throws std::invalid_argument if `row` has the wrong size or `when` is not later than
   *   the newest observation.
   */
  void push(Instant when, const std::vector<Scalar>& row) {
    if (row.size() != fields_) {
      throw std::invalid_argument("a series buffer observation must carry every field");
    }
    const math::Vector values = math::Vector::Map(row.data(), static_cast<math::Index>(row.size()));
    push(when, values);
  }

  /**
   * @brief Appends one bar, dropping the oldest observation if the buffer is full.
   *
   * The bar's fields go to the columns in `BarField` order; a bar with no open or traded
   * value stores `missing()` there.
   *
   * @param bar The bar; its `timestamp_` must be later than the newest observation.
   * @throws std::invalid_argument if the buffer was not built with `bar_field_count` fields,
   *   or the bar is not later than the newest observation.
   */
  void push(const Bar& bar) {
    if (fields_ != bar_field_count) {
      throw std::invalid_argument("pushing a bar needs a buffer over every bar field");
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
   * @brief Returns one field's values, oldest first.
   *
   * @param field The field's column position.
   * @return A view of `size()` values in the buffer's storage, valid until the next `push`
   *   or `clear`.
   * @throws std::out_of_range if `field` is not below `fields()`.
   */
  math::SeriesView column(std::size_t field) const {
    if (field >= fields_) {
      throw std::out_of_range("series buffer field is past the end");
    }
    return values_.col(static_cast<math::Index>(field))
      .segment(static_cast<math::Index>(begin_), static_cast<math::Index>(count_));
  }

  /**
   * @brief Returns one bar field's values, oldest first.
   *
   * @param field The field.
   * @return A view of `size()` values, valid until the next `push` or `clear`.
   * @throws std::out_of_range if the buffer has fewer columns than `field` needs.
   */
  math::SeriesView column(BarField field) const { return column(bar_field_index(field)); }

  /**
   * @brief Reads one value.
   *
   * @param position The observation, counting from `0` for the oldest held.
   * @param field The field's column position.
   * @return The value.
   * @throws std::out_of_range if `position` or `field` is past the end.
   */
  Scalar at(std::size_t position, std::size_t field) const {
    if (position >= count_ || field >= fields_) {
      throw std::out_of_range("series buffer position is past the end");
    }
    return values_(
      static_cast<math::Index>(begin_ + position), static_cast<math::Index>(field)
    );
  }

  /**
   * @brief Returns the instant of one observation.
   *
   * @param position The observation, counting from `0` for the oldest held.
   * @return Its instant.
   * @throws std::out_of_range if `position` is past the end.
   */
  Instant instant(std::size_t position) const {
    if (position >= count_) {
      throw std::out_of_range("series buffer position is past the end");
    }
    return stamps_[begin_ + position];
  }

  /**
   * @brief Returns the instant of the newest observation.
   * @return The instant of the last `push`.
   * @throws std::out_of_range if the buffer is empty.
   */
  Instant newest_instant() const {
    if (count_ == 0) {
      throw std::out_of_range("an empty series buffer has no newest observation");
    }
    return stamps_[begin_ + count_ - 1];
  }

  /** @brief Discards every observation; the storage is kept. */
  void clear() noexcept {
    begin_ = 0;
    count_ = 0;
  }

private:
  // Moves the held observations to the front. Source and destination never overlap: this runs
  // only when the held run reaches the end of storage, so begin_ = 2 * capacity - count_ >= count_.
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
