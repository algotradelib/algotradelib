#pragma once

#include "./conversion.hpp"
#include "./time_index.hpp"

#include "../math/matrix.hpp"

#include <algotradelib/core>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace analytics {

/** @brief A bar field, used as a column of a `SeriesFrame`. */
enum class BarField {
  OPEN,
  HIGH,
  LOW,
  CLOSE,
  VOLUME,
  TRADED_VALUE,
};

/** @brief Every `BarField`, in declaration order. */
inline constexpr std::array<BarField, 6> all_bar_fields{
  BarField::OPEN,
  BarField::HIGH,
  BarField::LOW,
  BarField::CLOSE,
  BarField::VOLUME,
  BarField::TRADED_VALUE,
};

/** @brief The number of `BarField` values, and so of columns in a `SeriesFrame`. */
inline constexpr std::size_t bar_field_count = all_bar_fields.size();

/**
 * @brief Returns the column position of a field in a `SeriesFrame`.
 * @param field The field.
 * @return Its column index, from `0` to `bar_field_count - 1`.
 */
inline constexpr std::size_t bar_field_index(BarField field) noexcept {
  return static_cast<std::size_t>(field);
}

/**
 * @brief Returns a lowercase display name for a field, such as `"close"`.
 * @param field The field.
 * @return The name; empty for a value that is not a `BarField`.
 */
inline constexpr std::string_view bar_field_name(BarField field) noexcept {
  switch (field) {
    case BarField::OPEN:
      return "open";
    case BarField::HIGH:
      return "high";
    case BarField::LOW:
      return "low";
    case BarField::CLOSE:
      return "close";
    case BarField::VOLUME:
      return "volume";
    case BarField::TRADED_VALUE:
      return "traded_value";
  }
  return "";
}

static_assert(
  lists_every_value(all_bar_fields, [](BarField field) { return !bar_field_name(field).empty(); }),
  "all_bar_fields must list every BarField"
);

/**
 * @brief One instrument's bars as numeric columns: one row per bar, one column per `BarField`.
 *
 * Build one with `from_bars()`. `column()` and the named accessors return views into the
 * frame's own storage, without copying.
 *
 * A field the broker did not report (an open on a day with no trade, an omitted traded
 * value) is `missing()` in that row, not zero.
 */
class SeriesFrame {
public:
  /** @brief Constructs an empty frame. */
  SeriesFrame() = default;

  /**
   * @brief Constructs a frame with one row per instant, every value `missing()`.
   *
   * @param index The instants of the rows.
   * @param instrument The instrument the frame describes.
   */
  explicit SeriesFrame(TimeIndex index, InstrumentId instrument = {})
  : instrument_(std::move(instrument)), index_(std::move(index)) {
    values_ = math::Matrix::Constant(
      static_cast<math::Index>(index_.size()),
      static_cast<math::Index>(bar_field_count),
      missing()
    );
  }

  /**
   * @brief Returns the number of rows.
   * @return The number of instants (bars) in the frame.
   */
  std::size_t rows() const noexcept { return index_.size(); }

  /**
   * @brief Returns the number of columns.
   * @return `bar_field_count`.
   */
  std::size_t columns() const noexcept { return bar_field_count; }

  /**
   * @brief Returns the instants of the rows.
   * @return The frame's time index.
   */
  const TimeIndex& time() const noexcept { return index_; }

  /**
   * @brief Returns the instrument the frame describes.
   * @return The instrument.
   */
  const InstrumentId& instrument() const noexcept { return instrument_; }

  /**
   * @brief Returns one field's column.
   * @param field The field.
   * @return A view into the frame's storage; valid while the frame is alive and unmoved.
   */
  math::SeriesView column(BarField field) const {
    return values_.col(static_cast<math::Index>(bar_field_index(field)));
  }

  /**
   * @brief Returns the close column.
   * @return A view of `BarField::CLOSE`.
   */
  math::SeriesView close() const { return column(BarField::CLOSE); }

  /**
   * @brief Returns the open column.
   * @return A view of `BarField::OPEN`.
   */
  math::SeriesView open() const { return column(BarField::OPEN); }

  /**
   * @brief Returns the high column.
   * @return A view of `BarField::HIGH`.
   */
  math::SeriesView high() const { return column(BarField::HIGH); }

  /**
   * @brief Returns the low column.
   * @return A view of `BarField::LOW`.
   */
  math::SeriesView low() const { return column(BarField::LOW); }

  /**
   * @brief Returns the volume column.
   * @return A view of `BarField::VOLUME`.
   */
  math::SeriesView volume() const { return column(BarField::VOLUME); }

  /**
   * @brief Reads one value.
   *
   * @param row The row position.
   * @param field The field.
   * @return The value, or `missing()` if it was not reported.
   * @throws std::out_of_range if `row` is past the end.
   */
  Scalar at(std::size_t row, BarField field) const {
    if (row >= rows()) {
      throw std::out_of_range("frame row is past the end");
    }
    return values_(
      static_cast<math::Index>(row), static_cast<math::Index>(bar_field_index(field))
    );
  }

  /**
   * @brief Writes one value.
   *
   * @param row The row position.
   * @param field The field.
   * @param value The value to store.
   * @throws std::out_of_range if `row` is past the end.
   */
  void set(std::size_t row, BarField field, Scalar value) {
    if (row >= rows()) {
      throw std::out_of_range("frame row is past the end");
    }
    values_(
      static_cast<math::Index>(row), static_cast<math::Index>(bar_field_index(field))
    ) = value;
  }

  /**
   * @brief Returns all values.
   * @return A view of the rows-by-`bar_field_count` storage, columns in `BarField` order.
   */
  math::MatrixView values() const { return values_; }

private:
  InstrumentId instrument_;
  TimeIndex index_;
  math::Matrix values_;
};

/**
 * @brief Converts a bar series into a frame of `Scalar` columns.
 *
 * A bar with no open or no traded value leaves that cell `missing()`.
 *
 * @param series The bars, strictly ascending in time.
 * @return A frame with one row per bar.
 * @throws std::invalid_argument if `series` is not strictly ascending in time; check
 *   `BarSeries::ascending()` first to avoid the exception.
 */
inline SeriesFrame from_bars(const BarSeries& series) {
  std::vector<TimeIndex::Instant> stamps;
  stamps.reserve(series.bars_.size());
  for (const auto& bar : series.bars_) {
    stamps.push_back(bar.timestamp_);
  }

  SeriesFrame frame(TimeIndex(std::move(stamps)), series.instrument_);
  for (std::size_t row = 0; row < series.bars_.size(); ++row) {
    const auto& bar = series.bars_[row];
    // An absent open is a trading day on which nothing traded. It stays unobserved
    // rather than becoming zero or the previous close.
    if (bar.open_) {
      frame.set(row, BarField::OPEN, to_scalar(*bar.open_));
    }
    frame.set(row, BarField::HIGH, to_scalar(bar.high_));
    frame.set(row, BarField::LOW, to_scalar(bar.low_));
    frame.set(row, BarField::CLOSE, to_scalar(bar.close_));
    frame.set(row, BarField::VOLUME, to_scalar(bar.volume_));
    if (bar.traded_value_) {
      frame.set(row, BarField::TRADED_VALUE, to_scalar(*bar.traded_value_));
    }
  }
  return frame;
}

} // namespace analytics
} // namespace algotradelib
