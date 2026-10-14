#pragma once

#include <algotradelib/core>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief Trading costs for a backtest: commission (`CommissionModel`, `CommissionPolicy`),
 *   slippage (`SlippageModel`) and how a limit order's fill price is chosen (`LimitFillPrice`).
 *
 * Every model charges nothing by default, and the backtest report states what was applied, so a
 * zero-cost run is visible. Sale tax is set on the fill model, `NextBarFillModel::with_sell_tax`.
 */

/**
 * @brief A flat commission: a rate on traded value, with an optional per-execution minimum.
 *
 * Charged on buys and sells alike; it carries no tax (see `NextBarFillModel::with_sell_tax`).
 */
struct CommissionModel {
  /** @brief The fraction of traded value charged per execution (`0.00015`, not `0.015`). */
  Decimal64 rate_{Decimal64::parse("0")};
  /** @brief The smallest commission on any execution. */
  Decimal64 minimum_{Decimal64::parse("0")};
  /**
   * @brief Whether the rate came from a broker's published schedule; `commission_model_from` sets it
   *   from the schedule's `origin_`.
   */
  bool from_broker_{false};

  /**
   * @brief Computes the commission on one execution.
   * @param quantity The quantity.
   * @param price The price.
   * @return `quantity * price * rate_` truncated to whole currency units, or `minimum_` if that is
   *   larger; `std::nullopt` if the arithmetic does not fit.
   */
  std::optional<Decimal64> on(const Decimal64& quantity, const Decimal64& price) const {
    const auto value = decimal_multiply(quantity, price);
    if (!value) {
      return std::nullopt;
    }
    auto charged = decimal_multiply(*value, rate_);
    if (!charged) {
      return std::nullopt;
    }
    // Truncated to whole currency units, as a broker charges.
    const auto whole = decimal_divide(*charged, Decimal64::parse("1"), 0);
    if (!whole) {
      return std::nullopt;
    }
    return decimal_less(*whole, minimum_) ? minimum_ : *whole;
  }

  /**
   * @brief Returns a model that charges nothing (the report says so).
   * @return The model.
   */
  static CommissionModel none() { return CommissionModel{}; }

  /**
   * @brief Returns a rate-based model.
   * @param rate The fraction of traded value charged.
   * @param minimum The smallest commission per execution.
   * @return The model.
   */
  static CommissionModel of(Decimal64 rate, Decimal64 minimum = Decimal64::parse("0")) {
    return CommissionModel{std::move(rate), std::move(minimum)};
  }
};

/**
 * @brief Builds a flat commission model from a schedule's rate for a market on a date.
 *
 * The model has no minimum (none is published) and no tax; it charges that one rate for the whole
 * replay, even if the schedule changes rate later. For per-date rates use
 * `CommissionPolicy::scheduled`.
 *
 * @param schedule The schedule, published by a broker or stated by the caller.
 * @param country The market.
 * @param date The date whose rate to use.
 * @return The model (`from_broker_` true only when the schedule's `origin_` is `BROKER_PUBLISHED`), or
 *   an `INVALID_ARGUMENT` error with broker code
 *   `no_rate_for_date` or `rate_bounds_unreadable` (see `CommissionSchedule::RateSelection`).
 */
inline Result<CommissionModel> commission_model_from(
  const CommissionSchedule& schedule,
  Country country,
  const Date& date
) {
  const auto selection = schedule.rate_for(country, date);
  if (!selection.rate_) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    // Nothing was asked of anybody: this reads a schedule already in hand.
    error.reached_broker_ = false;
    // Distinct codes: a missing row wants a rate stated; an unreadable window wants a refetch.
    if (selection.bounds_unreadable_) {
      error.broker_code_ = "rate_bounds_unreadable";
      error.message_ = "the published schedule holds a commission rate for that market whose date "
        "bounds could not be read, so whether it applies on that date is unknown";
      return Result<CommissionModel>(std::move(error));
    }
    error.broker_code_ = "no_rate_for_date";
    error.message_ = "the published schedule states no commission rate for that market and date";
    return Result<CommissionModel>(std::move(error));
  }
  const auto& rate = selection.rate_;
  auto model = CommissionModel::of(rate->rate_);
  // A caller's stated schedule stays the caller's: the report says where the rate it charged came from.
  model.from_broker_ = schedule.origin_ == CommissionOrigin::BROKER_PUBLISHED;
  return Result<CommissionModel>(std::move(model));
}

/** @brief The commission rate that charged an execution, as the backtest report lists it. */
struct AppliedRate {
  /** @brief The market the rate was selected for. */
  Country country_{Country::KR};
  /** @brief The venue the rate was keyed to. */
  VenueKey venue_;
  /** @brief The broker's commission, as a fraction of traded value. */
  Decimal64 rate_;
  /** @brief The institution fee rate; empty if the source gave one combined rate. */
  std::optional<Decimal64> institution_rate_;
  /** @brief The per-execution minimum on the broker's commission; zero if none. */
  Decimal64 minimum_{Decimal64::parse("0")};
  /** @brief The first date of the rate's window; empty if open-ended. */
  std::optional<Date> from_;
  /** @brief The last date of the rate's window; empty if open-ended. */
  std::optional<Date> until_;
  /** @brief Whether a broker published the rate or a caller stated it. */
  CommissionOrigin origin_{CommissionOrigin::CALLER_STATED};
  /**
   * @brief How many executions the rate charged, counted in the report's table.
   *
   * Zero on the copy attached to a single execution. Not compared by `operator==`.
   */
  std::size_t executions_{0};

  /**
   * @brief Compares every field except `executions_`.
   * @param lhs The first rate.
   * @param rhs The second rate.
   * @return `true` if they describe the same rate.
   */
  friend bool operator==(const AppliedRate& lhs, const AppliedRate& rhs) {
    return lhs.country_ == rhs.country_ && lhs.venue_ == rhs.venue_ && lhs.rate_ == rhs.rate_ &&
      lhs.institution_rate_ == rhs.institution_rate_ && lhs.minimum_ == rhs.minimum_ &&
      lhs.from_ == rhs.from_ && lhs.until_ == rhs.until_ && lhs.origin_ == rhs.origin_;
  }
};

/**
 * @brief What one execution was charged: the broker's commission and the institution fee.
 *
 * Each component is truncated to whole currency units separately. An empty `institution_` means
 * the rate was not split, so `broker_` includes everything.
 */
struct CommissionCharge {
  /** @brief The broker's commission, after its minimum. */
  Decimal64 broker_{Decimal64::parse("0")};
  /** @brief The institution fee (유관기관제비용), if the rate is split. */
  std::optional<Decimal64> institution_;
  /** @brief The rate that charged it. */
  AppliedRate applied_;
};

/**
 * @brief How a backtest charges commission: one stated model, or a schedule resolved per execution.
 *
 * `stated` charges one `CommissionModel` throughout. `scheduled` picks the rate for each execution
 * by its market, its local trading date and the venue the caller asserts (a replay cannot know
 * where an order executed, so the report lists the venue as an assumption). A rate that cannot be
 * resolved is an error, and the replay halts rather than skipping or zero-costing the fill.
 */
class CommissionPolicy {
public:
  /** @brief Constructs a policy that charges nothing (the report says so). */
  CommissionPolicy() = default;

  /**
   * @brief Makes a policy that charges one model on every execution.
   * @param model The model; its `from_broker_` is kept.
   * @return The policy.
   */
  static CommissionPolicy stated(CommissionModel model) {
    CommissionPolicy policy;
    policy.flat_ = std::move(model);
    return policy;
  }

  /**
   * @brief Makes a policy that picks a rate from a schedule for each execution.
   *
   * @param schedule The rates. A caller-stated schedule must name its `tariff_`.
   * @param asserted The venue fills are assumed to happen at; not `Venue::COMBINED`, which is
   *   routing rather than a venue.
   * @param dating_market The market whose local trading day the replay's dates are; a dated rate for
   *   another market is refused when charged.
   * @return The policy, or an `INVALID_ARGUMENT` error: `commission_tariff_unnamed` or
   *   `commission_venue_not_an_execution`.
   */
  static Result<CommissionPolicy> scheduled(
    CommissionSchedule schedule,
    VenueKey asserted,
    Country dating_market
  ) {
    if (schedule.origin_ == CommissionOrigin::CALLER_STATED && schedule.tariff_.empty()) {
      return Result<CommissionPolicy>(refusal(
        "commission_tariff_unnamed",
        "a caller-stated commission schedule must name the tariff it was read from, because the "
        "published table is channel by venue and nothing here can select the channel"
      ));
    }
    if (asserted.kind() == VenueKeyKind::NORMALIZED && asserted.venue() == Venue::COMBINED) {
      return Result<CommissionPolicy>(refusal(
        "commission_venue_not_an_execution",
        "COMBINED is consolidated routing rather than a venue an order matched on, so it cannot be "
        "asserted as the venue a fill happened at"
      ));
    }
    CommissionPolicy policy;
    policy.schedule_ = std::move(schedule);
    policy.asserted_ = std::move(asserted);
    policy.dating_market_ = dating_market;
    return Result<CommissionPolicy>(std::move(policy));
  }

  /**
   * @brief Tests whether the policy resolves a schedule.
   * @return `true` for a `scheduled` policy.
   */
  bool is_scheduled() const noexcept { return schedule_.has_value(); }

  /**
   * @brief Returns the asserted venue.
   * @return The venue a scheduled policy charges at; `VenueKey::any()` for a stated one.
   */
  const VenueKey& asserted_venue() const noexcept { return asserted_; }

  /**
   * @brief Returns the schedule's tariff name.
   * @return The name; empty for a stated policy.
   */
  std::string_view tariff() const noexcept {
    return schedule_ ? std::string_view(schedule_->tariff_) : std::string_view();
  }

  /**
   * @brief Computes the commission on one execution.
   *
   * A scheduled policy charges no minimum (a schedule states none).
   *
   * @param quantity The quantity.
   * @param price The price.
   * @param date The market-local trading date of the execution (see `SessionWindow::local_date`).
   * @param country The instrument's market.
   * @return The charge, or an `INVALID_ARGUMENT` error: `commission_not_representable`,
   *   `commission_date_not_market_local` (a dated rate for a market other than `dating_market`),
   *   `rate_bounds_unreadable` or `no_rate_for_date`.
   */
  Result<CommissionCharge> charge(
    const Decimal64& quantity,
    const Decimal64& price,
    const Date& date,
    Country country
  ) const {
    if (!schedule_) {
      const auto charged = flat_.on(quantity, price);
      if (!charged) {
        return Result<CommissionCharge>(refusal(
          "commission_not_representable",
          "the commission on that execution does not fit an exact decimal"
        ));
      }
      CommissionCharge result;
      result.broker_ = *charged;
      // A flat model is one inclusive figure: no institution split.
      result.applied_.country_ = country;
      result.applied_.rate_ = flat_.rate_;
      result.applied_.minimum_ = flat_.minimum_;
      result.applied_.origin_ = flat_.from_broker_ ?
        CommissionOrigin::BROKER_PUBLISHED :
        CommissionOrigin::CALLER_STATED;
      return Result<CommissionCharge>(std::move(result));
    }

    // A market other than the dating market is refused if any of its rates is dated: the replay's
    // date is another market's day, so which dated rate applies cannot be decided.
    if (country != dating_market_ && schedule_->any_dated_rate_for(country)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "commission_date_not_market_local";
      error.message_ = "this market's rates have dated bounds, and the replay's date " +
        named(date) + " is that of another market's trading day, so which rate applies cannot be "
        "decided here; price it in a replay whose session is that market's, or state rates with "
        "no dated bounds";
      return Result<CommissionCharge>(std::move(error));
    }

    const auto selection = schedule_->rate_for(country, asserted_, date);
    const auto& rate = selection.rate_;
    if (!rate) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      // Name the date, and distinguish an unreadable window (refetch) from a missing rate (state one).
      if (selection.bounds_unreadable_) {
        error.broker_code_ = "rate_bounds_unreadable";
        error.message_ = "the schedule holds a commission rate for " + described(asserted_) +
          " in this instrument's market whose date bounds could not be read, so whether it applies "
          "on " + named(date) + " is unknown; refetch the schedule or state the rates";
        return Result<CommissionCharge>(std::move(error));
      }
      error.broker_code_ = "no_rate_for_date";
      error.message_ = "the schedule states no commission rate for " + named(date) +
        " at venue " + described(asserted_) + " in the market this instrument is listed in";
      return Result<CommissionCharge>(std::move(error));
    }

    // No minimum on a scheduled rate: no schedule states one. A caller with a minimum uses `stated`.
    const auto broker = component(quantity, price, rate->rate_, Decimal64::parse("0"));
    if (!broker) {
      return Result<CommissionCharge>(refusal(
        "commission_not_representable",
        "the commission on that execution does not fit an exact decimal"
      ));
    }
    CommissionCharge result;
    result.broker_ = *broker;
    if (rate->institution_rate_) {
      // Its own component, with no minimum (the minimum is the broker's).
      const auto institution =
        component(quantity, price, *rate->institution_rate_, Decimal64::parse("0"));
      if (!institution) {
        return Result<CommissionCharge>(refusal(
          "commission_not_representable",
          "the institution fee on that execution does not fit an exact decimal"
        ));
      }
      result.institution_ = *institution;
    }
    result.applied_.country_ = country;
    result.applied_.venue_ = rate->venue_;
    result.applied_.rate_ = rate->rate_;
    result.applied_.institution_rate_ = rate->institution_rate_;
    result.applied_.from_ = rate->from_;
    result.applied_.until_ = rate->until_;
    result.applied_.origin_ = schedule_->origin_;
    return Result<CommissionCharge>(std::move(result));
  }

private:
  // The venue a refusal was asked about, as the asker spelled it.
  static std::string described(const VenueKey& venue) {
    switch (venue.kind()) {
      case VenueKeyKind::ANY:
        return "(none asserted)";
      case VenueKeyKind::NORMALIZED:
        return std::string(venue_name(venue.venue()));
      case VenueKeyKind::RAW:
        return "'" + venue.raw_code().str() + "'";
    }
    return "(unknown)";
  }

  // `date` as `YYYY-MM-DD`.
  static std::string named(const Date& date) {
    const auto pad = [](unsigned value) {
      return value < 10 ? "0" + std::to_string(value) : std::to_string(value);
    };
    return std::to_string(date.year_) + "-" + pad(date.month_) + "-" + pad(date.day_);
  }

  static BrokerError refusal(std::string code, std::string message) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    // Nothing was asked of anybody: this reads a schedule already in hand.
    error.reached_broker_ = false;
    error.broker_code_ = std::move(code);
    error.message_ = std::move(message);
    return error;
  }

  // One component's charge, truncated to whole units and floored at `minimum`.
  static std::optional<Decimal64> component(
    const Decimal64& quantity,
    const Decimal64& price,
    const Decimal64& rate,
    const Decimal64& minimum
  ) {
    CommissionModel model;
    model.rate_ = rate;
    model.minimum_ = minimum;
    return model.on(quantity, price);
  }

  // The stated model; unused by a scheduled policy.
  CommissionModel flat_{};
  // Present exactly when this policy resolves a schedule.
  std::optional<CommissionSchedule> schedule_;
  // The venue a caller asserted its fills happened at.
  VenueKey asserted_;
  // The market whose local trading day `charge`'s date is; dated rates of other markets refuse.
  Country dating_market_{Country::KR};
};

/**
 * @brief Slippage: how far a fill price moves against the order (a buy pays more, a sell gets less).
 *
 * `basis_points_` is a fraction of the price in ten-thousandths; `ticks_` walks that many steps along
 * the instrument's grid, one at a time, so a walk that crosses into another band takes that band's
 * tick from there on. Both apply when both are set: the ticks are walked from the price before
 * slippage, and the two amounts added.
 */
struct SlippageModel {
  /** @brief The price given up per execution, in ten-thousandths of the price. */
  Decimal64 basis_points_{Decimal64::parse("0")};
  /** @brief Grid steps given up per execution, each a step to the next grid price against the order. */
  int ticks_{0};

  /**
   * @brief Moves a price against the order.
   *
   * @param price The price before slippage.
   * @param side The order's side.
   * @param rule The instrument's grid. A buy steps up by the tick at each price; a sell steps down by
   *   the tick of the band below each price. From a price on the KRX grid each step reaches the next
   *   grid price; from a price off the grid, or with a custom table whose band starts are not multiples
   *   of both ticks, it may not (as with `PriceRule::round_to_tick`). A step the grid has no tick for (an
   *   unknown grid, a sell at the first band's start) or that would end at or below zero is not taken.
   * @return The slipped price and the amount given up, or `std::nullopt` if the arithmetic does
   *   not fit.
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
      // One step at a time, each with the tick where it starts: the first tick of a band is not the
      // last tick of the band below.
      const auto zero = Decimal64::parse("0");
      auto walked = price;
      for (int step = 0; step < ticks_; ++step) {
        const auto tick = side == OrderSide::BUY ?
          rule.tick_at(walked) :
          tick_below(rule, walked);
        if (!tick) {
          // An unknown grid, or the bottom of a known one, contributes nothing rather than a guessed step.
          break;
        }
        const auto next = side == OrderSide::BUY ?
          decimal_add(walked, *tick) :
          decimal_subtract(walked, *tick);
        if (!next) {
          return std::nullopt;
        }
        if (!decimal_less(zero, *next)) {
          // No price at or below zero.
          break;
        }
        walked = *next;
      }
      const auto steps = side == OrderSide::BUY ?
        decimal_subtract(walked, price) :
        decimal_subtract(price, walked);
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

    const auto given_up = side == OrderSide::BUY ?
      decimal_subtract(moved, price) :
      decimal_subtract(price, moved);
    if (!given_up) {
      return std::nullopt;
    }
    return std::pair<Decimal64, Decimal64>{moved, *given_up};
  }

  /**
   * @brief Returns a model with no slippage (the report says so).
   * @return The model.
   */
  static SlippageModel none() { return SlippageModel{}; }

  /**
   * @brief Returns a basis-point slippage model.
   * @param points The ten-thousandths of the price given up.
   * @return The model.
   */
  static SlippageModel of_basis_points(Decimal64 points) {
    SlippageModel model;
    model.basis_points_ = std::move(points);
    return model;
  }

  /**
   * @brief Returns a tick slippage model.
   * @param count The ticks given up; zero or less applies none.
   * @return The model.
   */
  static SlippageModel of_ticks(int count) {
    SlippageModel model;
    model.ticks_ = count;
    return model;
  }

private:
  // The tick of the band below a price: the step down to the next lower grid price, for a price on a
  // grid like KRX's. None at or below the first band's start, or without bands.
  static std::optional<Decimal64> tick_below(const PriceRule& rule, const Decimal64& price) {
    std::optional<Decimal64> tick;
    for (const auto& band : rule.bands_) {
      if (!decimal_less(band.from_, price)) {
        break;
      }
      tick = band.tick_;
    }
    return tick;
  }
};

/** @brief Which price a backtest credits a limit order that fills. */
enum class LimitFillPrice {
  /**
   * @brief The default: fill at the limit, or at the bar's worst traded price when the whole bar
   *   was better than the limit.
   *
   * A bar that gaps through a resting limit is not credited its open. A limit marketable through
   * the whole bar (a buy above the high) is credited the high (a sell below the low, the low), which
   * is better than the limit; `ReportCaveats::limit_fills_bounded_` counts those fills.
   */
  AT_LIMIT,
  /**
   * @brief Fill at the bar's open when it opened through the limit (assumes the order was resting
   *   before the open).
   */
  AT_OPEN_IF_BETTER,
};

/**
 * @brief Returns a lowercase name for a choice, such as `"at_limit"`, for reports.
 * @param choice The choice.
 * @return The name.
 */
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
