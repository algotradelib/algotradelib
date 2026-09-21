#pragma once

#include "./event.hpp"
#include "./portfolio.hpp"
#include "./reconcile.hpp"

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

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
 * - `halted_` starts false, but `halt()` is one call and takes effect before every other check.
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
   * Stops every order until `resume()`. Takes effect before any other check.
   *
   * In-process and in-memory. It stops this runner from *sending*; it does not withdraw anything
   * already sent, and it does not survive the process. Killing a program is not a kill switch in the
   * exchange's sense: orders the broker accepted stay live. Flattening is a strategy's decision --
   * see `vanilla::TimedLiquidation` -- or an operator's, through the broker's own tools.
   */
  void halt() noexcept { halted_ = true; }

  /** Allows orders again. */
  void resume() noexcept { halted_ = false; }

  /** Returns whether the kill switch is on. */
  bool halted() const noexcept { return halted_; }

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

    if (halted_) {
      return refuse(GuardReason::TRADING_HALTED);
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
  RiskLimits limits_;
  bool halted_{false};
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
