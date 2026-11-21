#pragma once

#include <algotradelib/core>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * @brief One run's cash and its own shares, for `strategy::RiskLimits::run_budget_`.
 *
 * A new run opens with the budget as its cash and no shares. Each fill moves both: a buy spends its amount and
 * its fee and adds its quantity, a sell returns its amount less its fee and takes its quantity away. The cash
 * may go below zero only by a fee booked above what was reserved; the shares never do.
 *
 * `runner::Runner` keeps one when a budget is set; read it through `Runner::run_cash` and `Runner::run_holding`.
 */
class RunLedger {
public:
  /**
   * @brief Opens a new run.
   * @param opening The run's starting cash, which names its currency.
   */
  explicit RunLedger(Money opening) : cash_(std::move(opening)) {}

  /**
   * @brief Returns the run's cash.
   * @return The cash, in the run's currency.
   */
  const Money& cash() const noexcept {
    return cash_;
  }

  /**
   * @brief Returns the run's currency.
   * @return The currency every amount the run books must be in.
   */
  const Currency& currency() const noexcept {
    return cash_.currency_;
  }

  /**
   * @brief Returns the run's own shares of an instrument.
   * @param instrument The instrument.
   * @return The shares; zero for an instrument the run never traded.
   */
  Decimal64 holding(const InstrumentId& instrument) const {
    const auto at = find(instrument);
    return at ? shares_[*at].quantity_ : Decimal64::parse("0");
  }

  /**
   * @brief Books one fill.
   *
   * @param instrument The instrument.
   * @param side The fill's side.
   * @param filled The quantity filled.
   * @param amount What the fill traded for, quantity times price.
   * @param fee Its commission and taxes together.
   * @return `true` if booked; `false`, with nothing changed, if `amount` is in another currency than the run's,
   *   a figure is negative, a sell is more than the run's shares, or the arithmetic does not fit.
   */
  bool book(
    const InstrumentId& instrument, OrderSide side, const Decimal64& filled, const Money& amount, const Decimal64& fee
  ) {
    const auto zero = Decimal64::parse("0");
    if (
      !(amount.currency_ == cash_.currency_) || decimal_less(filled, zero) || decimal_less(amount.amount_, zero) ||
      decimal_less(fee, zero)
    ) {
      return false;
    }
    const auto at = find(instrument);
    const Decimal64 held = at ? shares_[*at].quantity_ : zero;
    std::optional<Decimal64> shares;
    std::optional<Decimal64> cash;
    if (side == OrderSide::BUY) {
      shares = decimal_add(held, filled);
      const auto spent = decimal_add(amount.amount_, fee);
      cash = spent ? decimal_subtract(cash_.amount_, *spent) : std::nullopt;
    } else {
      if (decimal_less(held, filled)) {
        return false;
      }
      shares = decimal_subtract(held, filled);
      const auto returned = decimal_subtract(amount.amount_, fee);
      cash = returned ? decimal_add(cash_.amount_, *returned) : std::nullopt;
    }
    if (!shares || !cash) {
      return false;
    }
    if (at) {
      shares_[*at].quantity_ = *shares;
    } else {
      add(instrument, *shares);
    }
    cash_.amount_ = *cash;
    return true;
  }

private:
  // One instrument's shares.
  struct Shares {
    InstrumentId instrument_;
    Decimal64 quantity_;
  };

  // The instrument at a position of `shares_`, for the table.
  struct IdOf {
    const std::vector<Shares>* shares_;
    const InstrumentId& operator()(std::size_t at) const noexcept {
      return (*shares_)[at].instrument_;
    }
  };

  // Where `instrument` sits in `shares_`: a scan up to `id_detail::scan_limit` rows, the table beyond.
  std::optional<std::size_t> find(const InstrumentId& instrument) const noexcept {
    if (table_.empty()) {
      for (std::size_t at = 0; at < shares_.size(); ++at) {
        if (shares_[at].instrument_ == instrument) {
          return at;
        }
      }
      return std::nullopt;
    }
    return table_.find(instrument, IdOf{&shares_});
  }

  // Appends a row; if the table cannot take it, the row is removed again and the exception passes on.
  void add(const InstrumentId& instrument, const Decimal64& quantity) {
    shares_.push_back(Shares{instrument, quantity});
    try {
      if (!table_.empty()) {
        table_.insert(instrument, shares_.size() - 1, IdOf{&shares_});
      } else if (shares_.size() > id_detail::scan_limit) {
        id_detail::PositionTable built;
        for (std::size_t at = 0; at < shares_.size(); ++at) {
          built.insert(shares_[at].instrument_, at, IdOf{&shares_});
        }
        table_ = std::move(built);
      }
    } catch (...) {
      shares_.pop_back();
      throw;
    }
  }

  Money cash_;
  std::vector<Shares> shares_;
  id_detail::PositionTable table_;
};

} // namespace runner
} // namespace algotradelib
