#pragma once

#include <algotradelib/core>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * Returns whether a status describes an order that can still trade.
 *
 * Listed explicitly rather than as "not terminal", so a status this library gains later does not
 * silently start counting as live. `UNKNOWN` counts as open: an order whose state is unrecognized
 * may still be working, and the two consequences of being wrong are not symmetric. Under-counting
 * a working quantity causes double-sizing; over-counting merely holds an order back. Treating an
 * unrecognized state as closed would also mean never trying to cancel it.
 */
inline bool order_is_open(OrderStatus status) {
  switch (status) {
    case OrderStatus::UNKNOWN:
    case OrderStatus::PENDING:
    case OrderStatus::PENDING_CANCEL:
    case OrderStatus::PENDING_REPLACE:
    case OrderStatus::PARTIAL_FILLED:
      return true;
    case OrderStatus::FILLED:
    case OrderStatus::CANCELED:
    case OrderStatus::REJECTED:
    case OrderStatus::CANCEL_REJECTED:
    case OrderStatus::REPLACE_REJECTED:
    case OrderStatus::REPLACED:
      return false;
  }
  return true;
}

/**
 * What a strategy is currently holding, and what it has already asked for.
 *
 * Templated on the broker because a working order carries an `OrderRef<Broker>`, which is only
 * meaningful to the broker that issued it. A strategy never spells the parameter: it reads
 * `ctx.portfolio()` with `auto` and names no broker.
 *
 * **`working_` is the half most easily forgotten, and the reason it is here.** A strategy that
 * sizes from positions alone double-sizes after a partial fill: the signal still says "hold 100",
 * 40 are filled and 60 are still working, and sizing against the position asks for 60 more on top
 * of the 60 already in the market. `working_quantity` exists so that arithmetic is available
 * rather than re-derived, and `TargetPosition` exists so most strategies never have to do it.
 */
template <typename Broker>
struct Portfolio {
  /**
   * Every position row the broker reported.
   *
   * One instrument may appear in more than one row, so size a holding with `net_quantity_of`
   * rather than reading the first match.
   */
  std::vector<HoldingPosition> positions_;
  /** Spendable cash per currency, unconverted — no implicit foreign exchange. */
  std::vector<Money> cash_;
  /**
   * What the broker said may be spent, per currency.
   *
   * Separate from `cash_`, which is this library's own accounting and is filled by the backtest
   * ledger. Live this is the broker's answer; in a replay it is the ledger's modelled cash, which
   * the report states as modelled. The two fields are different quantities either way: see
   * `BuyingPower`. Empty means nobody has asked, which is not the same as an answer of zero — a
   * caller enforcing a buying-power limit must treat the empty case as unknown rather than as no
   * money.
   */
  std::vector<BuyingPower> buying_power_;
  /**
   * Orders still outstanding at the broker.
   *
   * The half most easily forgotten: sizing from `positions_` alone double-sizes after a partial
   * fill. `working_quantity` does that arithmetic.
   */
  std::vector<WorkingOrder<Broker>> working_;
  /**
   * When the holdings behind this portfolio arrived, and what a staleness gate measures against.
   *
   * **The local clock, not the broker's.** It is copied from `HoldingsSnapshot::received_at_`, which
   * both adapters set to `system_clock::now()` as they parse the reply, because neither broker
   * timestamps a holdings answer -- the same reason `BuyingPower::received_at_` gives. So this is
   * when the figures *arrived*, not when they were true, and the gate understates their age by
   * however long the request took.
   */
  std::chrono::system_clock::time_point as_of_{};

  /**
   * Returns the quantity held of `instrument`, or nothing when there is no position.
   *
   * Nothing rather than zero, because "no position reported" and "a position of zero" are
   * different claims, and a broker that omits a closed position is making the first.
   */
  std::optional<Decimal64> quantity_of(const InstrumentId& instrument) const {
    for (const auto& position : positions_) {
      if (position.instrument_ == instrument) {
        return position.quantity_;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns the **net** quantity held of `instrument` across every row, or nothing when the
   * arithmetic will not fit.
   *
   * **This is the one to size an order against; `quantity_of` is not.** That one returns the *first*
   * matching row, and nothing dedupes what an adapter appends -- a broker reporting one instrument
   * as two lots is ordinary, and `backtest::ReplayLedger` keeps a lot after it returns to flat. Two
   * rows of 60 and 40 are a holding of 100, and reading 60 understates it. Anything computing "how
   * much do I have" from the first row is wrong by however much the other rows hold.
   *
   * **Zero when nothing is reported, unlike `quantity_of`.** The distinction that one preserves --
   * no row versus a row of zero -- is real, and a caller that genuinely wants it should keep using
   * it. It is not a distinction an *amount* can carry, and every caller of this one goes on to do
   * arithmetic with the result, so folding both to zero here is what stops each of them inventing
   * the same fold. Note that "am I holding anything" is **not** such a caller: a spent lot of zero
   * listed beside a live one makes `quantity_of` answer flat, which is how `vanilla::Reversion`
   * came to re-enter a position it already held.
   *
   * `std::nullopt` means the sum overflowed or would not be representable, which is a broken
   * snapshot rather than a quantity of zero, and is the one case a caller must not treat as flat.
   */
  std::optional<Decimal64> net_quantity_of(const InstrumentId& instrument) const {
    auto total = Decimal64::parse("0");
    for (const auto& position : positions_) {
      if (!(position.instrument_ == instrument)) {
        continue;
      }
      const auto sum = decimal_add(total, position.quantity_);
      if (!sum) {
        return std::nullopt;
      }
      total = *sum;
    }
    return total;
  }

  /**
   * Returns the quantity still working on `side` for `instrument`.
   *
   * The remaining quantity of each open order — ordered minus filled — summed. Orders in a
   * terminal state contribute nothing, and an order whose filled quantity exceeds its ordered
   * quantity contributes zero rather than a negative, because a broker reporting that is
   * describing something this arithmetic should not amplify.
   */
  Decimal64 working_quantity(const InstrumentId& instrument, OrderSide side) const {
    Decimal64 total = Decimal64::parse("0");
    for (const auto& order : working_) {
      if (!(order.order_.instrument_ == instrument) || order.order_.side_ != side) {
        continue;
      }
      if (!order_is_open(order.order_.status_)) {
        continue;
      }
      const auto remaining = decimal_subtract(
        order.order_.quantity_, order.order_.execution_.filled_quantity_
      );
      if (!remaining || decimal_less(*remaining, Decimal64::parse("0"))) {
        continue;
      }
      const auto sum = decimal_add(total, *remaining);
      if (!sum) {
        continue;
      }
      total = *sum;
    }
    return total;
  }

  /** Returns the cash held in `currency`, or nothing when the broker reported none. */
  std::optional<Decimal64> cash_in(const Currency& currency) const {
    for (const auto& amount : cash_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns the buying power the broker reported in `currency`, or nothing when it reported none.
   *
   * Nothing means *unknown*, and the distinction from zero is the reason this returns an optional
   * rather than a default. Nobody has asked, or the broker refused — neither of which is a
   * statement that the account cannot buy.
   *
   * Live the figure is the broker's; in a replay it is the ledger's modelled cash. A caller cannot
   * tell them apart from here, which is deliberate — the guard should read one field in both
   * environments — and it is why the backtest report is where the modelling is disclosed.
   */
  std::optional<Decimal64> buying_power_in(const Currency& currency) const {
    for (const auto& amount : buying_power_) {
      if (amount.cash_.currency_ == currency) {
        return amount.cash_.amount_;
      }
    }
    return std::nullopt;
  }

  /**
   * Returns what the broker said is sellable of `instrument`, or nothing when it did not say.
   *
   * **Nothing is not zero and it is not the held quantity.** A broker that publishes the figure at
   * all publishes it per position — KIS carries it on every holdings snapshot — and a broker that
   * does not leaves it absent, as Toss does. A caller that substitutes `quantity_of` when this is
   * absent has answered a different question: T+2 settlement, pledged and locked quantity all make
   * a held position less than fully sellable, and that gap is exactly what this reports.
   *
   * Absent also covers "there is no such position", which `quantity_of` distinguishes for a caller
   * that needs to tell the two apart.
   */
  std::optional<Decimal64> sellable_quantity_of(const InstrumentId& instrument) const {
    for (const auto& position : positions_) {
      if (position.instrument_ == instrument) {
        return position.sellable_quantity_;
      }
    }
    return std::nullopt;
  }

  /** Returns the working order addressed by `ref`, or nothing when it is no longer open. */
  const WorkingOrder<Broker>* find_working(const OrderRef<Broker>& ref) const {
    for (const auto& order : working_) {
      if (order.ref_ == ref) {
        return &order;
      }
    }
    return nullptr;
  }

};

} // namespace strategy
} // namespace algotradelib
