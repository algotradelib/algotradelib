#pragma once

#include "./engine.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * Summarizing a replay, including what it could not tell you.
 *
 * Needs a linear-algebra backend, because the metrics do. The engine itself does not, so a replay can
 * run in a build without one and be summarized by a caller's own arithmetic.
 *
 * **Every caveat is a field rather than a footnote.** A backtester is the easiest place in a library to
 * produce confident, wrong numbers, and a reader who has to know which paragraph of documentation
 * applied to their run will eventually not know. So what the fill model assumed, what it did not model,
 * and whether a universe could have suffered survivorship bias all travel with the figures.
 *
 * This is also the first consumer of `analytics`'s evaluation layer. `sortino` and the turnover pair had
 * no caller at all before it, and writing this is the test of whether the conventions chosen there were
 * the right ones.
 */

/** What a replay's numbers do and do not account for. */
struct ReportCaveats {
  /** What a limit order was credited with. */
  LimitFillPrice limit_fill_{LimitFillPrice::AT_LIMIT};
  /** The commission rate applied; zero means a zero-cost run, which is stated rather than implied. */
  Decimal64 commission_rate_;
  /** The per-order commission floor applied. */
  Decimal64 commission_minimum_;
  /** Basis points of slippage applied. */
  Decimal64 slippage_basis_points_;
  /** Tick steps of slippage applied. */
  int slippage_ticks_{0};

  /**
   * Whether more than one instrument was replayed.
   *
   * When true the result may carry survivorship bias: broker reference data lists instruments that
   * exist *now*, so a universe assembled from it silently excludes everything delisted during the
   * period, which flatters a cross-sectional result. The engine cannot fix that — it is a property of
   * the data — so it says so instead.
   */
  bool multiple_instruments_{false};

  /** Bars that did not follow their predecessor; a replay does not interpolate across a gap. */
  std::size_t gaps_{0};

  /** Orders still queued when the replay ended, never filled. */
  std::size_t unfilled_{0};

  /**
   * Always true, and present so a reader is told rather than left to assume.
   *
   * No fill is capped by what traded: a bar's volume says how much changed hands in total, not how much
   * was available to this order at its price. Queue position, market impact and borrow availability are
   * likewise unmodelled.
   */
  static constexpr bool volume_ignored = true;
};

/** What a replay produced, with every figure's absence meaning what `analytics` says it means. */
struct Report {
  /** Bars replayed. */
  std::size_t bars_{0};
  /** Executions the fill model produced. */
  std::size_t executions_{0};

  /** Equity at the first and last recorded points. */
  Decimal64 opening_equity_;
  Decimal64 closing_equity_;
  /** Total return over the replay, as a fraction. */
  math::Scalar total_return_{0};

  /** Per-period Sharpe ratio, or unobserved when it does not exist. */
  math::Scalar sharpe_{0};
  /** Annualized Sharpe, present only when a period count was supplied. */
  math::Scalar annualized_sharpe_{0};
  /** Per-period Sortino ratio; `undefined()` when nothing fell below the target. */
  math::Scalar sortino_{0};
  /** Worst peak-to-trough decline, as a non-negative fraction. */
  math::Scalar max_drawdown_{0};
  /** One-way turnover summed over the replay, as a fraction of equity traded. */
  math::Scalar total_turnover_{0};

  /** Commission and slippage paid, so a reader can see what the model cost. */
  Decimal64 commission_paid_;
  Decimal64 slippage_paid_;

  /** Instructions reconciliation could not build. */
  std::size_t rejected_{0};
  /** Instructions a guard refused. */
  std::size_t refused_{0};

  ReportCaveats caveats_;
};

namespace report_detail {

/** Returns a decimal as the scalar the metrics compute in. */
inline math::Scalar to_scalar(const Decimal64& value) {
  auto scaled = static_cast<math::Scalar>(value.coefficient());
  for (std::uint8_t at = 0; at < value.scale(); ++at) {
    scaled /= math::Scalar{10};
  }
  return scaled;
}

} // namespace report_detail

/**
 * Summarizes a replay.
 *
 * `periods_per_year` is optional and has no default, for the reason `analytics::annualize_ratio` takes
 * it rather than assuming: the answer is 245 for daily bars on KRX and 95,550 for minute bars over the
 * same year. Absent leaves the annualized figure unobserved rather than scaled by a guess —
 * `backtest::periods_per_year` derives it from an interval, a session and a trading-day count.
 *
 * `risk_free` is per period, in the same units as the returns, matching `analytics::sharpe`.
 */
inline Report summarize(
  const ReplayResult& result,
  const NextBarFillModel& fills,
  std::optional<math::Scalar> periods_per_year = std::nullopt,
  math::Scalar risk_free = math::Scalar{0}
) {
  Report report;
  report.bars_ = result.bars_;
  report.executions_ = result.executions_.size();
  report.rejected_ = result.rejected_.size();
  report.refused_ = result.refused_.size();

  report.caveats_.limit_fill_ = fills.limit_price();
  report.caveats_.commission_rate_ = fills.commission().rate_;
  report.caveats_.commission_minimum_ = fills.commission().minimum_;
  report.caveats_.slippage_basis_points_ = fills.slippage().basis_points_;
  report.caveats_.slippage_ticks_ = fills.slippage().ticks_;
  report.caveats_.gaps_ = result.gaps_;
  report.caveats_.unfilled_ = result.unfilled_;

  // Costs summed exactly, because they are money.
  auto commission = Decimal64::parse("0");
  auto slippage = Decimal64::parse("0");
  std::vector<InstrumentId> seen;
  for (const auto& execution : result.executions_) {
    if (const auto sum = decimal_add(commission, execution.commission_)) {
      commission = *sum;
    }
    // Slippage is a per-share price difference, so its cost is that times the quantity.
    if (const auto cost = decimal_multiply(execution.slippage_, execution.fill_.quantity_)) {
      if (const auto sum = decimal_add(slippage, *cost)) {
        slippage = *sum;
      }
    }
    bool known = false;
    for (const auto& id : seen) {
      if (id == execution.fill_.instrument_) {
        known = true;
        break;
      }
    }
    if (!known) {
      seen.push_back(execution.fill_.instrument_);
    }
  }
  report.commission_paid_ = commission;
  report.slippage_paid_ = slippage;
  report.caveats_.multiple_instruments_ = seen.size() > 1;

  if (result.equity_.empty()) {
    // Nothing to measure. Every figure stays unobserved rather than becoming zero, which would read as
    // a flat result rather than as an absent one.
    report.total_return_ = math::missing();
    report.sharpe_ = math::missing();
    report.annualized_sharpe_ = math::missing();
    report.sortino_ = math::missing();
    report.max_drawdown_ = math::missing();
    report.total_turnover_ = math::missing();
    return report;
  }

  report.opening_equity_ = result.equity_.front().equity_;
  report.closing_equity_ = result.equity_.back().equity_;

  // The returns and the equity curve, as the columns the metrics take.
  math::Vector returns(static_cast<math::Index>(result.equity_.size()));
  math::Vector levels(static_cast<math::Index>(result.equity_.size()));
  for (std::size_t at = 0; at < result.equity_.size(); ++at) {
    returns[static_cast<math::Index>(at)] = result.equity_[at].period_return_;
    levels[static_cast<math::Index>(at)] = report_detail::to_scalar(result.equity_[at].equity_);
  }

  const auto opening = report_detail::to_scalar(report.opening_equity_);
  report.total_return_ = opening == math::Scalar{0} ?
    math::undefined() :
    report_detail::to_scalar(report.closing_equity_) / opening - math::Scalar{1};

  report.sharpe_ = analytics::sharpe(returns, risk_free);
  report.sortino_ = analytics::sortino(returns, risk_free);
  report.max_drawdown_ = analytics::max_drawdown(levels).magnitude_;

  report.annualized_sharpe_ = periods_per_year ?
    analytics::annualize_ratio(report.sharpe_, *periods_per_year) :
    math::missing();

  // Turnover from the executions rather than from a weight matrix, because a replay knows what it
  // actually traded. One-way, matching `analytics::one_way_turnover`: the fraction of equity that
  // changed hands.
  report.total_turnover_ = opening == math::Scalar{0} ? math::undefined() : math::Scalar{0};
  if (math::observed(report.total_turnover_)) {
    for (const auto& execution : result.executions_) {
      const auto traded = decimal_multiply(execution.fill_.quantity_, execution.fill_.price_);
      if (traded) {
        report.total_turnover_ += report_detail::to_scalar(*traded) / opening;
      }
    }
  }

  return report;
}

} // namespace backtest
} // namespace algotradelib
