// Your own strategy, replayed beside the shipped ones on identical bars.
//
// `example-backtest-offline` compares the vanilla strategies to each other. This one answers the
// question a new consumer actually has: **where does my strategy go, and how do I put it next to
// yours?** Everything above `MyStrategy` is scaffolding you will replace; `MyStrategy` itself is the
// part you own.
//
// This file has two jobs, and it is one file so that they cannot disagree. It is an example built
// inside this repository, and it is what `tools/atl-new-project.sh` copies into a generated project
// as `src/main.cpp`. A template kept separately from a compiled example is a second copy that
// nothing builds, and it soon goes stale.
//
// No credentials and no network: the bars are built in code, so the output is identical on every
// run and on every machine. Replacing `closes()` with your own history is the first thing to do
// after the numbers below stop surprising you -- see "Using it" on the documentation main page
// (`doxygen/index.md`) for where bars come from, because this library deliberately does not read files.
//
// Run it as:
//
//   example-backtest-compare

#include <algotradelib/backtest>
#include <algotradelib/vanilla>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace strategy = algotradelib::strategy;
namespace backtest = algotradelib::backtest;
namespace vanilla = algotradelib::strategy::vanilla;
namespace math = algotradelib::math;

using algotradelib::Bar;
using algotradelib::BarInterval;
using algotradelib::BarStamp;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::PriceRule;
using algotradelib::SecurityType;
using algotradelib::Venue;

InstrumentId traded() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

// ---------------------------------------------------------------------------------------------
// Your strategy.
//
// A strategy is a plain struct with **template** member functions. It is not templated on a broker,
// holds no session, and performs no I/O, which is exactly why this same source runs in a dry run and
// in a live session without being rewritten. The template is load-bearing rather than stylistic: it
// is what lets one strategy be handed three different runner contexts. `ATL_ASSERT_STRATEGY` below
// compiles every handler against two stub contexts, so a handler accidentally written against one
// concrete context, or one the runner's call does not fit, fails here rather than when somebody first
// runs it live.
//
// Emit a **target position**, not an order. A strategy that emits orders has to remember whether it
// already entered, and after a restart it does not know -- so it either misses an exit or re-enters a
// position it already holds. Stating the position you want leaves that to reconciliation, which can
// see the portfolio and the working orders; you cannot.
// ---------------------------------------------------------------------------------------------
struct MyStrategy {
  /** @brief How many closes the average is taken over. */
  std::size_t lookback_{5};
  /** @brief How many shares to hold when the signal is on. */
  Decimal64 quantity_{Decimal64::parse("1000")};

  std::vector<Decimal64> history_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    history_.push_back(bar.bar_.close_);
    if (history_.size() < lookback_) {
      return;
    }

    // Kept in `Decimal64` rather than converted to `math::Scalar`, for two reasons worth knowing
    // before you write the real thing.
    //
    // The first is that this strategy then needs no linear-algebra backend, so it compiles in a
    // build configured with `ATL_USE_EIGEN=OFF`. The shipped indicator strategies do need one, which
    // is why `SmaCrossover` and `DonchianBreakout` below sit inside a guard and this does not.
    //
    // The second is the comparison itself. Rather than divide the sum to get a mean -- which is the
    // one operation on an exact decimal that has to choose a scale and therefore round -- multiply
    // the other side. `close * n < sum` is the same question as `close < sum / n`, and it is exact.
    // Every arithmetic function returns `std::optional` because it checks for overflow, so a
    // strategy that cannot compute its own signal emits nothing instead of guessing.
    //
    // For real indicator work, cross into `math::Scalar` through `analytics::to_scalar` and come
    // back through `PriceRule::round_to_tick`. That gate is not decoration: it is what stops an
    // indicator's value reaching the wire as an off-tick limit price.
    std::optional<Decimal64> sum = Decimal64::parse("0");
    for (std::size_t at = history_.size() - lookback_; sum && at < history_.size(); ++at) {
      sum = algotradelib::decimal_add(*sum, history_[at]);
    }
    const auto scaled = algotradelib::decimal_multiply(
      history_.back(), Decimal64::parse(std::to_string(lookback_))
    );
    if (!sum || !scaled) {
      return;
    }

    // Buy what has fallen below its own recent average, hold nothing otherwise. It is a toy, and
    // the point of the run below is that you can watch it lose to buying and holding.
    const bool cheap = algotradelib::decimal_less(*scaled, *sum);

    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = cheap ? quantity_ : Decimal64::parse("0");
    ctx.submit(target);
  }
};

// ---------------------------------------------------------------------------------------------
// Everything below is the harness. Replace the bars; the rest can stay as it is.
// ---------------------------------------------------------------------------------------------

/** @brief What the strategies trade. A replay needs the reference data a live session would have fetched. */
Instrument reference() {
  Instrument instrument;
  instrument.id_ = traded();
  instrument.name_ = "Samsung Electronics";
  instrument.type_ = SecurityType::STOCK;
  instrument.currency_ = Currency{"KRW"};
  instrument.lot_size_ = Decimal64::parse("1");
  // The tick ladder is this library's own versioned table, because neither broker publishes one.
  // An unknown ladder refuses every price rather than guessing, since a guessed tick is an order the
  // exchange rejects.
  instrument.price_rule_ = PriceRule::krx_equity();
  return instrument;
}

/** @brief A path that rises, stalls, falls and recovers, so a trend strategy has both sides to trade. */
const std::vector<int>& closes() {
  static const std::vector<int> path = {
    60000, 60500, 61200, 61000, 62000, 63000, 64500, 65000, 66200, 67000,
    68500, 69000, 70200, 71000, 72500, 73000, 72800, 72000, 71000, 69500,
    68000, 66500, 65000, 63500, 62000, 61000, 60000, 59000, 58500, 58000,
    58500, 59500, 61000, 62500, 64000, 65500, 67000, 68500, 70000, 71500,
  };
  return path;
}

backtest::ReplayConfig config_for() {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {reference()};
  config.series_capacity_ = 64;
  // Any phase, so the point stays the strategies rather than the operator's session limits.
  config.limits_.permitted_phases_ = std::nullopt;
  return config;
}

/** @brief Costs a Korean online brokerage charges, in the right order of magnitude. */
backtest::NextBarFillModel costs() {
  return backtest::NextBarFillModel(
    backtest::CommissionModel::of(Decimal64::parse("0.00015"), Decimal64::parse("100")),
    backtest::SlippageModel::of_ticks(1)
  );
}

/**
 * @brief Replays `closes()` through one strategy.
 *
 * A function template rather than a container, because `Backtester<A>` and `Backtester<B>` are
 * unrelated types -- which is the same static-dispatch choice the rest of the library makes, and the
 * reason none of this costs a virtual call. Adding a fourth strategy below is one more call, not a
 * registration.
 */
template <typename Strategy>
backtest::Backtester<Strategy> replay(Strategy instance) {
  backtest::Backtester<Strategy> engine(std::move(instance), config_for(), costs());
  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  for (std::size_t at = 0; at < closes().size(); ++at) {
    Bar bar;
    bar.timestamp_ = start + std::chrono::hours(24 * static_cast<int>(at));
    bar.open_ = Decimal64::parse(std::to_string(closes()[at]));
    bar.high_ = *bar.open_;
    bar.low_ = *bar.open_;
    bar.close_ = *bar.open_;
    bar.volume_ = Decimal64::parse("1200000");
    engine.feed(strategy::make_bar_event(traded(), BarInterval::ONE_DAY, BarStamp::CLOSE, bar));
  }
  engine.finish();
  return engine;
}

#if defined(ATL_HAS_EIGEN)

/** @brief Prints a scalar as a percentage, or a dash where the figure does not exist. */
std::string percent(math::Scalar value) {
  if (!math::observed(value)) {
    return math::is_undefined(value) ? "undefined" : "       --";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << std::setw(7) << value * 100.0 << " %";
  return text.str();
}

/** @brief Prints a scalar plainly, or a dash. */
std::string plain(math::Scalar value) {
  if (!math::observed(value)) {
    return math::is_undefined(value) ? "undefined" : "       --";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(3) << std::setw(9) << value;
  return text.str();
}

/** @brief Replays one strategy and prints its row. */
template <typename Strategy>
void compare(const std::string& name, Strategy instance, math::Scalar periods) {
  const auto engine = replay(std::move(instance));
  const auto report = backtest::summarize(engine.result(), engine.fills(), periods);
  std::cout << "  " << std::left << std::setw(21) << name << std::right
            << percent(report.total_return_) << "  " << plain(report.sharpe_) << "  "
            << percent(report.max_drawdown_) << "  " << std::setw(5) << report.executions_ << "\n";
}

#endif

} // namespace

// Compiles every handler against a stub with no capability and one with every capability, so both
// branches of each capability gate are checked. Each runner checks it again with its own context.
ATL_ASSERT_STRATEGY(MyStrategy, algotradelib::strategy::MinimalContext);
ATL_ASSERT_STRATEGY(MyStrategy, algotradelib::strategy::FullContext);

int main() {
  std::cout << "005930, " << closes().size()
            << " bars built in code -- no credentials, no network\n\n";

#if defined(ATL_HAS_EIGEN)
  // Annualizing takes its period count from the caller rather than assuming a bar is a day. Get this
  // wrong and the annualized figure is wrong by the square root of the ratio.
  const auto periods =
    backtest::periods_per_year(BarInterval::ONE_DAY, strategy::SessionWindow::krx(), 245.0);

  std::cout << "  " << std::left << std::setw(21) << "strategy" << std::right
            << std::setw(9) << "return" << "  " << std::setw(9) << "Sharpe" << "  "
            << std::setw(9) << "max dd" << "  " << std::setw(5) << "fills" << "\n"
            << "  " << std::string(59, '-') << "\n";

  compare("MyStrategy(5)", MyStrategy{}, *periods);

  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = traded();
  holding.params_.quantity_ = Decimal64::parse("1000");
  compare("BuyAndHold", holding, *periods);

  vanilla::MovingAverageCrossover::Params crossing;
  crossing.instrument_ = traded();
  crossing.fast_ = 3;
  crossing.slow_ = 8;
  crossing.quantity_ = Decimal64::parse("1000");
  compare("SmaCrossover(3,8)", vanilla::SmaCrossover{crossing}, *periods);

  vanilla::DonchianBreakout breaking;
  breaking.params_.instrument_ = traded();
  breaking.params_.lookback_ = 5;
  breaking.params_.quantity_ = Decimal64::parse("1000");
  compare("DonchianBreakout(5)", breaking, *periods);

  std::cout
    << "\nForty invented bars rank nothing. What this shows is that the comparison is honest: every\n"
    << "strategy above saw identical bars, paid identical costs, and filled against the bar after the\n"
    << "one it decided on -- which is structural here, not a rule anybody has to remember. Read\n"
    << "`report.caveats_` before you believe a number, and the reading-a-backtest vignette before you\n"
    << "believe a ranking.\n";
#else
  // No backend, so there is no `Report` to print and no vanilla to compare against either: every
  // vanilla but `BuyAndHold` and `TimedLiquidation` sits in a header that needs `<algotradelib/analytics>`,
  // so `<algotradelib/vanilla>` does not ship them here. The replay engine itself needs no backend at all, which is worth showing
  // rather than asserting -- so the strategy is still run, and what it did is still counted.
  const auto engine = replay(MyStrategy{});
  std::cout << "MyStrategy replayed " << engine.result().bars_ << " bars and filled "
            << engine.result().executions_.size() << " orders.\n\n"
            << "Scoring those fills needs a linear algebra backend and this build has none.\n"
            << "Configure with -DATL_USE_EIGEN=ON for the metrics and the vanilla comparison.\n";
#endif

  return 0;
}
