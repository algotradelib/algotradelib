#pragma once

#include "./engine.hpp"

#include <algotradelib/analytics>
#include <algotradelib/core>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {
namespace backtest {

/**
 * @file
 * @brief The backtest report: performance figures, costs, and every assumption and limitation of
 *   the run as a field (`ReportCaveats`).
 *
 * Needs a linear algebra backend (`ATL_USE_EIGEN=ON`); the engine itself does not.
 */

/** @brief What a replay's figures assumed and did not account for. */
struct ReportCaveats {
  /** @brief Which price a filled limit order was credited. */
  LimitFillPrice limit_fill_{LimitFillPrice::AT_LIMIT};
  /**
   * @brief Every commission rate that charged an execution, with how many it charged.
   *
   * Only rates actually used, not every rate in the schedule. A zero-cost run that traded lists a
   * zero rate.
   */
  std::vector<AppliedRate> applied_rates_;
  /**
   * @brief How many executions were charged a single combined rate (no institution fee split).
   *
   * Nonzero means `Report::commission_paid_` includes the institution fee for those executions and
   * `Report::institution_fees_paid_` is absent.
   */
  std::size_t inclusive_commission_executions_{0};
  /**
   * @brief The venue costs were computed at, as the caller asserted it (a replay cannot observe where
   *   a fill executed).
   *
   * Empty when no scheduled commission policy was used.
   */
  std::optional<VenueKey> asserted_venue_;
  /** @brief The tariff the commission schedule named; empty for a flat model. */
  std::string commission_tariff_;
  /**
   * @brief Whether some cost could not be summed exactly, so the cost totals are incomplete.
   *
   * When set, every scalar cost total is absent and the per-currency totals hold only what could be
   * added. It takes amounts far beyond any real account.
   */
  bool cost_totals_incomplete_{false};
  /** @brief The basis points of slippage applied. */
  Decimal64 slippage_basis_points_;
  /** @brief The ticks of slippage applied. */
  int slippage_ticks_{0};

  /**
   * @brief Whether more than one instrument traded: a caution about survivorship bias.
   *
   * A universe built from a broker's current listings omits instruments delisted during the period,
   * which tends to flatter the result. The engine cannot see how the universe was chosen, so this is
   * a proxy that is also set for a hand-picked universe with no such problem. Figures are still
   * computed.
   */
  bool multiple_instruments_{false};

  /** @brief Bars that did not follow their predecessor (see `ReplayClock::contiguous`); gaps are not interpolated. */
  std::size_t gaps_{0};

  /** @brief Orders still queued, unfilled, when the replay ended. */
  std::size_t unfilled_{0};

  /**
   * @brief How many quotes the replay was fed (zero for a bars-only replay).
   *
   * @note An order placed from `on_quote` does not fill against the quote: it fills against a later
   *   bar (a market order at that bar's open), at the earliest the first bar whose period began at or
   *   after the decision, where the engine can tell when a period began. Live, the same order reaches
   *   the broker at once. So a quote-driven strategy's replayed fills can be later, and at a
   *   different price, than its live ones.
   */
  std::size_t quotes_{0};

  /**
   * @brief How many times a fill was allowed without the engine being able to prove its price came
   *   after the decision.
   *
   * Counted once per queued order per bar considered (the fill model may still decline). It happens
   * when a bar's period start or end is not known exactly: a close-stamped weekly, monthly or yearly
   * bar, or an open-stamped bar of a day or longer, whose end is nominal (every shipped adapter
   * produces those). An open-stamped weekly series whose bars each start at or after the previous
   * bar's nominal end (Monday to Monday) counts nothing; a close-stamped weekly series counts every
   * decision.
   *
   * With no quotes, one interval per instrument and non-overlapping periods, the fills were causal
   * whatever this says. See `ReplayResult::unchecked_fill_causality_`.
   */
  std::size_t unchecked_fill_causality_{0};

  /**
   * @brief Always `true`: fills are never capped by traded volume, and queue position, market
   *   impact and borrow availability are not modelled.
   */
  static constexpr bool volume_ignored = true;

  /**
   * @brief Always `true`: the replay cannot know which venue's tape its bars came from.
   *
   * A KRX-listed stock trades on KRX and NXT, and KIS serves either tape or the consolidated one
   * (`KisConfig::market_tape_`, consolidated by default; older datasets may be KRX only). Bars
   * carry no venue, so a replay's bars and a live run's can legitimately differ.
   */
  static constexpr bool bar_tape_unverified = true;

  /**
   * @brief Always `true`: a replay reports every held share as sellable.
   *
   * Live, the broker's figure can be smaller (unsettled, pledged or locked shares), so a sale a
   * replay allows can be refused live under `RiskLimits::enforce_sellable_quantity_`.
   */
  static constexpr bool sellable_is_whole_holding = true;

  /**
   * @brief Always `true`: a replay's buying power is the ledger's own cash, not a broker's figure.
   *
   * The ledger models no settlement; a broker computes buying power on its own terms, and which way
   * an account differs cannot be known here. With `RiskLimits::enforce_buying_power_` on, the cash
   * is reduced by what the replay's own resting buys commit (at their limit price or the last close).
   */
  static constexpr bool buying_power_is_ledger_cash = true;

  /**
   * @brief Bars whose equity total could not be computed exactly (`ReplayResult::unvalued_bars_`).
   *
   * Each has no point on the equity curve and the point after it no return, so the return series is
   * shorter than the bar count by at least this much. Weight-sized intents on such a bar are rejected
   * for want of a portfolio value.
   */
  std::size_t unvalued_bars_{0};
  /** @brief The period end of the first unvalued bar, if any. */
  std::optional<std::chrono::system_clock::time_point> first_unvalued_;
  /**
   * @brief How many stop-limit orders a bar released, each using the assumed direction (a sell stop
   *   releases when the price falls to the trigger, a buy stop when it rises; brokers do not document
   *   it).
   */
  std::size_t stop_releases_{0};
  /**
   * @brief Fills of stops released inside a bar, which assume the price moved continuously through
   *   the trigger.
   *
   * A price that jumped past both trigger and limit (a halt reopening, a thin book) would instead
   * have left the stop resting.
   */
  std::size_t stop_fills_intrabar_{0};
  /**
   * @brief Limit-type fills whose price was moved to stay within the bar's range and the limit
   *   (see `LimitFillPrice::AT_LIMIT`).
   */
  std::size_t limit_fills_bounded_{0};
  /**
   * @brief Bars that closed with cash below zero in the replay's currency: the replay borrowed, which
   *   the default limits allow.
   *
   * Figures measured over borrowed money are leveraged. Short sale proceeds count as cash, so
   * leverage through shorts is not counted here. See `ReplayResult::overdrawn_bars_`.
   */
  std::size_t overdrawn_bars_{0};
  /** @brief The first bar end at which cash was below zero. */
  std::optional<std::chrono::system_clock::time_point> first_overdrawn_;
  /** @brief The lowest cash an execution left, if it went below zero. */
  std::optional<Money> lowest_cash_;
  /** @brief When the lowest cash was reached. */
  std::optional<std::chrono::system_clock::time_point> lowest_cash_at_;
  /** @brief The sale tax given to the fill model; absent if none was modelled. */
  std::optional<SellTax> sell_tax_;
  /** @brief `DAY` orders withdrawn unfilled when their session ended, as they would expire live. */
  std::size_t expired_day_orders_{0};
};

/**
 * @brief The summary of a replay; read `halted_` and `caveats_` before the figures.
 *
 * Unobserved figures follow the `analytics` conventions (`missing()`, `math::undefined()`).
 */
struct Report {
  /** @brief The bars replayed. */
  std::size_t bars_{0};
  /** @brief The executions. */
  std::size_t executions_{0};

  /** @brief Equity at the first recorded point. */
  Decimal64 opening_equity_;
  /** @brief Equity at the last recorded point. */
  Decimal64 closing_equity_;
  /**
   * @brief The total return over the replay, as a fraction; `math::undefined()` when the opening equity is
   *   zero or negative (a replay may start with negative cash).
   */
  math::Scalar total_return_{0};

  /**
   * @brief The per-period Sharpe ratio; see `analytics::sharpe`.
   *
   * `math::undefined()` when equity was zero or negative at any point but the last: the periods that
   * follow have no return, and a ratio over the rest would describe a run that did not happen.
   */
  math::Scalar sharpe_{0};
  /**
   * @brief The annualized Sharpe ratio; `missing()` unless a period count was given to `summarize`.
   *
   * With a period count, undefined whenever `sharpe_` is.
   */
  math::Scalar annualized_sharpe_{0};
  /**
   * @brief The per-period Sortino ratio; see `analytics::sortino`.
   *
   * `math::undefined()` when equity was zero or negative at any point but the last, as `sharpe_`.
   */
  math::Scalar sortino_{0};
  /** @brief The worst peak-to-trough decline of equity, as a non-negative fraction. */
  math::Scalar max_drawdown_{0};
  /**
   * @brief One-way turnover summed over the replay, as a fraction of equity.
   *
   * `missing()` when any execution was in a currency other than the equity curve's (whatever the opening
   * equity), and otherwise `math::undefined()` when the opening equity is zero or negative.
   */
  math::Scalar total_turnover_{0};

  /**
   * @brief Commission paid.
   *
   * Includes the institution fee for executions charged a combined rate
   * (`ReportCaveats::inclusive_commission_executions_`). Absent when nothing executed, when the
   * executions span currencies (see `commission_by_currency_`), or when a sum did not fit.
   */
  std::optional<Decimal64> commission_paid_;
  /** @brief Commission paid per currency. */
  std::vector<Money> commission_by_currency_;
  /**
   * @brief Institution fees (유관기관제비용) paid.
   *
   * Absent unless every execution's rate split the fee out; also under the same currency rule as
   * `commission_paid_`.
   */
  std::optional<Decimal64> institution_fees_paid_;
  /**
   * @brief Institution fees per currency.
   *
   * Partial when `ReportCaveats::inclusive_commission_executions_` is nonzero: it covers only the
   * executions whose rate split the fee.
   */
  std::vector<Money> institution_by_currency_;
  /**
   * @brief Slippage given up, in money.
   *
   * Absent when nothing executed, the executions span currencies, or a sum did not fit; see
   * `slippage_by_currency_`.
   */
  std::optional<Decimal64> slippage_paid_;
  /** @brief Slippage given up per currency. */
  std::vector<Money> slippage_by_currency_;

  /** @brief Instructions not placed: reconciliation could not build them, or the replay cannot model them (`BINDING_REFUSED`). */
  std::size_t rejected_{0};
  /** @brief Instructions a risk guard refused. */
  std::size_t refused_{0};

  /**
   * @brief Why the replay stopped early, if it did; read this before any figure.
   *
   * Set when an execution could not be costed, the ledger could not record one, or positions could
   * not be marked at a bar's close (`mark_not_representable`). The figures cover the bars up to that
   * point; the halting bar is counted in `bars_`, and fills settled on it before the refusal are
   * included. Nothing ran after the refusal, and `on_stop` was not called.
   */
  std::optional<BrokerError> halted_;
  /** @brief Orders cleared from the queue by a halt (unlike `ReportCaveats::unfilled_`). */
  std::size_t aborted_{0};

  /** @brief What this run assumed and did not model. */
  ReportCaveats caveats_;
  /**
   * @brief Sale tax paid.
   *
   * Absent when no tax was modelled (which is not a tax of zero), nothing executed in the taxed
   * market, the executions in the taxed market span currencies, or a sum did not fit. Untaxed
   * executions in other currencies do not affect it.
   */
  std::optional<Decimal64> tax_paid_;
  /** @brief Sale tax paid per currency, over executions in the taxed market. */
  std::vector<Money> tax_by_currency_;
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
 * @brief Summarizes a replay into a `Report`.
 *
 * @param result The replay's result.
 * @param fills The fill model the replay used, so the report states its costs and assumptions.
 * @param periods_per_year Periods in a year, for the annualized Sharpe ratio (see
 *   `backtest::periods_per_year`); absent leaves it `missing()`.
 * @param risk_free The risk-free rate per period, in the units of the returns.
 * @return The report.
 * @throws std::invalid_argument if `periods_per_year` is given, not positive, and the equity curve
 *   is not empty.
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
  report.halted_ = result.halted_;
  report.aborted_ = result.aborted_;
  if (fills.commission().is_scheduled()) {
    report.caveats_.asserted_venue_ = fills.commission().asserted_venue();
  }
  report.caveats_.commission_tariff_ = std::string(fills.commission().tariff());
  report.caveats_.slippage_basis_points_ = fills.slippage().basis_points_;
  report.caveats_.slippage_ticks_ = fills.slippage().ticks_;
  report.caveats_.gaps_ = result.gaps_;
  report.caveats_.unfilled_ = result.unfilled_;
  report.caveats_.stop_releases_ = result.stop_releases_;
  report.caveats_.stop_fills_intrabar_ = result.stop_fills_intrabar_;
  report.caveats_.limit_fills_bounded_ = result.limit_fills_bounded_;
  report.caveats_.quotes_ = result.quotes_;
  report.caveats_.unchecked_fill_causality_ = result.unchecked_fill_causality_;
  report.caveats_.unvalued_bars_ = result.unvalued_bars_;
  report.caveats_.first_unvalued_ = result.first_unvalued_;
  report.caveats_.overdrawn_bars_ = result.overdrawn_bars_;
  report.caveats_.first_overdrawn_ = result.first_overdrawn_;
  report.caveats_.lowest_cash_ = result.lowest_cash_;
  report.caveats_.lowest_cash_at_ = result.lowest_cash_at_;
  report.caveats_.expired_day_orders_ = result.expired_day_orders_;

  // Costs summed exactly, because they are money -- and **per currency**, because a replay whose
  // instruments span markets would otherwise have won added to dollars to make one scalar.
  // Checked, and a refusal is recorded rather than swallowed: leaving the previous total in place
  // and carrying on produced a plausible figure that was short by an unknown amount, which is the
  // one outcome every exact-decimal path here exists to prevent. Two individually representable
  // commissions whose sum does not fit is enough to reach it, and a probe did.
  bool costs_fit = true;
  const auto accumulate = [&costs_fit](std::vector<Money>& totals, const Money& amount) {
    for (auto& held : totals) {
      if (held.currency_ == amount.currency_) {
        const auto sum = decimal_add(held.amount_, amount.amount_);
        if (!sum) {
          costs_fit = false;
          return;
        }
        held.amount_ = *sum;
        return;
      }
    }
    totals.push_back(amount);
  };

  std::vector<InstrumentId> seen;
  for (const auto& execution : result.executions_) {
    accumulate(report.commission_by_currency_, Money{execution.commission_, execution.currency_});
    if (execution.institution_fee_) {
      accumulate(
        report.institution_by_currency_, Money{*execution.institution_fee_, execution.currency_}
      );
    } else {
      // The rate stated one figure, so this execution's commission is inclusive and the split total
      // below cannot be completed. Counted rather than approximated.
      ++report.caveats_.inclusive_commission_executions_;
    }
    // The rate that charged it, collapsed so an unchanged rate is one row with a count rather than
    // one row per fill. Accumulated here, from the executions the ledger accepted, rather than from
    // the schedule: a row the replay never used is not a rate this run applied.
    bool counted = false;
    for (auto& applied : report.caveats_.applied_rates_) {
      if (applied == execution.applied_) {
        ++applied.executions_;
        counted = true;
        break;
      }
    }
    if (!counted) {
      auto entry = execution.applied_;
      entry.executions_ = 1;
      report.caveats_.applied_rates_.push_back(std::move(entry));
    }
    // The tax, totalled per currency, over the executions in the market it was stated for: another
    // market's fills pay none by construction, and counting them would add a zero row in their
    // currency and cost a single-currency total its scalar.
    if (fills.sell_tax() && execution.fill_.instrument_.country_ == fills.sell_tax()->country_) {
      accumulate(report.tax_by_currency_, Money{execution.tax_, execution.currency_});
    }
    // Slippage is a per-share price difference, so its cost is that times the quantity -- and it
    // is totalled per currency like every other cost, because a replay can hold fills in more
    // than one and adding them would produce a figure denominated in nothing.
    if (const auto cost = decimal_multiply(execution.slippage_, execution.fill_.quantity_)) {
      accumulate(report.slippage_by_currency_, Money{*cost, execution.currency_});
    } else {
      costs_fit = false;
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
  report.caveats_.cost_totals_incomplete_ = !costs_fit;
  // A scalar is stated only where one can be true: one currency, something executed, and every
  // addition representable. On a mixed-currency replay the per-currency vectors are the whole
  // answer, and on an arithmetic refusal nothing is stated at all rather than a figure short by an
  // amount nobody can name.
  if (costs_fit && report.commission_by_currency_.size() == 1) {
    report.commission_paid_ = report.commission_by_currency_.front().amount_;
  }
  // Present only when every charged rate split the two components, so the total is complete rather
  // than a sum over the subset that happened to state one.
  if (costs_fit && report.caveats_.inclusive_commission_executions_ == 0 &&
      report.institution_by_currency_.size() == 1) {
    report.institution_fees_paid_ = report.institution_by_currency_.front().amount_;
  }
  if (costs_fit && report.slippage_by_currency_.size() == 1) {
    report.slippage_paid_ = report.slippage_by_currency_.front().amount_;
  }
  report.caveats_.sell_tax_ = fills.sell_tax();
  if (fills.sell_tax() && costs_fit && report.tax_by_currency_.size() == 1) {
    report.tax_paid_ = report.tax_by_currency_.front().amount_;
  }
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

  // From a negative opening the ratio's sign reverses (a replay may start with negative cash), so a
  // non-positive opening has no return and no turnover, as a zero one has none.
  const auto opening = report_detail::to_scalar(report.opening_equity_);
  report.total_return_ = !(opening > math::Scalar{0}) ?
    math::undefined() :
    report_detail::to_scalar(report.closing_equity_) / opening - math::Scalar{1};

  report.sharpe_ = analytics::sharpe(returns, risk_free);
  report.sortino_ = analytics::sortino(returns, risk_free);
  // Equity at or below zero before the end leaves the periods after it without a return
  // (`EquityPoint::period_return_`), so no ratio describes the run.
  for (std::size_t at = 0; at + 1 < result.equity_.size(); ++at) {
    if (result.equity_[at].equity_.coefficient() <= 0) {
      report.sharpe_ = math::undefined();
      report.sortino_ = math::undefined();
      break;
    }
  }
  report.max_drawdown_ = analytics::max_drawdown(levels).magnitude_;

  report.annualized_sharpe_ = periods_per_year ?
    analytics::annualize_ratio(report.sharpe_, *periods_per_year) :
    math::missing();

  // Turnover from the executions rather than from a weight matrix, because a replay knows what it
  // actually traded. One-way, matching `analytics::one_way_turnover`: the fraction of equity that
  // changed hands.
  // Same money on both sides, or no figure at all.
  bool one_currency = true;
  for (const auto& execution : result.executions_) {
    if (!(execution.currency_ == result.currency_)) {
      one_currency = false;
      break;
    }
  }
  report.total_turnover_ = !(opening > math::Scalar{0}) ? math::undefined() : math::Scalar{0};
  if (!one_currency) {
    report.total_turnover_ = math::missing();
  }
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
