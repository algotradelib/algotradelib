// The analytics layer end to end, with no network and no credentials.
//
// Bars are built in code rather than fetched, so this runs anywhere and its output is
// the same every time -- which is what makes it useful as a worked example of the
// conventions rather than as a demonstration that a broker replied.
//
// It walks the whole path a strategy takes:
//
//   BarSeries -> SeriesFrame -> indicators -> a metric -> back to a Decimal64 price
//
// and prints the warm-up region explicitly, because "the first N rows are blank and that
// is deliberate" is the single most important thing to understand about these functions.
//
// Run it as:
//
//   example-analytics-offline

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using algotradelib::Bar;
using algotradelib::BarInterval;
using algotradelib::BarSeries;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::SecurityType;
using algotradelib::Venue;

namespace analytics = algotradelib::analytics;
namespace math = algotradelib::math;

using analytics::Scalar;
using analytics::observed;

/** Prints a scalar, or a dash where there is no observation. */
std::string cell(Scalar value) {
  if (!observed(value)) {
    return "     --";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << std::setw(7) << value;
  return text.str();
}

/**
 * Builds a month of daily bars for one instrument.
 *
 * Exact decimals, the way a broker reports them, because that is the input the
 * conversion boundary is written for. One day is deliberately left without an open, to
 * show that it stays unobserved rather than becoming a zero.
 */
BarSeries fixture() {
  // Written as the strings a broker actually sends, so the conversion boundary is
  // exercised on real decimal text rather than on doubles dressed up as decimals.
  static constexpr const char* closes[] = {
    "70000", "70800", "70200", "71500", "72300", "71900", "73100", "74000", "73400",
    "72600", "71800", "72500", "73900", "75200", "74800", "76100", "77000", "76200",
    "75400", "74100",
  };
  static constexpr const char* highs[] = {
    "70500", "71300", "70700", "72000", "72800", "72400", "73600", "74500", "73900",
    "73100", "72300", "73000", "74400", "75700", "75300", "76600", "77500", "76700",
    "75900", "74600",
  };
  static constexpr const char* lows[] = {
    "69400", "70200", "69600", "70900", "71700", "71300", "72500", "73400", "72800",
    "72000", "71200", "71900", "73300", "74600", "74200", "75500", "76400", "75600",
    "74800", "73500",
  };

  BarSeries series;
  series.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
  series.interval_ = BarInterval::ONE_DAY;
  series.currency_ = Currency{"KRW"};
  series.received_at_ = std::chrono::system_clock::now();

  const auto start = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  for (std::size_t day = 0; day < std::size(closes); ++day) {
    Bar bar;
    bar.timestamp_ = start + std::chrono::hours(24 * static_cast<int>(day));
    // The fourth day has no open, which is what a session with no trade at the auction
    // looks like. It stays unobserved through the frame and every indicator masks it.
    if (day != 3) {
      bar.open_ = Decimal64::parse(closes[day]);
    }
    bar.high_ = Decimal64::parse(highs[day]);
    bar.low_ = Decimal64::parse(lows[day]);
    bar.close_ = Decimal64::parse(closes[day]);
    bar.volume_ = Decimal64::parse("1200000");
    series.bars_.push_back(bar);
  }
  return series;
}

} // namespace

int main() {
  const BarSeries bars = fixture();

  // The one place exact decimals become scalars. Everything below computes in `Scalar`
  // and nothing below it sees a `Decimal64` again until the very last step.
  const analytics::SeriesFrame frame = analytics::from_bars(bars);

  // Views into the frame's own storage, not copies: a column of a column-major frame is
  // contiguous, which is what `SeriesView` is for.
  const math::SeriesView close = frame.close();
  const math::SeriesView open = frame.open();
  const math::Vector average = analytics::sma(close, 5);
  const math::Vector smoothed = analytics::ema(close, 5);
  const math::Vector strength = analytics::rsi(close, 5);
  const math::Vector score = analytics::zscore(close, 5);
  const auto bands = analytics::bollinger(close, 5, 2.0);
  const math::Vector range = analytics::atr(frame.high(), frame.low(), close, 5);
  const math::Vector period_returns = analytics::returns(close);
  const math::Vector growth = analytics::cumulative_returns(period_returns);

  std::cout << bars.instrument_.symbol_ << ", " << bars.bars_.size()
            << " daily bars, no network\n\n";
  std::cout << "row   close     open     sma5     ema5     rsi5    zscore"
            << "    upper    lower     atr5   cumret\n";
  for (math::Index row = 0; row < close.size(); ++row) {
    std::cout << std::setw(3) << row << " " << cell(close[row]) << " "
              << cell(open[row]) << " " << cell(average[row]) << " "
              << cell(smoothed[row]) << " " << cell(strength[row]) << " "
              << cell(score[row]) << " " << cell(bands.upper_[row]) << " "
              << cell(bands.lower_[row]) << " " << cell(range[row]) << " "
              << cell(growth[row] * 100.0) << "\n";
  }

  // Row 3 has no open. Notice it is a dash above rather than a zero, and that the
  // indicators over the close column are unaffected by it.
  std::cout << "\nThe dashes are the point. A window reports nothing until it holds "
            << "enough\nobservations, and row 3 has no open at all.\n";

  // What a live loop keeps instead of a growing buffer: one number per statistic.
  analytics::RunningMoments moments;
  analytics::RunningDrawdown drawdown;
  analytics::RunningEma running(5);
  for (math::Index row = 0; row < close.size(); ++row) {
    moments.update(period_returns[row]);
    drawdown.update(close[row]);
    running.update(close[row]);
  }

  std::cout << "\nOver the whole series, in constant space:\n"
            << "  mean daily return     " << std::fixed << std::setprecision(4)
            << moments.mean() * 100.0 << " %\n"
            << "  daily volatility      " << moments.stdev() * 100.0 << " %\n"
            << "  per-period Sharpe     " << analytics::sharpe(period_returns) << "\n"
            << "  annualized Sharpe     "
            << analytics::annualize_ratio(analytics::sharpe(period_returns), 245.0) << "\n"
            << "  worst drawdown        "
            << analytics::max_drawdown(close).magnitude_ * 100.0 << " %\n";

  // And the accumulator agrees with the vectorized indicator exactly, which is the
  // property that keeps a backtest and a live loop computing the same thing.
  std::cout << "  running ema == ema    "
            << (running.value() == smoothed[close.size() - 1] ? "yes" : "no") << "\n";

  // The one sanctioned route back to something a broker will accept. The upper band is a
  // computed scalar; an order needs an exact decimal on the instrument's own tick grid.
  const auto rule =
    algotradelib::price_rule_for(bars.instrument_.country_, SecurityType::STOCK);
  const auto limit = analytics::round_scalar_to_tick(
    bands.upper_[close.size() - 1], rule, OrderSide::SELL
  );
  std::cout << "\nA sell limit at the upper band, rounded onto the KRX tick grid: "
            << (limit ? limit->str() : std::string("refused")) << "\n";

  return 0;
}
