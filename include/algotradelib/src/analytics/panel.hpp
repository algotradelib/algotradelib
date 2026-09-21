#pragma once

#include "./backend.hpp"
#include "./frame.hpp"
#include "./time_index.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * How to reconcile several instruments whose bars do not line up.
 *
 * There is no default, and the argument is required, because every choice here answers
 * a different question and the wrong one produces numbers that look entirely plausible.
 * A cross-sectional covariance computed over forward-filled holidays is not the same
 * quantity as one computed over days every instrument traded, and nothing in the output
 * says which you got.
 */
enum class Alignment {
  /**
   * Every instant any instrument reported, with gaps left unobserved.
   *
   * The honest default to reach for: no value is invented, and a reduction that masks
   * unobserved values simply sees fewer observations for the instruments that were not
   * trading. Use it when you would rather have a smaller sample than a fabricated one.
   */
  UNION_UNOBSERVED,
  /**
   * Only instants every instrument reported.
   *
   * The right choice for a cross-sectional statistic that needs a complete row —
   * covariance, ranking, portfolio weights — because it removes the rows that would
   * otherwise be partly masked. It can discard a great deal of data when one
   * instrument's history is short.
   */
  INTERSECTION,
  /**
   * Every instant any instrument reported, gaps carried forward from the last
   * observation.
   *
   * Convenient and the easiest to misuse. A carried-forward price is a real price that
   * is no longer current, so a return computed across a filled gap is zero when nothing
   * was traded and then jumps when trading resumes. Never fills before an instrument's
   * first observation, which stays unobserved because there is nothing to carry.
   */
  FORWARD_FILL,
};

/** Returns a stable lowercase name for `alignment`, for display. */
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
 * One field across many instruments: rows are time, columns are instruments.
 *
 * A different layout from `SeriesFrame` on purpose, not one matrix used two ways.
 * Cross-sectional work — ranking a universe, a covariance matrix, portfolio weights —
 * reads a *row* across instruments, and gets it from a layout built for that, while
 * per-instrument work reads a contiguous column of a frame. Choosing one layout for
 * both would make half the work strided.
 *
 * The panel records the alignment it was built with, because the number a reduction
 * produces is not interpretable without it.
 */
class Panel {
public:
  /** Constructs an empty panel. */
  Panel() = default;

  /** Returns how many rows, and therefore instants, the panel holds. */
  std::size_t rows() const noexcept { return index_.size(); }

  /** Returns how many instrument columns the panel holds. */
  std::size_t columns() const noexcept { return instruments_.size(); }

  /** Returns the instants the rows correspond to. */
  const TimeIndex& time() const noexcept { return index_; }

  /** Returns the instruments the columns correspond to, in column order. */
  const std::vector<InstrumentId>& instruments() const noexcept { return instruments_; }

  /** Returns which field the panel holds. */
  BarField field() const noexcept { return field_; }

  /** Returns how the panel reconciled its instruments. */
  Alignment alignment() const noexcept { return alignment_; }

  /** Returns the column index of `instrument`, or nothing when it is absent. */
  std::optional<std::size_t> find(const InstrumentId& instrument) const {
    for (std::size_t column = 0; column < instruments_.size(); ++column) {
      if (instruments_[column] == instrument) {
        return column;
      }
    }
    return std::nullopt;
  }

  /** Returns one instrument's series as a reference into the panel's own storage. */
  SeriesView column(std::size_t position) const {
    if (position >= instruments_.size()) {
      throw std::out_of_range("panel column is past the end");
    }
    return values_.col(static_cast<Eigen::Index>(position));
  }

  /** Returns one value. */
  Scalar at(std::size_t row, std::size_t column) const {
    if (row >= rows() || column >= columns()) {
      throw std::out_of_range("panel position is past the end");
    }
    return values_(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(column));
  }

  /** Returns the whole storage as a reference, for a cross-sectional computation. */
  MatrixView values() const { return values_; }

  /**
   * Builds a panel from several bar series.
   *
   * `alignment` has no default on purpose; see `Alignment`.
   *
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

    panel.values_ = Matrix::Constant(
      static_cast<Eigen::Index>(panel.index_.size()),
      static_cast<Eigen::Index>(frames.size()),
      missing()
    );

    for (std::size_t column = 0; column < frames.size(); ++column) {
      fill_column(panel, column, frames[column], field, alignment);
    }
    return panel;
  }

private:
  /** Returns every instant any frame reported, ascending and deduplicated. */
  static std::vector<TimeIndex::Instant> combined_instants(const std::vector<SeriesFrame>& frames) {
    std::set<TimeIndex::Instant> ordered;
    for (const auto& frame : frames) {
      for (const auto& stamp : frame.time().stamps()) {
        ordered.insert(stamp);
      }
    }
    return std::vector<TimeIndex::Instant>(ordered.begin(), ordered.end());
  }

  /** Returns only the instants every frame reported. */
  static std::vector<TimeIndex::Instant> shared_instants(const std::vector<SeriesFrame>& frames) {
    std::vector<TimeIndex::Instant> shared = frames.front().time().stamps();
    for (std::size_t position = 1; position < frames.size(); ++position) {
      const auto& other = frames[position].time().stamps();
      std::vector<TimeIndex::Instant> narrowed;
      std::set_intersection(
        shared.begin(), shared.end(), other.begin(), other.end(), std::back_inserter(narrowed)
      );
      shared = std::move(narrowed);
    }
    return shared;
  }

  /** Writes one instrument's column under the chosen alignment. */
  static void fill_column(
    Panel& panel,
    std::size_t column,
    const SeriesFrame& frame,
    BarField field,
    Alignment alignment
  ) {
    for (std::size_t row = 0; row < panel.rows(); ++row) {
      const auto& when = panel.index_.at(row);
      std::optional<std::size_t> source;
      if (alignment == Alignment::FORWARD_FILL) {
        // The latest observation at or before this instant. Before the instrument's
        // first observation there is nothing to carry, and `asof` reports that rather
        // than reaching forward.
        source = frame.time().asof(when);
      } else {
        source = frame.time().find(when);
      }
      if (!source) {
        continue;
      }
      panel.values_(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(column)) =
        frame.at(*source, field);
    }
  }

  std::vector<InstrumentId> instruments_;
  TimeIndex index_;
  Matrix values_;
  BarField field_{BarField::CLOSE};
  Alignment alignment_{Alignment::UNION_UNOBSERVED};
};

} // namespace analytics
} // namespace algotradelib
