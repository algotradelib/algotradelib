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
#include <utility>
#include <variant>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * The strategy vocabulary, imported once.
 *
 * A runner speaks in the strategy's own types — it is the component that feeds one — so importing
 * them is clearer than qualifying every mention. The types stay defined in `strategy`, which is
 * what a strategy author includes; nothing is duplicated here.
 */
using strategy::BarEvent;
using strategy::CancelIntent;
using strategy::Fill;
using strategy::MarketPhase;
using strategy::OrderIntent;
using strategy::Portfolio;
using strategy::QuoteEvent;
using strategy::TargetPosition;

/**
 * Everything a strategy sees, and the only thing it is given.
 *
 * One type, shared by every runner, which is what makes a strategy's source identical in a dry
 * run, a live session, and later a backtest. A runner fills the caches and hands the same context
 * to the same handlers; only where the data came from differs.
 *
 * **Nothing here performs I/O.** Every read is from a cache the runner has already filled, so a
 * handler cannot block and cannot issue a request. Every write is queued and drained by the runner
 * after the handler returns, so a handler cannot send an order either — which is what keeps order
 * safety in one place rather than restated in every strategy.
 *
 * Templated on the broker tag and on the capability set. The tag is only reachable through
 * `OrderRef`, which a strategy takes with `auto`, so a strategy's source still names no broker.
 */

/**
 * One thing a strategy asked for, in the order it asked.
 *
 * A single ordered queue rather than one list per kind, because the order is part of the meaning.
 * A strategy that cancels a resting order and then states a target expects the cancellation first;
 * draining targets before cancellations would reconcile against orders the strategy had already
 * disowned and produce the opposite instruction.
 */
template <typename Broker>
using Instruction = std::variant<OrderIntent, TargetPosition, CancelIntent<Broker>>;

/** Identifies one cached series: an instrument at one bar interval. */
struct SeriesKey {
  /** The instrument the series is for. */
  InstrumentId instrument_;
  /** The bar interval, so the same instrument at two intervals is two series. */
  BarInterval interval_{BarInterval::ONE_DAY};

  /** Orders keys so they can live in a map; the ordering itself carries no meaning. */
  friend bool operator<(const SeriesKey& lhs, const SeriesKey& rhs) {
    if (lhs.instrument_.country_ != rhs.instrument_.country_) {
      return lhs.instrument_.country_ < rhs.instrument_.country_;
    }
    if (lhs.instrument_.venue_ != rhs.instrument_.venue_) {
      return lhs.instrument_.venue_ < rhs.instrument_.venue_;
    }
    if (lhs.instrument_.symbol_ != rhs.instrument_.symbol_) {
      return lhs.instrument_.symbol_ < rhs.instrument_.symbol_;
    }
    return lhs.interval_ < rhs.interval_;
  }
};

/** The context a runner presents to a strategy. */
template <typename BrokerTag, typename CapsType>
class Context {
public:
  /** The tag identifying the broker whose references this context hands out. */
  using Broker = BrokerTag;
  /** The capability set a strategy gates on with `if constexpr`. */
  using caps = CapsType;

  /** A structured, non-blocking log. Lines are kept for the runner to emit or a test to read. */
  class Log {
  public:
    /** Records one line. Never blocks and never throws on a full buffer; the oldest is dropped. */
    void note(std::string_view message) {
      if (lines_.size() >= capacity_) {
        lines_.erase(lines_.begin());
      }
      lines_.emplace_back(message);
    }

    /** Returns the lines recorded since they were last taken. */
    const std::vector<std::string>& lines() const noexcept { return lines_; }

    /** Removes and returns the recorded lines. */
    std::vector<std::string> take() {
      std::vector<std::string> taken;
      taken.swap(lines_);
      return taken;
    }

  private:
    /**
     * Bounded on purpose.
     *
     * A strategy that logs every bar over a long session would otherwise grow this without limit
     * inside a component that must not allocate unboundedly. Dropping the oldest line is a worse
     * log; growing without bound is a worse program.
     */
    static constexpr std::size_t capacity_ = 4096;
    std::vector<std::string> lines_;
  };

  /** Constructs a context with the given warm-up window for cached series. */
  explicit Context(std::size_t series_capacity = 512) : series_capacity_(series_capacity) {}

  // --- What a strategy reads -------------------------------------------------------------

  /** Returns the instant the runner is acting at. */
  std::chrono::system_clock::time_point now() const noexcept { return now_; }

  /** Returns where the clock stands relative to the trading session. */
  MarketPhase phase() const noexcept { return phase_; }

  /** Returns the trading calendar the runner last read. */
  const TradingCalendar& calendar() const noexcept { return calendar_; }

  /** Returns positions, cash, and the orders already working. */
  const Portfolio<BrokerTag>& portfolio() const noexcept { return portfolio_; }

  /**
   * Returns reference data for `id`, or nothing when the runner has not loaded it.
   *
   * Nothing rather than a default, because the `PriceRule` on it is what turns a computed price
   * into a valid one, and a defaulted rule would be an unknown grid that silently refused every
   * price. Absent says "not loaded yet"; a default would say "there is no grid".
   */
  std::optional<Instrument> instrument(const InstrumentId& id) const {
    const auto found = instruments_.find(key_of(id));
    if (found == instruments_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  /** Returns the most recent quoted price for `id`, or nothing when none has been seen. */
  std::optional<Decimal64> last_price(const InstrumentId& id) const {
    const auto found = prices_.find(key_of(id));
    if (found == prices_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

#if defined(ATL_HAS_EIGEN)
  /**
   * Returns the trailing bars of one instrument at one interval, or nothing when none are cached.
   *
   * A view into the runner's own cache: contiguous, not copied, and safe to hand straight to an
   * indicator. `analytics::RollingWindow::full()` says whether there are enough for a window.
   *
   * **Available only when the library is built with a linear-algebra backend.** The series type is
   * an analytics type, and the strategy surface itself requires no backend so that a strategy
   * trading on prices and positions alone does not have to acquire one. A strategy that reads
   * series is, by definition, one that computes on them.
   */
  const analytics::RollingWindow* bars(
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

  /** Returns the log. */
  Log& log() noexcept { return log_; }

  // --- What a strategy asks for ---------------------------------------------------------

  /** Queues an order. Nothing is sent until the runner drains the queue. */
  void submit(OrderIntent intent) { queued_.emplace_back(std::move(intent)); }

  /** Queues a target position, which the runner diffs against the portfolio. */
  void submit(TargetPosition target) { queued_.emplace_back(std::move(target)); }

  /** Queues a cancellation of one working order. */
  void cancel(const OrderRef<BrokerTag>& ref, std::string tag = {}) {
    queued_.emplace_back(CancelIntent<BrokerTag>{ref, std::move(tag)});
  }

  // --- What the runner uses ------------------------------------------------------------

  /** Returns what the strategy asked for, in order, without removing it. */
  const std::vector<Instruction<BrokerTag>>& queued() const noexcept { return queued_; }

  /** Removes and returns what the strategy asked for, in order. */
  std::vector<Instruction<BrokerTag>> drain() {
    std::vector<Instruction<BrokerTag>> taken;
    taken.swap(queued_);
    return taken;
  }

  /** Sets the instant the runner is acting at. */
  void set_now(std::chrono::system_clock::time_point when) noexcept { now_ = when; }

  /** Sets the market phase. */
  void set_phase(MarketPhase phase) noexcept { phase_ = phase; }

  /**
   * Replaces the trading calendar.
   *
   * **Whose call this is matters.** In a live run the calendar belongs to `runner::PollingLoop`, which
   * derives the market phase from it and re-derives before every call it makes into a source or a
   * strategy; a source writes it during `PollKind::CALENDAR` and a replay states it once through
   * `backtest::ReplayConfig::calendar_`. A **strategy** must not write it: `on_phase` and `on_stop`
   * receive a mutable context, and a handler that revised the calendar and then submitted an order in
   * the same call would have that order checked against the phase as it stood before the call -- the
   * loop cannot intervene inside its own call into a handler. It does notice afterwards and reports it
   * as `runner::LoopReport::sends_under_a_revised_phase_`, which is the most that is available.
   *
   * A strategy that wants to know whether a date trades **reads** `calendar()`, which is what it is
   * published for.
   */
  void set_calendar(TradingCalendar calendar) { calendar_ = std::move(calendar); }

  /** Replaces the portfolio snapshot. */
  void set_portfolio(Portfolio<BrokerTag> portfolio) { portfolio_ = std::move(portfolio); }

  /** Adds or replaces reference data for one instrument. */
  void set_instrument(Instrument reference) {
    const auto key = key_of(reference.id_);
    instruments_.insert_or_assign(key, std::move(reference));
  }

  /** Records the latest quoted price, used to value market orders and size notionals. */
  void set_last_price(const InstrumentId& id, Decimal64 price) {
    prices_.insert_or_assign(key_of(id), std::move(price));
  }

#if defined(ATL_HAS_EIGEN)
  /**
   * Folds one bar into the cached series for its instrument and interval.
   *
   * The series is created on first use at the context's configured capacity, so a strategy's
   * warm-up window is set once by the runner rather than guessed per instrument.
   */
  void record_bar(const BarEvent& event) {
    const SeriesKey key{event.instrument_, event.interval_};
    auto found = series_.find(key);
    if (found == series_.end()) {
      found = series_.emplace(key, analytics::RollingWindow(series_capacity_)).first;
    }
    found->second.push(event.bar_);
  }
#endif

private:
  /** Flattens an instrument identity into a map key, so lookups do not allocate a string. */
  static SeriesKey key_of(const InstrumentId& id) { return SeriesKey{id, BarInterval::ONE_DAY}; }

  std::size_t series_capacity_;
  std::chrono::system_clock::time_point now_{};
  MarketPhase phase_{MarketPhase::UNKNOWN};
  TradingCalendar calendar_;
  Portfolio<BrokerTag> portfolio_;
  std::map<SeriesKey, Instrument> instruments_;
  std::map<SeriesKey, Decimal64> prices_;
#if defined(ATL_HAS_EIGEN)
  std::map<SeriesKey, analytics::RollingWindow> series_;
#endif
  std::vector<Instruction<BrokerTag>> queued_;
  Log log_;
};

} // namespace runner
} // namespace algotradelib
