#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
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
 * excluded, so a change to them alone produces no update.
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
 * @brief Turns successive working-order listings into order updates: the rows that changed since
 *   the previous listing.
 *
 * `Runner` feeds each update to `on_order_update`; a live loop and a backtest both use it.
 *
 * **What a caller is told:** what the broker last stated about an order it still lists. An order
 * that leaves the listing produces nothing, because the listing does not say whether it filled,
 * was cancelled, expired or was rejected. Whether a listing can show a terminal status at all is
 * `BrokerCaps::working_orders_state_terminal` (`false` for every shipped broker).
 *
 * **A change** is a difference in `status_`, `execution_.filled_quantity_`, `price_`,
 * `trigger_price_`, `quantity_`, `order_amount_` or `canceled_at_`, compared as numbers whatever
 * their decimal scale. Identity fields and fill money (average price, amounts, fees, fill time) are
 * not compared; read them off the row. A new row is reported; two `UNKNOWN` statuses in a row are
 * not a change.
 *
 * Rows are matched by `OrderRef` (both parts). A row with an empty reference is counted in
 * `untracked_rows()` and never reported; a runner's reservation row (`WorkingOrder::reserved_`) is
 * skipped.
 *
 * @note In a live loop the update arrives during the working-order poll, before that iteration's
 *   holdings read, so a `TargetPosition` stated in `on_order_update` is withheld as
 *   `PORTFOLIO_SUPERSEDED` and planned after the holdings read; an `OrderIntent` stated there is
 *   dropped.
 *
 * @tparam Broker The broker tag.
 */
template <typename Broker>
class OrderUpdateWatch {
public:
  /**
   * @brief Compares a listing with the previous one and returns the rows that changed.
   *
   * A row not seen before is returned. Rows missing from `rows` are forgotten, so an order that
   * leaves and returns is reported again as new. An order listed twice with disagreeing readings is
   * not reported and is counted in `contradicted_rows()`.
   *
   * @param rows The **complete** listing, every page; a partial listing makes orders appear and
   *   disappear. The shipped adapters return complete listings.
   * @return The changed rows' snapshots.
   */
  std::vector<OrderSnapshot> changed(const std::vector<WorkingOrder<Broker>>& rows) {
    return observe(rows, /*report=*/true);
  }

  /**
   * @brief Takes a listing as the baseline without reporting anything (such as the startup read,
   *   which appears in `Portfolio::working_` instead).
   * @param rows The complete listing.
   */
  void adopt(const std::vector<WorkingOrder<Broker>>& rows) {
    observe(rows, /*report=*/false);
  }

  /**
   * @brief Returns how many rows had an empty reference, and so were never reported.
   * @return The count of such rows over the watch's life (per sighting).
   */
  std::size_t untracked_rows() const noexcept { return untracked_; }

  /**
   * @brief Returns how many orders the watch is tracking.
   * @return The number of orders kept from the last listing (not counting reserved rows, rows with
   *   an empty reference, or orders the listing contradicted).
   */
  std::size_t tracked_orders() const noexcept { return tracked_.size(); }

  /**
   * @brief Returns how many times a listing stated one order twice with disagreeing readings (and
   *   so it was not reported).
   * @return The count over the watch's life.
   */
  std::size_t contradicted_rows() const noexcept { return contradicted_; }

private:
  // One order's last observed state, against which the next listing is compared.
  struct Tracked {
    OrderRef<Broker> ref_;
    OrderSnapshot last_;
    // Set when this listing stated the same order twice and the two readings disagreed.
    bool contradicted_{false};
  };

  // Runs one observation, reporting differences or silently adopting them.
  //
  // Two passes, and the split is what keeps the duplicate rule honest. The first reduces the
  // listing to one entry per reference; only then does the second decide what changed, so a
  // contradiction found on the listing's last row still suppresses a report the first row would
  // otherwise have earned.
  std::vector<OrderSnapshot> observe(
    const std::vector<WorkingOrder<Broker>>& rows, bool report
  ) {
    std::vector<Tracked> next;
    next.reserve(rows.size());
    // Where each reference sits, in this listing (`next`) and in the previous one (`tracked_`), so a
    // long listing is read once and not once per row. Both are looked up and never iterated: the
    // order of `next`, and so of the updates, is the listing's. Built here from the vectors each
    // time, so nothing has to be kept in step with `tracked_`; a reference maps to the first entry
    // that has it, which is the one a scan would find.
    std::unordered_map<OrderRef<Broker>, std::size_t> built;
    built.reserve(rows.size());
    std::unordered_map<OrderRef<Broker>, std::size_t> held_at;
    held_at.reserve(tracked_.size());
    for (std::size_t at = 0; at < tracked_.size(); ++at) {
      held_at.emplace(tracked_[at].ref_, at);
    }

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
      const auto [at, first] = built.try_emplace(row.ref_, next.size());
      if (first) {
        next.push_back(Tracked{row.ref_, row.order_, false});
        continue;
      }
      // One reference, one entry: a duplicated row must not become two events (and two orders).
      auto& already = next[at->second];
      if (!order_update_detail::same_observable_state(already.last_, row.order_)) {
        // Two readings that disagree: report neither, and count it.
        already.contradicted_ = true;
      }
    }

    std::vector<OrderSnapshot> updates;
    for (const auto& entry : next) {
      if (entry.contradicted_) {
        ++contradicted_;
        continue;
      }
      const auto found = held_at.find(entry.ref_);
      const Tracked* held = found == held_at.end() ? nullptr : &tracked_[found->second];
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

  std::vector<Tracked> tracked_;
  std::size_t untracked_{0};
  std::size_t contradicted_{0};
};

} // namespace runner
} // namespace algotradelib
