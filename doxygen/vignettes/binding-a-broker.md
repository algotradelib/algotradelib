# Binding a broker {#binding-a-broker}

A strategy names no broker. A **runner** does, in one type parameter, and that is the only place the
choice appears. This page is about running a strategy: first with no broker at all, then against a
real one.

See @ref writing-a-strategy for writing the strategy itself.

## Three umbrellas, and the line between them

| Header | What it gives you | Links TLS? |
| --- | --- | --- |
| `<algotradelib/strategy>` | the surface a strategy is written against | no |
| `<algotradelib/runner>` | the context, the dispatcher, `DryRunner` | **no** |
| `<algotradelib/live>` | `SessionExecutor`, over a bound `BrokerSession` | yes |

The split is load-bearing rather than tidy. `<algotradelib/runner>` reaches no broker adapter, no
Boost.Asio, and no OpenSSL, which is asserted at compile time from a translation unit that includes
nothing else. So a dry-run binary contains **no symbol that could place an order** — a stronger
promise than a flag, because a function that is not linked cannot be called.

Including `<algotradelib/live>` deliberately gives that up.

## Running with no broker

```cpp
#include <algotradelib/runner>
#include <algotradelib/strategies>

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

## Order of operations

`Plan` returns cancellations and placements separately, and the runner applies **cancellations
first**. A replacement placed before its predecessor is withdrawn is briefly a double position;
cancelling first leaves the instrument unrepresented for a moment instead, which is the lesser
exposure and the one a strategy can reason about.

## Observing against a live session

The honest way to watch a strategy against real market data without trading is to halt the guard
rather than to skip the send:

```cpp
live.guard().halt();
```

The pipeline then runs exactly as it would live, and what stops the order is the same kill switch an
operator uses. Skipping the send instead would exercise a different code path from the one that
ships, which is the defect this whole arrangement exists to avoid.
