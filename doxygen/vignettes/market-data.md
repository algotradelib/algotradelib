# Market data {#market-data}

Bars, the order book, price bounds, recent executions, instrument reference data,
and the trading calendar, all through the broker port so a strategy asks the same
way whichever broker is bound.

```cpp
BarQuery query;
query.instrument_ = InstrumentId{Country::KR, Venue::COMBINED, "005930"};
query.interval_ = BarInterval::ONE_DAY;
query.count_ = 250;            // how many bars you want, not how many a page holds

auto bars = session.bars(query);
auto book = session.depth(query.instrument_);
auto limits = session.price_limits(query.instrument_);
auto reference = session.instrument(query.instrument_);
auto calendar = session.calendar(Country::KR);
```

`count_` is what the caller wants; satisfying it is the port's job. Pages are far
smaller than a useful window — 200 at Toss, 100 for KIS daily bars, 30 or 120 for
KIS minute bars depending on which of its two minute endpoints applies — and paging
is bounded by a page budget and stops on an empty page.

## Two conventions that prevent a silent lookahead

**`BarSeries::stamp_` says which edge of its periods the timestamps mark.** Toss
stamps a minute bar at its close and a daily bar at local midnight; KIS and Kiwoom
stamp a minute bar at the start of its minute and a period bar by its trading date,
so every bar from them is open-stamped. The convention travels
in the value because a timestamp alone cannot reveal it, and reading one as the other
is a lookahead. `BarSeries::ascending()` makes the ordering invariant checkable — every
broker sends newest first and its adapter reverses once.

The interval, the stamp convention, and the currency live on the series rather than on
each bar, because all three are constant across one. A `Bar` is therefore 120 bytes of
prices and a timestamp and nothing else — still an exact-decimal value type for the
wire and for accounting, not a layout for modelling. Converting a series into columnar
floating-point storage is `market-analytics`'s job, at one documented boundary.

**`Bar::open_` is optional.** A trading day can produce no execution at all, which
KIS reports with `mod_yn`. An absent open is neither zero nor the previous close.

**A daily query answers the session still trading.** Every adapter asks for daily bars up to
today, and the broker includes today's row while the market is open: KIS documents `mod_yn` for
"the current business day", and Kiwoom's daily close field is `cur_prc`, the current price. That
row's close is the price so far. A `BarSeries` read directly therefore can end in a bar that is
not finished; `live::SessionSource` holds such a bar back until its period end
(`strategy::bar_period_end`) has passed the instant the loop reported, so a strategy is shown only
finished bars and each exactly once, with its final close. For a daily bar stamped at local midnight
that is the following midnight.
A minute query ends the same way: KIS and Kiwoom answer the minute in progress, stamped with its
start, and it is fed once that minute has ended.
So a live minute read asks for at least two bars: a read of one would answer only the minute in
progress at every poll, and none would be fed.

## The price grid is the library's own

**No adapter builds the grid from the broker.** A broker states at most the tick at
one price (KIS's quote carries `aspr_unit`; Toss's refusal of an off-tick price names
the tick it wanted), not the whole ladder, so `PriceRule` is a table this library
maintains, carried on `Instrument::price_rule_`. It records its
own `source_` so a stale table is visible rather than assumed, and it is data a caller
can replace.

```cpp
const auto& rule = instrument.price_rule_;            // depends on what it is
if (auto limit = rule.round_to_tick(computed_price, OrderSide::BUY)) {
  order.price_ = *limit;
}
```

A buy rounds **down** and a sell rounds **up**, so rounding never improves a price
past the instruction.

`round_to_tick` returns an **optional**, and that is the important part. A rule has
three states:

| `kind_` | Meaning | `round_to_tick` |
| --- | --- | --- |
| `STATED` | the bands describe the market's grid | rounds |
| `UNCONSTRAINED` | the market quotes in free decimals | returns the price unchanged |
| `UNKNOWN` | this library has no table for the instrument | **returns nothing** |

KRX prices funds and notes on a different ladder from cash equities and this library
carries no table for it, so an ETF or ETN arrives with `UNKNOWN` rather than the equity table.
Attaching the equity grid would be worse than attaching none: it would round onto a
ladder the exchange does not use and the order would be rejected, with the library
having confidently produced the price. A default-constructed rule is `UNKNOWN` too, so
a rule that was never set cannot silently approve anything.

## What differs between the brokers

Per the intersection principle, each of these can be tested at compile time, and none
is emulated for a broker that lacks it (the session-hours row only says that Toss
publishes them; see below).

| Capability | KIS | Toss | Kiwoom |
| --- | --- | --- | --- |
| `supports_interval(ONE_WEEK/MONTH/YEAR)` | yes | no | yes |
| `has_batch_prices` | no | yes, 200 symbols | no |
| `has_batch_reference_data` | no | yes, 200 symbols | no |
| `has_calendar_range` | yes, from a base date | no, three days | no calendar at all (`has_calendar` is `false`) |
| `has_calendar_sessions` | no | yes | no |

An interval a broker does not publish is **refused**, never produced by aggregating
a shorter one. A batch verb a broker lacks does not compile rather than looping
single requests behind the caller's back, which would wear one signature over
several times the request cost.

KIS's per-bar corporate-action columns, its after-hours depth totals, and Toss's
intraday session windows are not normalized — only one broker publishes each — and
the library does not expose them yet.

## Two broker-specific behaviours worth knowing

**A KIS minute request does not cross a trading day.** KIS pages minutes by time
within a day; stepping to the previous *trading* day would need the calendar, and
its calendar endpoint is documented "가급적 1일 1회 호출" — once a day, because it touches
the broker's ledger service. Coupling a bar request to that would make it fail for a
reason unrelated to bars. The series stops at the boundary and reports
`next_cursor_` instead.

**KIS's calendar is cached per client** for the same reason, and served from the
cache for any date it already covers. Pass `refresh` to fetch again deliberately.

## The calendar answers three states, not two

```cpp
auto open = calendar.value().is_trading_day(date);   // std::optional<bool>
```

Empty means the date is **not covered**, which is different from closed. Reporting
an uncovered date as closed would stop a strategy on a day that is in fact open;
reporting it as open would let it place orders into a holiday.

## Example

`example-portable-market-data` is one source file compiled for KIS and Toss:

```bash
example-portable-market-data kis 005930
example-portable-market-data tossinvest 005930 [account-sequence]
```

Its `report()` names no vendor type; every difference above appears as an
`if constexpr` on a capability. It reads market data only and can place no order.


## Where to go next

Turning bars into numbers a strategy computes on is the
[analytics layer](analytics-conventions.md), and the one gated route from a computed number back to
an order price is `round_scalar_to_tick`, against the `PriceRule` above.
[Writing a strategy](writing-a-strategy.md) shows what a handler receives.
