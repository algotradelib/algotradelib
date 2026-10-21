#pragma once

#include "../strategy/event.hpp"
#include "../strategy/portfolio.hpp"
#include "cost.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief The replay ledger: positions, cash and realized profit a backtest keeps, in exact decimals.
 */

/**
 * @brief The broker tag of a replay's order references (they reach no broker).
 */
struct BacktestTag {
  /** @brief The name an order reference prints: `"backtest"`. */
  static constexpr std::string_view name = "backtest";
};

/** @brief One execution a backtest produced, and what it cost. */
struct Execution {
  /** @brief The fill, as `on_fill` receives it. */
  strategy::Fill fill_;
  /** @brief The broker's commission; includes the institution fee when `institution_fee_` is empty. */
  Decimal64 commission_;
  /**
   * @brief The institution fee (유관기관제비용), if the rate was split; empty is not zero.
   *
   * Both this and `commission_` are deducted from cash.
   */
  std::optional<Decimal64> institution_fee_;
  /** @brief The currency of the cost figures. */
  Currency currency_{Currency{"KRW"}};
  /** @brief The commission rate that charged this execution. */
  AppliedRate applied_;
  /** @brief The price given up to slippage (already included in the fill price). */
  Decimal64 slippage_;
  /**
   * @brief The sale tax charged; zero unless a sell tax applies (`NextBarFillModel::with_sell_tax`).
   *
   * Deducted from cash; like the fees, not part of realized profit, which is price alone.
   */
  Decimal64 tax_;
};

/**
 * @brief Positions, cash and realized profit, updated by each execution of a replay.
 *
 * Cash and profit are kept per currency and never converted.
 */
class ReplayLedger {
public:
  /**
   * @brief Constructs a ledger holding only cash.
   * @param cash The starting cash.
   * @param currency Its currency.
   */
  ReplayLedger(Decimal64 cash, Currency currency) : own_(currency) {
    cash_.push_back(Money{std::move(cash), std::move(currency)});
    recount();
  }

  /**
   * @brief Copies a ledger.
   * @param other The ledger to copy.
   */
  ReplayLedger(const ReplayLedger& other) = default;

  /**
   * @brief Moves a ledger.
   * @param other The ledger to move from.
   */
  ReplayLedger(ReplayLedger&& other) noexcept
  : positions_(std::move(other.positions_)),
    index_(std::move(other.index_)),
    cash_(std::move(other.cash_)),
    realized_(std::move(other.realized_)),
    executions_(std::move(other.executions_)),
    held_(std::move(other.held_)),
    lots_(std::move(other.lots_)),
    changed_(std::move(other.changed_)),
    changed_count_(other.changed_count_),
    own_(other.own_),
    running_(other.running_) {
    other.forget_tracking();
  }

  /**
   * @brief Replaces this ledger with a move of another; moving a ledger into itself leaves it unchanged.
   * @param other The ledger to move from.
   * @return This ledger.
   */
  ReplayLedger& operator=(ReplayLedger&& other) noexcept {
    // A self-move must not reach the members: it would empty `positions_` while `index_` kept its entries.
    if (this != &other) {
      positions_ = std::move(other.positions_);
      index_ = std::move(other.index_);
      cash_ = std::move(other.cash_);
      realized_ = std::move(other.realized_);
      executions_ = std::move(other.executions_);
      held_ = std::move(other.held_);
      lots_ = std::move(other.lots_);
      changed_ = std::move(other.changed_);
      changed_count_ = other.changed_count_;
      own_ = other.own_;
      running_ = other.running_;
      other.forget_tracking();
    }
    return *this;
  }

  /**
   * @brief Replaces this ledger with a copy of another, entirely or not at all.
   * @param other The ledger to copy.
   * @return This ledger.
   * @throws std::bad_alloc If the copy cannot be made; this ledger is then unchanged.
   */
  ReplayLedger& operator=(const ReplayLedger& other) {
    if (this != &other) {
      ReplayLedger copy(other);
      *this = std::move(copy);
    }
    return *this;
  }

  /**
   * @brief Returns the cash held in a currency.
   * @param currency The currency.
   * @return The amount, or `std::nullopt` if none is held.
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
   * @brief Returns the quantity held of an instrument.
   * @param instrument The instrument.
   * @return The quantity (zero if the position went flat), or `std::nullopt` if never held.
   */
  std::optional<Decimal64> quantity_of(const InstrumentId& instrument) const {
    if (const auto* lot = find(instrument)) {
      return lot->quantity_;
    }
    return std::nullopt;
  }

  /**
   * @brief Returns the average price of the position in an instrument.
   * @param instrument The instrument.
   * @return The average price, or `std::nullopt` if flat or never held.
   */
  std::optional<Decimal64> average_price_of(const InstrumentId& instrument) const {
    if (const auto* lot = find(instrument); lot && lot->quantity_.coefficient() != 0) {
      return lot->average_purchase_price_;
    }
    return std::nullopt;
  }

  /**
   * @brief Returns the profit realized by closing positions, in a currency (fees excluded).
   * @param currency The currency.
   * @return The amount; zero if none.
   */
  Decimal64 realized_in(const Currency& currency) const {
    for (const auto& entry : realized_) {
      if (entry.currency_ == currency) {
        return entry.amount_;
      }
    }
    return Decimal64::parse("0");
  }

  /**
   * @brief Returns every position, including ones that went flat.
   *
   * Of the money fields only `market_value_` (quantity times last mark) is filled;
   * `purchase_value_`, `unrealized_profit_loss_` and `unrealized_return_` stay at their defaults.
   *
   * @return The positions.
   */
  const std::vector<HoldingPosition>& positions() const noexcept { return positions_; }

  /**
   * @brief Returns every execution applied.
   * @return The executions, oldest first.
   */
  const std::vector<Execution>& executions() const noexcept { return executions_; }

  /**
   * @brief Returns total equity in the ledger's own currency (the constructor's), as `equity_in` would.
   *
   * The same value, digit for digit and in the same representation, as `equity_in` for that currency,
   * without adding up every position: the ledger keeps a running total. It uses it only while the
   * magnitudes of the cash and of every position it counts, added up and written at the largest scale
   * among them, fit -- which guarantees that `equity_in`'s own additions, in any order, cannot overflow --
   * and otherwise calls `equity_in`.
   *
   * @return The equity, or `std::nullopt` if the arithmetic does not fit.
   */
  std::optional<Decimal64> equity() const {
    if (!running_.valid_ || !fits_at(running_.bound_, running_.scale_)) {
      return equity_in(own_);
    }
    // With nothing counted, `equity_in` returns the cash as it is stored, without normalizing it.
    if (running_.counted_ == 0) {
      return cash_in(own_).value_or(Decimal64(0, 0));
    }
    return running_.total_;
  }

  /**
   * @brief Applies one execution to the position, cash and realized profit.
   *
   * A trade through flat (selling 150 from a long of 100) closes the old position, realizing its
   * profit against the old average, and opens the remainder at the new price. Commission,
   * institution fee and tax are deducted from cash.
   *
   * @param execution The execution.
   * @param currency The instrument's currency.
   * @return `false`, with the ledger unchanged, if the arithmetic does not fit, or if `currency`
   *   differs from the currency of a non-flat position in the instrument; `true` otherwise.
   * @throws std::bad_alloc If an allocation fails; the ledger is then unchanged.
   */
  bool apply(const Execution& execution, const Currency& currency) {
    const auto& fill = execution.fill_;
    const auto zero = Decimal64(0, 0);

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
    // Both the broker's commission and the institution fee leave cash.
    const auto after_fees = execution.institution_fee_ ?
      decimal_subtract(*after_commission, *execution.institution_fee_) :
      after_commission;
    if (!after_fees) {
      return false;
    }
    const auto after_tax = decimal_subtract(*after_fees, execution.tax_);
    if (!after_tax) {
      return false;
    }

    // Read, never opened. Opening the lot here is what made an early return leave a trace.
    const auto* existing = find(fill.instrument_);
    const auto held = existing != nullptr ? existing->quantity_ : zero;
    const auto average = existing != nullptr ? existing->average_purchase_price_ : zero;

    // A non-flat position cannot change currency; refuse rather than relabel it.
    if (existing != nullptr && held.coefficient() != 0 &&
        !(existing->market_value_.currency_ == currency)) {
      return false;
    }

    const auto closing = closed_quantity(held, *signed_quantity);
    if (!closing) {
      return false;
    }

    // Realized profit exists only on the part that closed, and only against the old average.
    std::optional<Decimal64> realized_total;
    if (closing->coefficient() != 0) {
      const auto moved = decimal_subtract(fill.price_, average);
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
      realized_total = decimal_add(realized_in(currency), *banked);
      if (!realized_total) {
        return false;
      }
    }

    const auto resulting = decimal_add(held, *signed_quantity);
    if (!resulting) {
      return false;
    }

    const auto valued = decimal_multiply(*resulting, fill.price_);
    if (!valued) {
      return false;
    }

    // The average price follows only the part that opened or added. A position crossing flat takes the
    // fill price outright, because nothing of the old basis survives.
    auto new_average = average;
    if (resulting->coefficient() == 0) {
      new_average = zero;
    } else if (crossed_or_opened(held, *resulting)) {
      new_average = fill.price_;
    } else if (adding(held, *signed_quantity)) {
      const auto blended = weighted_average(held, average, *signed_quantity, fill.price_);
      if (!blended) {
        return false;
      }
      new_average = *blended;
    }
    // Reducing without crossing leaves the average alone: selling half a position does not change
    // what the remaining half cost.

    const auto cash_total = decimal_add(
      cash_in(currency).value_or(zero), *after_tax
    );
    if (!cash_total) {
      return false;
    }

    // Room in `held_` for this lot is made before anything changes, so the insert below cannot allocate,
    // whatever the list's history (a copy holds no spare room).
    if (resulting->coefficient() != 0 && held_.size() == held_.capacity()) {
      held_.reserve(std::max<std::size_t>(8, 2 * held_.capacity()));
    }
    // **And every other allocation, so that a failed one leaves the ledger as it was.** A currency seen for
    // the first time gets its cash and realized entries' room here, and the execution is copied and given
    // room now; after `find_or_open` below (which undoes itself if it fails), nothing allocates.
    reserve_entry(cash_, currency);
    if (realized_total) {
      reserve_entry(realized_, currency);
    }
    auto recorded = execution;
    if (executions_.size() == executions_.capacity()) {
      executions_.reserve(std::max<std::size_t>(8, 2 * executions_.capacity()));
    }

    // --- No arithmetic below can fail, and nothing below returns false -----------------------
    auto* lot = find_or_open(fill.instrument_);
    if ((lot->quantity_.coefficient() != 0) != (resulting->coefficient() != 0)) {
      const auto at = static_cast<std::size_t>(lot - positions_.data());
      const auto where = std::lower_bound(held_.begin(), held_.end(), at);
      if (resulting->coefficient() != 0) {
        held_.insert(where, at);
      } else {
        held_.erase(where);
      }
    }
    lot->average_purchase_price_ = new_average;
    lot->quantity_ = *resulting;
    lot->last_price_ = fill.price_;
    // **The currency is recorded here because this is the only place it is known.** `mark` is not
    // told one and neither is `equity_in`'s caller beyond the currency it is asking about, so a
    // position that never wrote its own currency down cannot be told apart from one denominated in
    // whatever the caller happened to ask for. `Currency` default-constructs to KRW
    // (`Currency`'s default constructor), which makes the untold case silently *answer* rather than be absent --
    // a USD position would read as Korean won to anything summing per currency.
    lot->market_value_ = Money{*valued, currency};
    set_amount(cash_, currency, *cash_total);
    if (realized_total) {
      set_amount(realized_, currency, *realized_total);
    }
    static_assert(std::is_nothrow_move_constructible<Execution>::value,
      "the execution is moved into room made for it, which must not throw");
    executions_.push_back(std::move(recorded));
    const auto at = static_cast<std::size_t>(lot - positions_.data());
    note_changed(at);
    retrack(at);
    if (currency == own_) {
      retrack_cash(*cash_total);
    }
    return true;
  }

  /**
   * @brief Marks positions in an instrument to a price, updating their market value.
   *
   * @param instrument The instrument.
   * @param price The new mark.
   * @return `false` if a position's value does not fit (that position keeps its old mark and value;
   *   `Backtester` halts the replay with `mark_not_representable`); `true` otherwise.
   */
  bool mark(const InstrumentId& instrument, const Decimal64& price) {
    // An instrument has at most one lot: `find_or_open` opens one only when none matches.
    const auto slot = slot_of(instrument);
    if (!slot) {
      return true;
    }
    auto& lot = positions_[*slot];
    // **Both fields move or neither does, and the caller is told which.** Two earlier versions
    // each lost one half of that. Advancing the price and leaving the old `market_value_`
    // handed a strategy reading `positions()` or `snapshot()` a stale number presented as
    // current, with no error anywhere; skipping the row silently declined to apply a price the
    // caller asked for. The currency is the one `apply` recorded -- a lot exists only because a
    // fill opened it -- and it is the half nothing can invalidate.
    const auto valued = decimal_multiply(lot.quantity_, price);
    if (!valued) {
      return false;
    }
    lot.last_price_ = price;
    lot.market_value_ = Money{*valued, lot.market_value_.currency_};
    note_changed(*slot);
    retrack(*slot);
    return true;
  }

  /**
   * @brief Returns the portfolio a strategy sees.
   *
   * The model a replay presents: every held share is sellable (basis whole holding, deduct resting
   * sells, unverified), buying power is this ledger's cash (no settlement is modelled), and `cash_`
   * is filled. Working orders are left empty; `Backtester` fills them from its queue.
   *
   * @tparam BrokerTag The order-reference tag of the portfolio.
   * @param as_of The snapshot time, for `as_of_` and the buying-power stamps.
   * @return The portfolio.
   */
  template <typename BrokerTag>
  strategy::Portfolio<BrokerTag> snapshot(
    std::chrono::system_clock::time_point as_of
  ) const {
    strategy::Portfolio<BrokerTag> portfolio;
    portfolio.positions_ = positions_;
    // Every held share is sellable: a model consistent with crediting sales on the fill (no
    // settlement); `ReportCaveats` discloses it.
    for (auto& position : portfolio.positions_) {
      position.sellable_quantity_ = position.quantity_;
    }
    // The basis a producer of sellable figures must state: whole holding, deduct resting, unverified.
    portfolio.sellable_basis_ = SellableBasis{
      SellableBasis::Coverage::WHOLE_HOLDING,
      SellableBasis::RestingTreatment::DEDUCT,
      false,
    };
    portfolio.cash_ = cash_;
    // Buying power modelled as this ledger's cash, stamped at the snapshot time; the report states
    // the model.
    portfolio.buying_power_.reserve(cash_.size());
    for (const auto& amount : cash_) {
      portfolio.buying_power_.push_back(BuyingPower{amount, as_of});
    }
    portfolio.as_of_ = as_of;
    return portfolio;
  }

  /**
   * @brief Brings a portfolio built by `snapshot` up to date, changing only what changed since.
   *
   * Afterwards its positions, sellable basis, cash, buying power and `as_of_` are what `snapshot` would
   * return now; its working orders and everything else are left alone. Only the lots that `apply` or `mark`
   * changed since `snapshot`, `clear_changed` or the last `update` are rewritten, plus any opened since,
   * so a bar that marked one lot rewrites one row.
   *
   * The portfolio must be one this ledger's `snapshot` built, whose positions only `update` has changed
   * since; anything else written to them stays.
   *
   * @tparam BrokerTag The order-reference tag of the portfolio.
   * @param portfolio The portfolio to update.
   * @param as_of The time, for `as_of_` and the buying-power stamps.
   * @throws std::bad_alloc If a row, the cash or the buying power cannot be copied; the ledger still lists
   *   every changed lot, so a later `update` completes it.
   */
  template <typename BrokerTag>
  void update(strategy::Portfolio<BrokerTag>& portfolio, std::chrono::system_clock::time_point as_of) {
    auto& rows = portfolio.positions_;
    for (std::size_t k = 0; k < changed_count_; ++k) {
      const auto at = changed_[k];
      if (at >= rows.size()) {
        continue;
      }
      // The fields `apply` and `mark` write; a lot's instrument never changes.
      const auto& lot = positions_[at];
      auto& row = rows[at];
      row.quantity_ = lot.quantity_;
      row.sellable_quantity_ = lot.quantity_;
      row.average_purchase_price_ = lot.average_purchase_price_;
      row.last_price_ = lot.last_price_;
      row.market_value_ = lot.market_value_;
    }
    if (rows.size() < positions_.size()) {
      rows.reserve(positions_.size());
      while (rows.size() < positions_.size()) {
        rows.push_back(positions_[rows.size()]);
        rows.back().sellable_quantity_ = rows.back().quantity_;
      }
    }
    portfolio.sellable_basis_ = SellableBasis{
      SellableBasis::Coverage::WHOLE_HOLDING,
      SellableBasis::RestingTreatment::DEDUCT,
      false,
    };
    portfolio.cash_ = cash_;
    portfolio.buying_power_.clear();
    portfolio.buying_power_.reserve(cash_.size());
    for (const auto& amount : cash_) {
      portfolio.buying_power_.push_back(BuyingPower{amount, as_of});
    }
    portfolio.as_of_ = as_of;
    clear_changed();
  }

  /**
   * @brief Forgets which lots changed, after a `snapshot` that already shows them.
   */
  void clear_changed() noexcept {
    for (std::size_t k = 0; k < changed_count_; ++k) {
      lots_[changed_[k]].changed_ = false;
    }
    changed_count_ = 0;
  }

  /**
   * @brief Returns total equity in a currency: cash plus positions at their last marks.
   *
   * Positions in other currencies are left out, not converted.
   *
   * @param currency The currency.
   * @return The equity, or `std::nullopt` if the arithmetic does not fit.
   */
  std::optional<Decimal64> equity_in(const Currency& currency) const {
    auto total = cash_in(currency).value_or(Decimal64(0, 0));
    // The lots with a non-zero quantity, in the order of `positions_`.
    for (const auto at : held_) {
      const auto& lot = positions_[at];
      // Only positions in `currency` (recorded on `market_value_` by `apply`); no conversion.
      if (!(lot.market_value_.currency_ == currency)) {
        continue;
      }
      // Computed from the last mark rather than read from `market_value_`.
      const auto value = decimal_multiply(lot.quantity_, lot.last_price_);
      if (!value) {
        return std::nullopt;
      }
      const auto sum = decimal_add(total, *value);
      if (!sum) {
        return std::nullopt;
      }
      total = *sum;
    }
    return total;
  }

private:
  // Returns the position in `instrument`, or nothing, **without creating one**.
  const HoldingPosition* find(const InstrumentId& instrument) const {
    const auto slot = slot_of(instrument);
    return slot ? &positions_[*slot] : nullptr;
  }

  // Returns where `instrument`'s lot is in `positions_`: by the index once the ledger holds more than
  // `id_detail::scan_limit` lots, by a scan before that.
  std::optional<std::size_t> slot_of(const InstrumentId& instrument) const {
    if (!index_.empty()) {
      return index_.find(instrument, lot_ids());
    }
    for (std::size_t at = 0; at < positions_.size(); ++at) {
      if (positions_[at].instrument_ == instrument) {
        return at;
      }
    }
    return std::nullopt;
  }

  // The id at a position of `positions_`, for the index.
  struct LotIds {
    const std::vector<HoldingPosition>* positions_;
    const InstrumentId& operator()(std::size_t at) const noexcept { return (*positions_)[at].instrument_; }
  };
  LotIds lot_ids() const noexcept { return LotIds{&positions_}; }

  HoldingPosition* find_or_open(const InstrumentId& instrument) {
    if (const auto slot = slot_of(instrument)) {
      return &positions_[*slot];
    }
    HoldingPosition opened;
    opened.instrument_ = instrument;
    opened.quantity_ = Decimal64(0, 0);
    opened.average_purchase_price_ = Decimal64(0, 0);
    opened.last_price_ = Decimal64(0, 0);
    // Room in the lists kept beside `positions_` first, so that after the lot is added nothing below can leave
    // them disagreeing with it.
    grow_for(lots_, positions_.size() + 1);
    grow_for(changed_, positions_.size() + 1);
    positions_.push_back(std::move(opened));
    lots_.emplace_back();
    changed_.push_back(0);
    if (index_.empty() && positions_.size() <= id_detail::scan_limit) {
      return &positions_.back();
    }
    // Past `id_detail::scan_limit` lots the index holds every lot: built whole at the first one past it,
    // extended after that. If it cannot be, the lot comes out again, so the two never disagree. Lots are never
    // removed or reordered, so a position stays valid.
    try {
      if (index_.empty()) {
        id_detail::PositionTable built;
        for (std::size_t at = 0; at < positions_.size(); ++at) {
          built.insert(positions_[at].instrument_, at, lot_ids());
        }
        index_ = std::move(built);
      } else {
        index_.insert(instrument, positions_.size() - 1, lot_ids());
      }
    } catch (...) {
      positions_.pop_back();
      lots_.pop_back();
      changed_.pop_back();
      throw;
    }
    return &positions_.back();
  }

  // Returns how much of `held` this trade closes, which is zero when it only adds.
  static std::optional<Decimal64> closed_quantity(
    const Decimal64& held,
    const Decimal64& traded
  ) {
    const auto zero = Decimal64(0, 0);
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

  // Returns whether the trade opened from flat or crossed through it.
  static bool crossed_or_opened(const Decimal64& held, const Decimal64& resulting) {
    const auto zero = Decimal64(0, 0);
    if (held.coefficient() == 0) {
      return true;
    }
    return decimal_less(zero, held) != decimal_less(zero, resulting);
  }

  // Returns whether the trade added to a position rather than reducing it.
  static bool adding(const Decimal64& held, const Decimal64& traded) {
    const auto zero = Decimal64(0, 0);
    return decimal_less(zero, held) == decimal_less(zero, traded);
  }

  // Returns the average of two lots by magnitude, which is what adding to a position produces.
  static std::optional<Decimal64> weighted_average(
    const Decimal64& held,
    const Decimal64& held_price,
    const Decimal64& traded,
    const Decimal64& traded_price
  ) {
    const auto zero = Decimal64(0, 0);
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

  // Writes `total` as the whole amount held in `currency`, appending the entry when there is none.
  //
  // Takes the **total** rather than a change, because the addition is the fallible half and `apply`
  // has already done it before committing anything. What is left cannot fail, which is what lets
  // `apply` promise the ledger is either wholly updated or untouched.
  static void set_amount(
    std::vector<Money>& amounts,
    const Currency& currency,
    const Decimal64& total
  ) {
    for (auto& amount : amounts) {
      if (amount.currency_ == currency) {
        amount.amount_ = total;
        return;
      }
    }
    amounts.push_back(Money{total, currency});
  }

  std::vector<HoldingPosition> positions_;
  // Where each instrument's lot is in `positions_`; empty until the ledger holds more than
  // `id_detail::scan_limit` lots.
  id_detail::PositionTable index_;
  std::vector<Money> cash_;
  std::vector<Money> realized_;
  std::vector<Execution> executions_;
  // Where the lots with a non-zero quantity are in `positions_`, ascending; `apply` keeps it.
  std::vector<std::size_t> held_;

  // What the ledger keeps about each lot, beside `positions_` and in the same order.
  struct LotTrack {
    // Listed in `changed_`.
    bool changed_{false};
    // The value `running_` counts for this lot: its market value, while it is held in `own_`.
    std::optional<Decimal64> counted_;
  };
  std::vector<LotTrack> lots_;
  // The lots changed since the last `update` or `clear_changed`, in its first `changed_count_` places. As
  // long as `positions_`, so that listing a lot never allocates.
  std::vector<std::size_t> changed_;
  std::size_t changed_count_{0};

  // The running equity in `own_` that `equity()` returns, and what proves it equal to `equity_in`'s.
  struct Running {
    // False when an update did not fit and counting everything again did not either.
    bool valid_{false};
    // The cash plus every counted value.
    Decimal64 total_;
    // The magnitudes of the cash and of every counted value, added up.
    Decimal64 bound_;
    // The largest scale among the cash and the counted values seen; never lowered.
    std::uint8_t scale_{0};
    // The cash counted in `total_`.
    Decimal64 cash_;
    // How many lots are counted: those held (non-zero quantity) in `own_`, whatever their value.
    std::size_t counted_{0};
  };
  // The constructor's currency.
  Currency own_;
  Running running_;

  // Makes room for `currency`'s entry in `amounts` if it has none, so `set_amount` will not allocate.
  static void reserve_entry(std::vector<Money>& amounts, const Currency& currency) {
    for (const auto& amount : amounts) {
      if (amount.currency_ == currency) {
        return;
      }
    }
    amounts.reserve(amounts.size() + 1);
  }

  // Makes room for `size` entries, doubling, so opening lots one at a time does not copy every list each time.
  template <typename T>
  static void grow_for(std::vector<T>& list, std::size_t size) {
    if (list.capacity() < size) {
      list.reserve(std::max<std::size_t>(size, 2 * list.capacity()));
    }
  }

  // Lists lot `at` as changed; never allocates, since `changed_` has a place for every lot.
  void note_changed(std::size_t at) noexcept {
    if (!lots_[at].changed_) {
      lots_[at].changed_ = true;
      changed_[changed_count_++] = at;
    }
  }

  static std::optional<Decimal64> magnitude(const Decimal64& value) {
    return value.coefficient() < 0 ? decimal_subtract(Decimal64(0, 0), value) : std::optional<Decimal64>(value);
  }

  // Returns whether a non-negative `value` still fits when written at `scale`.
  static bool fits_at(const Decimal64& value, std::uint8_t scale) noexcept {
    auto coefficient = value.coefficient();
    for (auto step = value.scale(); step < scale; ++step) {
      if (coefficient > std::numeric_limits<std::int64_t>::max() / 10) {
        return false;
      }
      coefficient *= 10;
    }
    return true;
  }

  // Replaces `before` with `after` in the running totals, or marks them invalid if that does not fit.
  void replace_counted(const std::optional<Decimal64>& before, const std::optional<Decimal64>& after) noexcept {
    std::optional<Decimal64> total = running_.total_;
    std::optional<Decimal64> bound = running_.bound_;
    if (before) {
      const auto size = magnitude(*before);
      total = decimal_subtract(*total, *before);
      bound = size ? decimal_subtract(*bound, *size) : std::nullopt;
    }
    if (after && total && bound) {
      const auto size = magnitude(*after);
      total = decimal_add(*total, *after);
      bound = size ? decimal_add(*bound, *size) : std::nullopt;
      running_.scale_ = std::max(running_.scale_, after->scale());
    }
    if (!total || !bound) {
      running_.valid_ = false;
      return;
    }
    running_.total_ = *total;
    running_.bound_ = *bound;
  }

  // Brings lot `at`'s counted value up to date.
  void retrack(std::size_t at) noexcept {
    const auto& lot = positions_[at];
    std::optional<Decimal64> value;
    if (lot.quantity_.coefficient() != 0 && lot.market_value_.currency_ == own_) {
      value = lot.market_value_.amount_;
    }
    auto& counted = lots_[at].counted_;
    if (counted == value) {
      return;
    }
    if (counted.has_value() != value.has_value()) {
      running_.counted_ = value ? running_.counted_ + 1 : running_.counted_ - 1;
    }
    if (running_.valid_) {
      replace_counted(counted, value);
    }
    counted = value;
    if (!running_.valid_) {
      recount();
    }
  }

  // Brings the counted cash up to date.
  void retrack_cash(const Decimal64& cash) noexcept {
    if (running_.valid_) {
      replace_counted(running_.cash_, cash);
    }
    running_.cash_ = cash;
    if (!running_.valid_) {
      recount();
    }
  }

  // Leaves a ledger whose contents were moved out consistent with what it now holds (no lots, and whatever
  // cash a moved-from vector keeps): nothing listed as changed, the running equity counted again.
  void forget_tracking() noexcept {
    lots_.clear();
    changed_.clear();
    changed_count_ = 0;
    recount();
  }

  // Counts everything again: at construction, and after an update that did not fit.
  void recount() noexcept {
    Running fresh;
    fresh.cash_ = cash_in(own_).value_or(Decimal64(0, 0));
    fresh.total_ = fresh.cash_;
    fresh.scale_ = fresh.cash_.scale();
    const auto cash_size = magnitude(fresh.cash_);
    fresh.valid_ = cash_size.has_value();
    if (cash_size) {
      fresh.bound_ = *cash_size;
    }
    for (const auto at : held_) {
      const auto& value = lots_[at].counted_;
      if (!value) {
        continue;
      }
      ++fresh.counted_;
      fresh.scale_ = std::max(fresh.scale_, value->scale());
      if (!fresh.valid_) {
        continue;
      }
      const auto size = magnitude(*value);
      const auto total = decimal_add(fresh.total_, *value);
      const auto bound = size ? decimal_add(fresh.bound_, *size) : std::nullopt;
      if (!total || !bound) {
        fresh.valid_ = false;
        continue;
      }
      fresh.total_ = *total;
      fresh.bound_ = *bound;
    }
    running_ = fresh;
  }
};

} // namespace backtest
} // namespace algotradelib
