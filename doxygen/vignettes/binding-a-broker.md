# Binding a broker {#binding-a-broker}

A strategy names no broker. A **runner** does, in one type parameter, and that is the only place the
choice appears. This page is about running a strategy: first with no broker at all, then against a
real one.

See @ref writing-a-strategy for writing the strategy itself.

## Four umbrellas, and the line between them

| Header | What it gives you | Reaches transport? |
| --- | --- | --- |
| `<algotradelib/strategy>` | the surface a strategy is written against | no |
| `<algotradelib/vanilla>` | the strategies this library ships | no |
| `<algotradelib/runner>` | the context, the dispatcher, `DryRunner` | **no** |
| `<algotradelib/live>` | `SessionExecutor`, over a bound `BlockingSession` | yes |

`<algotradelib/vanilla>` is named for the namespace it fills, `strategy::vanilla`, rather than as a
plural of `strategy` — two headers differing only in an `s` are indistinguishable in an autocomplete
list.

The split is load-bearing rather than tidy. `<algotradelib/runner>` reaches no broker adapter, no
Boost.Asio, and no OpenSSL, asserted at compile time from a translation unit that includes nothing
else. So a dry-run binary instantiates **no adapter and no transport code** — confirmed by symbol
inspection of an installed consumer, which finds no `algotradelib::network`, `algotradelib::kis` or
`algotradelib::tossinvest` symbol at all. There is no instantiated code path from a dry run to a
market, which is a stronger guarantee than a runtime flag that can be set wrong.

Be precise about what that is not: libssl is still on the link line. The package exports one
interface target carrying the transport dependencies for every consumer, so a dry-run binary has the
library as a dylib dependency it never calls. Making the promise literal needs a second,
transport-free target, which the library does not have yet.

Including `<algotradelib/live>` deliberately gives up the compile-time half as well.

## Running with no broker

```cpp
#include <algotradelib/runner>
#include <algotradelib/vanilla>

namespace runner = algotradelib::runner;
namespace strategy = algotradelib::strategy;
namespace vanilla = algotradelib::strategy::vanilla;

vanilla::BuyAndHold holding;
holding.params_.instrument_ = instrument;
holding.params_.quantity_ = algotradelib::Decimal64::parse("10");

runner::DryRunner<vanilla::BuyAndHold> dry(holding);
dry.context().set_instrument(reference);      // reference data, with its PriceRule
dry.context().set_phase(strategy::MarketPhase::OPEN);   // one pass, driven by hand: see below
dry.context().set_last_price(instrument, last_close);

const auto report = dry.feed(bar_event);
for (const auto& order : report.would_place_) {
  // what would have been sent
}
```

**That `set_phase` belongs to a hand-driven pass and nowhere else.** `runner::PollingLoop` derives the
phase itself, each iteration, from a `strategy::SessionWindow` and the trading calendar it polls — and
it overwrites anything set here. A `Runner` fed by hand has no clock of its own, so it is the caller's
to state; a program that runs continuously uses the loop and does not state it at all.



`DryRunner` runs the entire live pipeline minus the session: handlers fire, reconciliation builds
instructions, and the guards accept or refuse them. It is one configuration of the same `Runner` a
live session uses, not a parallel implementation — a dry run that ran different code would be testing
something other than what ships.

The report says everything that happened, and the distinctions matter:

| Field | Meaning |
| --- | --- |
| `would_place_` | passed reconciliation and every guard |
| `would_cancel_` | working orders the broker **accepted** a withdrawal for, **before** any placement |
| `rejected_` | it was not placed, with the `RejectReason` saying why: reconciliation could not build it, the broker refused it (`BROKER_REFUSED`), or the binding refused to send it (`BINDING_REFUSED`) |
| `refused_` | a guard forbade it (`GuardReason`), with the limit and what was attempted |
| `withheld_` | the instrument's order state is unresolved; reported, never retried |
| `indeterminate_` | the outcome is unknown (see the rule below: a failure in transit, an unreadable reply, an HTTP 5xx, or a broker refusal reported as `PROTOCOL`) |
| `unavailable_` | local machinery could not send it; no broker was asked |

A cancellation has the same outcomes and its own bucket for each, because only `would_cancel_` means
the broker accepted the request:

| Field | Meaning |
| --- | --- |
| `rejected_cancel_` | the broker refused the request, or the binding refused to send it; `by_broker_` says which |
| `refused_cancel_` | a hold on the guard refused it, with the `HaltReason` |
| `indeterminate_cancel_` | the outcome is unknown, as for `indeterminate_`; the order may or may not have been withdrawn |
| `unavailable_cancel_` | local machinery could not send it; no broker was asked |
| `unresolved_cancel_` | it named an order the snapshot does not hold |

**None of these — `would_cancel_` included — tells you whether the order is still working.**
Cancellation is asynchronous at both brokers: Toss publishes `PENDING_CANCEL` while the request is in
flight and `CANCEL_REJECTED` when it fails, which returns the order to its previous state. So an
accepted withdrawal can still be resting, and a refused one may have filled a moment earlier — a fill
is an ordinary reason to refuse a withdrawal. These buckets say what happened to the *instruction*.
For the order's state, read the broker's order list.

A binding refuses rather than sends when the instruction asks for something it cannot do — an
idempotency key at a broker that accepts none, a withdrawal for an order the broker has no
reference for — and so does a **client**, for an instruction whose shape it validates before any
I/O: an order id too long for the broker's format, a time in force it does not express. Both are
`SendOutcome::REFUSED_LOCALLY`, which is a state of its own rather than a rejection because
**nothing was sent, so no request may be counted against the rate window**:
spending budget for a request nobody made refuses a later order that would have been allowed. It is
not `UNAVAILABLE` either, which is sticky and stops the runner trading — the next instruction may be
perfectly sendable.

**Which one a given entry was is on the entry**, as `RejectReason::BROKER_REFUSED` or
`BINDING_REFUSED` on a `rejected_` row, and as `by_broker_` on a `rejected_cancel_` row. Not in
`last_error()`: a pass sends several instructions and a later accepted one clears it, so a report
would outlive the only record of why an earlier entry is there.

`rejected_` and `refused_` are separate on purpose: the first means the order was not placed —
usually because the library could not build one, sometimes because the broker or the binding refused
it — while the second means the operator forbade one. Conflating them would make a configuration
mistake look like a data problem.

## Setting the limits

```cpp
strategy::RiskLimits limits;
limits.max_order_notional_ = algotradelib::Decimal64::parse("5000000");
limits.max_position_quantity_ = algotradelib::Decimal64::parse("100");
limits.max_gross_exposure_ = algotradelib::Decimal64::parse("50000000");
limits.max_orders_per_window_ = 10;
limits.rate_window_ = std::chrono::minutes(1);

runner::DryRunner<MyStrategy> dry(MyStrategy{}, limits);
dry.guard().halt();     // the kill switch, checked before anything else
```

`max_position_quantity_` is measured against the position that would exist if the order **and
everything already working on its side** filled, not against the last snapshot's holding. Two buys
of sixty against a ceiling of a hundred are refused on the second, whether the first is still in the
same pass or already resting at the broker. A resting order on the *other* side is not counted as
relief, because relief assumes a fill that may never come.

`halt()` with no argument is **your** hold, `HaltReason::OPERATOR`. Machinery holds the same guard
for its own reasons — a polling loop whose holdings snapshot is stale, a runner whose journal cannot
be written — and each releases only what it took, so nothing else can clear your halt and your
`resume()` cannot clear theirs. `guard().holds()` says what is in force when more than one thing is.

While any hold stands the runner places **nothing**. Cancellations depend on the hold
(`strategy::halt_blocks`): `OPERATOR` and `STOPPING` refuse them too (they appear in
`refused_cancel_` with the hold that refused them), `LOCAL_FAILURE` makes them unavailable
(`unavailable_cancel_`), and the data holds (`ORDER_LIST_UNREAD`, `PORTFOLIO_*`,
`BUYING_POWER_UNREAD`, `POLL_FAILING`) let a named cancellation go.

**A guard rejects; it never resizes.** Halving an order that exceeded a ceiling would place a
position the strategy never sized against a signal it never gave — and the strategy would then
reconcile against that position and compound it.

Two defaults are deliberately not "unlimited". `permitted_phases_` defaults to trading only while the
market is `OPEN`, and an absent value explicitly means any phase, so `MarketPhase::UNKNOWN` — an
uncovered calendar date — is refused unless you ask for it. That limit is two-sided: it refuses an
order that opens or increases a position, and permits one that reduces a held position toward flat,
so an exit is expressible in any phase and `vanilla::TimedLiquidation` works under the default. The
rate limit counts over a sliding window rather than resetting on a boundary, and budget is consumed
when an order is *sent*, not when it is checked.

## How finely a target is chased

```cpp
strategy::TradeTolerance tolerance;
tolerance.minimum_notional_ = algotradelib::Decimal64::parse("100000");
tolerance.minimum_fraction_of_working_ = algotradelib::Decimal64::parse("0.02");
runner.set_trade_tolerance(tolerance);
```

An execution-cost policy, so it lives with the runner rather than in a strategy: a strategy states the
position it wants, and whether a one-share drift is worth an instruction depends on commission, on
whether the venue charges per instruction, and on what queue priority is worth.

**Every floor is unset by default, which chases every share.** No number is invented, because each
depends on a broker's own fee schedule.

### Three units, and why one share count is the weakest

A single share count means very different things across a portfolio. With `minimum_quantity_` set to
five:

| Instrument at | Implied value threshold |
| --- | --- |
| 200,000 | 1,000,000 |
| 70,000 | 350,000 |
| 20,000 | 100,000 |
| 3,000 | 15,000 |

A factor of sixty-seven for one setting. So there are three floors, and which one matters depends on
which cost dominates:

| Floor | Keys on | The cost it answers |
| --- | --- | --- |
| `minimum_quantity_` | the drift in shares | lot economics, or a per-share fee |
| `minimum_notional_` | the drift's value | a fixed or minimum fee per order |
| `minimum_fraction_of_working_` | drift over what is working | queue priority given up |

The third is the subtle one and often the largest. Take Samsung at 70,000 with a target of 100 drifting
to 99: the drift is one share, worth 70,000, and one percent of the order already resting. Replacing
that order risks its **whole** place in the book to adjust one percent of it — the loss is proportional
to the 100, not to the 1. Which is why it applies only when something is working: with nothing resting
there is no priority to give up, and a drift is simply a new order.

### Two rules worth knowing

**Any stated floor being met is enough to suppress**, because each is an independent reason not to
trade. Requiring all of them would mean a drift worth less than a fixed fee still traded because it
happened to be a large fraction of a tiny working order.

**A floor that cannot be evaluated does not suppress.** Without a reference price the notional floor is
unknowable, and the honest response is to trade. That is the opposite of `RiskGuard`, where a limit that
cannot be evaluated *refuses* — same reasoning, mirrored consequences: there, failing to refuse risks an
oversized position; here, failing to trade leaves an invisible shortfall, and an invisible shortfall is
worse than visible churn.

### What it does not do

It measures the **drift**, not the quantity that would be sent, because those differ in exactly the
case it exists for. And it never suppresses withdrawing an order that is working the *wrong way*:
leaving a sell resting while buying toward a target works against the target rather than merely failing
to reach it.

## Binding a real broker

```cpp
#include <algotradelib/kis>
#include <algotradelib/live>

using Session = algotradelib::BlockingSession<algotradelib::kis::KisBlockingClient>;
using Executor = algotradelib::live::SessionExecutor<Session>;

Session session(std::move(client), std::move(account));
Executor executor(session);

runner::Runner<
  MyStrategy, typename Session::Caps, typename Session::BrokerTag, Executor
> live(MyStrategy{}, limits, 512, Executor(session));
```

Swapping to Toss changes `KisBlockingClient` to `TossBlockingClient`. Nothing in the strategy
changes, and nothing in the loop around it changes.

**Where orders are routed is the binding's to say, not the strategy's.** A strategy names no venue,
so every order it plans asks for `UNSPECIFIED`, which takes the broker's own default: KRX at KIS, none
at Toss — and a refusal at Kiwoom, which documents no default. Pass the route as the executor's second
argument, `Executor(session, Venue::KRX)`, or as the last argument of `live::bind` and
`live::bind_journaled` (after `live::default_series_capacity`). An order that names a venue keeps it.
The runner writes the route into the order before sending it, so the journal line, the working row it
reserves and its report all name the venue the broker was asked for. At KIS, `Venue::COMBINED` is how
a runner opts into SOR, which a demo account refuses. At KIS and Kiwoom alike, prefer KRX or NXT
until SOR has been observed: how a SOR order is listed back decides whether recovery after a crash
recognises it (`SessionExecutor::default_route` explains). `example-strategy-live` compiles exactly
one `trade()` function template for both.

Fill the context from what the session reads — reference data first, because without a tick grid and
a lot size reconciliation has nothing to build and will refuse every order by name.

**Include the holdings snapshot, and stamp it with when the broker answered.** A runner built on a
`SessionExecutor` will not place against an account it has never read: `SessionExecutor` declares
`reaches_a_broker`, and a `Portfolio` whose `as_of_` is unset reads as one nobody has read rather than
as an account that is empty. That distinction is the point — empty is the *permissive* answer to
every position ceiling and to every target the runner reconciles — so copying the rows without
`received_at_` gets the orders withheld, reported in `Report::withheld_plans_`. A run that genuinely
has no holdings to read says `live.allow_absent_portfolio()` and means it.

An executor you write yourself is **assumed to reach a broker** and needs no marker to be covered.
Declare `static constexpr bool reaches_a_broker = false` on it only if it reaches none — a recorder,
a simulator, a test double. The default runs that way round because the two mistakes are not
symmetrical: an adapter wrongly exempted sends a real order against an account nobody read, while a
double wrongly treated as live withholds until you classify it, which you find out immediately.

Note that the runner is held **by value**, not behind a smart pointer, and so is the strategy inside
it. The strategy is a template parameter resolved at compile time, so there is no base class to point
at and no dynamic dispatch to enable; an indirection would cost a dereference in an otherwise inlined
handler and buy nothing. Holding by value is also what lets a caller read the state a strategy kept.
What *is* held by pointer is the session, because `SessionExecutor` borrows a connection it does not
own. The rule is ownership rather than size.

## What each broker will tell you about the account

Three questions, and the brokers do not answer them the same way. `BrokerCaps` says which you can
ask before you write the call, so one strategy source compiles against all three.

| | Toss | KIS | Kiwoom |
| --- | --- | --- | --- |
| `has_buying_power` | yes | yes | yes |
| `has_buying_power_quantity` | no — an amount only | yes — also how many shares | yes — per margin tier, through `session.client()` |
| `sellable_source` | `INQUIRY` — one request per instrument | `HOLDINGS` — free, on every snapshot | `NONE` — not yet: the session's holdings are refused until you assert `KiwoomConfig::holdings_position_scope_`; the rows then carry `trde_able_qty`, which `RiskGuard` uses only if `holdings_sellable_scope_` is `WHOLE_HOLDING` |
| `has_commission_schedule` | yes | no | no |

`sellable_source` names a **source** rather than asking `has_*`, and the distinction is deliberate:
KIS does publish a dedicated 매도가능수량조회, and this library does not use it, because the same
field arrives on the holdings snapshot for nothing and on a demo account too. The constant says where
the figure comes from, not what the vendor offers; for Kiwoom, `NONE` means no source this library stands
behind yet.

Calling a verb a broker cannot serve is a compile error naming the broker and what to do instead:

```
session.commissions();        // on KIS: "this broker publishes no commission schedule; gate the
                              //  call with BrokerCaps<Client>::has_commission_schedule"
session.sellable_quantity(i); // on KIS: "read HoldingPosition::sellable_quantity_ from the
                              //  holdings snapshot"
```

Buying power is per currency and the broker prices it that way, so ask for the one you trade in. Toss
answers KRW and USD; the KIS and Kiwoom domestic inquiries answer won, and any other currency is
refused before the request is built rather than answered in the wrong unit.

## The rule that matters most

No order request is ever replayed. Only Toss offers an idempotency key (`client_order_id_`, ten
minutes); KIS and Kiwoom offer none. A request that fails **in transit** has an outcome nobody knows:
the broker may have recorded the order and lost the reply, or never received it at all.

So a failure is classified by whether the order might exist, and four categories say it might:

| Category | Why it is unknown |
| --- | --- |
| `TRANSPORT` | the connection failed; did the bytes arrive? |
| `CANCELLED` | a *local* deadline or a closing session says nothing about what was already sent |
| `PROTOCOL` | the answer could not be parsed — it may have been an ack — or the broker declared a failure the client does not classify (at KIS an unrecognized reply with `rt_cd` not `0`, such as insufficient balance; at Toss any refusal but a rate limit; at Kiwoom a `return_code` outside its documented error list) |
| `HTTP` 5xx | the broker's own infrastructure failing, possibly after the engine took the order |

An HTTP 4xx, an authentication failure, an invalid argument and a rate-limit refusal are *answers*:
the order does not exist, so nothing needs reconciling. At KIS, Toss and Kiwoom most refusals arrive
as `PROTOCOL` instead, so they are treated as unknown and the instrument is reconciled.

Three refusals are recognized as answers, on the KIS and Kiwoom **demo** servers only: a cash sell
refused because nothing is held to sell (KIS `40240000`, Kiwoom `800033`), a cash order refused because
the market has closed (KIS `40580000`, Kiwoom `RC4058`), and a cancellation refused because nothing is
left to cancel (KIS `40330000`, Kiwoom `RC4033`), which is what a cancellation of an order that has
just filled gets. The reply must be an HTTP 200 that states the code and carries no new
acknowledgement (the original order's number, echoed, is not one); it is then `INVALID_ARGUMENT` with
`reached_broker_` set, and its message ends by naming the rule: `recognized refusal: KIS demo, cash
cancellation, 40330000)`, for example. The same reply from a real server is not recognized: no real-account
refusal has been seen, so there it is still an unknown outcome. A refused cancellation says that the
cancellation did nothing, not what became of the order.

When the outcome is unknown, the runner marks the instrument and **withholds everything further for
it**. Not retried — a retry is exactly what doubles a position. Nothing clears the mark except
reading the broker's own order list:

```cpp
if (!live.ledger().empty()) {
  for (const auto& id : live.ledger().pending()) {
    // read session.orders() / session.working_orders() for `id`, decide what exists,
    // then release it:
    live.ledger().resolve_after_reading_orders(id);
  }
}
```

The method is named for what it requires rather than what it does, because calling it without having
read that list is the one way to defeat the mechanism. A timeout would resume trading on a guess; a
later success on a *different* instruction says nothing about the one that vanished.

The hold is per instrument, so one unresolved name does not silence a cross-sectional strategy. And a
cancellation is never itself withheld — withdrawing an order is the one action that cannot make an
unresolved position worse.

The asymmetry underneath all of this is worth stating plainly: calling an indeterminate outcome a
rejection risks an unwatched live order and a doubled position, while calling a rejection
indeterminate costs one reconciliation. Only one of those can lose money, so an unrecognized error
category is treated as unknown.

## What killing the process does, and does not do

Worth being exact about, because the answer is reassuring in one direction and not in the other.

**It stops the runner.** There is no runner, so nothing further is sent. `guard().halt()` is the same
kind of thing: it stops *this program* from sending.

Three things "stop" can mean, which are worth keeping apart because each leaves a different position:
*quiesce* (no new orders, positions held, working orders still resting — this is what `halt()` does),
*withdraw* (quiesce, then cancel the working orders), and *flatten* (withdraw, then close the
positions). Flattening is a strategy's decision — `vanilla::TimedLiquidation` makes it on a session
boundary — or an operator's through the broker's own tools.

**It does not withdraw anything.** An order the broker already accepted stays live on the exchange,
which knows nothing about a process that died. Killing a program is not a kill switch in the
exchange's sense, and `halt()` is not a flatten. Flattening is a decision — `vanilla::TimedLiquidation`
makes it on a session boundary, or an operator makes it through the broker's own tools.

**And the reconciliation obligation is lost with it.** `IndeterminacyLedger` lives in memory. Two
things follow:

- A mark made a moment before termination is gone.
- An order *in flight* at the moment of a `Ctrl+C` never reaches the classification at all. A default
  `SIGINT` handler terminates immediately, so no error path runs, `outcome_of` is never called, and
  nothing records that the outcome is unknown.

So **after any abnormal termination, the broker's order list must be read before trading again.** A
restarted runner starts with an empty ledger, and an empty ledger does not mean nothing is owed — it
means nothing is *remembered*.

**That is automatic, and it is worth knowing which part does what.** `runner::PollingLoop` will
not send anything until it has read the broker's order list once, so even with no journal at all a
restarted process cannot trade blind. With a journal bound through `live::bind_journaled`, the
instructions themselves survive the process: recovery reads them back, withholds every instrument
whose outcome is unknown, and resolves them against the broker's own listings. See the
[recovering after a crash](recovering-after-a-crash.md) vignette for what that looks like during an
incident.

**The stop you choose still matters, and the fastest one is the worst.** Closing the session out from
under an in-flight request surfaces as `ErrorCategory::CANCELLED`, which is classified
`INDETERMINATE` — so aborting the connection *manufactures* the very state that has to be reconciled.
A graceful stop that quiesces and lets the request in flight complete leaves nothing in doubt, at the
cost of one loop iteration. `runner::StopIntent` names the three stops, `runner::SignalStop` and
`runner::FileStopChannel` deliver them from outside the process, and the
[running a strategy live](running-a-strategy-live.md) vignette covers the grace period a supervisor
has to allow so its `SIGKILL` does not convert the cleanest stop into the worst one.

## Order of operations

`Plan` returns cancellations and placements separately, and the runner applies **cancellations
first**. A replacement placed before its predecessor is withdrawn is briefly a double position;
cancelling first leaves the instrument unrepresented for a moment instead, which is the lesser
exposure and the one a strategy can reason about.

## Observing against a live session

To watch a strategy against real market data without trading, keep the session for the data and give
the *send step* an executor that has no session:

```cpp
// Trading: instructions go to the broker.
drive<Caps, Tag>(instrument, state, live::SessionExecutor<Session>(session));

// Observing: identical pipeline, identical data, and nothing for an order to go through.
drive<Caps, Tag>(instrument, state, runner::RecordingExecutor<Tag>{});
```

Both paths run the same `Runner` over the same real bars. The difference is where the send goes, which
is the one thing that should differ — so observing exercises the code that ships rather than a
variant of it.

Two alternatives are worth rejecting explicitly. **Skipping the send** behind an `if` would leave the
observe path running different code from the live one, which is the defect this whole arrangement
exists to avoid. **Halting the guard** keeps the code path but reports every decision as `refused_`
with `TRADING_HALTED` — which reads like a failure when it is the intended mode, and leaves the send
path present and merely unreached. Neither is wrong exactly; swapping the executor is simply more
honest about what is happening.

The kill switch is still the right tool for its own job: stopping a *live* runner that is trading.
