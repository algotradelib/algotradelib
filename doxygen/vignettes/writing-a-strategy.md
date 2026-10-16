# Writing a strategy {#writing-a-strategy}

A strategy is a plain struct with template member functions. It receives events and emits intents.
It performs no I/O, holds no session, names no broker, and therefore runs unchanged in a dry run, in
a live session against any of the supported brokers, and later in a backtest. It names no venue
either: where its orders are routed is the binding's `default_route` (Kiwoom requires one — see the
README's "Brokers and accounts").

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
| `on_bars(Ctx&, const BarSlice&)` | every instrument's bar of a period has closed — instead of `on_bar`, see [below](#several-instruments) |
| `on_quote(Ctx&, const QuoteEvent&)` | a quote was read |
| `on_fill(Ctx&, const Fill&)` | one execution happened against an order of yours — **in a backtest only**, see below |
| `on_order_update(Ctx&, const OrderSnapshot&)` | an order the broker is still listing changed — see below |
| `on_phase(Ctx&, MarketPhase)` | the session moved between phases |
| `on_stop(Ctx&)` | after the last event — see below, it cannot trade |

**`on_fill` does not fire live, and this is the one difference between the two modes that can cost
money.** A fill is discovered live by polling: positions are replaced and working orders are
replaced, and neither synthesises an execution. Discrete executions cannot be recovered from
periodic cumulative snapshots; that waits until the library streams execution notifications. So a
strategy that places an entry and sets its stop from `on_fill` is **correct in a backtest and inert live**: the handler never runs and
the stop is never placed.

**It is reported, not refused.** Using `on_fill` for backtest-only bookkeeping is legitimate, so
nothing stops such a strategy compiling or running live. Instead, a runner that sends to a broker and
whose strategy defines `on_fill` has `Runner::on_fill_never_fires`, and every `PollingLoop` run over it
ends with the note *"strategy defines on_fill; it never fires on this runner"* in
`LoopReport::notes_`. `example-live-program` prints it before trading too. A dry run, an observer and a
replay stay silent: the first two send nothing, and a replay does fire `on_fill`. (A program that
feeds `Fill` events to the runner itself, from a source of its own, does reach `on_fill` and can
ignore the note: "never" describes what this library polls.)

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
- **It is only as fresh as the latest successful holdings poll.** The loop polls working orders, then
  holdings, then bars, so when both account reads are due in an iteration, `on_bar` sees the holdings
  read moments earlier. When holdings were not due, or failed, it sees an older read — and if the
  working-order listing is newer than that read, what your handler decides is withheld as
  `portfolio_superseded` rather than sized against it.
- **On daily bars, "the next `on_bar`" may be tomorrow.** A shorter bar interval narrows the window.
  A faster holdings cadence does **not** on its own, because the check runs in `on_bar`: a fresher
  position nobody looks at until the next bar is still not looked at until the next bar.

**A broker-side stop closes the gap by leaving the trigger at the exchange, and it has five costs
worth knowing before reaching for it.** `OrderIntent::stop_limit(instrument, side, size, trigger,
limit)` places one at KIS (`ORD_DVSN` `22`) and Kiwoom (`trde_tp` `28`); a broker that cannot carry a
trigger refuses it rather than sending a plain limit.

- **A replay models it with stated assumptions.** It releases when a bar reaches the trigger and fills
  as the limit it becomes, at a price inside the bar that never breaches the limit. The report counts
  releases and same-bar fills (`stop_releases_`, `stop_fills_intrabar_`); see
  [Reading a backtest](reading-a-backtest.md).
- **A target that needs a trade withdraws it first.** While a stop rests on an instrument, a
  `TargetPosition` the holding already meets keeps the stop (it is protecting what you mean to hold).
  One that needs a trade withdraws the stop, sends nothing, and is reported as
  `RejectReason::STOP_ORDER_RESTING` — the stop is neither counted as the exit nor left beside a second
  sell. **The runner keeps that target** (`Runner::deferred_targets`) and settles it by itself once
  the broker's **own record of the stop** — read from order history, on demand, in the working-order
  poll that no longer lists it — is terminal with nothing filled. A listing cannot tell a cancelled
  stop from a filled one, and holdings are no proof either. If the stop filled anything, nothing is
  sold: a full fill already met the target, and **the remainder of a partial fill is yours to
  restate** (counted by `Runner::deferred_dropped_after_fill`). No bar, phase change or holdings poll
  is needed. Anything newer the strategy states for the instrument replaces the kept target; it is
  not journalled, so a restart forgets it.
- **It counts before it releases.** A resting sell stop is deducted from what may be sold and counted
  toward position and exposure ceilings at its limit, so an immediate exit beside it may be refused.
- **Its trigger is never moved.** A trigger off the instrument's tick grid is refused as
  `TRIGGER_OFF_GRID` rather than rounded, because which direction is cautious depends on whether the
  stop releases on a rise or a fall, and neither broker documents that.
- **On daily bars it is absent overnight.** It rests as a DAY order — this library has no time in force
  that outlasts one session (`TimeInForce`) — so it is gone at the close. A strategy that states it on
  every bar has it sent again after midnight, when the next daily bar is fed: a broker not taking
  orders refuses it, and it is kept for the first `OPEN` iteration, after 09:00, which needs a trading
  calendar ("kept for the next session" in [Running a strategy live](running-a-strategy-live.md)). From
  the close until then, through the opening auction, the position has no stop. A strategy that states
  it once, on entry, has none after the first close.

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

**Live, a target you state from `on_order_update` is planned against the next holdings read.** The
update arrives during the working-order poll, which runs before holdings, so the account at that moment
predates the listing that produced the update. The target is withheld as `portfolio_superseded`,
retained, and planned again after the loop has read holdings; an `OrderIntent` stated there is withheld
and dropped. So answer an update with a target, not with an order.

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
record where you were; use `on_bar` or `on_phase` to decide what to trade, including on a session
boundary, where `vanilla::TimedLiquidation` flattens from `on_phase`.

A runner driven directly, without the loop, gets different behaviour from `Runner::stop()`:
`on_stop` runs and what it asks for is settled. The difference is the loop, because the loop is
where an operator's stop exists.

**Declare each as a template on the context, taking it by reference and the event as `const&`.**
The runner calls every handler whose name your strategy declares, and nothing else. So a handler the call
does not fit is a compile error at the call, never a handler skipped in silence:
- an `on_fill(Ctx&, Fill&)` with a non-const event, or an `on_phase(Ctx&)` with no event;
- a context taken by value (no context can be copied, because what the handler submitted would go to the
  copy);
- a private member, or one that cannot be called, under a handler's name.

An event taken by value is fine. `ATL_ASSERT_STRATEGY(MyStrategy, Ctx)`, which every runner asserts, makes
that check early:
- the strategy is a class and not `final` (its handler names could not be looked up otherwise; do not mark a
  strategy `final`);
- `Ctx` cannot be copied, from a `const` or a mutable reference (a context of your own must not derive from a
  copyable base either: a handler taking that base by value would get a sliced copy);
- it declares at least one handler;
- **every declared handler's call and body compile for `Ctx`**, including one for an event the program never
  feeds.

A handler disabled with `enable_if` for some contexts is rejected where it is disabled: declare it plainly and
gate inside it with `if constexpr`. A handler written against one concrete context type compiles only for the
runner whose context it takes, and fails where another runner is built. A wrapper that forwards to an inner
strategy through `runner::dispatch` gets the same check on the inner one.

A consequence worth knowing before it bites: a strategy declared *inside* a function cannot have
template members at all, because C++ forbids templates in local classes. Declare strategies at
namespace scope.

## Several instruments at once: `on_bars` {#several-instruments}

`on_bar` receives one bar at a time, so a strategy deciding on several instruments together — a pair's
hedge ratio, a ranking across a universe — has to hold each bar until the others of the same period
arrive. Declare `on_bars` instead and the runner does that: it calls it **once per period end** with a
`BarSlice` holding every instrument's bar for that period, after all of them are cached.

```cpp
struct PairSpread {
  InstrumentId y_;
  InstrumentId x_;

  template <class Ctx>
  void on_bars(Ctx& ctx, const BarSlice& slice) {
    const BarEvent* y = slice.find(y_);
    const BarEvent* x = slice.find(x_);
    if (y == nullptr || x == nullptr) {
      return;  // a leg has no bar this period
    }
    // Both series end at this period here, so a hedge ratio can be fitted on them directly.
    const auto* ys = ctx.bars(y_, slice.interval_);
    const auto* xs = ctx.bars(x_, slice.interval_);
    // ...
  }
};
```

- **One or the other.** A strategy declares `on_bar` or `on_bars`; declaring both does not compile, so
  neither runs in silence beside the other. The rest of the handlers are unaffected.
- **A missing instrument is absent from the slice**, and `find` returns `nullptr` for it. There is no
  deadline and no placeholder: what an absent bar means is the strategy's decision.
- **Several intervals** ending at the same instant (a minute and a day) give one call each, in the
  order their first bars arrived; each slice holds one interval.
- **Instruments that close at different instants** (a Korean and a US stock) never share a slice.
- **In a backtest** a period is delivered when the first later bar arrives, before that bar fills
  anything, or at a quote at or after it, or at `finish()`. Its orders wait for a later period's bar,
  as `on_bar`'s do. Feed every bar of a period before anything later: a bar for a period already
  delivered, an earlier one, or a second bar of one instrument in a period throws
  `std::invalid_argument` before anything changes.
- **Live**, each poll's bars are grouped by period; history periods go through
  `Runner::feed_warmup` as before, and only the newest period can send. A bar that arrives after its
  period was delivered (a name that missed that period's poll) is cached for its own series but not
  delivered, and `SessionSource::bars_not_delivered` counts it.
- **A source of your own** calls `Runner::record` for each bar of a period, then
  `Runner::feed(const BarSlice&)`; `Runner::feed(const BarEvent&)` does not compile for such a
  strategy, so a path still feeding single bars cannot skip it.
- **Without a linear algebra backend** there is no cache: the slice is what the strategy has.

## Emit a target, not an order

Every strategy this library ships states a position: `TargetPosition`, or — for
`EqualWeightRebalance` — an `OrderIntent` sized by `SizeSpec::target_weight`, which the runner plans
as the position the weight states. Do the same unless you have a reason not to, and the reason is
this:

A strategy that emits orders has to remember whether it already entered. After a restart it does
not know, so it either misses an exit or re-enters a position it already holds. Worse, it
double-sizes after a partial fill: the signal still says "hold 100", forty are filled and sixty are
still working, and sizing against the position alone asks for sixty more on top of the sixty already
in the market.

Stating the position you want leaves that arithmetic to reconciliation, which can see the portfolio
*and* the orders already working. `BuyAndHold` never trades twice while remembering nothing at all,
because once the position matches, reconciliation produces nothing.

Use `OrderIntent` when you genuinely mean "trade this much" rather than "hold this much" — and
accept that the sizing is then yours. The one exception is a weight: `SizeSpec::target_weight` means
"hold this fraction", so a weight-sized buy intent is planned as a target and diffed against the
portfolio (a sell, a trigger, a non-day validity or an idempotency key on one is refused). It needs a
portfolio value, which a replay supplies and a live runner does not unless you set
`Runner::set_portfolio_value`. It stays an intent, though:
withheld for the account it is dropped rather than kept for later as a `TargetPosition` would be, and
refused for the market phase (or because the broker could not be reached, or turned it away for its
rate limit) it is kept as an intent is, with the check an intent gets at release.

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
- `ctx.log().note(text)` — one plain line, kept in memory (the oldest dropped past 4,096); it performs no I/O.

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

Live, a cold start shows your handlers the whole history it reads — every instrument's bars merged in
time order, as a replay feeds them — and sends only what they decide on the newest bar of each
instrument; everything before it is warm-up, discarded and counted (see "Running a strategy live").
So write handlers that can state, on any single bar, what should be held — a level — rather than ones
that act only on the bar where something crossed. Beyond that, state your requirement and honour it:

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
than a value**: `math::observed()` is false for the warm-up (`missing()`) *and* for a window with no
dispersion (`undefined()`), where a z-score or a correlation genuinely does not exist.
`math::is_undefined()` separates those two if you care.

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

`ATL_ASSERT_STRATEGY(MyStrategy, strategy::FullContext)` and `(MyStrategy, strategy::MinimalContext)` compile
**both** branches, since the assertion compiles the handler bodies for the context it names: one stub has every
capability present and the other none. A key attached for a broker that accepts none is refused rather than
dropped: dropping it would make a retry look safe on a broker offering no such guarantee.

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
