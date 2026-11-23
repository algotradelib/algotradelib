// A backtest with no broker, no credentials, and no network.
//
// Bars are built in code, so the output is identical every time. That makes this the place to see what
// a backtest does and, more usefully, what it declines to tell you: the caveats print alongside the
// figures because a reader who has to remember which assumptions applied to their run eventually will
// not remember.
//
// `MyStrategy` below is the part to replace with your own. It is replayed at daily and at minute
// granularity, and the engine is told nothing about which it is getting; then once more at no cost, to
// show what the costs were doing. Neither KIS nor Toss makes intraday history easy, but that is a
// limitation of those adapters rather than of backtesting.
//
// The replays run in every build. Scoring them (`backtest::Report`) needs a linear algebra backend
// (`ATL_USE_EIGEN=ON`); without one the program prints what each replay filled.
//
// Run it as:
//
//   example-backtest-offline

#include <algotradelib/backtest>
#include <algotradelib/krx>

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
namespace math = algotradelib::math;

#if defined(ATL_HAS_EIGEN)
/**
 * @brief Returns a monetary figure, or why there is none.
 *
 * A total that spans currencies is absent rather than summed, and so is one from a replay that
 * executed nothing -- neither is a charge of zero, and printing `0` would say it was.
 */
std::string money_or_absent(const std::optional<algotradelib::Decimal64>& amount) {
  return amount ? amount->str() : std::string("(absent: see the caveats)");
}
#endif

using algotradelib::Bar;
using algotradelib::BarInterval;
using algotradelib::BarStamp;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::SecurityType;
using algotradelib::Venue;

InstrumentId samsung() {
  return InstrumentId{Country::KR, Venue::COMBINED, "005930"};
}

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
    // The first is that this strategy then needs no linear-algebra backend, so it compiles and runs in
    // a build configured with `ATL_USE_EIGEN=OFF`.
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
    const auto scaled = algotradelib::decimal_multiply(history_.back(), Decimal64::parse(std::to_string(lookback_)));
    if (!sum || !scaled) {
      return;
    }

    // Buy what has fallen below its own recent average, hold nothing otherwise. It is a toy: the
    // point of the runs below is what a replay reports, not this signal.
    const bool cheap = algotradelib::decimal_less(*scaled, *sum);

    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = cheap ? quantity_ : Decimal64::parse("0");
    ctx.submit(target);
  }
};

/** @brief A path that rises, stalls, falls and recovers, so a trend strategy has both sides to trade. */
const std::vector<int>& closes() {
  static const std::vector<int> path = {
    60000,
    60500,
    61200,
    61000,
    62000,
    63000,
    64500,
    65000,
    66200,
    67000,
    68500,
    69000,
    70200,
    71000,
    72500,
    73000,
    72800,
    72000,
    71000,
    69500,
    68000,
    66500,
    65000,
    63500,
    62000,
    61000,
    60000,
    59000,
    58500,
    58000,
    58500,
    59500,
    61000,
    62500,
    64000,
    65500,
    67000,
    68500,
    70000,
    71500,
  };
  return path;
}

/** @brief Costs a Korean online brokerage charges, in the right order of magnitude. */
backtest::NextBarFillModel realistic_costs() {
  return backtest::NextBarFillModel(
    backtest::CommissionModel::of(Decimal64::parse("0.00015"), Decimal64::parse("100")),
    backtest::SlippageModel::of_ticks(1)
  );
}

backtest::ReplayConfig config_for() {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {samsung_reference()};
  config.series_capacity_ = 64;
  // Any phase, so a minute replay's pre-open and post-close bars are not refused while the point here
  // is the strategy rather than the operator's limits.
  config.limits_.permitted_phases_ = std::nullopt;
  return config;
}

/** @brief Replays `closes()` through `strategy` at `interval`, under `fills` (the realistic costs by default). */
template <typename Strategy>
backtest::Backtester<Strategy> replay(
  Strategy instance, BarInterval interval, const backtest::NextBarFillModel& fills = realistic_costs()
) {
  backtest::Backtester<Strategy> engine(std::move(instance), config_for(), fills);
  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  for (std::size_t at = 0; at < closes().size(); ++at) {
    const auto step = interval == BarInterval::ONE_MINUTE ?
      std::chrono::minutes(static_cast<int>(at)) :
      std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours(24 * static_cast<int>(at)));
    Bar bar;
    bar.timestamp_ = start + step;
    bar.open_ = Decimal64::parse(std::to_string(closes()[at]));
    bar.high_ = *bar.open_;
    bar.low_ = *bar.open_;
    bar.close_ = *bar.open_;
    bar.volume_ = Decimal64::parse("1200000");
    engine.feed(strategy::make_bar_event(samsung(), interval, BarStamp::CLOSE, bar));
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

/** @brief Prints one report, figures first and then what they do not account for. */
void print(const std::string& title, const backtest::Report& report) {
  std::cout
    << title << "\n"
    << "  bars            " << report.bars_ << "\n"
    << "  executions      " << report.executions_ << "\n"
    << "  total return    " << percent(report.total_return_) << "\n"
    << "  Sharpe          " << plain(report.sharpe_) << "  (per bar)\n"
    << "  annualized      " << plain(report.annualized_sharpe_) << "\n"
    << "  Sortino         " << plain(report.sortino_) << "\n"
    << "  max drawdown    " << percent(report.max_drawdown_) << "\n"
    << "  turnover        " << percent(report.total_turnover_) << "\n"
    << "  commission      " << money_or_absent(report.commission_paid_) << " KRW\n"
    << "  slippage        " << money_or_absent(report.slippage_paid_) << " KRW\n";
  std::cout
    << "  assumed         limit fills " << backtest::limit_fill_price_name(report.caveats_.limit_fill_) << ", slippage "
    << report.caveats_.slippage_ticks_ << " tick\n";
  // The rates that actually charged something, rather than one number: a run spanning a rate change
  // or costing by venue charges more than one, and printing the first would name an accident.
  for (const auto& applied : report.caveats_.applied_rates_) {
    std::cout
      << "  commission rate " << applied.rate_.str() << " with a " << applied.minimum_.str() << " floor, "
      << commission_origin_name(applied.origin_) << ", charged " << applied.executions_ << " execution(s)\n";
  }
  if (report.caveats_.cost_totals_incomplete_) {
    // The third reason a total can be absent, and the one a reader must not miss: it is not that
    // the run spanned currencies or traded nothing, it is that an addition did not fit and the
    // figures below it are short by an amount nobody can name.
    std::cout
      << "  cost totals     INCOMPLETE -- an exact sum did not fit; slippage is understated"
      << " and the fee totals are withheld\n";
  }
  if (report.caveats_.inclusive_commission_executions_ > 0) {
    std::cout
      << "  commission is    inclusive of any institution charge on "
      << report.caveats_.inclusive_commission_executions_ << " execution(s)\n";
  }
  std::cout << "  not modelled    volume, queue position, market impact, borrow\n";
  if (report.caveats_.gaps_ > 0) {
    std::cout << "  gaps            " << report.caveats_.gaps_ << " (not interpolated across)\n";
  }
  if (report.caveats_.unfilled_ > 0) {
    std::cout << "  unfilled        " << report.caveats_.unfilled_ << " (no next bar to fill against)\n";
  }
  if (report.caveats_.expired_day_orders_ > 0) {
    std::cout
      << "  expired         " << report.caveats_.expired_day_orders_
      << " DAY order(s) unfilled at their session's close, as at a broker\n";
  }
  if (report.caveats_.multiple_instruments_) {
    std::cout
      << "  CAVEAT          more than one instrument: this result may carry survivorship\n"
      << "                  bias, because reference data lists what exists now\n";
  }
  std::cout << "\n";
}
#endif

} // namespace

// Compiles every handler against a stub with no capability and one with every capability, so both
// branches of each capability gate are checked. Each runner checks it again with its own context.
ATL_ASSERT_STRATEGY(MyStrategy, algotradelib::strategy::MinimalContext);
ATL_ASSERT_STRATEGY(MyStrategy, algotradelib::strategy::FullContext);

int main() {
  std::cout << "005930, " << closes().size() << " bars built in code -- no credentials, no network\n\n";

  // The same strategy over the same path: daily, then as minute bars, then daily at no cost at all.
  const auto daily = replay(MyStrategy{}, BarInterval::ONE_DAY);
  const auto minutes = replay(MyStrategy{}, BarInterval::ONE_MINUTE);
  const auto costless = replay(MyStrategy{}, BarInterval::ONE_DAY, backtest::NextBarFillModel{});

#if defined(ATL_HAS_EIGEN)
  const auto daily_periods = backtest::periods_per_year(BarInterval::ONE_DAY, strategy::SessionWindow::krx(), 245.0);
  const auto minute_periods =
    backtest::periods_per_year(BarInterval::ONE_MINUTE, strategy::SessionWindow::krx(), 245.0);
  std::cout
    << "periods per year: " << *daily_periods << " daily, " << *minute_periods
    << " minute -- which is why annualizing takes the count rather than assuming it\n\n";

  print("MyStrategy(5), daily", backtest::summarize(daily.result(), daily.fills(), *daily_periods));

  // The engine is told nothing about which granularity it is getting; only the annualizing constant
  // differs, and it differs because the caller says so rather than because the engine guessed.
  print(
    "MyStrategy(5), minute bars -- identical decisions, different annualizing",
    backtest::summarize(minutes.result(), minutes.fills(), *minute_periods)
  );
  std::cout
    << "  ^ Note the annualized Sharpe against the daily run above. The per-bar figure is\n"
    << "    identical -- the decisions were identical -- and only the constant differs, because\n"
    << "    sqrt(95550) is about twenty times sqrt(245). The larger number is arithmetically\n"
    << "    right and practically meaningless here: this path moves like daily data, so calling\n"
    << "    its bars minutes claims a frequency the returns do not have. Two lessons. Mislabel\n"
    << "    your bar frequency and the annualized figure is wrong by the square root of the\n"
    << "    ratio. And annualizing assumes returns are serially uncorrelated, which a trading\n"
    << "    rule can violate by construction.\n\n";

  print(
    "MyStrategy(5), daily, at zero cost -- the difference is what costs cost",
    backtest::summarize(costless.result(), costless.fills(), *daily_periods)
  );

  std::cout
    << "Read the caveats, not only the figures. A backtest is the easiest place to produce a\n"
    << "confident wrong number, and the ones above assume every fill was available in full at\n"
    << "the price shown, which no bar can actually tell us.\n";
#else
  const auto filled = [](const char* title, const auto& engine) {
    std::cout
      << title << ": replayed " << engine.result().bars_ << " bars and filled " << engine.result().executions_.size()
      << " orders\n";
  };
  filled("MyStrategy(5), daily", daily);
  filled("MyStrategy(5), minute bars", minutes);
  filled("MyStrategy(5), daily, at zero cost", costless);
  std::cout
    << "\nScoring those fills needs a linear algebra backend and this build has none.\n"
    << "Configure with -DATL_USE_EIGEN=ON for the report and its caveats.\n";
#endif

  return 0;
}
