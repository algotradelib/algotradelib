#pragma once

#include "./frame.hpp"
#include "./time_index.hpp"

#include "../math/matrix.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * @brief How `Panel::from_bars` lines up instruments whose bars fall on different instants.
 *
 * There is no default: each choice changes what a cross-sectional statistic means.
 */
enum class Alignment {
  /**
   * @brief Every instant any instrument reported; an instrument with no bar there is `missing()`.
   *
   * No value is invented; reductions see fewer observations for instruments that did not trade.
   */
  UNION_UNOBSERVED,
  /**
   * @brief Only the instants every instrument reported.
   *
   * Gives complete rows for cross-sectional work (covariance, ranking, weights), but drops much
   * data when one instrument's history is short.
   */
  INTERSECTION,
  /**
   * @brief Every instant any instrument reported; a gap repeats the instrument's last value.
   *
   * Rows before an instrument's first bar stay `missing()`.
   *
   * @warning A carried value is stale: a return across a filled gap is zero, then jumps when
   *   trading resumes.
   */
  FORWARD_FILL,
};

/**
 * @brief Returns a lowercase display name for an alignment, such as `"intersection"`.
 * @param alignment The alignment.
 * @return The name.
 */
inline constexpr std::string_view alignment_name(Alignment alignment) noexcept {
  switch (alignment) {
    case Alignment::UNION_UNOBSERVED:
      return "union_unobserved";
    case Alignment::INTERSECTION:
      return "intersection";
    case Alignment::FORWARD_FILL:
      return "forward_fill";
  }
  return "unknown";
}

/**
 * @brief One bar field across many instruments: one row per instant, one column per instrument.
 *
 * Build one with `from_bars()`. A row is a cross-section (all instruments at one instant), which
 * is what ranking, covariance and portfolio weights read. The panel records the field and the
 * `Alignment` it was built with.
 */
class Panel {
public:
  /** @brief Constructs an empty panel. */
  Panel() = default;

  /**
   * @brief Returns the number of rows.
   * @return The number of instants.
   */
  std::size_t rows() const noexcept { return index_.size(); }

  /**
   * @brief Returns the number of columns.
   * @return The number of instruments.
   */
  std::size_t columns() const noexcept { return instruments_.size(); }

  /**
   * @brief Returns the instants of the rows.
   * @return The panel's time index.
   */
  const TimeIndex& time() const noexcept { return index_; }

  /**
   * @brief Returns the instruments of the columns.
   * @return The instruments, in column order.
   */
  const std::vector<InstrumentId>& instruments() const noexcept { return instruments_; }

  /**
   * @brief Returns the bar field the panel holds.
   * @return The field passed to `from_bars`.
   */
  BarField field() const noexcept { return field_; }

  /**
   * @brief Returns how the panel lined up its instruments.
   * @return The alignment passed to `from_bars`.
   */
  Alignment alignment() const noexcept { return alignment_; }

  /**
   * @brief Finds an instrument's column.
   * @param instrument The instrument, compared with `InstrumentId::operator==`.
   * @return Its column position, or `std::nullopt` if it is not in the panel.
   */
  std::optional<std::size_t> find(const InstrumentId& instrument) const {
    for (std::size_t column = 0; column < instruments_.size(); ++column) {
      if (instruments_[column] == instrument) {
        return column;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Returns one instrument's column.
   * @param position The column position.
   * @return A view into the panel's storage; valid while the panel is alive and unmoved.
   * @throws std::out_of_range if `position` is past the end.
   */
  math::SeriesView column(std::size_t position) const {
    if (position >= instruments_.size()) {
      throw std::out_of_range("panel column is past the end");
    }
    return values_.col(static_cast<math::Index>(position));
  }

  /**
   * @brief Reads one value.
   * @param row The row position.
   * @param column The column position.
   * @return The value, or `missing()`.
   * @throws std::out_of_range if `row` or `column` is past the end.
   */
  Scalar at(std::size_t row, std::size_t column) const {
    if (row >= rows() || column >= columns()) {
      throw std::out_of_range("panel position is past the end");
    }
    return values_(static_cast<math::Index>(row), static_cast<math::Index>(column));
  }

  /**
   * @brief Returns all values.
   * @return A view of the rows-by-instruments storage.
   */
  math::MatrixView values() const { return values_; }

  /**
   * @brief Builds a panel of one bar field from several bar series.
   *
   * @param series The series, one per column, each strictly ascending in time.
   * @param field The bar field to take from each series.
   * @param alignment How to line up instants the series do not share.
   * @return The panel; empty (with `field` and `alignment` recorded) if `series` is empty.
   * @throws std::invalid_argument if any series is not strictly ascending in time.
   */
  static Panel from_bars(
    const std::vector<BarSeries>& series,
    BarField field,
    Alignment alignment
  ) {
    Panel panel;
    panel.field_ = field;
    panel.alignment_ = alignment;
    if (series.empty()) {
      return panel;
    }

    std::vector<SeriesFrame> frames;
    frames.reserve(series.size());
    for (const auto& one : series) {
      frames.push_back(analytics::from_bars(one));
      panel.instruments_.push_back(one.instrument_);
    }

    auto stamps = alignment == Alignment::INTERSECTION ?
      shared_instants(frames) :
      combined_instants(frames);
    panel.index_ = TimeIndex(std::move(stamps));

    panel.values_ = math::Matrix::Constant(
      static_cast<math::Index>(panel.index_.size()),
      static_cast<math::Index>(frames.size()),
      missing()
    );

    for (std::size_t column = 0; column < frames.size(); ++column) {
      fill_column(panel, column, frames[column], field, alignment);
    }
    return panel;
  }

private:
  // Every instant any frame reported, ascending and deduplicated.
  static std::vector<TimeIndex::Instant> combined_instants(const std::vector<SeriesFrame>& frames) {
    std::size_t total = 0;
    for (const auto& frame : frames) {
      total += frame.time().size();
    }
    std::vector<TimeIndex::Instant> all;
    all.reserve(total);
    for (const auto& frame : frames) {
      const auto& stamps = frame.time().stamps();
      all.insert(all.end(), stamps.begin(), stamps.end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all;
  }

  // Only the instants every frame reported.
  static std::vector<TimeIndex::Instant> shared_instants(const std::vector<SeriesFrame>& frames) {
    std::vector<TimeIndex::Instant> shared = frames.front().time().stamps();
    std::vector<TimeIndex::Instant> narrowed;
    narrowed.reserve(shared.size());
    for (std::size_t position = 1; position < frames.size(); ++position) {
      const auto& other = frames[position].time().stamps();
      narrowed.clear();
      std::set_intersection(
        shared.begin(), shared.end(), other.begin(), other.end(), std::back_inserter(narrowed)
      );
      shared.swap(narrowed);
    }
    return shared;
  }

  // Writes one instrument's column under the chosen alignment, walking both ascending indices
  // together in one pass.
  static void fill_column(
    Panel& panel,
    std::size_t column,
    const SeriesFrame& frame,
    BarField field,
    Alignment alignment
  ) {
    const auto& target = panel.index_.stamps();
    const auto& source = frame.time().stamps();
    if (target.empty() || source.empty()) {
      return;
    }
    const auto series = frame.column(field);
    const bool carry = alignment == Alignment::FORWARD_FILL;

    std::size_t at = 0;
    for (std::size_t row = 0; row < target.size(); ++row) {
      const auto& when = target[row];
      while (at + 1 < source.size() && !(when < source[at + 1])) {
        ++at;
      }
      // `source[at]` is now the latest instant at or before `when`, unless the source
      // begins after it — in which case there is nothing observed yet, and carrying
      // backwards would be inventing a price the instrument never had.
      if (when < source[at]) {
        continue;
      }
      if (!carry && source[at] != when) {
        continue;
      }
      panel.values_(static_cast<math::Index>(row), static_cast<math::Index>(column)) =
        series(static_cast<math::Index>(at));
    }
  }

  std::vector<InstrumentId> instruments_;
  TimeIndex index_;
  math::Matrix values_;
  BarField field_{BarField::CLOSE};
  Alignment alignment_{Alignment::UNION_UNOBSERVED};
};

} // namespace analytics
} // namespace algotradelib
