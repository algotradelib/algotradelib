// A strategy run end to end with no broker, no credentials, and no network.
//
// Bars are built in code, so the output is identical every time and the binary contains no code
// path that could place an order -- `<algotradelib/runner>` links no TLS, which a test asserts.
// That is what makes this the right place to see what a strategy decides before letting it near
// a market.
//
// Run it as:
//
//   example-strategy-dryrun

#include <algotradelib/krx>
#include <algotradelib/runner>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;

using algotradelib::Bar;
using algotradelib::BarInterval;
using algotradelib::BarStamp;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::HoldingPosition;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::SecurityType;
using algotradelib::Venue;

InstrumentId samsung() {
  return InstrumentId{Country::KR, Venue::COMBINED, "005930"};
}

/** @brief Reference data as a broker would report it: the KRX ladder and a lot of one share. */
Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.name_ = "Samsung Electronics";
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = algotradelib::krx::equity_price_rule();
  return reference;
}

/**
 * @brief A month of daily closes that rise, stall, and fall.
 *
 * Shaped so a trend strategy has something to enter and something to exit on, which is what makes
 * the output worth reading rather than a column of the same decision.
 */
const std::vector<const char*>& closes() {
  static const std::vector<const char*> series = {
    "60000",
    "60500",
    "61200",
    "61000",
    "62000",
    "63000",
    "64500",
    "65000",
    "66200",
    "67000",
    "68500",
    "69000",
    "70200",
    "71000",
    "72500",
    "73000",
    "72800",
    "72000",
    "71000",
    "69500",
    "68000",
    "66500",
    "65000",
    "63500",
    "62000",
    "61000",
    "60000",
    "59000",
    "58500",
    "58000",
  };
  return series;
}

// ---------------------------------------------------------------------------------------------
// Two strategies, written here as you would write your own.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Holds a fixed quantity: states the same target on every bar.
 *
 * It buys once and then sends nothing while the holding matches, including after a restart, because
 * reconciliation compares the target with the portfolio and the working orders.
 */
struct HoldsShares {
  /** @brief The shares to hold. */
  Decimal64 quantity_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = quantity_;
    target.tag_ = "hold";
    ctx.submit(target);
  }
};

/**
 * @brief Buys on a close above the highest of the five before it, and leaves the position only when the
 *   session ends.
 *
 * The signal and the flatten are two handlers of one strategy. On a phase it may not hold through it
 * states a target of zero, once per instrument however many rows the holding spans. `flattened_` is
 * load-bearing: in a replay `on_bar` runs right after `on_phase`, and live the next bar follows it, so
 * a signal that restates its target would replace the flatten with a buy.
 */
struct BreakoutThenFlatten {
  /** @brief The shares to hold after a breakout. */
  Decimal64 quantity_;
  /** @brief The closes seen, newest last. */
  std::vector<Decimal64> closes_;
  /** @brief Whether the session's end has flattened the position. */
  bool flattened_{false};

  static constexpr std::size_t lookback = 5;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    closes_.push_back(bar.bar_.close_);
    if (flattened_ || closes_.size() <= lookback) {
      return;
    }
    // Exact decimals, so this needs no linear algebra backend.
    Decimal64 highest = closes_[closes_.size() - 1 - lookback];
    for (std::size_t at = closes_.size() - lookback; at + 1 < closes_.size(); ++at) {
      if (algotradelib::decimal_less(highest, closes_[at])) {
        highest = closes_[at];
      }
    }
    if (!algotradelib::decimal_less(highest, closes_.back())) {
      return;
    }
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = quantity_;
    target.tag_ = "breakout";
    ctx.submit(target);
  }

  template <class Ctx>
  void on_phase(Ctx& ctx, strategy::MarketPhase phase) {
    if (phase == strategy::MarketPhase::OPEN || phase == strategy::MarketPhase::PRE_OPEN) {
      return;
    }
    std::vector<InstrumentId> targeted;
    for (const auto& position : ctx.portfolio().positions_) {
      bool seen = false;
      for (const auto& already : targeted) {
        seen = seen || already == position.instrument_;
      }
      if (seen || position.quantity_.coefficient() == 0) {
        continue;
      }
      targeted.push_back(position.instrument_);
      strategy::TargetPosition target;
      target.instrument_ = position.instrument_;
      target.target_quantity_ = Decimal64::parse("0");
      target.tag_ = "session-end";
      ctx.submit(target);
    }
    flattened_ = true;
  }
};

ATL_ASSERT_STRATEGY(HoldsShares, algotradelib::strategy::MinimalContext);
ATL_ASSERT_STRATEGY(BreakoutThenFlatten, algotradelib::strategy::MinimalContext);

/** @brief Builds the bar for day `index`, stamped at its close. */
strategy::BarEvent bar(std::size_t index) {
  Bar value;
  value.timestamp_ =
    std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (20000 + static_cast<int>(index)));
  value.open_ = Decimal64::parse(closes()[index]);
  value.high_ = Decimal64::parse(closes()[index]);
  value.low_ = Decimal64::parse(closes()[index]);
  value.close_ = Decimal64::parse(closes()[index]);
  value.volume_ = Decimal64::parse("1200000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, value);
}

/** @brief Prints one decision line, or nothing when the strategy was quiet. */
template <typename Report>
void print(std::size_t day, const Report& report) {
  for (const auto& order : report.would_place_) {
    std::cout
      << "  day " << std::setw(2) << day << "  " << (order.side_ == OrderSide::BUY ? "BUY " : "SELL") << ' '
      << std::setw(4) << order.quantity_.str() << " shares";
    if (order.price_) {
      std::cout << " at " << order.price_->str();
    } else {
      std::cout << " at market";
    }
    std::cout << "   [" << order.tag_ << "]\n";
  }
  for (const auto& rejection : report.rejected_) {
    std::cout
      << "  day " << std::setw(2) << day << "  rejected: " << strategy::reject_reason_name(rejection.reason_) << '\n';
  }
}

/**
 * @brief Counts refusals by reason rather than printing each one.
 *
 * A guard that refuses on one bar usually refuses on every bar, because the condition that tripped
 * it has not changed. Thirty identical lines say no more than one line and a count, and they bury
 * the decisions worth reading.
 */
using RefusalTally = std::map<std::string, std::pair<std::size_t, std::size_t>>;

template <typename Report>
void tally(std::size_t day, const Report& report, RefusalTally& counts) {
  for (const auto& refusal : report.refused_) {
    const std::string reason(strategy::guard_reason_name(refusal.reason_));
    auto found = counts.find(reason);
    if (found == counts.end()) {
      counts.emplace(reason, std::pair<std::size_t, std::size_t>{day, 1});
    } else {
      ++found->second.second;
    }
  }
}

/**
 * @brief Runs one strategy over the fixture and prints what it decided.
 *
 * The portfolio is advanced from the orders the runner would have sent, so the strategy sees the
 * consequences of its own decisions. Without that, a target-stating strategy would repeat the same
 * target every bar and the output would say nothing about when it changed its mind.
 */
/** @brief The portfolio a broker would report: `held` shares of Samsung, all of them sellable. */
strategy::Portfolio<runner::DryTag> portfolio_of(const Decimal64& held) {
  strategy::Portfolio<runner::DryTag> portfolio;
  HoldingPosition position;
  position.instrument_ = samsung();
  position.quantity_ = held;
  // What the account may sell, which a broker's holdings state and the guard checks a sell against by
  // default: here the whole holding. Without it a dry run refuses every sell (`quantity_not_sellable`).
  position.sellable_quantity_ = held;
  portfolio.positions_.push_back(position);
  portfolio.sellable_basis_ = algotradelib::SellableBasis{
    algotradelib::SellableBasis::Coverage::WHOLE_HOLDING,
    algotradelib::SellableBasis::RestingTreatment::DEDUCT,
    false,
  };
  return portfolio;
}

/** @brief Moves `held` by what a report would have placed. */
template <typename Report>
void apply(const Report& report, Decimal64& held) {
  for (const auto& order : report.would_place_) {
    const auto signed_quantity = order.side_ == OrderSide::BUY ?
      algotradelib::decimal_add(held, order.quantity_) :
      algotradelib::decimal_subtract(held, order.quantity_);
    if (signed_quantity) {
      held = *signed_quantity;
    }
  }
}

template <typename Strategy>
void run(const char* title, Strategy instance, const strategy::RiskLimits& limits = {}, bool end_session = false) {
  std::cout << title << '\n';

  runner::DryRunner<Strategy> dry(std::move(instance), limits);
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.set_portfolio_value(Decimal64::parse("100000000"));
  // A runner driven by hand starts its strategy before the first feed.
  dry.start();

  Decimal64 held = Decimal64::parse("0");
  RefusalTally refusals;
  for (std::size_t day = 0; day < closes().size(); ++day) {
    dry.context().set_last_price(samsung(), Decimal64::parse(closes()[day]));

    dry.context().set_portfolio(portfolio_of(held));

    const auto report = dry.feed(bar(day));
    print(day, report);
    tally(day, report, refusals);
    apply(report, held);
  }
  if (end_session) {
    // The session ends after the last bar. The runner sees the holding the last bar left, then the phase.
    std::cout << "  holding " << held.str() << " shares when the session closes\n";
    dry.context().set_portfolio(portfolio_of(held));
    const auto report = dry.feed(strategy::MarketPhase::POST_CLOSE);
    print(closes().size(), report);
    tally(closes().size(), report, refusals);
    apply(report, held);
  }
  for (const auto& entry : refusals) {
    std::cout
      << "  day " << std::setw(2) << entry.second.first << "  refused: " << entry.first << "  (and on "
      << entry.second.second - 1 << " later bars)\n";
  }
  std::cout << "  ending position: " << held.str() << " shares\n\n";
}

} // namespace

int main() {
  std::cout << "005930, " << closes().size() << " daily bars built in code -- no credentials, no network\n\n";

  run("HoldsShares: one target, then nothing, and it remembers nothing to do it", HoldsShares{Decimal64::parse("100")});

  run(
    "BreakoutThenFlatten: in on a breakout, out when the session closes", BreakoutThenFlatten{Decimal64::parse("100")},
    {}, true
  );

  // A guard refusing an order is part of the pipeline, not an error, so it belongs in the output.
  strategy::RiskLimits tight;
  tight.max_order_notional_ = Decimal64::parse("1000000");
  run(
    "HoldsShares with a 1,000,000 KRW per-order ceiling: refused, never resized", HoldsShares{Decimal64::parse("100")},
    tight
  );

  return 0;
}
