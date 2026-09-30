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
| Limit price | **the limit itself under `AT_LIMIT`** — or, when the limit was marketable through the whole bar, the bar's worst price instead (a buy its high, a sell its low), which is better than the limit but never better than anything the bar traded | see below; a price the bar never showed is not a fill |
| Slippage on a limit | never carries the fill through its limit or out of its bar | a limit order that could only have traded worse did not trade worse; `caveats_.limit_fills_bounded_` counts both |
| Missed limit | stays queued | discarding it would turn every limit into fill-or-kill |

**The limit-price choice is the one to understand.** A bar opening at 69,000 through a buy limit of
70,000 would often fill better in reality. Claiming that improvement assumes the order was in the book
before the auction *and* that volume existed at that level — neither of which a bar can tell you.
Refusing it understates a result; claiming it overstates one, and a backtester that flatters is worse
than one that disappoints.

So `AT_LIMIT`, the default, never credits the better **open**. It is still not "never better than the
limit": a limit marketable through the whole bar is credited the bar's worst price — a buy limited at
72,000 on a bar whose high is 71,000 fills at 71,000 — because the limit itself is a price nobody traded.
**Backtests from before this rule can therefore show better limit fills with no configuration change**;
`report.caveats_.limit_fills_bounded_` counts every fill the bar moved.

`LimitFillPrice::AT_OPEN_IF_BETTER` is the optimistic variant: it credits the open when the bar opened
through the limit, opt-in because the assumption is yours to make. Whichever you use,
`report.caveats_.limit_fill_` says which.

**A stop-limit order is dormant until a bar reaches its trigger**, then the limit it becomes:

| | Behaviour | Why |
| --- | --- | --- |
| Release | a sell stop when the bar's low reaches the trigger; a buy stop when its high does | the universal convention; **neither broker documents it**, and `caveats_.stop_releases_` counts how often it was used |
| Opened through the trigger | priced as a limit against the whole bar, under your `LimitFillPrice`, and dated from the open when the open was marketable | the whole bar followed the release |
| Any stop fill | credited a price **inside the bar** and never through the limit — a sell no lower than the higher of the low and its limit, a buy no higher than the lower of the high and its limit | an `AT_LIMIT` credit, or slippage, could otherwise name a price the bar never showed |
| Released inside the bar, limit marketable at the trigger | fills on that bar at **the worst price the bar traded that the limit allows** — a sell the higher of its limit and the low, a buy the lower of its limit and the high | the price at release was the trigger; crediting the limit itself could name a price the bar never reached |
| Released inside the bar, limit not marketable there | fills at the limit on that bar only if the **close** is at or through it, else rests from the next bar | the close is the one price known to come after the release |
| A bar with no open | a release is always "inside the bar" | a close cannot stand in for an open the bar did not state |
| Slippage | never carries a stop fill out of its bar or through its limit — the same bound as a plain limit | a limit order that could only have traded worse did not trade |

`caveats_.stop_fills_intrabar_` counts the fills that relied on a release inside the bar. Each assumes
the price moved continuously through the trigger; a jump past both trigger and limit would have left
the stop released and unfilled. A working stop is shown to the strategy as a stop with its trigger,
released or not, because neither broker's listing says which.

## Quotes, and the one place a quote-driven replay differs from a quote-driven live run

`Backtester::feed` takes a `strategy::QuoteEvent` as well as a `BarEvent`, so `on_quote` fires in a
replay. It did not before: `QuoteEvent` appeared nowhere in `backtest/`, while `PollKind::QUOTES` is
a supported live cadence — so a strategy that acts between bars ran live having been evaluated
against nothing. Interleave quotes and bars in timestamp order yourself, exactly as you already
interleave bars across instruments.

Sketched — `bar` and `quote` stand for events you build from your own data:

```
engine.feed(bar);      // yesterday's close  -> on_bar
engine.feed(quote);    // today 10:30        -> on_quote; an order placed here cannot fill
                       //                       against today's bar, whose open was 09:00
engine.feed(bar);      // today's close      -> on_bar
engine.feed(bar);      // tomorrow's close   -> a market order from the quote fills here, at
                       //                       tomorrow's open; a limit may miss and stay queued
engine.finish();
```

**A quote does the same thing in both modes, because both go through the same function.**
`Runner::feed(const QuoteEvent&)` sets the price a market order is valued at, advances the decision
clock to the quote's own instant, and calls the handler; live, the quote poll calls exactly that. What
a replay arranges around it is an approximation of the rest of a live iteration, and the last three
rows below say which way each one errs:

| | Live | Replay |
| --- | --- | --- |
| Updates the price a market order is valued at | yes | yes, the same call |
| Advances the clock a decision is stamped with | yes | yes, the same call |
| Re-derives the market phase at that instant | yes, before every due poll | yes |
| Announces an order placed from the handler | when the working-order poll is due | yes, always |
| Marks the book, records an equity point, fills anything | no ledger exists live | **no** |

The phase row matters more than it looks: `RiskLimits::permitted_phases_` defaults to `{OPEN}`, so a
replay that derived the phase only from bars would have shown a mid-session quote the *previous
close's* phase and refused an order a live run would have sent. A change is announced **before**
`on_quote` here and after all polls live, which is the ordering `feed(BarEvent)` has always used; an
order placed from `on_phase` is therefore visible to `on_quote` in a replay and would not yet exist
live. And the announcement of an order is unconditional here and conditional live, on the
working-order poll being due at all.

**So an order placed from `on_quote`, if it fills at all, fills against a later *bar*, at that bar's
price** — and, where
this engine can establish when a bar's period began, specifically against one that began **at or
after** the decision. For a quote part-way through a session that is not that session's bar but the
next one. A period that began earlier *contains* the decision, and its open is a price from before
the strategy decided anything. Today's daily bar opened at 09:00, and
crediting a 09:00 price to a 10:30 decision would be lookahead with the sign flipped. Live, the same
order reaches the broker immediately and can trade at once. `ReportCaveats::quotes_` is non-zero on
any run that fed a quote, and that is what it is telling you. A fill model that executes against a
quote is a different design with its own argument, and this is not it.

**A calendar period boundary the engine cannot establish makes the check impossible, and the report
says so rather than guessing.** It happens on either side of the comparison. A close-stamped weekly,
monthly or yearly bar does not state when its period *began* — and nothing in `feed`'s contract lets
the engine derive it, as three attempts to infer it from a previous bar each proved. An open-stamped
bar of any calendar interval does not state when its period *ended* either: that is
`bar_period_end`'s nominal 24 hours, 7, 31 or 366 days, which is what **every shipped adapter
produces for every non-minute bar**. So an adapter-fetched monthly or yearly replay carries this on
most of its fills — though not all: the nominal end is an upper *bound*, so wherever the next
period's start reaches it the check still passes, which every Monday-to-Monday week does.

Where it does not run: the fill is allowed on the older rule that an order never fills
against the bar that produced it, and `ReportCaveats::unchecked_fill_causality_` counts every such
decision. Non-zero means those fill prices are not guaranteed to come from after the decision, and
**any dated order can be one of them** — a quote is the obvious way in, and a bars-only replay
reaches it too.

**You can discharge it yourself**: with no quotes, one interval per instrument, and series whose
periods do not overlap, the fills were causal whatever this figure says. The engine simply could not
prove it from the events it was handed.

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

## A replay's clock visits bar ends only

A replay advances its clock to each bar's period end and derives the market phase there. For daily
bars stamped at local midnight — every shipped adapter's convention — that is the next midnight, which
is outside the session, so with the default `permitted_phases_ = {OPEN}` a daily replay refuses every
opening order (`phase_not_permitted`) and trades nothing. Every replay example sets
`permitted_phases_ = std::nullopt` for this reason: the order is decided at the bar's end and filled
at the next bar's open, which is what a daily strategy means. **Keep that setting in the replay only.**
Live, the default limits already carry a daily target to the next session's open (see "Running a
strategy live"), and `std::nullopt` there sends every decision at midnight.

For the same reason `on_phase` fires on a daily replay only when the phase at one bar end differs from
the last — in practice once — not once a session as it does live. A strategy that acts in `on_phase`
(`vanilla::EqualWeightRebalance`, `vanilla::TimedLiquidation`) needs intraday bars to be replayed
faithfully.

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

**With `enforce_buying_power_` off, its default, a replay borrows without limit.** It is off by
default because live the broker is what decides whether a buy is funded; in a replay nothing does, so
a buy the ledger's cash cannot pay for is filled and the cash goes below zero. A fixed-quantity
strategy whose price rose is then leveraged without having asked to be, and its return, Sharpe and
drawdown are a leveraged strategy's. `ReportCaveats::overdrawn_bars_` counts the bars that closed with
cash below zero, `first_overdrawn_` says when it started and `lowest_cash_` how far it went; read them
before a ranking. They measure cash in the replay's currency only: a short's proceeds count as cash,
so leverage through shorts is not in them (see "Borrow availability" above). Turning the check on
refuses such buys, but a market buy is checked at the reference price, without commission, and filled
at the next bar's open, so a gap up can still overdraw a little. Live, whether a buy the cash cannot
cover is refused depends on the account: a Korean account with 미수 (credit) enabled can buy past its
cash, and the broker's buying-power inquiry is what says how far.

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

**Or cost each execution from a schedule.** `backtest::CommissionPolicy::scheduled(schedule, venue,
dating_market)` resolves a rate **per execution**, against that execution's own market, the date your
replay's session gives it, and the venue you assert — so a replay spanning a rate change charges each rate on its own dates instead of one
rate throughout, and a KRX fill and an NXT fill can differ. The schedule can be the one a broker
published (Toss does; KIS publishes none for domestic stock, and
`backtest::commission_model_from(schedule, country, date)` remains for the older one-date shape) or
one you state from your own fee page.

Three things to know before you rely on it:

- **The venue is asserted, never observed.** A replay cannot know where a fill executed — a bar does
  not say, and the broker fields that would are undocumented. `ReportCaveats::asserted_venue_`
  records that you asserted it, and the figure is exactly as good as the assertion. There is one
  assertion per policy and one policy per `Backtester`, so instruments that executed on different
  venues want **separate replays** rather than separate policies.
- **The market comes from the instrument; the calendar comes from your session.** A replay has one
  `SessionWindow`, so it derives one local date — `dating_market` is you saying whose trading day
  that is. If any rate for **another** market carries a dated bound, a fill in that market is
  refused rather than charged against a day computed hours away: which rate applies depends on the
  day, and this replay cannot compute that market's day. A market whose rates are all open-ended is
  charged, because then no boundary can fall on the wrong side of anything.
- **A caller-stated schedule must name its tariff**, because a published fee table is channel by
  venue and the channel is a property of your account that no API states. A schedule without one is
  refused rather than defaulted to a plausible row.
- **A date no window covers halts the replay**, naming the date. It is not a free trade and not a
  fill that quietly did not happen: the refused trade would have changed everything the strategy did
  afterwards, so the run stops and `Report::halted_` says why.
- **A window that could not be read halts it too, and under a different code.** `no_rate_for_date`
  means the schedule covers no such date; `rate_bounds_unreadable` means the broker published a rate
  whose start or end arrived in a form this library cannot parse, so whether it applies on your date
  is unknown rather than known to be no. The remedies differ: the first wants a wider schedule or
  rates you state yourself, and the second wants the schedule refetched — the rate may well be the
  one in force. `CommissionSchedule::unreadable_bounds_` counts the rows it happened to, so you can
  see it before a replay stops on it.

`ReportCaveats::applied_rates_` then lists the rates that actually **charged** something, each with
its origin and how many executions it charged — not the rows the schedule happens to contain.

**`Report::commission_paid_` is optional, and there are three reasons it can be absent.** Two are
ordinary and one is not:

1. **The replay executed nothing.** No trade, no total — which is not a charge of zero.
2. **The replay spanned currencies.** `commission_by_currency_` is the answer, and a scalar could
   only have been a sum of unlike money.
3. **An exact-decimal step was refused** — a sum that does not fit, or a slippage multiplication
   that does not — which sets `ReportCaveats::cost_totals_incomplete_`. Then the per-currency
   vectors hold only what could be accumulated. It takes figures far beyond any real account to
   reach; it is reported because a money total that is quietly short is worse than one that says
   it is short.

`slippage_paid_` is optional for the same three reasons, and `slippage_by_currency_` is the answer
whenever a scalar cannot be — slippage is money, and a replay holding a won fill and a dollar fill
has no single number for it.

`institution_fees_paid_` is absent for those three reasons and a fourth: any rate that stated a
single figure without splitting the components, counted in `inclusive_commission_executions_`.

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
