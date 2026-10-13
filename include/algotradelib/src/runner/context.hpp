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
#include <map>
#include <optional>
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
    const auto* found = find_instrument(id);
    if (found == nullptr) {
      return std::nullopt;
    }
    return *found;
  }

  /**
   * @brief Returns an instrument's reference data without copying it.
   * @param id The instrument.
   * @return A pointer to the reference data, or `nullptr` if the runner has not loaded it. The
   *   pointer stays valid across later `set_instrument` calls (one for the same instrument replaces
   *   what it points at) until the context is destroyed or assigned to.
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
   */
  const analytics::SeriesBuffer* bars(
    const InstrumentId& id,
    BarInterval interval = BarInterval::ONE_DAY
  ) const {
    const auto found = series_.find(SeriesKey{id, interval});
    if (found == series_.end()) {
      return nullptr;
    }
    return &found->second;
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
  void set_portfolio(Portfolio<BrokerTag> portfolio) { portfolio_ = std::move(portfolio); }

  /**
   * @brief Adds or replaces an instrument's reference data.
   * @param reference The reference data.
   */
  void set_instrument(Instrument reference) {
    auto key = reference.id_;
    instruments_.insert_or_assign(std::move(key), std::move(reference));
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
   */
  void record_bar(const BarEvent& event) {
    const SeriesKey key{event.instrument_, event.interval_};
    auto found = series_.find(key);
    if (found == series_.end()) {
      found = series_.emplace(key, analytics::SeriesBuffer(series_capacity_)).first;
    }
    found->second.push(event.bar_);
  }
#endif

private:
  // Hashes an instrument id for the two per-instrument tables below. Both are looked up by id and
  // never iterated, so their order is nobody's to observe.
  struct InstrumentHash {
    std::size_t operator()(const InstrumentId& id) const noexcept {
      std::size_t seed = std::hash<std::string>{}(id.symbol_);
      seed ^= static_cast<std::size_t>(id.country_) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
      seed ^= static_cast<std::size_t>(id.venue_) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
      return seed;
    }
  };

  std::size_t series_capacity_;
  std::chrono::system_clock::time_point now_{};
  MarketPhase phase_{MarketPhase::UNKNOWN};
  TradingCalendar calendar_;
  Portfolio<BrokerTag> portfolio_;
  std::unordered_map<InstrumentId, Instrument, InstrumentHash> instruments_;
  std::unordered_map<InstrumentId, Decimal64, InstrumentHash> prices_;
#if defined(ATL_HAS_EIGEN)
  std::map<SeriesKey, analytics::SeriesBuffer> series_;
#endif
  std::vector<Instruction<BrokerTag>> queued_;
  Log log_;
};

} // namespace runner
} // namespace algotradelib
