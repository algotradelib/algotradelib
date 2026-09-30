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
  /**
   * Every rate that actually charged an execution, with how many it charged.
   *
   * **This replaced a single `commission_rate_`, and the reason is that one number could not tell
   * the truth about two.** A replay spanning a rate change charges each rate on its own dates, and a
   * replay costing by venue charges a KRX fill and an NXT fill differently; a scalar could state
   * only one of them, and which one was an accident of construction order.
   *
   * **These are the rates *used*, not the rates *available*.** A schedule row that covered dates the
   * replay never traded on, or a venue it never asserted, does not appear -- and neither does one
   * whose execution the ledger could not apply. A reader comparing this table against a broker's
   * schedule is comparing what was charged against what was published, which is the comparison worth
   * making.
   *
   * Empty on a zero-cost run, which is stated rather than implied: `AppliedRate::rate_` is zero and
   * the entry is present, so a reader sees that nothing was charged rather than that nothing is known.
   */
  std::vector<AppliedRate> applied_rates_;
  /**
   * How many executions were charged by a rate that did **not** distinguish an institution component.
   *
   * Nonzero means `Report::commission_paid_` is *inclusive* for those executions -- the source stated
   * one figure and said nothing about its composition, which is every rate Toss publishes -- and it
   * is why `Report::institution_fees_paid_` is absent whenever this is nonzero. A zero there would
   * claim an institution charge was levied and came to nothing.
   */
  std::size_t inclusive_commission_executions_{0};
  /**
   * The venue every scheduled cost was computed at, and it was **asserted rather than observed**.
   *
   * A replay cannot know where a fill executed: a bar does not say, `PlannedOrder::venue_` is routing
   * intent, and the KIS response fields that would say are undocumented. So a caller states the venue
   * and this records that they did, in the same shape as `bar_tape_unverified` -- the figure is as
   * good as the assertion and a reader can see the assertion.
   *
   * Empty when no scheduled policy was used, which is a run that cost nothing by venue at all.
   */
  std::optional<VenueKey> asserted_venue_;
  /** Which tariff the schedule named, empty on a caller-stated flat model. */
  std::string commission_tariff_;
  /**
   * Whether a cost total could not be summed exactly, so the figures above are **incomplete**.
   *
   * True means at least one exact-decimal step was refused -- an addition of two representable
   * amounts whose sum does not fit, or a slippage multiplication that does not -- and the amount
   * was left out rather than approximated. Every scalar cost total goes absent when it is set, and
   * the per-currency vectors hold only what could be accumulated. It takes figures far larger than
   * any real account to reach; it is reported because a money total that is quietly short is worse
   * than one that says it is short.
   */
  bool cost_totals_incomplete_{false};
  /** Basis points of slippage applied. */
  Decimal64 slippage_basis_points_;
  /** Tick steps of slippage applied. */
  int slippage_ticks_{0};

  /**
   * Whether more than one instrument traded, which is a **caution rather than a finding**.
   *
   * Survivorship bias comes from how a universe was *assembled*, not from how many instruments are in
   * it. Broker reference data lists what exists **now**, so a universe built by asking a broker what it
   * trades silently excludes everything delisted during the period — and what gets delisted is
   * disproportionately what did badly, which flatters the result by an amount nothing here can measure.
   *
   * The engine cannot observe how the universe was chosen, so this flag is a proxy, and it has false
   * positives by design: a hand-picked pair you know was listed throughout the period carries no
   * survivorship problem at all and still sets it.
   *
   * That asymmetry is why the report **flags and still computes** rather than refusing. Refusing would
   * block a legitimate replay in order to prevent a mistake the engine cannot actually detect, and a
   * library that declines to answer a well-formed question teaches its users to work around it.
   */
  bool multiple_instruments_{false};

  /** Bars that did not follow their predecessor; a replay does not interpolate across a gap. */
  std::size_t gaps_{0};

  /** Orders still queued when the replay ended, never filled. */
  std::size_t unfilled_{0};

  /**
   * Quotes this replay was fed, and therefore whether any reached the dispatcher at all.
   *
   * **Zero and non-zero say different things and both are worth saying.** Zero is a replay of bars
   * alone, which is every replay written before quotes could be fed and the ordinary case; non-zero
   * means quotes were fed and dispatched, and carries the caveat below with it. It does **not** say
   * a strategy handled one -- `on_quote` is optional -- or that anything was placed or filled
   * because of one.
   *
   * **What a reader has to know when this is non-zero: an order placed from `on_quote`, if it fills
   * at all, fills against a later *bar* and not against the quote.** (If at all: a limit can miss
   * the one session it has and expire, counted in `expired_day_orders_`, or still be working when the
   * run ends, in `unfilled_`.) A quote moves the clock, the price and the
   * phase, and it neither fills nor marks: `NextBarFillModel` prices a market order at a bar's open,
   * so -- **where this engine can establish when a bar's period began** -- the order rests until one
   * that began at or after the decision, because a period that began earlier contains the decision
   * and its open is a price from before the strategy decided anything. For a quote part-way through
   * a session that is not that session's bar but the next one; for a quote between a close and the
   * next open it is the very next bar. Where it **cannot** establish it -- a close-stamped weekly,
   * monthly or yearly bar -- no such rule applies and the fill can happen at a price from before the
   * quote, which is what `unchecked_fill_causality_` exists to say. Live, the same order reaches the
   * broker immediately and can trade at once.
   *
   * So a quote-driven strategy's replayed fills **can be** later, and at a different price, than the
   * same strategy's live ones -- by a bar where the quote fell inside a period, and by whatever a
   * bar's open differs from the quote otherwise. Can be, rather than are: a replay knows nothing
   * about when a live order would have executed, or whether it would have at all. That is the place a quote-driven backtest and a quote-driven live run
   * are known to differ, and it is stated here rather than in prose a reader has to remember applied
   * to their run. A fill model that executes against a quote would close it, and is a different
   * design with its own argument.
   */
  std::size_t quotes_{0};

  /**
   * How many times this replay allowed a fill it could **not** check for causality, because one of
   * the two instants the check compares was a calendar boundary it cannot establish: the bar's
   * period start, or the decision's own instant.
   *
   * **Eligibility decisions, not fills**: one per queued order per bar it was considered against, and
   * the fill model may decline it afterwards.
   *
   * **A calendar period boundary the engine cannot establish, on either side of the comparison.** A
   * close-stamped weekly, monthly or yearly bar does not state where its period *began* -- three
   * attempts to infer that from a previous bar were each falsified in review. And an open-stamped bar
   * of any calendar interval does not state where its period *ended*: that is `bar_period_end`'s
   * nominal 24 hours, 7, 31 or 366 days, which is what **every shipped adapter produces for every
   * non-minute bar**, so an adapter-fetched monthly or yearly replay carries this on most of its
   * fills. **Not on all of them**: a nominal end is an upper bound, so whenever the next period's
   * start reaches it the check still passes -- which is every Monday-to-Monday week, and is why a
   * clean weekly series counts nothing at all. The figure rises when a candidate start falls
   * *before* that bound.
   *
   * **What non-zero tells a reader**: for those, the fill price is not guaranteed to come from after
   * the decision. **Any dated order can be one**, and a quote is only the obvious way in -- a
   * bars-only replay reaches it too. **A reader can discharge it**: with no quotes, one interval per
   * instrument, and series whose periods do not overlap, the fills were causal whatever this figure
   * says -- the engine simply could not prove it from the events it was handed. See
   * `ReplayResult::unchecked_fill_causality_`.
   */
  std::size_t unchecked_fill_causality_{0};

  /**
   * Always true, and present so a reader is told rather than left to assume.
   *
   * No fill is capped by what traded: a bar's volume says how much changed hands in total, not how much
   * was available to this order at its price. Queue position, market impact and borrow availability are
   * likewise unmodelled.
   */
  static constexpr bool volume_ignored = true;

  /**
   * Always true, and the honest value: **a replay cannot know which book its bars describe.**
   *
   * A KRX-listed equity trades on two venues, and KIS publishes market data for KRX alone, for NXT
   * alone, or for the consolidated tape over both. `KisConfig::market_tape_` chooses which a live run
   * reads, and it is consolidated by default; a dataset recorded before that setting existed came from
   * KRX alone on every verb but the quote.
   *
   * Nothing carries that provenance. `BarSeries`, `strategy::BarEvent`, `SeriesKey` and
   * `analytics::SeriesFrame` all name an instrument and an interval and nothing about a venue, and
   * `summarize` is handed neither the config nor the bar source -- so a KRX dataset and a consolidated
   * one produce identical inputs here. **This says unverified rather than naming a tape, because
   * naming one would be a claim this code cannot support**, and a wrong claim would conceal exactly
   * the backtest-versus-live mismatch a caveat exists to expose.
   *
   * The consequence for a reader: two bars for one instrument at one timestamp can legitimately differ
   * between a replay and a live run, and the difference is which venues were included rather than an
   * error in either.
   */
  static constexpr bool bar_tape_unverified = true;

  /**
   * Always true, and present for the same reason: a replay reports every held share as sellable.
   *
   * Live, that figure comes from the broker and is routinely smaller than the holding — settlement,
   * pledging and locking all reduce it, which is what `RiskLimits::enforce_sellable_quantity_`
   * exists to respect. A replay has no broker to ask and models no settlement schedule, so it
   * reports the whole position, and a strategy that would have been refused a sale live is not
   * refused one here.
   *
   * This is a constant rather than a field because there is nothing yet to vary it: a ledger that
   * models settlement would make it one, and that is the point at which this becomes a number
   * rather than a disclosure.
   */
  static constexpr bool sellable_is_whole_holding = true;

  /**
   * Always true: a replay's buying power is modelled from this ledger's own cash, never a broker's
   * answer.
   *
   * The two are different quantities. The ledger credits a sale on the fill and models no
   * settlement schedule; a broker prices buying power on its own terms, including or excluding
   * amounts this ledger does not model at all. **Which way a particular account differs is not
   * something a replay can know**, so this states the model rather than claiming the replay is the
   * more permissive of the two.
   *
   * **With `RiskLimits::enforce_buying_power_` enabled, the figure is that cash less what this
   * replay's own outstanding buys have committed**, valued at each order's limit price or the last
   * close. Ledger cash alone has not paid for anything still resting, so two resting buys of a
   * hundred would each pass against a hundred of cash and both could still fill. The commitment is
   * this library's arithmetic rather than a broker's reservation, which is why it is disclosed here
   * and why it applies only when the check is on: `buying_power_in()` is a figure a strategy may
   * size from, and lowering it for a caller who never asked for the check would change how their
   * strategy behaves.
   *
   * A strategy sized against a buying-power limit therefore passes or fails here on modelled money,
   * and the live account is the only place the broker's own answer applies.
   */
  static constexpr bool buying_power_is_ledger_cash = true;

  /**
   * Bars whose equity total could not be computed exactly, copied from
   * `ReplayResult::unvalued_bars_`: each has no point on the curve, and the point after it carries
   * no return, so the return series is **shorter than the bar count** by at least this much. Only
   * the curve is affected: the strategy and its guards decided on those bars from per-lot and
   * per-currency figures, none of which is the missing total (see the result's field).
   * Appended, so a positional initializer of this struct is unaffected.
   */
  std::size_t unvalued_bars_{0};
  /** The first such bar's period end; empty when there was none. */
  std::optional<std::chrono::system_clock::time_point> first_unvalued_;
  /**
   * Stop-limit orders a bar released, which is how often the model's **direction assumption** was
   * used: a sell stop releases when the price falls to its trigger and a buy stop when it rises to it.
   * Neither KIS nor Kiwoom documents that; it is the universal convention.
   */
  std::size_t stop_releases_{0};
  /**
   * Fills that depended on a stop releasing **inside** a bar and filling on it.
   *
   * A bar has no path between its open and its extremes, so the model assumes the price moved
   * continuously through the trigger. A stop whose limit was marketable at the trigger is credited the
   * worst price the bar traded that its limit allows; one whose limit was not is filled only when the
   * close proves the price came back to it, at the limit. A price that jumped inside the bar past both
   * trigger and limit (a halt reopening, a thin book) would have released the stop and left it resting,
   * so each of these is a fill that rests on that continuity. Every stop fill, intrabar or not, is
   * credited a price inside its bar and never through its limit, slippage included.
   */
  std::size_t stop_fills_intrabar_{0};
  /**
   * Limit-basis fills whose credit the bound moved: onto the bar, back to the limit, or back from the
   * bar's opposite extreme. (A stop released inside its bar is *chosen* inside both, and counts only
   * if a later step moved it.)
   *
   * A limit is credited the worst price its bar traded that the limit allows: a buy no higher than the
   * lesser of the bar's high and its limit, a sell no lower than the greater of the low and its limit.
   * `AT_LIMIT` credits the limit itself, which lies outside the bar whenever the limit was marketable
   * through all of it, and slippage can carry any credit out of the bar or through the limit. Each
   * count is a fill whose recorded price the bar, not the configuration, decided. Appended.
   */
  std::size_t limit_fills_bounded_{0};
  /**
   * Bars that closed with the ledger's cash below zero in the replay's currency: the replay borrowed,
   * which the default limits allow; see `ReplayResult::overdrawn_bars_`. A figure measured over
   * borrowed money is a leveraged figure, whether or not the strategy meant to be leveraged. Cash
   * only: a short's proceeds count as cash, so leverage through shorts is not measured here.
   */
  std::size_t overdrawn_bars_{0};
  /** The first bar end at which cash was below zero. */
  std::optional<std::chrono::system_clock::time_point> first_overdrawn_;
  /** The lowest cash an execution left, when it went below zero. */
  std::optional<Money> lowest_cash_;
  /** When that lowest cash was reached. */
  std::optional<std::chrono::system_clock::time_point> lowest_cash_at_;
  /** The tax on sales, as stated to the fill model; absent when no tax was modelled. Appended. */
  std::optional<SellTax> sell_tax_;
  /**
   * DAY orders withdrawn unfilled at the end of their session; see `ReplayResult`. Non-zero is
   * ordinary for a limit or stop strategy: live, those orders expired at the close too. Appended.
   */
  std::size_t expired_day_orders_{0};
};

/** What a replay produced, with every figure's absence meaning what `analytics` says it means. */
struct Report {
  /** Bars replayed. */
  std::size_t bars_{0};
  /** Executions the fill model produced. */
  std::size_t executions_{0};

  /** Equity at the first and last recorded points. */
  Decimal64 opening_equity_;
  /** @copydoc Report::opening_equity_ */
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
  /**
   * One-way turnover summed over the replay, as a fraction of equity traded.
   *
   * **Unobserved when any execution was in a currency the equity curve is not**, because the
   * numerator and the denominator would then be different money. A replay holding a won fill and
   * a dollar fill produced a perfectly ordinary-looking ratio that added two hundred dollars to
   * seven hundred thousand won; a whole-portfolio turnover across currencies needs an exchange
   * rate this library does not carry, and inventing one would misprice the figure rather than
   * withhold it.
   */
  math::Scalar total_turnover_{0};

  /**
   * Commission paid, so a reader can see what the model cost.
   *
   * **Inclusive of any institution charge for executions whose rate did not distinguish one** --
   * `ReportCaveats::inclusive_commission_executions_` counts those. Where every rate did distinguish
   * one, this is the broker's component alone and `institution_fees_paid_` carries the other.
   *
   * **Absent on a replay whose executions span currencies**, rather than a sum of unlike ones:
   * `commission_by_currency_` is the answer there, and a scalar could only have been produced by
   * adding won to dollars. Absent also on a replay that executed nothing, which is not a charge of
   * zero -- a zero-cost run that *traded* states a zero here, with a zero rate in `applied_rates_`.
   */
  std::optional<Decimal64> commission_paid_;
  /** Commission paid per currency, which is always stated whatever the scalar above does. */
  std::vector<Money> commission_by_currency_;
  /**
   * Korea's 유관기관제비용, totalled separately because a different party charges it.
   *
   * **Absent rather than zero when any charged rate stated a single figure**, because the component
   * is then inside `commission_paid_` and nobody said how much. Present only when every execution was
   * charged by a rate that split the two, which makes the total complete rather than partial -- and
   * subject to the same currency rule as `commission_paid_`.
   */
  std::optional<Decimal64> institution_fees_paid_;
  /**
   * The institution component per currency, on the same terms as `commission_by_currency_`.
   *
   * **Partial when `ReportCaveats::inclusive_commission_executions_` is nonzero.** It then holds the
   * component for the executions whose rate distinguished one and nothing for the rest, whose
   * institution charge is inside `commission_by_currency_` in an amount nobody stated. The scalar
   * above goes absent in that case; this vector does not, because a known subset is worth having
   * and the count beside it says the subset is not the whole.
   */
  std::vector<Money> institution_by_currency_;
  /**
   * Slippage given up, on the same terms as `commission_paid_`.
   *
   * **Absent for the same three reasons**: nothing executed, the executions spanned currencies, or
   * an exact-decimal step was refused. It used to be a bare scalar summed across every execution
   * regardless of currency, so a replay holding one won fill and one dollar fill reported a number
   * that was neither -- the one arithmetic every monetary type here exists to prevent, left in the
   * one cost figure that had not been given a currency. `slippage_by_currency_` is the answer
   * whenever a scalar cannot be.
   */
  std::optional<Decimal64> slippage_paid_;
  /** Slippage given up per currency, which is always stated whatever the scalar above does. */
  std::vector<Money> slippage_by_currency_;

  /** Instructions reconciliation could not build; a replay's executor refuses nothing. */
  std::size_t rejected_{0};
  /** Instructions a guard refused. */
  std::size_t refused_{0};

  /**
   * Why the replay stopped early, when it did — **read this before any figure above it**.
   *
   * Present means the replay stopped early, and there are three origins: an execution whose cost
   * could not be resolved, one the ledger could not record, and a book that could not be marked at a
   * bar's close (`mark_not_representable`). The first two mean a trade the model produced did not
   * reach the books; the third means the strategy would have decided on a position one bar behind
   * its price. Either way continuing would describe a path that did not happen.
   *
   * Every number in this report describes the bars **up to** that point. The bar the halt happened
   * on is counted in `bars_`, because it was fed, and **what ran on it depends on where the refusal
   * fell**. Each bar settles resting orders first: settlement ahead of a refused fill may already have
   * filled other orders on that bar, with their `on_fill` and order updates, and a mark refusal comes
   * after the whole of that bar's settlement, so every fill due on it is in these figures. After the
   * refusal nothing ran -- no further fill, no mark, no equity point, no `on_phase`, no `on_bar`.
   * The strategy was not stopped normally either: `on_stop` never ran, because whatever it placed
   * would go into a replay that has stopped being able to describe the path honestly.
   */
  std::optional<BrokerError> halted_;
  /** Orders cleared from the queue by a halt, as distinct from `ReportCaveats::unfilled_`. */
  std::size_t aborted_{0};

  /** What this run assumed, carried with the figures so a reader need not remember. */
  ReportCaveats caveats_;
  /**
   * The tax charged on sales, where the fill model stated one (`NextBarFillModel::with_sell_tax`).
   * Absent when none was stated -- which is not a tax of zero, it is no tax modelled -- when nothing
   * executed in the stated market, and, like the other cost totals, when those executions span more
   * than one currency or a sum did not fit. Appended last, as every field added to this aggregate is.
   */
  std::optional<Decimal64> tax_paid_;
  /** The same, per currency, over the executions in the stated market. */
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
  // Same money on both sides, or no figure at all.
  bool one_currency = true;
  for (const auto& execution : result.executions_) {
    if (!(execution.currency_ == result.currency_)) {
      one_currency = false;
      break;
    }
  }
  report.total_turnover_ = opening == math::Scalar{0} ? math::undefined() : math::Scalar{0};
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
