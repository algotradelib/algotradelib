#pragma once

#include "../strategy/event.hpp"
#include "../strategy/portfolio.hpp"

#include <algotradelib/core>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * The positions and cash a replay maintains for itself.
 *
 * **The piece that does not exist anywhere else.** In a dry run or a live session the portfolio comes
 * from a broker or from the caller; in a replay nothing else can produce it, so the engine has to. Get
 * this wrong and every target-stating strategy repeats its target forever, because the position never
 * moves — which would look like a strategy that does nothing rather than an engine that does nothing.
 *
 * Exact decimals throughout. A backtest's job is to produce a number somebody will act on, and a
 * position accumulated in floating point drifts in the third decimal after a few thousand fills. The
 * analytics layer converts at its own documented boundary; this is not that boundary.
 */

/**
 * Compile-time identity of a replay, standing where a broker's tag would.
 *
 * A replay issues its own order references and they reach no broker, but they still travel through
 * `OrderRef<Tag>` so that the portfolio a strategy reads has exactly the type it has live. Giving a
 * backtest its own tag rather than borrowing `runner::DryTag` keeps the two distinguishable in a log:
 * a dry run read real reference data and a replay did not.
 */
struct BacktestTag {
  static constexpr std::string_view name = "backtest";
};

/** One execution the engine produced, and what it cost. */
struct Execution {
  strategy::Fill fill_;
  /** Commission charged, in the instrument's currency. */
  Decimal64 commission_;
  /**
   * Difference between the price the order asked for and the price it got.
   *
   * Recorded rather than folded into the price, so a report can say how much of a result came from the
   * model's assumptions rather than from the strategy.
   */
  Decimal64 slippage_;
};

/**
 * Positions, cash, and realized profit, advanced by the executions a replay produces.
 *
 * Cash is tracked per currency with no implicit conversion, matching `Portfolio` and for the same
 * reason: a total spanning currencies needs an exchange rate this library does not carry, and
 * inventing one would misprice every figure derived from it.
 */
class ReplayLedger {
public:
  /** Constructs a ledger holding `cash` of `currency` and nothing else. */
  ReplayLedger(Decimal64 cash, Currency currency) {
    cash_.push_back(Money{std::move(cash), std::move(currency)});
  }

  /** Returns the cash held in `currency`, or nothing when none is held. */
  std::optional<Decimal64> cash_in(const Currency& currency) const {
    for (const auto& amount : cash_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  }

  /** Returns the quantity held of `instrument`, or nothing when there is no position. */
  std::optional<Decimal64> quantity_of(const InstrumentId& instrument) const {
    for (const auto& lot : positions_) {
      if (lot.instrument_ == instrument) {
        return lot.quantity_;
      }
    }
    return std::nullopt;
  }

  /** Returns the average price paid for the position in `instrument`, or nothing when flat. */
  std::optional<Decimal64> average_price_of(const InstrumentId& instrument) const {
    for (const auto& lot : positions_) {
      if (lot.instrument_ == instrument && lot.quantity_.coefficient() != 0) {
        return lot.average_purchase_price_;
      }
    }
    return std::nullopt;
  }

  /** Returns realized profit in `currency`, which is what closing a position banked. */
  Decimal64 realized_in(const Currency& currency) const {
    for (const auto& entry : realized_) {
      if (entry.currency_ == currency) {
        return entry.amount_;
      }
    }
    return Decimal64::parse("0");
  }

  /** Returns every position, including any that have gone flat. */
  const std::vector<HoldingPosition>& positions() const noexcept { return positions_; }

  /** Returns every execution applied, oldest first. */
  const std::vector<Execution>& executions() const noexcept { return executions_; }

  /**
   * Applies one execution, moving the position, the cash, and realized profit.
   *
   * The arithmetic that matters, and the case that catches a wrong implementation: a position crossing
   * through flat. Selling 150 from a long of 100 is not "a short of 50 at the sale price" — it closes
   * 100, realizing profit against the old average, and opens 50 short at the new price. Treating it as
   * one weighted average would carry the closed leg's cost basis into the new position and misstate
   * every later figure.
   *
   * Returns false when the arithmetic will not fit an exact decimal, which leaves the ledger unchanged
   * rather than partly applied.
   */
  bool apply(const Execution& execution, const Currency& currency) {
    const auto& fill = execution.fill_;
    const auto zero = Decimal64::parse("0");

    // Signed quantity: a sale reduces the position.
    const auto signed_quantity = fill.side_ == OrderSide::BUY ?
      std::optional<Decimal64>(fill.quantity_) :
      decimal_subtract(zero, fill.quantity_);
    if (!signed_quantity) {
      return false;
    }

    const auto gross = decimal_multiply(fill.quantity_, fill.price_);
    if (!gross) {
      return false;
    }
    // Cash out on a buy, in on a sale, and commission out either way.
    const auto cash_change = fill.side_ == OrderSide::BUY ?
      decimal_subtract(zero, *gross) :
      std::optional<Decimal64>(*gross);
    if (!cash_change) {
      return false;
    }
    const auto after_commission = decimal_subtract(*cash_change, execution.commission_);
    if (!after_commission) {
      return false;
    }

    auto* lot = find_or_open(fill.instrument_);
    const auto held = lot->quantity_;
    const auto closing = closed_quantity(held, *signed_quantity);
    if (!closing) {
      return false;
    }

    // Realized profit exists only on the part that closed, and only against the old average.
    if (closing->coefficient() != 0) {
      const auto moved = decimal_subtract(fill.price_, lot->average_purchase_price_);
      if (!moved) {
        return false;
      }
      auto banked = decimal_multiply(*closing, *moved);
      if (!banked) {
        return false;
      }
      // Closing a short banks the opposite sign: it profits when the price fell.
      if (decimal_less(held, zero)) {
        banked = decimal_subtract(zero, *banked);
        if (!banked) {
          return false;
        }
      }
      if (!add_realized(currency, *banked)) {
        return false;
      }
    }

    const auto resulting = decimal_add(held, *signed_quantity);
    if (!resulting) {
      return false;
    }

    // The average price follows only the part that opened or added. A position crossing flat takes the
    // fill price outright, because nothing of the old basis survives.
    if (resulting->coefficient() == 0) {
      lot->average_purchase_price_ = zero;
    } else if (crossed_or_opened(held, *resulting)) {
      lot->average_purchase_price_ = fill.price_;
    } else if (adding(held, *signed_quantity)) {
      const auto blended = weighted_average(held, lot->average_purchase_price_, *signed_quantity, fill.price_);
      if (!blended) {
        return false;
      }
      lot->average_purchase_price_ = *blended;
    }
    // Reducing without crossing leaves the average alone: selling half a position does not change
    // what the remaining half cost.

    lot->quantity_ = *resulting;
    lot->last_price_ = fill.price_;
    // **The currency is recorded here because this is the only place it is known.** `mark` is not
    // told one and neither is `equity_in`'s caller beyond the currency it is asking about, so a
    // position that never wrote its own currency down cannot be told apart from one denominated in
    // whatever the caller happened to ask for. `Currency` default-constructs to KRW
    // (`core/types.hpp:154`), which makes the untold case silently *answer* rather than be absent --
    // a USD position would read as Korean won to anything summing per currency.
    if (!revalue(*lot, currency)) {
      return false;
    }
    if (!adjust_cash(currency, *after_commission)) {
      return false;
    }
    executions_.push_back(execution);
    return true;
  }

  /** Marks every position to `price` for `instrument`, so market value reflects the latest bar. */
  void mark(const InstrumentId& instrument, const Decimal64& price) {
    for (auto& lot : positions_) {
      if (lot.instrument_ == instrument) {
        lot.last_price_ = price;
        // The currency is the one `apply` recorded; a lot only exists because a fill opened it, so
        // there is always one. Re-valuing here is what makes `market_value_` mean "at the latest
        // mark" rather than "at the last fill".
        revalue(lot, lot.market_value_.currency_);
      }
    }
  }

  /**
   * Returns the portfolio to present to a strategy.
   *
   * Working orders are deliberately empty: a replay fills or discards an order within one bar, so
   * nothing rests. A future engine modelling orders that live across bars would fill this in, and
   * `reconcile` would then do the same projected-position arithmetic it does live.
   */
  template <typename BrokerTag>
  strategy::Portfolio<BrokerTag> snapshot(
    std::chrono::system_clock::time_point as_of
  ) const {
    strategy::Portfolio<BrokerTag> portfolio;
    portfolio.positions_ = positions_;
    // **Every held share is reported as sellable, and that is a model rather than a fact.** The
    // field means "what the broker says may be sold", and a replay has no broker; leaving it absent
    // instead would be read as "the broker did not say", which makes a guard checking it refuse
    // every sale a replay ever plans.
    //
    // The whole holding is the answer consistent with the rest of this ledger. `apply` credits a
    // sale to cash on the fill, so a replay already models a world with no settlement schedule, and
    // claiming T+0 cash beside T+2 shares would be two different worlds in one snapshot. The
    // assumption is disclosed on the report rather than only here -- `ReportCaveats` --
    // because a reader who never opens this file still has to know.
    for (auto& position : portfolio.positions_) {
      position.sellable_quantity_ = position.quantity_;
    }
    portfolio.cash_ = cash_;
    // **The broker's answer, modelled, because a replay has no broker to ask.** A buying-power check
    // reads this field and refuses when it is absent, so leaving it empty would refuse every buy a
    // replay plans -- the same trap the sellable field was in.
    //
    // The figure is this ledger's own cash, which is not what a broker would have said: the ledger
    // credits a sale on the fill and models no settlement, while a broker prices buying power on its
    // own terms, including or excluding amounts nothing here models. Which way a given account
    // differs is not something this can know, so the report states *the model used* rather than a
    // direction. Stamped with the snapshot time, which is when the modelled figure was read and is
    // not a broker's response time.
    portfolio.buying_power_.reserve(cash_.size());
    for (const auto& amount : cash_) {
      portfolio.buying_power_.push_back(BuyingPower{amount, as_of});
    }
    portfolio.as_of_ = as_of;
    return portfolio;
  }

  /**
   * Returns total equity in `currency`: cash plus every position at its last mark.
   *
   * Only positions denominated in `currency` contribute. A holding in another currency is omitted
   * rather than converted, and the omission is why a caller replaying a multi-currency portfolio needs
   * the exchange rates this library does not yet carry.
   */
  std::optional<Decimal64> equity_in(const Currency& currency) const {
    auto total = cash_in(currency).value_or(Decimal64::parse("0"));
    for (const auto& lot : positions_) {
      if (lot.quantity_.coefficient() == 0) {
        continue;
      }
      // **The filter the documentation above always claimed and the code never did.** Every
      // position used to contribute whatever it was denominated in, so a KRW holding was counted
      // into `equity_in(USD)` -- an implicit conversion at a rate of one, which is the single thing
      // `holdings-core` refuses to do. `Backtester` feeds this into the equity curve
      // (`backtest/engine.hpp`), so every derived metric of a multi-currency replay inherited it.
      //
      // The currency comes from `market_value_`, written by `apply`. It is a value this ledger
      // recorded rather than a default: `Currency()` is KRW, so trusting an unwritten field would
      // reinstate the same silent inclusion under a different name.
      if (!(lot.market_value_.currency_ == currency)) {
        continue;
      }
      const auto sum = decimal_add(total, lot.market_value_.amount_);
      if (!sum) {
        return std::nullopt;
      }
      total = *sum;
    }
    return total;
  }

private:
  /**
   * Writes `lot`'s market value from its quantity and latest mark, in `currency`.
   *
   * **Only `market_value_` is filled**, and the narrowness is deliberate. It is the one field
   * per-currency arithmetic needs -- it is the only place a `HoldingPosition` states which currency
   * it is denominated in -- and it is exact: quantity times the latest mark, which this ledger
   * already computes. `purchase_value_`, `unrealized_profit_loss_` and `unrealized_return_` stay as
   * they are; the first two are derivable but nothing reads them, and the third is a ratio whose
   * scale nobody has chosen. Filling a field this ledger has no answer for would be the
   * `ReportCaveats` problem one layer down.
   *
   * Returns false when the multiplication will not fit an exact decimal, which `apply` turns into a
   * refusal rather than a position valued at a number it could not compute.
   */
  static bool revalue(HoldingPosition& lot, const Currency& currency) {
    const auto value = decimal_multiply(lot.quantity_, lot.last_price_);
    if (!value) {
      return false;
    }
    lot.market_value_ = Money{*value, currency};
    return true;
  }

  HoldingPosition* find_or_open(const InstrumentId& instrument) {
    for (auto& lot : positions_) {
      if (lot.instrument_ == instrument) {
        return &lot;
      }
    }
    HoldingPosition opened;
    opened.instrument_ = instrument;
    opened.quantity_ = Decimal64::parse("0");
    opened.average_purchase_price_ = Decimal64::parse("0");
    opened.last_price_ = Decimal64::parse("0");
    positions_.push_back(opened);
    return &positions_.back();
  }

  /** Returns how much of `held` this trade closes, which is zero when it only adds. */
  static std::optional<Decimal64> closed_quantity(
    const Decimal64& held,
    const Decimal64& traded
  ) {
    const auto zero = Decimal64::parse("0");
    const bool long_held = decimal_less(zero, held);
    const bool short_held = decimal_less(held, zero);
    const bool buying = decimal_less(zero, traded);
    if ((long_held && buying) || (short_held && !buying) || held.coefficient() == 0) {
      return zero;
    }
    const auto magnitude_held = long_held ? held : decimal_subtract(zero, held);
    const auto magnitude_traded = buying ? traded : decimal_subtract(zero, traded);
    if (!magnitude_held || !magnitude_traded) {
      return std::nullopt;
    }
    return decimal_less(*magnitude_held, *magnitude_traded) ? *magnitude_held : *magnitude_traded;
  }

  /** Returns whether the trade opened from flat or crossed through it. */
  static bool crossed_or_opened(const Decimal64& held, const Decimal64& resulting) {
    const auto zero = Decimal64::parse("0");
    if (held.coefficient() == 0) {
      return true;
    }
    return decimal_less(zero, held) != decimal_less(zero, resulting);
  }

  /** Returns whether the trade added to a position rather than reducing it. */
  static bool adding(const Decimal64& held, const Decimal64& traded) {
    const auto zero = Decimal64::parse("0");
    return decimal_less(zero, held) == decimal_less(zero, traded);
  }

  /** Returns the average of two lots by magnitude, which is what adding to a position produces. */
  static std::optional<Decimal64> weighted_average(
    const Decimal64& held,
    const Decimal64& held_price,
    const Decimal64& traded,
    const Decimal64& traded_price
  ) {
    const auto zero = Decimal64::parse("0");
    const auto held_magnitude = decimal_less(held, zero) ? decimal_subtract(zero, held) : held;
    const auto traded_magnitude =
      decimal_less(traded, zero) ? decimal_subtract(zero, traded) : traded;
    if (!held_magnitude || !traded_magnitude) {
      return std::nullopt;
    }
    const auto held_cost = decimal_multiply(*held_magnitude, held_price);
    const auto traded_cost = decimal_multiply(*traded_magnitude, traded_price);
    if (!held_cost || !traded_cost) {
      return std::nullopt;
    }
    const auto total_cost = decimal_add(*held_cost, *traded_cost);
    const auto total_quantity = decimal_add(*held_magnitude, *traded_magnitude);
    if (!total_cost || !total_quantity || total_quantity->coefficient() == 0) {
      return std::nullopt;
    }
    // Four places, which holds a tick on every grid this library knows and more than a price needs.
    return decimal_divide(*total_cost, *total_quantity, 4);
  }

  bool adjust_cash(const Currency& currency, const Decimal64& change) {
    for (auto& amount : cash_) {
      if (amount.currency_ == currency) {
        const auto updated = decimal_add(amount.amount_, change);
        if (!updated) {
          return false;
        }
        amount.amount_ = *updated;
        return true;
      }
    }
    cash_.push_back(Money{change, currency});
    return true;
  }

  bool add_realized(const Currency& currency, const Decimal64& amount) {
    for (auto& entry : realized_) {
      if (entry.currency_ == currency) {
        const auto updated = decimal_add(entry.amount_, amount);
        if (!updated) {
          return false;
        }
        entry.amount_ = *updated;
        return true;
      }
    }
    realized_.push_back(Money{amount, currency});
    return true;
  }

  std::vector<HoldingPosition> positions_;
  std::vector<Money> cash_;
  std::vector<Money> realized_;
  std::vector<Execution> executions_;
};

} // namespace backtest
} // namespace algotradelib
