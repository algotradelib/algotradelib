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

#include <algotradelib/runner>
#include <algotradelib/strategies>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace {

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;
namespace vanilla = algotradelib::strategy::vanilla;

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
using algotradelib::PriceRule;
using algotradelib::SecurityType;
using algotradelib::Venue;

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** Reference data as a broker would report it: the KRX ladder and a lot of one share. */
Instrument samsung_reference() {
  Instrument reference;
  reference.id_ = samsung();
  reference.name_ = "Samsung Electronics";
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  return reference;
}

/**
 * A month of daily closes that rise, stall, and fall.
 *
 * Shaped so a trend strategy has something to enter and something to exit on, which is what makes
 * the output worth reading rather than a column of the same decision.
 */
const std::vector<const char*>& closes() {
  static const std::vector<const char*> series = {
    "60000", "60500", "61200", "61000", "62000", "63000", "64500", "65000", "66200", "67000",
    "68500", "69000", "70200", "71000", "72500", "73000", "72800", "72000", "71000", "69500",
    "68000", "66500", "65000", "63500", "62000", "61000", "60000", "59000", "58500", "58000",
  };
  return series;
}

/** Builds the bar for day `index`, stamped at its close. */
strategy::BarEvent bar(std::size_t index) {
  Bar value;
  value.timestamp_ = std::chrono::system_clock::time_point{} +
    std::chrono::hours(24 * (20000 + static_cast<int>(index)));
  value.open_ = Decimal64::parse(closes()[index]);
  value.high_ = Decimal64::parse(closes()[index]);
  value.low_ = Decimal64::parse(closes()[index]);
  value.close_ = Decimal64::parse(closes()[index]);
  value.volume_ = Decimal64::parse("1200000");
  return strategy::make_bar_event(samsung(), BarInterval::ONE_DAY, BarStamp::CLOSE, value);
}

/** Prints one decision line, or nothing when the strategy was quiet. */
template <typename Report>
void print(std::size_t day, const Report& report) {
  for (const auto& order : report.would_place_) {
    std::cout << "  day " << std::setw(2) << day << "  "
              << (order.side_ == OrderSide::BUY ? "BUY " : "SELL") << ' ' << std::setw(4)
              << order.quantity_.str() << " shares";
    if (order.price_) {
      std::cout << " at " << order.price_->str();
    } else {
      std::cout << " at market";
    }
    std::cout << "   [" << order.tag_ << "]\n";
  }
  for (const auto& rejection : report.rejected_) {
    std::cout << "  day " << std::setw(2) << day << "  rejected: "
              << strategy::reject_reason_name(rejection.reason_) << '\n';
  }
}

/**
 * Counts refusals by reason rather than printing each one.
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
 * Runs one strategy over the fixture and prints what it decided.
 *
 * The portfolio is advanced from the orders the runner would have sent, so the strategy sees the
 * consequences of its own decisions. Without that, a target-stating strategy would repeat the same
 * target every bar and the output would say nothing about when it changed its mind.
 */
template <typename Strategy>
void run(const char* title, Strategy instance, const strategy::RiskLimits& limits = {}) {
  std::cout << title << '\n';

  runner::DryRunner<Strategy> dry(std::move(instance), limits);
  dry.context().set_instrument(samsung_reference());
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.set_portfolio_value(Decimal64::parse("100000000"));

  Decimal64 held = Decimal64::parse("0");
  RefusalTally refusals;
  for (std::size_t day = 0; day < closes().size(); ++day) {
    dry.context().set_last_price(samsung(), Decimal64::parse(closes()[day]));

    strategy::Portfolio<runner::DryTag> portfolio;
    HoldingPosition position;
    position.instrument_ = samsung();
    position.quantity_ = held;
    portfolio.positions_.push_back(position);
    dry.context().set_portfolio(portfolio);

    const auto report = dry.feed(bar(day));
    print(day, report);
    tally(day, report, refusals);

    for (const auto& order : report.would_place_) {
      const auto signed_quantity = order.side_ == OrderSide::BUY ?
        algotradelib::decimal_add(held, order.quantity_) :
        algotradelib::decimal_subtract(held, order.quantity_);
      if (signed_quantity) {
        held = *signed_quantity;
      }
    }
  }
  for (const auto& entry : refusals) {
    std::cout << "  day " << std::setw(2) << entry.second.first << "  refused: " << entry.first
              << "  (and on " << entry.second.second - 1 << " later bars)\n";
  }
  std::cout << "  ending position: " << held.str() << " shares\n\n";
}

} // namespace

int main() {
  std::cout << "005930, " << closes().size()
            << " daily bars built in code -- no credentials, no network\n\n";

  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = samsung();
  holding.params_.quantity_ = Decimal64::parse("100");
  run("BuyAndHold: one target, then nothing, and it remembers nothing to do it", holding);

#if defined(ATL_HAS_EIGEN)
  vanilla::MovingAverageCrossover::Params crossing;
  crossing.instrument_ = samsung();
  crossing.fast_ = 3;
  crossing.slow_ = 8;
  crossing.quantity_ = Decimal64::parse("100");
  run(
    "SmaCrossover(3, 8): long while the fast average leads, flat while it lags",
    vanilla::SmaCrossover{crossing}
  );

  vanilla::DonchianBreakout breaking;
  breaking.params_.instrument_ = samsung();
  breaking.params_.lookback_ = 5;
  breaking.params_.quantity_ = Decimal64::parse("100");
  run("DonchianBreakout(5): the channel excludes today, so a break is measurable", breaking);

  vanilla::RsiThreshold indexed;
  indexed.params_.instrument_ = samsung();
  indexed.params_.period_ = 5;
  indexed.params_.oversold_ = 35.0;
  indexed.params_.overbought_ = 65.0;
  indexed.params_.quantity_ = Decimal64::parse("100");
  run("RsiThreshold(5, 35/65): Wilder's smoothing and Wilder's seed", indexed);
#else
  std::cout
    << "The indicator strategies need a linear algebra backend and this build has none.\n"
    << "Configure with -DATL_USE_EIGEN=ON to see SmaCrossover, DonchianBreakout and RsiThreshold.\n"
    << "BuyAndHold and TimedLiquidation read no series and run either way.\n\n";
#endif

  // A guard refusing an order is part of the pipeline, not an error, so it belongs in the output.
  strategy::RiskLimits tight;
  tight.max_order_notional_ = Decimal64::parse("1000000");
  vanilla::BuyAndHold oversized;
  oversized.params_.instrument_ = samsung();
  oversized.params_.quantity_ = Decimal64::parse("100");
  run(
    "BuyAndHold with a 1,000,000 KRW per-order ceiling: refused, never resized",
    oversized,
    tight
  );

  return 0;
}
