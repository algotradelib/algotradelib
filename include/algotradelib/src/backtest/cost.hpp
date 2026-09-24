#pragma once

#include <algotradelib/core>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace algotradelib {
namespace backtest {

/**
 * What a trade costs, as models a caller states rather than defaults a library picks.
 *
 * `analytics` deliberately carries no commission model, and this is why the gap had to be filled
 * somewhere: **a backtest without costs evaluates a strategy at a cost of zero, which is the single
 * most common way a backtest lies.** A strategy that turns over its book daily can look excellent at
 * zero cost and lose money at any real one.
 *
 * Both models default to charging nothing, and that default is deliberately *visible* rather than
 * convenient: a report states what it applied, so a zero-cost run is a reader's informed choice rather
 * than an oversight they cannot detect.
 */

/**
 * Commission on one execution.
 *
 * `rate_` is a fraction of the traded value, not a percentage — `0.00015` is the order of magnitude a
 * Korean online brokerage charges. `minimum_` is the floor some brokers apply per order, and it is the
 * reason a notional trade tolerance exists: a fixed floor is what makes a small trade uneconomic.
 *
 * Deliberately no tax model. Korean securities transaction tax applies on sales, at a rate that has
 * changed repeatedly, and hard-coding a number that moves would silently misprice every backtest run
 * after it changed. A caller who wants it states it as part of `rate_` for sells, knowingly.
 */
struct CommissionModel {
  /** Fraction of traded value charged, per execution. */
  Decimal64 rate_{Decimal64::parse("0")};
  /** Smallest commission charged on an execution that happens at all. */
  Decimal64 minimum_{Decimal64::parse("0")};
  /**
   * Whether the rate came from a broker's published schedule rather than from the caller.
   *
   * Carried on the model rather than passed alongside it, because the model is what reaches the
   * report: a flag the caller had to remember to set separately would be false on exactly the runs
   * where it mattered. Set only by `commission_model_from`.
   */
  bool from_broker_{false};

  /** Returns the commission on `quantity` at `price`, or nothing when it does not fit. */
  std::optional<Decimal64> on(const Decimal64& quantity, const Decimal64& price) const {
    const auto value = decimal_multiply(quantity, price);
    if (!value) {
      return std::nullopt;
    }
    auto charged = decimal_multiply(*value, rate_);
    if (!charged) {
      return std::nullopt;
    }
    // Rounded to whole currency units, because a broker charges a currency amount rather than a
    // fraction of one, and carrying fractions of a won would accumulate a fictitious difference.
    const auto whole = decimal_divide(*charged, Decimal64::parse("1"), 0);
    if (!whole) {
      return std::nullopt;
    }
    return decimal_less(*whole, minimum_) ? minimum_ : *whole;
  }

  /** Returns a model charging nothing, which a report will say it applied. */
  static CommissionModel none() { return CommissionModel{}; }

  /** Returns a rate-based model with an optional per-order floor. */
  static CommissionModel of(Decimal64 rate, Decimal64 minimum = Decimal64::parse("0")) {
    return CommissionModel{std::move(rate), std::move(minimum)};
  }
};

/**
 * Builds a cost model from the rate a broker published for `country` on `date`.
 *
 * **This is the only place a backtest's commission comes from anywhere but the caller**, and it
 * closes the gap that a replay charges a number someone guessed while the live account is charged
 * the broker's own. Only one of the two brokers publishes a schedule, so the model is seeded where
 * it can be and stated by the caller everywhere else.
 *
 * Three things it deliberately does not do:
 *
 * - **It sets no floor.** Toss publishes a rate and nothing else; a floor the broker did not state
 *   is unknown, not zero. `CommissionModel` has no way to carry "unknown", so `minimum_` stays at
 *   zero and this documentation is where a reader learns the difference. Widening the model to say
 *   so is a `backtest` change rather than this one.
 * - **It adds no tax.** `cost.hpp` refuses to hard-code Korean securities transaction tax for a
 *   reason that has not changed: the rate has moved repeatedly, and a stale constant silently
 *   misprices every run after it moved.
 * - **It answers for one date.** A replay spanning a rate change charges one rate throughout, which
 *   is honest and still wrong for the half of the period the other rate covered; charging per trade
 *   date needs a signature that takes one, and that is
 *   `plan/todo/commission-schedule-range.md`.
 *
 * A date no window covers is an error rather than a zero rate, because a rate of zero is a claim
 * the broker did not make and a zero-cost replay is the most flattering wrong answer there is.
 */
inline Result<CommissionModel> commission_model_from(
  const CommissionSchedule& schedule,
  Country country,
  const Date& date
) {
  const auto rate = schedule.rate_for(country, date);
  if (!rate) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    // Nothing was asked of anybody: this reads a schedule already in hand.
    error.reached_broker_ = false;
    error.broker_code_ = "no_rate_for_date";
    error.message_ = "the published schedule states no commission rate for that market and date";
    return Result<CommissionModel>(std::move(error));
  }
  auto model = CommissionModel::of(rate->rate_);
  model.from_broker_ = true;
  return Result<CommissionModel>(std::move(model));
}

/**
 * How far a fill lands from the price the model would otherwise give.
 *
 * Always **against** the order: a buy pays more and a sell receives less. Slippage that helped would
 * not be slippage, and a model that let it cut both ways would produce a backtest that was right on
 * average and flattering in exactly the cases a strategy trades most.
 *
 * `basis_points_` is a fraction of the price in ten-thousandths, which is how a spread is usually
 * quoted. `ticks_` adds a fixed number of grid steps, which is the better model for a wide-tick
 * instrument where the spread is one tick regardless of price.
 */
struct SlippageModel {
  /** Ten-thousandths of the price given up, per execution. */
  Decimal64 basis_points_{Decimal64::parse("0")};
  /** Whole tick steps given up, applied through the instrument's own grid. */
  int ticks_{0};

  /**
   * Returns `price` moved against `side`, and how far it moved.
   *
   * The tick component uses the instrument's grid, so one tick means one tick at that price level
   * rather than a number this model invented. An unknown grid applies no tick component rather than
   * guessing a step, which matches `PriceRule::UNKNOWN` refusing to round anywhere else.
   */
  std::optional<std::pair<Decimal64, Decimal64>> apply(
    const Decimal64& price,
    OrderSide side,
    const PriceRule& rule
  ) const {
    auto moved = price;

    if (basis_points_.coefficient() != 0) {
      const auto fraction = decimal_divide(basis_points_, Decimal64::parse("10000"), 8);
      if (!fraction) {
        return std::nullopt;
      }
      const auto amount = decimal_multiply(price, *fraction);
      if (!amount) {
        return std::nullopt;
      }
      const auto shifted = side == OrderSide::BUY ?
        decimal_add(moved, *amount) :
        decimal_subtract(moved, *amount);
      if (!shifted) {
        return std::nullopt;
      }
      moved = *shifted;
    }

    if (ticks_ > 0) {
      const auto tick = rule.tick_at(price);
      if (tick) {
        const auto steps = decimal_multiply(*tick, Decimal64(static_cast<std::int64_t>(ticks_), 0));
        if (!steps) {
          return std::nullopt;
        }
        const auto shifted = side == OrderSide::BUY ?
          decimal_add(moved, *steps) :
          decimal_subtract(moved, *steps);
        if (!shifted) {
          return std::nullopt;
        }
        moved = *shifted;
      }
      // An unknown grid contributes nothing rather than a guessed step.
    }

    const auto given_up = side == OrderSide::BUY ?
      decimal_subtract(moved, price) :
      decimal_subtract(price, moved);
    if (!given_up) {
      return std::nullopt;
    }
    return std::pair<Decimal64, Decimal64>{moved, *given_up};
  }

  /** Returns a model applying nothing, which a report will say it applied. */
  static SlippageModel none() { return SlippageModel{}; }

  /** Returns a model giving up `points` ten-thousandths of the price. */
  static SlippageModel of_basis_points(Decimal64 points) {
    SlippageModel model;
    model.basis_points_ = std::move(points);
    return model;
  }

  /** Returns a model giving up `count` tick steps at the instrument's own grid. */
  static SlippageModel of_ticks(int count) {
    SlippageModel model;
    model.ticks_ = count;
    return model;
  }
};

/** Whether a limit order is credited with a price better than it asked for. */
enum class LimitFillPrice {
  /**
   * Fill at the limit, never better.
   *
   * The default, and the conservative choice. A bar that gaps through a resting limit would often fill
   * better in reality, but claiming the improvement assumes the order was in the book before the
   * auction *and* that volume existed at that level — neither of which a bar can tell us. Refusing it
   * understates a result; claiming it overstates one, and a backtester that flatters is worse than one
   * that disappoints.
   */
  AT_LIMIT,
  /**
   * Fill at the bar's open when the bar opened through the limit.
   *
   * More realistic for an order that was genuinely resting, and it is opt-in because that assumption is
   * the caller's to make rather than the library's to make for them.
   */
  AT_OPEN_IF_BETTER,
};

/** Returns a stable lowercase name for `choice`, for a report to state what it assumed. */
inline constexpr std::string_view limit_fill_price_name(LimitFillPrice choice) noexcept {
  switch (choice) {
    case LimitFillPrice::AT_LIMIT:
      return "at_limit";
    case LimitFillPrice::AT_OPEN_IF_BETTER:
      return "at_open_if_better";
  }
  return "unknown";
}

} // namespace backtest
} // namespace algotradelib
