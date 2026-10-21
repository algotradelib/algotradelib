#pragma once

#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * @brief Tests whether an order in a given status may still trade.
 *
 * @param status The order status.
 * @return `true` for `PENDING`, `PENDING_CANCEL`, `PENDING_REPLACE`, `PARTIAL_FILLED` and
 *   `UNKNOWN` (an unrecognized state is assumed to be still working); `false` for the terminal
 *   statuses.
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

namespace portfolio_detail {

// Adds a position row's quantity to a running net quantity; `false` if the sum does not fit.
inline bool add_net(Decimal64& total, const HoldingPosition& position) {
  const auto sum = decimal_add(total, position.quantity_);
  if (!sum) {
    return false;
  }
  total = *sum;
  return true;
}

// Adds what a working row still has to fill, if it is open and on `side`; a row reporting more filled than
// ordered, or a sum that does not fit, adds nothing.
template <typename Working>
void add_working(Decimal64& total, const Working& order, OrderSide side) {
  if (order.order_.side_ != side || !order_is_open(order.order_.status_)) {
    return;
  }
  const auto remaining = decimal_subtract(order.order_.quantity_, order.order_.execution_.filled_quantity_);
  if (!remaining || decimal_less(*remaining, Decimal64(0, 0))) {
    return;
  }
  if (const auto sum = decimal_add(total, *remaining)) {
    total = *sum;
  }
}

} // namespace portfolio_detail

/**
 * @brief What the account holds and which orders are still working, as a strategy sees it.
 *
 * Read it with `ctx.portfolio()`. Size orders against positions **and** working orders
 * (`net_quantity_of`, `working_quantity`), or emit a `TargetPosition` and let the runner do it;
 * sizing from positions alone buys twice after a partial fill.
 *
 * @tparam Broker The broker tag of the working orders' references; read the portfolio with `auto`
 *   so a strategy never names it.
 */
template <typename Broker>
struct Portfolio {
  /**
   * @brief Every position row the broker reported.
   *
   * One instrument may appear in more than one row; size a holding with `net_quantity_of`.
   */
  std::vector<HoldingPosition> positions_;
  /**
   * @brief Cash per currency, in a backtest only.
   *
   * @warning Only the backtest ledger fills this; live it stays empty, so `cash_in` returns a
   *   number in a backtest and `std::nullopt` live for the same strategy. To size against money,
   *   read `buying_power_` instead.
   */
  std::vector<Money> cash_;
  /**
   * @brief What may be spent, per currency.
   *
   * Live, the broker's figure; in a backtest, the ledger's modelled cash (the backtest report says
   * so). Empty means no figure was read, which is not the same as zero; see `BuyingPower`.
   */
  std::vector<BuyingPower> buying_power_;
  /**
   * @brief Orders still outstanding at the broker.
   *
   * Use `working_quantity` to sum what is still to fill.
   */
  std::vector<WorkingOrder<Broker>> working_;
  /**
   * @brief When the holdings were received, on this machine's clock.
   *
   * Brokers do not timestamp holdings, so this is when the reply arrived, not when the figures
   * were true. Staleness checks measure against it.
   */
  std::chrono::system_clock::time_point as_of_{};
  /**
   * @brief When the working-order listing was received, on the same clock as `as_of_`.
   *
   * Set by a live source; the epoch (the default) means no stamped listing, as in a backtest.
   */
  std::chrono::system_clock::time_point working_as_of_{};

  /**
   * @brief Tests whether the working-order listing is newer than the holdings.
   *
   * Then a resting buy that filled between the two reads appears in neither, and the portfolio
   * under-counts the position. The runner withholds a plan sized against such a portfolio
   * (`WithheldPlanReason::PORTFOLIO_SUPERSEDED`).
   *
   * @return `true` if both are stamped and the listing arrived after the holdings.
   */
  bool listing_newer_than_holdings() const noexcept {
    const std::chrono::system_clock::time_point epoch{};
    return as_of_ != epoch && working_as_of_ != epoch && working_as_of_ > as_of_;
  }
  /**
   * @brief What the `sellable_quantity_` figures on `positions_` cover.
   *
   * Set by whatever filled the positions. `RiskGuard` refuses to measure a sell against a figure
   * whose basis is unestablished, which is the default, so a hand-built portfolio refuses checked
   * sells until this is set.
   */
  SellableBasis sellable_basis_;

  /**
   * @brief Returns the quantity of the first position row for an instrument.
   *
   * @param instrument The instrument.
   * @return The first matching row's quantity, or `std::nullopt` if no row matches.
   * @warning A holding can be split across rows. To size an order, or to ask whether anything is
   *   held, use `net_quantity_of`.
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
   * @brief Returns the net quantity held of an instrument, summed over every row.
   *
   * Use this to size an order: a holding reported as two rows of 60 and 40 is 100.
   *
   * @param instrument The instrument.
   * @return The sum, which is zero when no row matches; `std::nullopt` only if the sum overflowed
   *   (a broken snapshot, which must not be treated as flat).
   */
  std::optional<Decimal64> net_quantity_of(const InstrumentId& instrument) const {
    auto total = Decimal64(0, 0);
    for (const auto& position : positions_) {
      if (position.instrument_ == instrument && !portfolio_detail::add_net(total, position)) {
        return std::nullopt;
      }
    }
    return total;
  }

  /**
   * @brief Returns the quantity still to fill on open orders for an instrument and side.
   *
   * Sums ordered minus filled over the orders `order_is_open` accepts. An order reporting more
   * filled than ordered contributes zero.
   *
   * @param instrument The instrument.
   * @param side The side.
   * @return The remaining quantity, zero if there is none.
   */
  Decimal64 working_quantity(const InstrumentId& instrument, OrderSide side) const {
    Decimal64 total = Decimal64(0, 0);
    for (const auto& order : working_) {
      if (order.order_.instrument_ == instrument) {
        portfolio_detail::add_working(total, order, side);
      }
    }
    return total;
  }

 public:
  /** @brief A sellable quantity together with what it covers, read from the same row. */
  struct SellableReading {
    /** @brief The figure, or `std::nullopt` if no row of the instrument reported one. */
    std::optional<Decimal64> quantity_;
    /** @brief What the figure covers; unestablished if the rows disagree. */
    SellableBasis basis_;
  };

  /**
   * @brief Returns an instrument's sellable quantity and its basis, taken from the same row.
   *
   * A row's own basis is used if it has one, otherwise the portfolio's `sellable_basis_`. When an
   * instrument has several rows, figures are not summed; if the rows' bases differ, or their
   * figures differ under the same basis, the reading's basis is unestablished and `RiskGuard`
   * refuses a checked sell.
   *
   * @param instrument The instrument.
   * @return The figure and its basis.
   */
  SellableReading sellable_reading_for(const InstrumentId& instrument) const {
    SellableReading reading;
    bool seen = false;
    for (const auto& position : positions_) {
      if (!(position.instrument_ == instrument)) {
        continue;
      }
      const SellableBasis basis =
        position.sellable_basis_ ? *position.sellable_basis_ : sellable_basis_;
      if (!seen) {
        reading.quantity_ = position.sellable_quantity_;
        reading.basis_ = basis;
        seen = true;
        continue;
      }
      // A second row of the same instrument. Figures are never summed, and a disagreement makes even
      // the first row's figure uninterpretable, since nothing says which lot a sale draws from.
      // First, a basis that disagrees.
      if (basis.coverage_ != reading.basis_.coverage_ ||
          basis.resting_ != reading.basis_.resting_) {
        return SellableReading{reading.quantity_, SellableBasis{}};
      }
      // Then figures that disagree under one basis: two rows claiming the limit for the same shares
      // with different numbers contradict each other. (KIS pages can repeat a `pdno`.)
      // Compared as numbers: `operator==` also compares the scale, and "70" beside "70.0" is one figure.
      if (reading.quantity_ && position.sellable_quantity_ &&
          (decimal_less(*position.sellable_quantity_, *reading.quantity_) ||
           decimal_less(*reading.quantity_, *position.sellable_quantity_))) {
        return SellableReading{reading.quantity_, SellableBasis{}};
      }
      // A later row's figure where the first had none is the only figure stated.
      if (!reading.quantity_) {
        reading.quantity_ = position.sellable_quantity_;
      }
    }
    return reading;
  }

  /**
   * @brief Returns the cash held in a currency (backtest only; see `cash_`).
   * @param currency The currency.
   * @return The amount, or `std::nullopt` if none is recorded (always, live).
   */
  std::optional<Decimal64> cash_in(const Currency& currency) const {
    for (const auto& amount : cash_) {
      if (amount.currency_ == currency) {
        return amount.amount_;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Returns the buying power in a currency.
   *
   * Live, the broker's figure; in a backtest, the ledger's modelled cash.
   *
   * @param currency The currency.
   * @return The amount, or `std::nullopt` if unknown (not read, or the broker refused), which is
   *   not the same as zero.
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
   * @brief Returns the sellable quantity the broker reported for an instrument (first row).
   *
   * Sellable can be less than held (unsettled, pledged or locked shares). A broker that does not
   * publish it leaves this absent; do not substitute the held quantity. To check a sell, prefer
   * `sellable_reading_for`, which also says what the figure covers.
   *
   * @param instrument The instrument.
   * @return The first matching row's figure, or `std::nullopt` if the broker gave none or there is
   *   no position.
   */
  std::optional<Decimal64> sellable_quantity_of(const InstrumentId& instrument) const {
    for (const auto& position : positions_) {
      if (position.instrument_ == instrument) {
        return position.sellable_quantity_;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Finds a working order by its reference.
   * @param ref The order reference.
   * @return A pointer into `working_`, or `nullptr` if no working order has that reference.
   */
  const WorkingOrder<Broker>* find_working(const OrderRef<Broker>& ref) const {
    for (const auto& order : working_) {
      if (order.ref_ == ref) {
        return &order;
      }
    }
    return nullptr;
  }

};

/**
 * @brief Where each instrument's rows are in one portfolio: its position rows and its working rows, in order.
 *
 * Built once for a pass that reconciles many targets against the same portfolio, so that each target finds
 * its own rows instead of scanning all of them (see `ReconcileInputs::rows_`). Describes the portfolio it was
 * built from only while that portfolio's `positions_` and `working_` are unchanged.
 */
class PortfolioRows {
public:
  /** @brief Where one instrument's rows are. */
  struct Rows {
    /** @brief Places in `positions_`, ascending. */
    std::vector<std::size_t> positions_;
    /** @brief Places in `working_`, ascending. */
    std::vector<std::size_t> working_;
  };

  /**
   * @brief Indexes a portfolio.
   * @tparam Broker The portfolio's broker tag.
   * @param portfolio The portfolio.
   */
  template <typename Broker>
  explicit PortfolioRows(const Portfolio<Broker>& portfolio) {
    for (std::size_t at = 0; at < portfolio.positions_.size(); ++at) {
      rows_[portfolio.positions_[at].instrument_].positions_.push_back(at);
    }
    for (std::size_t at = 0; at < portfolio.working_.size(); ++at) {
      rows_[portfolio.working_[at].order_.instrument_].working_.push_back(at);
    }
  }

  /**
   * @brief Returns where an instrument's rows are.
   * @param instrument The instrument.
   * @return Its rows, or a null pointer if it has none.
   */
  const Rows* of(const InstrumentId& instrument) const {
    const auto found = rows_.find(instrument);
    return found == rows_.end() ? nullptr : &found->second;
  }

private:
  std::unordered_map<InstrumentId, Rows, id_detail::InstrumentIdHash> rows_;
};

} // namespace strategy
} // namespace algotradelib
