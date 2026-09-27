# Reading a backtest honestly {#reading-a-backtest}

A backtester is the easiest place in a library to produce confident, wrong numbers. This page is about
what this one assumes, what it refuses to claim, and which of its figures you should distrust first.

See @ref writing-a-strategy for the strategy side and @ref binding-a-broker for running one live.

## What it is

```cpp
#include <algotradelib/backtest>
#include <algotradelib/vanilla>

backtest::ReplayConfig config;
config.cash_ = atl::Decimal64::parse("100000000");
config.instruments_ = {reference};          // reference data, with its PriceRule

backtest::Backtester<vanilla::SmaCrossover> engine(
  vanilla::SmaCrossover{params},
  std::move(config),
  backtest::NextBarFillModel(
    backtest::CommissionModel::of(atl::Decimal64::parse("0.00015")),
    backtest::SlippageModel::of_ticks(1)
  )
);

for (const auto& bar : series.bars_) {
  engine.feed(strategy::make_bar_event(series.instrument_, series.interval_, series.stamp_, bar));
}
engine.finish();

const auto report = backtest::summarize(engine.result(), engine.fills(), periods_per_year);
```

The **third** configuration of `Runner`, after `DryRunner` and the session-backed one. A strategy's
source is identical in all three; if it were not, the surface would be wrong.

## The one guarantee worth relying on

**A fill never comes from the bar that produced the order.** `NextBarFillModel::decide` takes the next
bar and nothing else — it has no way to see the signal's own bar, so an engine wanting to cheat would
have to pass the wrong one deliberately.

That is why `Runner`'s executor accepts an order without filling it. The ordering inside `feed` is the
whole correctness argument:

1. fill what the previous bar asked for, against this bar — **before** the strategy sees it
2. apply the fills and tell the strategy
3. mark positions and record equity
4. advance the clock, fire a phase change
5. show the strategy the bar, which may queue orders for the *next* one

Reversing 1 and 5 is exactly the lookahead defect. Doing 3 before 2 would value a portfolio not yet
updated by its own fills.

An order submitted on the **last** bar never fills, and is counted in `caveats_.unfilled_`. There is no
next bar, and inventing one would be inventing data.

## What the fill model assumes

| | Behaviour | Why |
| --- | --- | --- |
| Market order | fills at the next bar's open | the only price the next bar states before trading |
| Buy limit | fills if the bar's low reached it | a bar that never touched the price is one the order sat through |
| Sell limit | fills if the bar's high reached it | the same, mirrored |
| Limit price | **at the limit, never better** | see below |
| Missed limit | stays queued | discarding it would turn every limit into fill-or-kill |

**The limit-price choice is the one to understand.** A bar opening at 69,000 through a buy limit of
70,000 would often fill better in reality. Claiming that improvement assumes the order was in the book
before the auction *and* that volume existed at that level — neither of which a bar can tell you.
Refusing it understates a result; claiming it overstates one, and a backtester that flatters is worse
than one that disappoints.

`LimitFillPrice::AT_OPEN_IF_BETTER` is the optimistic variant, opt-in because the assumption is yours to
make. Whichever you use, `report.caveats_.limit_fill_` says which.

## What it does not model at all

`ReportCaveats::volume_ignored` is always true, and present so you are told rather than left to assume:

- **Volume.** No fill is partial or capped. A bar's volume says how much changed hands in total, not how
  much was available to *your* order at *your* price. A participation rate would be a number with no
  data behind it.
- **Queue position.** The largest hidden cost in real execution, and invisible to bar data.
- **Market impact.** Your order is assumed not to move the price.
- **Borrow availability.** A short is assumed possible.
- **Settlement.** A sale's proceeds are credited on the fill and the whole holding is reported as
  sellable, so a replay trades in a T+0 world. `ReportCaveats::sellable_is_whole_holding` and
  `buying_power_is_ledger_cash` are both always true and present for the same reason as
  `volume_ignored`.

If a strategy's edge is small relative to these, the backtest cannot tell you whether the edge is real.

## What a replay says an account can do

The live path can ask the broker what may be spent and what may be sold. A replay has no broker, so
it answers from its own ledger — and **the two answers are different quantities wearing the same
field**:

- `buying_power_` is the ledger's cash, which the ledger credits on the fill. A live account's
  buying power is priced on the broker's terms, including or excluding amounts this ledger does not
  model. Which way a particular account differs is not something a replay can know, so the report
  states the model rather than a direction.
- `sellable_quantity_` is the whole holding. Live, it is routinely smaller — settlement, pledging,
  locking — so a sale a live account would have refused is not refused here.

The consequence worth keeping in mind: **a strategy sized against the sellable check passes more
easily in a replay than live**, because the whole holding is an upper bound on what a broker will
let you sell. For buying power there is no such direction — ledger cash may be above or below what a
broker would answer — so the difference is real but not one you can lean on either way. Either way,
if you enable `enforce_buying_power_` or `enforce_sellable_quantity_`, a clean backtest is not
evidence the live account will accept the same orders. `plan/todo/replay-settlement-model.md` is
what would close the sellable half.

## The equity curve is one currency's, and now genuinely so

`ReplayConfig::currency_` names the currency the report is in, and the equity curve is
`ledger.equity_in()` of it at every bar.

That figure used to count **every** position whatever it was denominated in, so a won-quoted holding
was added into a dollar equity curve at a rate of one — the implicit conversion `holdings-core` exists
to refuse, in the number every derived metric is computed from. It now counts only positions in the
currency being asked about, and the ledger records each position's currency from the fill that opened
it.

The engine had the matching problem one step earlier: it booked every fill in `ReplayConfig::currency_`
regardless of the instrument, which is documented as the currency the *starting cash* is in. A
dollar-quoted instrument therefore banked dollars as won before the ledger saw a currency at all. It
now books a fill in the instrument's own currency, from the reference data the replay already needs.

**What that means for a multi-currency replay.** Each currency is tracked apart and correctly, and a
replay that opens with won and trades a dollar instrument runs its dollar cash negative from zero,
because funding it would need a conversion this library does not do. The report still curves one
currency, so read it as one currency's result rather than the account's.

## Costs, which are the most common way a backtest lies

A strategy evaluated at zero cost can look excellent and lose money at any real one. So costs are models
you state, and `analytics` deliberately carries none of its own:

```cpp
backtest::CommissionModel::of(rate, per_order_minimum);
backtest::SlippageModel::of_ticks(1);          // one tick at the instrument's own grid
backtest::SlippageModel::of_basis_points(bps); // a fraction of the price
```

Slippage always moves **against** the order — slippage that helped would not be slippage, and a model
cutting both ways would be right on average and flattering in exactly the cases a strategy trades most.
A tick means a tick *at that price level*, through the instrument's `PriceRule`, so it is 100 won at
70,000 and 50 at 30,000; an unknown grid contributes nothing rather than a guessed step.

**Charging nothing is itself a model**, and the report says so. A zero-cost run should be your informed
choice, not an oversight you cannot detect.

**Or cost each execution from a schedule.** `backtest::CommissionPolicy::scheduled(schedule, country,
venue)` resolves a rate **per execution**, against that execution's market-local date and the venue
you assert — so a replay spanning a rate change charges each rate on its own dates instead of one
rate throughout, and a KRX fill and an NXT fill can differ. The schedule can be the one a broker
published (Toss does; KIS publishes none for domestic stock, and
`backtest::commission_model_from(schedule, country, date)` remains for the older one-date shape) or
one you state from your own fee page.

Three things to know before you rely on it:

- **The venue is asserted, never observed.** A replay cannot know where a fill executed — a bar does
  not say, and the broker fields that would are undocumented. `ReportCaveats::asserted_venue_`
  records that you asserted it, and the figure is exactly as good as the assertion.
- **A caller-stated schedule must name its tariff**, because a published fee table is channel by
  venue and the channel is a property of your account that no API states. A schedule without one is
  refused rather than defaulted to a plausible row.
- **A date no window covers halts the replay**, naming the date. It is not a free trade and not a
  fill that quietly did not happen: the refused trade would have changed everything the strategy did
  afterwards, so the run stops and `Report::halted_` says why.

`ReportCaveats::applied_rates_` then lists the rates that actually **charged** something, each with
its origin and how many executions it charged — not the rows the schedule happens to contain.

No tax model, deliberately: Korean securities transaction tax has changed repeatedly, and a hard-coded
number that moves would silently misprice every run after it changed. State it in the rate for sells if
you want it.

## Annualizing, and the trap in it

`summarize` takes `periods_per_year` and has no default, which is the same reason
`analytics::annualize_ratio` does. The number is **245** for daily bars on KRX and **95,550** for minute
bars over the same year.

`backtest::periods_per_year(interval, session, trading_days)` derives it. A calendar interval — weekly,
monthly — returns nothing rather than a plausible constant, because a week's divisor depends on the
market's week.

The trap, which `example-backtest-offline` demonstrates: the same strategy over the same path at two
granularities reports an identical per-bar Sharpe of 0.308, annualizing to **4.82** daily and **95.21**
at minute frequency. Both are arithmetically right. **Mislabel your bar frequency and the annualized
figure is wrong by the square root of the ratio.** And annualizing assumes returns are serially
uncorrelated, which a trending strategy violates by construction — so treat an annualized Sharpe as an
order of magnitude, not a measurement.

Absent a period count, `annualized_sharpe_` stays unobserved rather than being scaled by a guess.

## Absent figures mean something

The metrics keep `analytics`'s two kinds of absence, and the difference is diagnostic:

- `missing()` — not enough data. An empty replay leaves *every* figure unobserved rather than zero,
  because zero reads as a flat result rather than an absent one.
- `undefined()` — the data was there and the statistic does not exist. A Sortino ratio over a run that
  never lost has no downside to divide by; `math::is_undefined()` tells it from a warm-up.

A report that showed `0.0` where a figure did not exist would be the worst kind of wrong.

## Survivorship bias, which the engine cannot fix

`caveats_.multiple_instruments_` is set whenever more than one instrument traded. Broker reference data
lists instruments that exist **now**, so a universe assembled from it silently excludes everything
delisted during the period — and what gets delisted is disproportionately what did badly. A
cross-sectional result built that way is flattered by an amount nothing in the engine can measure.

This is a property of the data, not of the replay, so the engine states it rather than correcting it.
Single-instrument backtests are unaffected.

Read the flag as a **caution, not a finding**. The bias comes from how you *assembled* the universe, and
the engine cannot see that — it only sees how many instruments arrived, so the flag is a proxy and it has
false positives: a hand-picked pair you know was listed for the whole period sets it and carries no
survivorship problem at all.

That is also why the report still computes every metric instead of refusing. Refusing would block a
replay that is perfectly sound in order to prevent a mistake the engine has no way to detect. You know how
you picked the universe; the engine does not, so it tells you what it saw and leaves the judgement with you.

## Intraday

The engine is interval-agnostic by construction, and that is under test: the same strategy replayed at
daily and minute granularity is asserted **equal to itself** on execution count, position, cash and
realized profit. Nothing multiplies by 245 or divides by 24 hours; the clock advances by each bar's own
`period_end_`, and phases come from a `SessionWindow`.

What limits intraday work today is *data*, not the engine. KIS serves minute bars one trading day at a
time and crossing days needs a calendar walk that does not exist yet. A broker that serves intraday
history, or your own bar files, replays without any engine change.

A gap between sessions is counted in `caveats_.gaps_` and **not interpolated across** — nothing happened
in it that a strategy should be told about.

## What to distrust first

In rough order:

1. **A small edge.** If the result is within the range of what volume, queue position and impact could
   account for, the backtest has not measured it.
2. **An annualized ratio**, for the reasons above.
3. **A cross-sectional result** with `multiple_instruments_` set.
4. **Anything from a run whose `caveats_` you have not read.** They are fields precisely so you cannot
   forget which assumptions applied.

And the honest summary: this engine is good for telling a broken strategy from a plausible one, and for
comparing two strategies under identical assumptions. It is not good for predicting a return.
