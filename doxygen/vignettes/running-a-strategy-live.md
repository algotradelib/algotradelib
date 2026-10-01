# Running a strategy live {#running-a-strategy-live}

This is the operational half of `runner::PollingLoop`: how to start one, how to stop one on purpose,
and which of the ways to stop it will cost you money. The programming half is in the header. Read this
before the first time you leave a strategy running unattended, because two of the things below cannot
be discovered from the API and one of them is a supervisor setting on a machine you may not have
configured yourself.

## What the loop adds over `feed`

`Runner::feed(event)` runs the pipeline once. Everything in this library is built so that call is the
same in a backtest and in production, and it is genuinely all you need for one decision.

What you must not do is write the obvious wrapper:

```cpp
while (true) { runner.feed(next_bar()); }   // don't
```

At that moment you own four things you probably did not mean to: a cadence, a rate-limit interaction, a
reconciliation obligation, and a stop control. The loop exists to own them for you.

The whole production path — journal, recovery, loop, stops and the report — is
[`examples/example-live-program.cpp`](../../examples/example-live-program.cpp) with
[`examples/live_program.hpp`](../../examples/live_program.hpp); copy that. The loop at its centre:

```cpp
runner::FileJournal journal(path, scope);                     // one journal per account
auto live_runner = live::bind_journaled(session, journal, MyStrategy{}, limits);  // Kiwoom: add a route
const auto recovery = runner::recover_runner(live_runner, journal);
live::SessionSource<Session, runner::FileJournal> source(session, {samsung()});
source.set_journal(journal);

runner::PollSchedule schedule;
schedule.bars_ = std::chrono::minutes(1);
schedule.holdings_ = std::chrono::seconds(30);
schedule.working_orders_ = std::chrono::seconds(30);
schedule.price_limits_ = std::chrono::minutes(10);
schedule.tick_ = std::chrono::seconds(1);

runner::PollingLoop loop(live_runner, source, schedule);
loop.set_maximum_portfolio_age(std::chrono::minutes(2));
loop.set_maximum_price_limits_age(std::chrono::minutes(30));

runner::StopSource stop;
runner::SignalStop::install(stop);

const auto report = loop.run(stop);
```

`tick_` is not a poll. It is how often the loop wakes to check whether anything is due and whether a
stop has been requested, so it is also the floor on how quickly an operator gets a response. A loop
trading daily bars with a one-second tick wakes 86,400 times a day and polls once — which is the point.

## The three stops, and what each leaves you holding

| Intent | Stops sending | Cancels resting orders | Closes positions |
| --- | --- | --- | --- |
| `QUIESCE` | yes | no | no |
| `WITHDRAW` | yes | yes | no |
| `FLATTEN` | yes | yes | yes |

`QUIESCE` is the default and is what both signals mean. That is deliberate: `SIGTERM` arrives for
entirely routine reasons — a deploy, a host reboot, `docker stop` — and flattening on a reboot would
turn routine maintenance into a liquidation.

`SIGHUP` is ignored, so a dropped SSH connection cannot stop a process holding positions.

## How to stop one, from outside

**Signals**, which already reach a detached process every way you would want:

```sh
kill -TERM <pid>                              # anywhere
tmux send-keys -t trading:0.0 C-c             # into a tmux pane, from outside it
```

Both quiesce. Note the tmux form: a `Ctrl+C` typed in some *other* terminal does not reach the pane,
which is the property you wanted when you ran it in tmux in the first place.

**A control file**, which is the only channel that can name an intent other than quiesce:

```sh
echo quiesce  > /var/run/atl/stop
echo withdraw > /var/run/atl/stop
echo flatten  > /var/run/atl/stop
```

The loop reads it once per tick, so the response time is one tick. A word it does not recognize is
**reported, not guessed at** — `report.notes_` will say so — because picking the nearest intent would
mean choosing how much of your position to close on your behalf. Check the notes if a stop seems not to
have taken.

The file is never deleted by the loop, so "did my stop get read?" stays answerable afterwards. **It also
means a word left in it is carried out at every start**: a process started with `withdraw` or `flatten`
written reads the broker's order list and holdings, does that to the whole account — every working order
the account lists, every position it holds, not only this strategy's — and exits without letting the
strategy trade.
Remove the file before you start trading again.

## The hierarchy that actually matters

The fastest stop is the worst one. In increasing order of damage:

1. **Quiesce, finish the iteration, exit.** The request in flight completes and is classified, so
   nothing is left unknown. Costs one iteration plus one request timeout.
2. **Quiesce and withdraw.** Adds a cancellation per resting order, each of which can itself fail
   indeterminately.
3. **Quiesce and flatten.** Adds market orders — a trading decision taken under duress.
4. **Killing the process mid-flight.** *Manufactures* an unknown outcome: an order may have reached the
   broker with the reply lost, and nothing recorded that it might exist.
5. **`SIGKILL`.** Everything in 4, and the indeterminacy ledger dies with the process.

Levels 4 and 5 are the emergency doors. They are not faster in any way that helps you, because what
they save in seconds they cost in having to read the broker's order list by hand before trading can
resume.

## The trap: your supervisor's grace period

**This is the one thing here you cannot find out from the code, and it silently converts level 1 into
level 5.**

`systemd` sends `SIGTERM`, waits `TimeoutStopSec`, then sends `SIGKILL`. `docker stop` does the same
with a ten-second default. If that grace period is shorter than *one loop iteration plus one request
timeout*, then every clean shutdown you ever perform ends with a process killed mid-request — and you
will believe you shut down cleanly, because you asked nicely and it exited.

So make the grace period generous:

```ini
[Service]
TimeoutStopSec=120
```

```sh
docker stop --time 120 trading
```

And size it against your actual HTTP timeout, not against how long a shutdown usually takes.

## The three gates that stop you trading, on purpose

**The order list must be read before anything is sent.** A process that has just started cannot know
whether a *previous* process died mid-POST. An empty indeterminacy ledger does not mean nothing is
owed — it means nothing is remembered. So the loop reads the broker's own order list once before it
will send anything, and retries until it succeeds. While that is pending, `report.final_gate_` is
`order_list_unread`.

This is the cheapest half of reconciliation and it needs no persistence at all. The durable half is
the journal: `bind_journaled`, `recover_runner` and `set_journal`, in
[Recovering after a crash](recovering-after-a-crash.md).

**A stale or absent holdings snapshot blocks sizing, and stating a policy is not optional.** A
placement-enabled loop calls either `set_maximum_portfolio_age` or `allow_unbounded_portfolio_age`;
one that states neither holds and the report names the missing call. The library picks no default
duration, because how stale a position may be before it must not be sized against is a risk policy
only the operator can state. A snapshot that was **never read** is a separate hold,
`portfolio_absent`, which no age limit can express — an empty portfolio is the *permissive* answer to
every ceiling and to every target subtraction, so a run that has never read the account would
otherwise size an entry as though it were flat. `allow_absent_portfolio` is the deliberate way out
for a run that genuinely has no holdings to read.

**Within an iteration, the loop polls the order list, then holdings, then bars and quotes** — whichever
of them are due. A resting buy that fills
between the two account reads is the reason. Read holdings first and the listing second, and the fill
lands in neither: holdings still flat, the order already gone, and a target strategy buys again on the
next bar. Read the listing first and the same fill lands in both — the target reads as met, and at
worst a cancellation of an order that already filled is refused. Bars and quotes come after both,
because they can send, so on a cold start the first bars meet an account already read.

**A plan sized against holdings older than the listing is withheld** (`portfolio_superseded`), which
no opt-out reaches: it happens when a holdings read failed after a listing, when working orders are
polled more often than holdings (the loop notes it once per run), and for a target an
`on_order_update` handler states during the listing poll. A withheld `TargetPosition` — withheld for
this or because the account was never read or is too old — is retained and planned again at the end of
an iteration — after its polls and `on_phase` — once the account can be trusted and nothing holds; a
later target or order for the same instrument replaces it, and a stop discards it. An `OrderIntent` is dropped, because a fixed quantity
decided against the wrong picture should not be replayed later.

**These holds stop a placement, not everything.** A cancellation naming a row you chose still goes
out under them, because an order that cannot be withdrawn keeps resting and can fill. What does *not*
go out is a plan the library derived from the snapshot it cannot stand behind: a target of zero
against an account that merely *looks* flat would withdraw the orders protecting a real holding, so
such a plan is withheld whole and reported in `withheld_plans_` rather than half-sent.

**A checked sell also needs to know what the broker's figure means.** With
`enforce_sellable_quantity_` on, the guard measures a sale against the quantity the broker said may be
sold — and it refuses, with `sellable_basis_unestablished`, when nobody has established what that
quantity *covers*. That is a different refusal from `quantity_not_sellable`, deliberately: one means
resize the order, this one means declare a basis.

KIS and Toss declare one, so you will not meet this there. **Kiwoom does not by default** — its
coverage is something *you* assert, because Kiwoom's own specification does not say whether
`dmst_stex_tp` filters the positions or only values them, so a Kiwoom run with this limit on refuses
every sell until you set `KiwoomConfig::holdings_sellable_scope_`. You will also meet it if you write
your own source or your own adapter: fill `HoldingsSnapshot::sellable_basis_` (or, for a figure that
arrived per instrument, that row's `HoldingPosition::sellable_basis_`) with the coverage and the
resting treatment your broker actually supports, and the guard enforces. Leave it unstated and the
refusal names the declaration to add rather than being measured against a number of unknown meaning.

With a policy stated, the loop refuses to place while the snapshot is older than it, reporting
`portfolio_stale`. Reconciling a target against
stale positions is how a position gets double-sized by a route that `working_quantity` does not close.
Staleness is measured against `Portfolio::as_of_`, and **that is when the reply arrived here, not
when the positions were true at the broker**. Neither broker timestamps a holdings answer, so the
adapters stamp `HoldingsSnapshot::received_at_` with the local clock as they parse, and
`SessionSource` copies it straight into `as_of_`. The gate therefore understates the age of the
figures by however long the request took: a snapshot the broker computed slowly can pass an age
limit it should have failed. Set the limit with that headroom in mind.

**A run that sends to a broker needs the order listing too.** A holdings poll refreshes positions and
keeps the working orders it had; only a working-order poll replaces them. And an order the runner
sends stays in `working_` until that listing replaces it (below), so without one an order that expired
or was refused would stay "working" for ever, a target would stop being chased, and a filled one would
be cancelled again on every pass. So a runner that sends to a broker
(`Runner::keeps_sent_orders_until_listed`) holds with `portfolio_stale` while the source publishes no
listing, while `PollSchedule::working_orders_` is not set, and while the last listing is older than
`set_maximum_portfolio_age` — whatever the age policy, because opting out of a holdings age is not
opting out of a listing. The report names the missing piece once. Poll the listing more often than
the age by at least a tick: a cadence equal to the age reaches the limit a tick before each poll.
A dry run and an observer are not held.

**A price band is a per-trading-day value, and one nobody has aged is not applied.** `PriceLimits`
is "the highest price allowed today" and for KRX it moves with the previous close, so a band
registered once at startup describes the day the process started on. Until slice 29 nothing refreshed
one: `Runner::set_price_limits` was an operator's startup step, no `PollKind` read one, and the band
was clamped into every limit price for as long as the process lived. Now:

- `PollSchedule::price_limits_` refreshes it, one request per instrument in the source's universe, at
  market-data priority. Absent by default like every cadence but the calendar's.
- A run states `set_maximum_price_limits_age` or `allow_unbounded_price_limits_age`. A band with
  neither stated is **not applied**, and the report's notes name the two calls, once.
- A band the broker dated is aged from **the exchange's own stamp** — Toss publishes `timestamp`,
  Kiwoom `date` and `tm` — so a successful poll returning yesterday's band does not read as fresh.
  KIS publishes no stamp, so for a KIS band the policy bounds only how long ago it was *fetched*.
  The stamp may only make a band look **older**: one dated after the moment this process received it
  is capped at the receipt instant, because two clocks disagreeing is not evidence of freshness.

**Withholding a band does not refuse an order and does not hold anything.** It returns the order to
the state every run that registers no band is already in: the strategy's own price on the wire. That
is not free, and it is worth knowing what it costs before choosing the policy:

- **A fill can be lost.** Where the stated price is outside *today's real* band, the exchange refuses
  the order, and the clamped price would have been accepted and could have traded. A reducing sell is
  the case that stings — an exit priced below today's floor is refused instead of being lifted onto
  it. `LoopReport::unclamped_orders_rejected_` moves when an order it sent was refused.
- **Or nothing sends it at all.** A buy left above a withheld ceiling is the larger notional, so the
  per-order ceiling, the buying-power check or the gross exposure ceiling can refuse it; and a price
  no adapter will send — a sell at zero passes the grid check — is refused by the binding before any
  request goes out. `LoopReport::unclamped_orders_refused_` moves for both, and no exchange ever sees
  them.

Read both as **co-occurrence, not causation**: orders are refused for many reasons, and a count of
one is one order that happened to be carrying an unclamped price. They are a reason to look at the
policy, not proof about any single order — and **neither can see the case with no refusal in it**, an
order accepted at the raw price that then never filled. `LoopReport::price_bands_withheld_` counts
the decisions rather than their cost. The reason
withholding is still the default: a band whose age nobody has bounded is not evidence about today's
band, and this library does not alter an order's price on the strength of something that is not
evidence. The alternative is not "clamp correctly", it is "clamp to a number from an unknown day",
and that failure is an order resting at a price nobody chose which may never fill. One line of policy
overrides it either way.

## What the report says about orders that did not go out

Until slice 31 an ordinary broker refusal had **no field of its own**. `Runner::feed` returns a
report describing what became of every instruction — fourteen vectors of it — and
`live::SessionSource` discards that report on every poll, so a run in which the broker refused every
order looked very much like one in which it accepted every order: same iterations, same poll counts,
no failures. (Not quite identical — the phase and price-band figures can differ for some refused
orders — but nothing said *the broker refused these*.) The refusal was in the report `Runner::feed`
returns, which the source drops on the floor — and, **if you bound a journaling executor whose
terminal write landed**, in the order journal. `live::bind` does not journal at all: journalling is
`live::bind_journaled`, so a run built the way this page opens has no record of a refusal beyond the
report. And `bind_journaled` is not itself a promise of durability — it takes whatever journal you hand
it, including a `MemoryJournal` that is gone when the process is, and its *terminal* append can fail
after the broker has already answered, leaving the `PENDING` line and no outcome.

`LoopReport` now carries twelve figures and one record, all describing **the polling loop** — the
stop handler and the closing pass keep their own families, `stop_handler_*` and `closing_*`, so a
refused flatten is reported once rather than twice.

| Figure | What happened |
| --- | --- |
| `orders_unbuildable_` | reconciliation could not build the instruction: no reference data, no price, an unknown grid — **or a quantity that rounded to zero**, which is a deliberate drop rather than a fault, so a non-zero figure is not by itself a reason to go looking for missing data |
| `orders_reconcile_withheld_` | the indeterminacy ledger is holding that instrument until a listing settles an unknown outcome |
| `orders_guard_refused_` | `RiskGuard` refused it — **a hold as well as a limit**, since it checks the active holds first, so a halted run reports every order here. Nothing was sent |
| `orders_binding_refused_` | the binding would not send it; no request went out |
| `orders_broker_refused_` | the broker was asked and said no; a request was spent |
| `orders_indeterminate_` | the request failed in transit, so the order may or may not exist |
| `cancels_rejected_` | a withdrawal refused outright — **by the broker or by the binding**; `RejectedCancel::by_broker_` distinguishes them and this figure does not |
| `cancels_refused_` | a hold refused the withdrawal before anything was sent |
| `cancels_unresolved_` | the reference named an order the snapshot does not hold |
| `cancels_indeterminate_` | the withdrawal failed in transit; **it may still have withdrawn the order** |
| `plans_withheld_` | a whole plan held back before any of its parts was judged — **for one of four reasons this figure does not distinguish**: the account was never read, it is older than your stated maximum, it was read before the working-order listing it would be reconciled with (`portfolio_superseded`), or a hold is refusing placements for a plan that also wanted to withdraw something. The last two happen on a perfectly fresh portfolio, so do not read this as "the account is unknown". And **`final_gate_` does not always show it**: with `allow_absent_portfolio` the gate is open while a plan that would withdraw something is still withheld |
| `instructions_unavailable_` | local machinery was unavailable, so it was not sent — a pre-send journal write that failed is the usual cause and **not the only one**, since any executor answering `UNAVAILABLE` lands here. Placements and withdrawals together. It leaves a sticky `local_failure` hold, which `final_gate_` *may* name — a higher-priority hold can be reported while that one is still in force — so read this as how many instructions it cost |

**And `last_broker_refusal_`, which is the one worth reading first.** A count tells you orders are
being refused; this tells you which instrument, under which of your own tags, with the broker's own
code and message. One record, overwritten — a run refused ten thousand times keeps the last
one **that came with an error**, and a refusal that says nothing clears it rather than leaving the
previous order's words behind. Empty when *this* run's polling saw no refusal, even though the runner
may still hold one from an earlier run; and empty, with a non-zero count beside it, when the refusals
themselves were silent. It is empty on a dry runner and in a replay, which have no broker to be refused by.

**`last_unbuildable_` does the same for `orders_unbuildable_`**: the reconciliation's reason
(`RejectReason`), the instrument and your tag, for the last instruction this run's polling could not
build, and empty when it built them all. `live_program` prints it as `last unbuildable`. The commonest
live answer is `no_portfolio_value`: a weight-sized instruction (`SizeSpec::target_weight`, which
`EqualWeightRebalance` states) needs a portfolio value, which a replay supplies and a live runner does
not, so it is refused on every pass.

**An unknown buying power blocks buying.** Only if you asked for the check: set
`RiskLimits::enforce_buying_power_` and the loop refuses to send while it has no answer from the
broker about what may be spent, reporting `buying_power_unread`. Set a cadence with
`PollSchedule::buying_power_` and the currencies with `SessionSource::set_buying_power_currencies`,
and optionally an age with `set_maximum_buying_power_age`. A loop that never asked for the check is
never held by it.

**Every currency you configure has to answer.** The gate holds until each one the source was told to
poll has a reading the age rule accepts — not merely until one of them does. An account trading two
currencies where only one answers is a budget that never arrived, and one currency's figure is not
permission to spend another's.

The reason it is a hold rather than a refusal per order: sizing a buy against a budget nobody has
stated is the thing the check exists to stop, and a runner that has never been told cannot make a
per-order decision about it. Once a reading exists, an order too large for it is an ordinary
refusal — `insufficient_buying_power`, carrying the budget and the attempt.

All three gates work by holding the guard, which is the library's one primitive for "stop sending
without withdrawing or flattening". **A hold names who took it**, and is released only by them: the loop takes
`portfolio_stale`, `order_list_unread` and `buying_power_unread`, you take `operator` when you call
`halt()` by hand, and the runner takes `local_failure` when local machinery cannot send — usually a journal write that failed, and not only that. Trading resumes when the last hold is
released, so the loop opening its gate cannot lift your halt, and your `resume()` cannot lift the
loop's.

**Your strategy's `on_stop` cannot trade during a stop.** It is dispatched first, while the loop's
hold stands, so anything it submits is refused and counted in `stop_handler_refused_`. A stop belongs
to the operator; an order the strategy adds in that window would be planned against the same snapshot
as the loop's own closing orders and could close the same position twice.

**A `flatten` states its instructions directly** rather than expressing them as a target position.
It withdraws every working order once — including one on the same side as the close, which ordinary
reconciliation would keep as progress toward the target — and then sends a market order for the
position.

**It closes only the part no working order was already trading**, and that subtlety is worth the
paragraph. Ten held against a resting sell for ten: closing all ten alongside it could sell the
position twice, because a cancellation is a request and the resting sell can fill while its
withdrawal is still pending. Ten long would become ten short. So the close is sized to the holding
minus whatever is still working on the closing side — nothing at all, in that example — and
`closing_contended_` counts the positions where that happened. Whichever way it resolves is safe:
if the withdrawal completes, part of the position is still held and the report says so; if it fails
because the order filled, the account is flat. Neither is complete, so `closing_complete()` is
false, and a later holdings snapshot is what tells you which one you got.

**And when the picture itself is unverified, no close is sent at all.** That deduction can only
subtract orders the loop can see, so an order sent after the last listing — or a source that cannot
enumerate orders, like the history fallback — would slip past it and the full-size close would be
the same double sell by a different route. The withdrawals still go, because cancelling only reduces
exposure; the position stays, held and reported. A stop pending when the process starts (the control
file already says `flatten`) makes the startup read itself before it closes anything, so this case is
one of those listed under `closing_source_unverified_` below: a failed order-list or holdings read, a
source with no working-order inquiry, or an order sent after the last listing.

**A flatten beside your own protective stop therefore takes two runs.** The first withdraws the stop and
leaves the position, contended, and the program exits `CLOSING_INCOMPLETE`. Start the program again with
`flatten` still in the control file: it reads the order list before anything else, sends nothing for the
strategy (the stop's hold is in force from the first iteration, so the strategy cannot place its stop
again), and closes what is held once nothing is resting on the closing side. Between the two runs the
position has no stop. If the cancellation is still pending at the broker, that run is contended too,
sends nothing (its second cancellation may be refused, which the report counts) — run it again; the same
if the first run never saw the stop. Then remove the file.

**An operator's halt refuses cancellations too, including a stop's own.** Note that this is `halt()`
specifically, and not the uninformed holds described above — those let a withdrawal you named go out,
which is the distinction that paragraph turns on. `halt()` means "this runner sends
nothing", and a cancellation is a send — an order-writing request that can itself fail
indeterminately. So a `withdraw` or `flatten` arriving while you are halted cancels nothing, reports
`closing_refused_`, and says "the position is still held" in the notes. That is deliberate: a stop
reaches the process from a signal and from a control file, so a cancellation exempt from the halt
would let `echo withdraw > /var/run/atl/stop` send an instruction your own halt forbade. To get out
after halting, `resume()` first — one deliberate act, and `closing_complete()` then tells you whether
every instruction was accepted.

`report.final_gate_` is an optional, and names the most fundamental hold in force rather than only the
loop's own — so a run that sent nothing because *you* halted it says `operator`, instead of showing you
an open gate while nothing goes out. `guard().holds()` lists every hold when more than one stands, and
`guard().hold_detail(HaltReason::LOCAL_FAILURE)` carries a local-failure detail — whatever the binding
said it could not do, or whatever a caller of `halt_locally` supplied, such as a source that could not
journal a reconciliation decision. A failed journal write is the usual answer and not the only one.
Recorded through the runner it is the **first** such detail, kept while the hold stands; a program that
reaches past it and calls `guard().halt(LOCAL_FAILURE, …)` itself replaces the detail, so if you take
that hold directly the value is yours, not the first cause.

## Did everything I asked for go out?

Ask, because it is not always yes — and note that this is a narrower question than "am I flat", for
the reasons below:

```cpp
if (!report.closing_complete()) {
  for (const auto& note : report.notes_) { std::cerr << note << '\n'; }
}
```

`withdrawn_` and `flattened_` count what the loop **asked for**. `closing_placed_` counts what actually
**went out**. They differ, and the most common reason is the one below.

`closing_complete()` is false if *any* closing instruction failed to go, and there are six ways that
happens — `closing_rejected_` (reconciliation could not build it, the broker refused it, or the
binding refused to send it),
`closing_refused_`, `closing_withheld_`, `closing_indeterminate_`, `closing_unavailable_` (the
local machinery was unavailable, so no broker was
asked — usually a journal write that failed, and not only that) and `closing_unresolved_` (a cancellation named
an order the snapshot does not hold). Cancellations count in these as well as placements: a
withdrawal that failed in transit may have withdrawn the order or may not, and that is not the same
as one that went.

A seventh, `closing_contended_`, counts something different: a close the pass deliberately made
smaller, or did not send at all, because another order might close the same position. See the
flatten note above.

**And one flag covers the case before any of those**, `closing_source_unverified_`. A stop builds its
instructions from the portfolio snapshot — working orders for a `withdraw`, positions for a `flatten`
— so it can only be complete if that picture is current. Five ways it is not:

- **The startup read failed.** A `withdraw` or `flatten` requested before the loop's first iteration
  attempts the startup read itself; if that read fails, the picture cannot contain an order a *previous
  process* left resting. (A `quiesce` does not read: it sends nothing.)
- **An order was sent after the working orders were last read.** The holdings and working-order
  inquiries are separate polls on separate cadences, and `as_of_` is the *holdings* timestamp — so a
  snapshot stamped a second ago can carry working orders read an hour ago, missing an order this
  runner placed in between.
- **A listing is older than `set_maximum_portfolio_age`.** With no age policy stated, no age makes the
  picture unverified.
- **The holdings are older than the listing** — a holdings read failed after it. A fill between the two
  would be in neither, so a `flatten` could miss both the order and the shares it bought.
- **The source does not publish working orders at all.** A broker without the working-order inquiry
  falls back to reading order *history*, which satisfies the startup gate but never fills
  `working_`, so a withdrawal cannot be built from it. A source declares
  `static constexpr bool provides_working_orders = true` to say it does; anything that does not
  declare it is treated as one that does not, because the cost of guessing wrong is a stop that
  reports complete with an order still resting.

The pass still sends what it can see, because withdrawing some beats withdrawing none, but it will
not call the result complete, and the note says which of these it was. If you can, let the loop
complete a working-order poll before stopping it.

**It is not a confirmation of anything at the exchange.** `true` means every instruction the loop
asked for was *accepted by the broker*, and that is where the library's knowledge stops. An accepted
closing order may not have filled, which only a later holdings snapshot shows. An accepted
cancellation may not have completed: cancellation is asynchronous at both brokers — Toss publishes
`PENDING_CANCEL` while it is in flight and `CANCEL_REJECTED` when it fails, returning the order to its
previous state — so a withdraw that reports complete can still have orders working.

So after any stop, read the broker's own order list and holdings before believing anything about the
state. `closing_complete()` answers one question only: did anything fail to go out.

**`RiskLimits::permitted_phases_` defaults to `{MarketPhase::OPEN}`, and a flatten outside market
hours still goes out.** The phase limit governs what an order *opens*, not what it closes: an order
reducing a held position toward flat is permitted in any phase, because a limit that cannot be
de-risked past turns "the market is shut" into "you may not reduce risk". So an operator who asks to
be flat at 22:00 gets a request sent, and very likely a *broker's* rejection rather than the
library's — which is the better answer, since it is the venue's own, and recorded beyond the report
**if you bound a journaling executor and its terminal write landed**: `live::bind` does not journal,
`live::bind_journaled` does, and a failed terminal append leaves the `PENDING` line with no answer
beside it. Whether a recorded answer then outlives the process is a property of the journal you handed
in — a `MemoryJournal` is gone when the process is.
`LoopReport::sends_outside_permitted_phase_` counts how often that happened. When that request came
from a strategy's `TargetPosition` — `vanilla::TimedLiquidation`'s `POST_CLOSE` flatten is one — and the
broker rejected it, the target is kept and the exit is sent again at the next session's open (below).
The operator's own `flatten` is an order, not a target, and ends with the rejection.

**Every other limit still applies to that flatten, and each of them is silent in the same way.** A
sellable check with no broker reading, a per-order ceiling, a position ceiling, a halt: any of these
refuses the exit, the report says `closing_refused_`, and a note ends "the position is still held".
An operator who asks to be flat and hears nothing would believe they are. This is why the counts are
separate rather than one number, and why `closing_complete()` exists.

**An opening order after the close is refused exactly as before** — `PHASE_NOT_PERMITTED` — **but a
target, or an order, is kept for the next session.** This is the case every daily strategy meets: each shipped
adapter stamps a daily bar at 00:00 KST, `SessionSource` feeds it once its day has ended (at the next
midnight), and the strategy decides while the market is shut. A `TargetPosition` whose opening order
the phase limit refused, or whose exit it admitted and the broker (or the binding) then refused (a
broker not taking orders at midnight does), is kept (`Runner::targets_retained_for_phase`) and planned again in the
first iteration whose phase the limit admits — against that iteration's holdings, listing and prices.
That is what a replay does by filling at the next bar's open. The rules around it:

- One per instrument, the latest statement kept; a later target or order for the instrument replaces
  it, as does an `on_phase` target at the open.
- A stop discards it, and so does an operator's halt or a local failure: a decision made hours before
  a halt must not go out when the halt is lifted.
- **An order the broker could not be reached for is kept the same way**, whatever the phase it failed
  in (it is still released only in a phase the limits admit): a DNS, connect or
  TLS failure, a closed client, or a failed token request -- anything that never left the process,
  except the client's own validation, which would refuse it again. Dropped, the decision waited for its
  next bar: a day, for a daily strategy, and a protective stop missing for the whole session. It is
  offered again only after a holdings read newer than the failure, which is the evidence the broker
  answers again, so a dead connection is not retried every tick. Each failed attempt is reported as a
  `BINDING_REFUSED` rejection while the order waits, and it is counted in the `_for_phase` figures
  above. **So is an order the broker turned away for its rate limit** (KIS's `EGW00201` at a busy
  open): the broker answered and processed nothing, so it is reported as `BROKER_REFUSED` and kept the
  same way. Like a phase refusal neither has an age limit: one kept near the close and not stated again goes out
  at the next open, if its book is unchanged. An order whose request *was* written and whose reply was
  lost is not this: it may exist, and recovery resolves it from the broker's lists.
- **An `OrderIntent` is kept the same way** (`Runner::intents_kept_for_phase`) — the commonest is a
  daily strategy's protective stop, which a broker refuses at midnight; unkept, it was restated and
  refused every midnight and the position was never protected live. But an intent states a quantity
  decided against midnight's book, so at release it goes out only if the instrument's holding is still
  what it was when it was kept, and a kept intent carrying a trigger -- a stop sell or a breakout stop
  buy alike -- only if no stop now rests there; otherwise it is dropped and counted
  (`Runner::kept_intents_dropped`). The check reads the book before the kept targets it follows are
  placed, so a kept exit and a kept stop on one instrument go out together, as the pass stated them;
  `enforce_sellable_quantity_` or the broker's own sellable check is what refuses the second sell. Each instruction is kept once, intents in the order
  stated, after the kept targets. An intent withheld for the account is still dropped; so is a keyed
  intent a broker answered (a second order would reuse the key). A weight-sized intent skips the
  holding check, because it is planned again as the position it states. Kept intents are not
  journalled: a restart before the open loses those the newest bar does not state again (a stop the
  strategy restates on every bar comes back from the history the new process reads). **A restart during
  the session sends any quantity- or notional-sized `OrderIntent` the newest bar states again**: the new
  process feeds that bar live, in `OPEN`, and such an intent is planned without reading the book, so
  nothing sees the copy an earlier process sent this session -- the morning's release, or a process
  started after the open. Each restart sends it once more; a supervisor restarting a crashing process
  every minute sends it every minute (`max_orders_per_window_` counts in memory). A second stop sell is
  likely refused by the broker's sellable check, or by `enforce_sellable_quantity_`, which deducts the
  resting stop. A limit buy is sent again whenever the strategy states it again (always, while the first
  copy rests unfilled); of this library's limits only `max_position_quantity_` and
  `max_gross_exposure_`, which count the resting buy, refuse it. A `TargetPosition` counts what is
  working and is not affected (nor is a weight-sized intent, planned as one): state one for anything
  that must survive a restart. A `CancelIntent` does not forget them; released together at the open they
  can meet `max_orders_per_window_`; and a target and an intent kept by one pass while a stop rests can
  supersede each other on release (the later statement wins, as always). The run's report notes how many
  were dropped.
- It lives for one `PollingLoop::run`. A restarted process re-derives it from the history the source
  feeds on its first poll; a second `run` on the same source does not, because that source has already
  fed those bars, so the decision waits for the next bar.
- It is placed in the first `OPEN` iteration, after 09:00, so it misses the opening auction the replay's
  fill at the open stands for. **Do not add `MarketPhase::PRE_OPEN` to `permitted_phases_` to reach
  the auction**: `PRE_OPEN` begins at 00:00 on a trading day, so the midnight decision would go out at
  midnight, a broker not taking orders would reject it, and — the phase having permitted it — the
  target would be dropped.
- **It helps only while `permitted_phases_` stays inside the hours the broker takes orders.** A target
  whose order a broker rejects in a permitted phase is not kept: that is a refusal of the order, not of
  the hour.
- It is planned against that iteration's holdings and listing, and against the **last price the runner
  has**: with bars alone that is yesterday's close, because the session's forming bar is not fed, so
  sizing and the buying-power check do not see the opening gap. Poll quotes (`PollSchedule::quotes_`)
  for a price from the session.
- **Do not copy a replay's `permitted_phases_ = std::nullopt` into a live program.** Every replay example
  sets it, because a replay's clock visits only bar period ends; live, it sends every daily decision at
  midnight, where a broker rejects it.

### The history a cold start reads

**An instruction reaches a broker only when it was decided on the newest closed bar the source held
for that instrument at that poll.** A cold start's first read is a history — `live_program::Settings::
bar_count_` bars per instrument — and `SessionSource` shows all of it to the strategy, in period-end
order across the universe as a replay would, so indicators and a strategy's own state are warm. But
everything decided on the older bars is discarded (`Runner::feed_warmup`), counted
(`Runner::warmup_instructions_discarded`, `SessionSource::bars_fed_as_warmup`) and noted once in the
run's report; only the newest bar's decision can be sent, or kept for the phase. The same holds for a
backlog after an outage: two missed daily bars are both shown, and only the newer one acts.

Why it matters: a restarted process used to act on every signal in its history, at today's price. An
event-triggered strategy — `vanilla::RsiThreshold` enters on "not holding and oversold" — met an
oversold bar months back against a flat book and bought again a minute after the previous process had
sold. A level strategy (`SmaCrossover`) was right by accident, because only its last target survived.
So a process started overnight and one started during the session now take the same decision: the
newest bar's. Four consequences to design for:

- **State levels, not transitions**, including for cancellations: a transition detected on history is
  not acted on, and the newest bar's pass must be able to say what to hold without having seen it
  happen.
- **Poll bars no slower than their interval.** A poll that brings five minute bars acts on the newest
  only; for a level strategy that is the position a continuous process would hold, for an event
  strategy a signal lost (and counted) rather than one acted on late.
- **An instrument whose read ends early waits.** Only a newest bar whose period ended strictly after
  every history bar of the read can send, so a name suspended today or for days — its newest bar no
  later than the others' history — is shown to the strategy as history, and its own decision waits
  for its next bar.
- **A bar query that fails feeds nothing that poll**, for any instrument: the next poll reads them all
  again. One name that always fails therefore stops bars for the whole universe, and the loop reports
  the failing poll.

A source of your own must do the same: feed a read's new bars in period-end order across
instruments, and feed live only each instrument's newest bar whose period ended strictly after every
bar of the read fed as history; everything else goes through `feed_warmup`.

## The market phase, and what the loop needs to know it

The loop derives the phase each iteration from two things:

- **a session window you state**, `set_session_window(...)`, defaulting to `SessionWindow::krx()`. Only
  Toss publishes intraday hours and they are not normalized, so this is configuration rather than
  something the library discovers. For a market that is not KRX, state it.
- **a trading calendar it polls**, `PollKind::CALENDAR`. This is the one cadence in `PollSchedule` that
  is **on by default**, six hours, because a loop without a calendar has an uncovered date, an
  uncovered date is `MarketPhase::UNKNOWN`, and `UNKNOWN` is not in the default
  `permitted_phases_` — so every order opening a position would be refused. KIS's client caches and
  answers a covered date without a request, so four polls a day cost one request a day.

`on_phase` fires on a change, once, with this iteration's prices and portfolio already installed. A
strategy written on it — `vanilla::TimedLiquidation` is the shipped one — behaves the same live as in a
replay **of intraday bars**. A daily replay's clock visits only bar ends, so there it fires about once
in the whole run (see "Reading a backtest honestly").

**If you write your own source**, one rule is yours to keep, in two halves: write the trading calendar
during `PollKind::CALENDAR`, nowhere else, and **do not feed the runner from that poll**. The loop
re-derives the phase before every call it makes that could send, but nothing of its runs between your
`set_calendar` and your `Runner::feed`, so a poll that does both settles an order against the phase as
it stood when that poll began. `SessionSource::poll_calendar` installs a calendar and feeds nothing,
which is the shape with no edge.

The same rule applies to a **strategy**: `on_phase` and `on_stop` receive a mutable context, and
`Context::set_calendar` is not theirs to call — read `calendar()` instead, which is what it is published
for. A calendar revised by `on_stop` is the one case the loop can still answer, because the closing pass
is a separate call and the phase is re-derived before it.

Where it cannot, it witnesses: `LoopReport::sends_under_a_revised_phase_` counts the calls where the
phase moved *and* a request went out, with a note naming the call. It is a contract diagnostic rather
than proof that a particular order was mis-checked, and it says so.

When the phase cannot be established, the run says so: `final_phase_` is `unknown` and a note names
the reason — no cadence configured, no calendar published by the broker, every poll failed, or the
date is uncovered. It is deliberately **not** a halt: a halt would also stop cancellations, which
`Runner::send_cancel` exempts from the phase limit because withdrawing only ever reduces exposure.
`RiskGuard` already refuses every *placement that opens a position* with `PHASE_NOT_PERMITTED`, and
permits one that closes a held position — which is the case a halt would wrongly stop, for the same
reason it would wrongly stop a cancellation. So there is nothing a halt would add and something it
would take away.

## Poll failures are not send failures

A poll that fails sent nothing, so nothing needs reconciling and no ledger entry is owed. A quote that
did not arrive is an absence of information, and the loop carries on without it.

What it does bound is repetition: `FailurePolicy::consecutive_limit_` consecutive failures of one kind
and the loop quiesces rather than spinning against a broker that is refusing it, reporting
`final_gate_ == poll_failing` with `stopped_by_` empty — so you can tell the loop's own decision from an
operator's stop. Counted per kind, so a
broken working-order inquiry does not mask a healthy bar feed, and **consecutive** rather than
cumulative, so one failure in three is degraded rather than down.

A rate-limit refusal is specifically **not** a failure. It is the limiter working, and the controller
already holds capacity back from polling so that an order is never the request that waits.

## Asking the broker what the account can do

Two checks, both off by default, because turning either on changes what an existing program does and
because each is the broker's answer rather than a limit you chose.

```cpp
strategy::RiskLimits limits;
limits.enforce_buying_power_ = true;        // refuse a buy the broker cannot fund
limits.enforce_sellable_quantity_ = true;   // refuse a sell of shares it will not release
```

**What they cost differs by broker, and `BrokerCaps::sellable_source` says which you have.** At KIS
the sellable figure arrives on every holdings row, so the check costs nothing at all. At Toss it
takes a request per instrument you actually sell, spent at the moment the sell is planned and kept
with the position until the next holdings poll replaces it.

**Absent is not zero.** A figure the broker has not stated is unknown, and unknown refuses. Neither
check falls back to what you hold or to `cash_in()` — a position is not evidence of a *sellable*
position, and settlement, pledging and locking are exactly the gap being reported.

**A pass sizes against what it has already sent.** Two sells of sixty against a hundred sellable do
not both go out, and neither do two buys the budget can only fund once. The reservation outlives the
pass and is discarded when a working-order poll replaces the listing, so a second bar before the next
poll cannot re-spend the same money.

**Every order sent to a broker counts as working until the next working-order listing replaces it,
whatever the limits** — on a broker that lists working orders, which all three shipped ones do. Any
pass before the next listing therefore sees the order an earlier one sent and does not send it again:
two bars of a minute strategy between listings, a runner driven by hand, a quote and a bar in one
iteration. Before 2026-09-30 this held only with one of the checks above enabled, and a target strategy
under default limits sent one order per bar of a cold start's history (which `SessionSource` now feeds
as history, above, so only its newest bar can send at all). A dry run and `live::bind_observer` send
to no broker, so their rows carry no broker reference — a later opposing target would ask to withdraw
an order identified by nothing — and they keep the old scoping: they report a restated target again on
every pass.

**Passing is not a promise.** These are readings, and a fill against a resting order moves them
without telling anyone. The check is the difference between sizing against the broker's last answer
and sizing against nothing.

## What the broker says it charged you

`LoopReport::charged_costs_` carries the commission and tax the broker stated on the orders this run
saw, per currency. Toss states both on every order; at KIS the order snapshots state neither, and
that absence is reported as an absence rather than as a charge of zero.

It exists because a replay reports a modelled commission and a live run reported **nothing at all**,
so there was no direction in which the two could be compared.

**Read it as a subtotal of unknown coverage, not as a total**, and the field's own documentation says
so first. The loop polls *working* orders: an order that fills and leaves that listing between two
polls is never seen carrying its final commission, and one placed and filled between two polls is
never seen at all. Neither is detectable from a listing, so:

- `working_list_disappearances_` counts rows that left the listing — on a fill, and equally on a
  cancellation. It is a diagnostic, and it **can read zero over a charge that was missed**.
- `conflicting_restatements_` counts orders that restated a different amount at the same filled
  quantity. Nothing on a snapshot says which reading is later, so the first is kept and the
  disagreement is reported rather than settled by arrival order.
- The figures are differenced per `run`, so a second run on the same source reports its own
  observations rather than the first run's as well.

A total with known coverage needs a deliberate order-history poll, which this loop does not make.

## The portfolio exposure ceiling, and what it means by "portfolio"

`RiskLimits::max_gross_exposure_` caps the exposure the account may reach. Until `ai/guard-inputs` it
did not do that: nothing in the library ever supplied the current exposure, so the ceiling was
compared against the value of the single order in front of it, and a ten-million ceiling did not stop
a strategy reaching twenty million two million at a time.

```cpp
strategy::RiskLimits limits;
limits.max_gross_exposure_ = Decimal64::parse("10000000");
```

Three things to know before turning it on.

**It is a ceiling per currency.** The same number is applied independently to each currency, in the
currency the order is priced in — `max_order_notional_`'s reading, and `holdings-core`'s refusal to
add currencies without a rate you can see. So an account holding ten million of exposure in won and
ten million in dollars passes a fifteen-million ceiling. If you need a converted account-wide cap,
this is not it; `plan/todo/exchange-rates.md` is where that becomes expressible.

**It measures what would happen if everything outstanding filled**, not what the last poll reported —
the same standard `max_position_quantity_` uses. Working orders count, so two orders in one pass
cannot each fit and together breach it, and the reservation carries across a bar so a second bar
before the next poll cannot either. An instrument is valued at the most expensive price that could
apply to it, so a limit buy above the current quote is not under-counted.

**It governs what an order adds, so it never blocks the way out.** An order that does not raise
reachable exposure is permitted however far over the ceiling the account already sits — a sell
against a long position up to flat, or a buy covering a short up to flat. The corollary is a real
loosening: such an order is allowed even when it is larger than the ceiling itself.

It is *not* true that any order opposing a position is exempt. Selling 300 against a long of 100
reaches a short of 200, which is more reachable exposure than the 100 it started from, and the same
rule refuses it. The test is what the order does to the projection, not which way it points. If you
wanted a cap on a single order, that is `max_order_notional_`, which is unchanged.

**An account it cannot value refuses everything.** Every instrument the account holds or has working
needs registered reference data, and a mark. One that has neither is not quietly left out of the sum —
it would under-count, which is the direction a risk limit must not err in — so the order is refused
with `GuardReason::VALUE_NOT_KNOWN` until you register it.

## What this does not do yet

- **Stream.** It polls. `plan/todo/realtime-streaming.md` drops into the same source seam.
- **Run more than one loop.** One loop, one thread. See `plan/todo/parallel-execution.md`.
- **Tell you whether the sellable figure already excludes your resting orders.** Neither broker
  documents it, so the guard subtracts them — safe, and possibly twice. Turn on
  `probe_sellable_convention()` and a run that observes one says so in `report.notes_`.

Surviving a restart is no longer on this list. Bind a `runner::FileJournal` and the ledger becomes
durable: every instruction is recorded before it is sent, a restarted runner recovers what its
predecessor left owed, and the listings this source already reads resolve it. See the
[recovering after a crash](recovering-after-a-crash.md) vignette — including why `not_placed` and
`undecided` are the pair to get right, and why an `ambiguous` verdict is a stop rather than a retry.

## Trying it without a broker

`examples/example-live-loop.cpp` runs the real loop against a source that invents its data — no
credentials, no network, and it finishes instantly because it uses `ManualClock`. It is also the
shortest worked example of writing a source for a broker this library does not support: implement
`poll` and `read_order_list`, and the cadence, the stop intents, the gates and the failure policy all
come along unchanged.

**That is enough only because the example's runner sends nothing** (`runner::DryRunner`). A source that
feeds a runner which sends to a broker owes more, and `live::SessionSource` is the reference for each
of these:

- **A working-order listing, published (`provides_working_orders`) and polled, that replaces
  `working_` wholesale** and stamps `working_as_of_`. An order the runner sent counts as working until
  that listing arrives. A source that merges the listing in leaves an expired or refused order working
  for ever, and the loop cannot see it: the row silently stands in for the target. A source that
  publishes or polls no listing is held instead.
- **A holdings poll that keeps `working_`**, replacing only the positions, their `sellable_basis_` and
  `as_of_`. Rebuilt from the holdings answer alone, the portfolio drops the order just sent, and the
  next bar sends it again. Leave `as_of_` unstamped and the run holds as never read; leave
  `sellable_basis_` undeclared and every checked sell refuses.
- **Answer a listing poll `UPDATED` or `UNCHANGED` only when the broker answered.** The loop dates the
  listing by that outcome, so a made-up answer keeps the gate open on a stale listing.
- **Bars: each closed bar once, none still forming, history through `Runner::feed_warmup`.** Only
  each instrument's newest bar, by the rule in "The history a cold start reads", goes through `feed`.
  A bar fed twice sends an `OrderIntent` twice; a broker's daily query includes the session still
  trading.
- **A calendar written during `PollKind::CALENDAR` only**, and no bar fed from that poll ("The market
  phase, and what the loop needs to know it", above).
- **For a journaling binding, listings handed to the journal's reconciliation**
  (`SessionSource::set_journal`).
