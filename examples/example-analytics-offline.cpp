// The analytics layer end to end, with no network and no credentials.
//
// Bars are built in code rather than fetched, so this runs anywhere and its output is
// the same every time -- which is what makes it useful as a worked example of the
// conventions rather than as a demonstration that a broker replied.
//
// It walks the whole path a strategy takes:
//
//   BarSeries -> SeriesFrame -> your own computation -> a metric -> back to a Decimal64 price
//
// and prints the absent values explicitly, because "a missing value stays missing, and is
// never a zero" is the single most important thing to understand about this layer.
//
// Run it as:
//
//   example-analytics-offline

#include <algotradelib/analytics>
#include <algotradelib/core>
#include <algotradelib/krx>

#include <chrono>
#include <cmath>
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

using analytics::observed;
using analytics::Scalar;

/** @brief Prints a scalar, or a dash where there is no observation. */
std::string cell(Scalar value) {
  if (!observed(value)) {
    return "     --";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << std::setw(7) << value;
  return text.str();
}

/**
 * @brief Each row's simple return on the row before.
 *
 * Absent on row 0, on a row next to an absent price, and after a price not above zero (from a
 * negative base the sign would reverse). The rows are not compressed around a gap first: a return
 * across it would span two periods.
 */
math::Vector period_returns(math::SeriesView prices) {
  math::Vector result = math::Vector::Constant(prices.size(), analytics::missing());
  for (math::Index row = 1; row < prices.size(); ++row) {
    if (observed(prices[row]) && observed(prices[row - 1]) && prices[row - 1] > 0) {
      result[row] = prices[row] / prices[row - 1] - 1;
    }
  }
  return result;
}

/** @brief The mean and the sample deviation of the observed values. */
struct Moments {
  /** @brief The mean, or absent with no observation. */
  Scalar mean_{analytics::missing()};
  /** @brief The sample deviation, or absent with fewer than two observations. */
  Scalar deviation_{analytics::missing()};
};

/** @brief Computes `Moments` over `values`, skipping what is absent. */
Moments moments(math::SeriesView values) {
  Moments result;
  Scalar sum = 0;
  Scalar count = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(values[row])) {
      sum += values[row];
      ++count;
    }
  }
  if (count < 1) {
    return result;
  }
  result.mean_ = sum / count;
  if (count < 2) {
    return result;
  }
  Scalar squares = 0;
  for (math::Index row = 0; row < values.size(); ++row) {
    if (observed(values[row])) {
      squares += (values[row] - result.mean_) * (values[row] - result.mean_);
    }
  }
  result.deviation_ = std::sqrt(squares / (count - 1));
  return result;
}

/**
 * @brief Builds a month of daily bars for one instrument.
 *
 * Exact decimals, the way a broker reports them, because that is the input the
 * conversion boundary is written for. One day is deliberately left without an open, to
 * show that it stays unobserved rather than becoming a zero.
 */
BarSeries fixture() {
  // Written as the strings a broker actually sends, so the conversion boundary is
  // exercised on real decimal text rather than on doubles dressed up as decimals.
  static constexpr const char* closes[] = {
    "70000",
    "70800",
    "70200",
    "71500",
    "72300",
    "71900",
    "73100",
    "74000",
    "73400",
    "72600",
    "71800",
    "72500",
    "73900",
    "75200",
    "74800",
    "76100",
    "77000",
    "76200",
    "75400",
    "74100",
  };
  static constexpr const char* highs[] = {
    "70500",
    "71300",
    "70700",
    "72000",
    "72800",
    "72400",
    "73600",
    "74500",
    "73900",
    "73100",
    "72300",
    "73000",
    "74400",
    "75700",
    "75300",
    "76600",
    "77500",
    "76700",
    "75900",
    "74600",
  };
  static constexpr const char* lows[] = {
    "69400",
    "70200",
    "69600",
    "70900",
    "71700",
    "71300",
    "72500",
    "73400",
    "72800",
    "72000",
    "71200",
    "71900",
    "73300",
    "74600",
    "74200",
    "75500",
    "76400",
    "75600",
    "74800",
    "73500",
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
    // looks like. It stays unobserved through the frame and every computation masks it.
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
  const math::Vector returns = period_returns(close);

  std::cout << bars.instrument_.symbol_ << ", " << bars.bars_.size() << " daily bars, no network\n\n";
  std::cout << "row   close     open   return\n";
  for (math::Index row = 0; row < close.size(); ++row) {
    std::cout
      << std::setw(3) << row << " " << cell(close[row]) << " " << cell(open[row]) << " " << cell(returns[row] * 100.0)
      << "\n";
  }

  // Row 3 has no open. Notice it is a dash above rather than a zero, and that the close
  // column and its returns are unaffected by it.
  std::cout << "\nThe dashes are the point. Row 0 has no return, and row 3 has no open at all.\n";

  const auto daily = moments(returns);
  std::cout
    << "\nOver the whole series:\n"
    << "  mean daily return     " << std::fixed << std::setprecision(4) << daily.mean_ * 100.0 << " %\n"
    << "  daily volatility      " << daily.deviation_ * 100.0 << " %\n"
    << "  per-period Sharpe     " << analytics::sharpe(returns) << "\n"
    << "  annualized Sharpe     " << analytics::annualize_ratio(analytics::sharpe(returns), 245.0) << "\n"
    << "  worst drawdown        " << analytics::max_drawdown(close).magnitude_ * 100.0 << " %\n";

  // The one sanctioned route back to something a broker will accept. Two deviations above the
  // average of the last five closes is a computed scalar; an order needs an exact decimal on the
  // instrument's own tick grid.
  const auto recent = moments(close.tail(5));
  const auto rule = algotradelib::krx::equity_price_rule();
  const auto limit = analytics::round_scalar_to_tick(recent.mean_ + 2.0 * recent.deviation_, rule, OrderSide::SELL);
  std::cout
    << "\nA sell limit two deviations above the last five closes' average, rounded onto the KRX tick grid: "
    << (limit ? limit->str() : std::string("refused")) << "\n";

  return 0;
}
