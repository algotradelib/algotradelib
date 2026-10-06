#pragma once

#include "./portfolio.hpp"

#include <algotradelib/core>

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib {
namespace strategy {

/**
 * @brief Whether a broker's sellable quantity already excludes sell orders resting at the broker.
 *
 * No broker documents this. `RiskGuard` assumes it does not (it subtracts resting sells itself);
 * `SellableConventionProbe` infers the answer from ordinary polling.
 */
enum class SellableConvention {
  /** @brief Nothing conclusive has been observed yet. */
  UNDETERMINED,
  /** @brief The figure fell when a sell began resting: the broker excludes resting sells. */
  NET_OF_RESTING,
  /** @brief The figure did not move when a sell began resting: the broker includes them. */
  GROSS_OF_RESTING,
  /** @brief Both behaviours were observed; no single rule fits. */
  INCONSISTENT,
};

/**
 * @brief Returns a lowercase name for a convention, such as `"net_of_resting"`, for logs.
 * @param convention The convention.
 * @return The name.
 */
inline constexpr std::string_view sellable_convention_name(SellableConvention convention) noexcept {
  switch (convention) {
    case SellableConvention::UNDETERMINED:
      return "undetermined";
    case SellableConvention::NET_OF_RESTING:
      return "net_of_resting";
    case SellableConvention::GROSS_OF_RESTING:
      return "gross_of_resting";
    case SellableConvention::INCONSISTENT:
      return "inconsistent";
  }
  return "undetermined";
}

/**
 * @brief How many observations supported each answer.
 *
 * One observation is evidence, not proof (a settlement in the same window moves the same
 * number); look at the counts before acting on a verdict.
 */
struct SellableEvidence {
  /** @brief Observations where the figure fell by the quantity that began resting. */
  std::size_t net_{0};
  /** @brief Observations where the figure did not move when a sell began resting. */
  std::size_t gross_{0};

  /**
   * @brief Returns the convention the counts support.
   * @return `INCONSISTENT` if both counts are nonzero, `UNDETERMINED` if both are zero, otherwise
   *   the one with observations.
   */
  SellableConvention verdict() const noexcept {
    if (net_ > 0 && gross_ > 0) {
      return SellableConvention::INCONSISTENT;
    }
    if (net_ > 0) {
      return SellableConvention::NET_OF_RESTING;
    }
    if (gross_ > 0) {
      return SellableConvention::GROSS_OF_RESTING;
    }
    return SellableConvention::UNDETERMINED;
  }

  /**
   * @brief Returns the number of observations.
   * @return `net_ + gross_`.
   */
  std::size_t total() const noexcept { return net_ + gross_; }
};

/**
 * @brief Infers a broker's `SellableConvention` from successive portfolios.
 *
 * Call `observe` with each polled portfolio. An observation is recorded only when, between two
 * polls, an instrument's holding is unchanged, the resting sell quantity the broker reported grew,
 * and the gap between held and sellable moved by exactly that amount (`NET_OF_RESTING`) or not
 * at all (`GROSS_OF_RESTING`); anything else is discarded. Positions whose sellable basis is not
 * enforceable, or which carry no sellable figure, are skipped, so in a backtest or with a broker
 * that does not publish the figure the verdict stays `UNDETERMINED`.
 *
 * @tparam Broker The broker tag of the portfolio.
 */
template <typename Broker>
class SellableConventionProbe {
public:
  /**
   * @brief Records one polled portfolio, comparing it with the previous one.
   * @param portfolio The portfolio.
   */
  void observe(const Portfolio<Broker>& portfolio) {
    // **A figure of unknown coverage teaches nothing about resting orders, and would teach something
    // false.** This probe reads the *distance* between what is held and what is sellable and watches
    // how that distance moves. Both numbers have to describe the same shares for the distance to mean
    // anything: if `rmnd_qty` is account-wide while `trde_able_qty` covers one venue -- exactly what
    // `SellableBasis::Coverage::UNESTABLISHED` means at Kiwoom -- the distance is a venue split, and a
    // position moving between venues would read as a resting convention. So an unenforceable basis is
    // skipped whole rather than judged, and the verdict stays `UNDETERMINED`, which is true.
    for (const auto& position : portfolio.positions_) {
      const auto reading = portfolio.sellable_reading_for(position.instrument_);
      if (!reading.basis_.is_enforceable()) {
        // **Skipped, and the baseline is dropped with it.** Merely ignoring the reading would leave the
        // previous one pending, so the *next* enforceable reading would be compared against a
        // baseline from before the gap -- and the distance would appear to move by whatever happened
        // in between. A declared 100/100/0, an undeclared 100/70/0, then a declared 100/70/30 would
        // record a confident NET_OF_RESTING across a window the probe never observed.
        //
        // Resolved per instrument rather than per portfolio, because a per-instrument inquiry's basis
        // lands on its own row: checking only the snapshot's would discard every Toss reading.
        forget(position.instrument_);
        continue;
      }
      if (!position.sellable_quantity_) {
        // The broker did not publish it, so there is nothing to learn from this position.
        continue;
      }
      const auto resting = reported_resting(portfolio, position.instrument_);
      const Reading now{position.quantity_, *position.sellable_quantity_, resting};
      if (auto* previous = find(last_, position.instrument_)) {
        judge(position.instrument_, *previous, now);
        *previous = now;
        continue;
      }
      last_.push_back({position.instrument_, now});
    }
  }

  /**
   * @brief Discards the last recorded reading for an instrument, so the next one is not compared
   *   across a gap.
   * @param instrument The instrument.
   */
  void forget(const InstrumentId& instrument) {
    for (auto it = last_.begin(); it != last_.end(); ++it) {
      if (it->first == instrument) {
        last_.erase(it);
        return;
      }
    }
  }

  /**
   * @brief Returns the evidence for one instrument.
   * @param instrument The instrument.
   * @return Its counts, zero if nothing was observed.
   */
  SellableEvidence evidence(const InstrumentId& instrument) const {
    if (const auto* found = find(evidence_, instrument)) {
      return *found;
    }
    return SellableEvidence{};
  }

  /**
   * @brief Returns the evidence summed over every instrument.
   * @return The counts for the broker as a whole.
   */
  SellableEvidence overall() const {
    SellableEvidence total;
    for (const auto& entry : evidence_) {
      total.net_ += entry.second.net_;
      total.gross_ += entry.second.gross_;
    }
    return total;
  }

  /**
   * @brief Returns the broker's convention as observed so far.
   * @return `overall().verdict()`.
   */
  SellableConvention verdict() const { return overall().verdict(); }

  /**
   * @brief Returns the evidence per instrument.
   * @return Each instrument that contributed an observation, with its counts.
   */
  const std::vector<std::pair<InstrumentId, SellableEvidence>>& per_instrument() const noexcept {
    return evidence_;
  }

private:
  // Returns the sell quantity **the broker reported** resting, ignoring the runner's reservations.
  //
  // A runner adds a row of its own for an order it has just sent, so that the next order in the
  // pass cannot re-sell the same shares. Those rows are this library's arithmetic, and counting
  // them here would be circular: the resting quantity would rise with no change in the broker's
  // sellable figure, and the probe would record `GROSS_OF_RESTING` on evidence it manufactured
  // itself -- every time, for any broker, whenever the sellable check happened to be enabled.
  static Decimal64 reported_resting(
    const Portfolio<Broker>& portfolio,
    const InstrumentId& instrument
  ) {
    Decimal64 total = Decimal64::parse("0");
    for (const auto& order : portfolio.working_) {
      if (order.reserved_ || !(order.order_.instrument_ == instrument) ||
          order.order_.side_ != OrderSide::SELL) {
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
      if (const auto sum = decimal_add(total, *remaining)) {
        total = *sum;
      }
    }
    return total;
  }

  // One position as a poll reported it.
  struct Reading {
    Decimal64 held_;
    Decimal64 sellable_;
    Decimal64 resting_;
  };

  // Returns whether two decimals are numerically equal, across differing scales.
  static bool same(const Decimal64& lhs, const Decimal64& rhs) {
    // `operator==` compares the coefficient and the scale, so "70" and "70.0" are not equal by it.
    // A broker is free to change the scale it reports between two polls, and a probe that read that
    // as a change would conclude from noise.
    return !decimal_less(lhs, rhs) && !decimal_less(rhs, lhs);
  }

  // Records what the move from `before` to `now` says, if it says anything.
  void judge(const InstrumentId& instrument, const Reading& before, const Reading& now) {
    if (!same(before.held_, now.held_)) {
      return;
    }
    const auto grew = decimal_subtract(now.resting_, before.resting_);
    if (!grew || !decimal_less(Decimal64::parse("0"), *grew)) {
      return;
    }
    const auto gap_before = decimal_subtract(before.held_, before.sellable_);
    const auto gap_now = decimal_subtract(now.held_, now.sellable_);
    if (!gap_before || !gap_now) {
      return;
    }
    const auto moved = decimal_subtract(*gap_now, *gap_before);
    if (!moved) {
      return;
    }

    auto* entry = find(evidence_, instrument);
    if (entry == nullptr) {
      evidence_.push_back({instrument, SellableEvidence{}});
      entry = &evidence_.back().second;
    }
    if (same(*moved, *grew)) {
      ++entry->net_;
    } else if (same(*moved, Decimal64::parse("0"))) {
      ++entry->gross_;
    }
    // Anything else moved by an amount neither rule predicts -- a settlement in the same window,
    // most likely -- and is discarded rather than forced into one of the two answers.
  }

  // Returns the entry for `instrument`, or null.
  template <typename Entries>
  static auto find(Entries& entries, const InstrumentId& instrument)
    -> decltype(&entries.front().second) {
    for (auto& entry : entries) {
      if (entry.first == instrument) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  std::vector<std::pair<InstrumentId, Reading>> last_;
  std::vector<std::pair<InstrumentId, SellableEvidence>> evidence_;
};

} // namespace strategy
} // namespace algotradelib
