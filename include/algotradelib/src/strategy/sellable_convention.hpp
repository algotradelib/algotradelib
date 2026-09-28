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
 * Whether a broker's sellable figure already excludes orders resting on the sell side.
 *
 * **Neither broker states this, and the answer changes what a correct guard does.** Toss's
 * `sellableQuantity` is documented in units only; KIS's `ord_psbl_qty` carries an empty Description
 * in the API workbook; the KIS example repository never mentions it. So `RiskGuard` subtracts
 * resting sells from the figure, because over-counting refuses an order that would have been
 * accepted while under-counting sells a position twice -- but if the broker already subtracts them,
 * that deduction is charging twice for the same order.
 *
 * Settling it by experiment needs a market session and someone to remember to look. This settles it
 * from data the runner already polls, as a by-product of ordinary trading.
 */
enum class SellableConvention {
  /** Nothing conclusive has been seen yet, which is the honest state for most runs. */
  UNDETERMINED,
  /** The figure fell when an order began resting: the broker already excludes it. */
  NET_OF_RESTING,
  /** The figure did not move when an order began resting: the broker does not exclude it. */
  GROSS_OF_RESTING,
  /** Both have been observed, so no single rule describes what was seen. */
  INCONSISTENT,
};

/** Returns a stable lowercase name for `convention`, for logs and structured reporting. */
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
 * How many observations supported each answer.
 *
 * Counts rather than a flag, because one observation is evidence and not proof: a settlement or a
 * pledge release inside the same window moves the same number, and nothing here can see either. A
 * reader deciding whether to act on this should want to know it held across several days, which is
 * what the counts are for.
 */
struct SellableEvidence {
  /** Observations where the figure fell by the quantity that began resting. */
  std::size_t net_{0};
  /** Observations where the figure did not move when an order began resting. */
  std::size_t gross_{0};

  /** Returns what the counts say, with disagreement reported rather than resolved. */
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

  /** Returns how many observations contributed at all. */
  std::size_t total() const noexcept { return net_ + gross_; }
};

/**
 * Infers a broker's sellable convention from successive portfolio observations.
 *
 * Fed one portfolio per poll, and it reasons only about **changes**: the absolute distance between a
 * holding and its sellable quantity says nothing, because settlement, pledging and locking all open
 * one. What is diagnostic is the *movement* of that distance at the moment a sell order starts
 * resting.
 *
 * Two guards keep an observation honest, and both discard rather than guess:
 *
 * - **The holding must be unchanged.** A fill moves the position and the resting quantity together,
 *   so any window containing one is unusable.
 * - **The resting quantity must have grown.** If it did not, nothing happened that this can learn
 *   from, and a change in the distance is somebody else's doing.
 *
 * What remains is still evidence rather than proof: a settlement landing in the same window as a new
 * resting order would be read as `GROSS_OF_RESTING`, since the distance would move by something
 * other than the resting quantity -- which is why a distance that moves by the *wrong* amount is
 * discarded too, and why the counts are public.
 *
 * Costs nothing where the figure is absent, which is every broker that does not publish it and every
 * replay: no previous observation is ever completed, and the verdict stays `UNDETERMINED`.
 */
template <typename Broker>
class SellableConventionProbe {
public:
  /** Records one portfolio, completing any observation the previous one opened. */
  void observe(const Portfolio<Broker>& portfolio) {
    // **A figure of unknown coverage teaches nothing about resting orders, and would teach something
    // false.** This probe reads the *distance* between what is held and what is sellable and watches
    // how that distance moves. Both numbers have to describe the same shares for the distance to mean
    // anything: if `rmnd_qty` is account-wide while `trde_able_qty` covers one venue -- exactly what
    // `SellableBasis::Coverage::UNESTABLISHED` means at Kiwoom -- the distance is a venue split, and a
    // position moving between venues would read as a resting convention. So an unenforceable basis is
    // skipped whole rather than judged, and the verdict stays `UNDETERMINED`, which is true.
    if (!portfolio.sellable_basis_.is_enforceable()) {
      return;
    }
    for (const auto& position : portfolio.positions_) {
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

  /** Returns what has been seen for `instrument`. */
  SellableEvidence evidence(const InstrumentId& instrument) const {
    if (const auto* found = find(evidence_, instrument)) {
      return *found;
    }
    return SellableEvidence{};
  }

  /** Returns every instrument's evidence summed, which is the answer about the broker. */
  SellableEvidence overall() const {
    SellableEvidence total;
    for (const auto& entry : evidence_) {
      total.net_ += entry.second.net_;
      total.gross_ += entry.second.gross_;
    }
    return total;
  }

  /** Returns the verdict across every instrument seen. */
  SellableConvention verdict() const { return overall().verdict(); }

  /** Returns each instrument that has contributed an observation, with its evidence. */
  const std::vector<std::pair<InstrumentId, SellableEvidence>>& per_instrument() const noexcept {
    return evidence_;
  }

private:
  /**
   * Returns the sell quantity **the broker reported** resting, ignoring the runner's reservations.
   *
   * A runner adds a row of its own for an order it has just sent, so that the next order in the
   * pass cannot re-sell the same shares. Those rows are this library's arithmetic, and counting
   * them here would be circular: the resting quantity would rise with no change in the broker's
   * sellable figure, and the probe would record `GROSS_OF_RESTING` on evidence it manufactured
   * itself -- every time, for any broker, whenever the sellable check happened to be enabled.
   */
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

  /** One position as a poll reported it. */
  struct Reading {
    Decimal64 held_;
    Decimal64 sellable_;
    Decimal64 resting_;
  };

  /** Returns whether two decimals are numerically equal, across differing scales. */
  static bool same(const Decimal64& lhs, const Decimal64& rhs) {
    // `operator==` compares the coefficient and the scale, so "70" and "70.0" are not equal by it.
    // A broker is free to change the scale it reports between two polls, and a probe that read that
    // as a change would conclude from noise.
    return !decimal_less(lhs, rhs) && !decimal_less(rhs, lhs);
  }

  /** Records what the move from `before` to `now` says, if it says anything. */
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

  /** Returns the entry for `instrument`, or null. */
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
