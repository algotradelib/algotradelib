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
 * Which rate charged an execution, carried so a report can state rates *used* rather than rates held.
 *
 * A report that listed the rows a schedule contains would be describing what was available, not what
 * was charged — and the two differ whenever a schedule spans more dates or more venues than the
 * replay traded. Reconstructing it afterwards from the amounts is not possible either: two rows can
 * charge the same figure on one fill. So the row travels with the charge, onto the execution, and the
 * report accumulates it at the point the ledger accepts the fill.
 */
struct AppliedRate {
  /**
   * The market the rate was selected for.
   *
   * Carried because two markets can publish identical venue-neutral rates, and a table that
   * collapsed them into one row would say a single rate charged both when two rows did.
   */
  Country country_{Country::KR};
  /** Which venue the rate was keyed to. */
  VenueKey venue_;
  /** The broker's commission component, as a fraction of traded value. */
  Decimal64 rate_;
  /** The institution component, empty when the source said nothing about a split. */
  std::optional<Decimal64> institution_rate_;
  /** The per-order floor applied to the broker component, zero when none. */
  Decimal64 minimum_{Decimal64::parse("0")};
  /** The window the rate applies from, empty when open-ended. */
  std::optional<Date> from_;
  /** The window the rate applies until, empty when open-ended. */
  std::optional<Date> until_;
  /** Whether a broker published the number or a caller stated it. */
  CommissionOrigin origin_{CommissionOrigin::CALLER_STATED};
  /**
   * How many executions this rate charged, filled in by `summarize` rather than by the fill model.
   *
   * Zero on the copy that travels with one execution -- that copy describes one charge -- and
   * counted up on the report's collapsed table. It is deliberately outside `operator==`, so two
   * charges by the same rate compare equal and collapse.
   */
  std::size_t executions_{0};

  /** Compares every field **but the count**, so a report can collapse identical rates into one row. */
  friend bool operator==(const AppliedRate& lhs, const AppliedRate& rhs) {
    return lhs.country_ == rhs.country_ && lhs.venue_ == rhs.venue_ && lhs.rate_ == rhs.rate_ &&
      lhs.institution_rate_ == rhs.institution_rate_ && lhs.minimum_ == rhs.minimum_ &&
      lhs.from_ == rhs.from_ && lhs.until_ == rhs.until_ && lhs.origin_ == rhs.origin_;
  }
};

/**
 * What one execution was charged, by component.
 *
 * The two components are charged by **different parties** — a broker's commission and Korea's
 * 유관기관제비용, billed by the exchange and the depository — so they are rounded separately, each to
 * whole currency units, rather than summed and rounded once. Rounding the sum would be arithmetic
 * about a single charge that nobody makes.
 *
 * `institution_` is empty when the rate said nothing about a split, which is every rate Toss
 * publishes. Empty is not zero: it means `broker_` is an **inclusive** figure, and a report says so
 * rather than claiming a known zero for a component nobody described.
 */
struct CommissionCharge {
  /** The broker's commission on this execution, after its floor. */
  Decimal64 broker_{Decimal64::parse("0")};
  /** The institution component, empty when the rate did not distinguish one. */
  std::optional<Decimal64> institution_;
  /** Which rate charged it. */
  AppliedRate applied_;
};

/**
 * What a trade costs, resolved per execution rather than frozen at construction.
 *
 * Two origins, one type. A caller who states their own number gets `stated`; a caller who has a
 * schedule — published by a broker or read off a fee page for their own account — gets `scheduled`,
 * and the rate is chosen **per execution**, against the date that execution happened on and the venue
 * the caller asserted. That is what lets a replay spanning a rate change charge each rate on its own
 * dates instead of one rate throughout.
 *
 * **The venue is asserted once, here, and never inferred.** A replay has no execution venue to offer:
 * a bar does not say where it traded, `PlannedOrder::venue_` is routing *intent*, and KIS's own
 * response fields for the venue an order reached are undocumented — see
 * `issues/venue-aware-costs-kis-venue-attribution.md`. So the venue a cost is computed at is something
 * a caller states and the report discloses as an assumption, which is the same shape as
 * `ReportCaveats::bar_tape_unverified`.
 *
 * **A rate that cannot be resolved is a refusal, not a zero and not a missing fill.** `charge` returns
 * `Result`, and the replay turns a refusal into a halt: a fill that silently did not happen would
 * change the path the strategy takes and every performance figure derived from it, which is the most
 * flattering wrong answer a backtester can give.
 */
class CommissionPolicy {
public:
  /** Constructs a policy charging nothing, which a report will say it applied. */
  CommissionPolicy() = default;

  /**
   * Returns a policy charging what `model` states, on every execution and every date.
   *
   * **It preserves `model.from_broker_`** rather than labelling every flat model caller-stated: a
   * model built by `commission_model_from` came from a broker's published schedule and has to keep
   * saying so wherever it is carried.
   */
  static CommissionPolicy stated(CommissionModel model) {
    CommissionPolicy policy;
    policy.flat_ = std::move(model);
    return policy;
  }

  /**
   * Returns a policy resolving `schedule` per execution, or an error the caller cannot recover from.
   *
   * Two refusals, both before any costing happens:
   *
   * - **A caller-stated schedule with no `tariff_`.** The published fee table is channel by venue and
   *   the channel is a property of the account that no API states — 뱅키스 against a branch-opened
   *   account is an order of magnitude. Nothing here can select it, so a schedule that does not name
   *   the row it was read from is refused rather than defaulted.
   * - **`Venue::COMBINED` as the asserted venue.** It is what KIS's `SOR` normalizes to: consolidated
   *   *routing*, not a venue an order matched on. A rate row may be keyed to it, because a broker
   *   could publish a consolidated rate; asserting it as the place a fill happened is a guess.
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

  /** Returns whether this policy resolves a schedule rather than charging one stated number. */
  bool is_scheduled() const noexcept { return schedule_.has_value(); }

  /** Returns the venue a scheduled policy costs at; neutral on a stated one. */
  const VenueKey& asserted_venue() const noexcept { return asserted_; }

  /** Returns the tariff the schedule named, empty on a stated model. */
  std::string_view tariff() const noexcept {
    return schedule_ ? std::string_view(schedule_->tariff_) : std::string_view();
  }

  /**
   * Returns what `quantity` at `price` costs on `date`, or why it cannot be costed.
   *
   * `date` is the **market-local** calendar day the execution happened on, which is what a published
   * rate window is written in; `SessionWindow::local_date` converts the replay clock's instant. A bar
   * either side of UTC midnight is the case that distinguishes the two.
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
      // A flat model states one number and says nothing about its composition, which is the same
      // answer an empty `institution_rate_` gives: the figure is inclusive, not split.
      result.applied_.country_ = country;
      result.applied_.rate_ = flat_.rate_;
      result.applied_.minimum_ = flat_.minimum_;
      result.applied_.origin_ = flat_.from_broker_ ?
        CommissionOrigin::BROKER_PUBLISHED :
        CommissionOrigin::CALLER_STATED;
      return Result<CommissionCharge>(std::move(result));
    }

    const auto rate = schedule_->rate_for(country, asserted_, date);
    if (rate && country != dating_market_ && (rate->from_ || rate->until_)) {
      // **The date belongs to another market's calendar.** A replay derives one local date, from
      // its single `SessionWindow`, so a fill in a market nine hours away can be stamped with the
      // next day -- and a rate window that changes at that boundary would then charge the wrong
      // side of it. Refused rather than charged, and only where it could matter: an undated row is
      // the same rate whichever calendar named the day, so a foreign listing priced by an
      // open-ended row is unaffected.
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "commission_date_not_market_local";
      error.message_ = "the rate for this market has dated bounds, and the replay's date " +
        named(date) + " is that of another market's trading day; price it in a replay whose "
        "session is that market's, or state an open-ended rate";
      return Result<CommissionCharge>(std::move(error));
    }
    if (!rate) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "no_rate_for_date";
      // The date is named rather than described: a refusal a reader has to reproduce to understand
      // is a refusal they will work around instead of fixing.
      error.message_ = "the schedule states no commission rate for " + named(date) +
        " at venue " + described(asserted_) + " in the market this instrument is listed in";
      return Result<CommissionCharge>(std::move(error));
    }

    // **No floor on a scheduled rate, and that is not an oversight.** A per-order minimum is a term
    // of a tariff, and neither a broker's published schedule nor the fee page a caller reads states
    // one -- so a floor here would be a number this library invented. A caller who has one states
    // their whole cost through `stated`, where the floor is theirs to set.
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
      // Rounded on its own component and with no floor: the floor is a broker's per-order minimum,
      // and this charge is not the broker's.
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
  /** Returns the venue a refusal was asked about, in the terms the asker used. */
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

  /** Returns `date` as `YYYY-MM-DD`, for a refusal that names what it refused. */
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

  /** Returns one component's charge, rounded to whole currency units and floored at `minimum`. */
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

  /** The stated model. Unused by a scheduled policy, which charges what the schedule says. */
  CommissionModel flat_{};
  /** Present exactly when this policy resolves a schedule. */
  std::optional<CommissionSchedule> schedule_;
  /** The venue a caller asserted its fills happened at. */
  VenueKey asserted_;
  /**
   * The market whose local trading day the `date` handed to `charge` is in.
   *
   * A replay has one `SessionWindow` and therefore one local calendar, so this is the caller's
   * statement of which market's day that is. A dated rate for any other market is refused rather
   * than charged against a day computed in the wrong place.
   */
  Country dating_market_{Country::KR};
};

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
