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
| `<algotradelib/live>` | `SessionExecutor`, over a bound `BrokerSession` | yes |

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
transport-free target, which is recorded as a todo rather than done.

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
dry.context().set_phase(strategy::MarketPhase::OPEN);
dry.context().set_last_price(instrument, last_close);

const auto report = dry.feed(bar_event);
for (const auto& order : report.would_place_) {
  // what would have been sent
}
```

`DryRunner` runs the entire live pipeline minus the session: handlers fire, reconciliation builds
instructions, and the guards accept or refuse them. It is one configuration of the same `Runner` a
live session uses, not a parallel implementation — a dry run that ran different code would be testing
something other than what ships.

The report says everything that happened, and the distinctions matter:

| Field | Meaning |
| --- | --- |
| `would_place_` | passed reconciliation and every guard |
| `would_cancel_` | working orders that would be withdrawn, **before** any placement |
| `rejected_` | reconciliation could not build an instruction (`RejectReason`) |
| `refused_` | a guard forbade it (`GuardReason`), with the limit and what was attempted |
| `withheld_` | the instrument's order state is unresolved; reported, never retried |
| `indeterminate_` | the send failed in transit and the outcome is unknown |
| `unresolved_cancel_` | a cancellation naming an order the snapshot does not hold |

`rejected_` and `refused_` are separate on purpose: the first means the library could not build an
order, the second means the operator forbade one. Conflating them would make a configuration mistake
look like a data problem.

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

**A guard rejects; it never resizes.** Halving an order that exceeded a ceiling would place a
position the strategy never sized against a signal it never gave — and the strategy would then
reconcile against that position and compound it.

Two defaults are deliberately not "unlimited". `permitted_phases_` defaults to trading only while the
market is `OPEN`, and an absent value explicitly means any phase, so `MarketPhase::UNKNOWN` — an
uncovered calendar date — is refused unless you ask for it. The rate limit counts over a sliding
window rather than resetting on a boundary, and budget is consumed when an order is *sent*, not when
it is checked.

## How finely a target is chased

```cpp
runner.set_minimum_trade(algotradelib::Decimal64::parse("5"));
```

An execution-cost policy, so it lives with the runner rather than in a strategy: a strategy states the
position it wants, and whether a one-share drift is worth an instruction depends on commission, on
whether the venue charges per instruction, and on what queue priority is worth.

Unset by default, which chases every share. Set, it suppresses a drift smaller than the value — a
target moving from 100 to 99 would otherwise withdraw a working order for 100 and place a new one for
99, spending two instructions to move the position by one share.

Two things it deliberately does not do. It measures the **drift**, not the quantity that would be
sent, because those differ in exactly the case it exists for. And it never suppresses withdrawing an
order that is working the *wrong way*: leaving a sell resting while buying toward a target works
against the target rather than merely failing to reach it.

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
changes, and nothing in the loop around it changes. `example-strategy-live` compiles exactly one
`trade()` function template for both.

Fill the context from what the session reads — reference data first, because without a tick grid and
a lot size reconciliation has nothing to build and will refuse every order by name.

Note that the runner is held **by value**, not behind a smart pointer, and so is the strategy inside
it. The strategy is a template parameter resolved at compile time, so there is no base class to point
at and no dynamic dispatch to enable; an indirection would cost a dereference in an otherwise inlined
handler and buy nothing. Holding by value is also what lets a caller read the state a strategy kept.
What *is* held by pointer is the session, because `SessionExecutor` borrows a connection it does not
own. The rule is ownership rather than size.

## The rule that matters most

An order POST is not idempotent on either supported broker. A request that fails **in transit** has
an outcome nobody knows: the broker may have recorded the order and lost the reply, or never received
it at all.

So a failure is classified by whether the order might exist, and four categories say it might:

| Category | Why it is unknown |
| --- | --- |
| `TRANSPORT` | the connection failed; did the bytes arrive? |
| `CANCELLED` | a *local* deadline or a closing session says nothing about what was already sent |
| `PROTOCOL` | the broker answered and the answer could not be parsed — it may have been an ack |
| `HTTP` 5xx | the broker's own infrastructure failing, possibly after the engine took the order |

A 4xx, an authentication failure, and a rate-limit refusal are all *answers*: the order does not
exist, so nothing needs reconciling.

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

**It does not withdraw anything.** An order the broker already accepted stays live on the exchange,
which knows nothing about a process that died. Killing a program is not a kill switch in the
exchange's sense, and `halt()` is not a flatten. Flattening is a decision — `vanilla::TimedLiquidation`
makes it on a session boundary, or an operator makes it through the broker's own tools.

**And the reconciliation obligation is lost with it.** `IndeterminacyLedger` lives in memory. Two
distinct problems follow:

- A mark made a moment before termination is gone.
- An order *in flight* at the moment of a `Ctrl+C` never reaches the classification at all. A default
  `SIGINT` handler terminates immediately, so no error path runs, `outcome_of` is never called, and
  nothing records that the outcome is unknown.

So **after any abnormal termination, read the broker's order list before trading again.** A restarted
runner starts with an empty ledger, and an empty ledger does not mean nothing is owed — it means
nothing is *remembered*. Until that is automatic, it is the operator's obligation; see
`plan/todo/durable-indeterminacy.md`.

The exposure is bounded in practice by how long a runner runs: this branch ships no polling loop, so a
program does one pass and exits, and at most the orders of that pass can be in doubt.

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
