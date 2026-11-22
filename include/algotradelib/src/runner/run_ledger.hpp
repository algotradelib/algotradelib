#pragma once

#include "./journal.hpp"

#include <algotradelib/core>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/** @brief Where a run's placement stands in its ledger (`Runner::run_unsettled`). */
enum class RunPlacementState {
  /** @brief Held for an operator (`JournalOutcome::HELD`): what it filled is not known. */
  HELD,
  /** @brief Accounted for without a booking (released by an operator, or confirmed by a runner without a budget). */
  UNBOOKED,
  /** @brief Booked: its fill, if any, moved the run's cash and shares. */
  BOOKED,
};

/** @brief A placement of a run, as its ledger knows it. */
struct RunPlacement {
  /** @brief The placement's journal id. */
  std::string id_;
  /** @brief The instrument. */
  InstrumentId instrument_;
  /** @brief The side. */
  OrderSide side_{OrderSide::BUY};
  /** @brief The quantity it was sent for. */
  Decimal64 quantity_;
  /** @brief Where it stands. */
  RunPlacementState state_{RunPlacementState::HELD};
};

/** @brief Cash an operator adds to a run or takes from it, once (`RunOpening::transfers_`). */
struct RunTransfer {
  /** @brief The operator's id for it; a transfer whose id the run already holds is not applied again. */
  std::string id_;
  /** @brief How much, in the run's currency; positive. */
  Money amount_;
  /** @brief Whether it is taken from the run (a withdrawal) rather than added (a deposit). */
  bool withdrawal_{false};
};

/**
 * @brief How a budgeted runner opens its run when `runner::recover_runner` reads its journal
 *   (`Runner::set_run_opening`).
 *
 * By default a new run opens with `strategy::RiskLimits::run_budget_`, labelled by the library. If the journal
 * holds an earlier run, it is closed first: its open orders are cancelled (or waited for), each is accounted
 * for and booked to it as usual, and only then is the new run's opening written. The old run's remaining shares
 * are the account's, not the new run's.
 */
struct RunOpening {
  /**
   * @brief Resume the journal's latest run instead of opening a new one.
   *
   * On an intact journal with no run nothing opens (`RunRecoverySummary::refused_` says so): the run's buys and
   * sells are refused, and withdrawals go as without a budget.
   */
  bool resume_{false};
  /** @brief For a new run: wait for the old run's open orders to end, instead of cancelling them. */
  bool wait_for_open_orders_{false};
  /** @brief Deposits and withdrawals for a resumed run, each applied once by its id, in order. */
  std::vector<RunTransfer> transfers_;
};

/** @brief Why recovery opened no run (`RunRecoverySummary::refusal_`). */
enum class RunRefusal {
  /** @brief None: the run opened, or a new run waits for the old run to close. */
  NONE,
  /**
   * @brief A resume found an intact journal with no run. Not damage, and no halt: the run's buys and sells are
   *   refused, and withdrawals go as without a budget.
   */
  NO_RUN,
  /** @brief The journal was not read whole, or holds a damaged line. */
  DAMAGED,
  /** @brief The run's records do not add up (a repeated opening, a double booking, an invalid booking or transfer). */
  LEDGER,
  /** @brief The journal's run is in another currency than `strategy::RiskLimits::run_budget_`. */
  CURRENCY,
  /** @brief A stated transfer was refused (an id reused with another amount, a withdrawal past free cash, ...). */
  TRANSFER,
  /** @brief A line of the opening could not be written. */
  WRITE,
};

/** @brief How recovery opened a budgeted runner's run, or why it did not (`RecoveryReport::run_`). */
struct RunRecoverySummary {
  /** @brief Whether the run is open. */
  bool opened_{false};
  /**
   * @brief Whether a new run waits for the old run's open orders to be accounted for: not opened yet
   *   (`Runner::run_recovery` says when it is).
   */
  bool closing_{false};
  /** @brief Whether a run found in the journal was resumed; `false` for a new run. */
  bool resumed_{false};
  /** @brief The run's id. */
  std::string id_;
  /** @brief The run's cash once open. */
  std::optional<Money> cash_;
  /** @brief How many transfers were written now. */
  std::size_t transfers_applied_{0};
  /** @brief Whether `strategy::RiskLimits::run_budget_` was used: only a new run opens with it. */
  bool budget_used_{false};
  /**
   * @brief Why the run did not open, by kind; `RunRefusal::NONE` when it did. Every kind but `NO_RUN` halts the
   *   runner (`LOCAL_FAILURE`) and stops the journal's writes.
   */
  RunRefusal refusal_{RunRefusal::NONE};
  /** @brief Why the run did not open, for the operator; empty when it did. */
  std::string refused_;
};

template <typename Strategy, typename CapsType, typename BrokerTag, typename Executor>
class Runner;

namespace run_detail {
struct Fold;
} // namespace run_detail

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
   * @brief Returns the run's id.
   * @return The id of its opening line; empty for a run kept only in memory.
   */
  const std::string& id() const noexcept {
    return id_;
  }

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
    const auto next = booked(instrument, side, filled, amount, fee);
    if (!next) {
      return false;
    }
    const auto at = find(instrument);
    if (at) {
      shares_[*at].quantity_ = next->second;
    } else {
      add(instrument, next->second);
    }
    cash_.amount_ = next->first;
    return true;
  }

private:
  // `Runner` alone keeps a journal's run: it opens it, folds it, and moves its placements.
  template <typename, typename, typename, typename>
  friend class Runner;

  // Opens a new run recorded in a journal.
  RunLedger(std::string id, Money opening) : id_(std::move(id)), cash_(std::move(opening)) {}

  // Reads a journal's run: its latest `RunRecordKind::OPEN` record and every line of that run after it.
  static run_detail::Fold fold(const JournalReplay& replay);

  // Tests whether every placement of the run is booked or still owed: none held or released unbooked.
  bool settled() const noexcept {
    return unsettled_ == 0;
  }

  // Tests whether the run's shares of an instrument are known.
  bool settled_on(const InstrumentId& instrument) const {
    if (unsettled_ == 0) {
      return true;
    }
    for (const auto& placement : placements_) {
      if (placement.state_ != RunPlacementState::BOOKED && placement.instrument_ == instrument) {
        return false;
      }
    }
    return true;
  }

  // Lists the placements held for an operator or released without a booking.
  std::vector<RunPlacement> unsettled() const {
    std::vector<RunPlacement> found;
    if (unsettled_ == 0) {
      return found;
    }
    for (const auto& placement : placements_) {
      if (placement.state_ != RunPlacementState::BOOKED) {
        found.push_back(placement);
      }
    }
    return found;
  }

  // Returns a placement of the run.
  const RunPlacement* placement(const std::string& id) const {
    const auto at = placement_at_.find(id);
    return at == placement_at_.end() ? nullptr : &placements_[at->second];
  }

  // Returns a transfer the run already holds.
  const RunTransfer* transfer_of(const std::string& id) const {
    for (const auto& transfer : transfers_) {
      if (transfer.id_ == id) {
        return &transfer;
      }
    }
    return nullptr;
  }

  // Applies a deposit or a withdrawal; `Runner` refuses a withdrawal past the cash before it is written.
  bool transfer(const RunTransfer& transfer) {
    if (
      transfer.id_.empty() || transfer_of(transfer.id_) != nullptr ||
      !(transfer.amount_.currency_ == cash_.currency_) || !decimal_less(Decimal64::parse("0"), transfer.amount_.amount_)
    ) {
      return false;
    }
    const auto cash = transfer.withdrawal_ ?
      decimal_subtract(cash_.amount_, transfer.amount_.amount_) :
      decimal_add(cash_.amount_, transfer.amount_.amount_);
    if (!cash) {
      return false;
    }
    transfers_.push_back(transfer);
    cash_.amount_ = *cash;
    return true;
  }

  // Records a placement held for an operator.
  bool hold(const RunPlacement& placement) {
    return settle_state(placement, RunPlacementState::HELD);
  }

  // Records a placement accounted for without a booking, until `book_released` books it.
  bool release(const RunPlacement& placement) {
    return settle_state(placement, RunPlacementState::UNBOOKED);
  }

  // Tests whether `book_placement` would book a placement.
  bool can_book_placement(
    const RunPlacement& placement, const Decimal64& filled, const Money& amount, const Decimal64& fee
  ) const {
    const auto* known = this->placement(placement.id_);
    return (known == nullptr || known->state_ == RunPlacementState::HELD) &&
      !decimal_less(placement.quantity_, filled) &&
      booked(placement.instrument_, placement.side_, filled, amount, fee).has_value();
  }

  // Books a placement's fill as its `CONFIRMED` line does.
  bool book_placement(
    const RunPlacement& placement, const Decimal64& filled, const Money& amount, const Decimal64& fee
  ) {
    if (!can_book_placement(placement, filled, amount, fee)) {
      return false;
    }
    book(placement.instrument_, placement.side_, filled, amount, fee);
    settle_state(placement, RunPlacementState::BOOKED);
    return true;
  }

  // Tests whether `book_released` would book a placement released without a booking.
  bool can_book_released(
    const std::string& id, const Decimal64& filled, const Money& amount, const Decimal64& fee
  ) const {
    const auto* known = placement(id);
    return known != nullptr && known->state_ == RunPlacementState::UNBOOKED &&
      !decimal_less(known->quantity_, filled) &&
      booked(known->instrument_, known->side_, filled, amount, fee).has_value();
  }

  // Books a placement released without a booking, as a `RunRecordKind::FILL` record does.
  bool book_released(const std::string& id, const Decimal64& filled, const Money& amount, const Decimal64& fee) {
    if (!can_book_released(id, filled, amount, fee)) {
      return false;
    }
    const auto released = *placement(id);
    book(released.instrument_, released.side_, filled, amount, fee);
    return settle_state(released, RunPlacementState::BOOKED);
  }

  // The cash and the instrument's shares after a fill, or nothing if `book` refuses it.
  std::optional<std::pair<Decimal64, Decimal64>> booked(
    const InstrumentId& instrument, OrderSide side, const Decimal64& filled, const Money& amount, const Decimal64& fee
  ) const {
    const auto zero = Decimal64::parse("0");
    if (
      !(amount.currency_ == cash_.currency_) || decimal_less(filled, zero) || decimal_less(amount.amount_, zero) ||
      decimal_less(fee, zero)
    ) {
      return std::nullopt;
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
        return std::nullopt;
      }
      shares = decimal_subtract(held, filled);
      const auto returned = decimal_subtract(amount.amount_, fee);
      cash = returned ? decimal_add(cash_.amount_, *returned) : std::nullopt;
    }
    if (!shares || !cash) {
      return std::nullopt;
    }
    return std::make_pair(*cash, *shares);
  }

  // Moves a placement to `state`, recording it if new; refused once it is booked (a released one is booked only by
  // `book_released`).
  bool settle_state(const RunPlacement& placement, RunPlacementState state) {
    const auto at = placement_at_.find(placement.id_);
    if (at == placement_at_.end()) {
      auto added = placement;
      added.state_ = state;
      placement_at_.emplace(added.id_, placements_.size());
      placements_.push_back(std::move(added));
      unsettled_ += state != RunPlacementState::BOOKED ? 1 : 0;
      return true;
    }
    auto& known = placements_[at->second];
    if (known.state_ == RunPlacementState::BOOKED) {
      return false;
    }
    // Leaving `HELD` or `UNBOOKED` for `BOOKED` settles it.
    if (state == RunPlacementState::BOOKED) {
      --unsettled_;
    }
    known.state_ = state;
    return true;
  }

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

  std::string id_;
  Money cash_;
  std::vector<Shares> shares_;
  id_detail::PositionTable table_;
  std::vector<RunTransfer> transfers_;
  std::vector<RunPlacement> placements_;
  std::unordered_map<std::string, std::size_t> placement_at_;
  // How many placements are `HELD` or `UNBOOKED`.
  std::size_t unsettled_{0};
};

namespace run_detail {

// A journal's run as `RunLedger::fold` read it.
struct Fold {
  // The run, or none if the journal holds no opening (or `failure_` is set).
  std::optional<RunLedger> run_;
  // Why the run's records do not add up; empty when they do. Recovery then halts.
  std::string failure_;
};

} // namespace run_detail

inline run_detail::Fold RunLedger::fold(const JournalReplay& replay) {
  run_detail::Fold folded;
  std::optional<std::size_t> opened;
  for (std::size_t at = 0; at < replay.entries_.size(); ++at) {
    const auto& entry = replay.entries_[at];
    if (entry.action_ == JournalAction::LEDGER && entry.run_ && entry.run_->kind_ == RunRecordKind::OPEN) {
      opened = at;
    }
  }
  if (!opened) {
    return folded;
  }
  const auto& open = *replay.entries_[*opened].run_;
  if (!open.amount_ || open.run_ != replay.entries_[*opened].id_) {
    folded.failure_ = "the run's opening line " + replay.entries_[*opened].id_ + " is incomplete";
    return folded;
  }
  // A line of the run before its latest opening: the opening repeated, which would restart the run at its budget.
  for (std::size_t at = 0; at < *opened; ++at) {
    if (replay.entries_[at].run_ && replay.entries_[at].run_->run_ == open.run_) {
      folded.failure_ = "the run's opening line " + open.run_ + " is repeated";
      return folded;
    }
  }
  RunLedger run(open.run_, *open.amount_);
  const auto fail = [&folded](const JournalEntry& entry, const char* why) {
    folded.failure_ = "the run's journal line " + entry.id_ + " " + why;
    return std::move(folded);
  };
  for (std::size_t at = *opened + 1; at < replay.entries_.size(); ++at) {
    const auto& entry = replay.entries_[at];
    if (!entry.run_ || entry.run_->run_ != run.id_) {
      continue;
    }
    const auto& record = *entry.run_;
    const RunPlacement placement{entry.id_, entry.instrument_, entry.side_, entry.quantity_, RunPlacementState::HELD};
    // What the file reader guarantees, checked again for a journal held in memory.
    const bool whole = entry.action_ == JournalAction::LEDGER ?
      record.kind_ && record.amount_ && (*record.kind_ != RunRecordKind::FILL || (record.filled_ && record.fee_)) :
      !record.filled_ || (record.amount_ && record.fee_);
    if (!whole) {
      return fail(entry, "is incomplete");
    }
    if (entry.action_ == JournalAction::LEDGER) {
      if (*record.kind_ == RunRecordKind::OPEN) {
        return fail(entry, "opens its run a second time");
      }
      if (*record.kind_ == RunRecordKind::FILL) {
        if (!run.book_released(record.of_, *record.filled_, *record.amount_, *record.fee_)) {
          return fail(entry, "books a placement that was not released unbooked, or a fill it cannot take");
        }
      } else if (
        !run.transfer(RunTransfer{record.given_, *record.amount_, *record.kind_ == RunRecordKind::WITHDRAWAL})
      ) {
        return fail(entry, "repeats a transfer id, or is in another currency, or does not fit");
      }
      continue;
    }
    if (entry.action_ != JournalAction::PLACE) {
      continue;
    }
    if (entry.outcome_ == JournalOutcome::HELD) {
      if (!run.hold(placement)) {
        return fail(entry, "holds a placement already booked");
      }
    } else if (entry.outcome_ == JournalOutcome::CONFIRMED) {
      const bool booked = record.filled_.has_value();
      if (
        booked ?
          !run.book_placement(placement, *record.filled_, *record.amount_, *record.fee_) :
          !run.release(placement)
      ) {
        return fail(entry, "accounts for a placement twice, or books a fill the run cannot take");
      }
    }
  }
  folded.run_ = std::move(run);
  return folded;
}

} // namespace runner
} // namespace algotradelib
