#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "../core/types.hpp"

namespace algotradelib {
namespace runner {

namespace order_update_detail {

/**
 * Returns whether two decimals are the same number, at whatever scale each was written.
 *
 * **Not `operator==`, and the difference is the whole reason this exists.** `Decimal64::operator==`
 * compares the coefficient *and the scale*, so `parse("10")` and `parse("10.0")` are distinct
 * values (`core/types.hpp:347-349`). A broker that restates one quantity at a different scale
 * between two listings has not changed the order, and a detector comparing with `==` would report
 * an update every poll for as long as it kept doing so -- a firehose driven by a wire format's
 * punctuation.
 *
 * So the comparison is numeric, via the ordering that already aligns scales.
 */
inline bool same_number(const Decimal64& lhs, const Decimal64& rhs) {
  return !decimal_less(lhs, rhs) && !decimal_less(rhs, lhs);
}

/** Returns whether two optional decimals are both absent, or both the same number. */
inline bool same_number(
  const std::optional<Decimal64>& lhs, const std::optional<Decimal64>& rhs
) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  return !lhs.has_value() || same_number(*lhs, *rhs);
}

/**
 * Returns whether two readings of one order describe the same observable state.
 *
 * The fields compared are the ones a broker moves while an order is outstanding. Everything else
 * on an `OrderSnapshot` identifies the order or describes money attached to a fill, and both are
 * excluded deliberately -- see `OrderUpdateWatch` for the argument, which is a design decision
 * rather than an implementation detail.
 */
inline bool same_observable_state(const OrderSnapshot& lhs, const OrderSnapshot& rhs) {
  return lhs.status_ == rhs.status_ &&
    same_number(lhs.execution_.filled_quantity_, rhs.execution_.filled_quantity_) &&
    same_number(lhs.price_, rhs.price_) &&
    same_number(lhs.quantity_, rhs.quantity_) &&
    same_number(lhs.order_amount_, rhs.order_amount_) &&
    lhs.canceled_at_ == rhs.canceled_at_;
}

} // namespace order_update_detail

/**
 * Reports which orders in a listing have changed since the last listing this watch was shown.
 *
 * ## What it is for
 *
 * `Runner::feed(const OrderSnapshot&)` dispatches `on_order_update`, one of the seven handlers a
 * strategy may define. Nothing in this library fed it before slice 28, in either a replay or a live
 * run, so a documented extension point could not fire. Both modes already hold the rows: a live
 * loop sweeps them from the working-order inquiry, and a replay builds the same shape for
 * `Portfolio::working_`. This turns those listings into events, and it is the only producer.
 *
 * ## The contract, which is narrower than "you will see your order's lifecycle"
 *
 * **A caller is told what the broker last stated about an order the broker is still listing. It is
 * never told why an order stopped being listed.**
 *
 * A working-order listing shows what is resting. A row that leaves it may have filled, been
 * cancelled, expired or been rejected, and the listing does not say which -- inferring one is the
 * design `plan/todo/live-fill-inference.md` was parked over after thirteen verified review
 * blockers, because discrete executions cannot be recovered from periodic cumulative snapshots.
 * So a disappearance produces nothing here. `live::SessionSource` counts disappearances separately
 * (`ChargedCosts::working_list_disappearances_`) and that counter, too, says nothing about why.
 *
 * Whether a *terminal* state is ever reported is therefore a fact about each broker's inquiry
 * rather than a property of this class, and it is published as
 * `BrokerCaps<Client>::working_orders_state_terminal` so a strategy can gate on it instead of
 * discovering it.
 *
 * ## What counts as a change
 *
 * `status_`, `execution_.filled_quantity_`, `price_`, `quantity_`, `order_amount_` and
 * `canceled_at_`. Those are the lifecycle, execution progress, and the two things an amendment
 * moves. A row whose readings all match its predecessor is not reported.
 *
 * Not compared: `name_`, `shape_code_`, `instrument_`, `side_`, `currency_` and `ordered_at_`,
 * which identify the order rather than describe its progress; and the monetary figures a fill
 * carries -- an average price, a filled amount, a commission, a tax and a fill time, all of which
 * Toss publishes. Each of those moves *with* `filled_quantity_`, so comparing them would add no
 * event and five further chances to fire on a broker restating a figure. **An order update means
 * the instruction or its execution progress changed, not that a number attached to it was
 * restated**; a caller wanting the money reads it off the row it is handed, which carries all of it.
 *
 * `OrderStatus::UNKNOWN` is a value and not an absence: a broker publishing no status column
 * reports `UNKNOWN` every listing, and two consecutive `UNKNOWN`s are not a change. This class
 * reports a *difference between two observations* and never draws a conclusion about the order.
 *
 * ## Identity
 *
 * Keyed on `OrderRef<Broker>`, never on `OrderSnapshot::order_id_`. The reference carries
 * `primary_` **and** `secondary_`, and KIS puts its branch code in the second -- the part an
 * amendment must address. `order_id_` is the order number alone, so keying on it would compare two
 * rows that share a number across branches against each other, and the failure would be a
 * *suppressed* update rather than a loud one.
 *
 * A row whose reference is empty in both parts cannot be compared with a predecessor. It is counted
 * in `untracked_rows()` and reported to nobody, rather than reported every listing or dropped in
 * silence.
 *
 * A row marked `reserved_` is this library's own arithmetic rather than something a broker said
 * (`WorkingOrder::reserved_`), so it is skipped: neither caller passes one today, and the contract
 * above is about what a broker stated.
 *
 * ## Cost
 *
 * Matching is a linear scan, because `OrderRef` defines equality and no ordering and adding one to
 * a core value type to serve this would be the wrong direction. A working listing is tens of rows.
 */
template <typename Broker>
class OrderUpdateWatch {
public:
  /**
   * Returns the rows whose observable state differs from the last listing shown to this watch.
   *
   * A row never seen before is returned, which is how an order becoming resting is reported.
   * Tracked rows absent from `rows` are forgotten, so the watch stays the size of the listing --
   * and a row that disappears and later returns is reported again as new, which restates something
   * the caller knows rather than claiming something it does not.
   */
  std::vector<OrderSnapshot> changed(const std::vector<WorkingOrder<Broker>>& rows) {
    return observe(rows, /*report=*/true);
  }

  /**
   * Takes `rows` as the baseline without reporting any of them.
   *
   * Used where a listing is read but an event would be wrong or unsafe: the live loop's startup
   * read runs inside `Loop::gate_for` *before* the startup hold is installed, and dispatching to a
   * strategy there would let it submit against a picture the gate exists to withhold. The listing
   * is state rather than an event in any case -- it is installed on `Portfolio::working_`, which is
   * where a strategy reads what a predecessor left resting.
   */
  void adopt(const std::vector<WorkingOrder<Broker>>& rows) {
    observe(rows, /*report=*/false);
  }

  /**
   * Returns how many rows this watch has been shown that carry no reference at all.
   *
   * Monotone across the life of the watch, and counted per sighting rather than per order, because
   * a row with no identity cannot be told apart from another one. Non-zero means some orders are
   * not being reported and says how often, which is the witness for a rule this class cannot
   * enforce.
   */
  std::size_t untracked_rows() const noexcept { return untracked_; }

  /** Returns how many orders this watch is currently tracking. */
  std::size_t tracked_orders() const noexcept { return tracked_.size(); }

private:
  /** One order's last observed state, against which the next listing is compared. */
  struct Tracked {
    OrderRef<Broker> ref_;
    OrderSnapshot last_;
  };

  /** Runs one observation, reporting differences or silently adopting them. */
  std::vector<OrderSnapshot> observe(
    const std::vector<WorkingOrder<Broker>>& rows, bool report
  ) {
    std::vector<OrderSnapshot> updates;
    std::vector<Tracked> next;
    next.reserve(rows.size());

    for (const auto& row : rows) {
      if (row.reserved_) {
        continue;
      }
      if (row.ref_.primary_.empty() && row.ref_.secondary_.empty()) {
        ++untracked_;
        continue;
      }
      // Found among what was tracked before this listing, not among what this listing has already
      // contributed: a listing carrying one order twice must not compare its second row against its
      // first and call the pair a change.
      const auto* held = find(row.ref_);
      const bool differs =
        held == nullptr || !order_update_detail::same_observable_state(held->last_, row.order_);
      if (report && differs) {
        updates.push_back(row.order_);
      }
      next.push_back(Tracked{row.ref_, row.order_});
    }

    tracked_ = std::move(next);
    return updates;
  }

  /** Returns the tracked entry for `ref`, or `nullptr`. */
  const Tracked* find(const OrderRef<Broker>& ref) const {
    for (const auto& entry : tracked_) {
      if (entry.ref_ == ref) {
        return &entry;
      }
    }
    return nullptr;
  }

  std::vector<Tracked> tracked_;
  std::size_t untracked_{0};
};

} // namespace runner
} // namespace algotradelib
