#pragma once

#include "./backend.hpp"
#include "./time_index.hpp"

#include <algotradelib/core>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace analytics {

/** A column a bar series contributes to a frame. */
enum class BarField {
  OPEN,
  HIGH,
  LOW,
  CLOSE,
  VOLUME,
  TRADED_VALUE,
};

/** How many columns a bar series contributes. */
inline constexpr std::size_t bar_field_count = 6;

/** Returns a dense column index for `field`. */
inline constexpr std::size_t bar_field_index(BarField field) noexcept {
  return static_cast<std::size_t>(field);
}

/** Returns a stable lowercase name for `field`, for display. */
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
  return "unknown";
}

/**
 * One instrument's bars as numeric columns: rows are time, columns are fields.
 *
 * This is where a `BarSeries` stops being a value to send and becomes something to
 * compute on. The conversion happens once, here, and it is the only place a bar's
 * exact decimals become `Scalar`.
 *
 * Column-major storage with time on the rows means one field is contiguous, so
 * `column()` hands out a `SeriesView` that references the frame's own memory. Nothing
 * is copied to read a series, and an indicator never walks a strided view of its input.
 *
 * A field a broker did not report — an open on a day with no trade, a traded value a
 * broker omits — is `missing()` for that row rather than zero. Every reduction in this
 * layer masks it.
 */
class SeriesFrame {
public:
  /** Constructs an empty frame. */
  SeriesFrame() = default;

  /**
   * Constructs a frame over `index` with room for every bar field.
   *
   * Values start unobserved, so a field never written reads as absent rather than as
   * zero.
   */
  explicit SeriesFrame(TimeIndex index, InstrumentId instrument = {})
  : instrument_(std::move(instrument)), index_(std::move(index)) {
    values_ = Matrix::Constant(
      static_cast<Eigen::Index>(index_.size()),
      static_cast<Eigen::Index>(bar_field_count),
      missing()
    );
  }

  /** Returns how many rows, and therefore instants, the frame holds. */
  std::size_t rows() const noexcept { return index_.size(); }

  /** Returns how many field columns the frame holds. */
  std::size_t columns() const noexcept { return bar_field_count; }

  /** Returns the instants the rows correspond to. */
  const TimeIndex& time() const noexcept { return index_; }

  /** Returns which instrument the frame describes. */
  const InstrumentId& instrument() const noexcept { return instrument_; }

  /** Returns one field as a reference into the frame's own storage. */
  SeriesView column(BarField field) const {
    return values_.col(static_cast<Eigen::Index>(bar_field_index(field)));
  }

  /** Returns the close column, the series most work starts from. */
  SeriesView close() const { return column(BarField::CLOSE); }

  /** Returns the open column. */
  SeriesView open() const { return column(BarField::OPEN); }

  /** Returns the high column. */
  SeriesView high() const { return column(BarField::HIGH); }

  /** Returns the low column. */
  SeriesView low() const { return column(BarField::LOW); }

  /** Returns the volume column. */
  SeriesView volume() const { return column(BarField::VOLUME); }

  /**
   * Returns one value.
   *
   * @throws std::out_of_range if `row` is past the end.
   */
  Scalar at(std::size_t row, BarField field) const {
    if (row >= rows()) {
      throw std::out_of_range("frame row is past the end");
    }
    return values_(
      static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(bar_field_index(field))
    );
  }

  /**
   * Writes one value.
   *
   * @throws std::out_of_range if `row` is past the end.
   */
  void set(std::size_t row, BarField field, Scalar value) {
    if (row >= rows()) {
      throw std::out_of_range("frame row is past the end");
    }
    values_(
      static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(bar_field_index(field))
    ) = value;
  }

  /** Returns the whole storage as a reference, for a computation over every field. */
  MatrixView values() const { return values_; }

private:
  InstrumentId instrument_;
  TimeIndex index_;
  Matrix values_;
};

/**
 * Converts one bar series into a frame.
 *
 * The **only** place a bar's exact decimals become scalars. A caller that wants to
 * compute on bars comes through here, which is what keeps the conversion auditable.
 *
 * @throws std::invalid_argument if the series is not strictly ascending in time, which
 * `BarSeries::ascending()` reports without throwing. An out-of-order series would make
 * every `asof` lookup on the result ambiguous, so it is refused rather than indexed.
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
