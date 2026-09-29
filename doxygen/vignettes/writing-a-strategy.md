# Writing a strategy {#writing-a-strategy}

A strategy is a plain struct with template member functions. It receives events and emits intents.
It performs no I/O, holds no session, names no broker, and therefore runs unchanged in a dry run, in
a live session against any of the supported brokers, and later in a backtest.

```cpp
#include <algotradelib/strategy>

namespace atl = algotradelib;
namespace strategy = algotradelib::strategy;

struct MyStrategy {
  struct Params {
    atl::InstrumentId instrument_;
    atl::Decimal64 quantity_;
  };
  Params params_;

  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    if (!(bar.instrument_ == params_.instrument_)) {
      return;
    }
    strategy::TargetPosition target;
    target.instrument_ = params_.instrument_;
    target.target_quantity_ = params_.quantity_;
    target.tag_ = "my-strategy";
    ctx.submit(target);
  }
};
```

That is a complete strategy. Every handler is optional, so a strategy reacting only to bars writes
only `on_bar` and costs nothing for the rest.

## The handlers

| Handler | Called when |
| --- | --- |
| `on_start(Ctx&)` | before the first event |
| `on_bar(Ctx&, const BarEvent&)` | a bar has **closed** |
| `on_quote(Ctx&, const QuoteEvent&)` | a quote was read |
| `on_fill(Ctx&, const Fill&)` | one execution happened against an order of yours — **in a backtest only**, see below |
| `on_order_update(Ctx&, const OrderSnapshot&)` | an order the broker is still listing changed — see below |
| `on_phase(Ctx&, MarketPhase)` | the session moved between phases |
| `on_stop(Ctx&)` | after the last event — see below, it cannot trade |

**`on_fill` does not fire live, and this is the one difference between the two modes that can cost
money.** A fill is discovered live by polling: positions are replaced and working orders are
replaced, and neither synthesises an execution. Discrete executions cannot be recovered from
periodic cumulative snapshots — that was established across thirteen review findings and the design
is parked until the library streams execution notifications. So a strategy that places an entry and
sets its stop from `on_fill` is **correct in a backtest and inert live**: the handler never runs, the
stop is never placed, and nothing reports it.

**`on_order_update` is not a replacement for it, and treating it as one recreates the same trap.**
It reports rows that appear in a *listing*, and a market order can be sent, fill, and leave the
working-order list entirely between two polls — no row is ever seen, so no update is ever emitted.

**So say the hard part plainly: this library cannot yet guarantee you a protective order the moment
an entry fills, live.** Nothing polled can. Until execution notifications are streamed, every
available signal is periodic, and there is no arrangement of the handlers below that closes the gap
between a fill happening and your process learning of it.

What you can do is **observe the position** and act on the difference as soon as you see it: compare
`ctx.portfolio().net_quantity_of(...)` against the position you intended, on each `on_bar`. That is
remediation after the fact, not protection — read the three limits of it before relying on it:

- **It is as late as your holdings cadence.** `positions_` changes only when a `HOLDINGS` poll
  replaces it, so an entry that filled between polls is invisible until the next one.
- **It is one poll later than you might expect inside an iteration.** The loop polls `BARS` before
  `HOLDINGS`, and `on_bar` fires during the bar poll — so the position your handler reads is the one
  the *latest successful holdings poll* established, not one including a fill this iteration is
  about to discover. That poll need not have been the previous iteration's, and it may have failed.
- **On daily bars, "the next `on_bar`" may be tomorrow.** A shorter bar interval narrows the window.
  A faster holdings cadence does **not** on its own, because the check runs in `on_bar`: a fresher
  position nobody looks at until the next bar is still not looked at until the next bar.

**And do not reach for the obvious escape from a strategy, because the strategy layer does not have
it yet.** A broker-side stop would close the gap by leaving the trigger at the exchange, and the broker
port can now place one — `OrderRequest::trigger_price_` carries it to KIS's `ORD_DVSN` `22` and
Kiwoom's `trde_tp` `28` — but a strategy states targets and intents, which reconcile into limit and
market orders only, and a replay refuses a stop-limit order rather than model one. Until both of those
change, a stop placed beside a strategy through the session directly is an order the strategy's
backtest never saw.

**And placing the exit alongside the entry is not the answer either**, which is worth saying because
it is the next thing anyone tries. The two are independent submissions: there is no linked
entry-and-stop, and no reduce-only condition. So the exit can execute *first* — opening the position
it was meant to close — or rest at the exchange after the entry was rejected, leaving an order
against a position you never took.

**Two options actually exist, and only one of them is inside this library.**

1. **Size the entry so an unprotected interval is survivable.** This is the only choice that does not
   depend on timing at all, and it is the one this library supports today.
2. **Place the entry-linked protective order through a channel that has one** — the broker's own
   application or an API this library does not wrap. That is explicitly *outside* here, and naming
   it as outside is the point: nothing in this library can create that order for you.

Whichever you choose, treat the window as the open gap it is rather than as something a callback
hides.

**`on_order_update` reports what the broker last said about an order it is still listing.** The same
detector runs over each mode's own order listing, so the *rule* is identical in a backtest and live
— but what each mode can show you is not, because a replay is a simpler broker than a real one:

| It fires when | Backtest | Live |
| --- | --- | --- |
| an order appears in a listing, resting | yes | **only if a poll sees it there** |
| an amendment moves its price or quantity | no — a replay has no amendment, only place and cancel | yes |
| the filled quantity moves while the order rests | no — a replay fills an order whole or not at all | yes |

So in a backtest you will see one update per order, when it starts resting — with one exception
worth knowing: an order placed by `on_order_update` itself *during* `finish` has no later
observation boundary and is never reported. Do not read a single update as the library telling you
nothing happened afterwards; read the transition table above.

**And read the live column's condition as literally as it is written.** An order only produces an
update if a working-order poll actually observes a row for it. An order that is sent and completes
inside one polling interval never appears in any listing and is never reported — which is why the
first row says *appears in a listing* rather than *is accepted by the broker*.

**It never tells you why an order stopped being listed.** A working-order listing shows what is
resting, and a row that leaves it may have filled, been cancelled, expired or been rejected; the
listing does not say which, and this library will not guess. So do not wait for a terminal update
unless `BrokerCaps<Client>::working_orders_state_terminal` says the broker states one — it is
`false` on all three shipped adapters.

To learn what actually became of an order, read the two things that can answer it: **holdings** give
you the resulting *position*, and an **order-history inquiry** gives you that order's own outcome —
filled, cancelled, rejected or expired. Holdings alone cannot tell you which order did it.

Two more things worth knowing before you rely on it. An order that was already resting when the
runner started is adopted as the baseline and is not reported as an update: the startup read happens
before the loop's startup hold is installed, so nothing is dispatched from it, and that listing
reaches you through `ctx.portfolio().working_` instead. And an order you place *from*
`on_order_update` is reported at the next observation rather than inside the current one — subject
to the same condition as everything else here, that a later listing actually shows it.

**`on_stop` is a notification, not a last trade.** Under `runner::PollingLoop` it is dispatched while
the loop's stop hold is in force, so anything it submits is refused by the guard and counted in
`stop_handler_refused_`. That is deliberate: a stop belongs to the operator, who has asked for a
particular thing — quiesce, withdraw or flatten — and the loop sizes its closing orders against what
it can prove is outstanding. An order added by the strategy at that moment goes through none of that
reasoning and can close the same position twice. Use `on_stop` to write state, flush a log, or
record where you were; use `on_bar` to decide what to trade, including on a session boundary, which
is what `vanilla::TimedLiquidation` does.

A runner driven directly, without the loop, still gets the old behaviour from `Runner::stop()`:
`on_stop` runs and what it asks for is settled. The difference is the loop, because the loop is
where an operator's stop exists.

**Declare each as a template on the context.** A handler written against one concrete context type
compiles for that runner and silently excludes every other, so the strategy could not be dry-run or
backtested without being rewritten. `ATL_ASSERT_STRATEGY(MyStrategy, Ctx)` catches that: it checks
that at least one handler is detected, and that every handler detected is also detected for two
unrelated probe contexts, which is what proves it is generic.

A consequence worth knowing before it bites: a strategy declared *inside* a function cannot have
template members at all, because C++ forbids templates in local classes. Declare strategies at
namespace scope.

## Emit a target, not an order

Every strategy this library ships emits `TargetPosition`. Do the same unless you have a reason not
to, and the reason is this:

A strategy that emits orders has to remember whether it already entered. After a restart it does
not know, so it either misses an exit or re-enters a position it already holds. Worse, it
double-sizes after a partial fill: the signal still says "hold 100", forty are filled and sixty are
still working, and sizing against the position alone asks for sixty more on top of the sixty already
in the market.

Stating the position you want leaves that arithmetic to reconciliation, which can see the portfolio
*and* the orders already working. `BuyAndHold` never trades twice while remembering nothing at all,
because once the position matches, reconciliation produces nothing.

Use `OrderIntent` when you genuinely mean "trade this much" rather than "hold this much" — and
accept that the sizing is then yours.

## What the context gives you

Every read is from a cache the runner has already filled, so a handler never blocks and never issues
a request:

- `ctx.portfolio()` — positions, per-currency cash, and the orders already working.
  `working_quantity(instrument, side)` is the arithmetic that prevents double-sizing.
- `ctx.instrument(id)` — reference data, carrying the `PriceRule` a limit price needs. Absent means
  the runner has not loaded it, not that there is no grid.
- `ctx.last_price(id)`, `ctx.now()`, `ctx.phase()`, `ctx.calendar()`.
- `ctx.bars(id, interval)` — a contiguous trailing series, available when the library was built with
  a linear-algebra backend. Returns `nullptr` when nothing is cached.
- `ctx.log().note(...)` — structured and non-blocking.

Writes are queued and drained by the runner after the handler returns, which is what keeps order
safety in one place:

- `ctx.submit(TargetPosition)` / `ctx.submit(OrderIntent)`
- `ctx.cancel(ref)` — take the reference from `ctx.portfolio().working_` with `auto`, and your source
  still names no broker.

The queue preserves order, because a cancellation followed by a target means something different
from the reverse.

## Prices

A limit price must be a `strategy::Price`, and a `Price` has no public constructor. The only routes
in validate against the instrument's grid:

```cpp
const auto reference = ctx.instrument(bar.instrument_);
if (!reference) {
  return;
}
const auto limit = strategy::Price::rounded(
  bar.bar_.close_, reference->price_rule_, atl::OrderSide::BUY
);
if (!limit) {
  return;   // the grid is unknown, so no price can be made valid
}
```

`rounded` moves a price onto the grid in the direction that cannot cross your intent — a buy rounds
down, a sell rounds up — so rounding never makes a limit more aggressive than you asked for.
`checked` accepts a price already on the grid. Both return `std::nullopt` when the instrument's
ladder is unknown, which is the honest answer: a guessed tick is an order the exchange rejects at
best.

Computing a price from an indicator? Cross the boundary once, through
`analytics::round_scalar_to_tick`, and hand the exact result to `Price::checked`.

## Warm-up

State your requirement and honour it:

```cpp
std::size_t warmup_bars() const { return params_.window_ + 1; }
```

Then check before acting. `ctx.bars(...)` may be `nullptr`, a series may be shorter than the window,
and an indicator's warm-up region is `missing()`:

```cpp
const auto* series = ctx.bars(bar.instrument_, bar.interval_);
if (series == nullptr) {
  return;
}
const auto closes = series->column(analytics::BarField::CLOSE);
const auto window = strategy::vanilla::trailing(closes, params_.window_);
if (!window) {
  return;
}
const auto line = analytics::sma(*window, params_.window_);
const auto value = line[line.size() - 1];
if (!math::observed(value)) {
  return;
}
```

Two habits in that snippet are worth copying. **Slice before computing**: an indicator over the
whole cache is `O(rows × window)` per bar, and you need only the last value or two, so
`trailing(...)` keeps the per-bar cost from growing with the cache. And **test observability rather
than a value**: `missing()` covers the warm-up *and* a window with no dispersion, where a z-score or
a correlation genuinely does not exist. `math::is_undefined()` separates those two if you care.

## Reaching a broker-specific feature

Under a capability gate, so the broker-specific path is visible and compiled only where it exists:

```cpp
template <class Ctx>
void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
  auto intent = strategy::OrderIntent::market(/* ... */);
  if constexpr (Ctx::caps::has_client_order_id) {
    ctx.submit(intent.with_client_order_id(make_key()));
  } else {
    ctx.submit(intent);
  }
}
```

`ATL_ASSERT_STRATEGY` type-checks **both** branches, because it asserts against a probe context with
every capability present and one with none. A key attached for a broker that accepts none is refused
rather than dropped: dropping it would make a retry look safe on a broker offering no such
guarantee.

## What you do not have to handle

These live in the runner, once, and restating them in a strategy is how they get restated
inconsistently:

- rounding to the tick grid and the lot;
- clamping into today's permitted price band — **where there is a band the runner will stand behind**.
  A band is a per-trading-day value, and one whose age nobody has bounded is not applied at all, so
  the price you state can reach the broker unmodified and be refused by the exchange. Nothing about
  your strategy changes either way: state the price you mean. See @ref running-a-strategy-live for
  the two calls an operator makes;
- diffing a target against the portfolio and the working orders;
- every risk limit, and the kill switch;
- the order-safety rules — in particular that a request failing in transit leaves the outcome
  **unknown**, so nothing further is sent for that instrument until the broker's own order list has
  been read. A strategy never needs to know this happened; it simply finds its target unmet and asks
  again, and the runner keeps withholding until the question is settled.

See @ref binding-a-broker for running one.
