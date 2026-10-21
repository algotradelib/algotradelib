#pragma once

#include "../strategy/event.hpp"
#include "../strategy/intent.hpp"
#include "../strategy/portfolio.hpp"

#include <algotradelib/core>

#if defined(ATL_HAS_EIGEN)
#include <algotradelib/analytics>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace algotradelib {
namespace runner {

// The strategy types a runner speaks in, imported from `strategy`.
using strategy::BarEvent;
using strategy::CancelIntent;
using strategy::Fill;
using strategy::MarketPhase;
using strategy::OrderIntent;
using strategy::Portfolio;
using strategy::QuoteEvent;
using strategy::TargetPosition;

/**
 * @brief One request a strategy queued: an order, a target position or a cancellation.
 *
 * Requests are kept in one queue in the order the strategy made them, so a cancellation followed by
 * a target is handled in that order.
 *
 * @tparam Broker The broker tag of the cancellation's order reference.
 */
template <typename Broker>
using Instruction = std::variant<OrderIntent, TargetPosition, CancelIntent<Broker>>;

template <typename Strategy, typename CapsType, typename BrokerTag, typename Executor>
class Runner;

} // namespace runner

namespace backtest {
template <typename Strategy, typename CapsType>
class Backtester;
} // namespace backtest

namespace runner {
namespace context_detail {

// A count of a context's portfolio replacements that a move also advances. Moving a context's state into
// another replaces the destination's portfolio, so the destination's count must not merely take the source's
// value: it rises above both, and the source's (now emptied) count rises too. A replay comparing the count with
// the one it last saw then rebuilds whatever a move brought in.
struct Revision {
  std::uint64_t value_{0};

  Revision() = default;
  Revision(const Revision&) = delete;
  Revision& operator=(const Revision&) = delete;
  Revision(Revision&& other) noexcept : value_(other.value_ + 1) { ++other.value_; }
  Revision& operator=(Revision&& other) noexcept {
    value_ = (value_ > other.value_ ? value_ : other.value_) + 1;
    ++other.value_;
    return *this;
  }
};

// In-place access to a context's caches, for the runner and the replay only.
//
// A strategy reads its portfolio through `Context::portfolio()`, a const reference; it gets no mutable
// one from here, since everything below is private and only `Runner` and `Backtester` are friends. The
// counts tell a replay what changed since it last brought the portfolio up to date: a replacement
// (`Context::set_portfolio`, or a move), a runner's edit of its working rows, or new reference data. The price
// and series entries let a replay that sets them on every bar find them once.
class PortfolioAccess {
  template <typename Strategy, typename CapsType, typename BrokerTag, typename Executor>
  friend class runner::Runner;
  template <typename Strategy, typename CapsType>
  friend class backtest::Backtester;

  // For a runner editing working rows and buying power in place; counted as a runner edit.
  template <typename Ctx>
  static auto& for_runner_edit(Ctx& context) noexcept {
    ++context.runner_edits_;
    return context.portfolio_;
  }

  // For a replay bringing the portfolio up to date; not counted.
  template <typename Ctx>
  static auto& for_replay_sync(Ctx& context) noexcept {
    return context.portfolio_;
  }

  template <typename Ctx>
  static std::uint64_t replacements(const Ctx& context) noexcept { return context.replacements_.value_; }

  template <typename Ctx>
  static std::uint64_t runner_edits(const Ctx& context) noexcept { return context.runner_edits_; }

  template <typename Ctx>
  static std::uint64_t reference_changes(const Ctx& context) noexcept { return context.reference_changes_; }

  // Sets an instrument's last price and returns where it is kept, for a replay that sets it again on every bar:
  // the entry, once made, stays where it is for the context's life (a map node).
  template <typename Ctx>
  static Decimal64& set_price(Ctx& context, const InstrumentId& id, const Decimal64& price) {
    return context.prices_.insert_or_assign(id, price).first->second;
  }

#if defined(ATL_HAS_EIGEN)
  // The series a bar of `event`'s instrument and interval goes into, made if it is the first; it stays where it
  // is for the context's life, as `Context::bars` promises.
  template <typename Ctx>
  static analytics::SeriesBuffer& series_for(Ctx& context, const BarEvent& event) {
    return context.series_for(event);
  }

  // The newest bar time of `event`'s instrument and interval, kept instead of a series when the strategy reads
  // no history; made empty if it is the first, and it stays where it is for the context's life.
  template <typename Ctx>
  static std::optional<std::chrono::system_clock::time_point>& bar_time_for(Ctx& context, const BarEvent& event) {
    return context.bar_time_for(event);
  }

  // Refuses `event` unless it is later than its series' newest bar, then makes it the newest; the check a series
  // push makes, without the series.
  static void advance_bar_time(std::optional<std::chrono::system_clock::time_point>& newest, const BarEvent& event) {
    if (newest && !(*newest < event.bar_.timestamp_)) {
      throw std::invalid_argument("a series buffer observation must be later than the last");
    }
    newest = event.bar_.timestamp_;
  }

  // For a runner whose strategy declares it reads no bar history: `Context::bars` throws from now on.
  template <typename Ctx>
  static void stop_bar_history(Ctx& context) {
    context.history_.stop_keeping();
  }
#endif
};

#if defined(ATL_HAS_EIGEN)
// A context's bar history: each series' trailing bars, or, when the context keeps none, each series' newest bar
// time only (to refuse a bar that is not later). Whether it keeps them belongs to the context object, not to
// its contents: a move assignment keeps the destination's, so a context moved into a runner's place cannot turn
// on reads the runner never records for, and what it brings is converted to the destination's form in place,
// without allocating, so moving a context still cannot throw.
struct BarHistory {
  // One series of an instrument at an interval.
  struct Entry {
    // Its bars; never made in a context keeping no history.
    std::optional<analytics::SeriesBuffer> series_;
    // Its newest bar time where it has no bars: kept instead of them, or brought by a context keeping none,
    // when it bounds the first bar pushed.
    std::optional<std::chrono::system_clock::time_point> newest_;
  };
  using ByInterval = std::map<BarInterval, Entry>;

  // Node-based both ways, so an entry never moves once made.
  std::unordered_map<InstrumentId, ByInterval, id_detail::InstrumentIdHash> entries_;
  bool kept_{true};

  BarHistory() = default;
  BarHistory(const BarHistory&) = delete;
  BarHistory& operator=(const BarHistory&) = delete;
  BarHistory(BarHistory&&) = default;
  BarHistory& operator=(BarHistory&& other) noexcept {
    entries_ = std::move(other.entries_);
    if (!kept_) {
      keep_times_only();
    }
    return *this;
  }

  // For a runner whose strategy reads no history.
  void stop_keeping() noexcept {
    kept_ = false;
    keep_times_only();
  }

private:
  // Replaces each series by its newest bar time, keeping the later where both exist.
  void keep_times_only() noexcept {
    for (auto& [id, by_interval] : entries_) {
      for (auto& [interval, entry] : by_interval) {
        if (entry.series_ && !entry.series_->empty()) {
          const auto newest = entry.series_->newest_instant();
          if (!entry.newest_ || *entry.newest_ < newest) {
            entry.newest_ = newest;
          }
        }
        entry.series_.reset();
      }
    }
  }
};
#endif

} // namespace context_detail

/** @brief Identifies one cached bar series: an instrument at one bar interval. */
struct SeriesKey {
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The bar interval; one instrument at two intervals is two series. */
  BarInterval interval_{BarInterval::ONE_DAY};

  /**
   * @brief Orders keys by instrument, then interval, so they can key a map.
   * @param lhs The first key.
   * @param rhs The second key.
   * @return `true` if `lhs` sorts first.
   */
  friend bool operator<(const SeriesKey& lhs, const SeriesKey& rhs) {
    if (!(lhs.instrument_ == rhs.instrument_)) {
      return lhs.instrument_ < rhs.instrument_;
    }
    return lhs.interval_ < rhs.interval_;
  }
};

/**
 * @brief What a strategy's handlers receive as `ctx`: everything the strategy may read, and where it
 *   queues what it wants.
 *
 * The same type in a dry run, a live session and a backtest. Nothing here performs I/O: every read
 * comes from caches the runner filled, and `submit` and `cancel` only queue requests, which the
 * runner checks and sends after the handler returns. The members under "What the runner uses" are
 * for runners, not strategies.
 *
 * @tparam BrokerTag The broker tag of order references (a strategy reads them with `auto`).
 * @tparam CapsType The broker capabilities, exposed as `caps` for `if constexpr`.
 */
template <typename BrokerTag, typename CapsType>
class Context {
public:
  /** @brief The broker tag of the order references. */
  using Broker = BrokerTag;
  /** @brief The broker capabilities, for `if constexpr (Ctx::caps::has_depth)` and the like. */
  using caps = CapsType;

  /** @brief Deleted: a context cannot be copied, so a handler cannot take it by value. */
  Context(const Context&) = delete;
  /**
   * @brief Deleted: a context cannot be copied.
   * @return Not callable.
   */
  Context& operator=(const Context&) = delete;
  /** @brief Move constructor: takes the state of the context passed in, which is left valid but unspecified. */
  Context(Context&&) = default;
  /**
   * @brief Move assignment: takes the other context's state.
   * @return `*this`.
   */
  Context& operator=(Context&&) = default;

  /** @brief A log that keeps lines in memory for the runner to emit or a test to read. */
  class Log {
  public:
    /**
     * @brief Records one line; when 4096 lines are held, the oldest is dropped.
     * @param message The line.
     */
    void note(std::string_view message) {
      if (lines_.size() >= capacity_) {
        lines_.erase(lines_.begin());
      }
      lines_.emplace_back(message);
    }

    /**
     * @brief Returns the lines recorded since the last `take`.
     * @return The lines, oldest first.
     */
    const std::vector<std::string>& lines() const noexcept { return lines_; }

    /**
     * @brief Removes and returns the recorded lines.
     * @return The lines, oldest first.
     */
    std::vector<std::string> take() {
      std::vector<std::string> taken;
      taken.swap(lines_);
      return taken;
    }

  private:
    // Bounded so a long session cannot grow it without limit.
    static constexpr std::size_t capacity_ = 4096;
    std::vector<std::string> lines_;
  };

  /**
   * @brief Constructs a context.
   * @param series_capacity How many trailing bars each cached series keeps; must be positive with a
   *   linear algebra backend, or the first `record_bar` throws.
   */
  explicit Context(std::size_t series_capacity = 512) : series_capacity_(series_capacity) {}

  // --- What a strategy reads -------------------------------------------------------------

  /**
   * @brief Returns the instant the runner is acting at.
   * @return The current instant (a replay's clock in a backtest).
   */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Returns the market phase.
   * @return The phase.
   */
  MarketPhase phase() const noexcept { return phase_; }

  /**
   * @brief Returns the trading calendar the runner last read.
   * @return The calendar; empty if none was read.
   */
  const TradingCalendar& calendar() const noexcept { return calendar_; }

  /**
   * @brief Returns the portfolio: positions, buying power and the orders still working.
   * @return The portfolio.
   */
  const Portfolio<BrokerTag>& portfolio() const noexcept { return portfolio_; }

  /**
   * @brief Returns an instrument's reference data, including its price grid.
   * @param id The instrument.
   * @return The reference data, or `std::nullopt` if the runner has not loaded it.
   */
  std::optional<Instrument> instrument(const InstrumentId& id) const {
    const auto found = instruments_.find(id);
    if (found == instruments_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  /**
   * @brief Returns an instrument's reference data without copying it.
   *
   * Valid until the instrument's reference data is replaced (`set_instrument`) or the context is destroyed;
   * read it at once rather than keeping it.
   *
   * @param id The instrument.
   * @return The reference data, or a null pointer if the runner has not loaded it.
   */
  const Instrument* find_instrument(const InstrumentId& id) const {
    const auto found = instruments_.find(id);
    return found == instruments_.end() ? nullptr : &found->second;
  }

  /**
   * @brief Returns the latest price seen for an instrument (a quote's last price or a bar's close).
   * @param id The instrument.
   * @return The price, or `std::nullopt` if none has been seen.
   */
  std::optional<Decimal64> last_price(const InstrumentId& id) const {
    const auto found = prices_.find(id);
    if (found == prices_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

#if defined(ATL_HAS_EIGEN)
  /**
   * @brief Returns the cached trailing bars of an instrument at an interval.
   *
   * Only with a linear algebra backend (`ATL_USE_EIGEN=ON`). Use `SeriesBuffer::full()` to check
   * there are enough bars for a window.
   *
   * @param id The instrument.
   * @param interval The bar interval.
   * @return A pointer to the cache (valid until the next bar is recorded), or `nullptr` if no bar
   *   of that series has been seen.
   * @throws std::logic_error if the runner keeps no bar history, because its strategy declares
   *   `reads_bar_history = false`; also before any bar.
   */
  const analytics::SeriesBuffer* bars(
    const InstrumentId& id,
    BarInterval interval = BarInterval::ONE_DAY
  ) const {
    require_bar_history();
    const auto found = history_.entries_.find(id);
    if (found == history_.entries_.end()) {
      return nullptr;
    }
    const auto entry = found->second.find(interval);
    if (entry == found->second.end() || !entry->second.series_) {
      return nullptr;
    }
    return &*entry->second.series_;
  }
#endif

  /**
   * @brief Returns the log.
   * @return The log.
   */
  Log& log() noexcept { return log_; }

  // --- What a strategy asks for ---------------------------------------------------------

  /**
   * @brief Queues an order; the runner checks and sends it after the handler returns.
   * @param intent The order.
   */
  void submit(OrderIntent intent) { queued_.emplace_back(std::move(intent)); }

  /**
   * @brief Queues a target position; the runner trades the difference from the portfolio.
   * @param target The target.
   */
  void submit(TargetPosition target) { queued_.emplace_back(std::move(target)); }

  /**
   * @brief Queues a cancellation of a working order.
   * @param ref The order, from `portfolio().working_`.
   * @param tag A label for the cancellation.
   */
  void cancel(const OrderRef<BrokerTag>& ref, std::string tag = {}) {
    queued_.emplace_back(CancelIntent<BrokerTag>{ref, std::move(tag)});
  }

  // --- What the runner uses ------------------------------------------------------------

  /**
   * @brief Returns the queued requests without removing them (for runners).
   * @return The requests, in order.
   */
  const std::vector<Instruction<BrokerTag>>& queued() const noexcept { return queued_; }

  /**
   * @brief Removes and returns the queued requests (for runners).
   * @return The requests, in order.
   */
  std::vector<Instruction<BrokerTag>> drain() {
    std::vector<Instruction<BrokerTag>> taken;
    taken.swap(queued_);
    return taken;
  }

  /**
   * @brief Sets the current instant (for runners).
   * @param when The instant.
   */
  void set_now(std::chrono::system_clock::time_point when) noexcept { now_ = when; }

  /**
   * @brief Sets the market phase (for runners).
   * @param phase The phase.
   */
  void set_phase(MarketPhase phase) noexcept { phase_ = phase; }

  /**
   * @brief Replaces the trading calendar (for runners and sources, not strategies).
   *
   * A strategy should only read `calendar()`: an order submitted in the same handler call that
   * changed the calendar is checked against the phase from before the change.
   * `runner::LoopReport::sends_under_a_revised_phase_` counts such sends.
   *
   * @param calendar The calendar.
   */
  void set_calendar(TradingCalendar calendar) { calendar_ = std::move(calendar); }

  /**
   * @brief Replaces the portfolio (for runners).
   * @param portfolio The portfolio.
   */
  void set_portfolio(Portfolio<BrokerTag> portfolio) {
    portfolio_ = std::move(portfolio);
    ++replacements_.value_;
  }

  /**
   * @brief Adds or replaces an instrument's reference data.
   * @param reference The reference data.
   */
  void set_instrument(Instrument reference) {
    auto key = reference.id_;
    instruments_.insert_or_assign(std::move(key), std::move(reference));
    ++reference_changes_;
  }

  /**
   * @brief Records an instrument's latest price, used to value market orders and size notionals.
   * @param id The instrument.
   * @param price The price.
   */
  void set_last_price(const InstrumentId& id, Decimal64 price) {
    prices_.insert_or_assign(id, std::move(price));
  }

#if defined(ATL_HAS_EIGEN)
  /**
   * @brief Appends a bar to its cached series, creating the series at the configured capacity.
   * @param event The bar.
   * @throws std::invalid_argument if the bar is not later than the series' newest bar, or the series
   *   capacity is zero.
   * @throws std::logic_error if the context keeps no bar history (see `bars`).
   */
  void record_bar(const BarEvent& event) {
    require_bar_history();
    series_for(event).push(event.bar_);
  }
#endif

private:
  friend class context_detail::PortfolioAccess;

  // The two per-instrument tables below are looked up by id and never iterated, so their order is
  // nobody's to observe.
  using InstrumentHash = id_detail::InstrumentIdHash;

#if defined(ATL_HAS_EIGEN)
  // The series a bar of `event`'s instrument and interval goes into, made if it is the first; found by hash,
  // without building a key, and the instrument copied only for its first bar. A series without bars refuses
  // a bar not later than a newest bar time a context keeping no history brought in; its first push then sets
  // a later one.
  analytics::SeriesBuffer& series_for(const BarEvent& event) {
    auto& entry = entry_for(event);
    if (!entry.series_) {
      entry.series_.emplace(series_capacity_);
    }
    if (entry.newest_ && entry.series_->empty() && !(*entry.newest_ < event.bar_.timestamp_)) {
      throw std::invalid_argument("a series buffer observation must be later than the last");
    }
    return *entry.series_;
  }

  // The newest bar time of `event`'s series, for a context keeping no history; made empty if it is the first.
  std::optional<std::chrono::system_clock::time_point>& bar_time_for(const BarEvent& event) {
    return entry_for(event).newest_;
  }

  context_detail::BarHistory::Entry& entry_for(const BarEvent& event) {
    auto found = history_.entries_.find(event.instrument_);
    if (found == history_.entries_.end()) {
      found = history_.entries_.emplace(event.instrument_, context_detail::BarHistory::ByInterval{}).first;
    }
    return found->second[event.interval_];
  }

  void require_bar_history() const {
    if (!history_.kept_) {
      throw std::logic_error(
        "ctx.bars() read by a strategy declaring reads_bar_history = false, so its runner keeps no bar "
        "history; remove the declaration or declare it true"
      );
    }
  }
#endif

  std::size_t series_capacity_;
  std::chrono::system_clock::time_point now_{};
  MarketPhase phase_{MarketPhase::UNKNOWN};
  TradingCalendar calendar_;
  Portfolio<BrokerTag> portfolio_;
  // Counted for `context_detail::PortfolioAccess`: replacements of the portfolio (by `set_portfolio` or by a
  // move into or out of this context), runners' edits of it in place, and changes of reference data.
  context_detail::Revision replacements_;
  std::uint64_t runner_edits_{0};
  std::uint64_t reference_changes_{0};
  std::unordered_map<InstrumentId, Instrument, InstrumentHash> instruments_;
  std::unordered_map<InstrumentId, Decimal64, InstrumentHash> prices_;
#if defined(ATL_HAS_EIGEN)
  // Each instrument's cached series by interval, or with no history kept, their newest bar times.
  context_detail::BarHistory history_;
#endif
  std::vector<Instruction<BrokerTag>> queued_;
  Log log_;
};

} // namespace runner
} // namespace algotradelib
