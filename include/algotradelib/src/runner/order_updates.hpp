#pragma once

#include <algorithm>
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
 * **Not `operator==`, which compares the coefficient *and the scale*** (`core/types.hpp:347-349`),
 * so `Decimal64(100, 1)` and `Decimal64(10, 0)` are distinct values although both are ten. Two
 * readings of one order that differ only in how a quantity is written are not a change, and a
 * detector comparing with `==` would report an update every listing for as long as they differed.
 *
 * **Where that can actually arise, stated precisely rather than as a worry.** It is *not* the
 * shipped adapters' string parsing: `Decimal64::parse` strips trailing fractional zeros, so
 * `parse("10")` and `parse("10.0")` are the same value and no wire spelling reaches here as a
 * scale difference. It is a value built at an explicit scale -- the two-argument constructor and
 * `analytics::to_decimal(value, scale)`, which `Decimal64`'s own comment already calls out as not
 * normalized -- which is what an adapter this library has never seen may well use.
 *
 * So the comparison is numeric, via the ordering that already aligns scales. The cost is one extra
 * comparison; the alternative is a firehose on an adapter nobody here can inspect.
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
    same_number(lhs.trigger_price_, rhs.trigger_price_) &&
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
 * `status_`, `execution_.filled_quantity_`, `price_`, `trigger_price_`, `quantity_`,
 * `order_amount_` and `canceled_at_`. Those are the lifecycle, execution progress, and the three
 * things an amendment moves -- a stop order's trigger can be amended on its own, at the same limit
 * and quantity, and a strategy holding a stop must hear about it. A row whose readings all match
 * its predecessor is not reported.
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
   *
   * ## Precondition: `rows` is a complete listing
   *
   * Every call must carry **every** order the broker would list, not one page of them. This class
   * cannot tell a complete listing from a partial one -- a vector of rows looks the same either way
   * -- and the consequence of breaking it is quiet: pages alternating between polls would forget and
   * re-report the same orders indefinitely, and `live::SessionSource`'s disappearance counter beside
   * it would rise for orders that never went anywhere.
   *
   * `WorkingOrders::orders_` promises exactly this, and all three shipped adapters keep it: KIS and
   * Kiwoom assemble every page before completing and fail an incomplete walk, and Toss's `OPEN`
   * listing is unpaged. An adapter this library has never seen that answers one page per call would
   * break it, which is why the requirement is written here rather than assumed.
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

  /**
   * Returns how many times a listing stated one order twice with readings that disagreed.
   *
   * Monotone. Non-zero means some orders went unreported and says how often, which is the witness
   * for a rule this class can detect and cannot repair: a broker contradicting itself about one
   * order leaves nothing here that could choose between the two readings.
   */
  std::size_t contradicted_rows() const noexcept { return contradicted_; }

private:
  /** One order's last observed state, against which the next listing is compared. */
  struct Tracked {
    OrderRef<Broker> ref_;
    OrderSnapshot last_;
    /** Set when this listing stated the same order twice and the two readings disagreed. */
    bool contradicted_{false};
  };

  /**
   * Runs one observation, reporting differences or silently adopting them.
   *
   * Two passes, and the split is what keeps the duplicate rule honest. The first reduces the
   * listing to one entry per reference; only then does the second decide what changed, so a
   * contradiction found on the listing's last row still suppresses a report the first row would
   * otherwise have earned.
   */
  std::vector<OrderSnapshot> observe(
    const std::vector<WorkingOrder<Broker>>& rows, bool report
  ) {
    std::vector<Tracked> next;
    next.reserve(rows.size());

    for (const auto& row : rows) {
      // This library's own arithmetic rather than something a broker stated, and the contract is
      // about what a broker stated. Neither caller passes one today.
      if (row.reserved_) {
        continue;
      }
      if (row.ref_.primary_.empty() && row.ref_.secondary_.empty()) {
        ++untracked_;
        continue;
      }
      auto* already = seen(next, row.ref_);
      if (already == nullptr) {
        next.push_back(Tracked{row.ref_, row.order_, false});
        continue;
      }
      // **One reference, one entry, whatever the listing says.** Reporting a duplicated row twice
      // would hand a strategy one change as two events, and a strategy answering an update may
      // submit -- so a duplicated row becomes a duplicated order. Both brokers that page this
      // listing append their pages without deduplicating, so the input is not excluded upstream.
      if (!order_update_detail::same_observable_state(already->last_, row.order_)) {
        // **Two readings of one order that disagree cannot both be true, and nothing here can say
        // which is.** Reporting either states something unestablished; reporting both hands a
        // strategy a contradiction as two events. So this order is reported not at all from this
        // listing, and the disagreement is counted. That is the answer slice 27 reached for two
        // sellable figures that contradicted each other, for the same reason.
        already->contradicted_ = true;
      }
    }

    std::vector<OrderSnapshot> updates;
    for (const auto& entry : next) {
      if (entry.contradicted_) {
        ++contradicted_;
        continue;
      }
      const auto* held = find(entry.ref_);
      const bool differs =
        held == nullptr || !order_update_detail::same_observable_state(held->last_, entry.last_);
      if (report && differs) {
        updates.push_back(entry.last_);
      }
    }

    // A contradicted order keeps no baseline, because there is no reading of it this listing could
    // settle on. The next listing that states it once reports it afresh.
    next.erase(
      std::remove_if(
        next.begin(),
        next.end(),
        [](const Tracked& entry) { return entry.contradicted_; }
      ),
      next.end()
    );
    tracked_ = std::move(next);
    return updates;
  }

  /** Returns the entry already built for `ref` in this listing, or `nullptr`. */
  static Tracked* seen(std::vector<Tracked>& built, const OrderRef<Broker>& ref) {
    for (auto& entry : built) {
      if (entry.ref_ == ref) {
        return &entry;
      }
    }
    return nullptr;
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
  std::size_t contradicted_{0};
};

} // namespace runner
} // namespace algotradelib
