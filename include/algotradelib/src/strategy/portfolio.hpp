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
  std::vector<HoldingPosition> positions_;
  /** Spendable cash per currency, unconverted — no implicit foreign exchange. */
  std::vector<Money> cash_;
  std::vector<WorkingOrder<Broker>> working_;
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
      if (!open_status(order.order_.status_)) {
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

  /** Returns the working order addressed by `ref`, or nothing when it is no longer open. */
  const WorkingOrder<Broker>* find_working(const OrderRef<Broker>& ref) const {
    for (const auto& order : working_) {
      if (order.ref_ == ref) {
        return &order;
      }
    }
    return nullptr;
  }

private:
  /**
   * Returns whether a status describes an order that can still trade.
   *
   * Listed explicitly rather than as "not terminal", so a status this library gains later does
   * not silently start counting toward a working quantity. `UNKNOWN` counts as open: an order
   * whose state is unrecognized may still be live, and under-counting working quantity is what
   * causes double-sizing, which is the failure this member exists to prevent.
   */
  static bool open_status(OrderStatus status) {
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
};

} // namespace strategy
} // namespace algotradelib
