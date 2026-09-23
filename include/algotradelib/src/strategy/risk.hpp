#pragma once

#include "./event.hpp"
#include "./portfolio.hpp"
#include "./reconcile.hpp"

#include <algotradelib/core>

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * Who or what is holding trading stopped.
 *
 * A halt used to be one `bool`, which was exactly right when the only caller was a person deciding
 * to stop trading: one holder, one bool. It stopped being right once machinery held it too — a
 * polling loop's gate, a journal that cannot be written — because two holders of one bool cannot
 * tell each other apart, and the one that clears it clears the other's intent with it.
 *
 * So a hold names itself, and trading resumes only when every hold is released. The enumerators are
 * ordered **most fundamental first**, and `RiskGuard::blocking_reason` relies on that order: an
 * operator reading a refusal sees the reason that most needs their attention rather than whichever
 * condition happens to be checked first. A new reason goes where its gravity puts it, not at the end.
 */
enum class HaltReason {
  /** A person stopped trading, through `halt()` with no argument. Cleared only by a person. */
  OPERATOR,
  /**
   * Local machinery cannot be trusted, so nothing may be sent through it.
   *
   * The order journal is the motivating case: a runner that cannot record what it is about to do
   * must not do it, because every later order would fail the same way and produce a refusal per bar
   * that reads like the broker declining -- when in fact nothing left the process. Sticky, carries
   * the reason as its detail, and released only by `runner::Runner::resume_after_local_failure`.
   */
  LOCAL_FAILURE,
  /** A stop intent is being honoured and the run is on its way out. */
  STOPPING,
  /**
   * The broker's order list has not been read since this process started.
   *
   * The cheapest and most valuable half of order-journal reconciliation, and it needs no
   * persistence at all. A runner that has just started cannot know whether a *previous* process
   * died mid-POST, and an empty `runner::IndeterminacyLedger` does not mean nothing is owed -- it
   * means nothing is remembered. Until the list has been read once, sending could double a position
   * left by a predecessor.
   */
  ORDER_LIST_UNREAD,
  /**
   * The holdings snapshot is older than the configured maximum.
   *
   * Reconciling a target against a stale portfolio is how a position gets double-sized by a route
   * that `working_quantity` does not close: the runner subtracts a position it no longer holds, or
   * fails to subtract one it acquired since.
   */
  PORTFOLIO_STALE,
  /**
   * A poll kept failing, so the loop gave up rather than spinning.
   *
   * Distinct from `STOPPING` because nobody asked for it: the loop decided, and an operator reading
   * the report needs to know which of the two happened.
   */
  POLL_FAILING,
};

/** How many `HaltReason` enumerators there are, for the guard's fixed-capacity storage. */
inline constexpr std::size_t halt_reason_count = 6;

/** Returns a stable lowercase name for `reason`, for logs and structured reporting. */
inline constexpr std::string_view halt_reason_name(HaltReason reason) noexcept {
  switch (reason) {
    case HaltReason::OPERATOR:
      return "operator";
    case HaltReason::LOCAL_FAILURE:
      return "local_failure";
    case HaltReason::STOPPING:
      return "stopping";
    case HaltReason::ORDER_LIST_UNREAD:
      return "order_list_unread";
    case HaltReason::PORTFOLIO_STALE:
      return "portfolio_stale";
    case HaltReason::POLL_FAILING:
      return "poll_failing";
  }
  return "unknown";
}

/**
 * The limits an operator imposes, checked before anything is sent.
 *
 * Separate from reconciliation, and the separation is the point. Reconciliation answers "what
 * instruction expresses this decision"; a guard answers "is this instruction permitted". The
 * failures read differently in a log — `RejectReason` means the library could not build an order,
 * `GuardReason` means the operator forbade one — and conflating them would make a configuration
 * mistake look like a data problem.
 *
 * **A guard rejects; it never resizes.** Halving an order that exceeded a notional limit would
 * place a position the strategy never sized, against a signal it never gave, and the strategy
 * would then reconcile against that position and compound the error. Refusing leaves the strategy
 * to decide, which is where the decision belongs.
 */

/** Which limit refused an order. */
enum class GuardReason {
  /** The kill switch is on. */
  TRADING_HALTED,
  /** The instrument is outside the permitted universe. */
  INSTRUMENT_NOT_PERMITTED,
  /** The market is not in a phase the operator permits trading in. */
  PHASE_NOT_PERMITTED,
  /** The order's value exceeds the per-order ceiling. */
  ORDER_NOTIONAL_TOO_LARGE,
  /** The resulting position would exceed the per-instrument ceiling. */
  POSITION_LIMIT_EXCEEDED,
  /** The resulting gross exposure would exceed the portfolio ceiling. */
  GROSS_EXPOSURE_EXCEEDED,
  /** Too many orders have been sent in the current window. */
  ORDER_RATE_EXCEEDED,
  /** A limit needed the order's value and no price was available to compute it. */
  VALUE_NOT_KNOWN,
};

/** Returns a stable lowercase name for `reason`, for logs and structured reporting. */
inline constexpr std::string_view guard_reason_name(GuardReason reason) noexcept {
  switch (reason) {
    case GuardReason::TRADING_HALTED:
      return "trading_halted";
    case GuardReason::INSTRUMENT_NOT_PERMITTED:
      return "instrument_not_permitted";
    case GuardReason::PHASE_NOT_PERMITTED:
      return "phase_not_permitted";
    case GuardReason::ORDER_NOTIONAL_TOO_LARGE:
      return "order_notional_too_large";
    case GuardReason::POSITION_LIMIT_EXCEEDED:
      return "position_limit_exceeded";
    case GuardReason::GROSS_EXPOSURE_EXCEEDED:
      return "gross_exposure_exceeded";
    case GuardReason::ORDER_RATE_EXCEEDED:
      return "order_rate_exceeded";
    case GuardReason::VALUE_NOT_KNOWN:
      return "value_not_known";
  }
  return "unknown";
}

/**
 * One order a guard refused, with enough to act on rather than only to read.
 *
 * `limit_` and `attempted_` are carried so an operator can see how far over the order was without
 * reconstructing the arithmetic, and so a strategy can adapt rather than retry the same size.
 */
struct GuardRejection {
  GuardReason reason_{GuardReason::TRADING_HALTED};
  InstrumentId instrument_;
  std::string tag_;
  /**
   * Which hold stopped the order, when `reason_` is `TRADING_HALTED`.
   *
   * Present for that reason and absent for every other, because a halt is the one refusal whose
   * *cause* lives outside the limits — an operator, a loop's gate, a journal that failed — and
   * "trading is halted" without saying by what is the diagnostic this type exists to avoid.
   */
  std::optional<HaltReason> halt_reason_;
  /** The ceiling that was exceeded, where the reason has one. */
  std::optional<Decimal64> limit_;
  /** What the order would have reached, where the reason has one. */
  std::optional<Decimal64> attempted_;
};

/**
 * What the operator permits.
 *
 * Every limit is optional, and absent means unlimited — except the two where an unlimited default
 * would be unsafe rather than merely permissive, which is why they are shaped differently:
 *
 * - `permitted_phases_` defaults to trading only while the market is `OPEN`. An absent value
 *   explicitly means any phase, which an operator has to ask for. `MarketPhase::UNKNOWN` is
 *   therefore refused by default, which is the point: an uncovered calendar date must not become
 *   an order placed into a holiday.
 * - Nothing is halted to begin with, but `halt()` is one call and takes effect before every other
 *   check. A halt is not a limit and does not live here: it is held on the guard itself, by
 *   `HaltReason`, so that the operator's hold and the machinery's cannot clear one another.
 */
struct RiskLimits {
  /** Largest value a single order may have, in the currency it is priced in. */
  std::optional<Decimal64> max_order_notional_;
  /** Largest absolute position, in shares, any one instrument may reach. */
  std::optional<Decimal64> max_position_quantity_;
  /** Largest total exposure the portfolio may reach. */
  std::optional<Decimal64> max_gross_exposure_;
  /** Most orders that may be sent within `rate_window_`. */
  std::optional<std::size_t> max_orders_per_window_;
  std::chrono::seconds rate_window_{60};
  /** Instruments that may be traded; absent means any. */
  std::optional<std::vector<InstrumentId>> permitted_universe_;
  /** Phases trading is allowed in; absent means any. */
  std::optional<std::vector<MarketPhase>> permitted_phases_{
    std::vector<MarketPhase>{MarketPhase::OPEN}
  };
};

/** What a guard needs about the world that is not the order or the portfolio. */
struct GuardInputs {
  std::chrono::system_clock::time_point now_{};
  MarketPhase phase_{MarketPhase::UNKNOWN};
  /** A price to value a market order with, since it carries none of its own. */
  std::optional<Decimal64> reference_price_;
  /** Current gross exposure, for the portfolio ceiling. */
  std::optional<Decimal64> gross_exposure_;
};

/**
 * Checks planned orders against the operator's limits, and remembers what was sent.
 *
 * Stateful, unlike reconciliation, because a rate limit is a statement about history. The state is
 * only the timestamps of orders actually sent, which is why `note_sent` is separate from `check`:
 * an order that was checked and then refused by a later guard, or never reached the broker, must
 * not consume rate budget. Recording at the point of sending rather than of checking is what keeps
 * the count honest.
 *
 * Owned by one runner and not synchronized, the model every stateful type in this library follows.
 */
class RiskGuard {
public:
  /** Constructs a guard enforcing `limits`. */
  explicit RiskGuard(RiskLimits limits = {}) : limits_(std::move(limits)) {}

  /** Returns the limits in force. */
  const RiskLimits& limits() const noexcept { return limits_; }

  /**
   * Stops every order until `resume()`, on the operator's behalf. Takes effect before any other
   * check.
   *
   * **The no-argument call is the operator's hold**, `HaltReason::OPERATOR`, and that is not merely
   * source compatibility: a bare `halt()` in a program or at a prompt is a person deciding to stop,
   * and a person's intent is exactly the one that must not be cleared by machinery that happens to
   * be holding the same switch. Every other holder names itself.
   *
   * **This is *quiesce*, and only that**: it stops this runner from *sending*. Three things an
   * operator might mean by "stop" are worth keeping apart, because each leaves a different position:
   *
   * - *quiesce* -- no new orders; positions held, working orders still resting. This.
   * - *withdraw* -- quiesce, then cancel the working orders. Nothing resting.
   * - *flatten* -- withdraw, then close the positions. Out of the market.
   *
   * Flattening is a strategy's decision, which is what `vanilla::TimedLiquidation` exists for, or an
   * operator's through the broker's own tools. It is deliberately not something a risk limit does.
   * `runner::StopIntent` names all three for a long-running session, and `runner::PollingLoop`
   * honours them; this switch is the primitive underneath the first of them.
   *
   * In-process and in-memory: nothing reaches this from outside the program, and it does not survive
   * the process. `runner::FileStopChannel` and `runner::SignalStop` are the out-of-band channels.
   */
  void halt() noexcept { held_[index_of(HaltReason::OPERATOR)] = true; }

  /**
   * Takes the hold named by `reason`, with an operator-readable `detail`.
   *
   * **Idempotent, and a set rather than a count.** Taking a hold that is already held replaces its
   * detail and nothing else, and one `resume(reason)` releases it however many times it was taken.
   * That is deliberate: the holders are distinct *conditions*, not distinct callers, so a loop that
   * recomputes `PORTFOLIO_STALE` on every iteration must not build a hold that as many refreshes are
   * needed to clear. A counter would get exactly that wrong, in the opposite direction from the
   * single bool this replaces.
   */
  void halt(HaltReason reason, std::string detail) {
    held_[index_of(reason)] = true;
    details_[index_of(reason)] = std::move(detail);
  }

  /** Takes the hold named by `reason`, with no detail. */
  void halt(HaltReason reason) { halt(reason, std::string()); }

  /** Releases the operator's hold. Other holds, if any, still stop trading. */
  void resume() noexcept { resume(HaltReason::OPERATOR); }

  /** Releases the hold named by `reason`. Releasing one that is not held does nothing. */
  void resume(HaltReason reason) noexcept {
    held_[index_of(reason)] = false;
    details_[index_of(reason)].clear();
  }

  /** Returns whether anything at all is stopping trading. */
  bool halted() const noexcept {
    for (const auto held : held_) {
      if (held) {
        return true;
      }
    }
    return false;
  }

  /** Returns whether `reason` in particular is holding. */
  bool halted(HaltReason reason) const noexcept { return held_[index_of(reason)]; }

  /**
   * Returns the most fundamental hold in force, or nothing when trading is permitted.
   *
   * "Most fundamental" is the declaration order of `HaltReason`, which is why that order is part of
   * the enum's contract. This is what a refusal reports, and it follows the rule `check` already
   * applies to limits: say the reason that most needs attention, not an incidental one.
   */
  std::optional<HaltReason> blocking_reason() const noexcept {
    for (std::size_t index = 0; index < halt_reason_count; ++index) {
      if (held_[index]) {
        return static_cast<HaltReason>(index);
      }
    }
    return std::nullopt;
  }

  /** Returns the detail recorded with `reason`'s hold, or an empty string when there is none. */
  const std::string& hold_detail(HaltReason reason) const noexcept {
    return details_[index_of(reason)];
  }

  /**
   * Returns every hold in force, most fundamental first.
   *
   * For diagnosis — an operator asking why trading is stopped when more than one thing is stopping
   * it, which is precisely the case the single bool could not answer. `blocking_reason` is what the
   * send path uses.
   */
  std::vector<HaltReason> holds() const {
    std::vector<HaltReason> held;
    for (std::size_t index = 0; index < halt_reason_count; ++index) {
      if (held_[index]) {
        held.push_back(static_cast<HaltReason>(index));
      }
    }
    return held;
  }

  /**
   * Records that an order was sent at `when`, for the rate limit.
   *
   * Called by the runner after the broker accepted the instruction, not when it was checked.
   */
  void note_sent(std::chrono::system_clock::time_point when) { sent_.push_back(when); }

  /** Returns how many orders were sent within `rate_window_` of `now`. */
  std::size_t recent_orders(std::chrono::system_clock::time_point now) const {
    std::size_t count = 0;
    for (const auto when : sent_) {
      if (now - when < limits_.rate_window_) {
        ++count;
      }
    }
    return count;
  }

  /** Drops recorded sends that can no longer affect the rate limit. */
  void forget_before(std::chrono::system_clock::time_point cutoff) {
    while (!sent_.empty() && sent_.front() < cutoff) {
      sent_.pop_front();
    }
  }

  /**
   * Returns the reason `order` is not permitted, or nothing when it is.
   *
   * Checked in order of cost and of bluntness: the kill switch, then what the operator has
   * forbidden outright, then the arithmetic ones. That order means an operator reading a log sees
   * the most fundamental reason rather than an incidental one — a halted system reports the halt,
   * not a notional ceiling it also happened to exceed.
   */
  template <typename Broker>
  std::optional<GuardRejection> check(
    const PlannedOrder& order,
    const Portfolio<Broker>& portfolio,
    const GuardInputs& inputs
  ) const {
    const auto refuse = [&order](GuardReason reason) {
      GuardRejection rejection;
      rejection.reason_ = reason;
      rejection.instrument_ = order.instrument_;
      rejection.tag_ = order.tag_;
      return std::optional<GuardRejection>(rejection);
    };
    const auto refuse_with =
      [&order](GuardReason reason, Decimal64 limit, Decimal64 attempted) {
        GuardRejection rejection;
        rejection.reason_ = reason;
        rejection.instrument_ = order.instrument_;
        rejection.tag_ = order.tag_;
        rejection.limit_ = std::move(limit);
        rejection.attempted_ = std::move(attempted);
        return std::optional<GuardRejection>(rejection);
      };

    if (const auto holding = blocking_reason()) {
      auto rejection = refuse(GuardReason::TRADING_HALTED);
      rejection->halt_reason_ = *holding;
      return rejection;
    }

    if (limits_.permitted_universe_) {
      bool permitted = false;
      for (const auto& allowed : *limits_.permitted_universe_) {
        if (allowed == order.instrument_) {
          permitted = true;
          break;
        }
      }
      if (!permitted) {
        return refuse(GuardReason::INSTRUMENT_NOT_PERMITTED);
      }
    }

    if (limits_.permitted_phases_) {
      bool permitted = false;
      for (const auto phase : *limits_.permitted_phases_) {
        if (phase == inputs.phase_) {
          permitted = true;
          break;
        }
      }
      if (!permitted) {
        return refuse(GuardReason::PHASE_NOT_PERMITTED);
      }
    }

    if (limits_.max_orders_per_window_ &&
        recent_orders(inputs.now_) >= *limits_.max_orders_per_window_) {
      return refuse_with(
        GuardReason::ORDER_RATE_EXCEEDED,
        Decimal64(static_cast<std::int64_t>(*limits_.max_orders_per_window_), 0),
        Decimal64(static_cast<std::int64_t>(recent_orders(inputs.now_) + 1), 0)
      );
    }

    // A market order carries no price, so its value has to come from the runner's last quote.
    // Without one, a notional or exposure limit cannot be evaluated, and a limit that cannot be
    // evaluated must refuse rather than wave the order through.
    const auto price = order.price_ ? order.price_ : inputs.reference_price_;
    const bool needs_value =
      limits_.max_order_notional_.has_value() || limits_.max_gross_exposure_.has_value();
    if (needs_value && !price) {
      return refuse(GuardReason::VALUE_NOT_KNOWN);
    }

    std::optional<Decimal64> value;
    if (price) {
      value = decimal_multiply(order.quantity_, *price);
      if (needs_value && !value) {
        return refuse(GuardReason::VALUE_NOT_KNOWN);
      }
    }

    if (limits_.max_order_notional_ && value &&
        decimal_less(*limits_.max_order_notional_, *value)) {
      return refuse_with(
        GuardReason::ORDER_NOTIONAL_TOO_LARGE, *limits_.max_order_notional_, *value
      );
    }

    if (limits_.max_position_quantity_) {
      const auto zero = Decimal64::parse("0");
      const auto held = portfolio.quantity_of(order.instrument_).value_or(zero);
      const auto signed_quantity = order.side_ == OrderSide::BUY ?
        std::optional<Decimal64>(order.quantity_) :
        decimal_subtract(zero, order.quantity_);
      if (!signed_quantity) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      const auto resulting = decimal_add(held, *signed_quantity);
      if (!resulting) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      // The ceiling is on magnitude, so a short of the same size is limited as a long is.
      const auto magnitude = decimal_less(*resulting, zero) ?
        decimal_subtract(zero, *resulting) :
        std::optional<Decimal64>(*resulting);
      if (!magnitude) {
        return refuse(GuardReason::POSITION_LIMIT_EXCEEDED);
      }
      if (decimal_less(*limits_.max_position_quantity_, *magnitude)) {
        return refuse_with(
          GuardReason::POSITION_LIMIT_EXCEEDED, *limits_.max_position_quantity_, *magnitude
        );
      }
    }

    if (limits_.max_gross_exposure_ && value) {
      const auto current = inputs.gross_exposure_.value_or(Decimal64::parse("0"));
      const auto resulting = decimal_add(current, *value);
      if (!resulting) {
        return refuse(GuardReason::GROSS_EXPOSURE_EXCEEDED);
      }
      if (decimal_less(*limits_.max_gross_exposure_, *resulting)) {
        return refuse_with(
          GuardReason::GROSS_EXPOSURE_EXCEEDED, *limits_.max_gross_exposure_, *resulting
        );
      }
    }

    return std::nullopt;
  }

private:
  /** Returns `reason`'s slot in the fixed-capacity hold arrays. */
  static constexpr std::size_t index_of(HaltReason reason) noexcept {
    return static_cast<std::size_t>(reason);
  }

  RiskLimits limits_;
  /**
   * Which holds are in force, indexed by `HaltReason`.
   *
   * A fixed array rather than a set or a vector because the reasons are a closed enum and there are
   * six of them: no allocation, no ordering to maintain, and declaration order is the priority order
   * `blocking_reason` needs for free.
   */
  std::array<bool, halt_reason_count> held_{};
  /** Each hold's operator-readable detail, empty where a hold carries none or is not held. */
  std::array<std::string, halt_reason_count> details_;
  /**
   * Timestamps of orders actually sent, oldest first.
   *
   * A deque rather than a counter because the window slides: a counter reset on a boundary would
   * let twice the limit through across one, which is the classic way a rate limit is wrong.
   */
  std::deque<std::chrono::system_clock::time_point> sent_;
};

} // namespace strategy
} // namespace algotradelib
