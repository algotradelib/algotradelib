#pragma once

#include <algotradelib/core>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * @file
 * @brief The events a runner delivers to a strategy's handlers, and the market phase.
 *
 * Each event is complete when the handler receives it; a handler decides and returns without
 * requesting more data.
 */

/**
 * @brief Where the clock stands relative to the trading session.
 *
 * Whether a date trades comes from the broker's calendar; the intraday open and close come from
 * the configured `SessionWindow`.
 */
enum class MarketPhase {
  /** @brief The calendar does not cover this date; nothing is known about it. */
  UNKNOWN,
  /** @brief A date the market does not trade. */
  CLOSED,
  /** @brief A trading day, before the session opens. */
  PRE_OPEN,
  /** @brief Inside the session. */
  OPEN,
  /** @brief A trading day, after the session closed. */
  POST_CLOSE,
};

/** @brief Every `MarketPhase`, in declaration order. */
inline constexpr std::array<MarketPhase, 5> all_market_phases{
  MarketPhase::UNKNOWN,
  MarketPhase::CLOSED,
  MarketPhase::PRE_OPEN,
  MarketPhase::OPEN,
  MarketPhase::POST_CLOSE,
};

/**
 * @brief Returns a lowercase name for a phase, such as `"pre_open"`, for logs and display.
 * @param phase The phase.
 * @return The name; empty for a value that is not a `MarketPhase`.
 */
inline constexpr std::string_view market_phase_name(MarketPhase phase) noexcept {
  switch (phase) {
    case MarketPhase::UNKNOWN:
      return "unknown";
    case MarketPhase::CLOSED:
      return "closed";
    case MarketPhase::PRE_OPEN:
      return "pre_open";
    case MarketPhase::OPEN:
      return "open";
    case MarketPhase::POST_CLOSE:
      return "post_close";
  }
  return "";
}

/**
 * @brief Tests whether a phase means the day's session is over (or never opened), so a day order no
 *   longer works.
 * @param phase The phase.
 * @return `true` for `CLOSED` and `POST_CLOSE`; `false` for `UNKNOWN`, `PRE_OPEN` and `OPEN`.
 */
inline constexpr bool market_phase_session_ended(MarketPhase phase) noexcept {
  switch (phase) {
    case MarketPhase::CLOSED:
    case MarketPhase::POST_CLOSE:
      return true;
    case MarketPhase::UNKNOWN:
    case MarketPhase::PRE_OPEN:
    case MarketPhase::OPEN:
      return false;
  }
  return false;
}

static_assert(
  lists_every_value(all_market_phases, [](MarketPhase phase) { return !market_phase_name(phase).empty(); }),
  "all_market_phases must list every MarketPhase"
);

/**
 * @brief The intraday session of a market, as the operator states it.
 *
 * Times are minutes from local midnight, with a fixed UTC offset. The default is the KRX regular
 * session, 09:00-15:30 KST.
 *
 * @note The offset is fixed: a market that observes daylight saving needs a different window
 *   each season.
 */
struct SessionWindow {
  /** @brief The session open, in minutes from local midnight. */
  std::chrono::minutes open_{std::chrono::hours(9)};
  /** @brief The session close, in minutes from local midnight. */
  std::chrono::minutes close_{std::chrono::hours(15) + std::chrono::minutes(30)};
  /** @brief The market's offset from UTC. */
  std::chrono::minutes utc_offset_{std::chrono::hours(9)};
  /** @brief A description of where the window came from, for reports. */
  std::string source_{"KRX regular session, 09:00-15:30 KST (library default)"};

  /**
   * @brief Returns the KRX regular session, 09:00-15:30 KST.
   * @return The default window.
   */
  static SessionWindow krx() { return SessionWindow{}; }

  /**
   * @brief Returns the market phase at an instant.
   *
   * @param now The instant.
   * @param trading_day Whether `now`'s local date (see `local_date`) is a trading day, as
   *   `TradingCalendar::is_trading_day` answered it.
   * @return `UNKNOWN` if `trading_day` is empty, `CLOSED` if it is `false`, and otherwise
   *   `PRE_OPEN`, `OPEN` or `POST_CLOSE` by local time. The open is inclusive, the close exclusive.
   */
  MarketPhase phase(
    std::chrono::system_clock::time_point now,
    std::optional<bool> trading_day
  ) const {
    if (!trading_day) {
      return MarketPhase::UNKNOWN;
    }
    if (!*trading_day) {
      return MarketPhase::CLOSED;
    }
    const auto local = now + utc_offset_;
    const auto since_midnight =
      std::chrono::duration_cast<std::chrono::minutes>(local - local_midnight(now));
    if (since_midnight < open_) {
      return MarketPhase::PRE_OPEN;
    }
    if (close_ <= since_midnight) {
      return MarketPhase::POST_CLOSE;
    }
    return MarketPhase::OPEN;
  }

  /**
   * @brief Returns the market's local date at an instant.
   *
   * Ask the `TradingCalendar` about this date, not the machine's own: in another time zone they
   * differ for part of each day.
   *
   * @param now The instant.
   * @return The date in the market's time zone.
   */
  Date local_date(std::chrono::system_clock::time_point now) const {
    using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
    return date_from_epoch_days(
      std::chrono::duration_cast<Days>(local_midnight(now).time_since_epoch()).count()
    );
  }

  /**
   * @brief Returns the instant the session opens on the local date of an instant.
   *
   * @param now Any instant on the date in question.
   * @return The open of `local_date(now)`, whether or not that date trades.
   */
  std::chrono::system_clock::time_point opens_at(
    std::chrono::system_clock::time_point now
  ) const {
    return local_midnight(now) - utc_offset_ + open_;
  }

private:
  // The market's local midnight before `now`, as a UTC-shifted instant. floor rounds toward
  // negative infinity, so instants before the epoch work too. phase() and local_date() share it so
  // they always agree on the day.
  std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> local_midnight(
    std::chrono::system_clock::time_point now
  ) const {
    using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;
    return std::chrono::time_point_cast<std::chrono::seconds>(
      std::chrono::floor<Days>(now + utc_offset_)
    );
  }
};

/**
 * @brief One bar, delivered to `on_bar`.
 *
 * `interval_` and `stamp_` say which period the bar covers; `period_end_` is the instant that
 * period ended, the earliest moment the bar could be known. See `bar_` for which bars a live
 * source may deliver while still forming.
 */
struct BarEvent {
  /** @brief The instrument the bar is for. */
  InstrumentId instrument_;
  /** @brief The period length the bar covers. */
  BarInterval interval_{BarInterval::ONE_DAY};
  /** @brief Whether `bar_.timestamp_` marks the period's open or close. */
  BarStamp stamp_{BarStamp::CLOSE};
  /**
   * @brief The bar.
   *
   * @note Live, `live::SessionSource` holds back a daily or minute bar whose period has not
   *   ended, such as the broker's row for the current session. Weekly, monthly and yearly bars
   *   stamped at their open are delivered as read and may still be forming.
   */
  Bar bar_;
  /** @brief When the bar's period ended: the earliest moment the bar could be known. */
  std::chrono::system_clock::time_point period_end_{};
};

/**
 * @brief Computes the instant a bar's period ended.
 *
 * @param bar The bar.
 * @param interval The period length the bar covers.
 * @param stamp Whether `bar.timestamp_` marks the period's open or close.
 * @return `bar.timestamp_` for a `CLOSE`-stamped bar; for an `OPEN`-stamped bar, the timestamp
 *   plus one interval. A month counts as 31 days and a year as 366, so for those intervals the
 *   result is an upper bound, not the exact end. So is an intraday bar cut short by the session's
 *   close (KRX's 15:00 hourly bar ends at 15:30, not 16:00): late, never early.
 */
inline std::chrono::system_clock::time_point bar_period_end(
  const Bar& bar,
  BarInterval interval,
  BarStamp stamp
) {
  if (stamp == BarStamp::CLOSE) {
    return bar.timestamp_;
  }
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return bar.timestamp_ + std::chrono::minutes(1);
    case BarInterval::FIVE_MINUTES:
      return bar.timestamp_ + std::chrono::minutes(5);
    case BarInterval::FIFTEEN_MINUTES:
      return bar.timestamp_ + std::chrono::minutes(15);
    case BarInterval::THIRTY_MINUTES:
      return bar.timestamp_ + std::chrono::minutes(30);
    case BarInterval::ONE_HOUR:
      return bar.timestamp_ + std::chrono::hours(1);
    case BarInterval::ONE_DAY:
      return bar.timestamp_ + std::chrono::hours(24);
    case BarInterval::ONE_WEEK:
      return bar.timestamp_ + std::chrono::hours(24 * 7);
    case BarInterval::ONE_MONTH:
      // Nominal: a month has no fixed length. Documented on the function.
      return bar.timestamp_ + std::chrono::hours(24 * 31);
    case BarInterval::ONE_YEAR:
      return bar.timestamp_ + std::chrono::hours(24 * 366);
  }
  return bar.timestamp_;
}

/**
 * @brief Makes a bar event, computing its `period_end_` with `bar_period_end`.
 *
 * @param instrument The instrument the bar is for.
 * @param interval The period length the bar covers.
 * @param stamp Whether the bar's timestamp marks the period's open or close.
 * @param bar The bar.
 * @return The event.
 */
inline BarEvent make_bar_event(
  InstrumentId instrument,
  BarInterval interval,
  BarStamp stamp,
  Bar bar
) {
  BarEvent event;
  event.instrument_ = std::move(instrument);
  event.interval_ = interval;
  event.stamp_ = stamp;
  event.period_end_ = bar_period_end(bar, interval, stamp);
  event.bar_ = std::move(bar);
  return event;
}

struct BarSlice;

/// \cond ATL_SLICE_DETAIL
namespace slice_detail {

// Where each bar of a delivered slice is, by instrument, and the bars it was built over (their storage and
// count). Only the period bucket fills it, on the slice it delivers; **a copy or a move of a slice starts with
// none**, so the only slice that can use it is the one the library built and hands on only as a const reference
// (to a handler, and from `live::SessionSource` to a runner), whose bars nobody changes. A copy, a move, or a slice
// built by its holder has no table and is scanned.
class SliceIndex {
public:
  SliceIndex() = default;
  SliceIndex(const SliceIndex&) noexcept {}
  SliceIndex(SliceIndex&&) noexcept {}
  SliceIndex& operator=(const SliceIndex&) noexcept {
    forget();
    return *this;
  }
  SliceIndex& operator=(SliceIndex&&) noexcept {
    forget();
    return *this;
  }

private:
  friend struct strategy::BarSlice;
  friend class Bucket;

  void forget() noexcept {
    table_.reset();
    built_over_ = nullptr;
    count_ = 0;
  }

  id_detail::PositionTable table_;
  const void* built_over_{nullptr};
  std::size_t count_{0};
};

} // namespace slice_detail
/// \endcond

/**
 * @brief Every instrument's bar of one interval for one period end, delivered to `on_bars`.
 *
 * A runner calls `on_bars` once per period end and interval, after every bar of that period has been
 * fed: cached (with a linear algebra backend, so `ctx.bars(id, interval_)` ends at `period_end_` for each
 * instrument here), priced and clocked. An instrument with no bar for the period is absent; there is no
 * deadline and no placeholder. Several intervals ending at the same instant give one slice each.
 *
 * The slice is valid only during the call.
 */
struct BarSlice {
  /** @brief When the period ended, shared by every bar in `bars_`. */
  std::chrono::system_clock::time_point period_end_{};
  /** @brief The period length every bar in `bars_` covers. */
  BarInterval interval_{BarInterval::ONE_DAY};
  /** @brief The bars, one per instrument, in the order they were fed. */
  std::vector<BarEvent> bars_;

  /**
   * @brief Finds an instrument's bar.
   *
   * The slice a runner hands to `on_bars` finds it by hash; a copy, a move, or a slice built by its holder, by a
   * scan.
   *
   * @param id The instrument.
   * @return Its bar, or `nullptr` if it has none for this period.
   */
  const BarEvent* find(const InstrumentId& id) const noexcept {
    if (!index_.table_.empty() && index_.built_over_ == bars_.data() && index_.count_ == bars_.size()) {
      const auto at = index_.table_.find(id, Ids{&bars_});
      return at ? &bars_[*at] : nullptr;
    }
    for (const auto& event : bars_) {
      if (event.instrument_ == id) {
        return &event;
      }
    }
    return nullptr;
  }

  /// \cond ATL_SLICE_DETAIL
  // The bucket's table of where each bar is, on the slice it delivers only (`slice_detail::SliceIndex`). The
  // storage and count are checked too, which catches bars reallocated or resized since; it cannot catch bars
  // changed in place, which is why only a slice handed on as const carries the table.
  slice_detail::SliceIndex index_;

  // The instrument at a position of `bars_`, for the table.
  struct Ids {
    const std::vector<BarEvent>* bars_;
    const InstrumentId& operator()(std::size_t at) const noexcept { return (*bars_)[at].instrument_; }
  };
  /// \endcond
};

/// \cond ATL_SLICE_DETAIL
namespace slice_detail {

/**
 * Every bar fed with one period end, split into one slice per interval in the order each interval was
 * first seen. Used by the runner's sources to group bars before `on_bars`.
 */
class Bucket {
  // The id at a position of a slice, for its table.
  struct SliceIds {
    const BarSlice* slice_;
    const InstrumentId& operator()(std::size_t at) const noexcept { return slice_->bars_[at].instrument_; }
  };
  static SliceIds ids_of(const BarSlice& slice) noexcept { return SliceIds{&slice}; }

public:
  Bucket() = default;
  Bucket(const Bucket&) = default;
  Bucket(Bucket&&) = default;
  Bucket& operator=(Bucket&&) = default;

  /** Copies all or nothing, so a slice and its table never disagree after a failed copy. */
  Bucket& operator=(const Bucket& other) {
    if (this != &other) {
      Bucket copy(other);
      *this = std::move(copy);
    }
    return *this;
  }

  /** Whether the bucket holds no bar. */
  bool empty() const noexcept { return slices_.empty(); }

  /** The period end of the bars held; meaningless when empty. */
  std::chrono::system_clock::time_point period_end() const noexcept { return period_end_; }

  /** Whether `event` would be the second bar of its instrument and interval here. */
  bool holds(const BarEvent& event) const noexcept {
    for (const auto& slice : slices_) {
      if (slice.interval_ == event.interval_) {
        if (slice.bars_.size() > id_detail::scan_limit) {
          if (const auto* entry = wide(event.interval_)) {
            return entry->table_.find(event.instrument_, ids_of(slice)).has_value();
          }
        }
        return slice.find(event.instrument_) != nullptr;
      }
    }
    return false;
  }

  /** Adds a bar; the caller has checked `holds` and that its period end matches a non-empty bucket. */
  void add(const BarEvent& event) {
    if (slices_.empty()) {
      period_end_ = event.period_end_;
    }
    for (auto& slice : slices_) {
      if (slice.interval_ == event.interval_) {
        slice.bars_.push_back(event);
        // If the table cannot grow, the bar comes out again, so the two never disagree.
        try {
          if (slice.bars_.size() == id_detail::scan_limit + 1) {
            widen(slice);
          } else if (slice.bars_.size() > id_detail::scan_limit) {
            if (auto* entry = wide(slice.interval_)) {
              entry->table_.insert(event.instrument_, slice.bars_.size() - 1, ids_of(slice));
            }
          }
        } catch (...) {
          slice.bars_.pop_back();
          throw;
        }
        return;
      }
    }
    BarSlice slice;
    slice.period_end_ = event.period_end_;
    slice.interval_ = event.interval_;
    slice.bars_.push_back(event);
    slices_.push_back(std::move(slice));
  }

  /** Takes the first slice out; the bucket must not be empty. */
  BarSlice take_first() {
    // Moved out first, since a move does not carry the table (`SliceIndex`); the bars keep their storage.
    BarSlice slice = std::move(slices_.front());
    hand_index_to(slice);
    slices_.erase(slices_.begin());
    return slice;
  }

  /** Takes the slices out, leaving the bucket empty. */
  std::vector<BarSlice> take() {
    for (auto& slice : slices_) {
      hand_index_to(slice);
    }
    std::vector<BarSlice> slices = std::move(slices_);
    slices_.clear();
    return slices;
  }

  /** Drops every bar held. */
  void clear() noexcept { slices_.clear(); }

private:
  // An interval's table, which describes that interval's slice only while the slice holds more than
  // `id_detail::scan_limit` bars: `widen` refills it when a slice passes that size and `add` extends it after,
  // and nothing reads it while the slice is narrower. It outlives the slice, so the next wide slice of the
  // interval reuses its storage; a bucket that never holds a wide slice has none.
  struct Wide {
    BarInterval interval_;
    id_detail::PositionTable table_;
  };

  const Wide* wide(BarInterval interval) const noexcept {
    for (const auto& entry : wide_) {
      if (entry.interval_ == interval) {
        return &entry;
      }
    }
    return nullptr;
  }

  Wide* wide(BarInterval interval) noexcept {
    for (auto& entry : wide_) {
      if (entry.interval_ == interval) {
        return &entry;
      }
    }
    return nullptr;
  }

  // Refills the interval's table from its slice, which has just passed `id_detail::scan_limit` bars. On a
  // failure the table is left empty, and the caller takes the bar back out, so nothing reads it.
  // Moves the table of a wide slice into it, with the bars it describes, so `BarSlice::find` can use it after
  // the slice is delivered. Moving a slice keeps its bars' storage, so the table stays valid for it; the bucket's
  // own table is rebuilt (`widen`) when a later period's slice grows wide again.
  void hand_index_to(BarSlice& slice) noexcept {
    if (slice.bars_.size() <= id_detail::scan_limit) {
      return;
    }
    if (auto* entry = wide(slice.interval_)) {
      slice.index_.table_ = std::move(entry->table_);
      entry->table_.reset();
      slice.index_.built_over_ = slice.bars_.data();
      slice.index_.count_ = slice.bars_.size();
    }
  }

  void widen(const BarSlice& slice) {
    Wide* entry = wide(slice.interval_);
    if (entry == nullptr) {
      wide_.push_back(Wide{slice.interval_, {}});
      entry = &wide_.back();
    }
    entry->table_.reset();
    try {
      for (std::size_t at = 0; at < slice.bars_.size(); ++at) {
        entry->table_.insert(slice.bars_[at].instrument_, at, ids_of(slice));
      }
    } catch (...) {
      entry->table_.reset();
      throw;
    }
  }

  std::chrono::system_clock::time_point period_end_{};
  std::vector<BarSlice> slices_;
  // At most one entry per interval that has had a wide slice.
  std::vector<Wide> wide_;
};

} // namespace slice_detail
/// \endcond

/** @brief A quote the runner has just read, delivered to `on_quote`. */
struct QuoteEvent {
  /** @brief The quote, as the broker reported it. */
  QuoteSnapshot quote_;
};

/**
 * @brief One execution of an order the strategy placed, delivered to `on_fill`.
 *
 * Each execution is its own event, so a partial fill arrives as several; `quantity_` is this
 * execution's quantity, and the order's running total is in the portfolio. `order_id_` names the
 * order by the broker's id where the source states one; a replay's fills leave it empty, and live,
 * `on_fill` does not fire (`Runner::on_fill_never_fires`). Look orders up in `ctx.portfolio()`.
 *
 * @warning A backtest delivers fills, but the shipped live loop never does: no broker inquiry
 *   produces a `Fill`. A strategy that reacts to fills in `on_fill` (for example, to set a stop)
 *   does nothing live. Live, executions show up through `on_order_update` and the portfolio.
 */
struct Fill {
  /** @brief The broker's order id. */
  std::string order_id_;
  /** @brief The instrument traded. */
  InstrumentId instrument_;
  /** @brief The side of the order. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The quantity of this execution, not the order's running total. */
  Decimal64 quantity_;
  /** @brief The execution price. */
  Decimal64 price_;
  /** @brief The tag of the intent that produced the order. */
  std::string tag_;
  /** @brief When the execution happened. */
  std::chrono::system_clock::time_point at_{};
};

} // namespace strategy
} // namespace algotradelib
