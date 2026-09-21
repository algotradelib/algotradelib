# Writing a strategy {#writing-a-strategy}

A strategy is a plain struct with template member functions. It receives events and emits intents.
It performs no I/O, holds no session, names no broker, and therefore runs unchanged in a dry run, in
a live session against either broker, and later in a backtest.

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
| `on_fill(Ctx&, const Fill&)` | one execution happened against an order of yours |
| `on_order_update(Ctx&, const OrderSnapshot&)` | an order changed state |
| `on_phase(Ctx&, MarketPhase)` | the session moved between phases |
| `on_stop(Ctx&)` | after the last event |

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
- clamping into today's permitted price band;
- diffing a target against the portfolio and the working orders;
- every risk limit, and the kill switch;
- the order-safety rules — in particular that a request failing in transit leaves the outcome
  **unknown**, so nothing further is sent for that instrument until the broker's own order list has
  been read. A strategy never needs to know this happened; it simply finds its target unmet and asks
  again, and the runner keeps withholding until the question is settled.

See @ref binding-a-broker for running one.
