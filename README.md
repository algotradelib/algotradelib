# algotradelib

C++17 building blocks for event-driven trading through Korean broker APIs.

## Quote clients

KIS and Toss expose the same broker-neutral quote result while retaining concrete
client types. The first supported instrument scope is Korean equities.

```cpp
#include <algotradelib/kis>

namespace net = boost::asio;
namespace ssl = net::ssl;

net::io_context ioc;
ssl::context tls(ssl::context::tls_client);
tls.set_default_verify_paths();
tls.set_verify_mode(ssl::verify_peer);

algotradelib::kis::KisConfig config;
config.from_env();
auto client = std::make_shared<algotradelib::kis::KisClient>(
  ioc.get_executor(), tls, config);

client->async_quote(
  {algotradelib::Country::KR, algotradelib::Venue::COMBINED, "005930"},
  [](algotradelib::Result<algotradelib::QuoteSnapshot> result) {
    if (result) {
      std::cout << result.value().last_price_.str() << '\n';
    } else {
      std::cerr << result.error().message_ << '\n';
    }
  });
ioc.run();
```

[`examples/example-kis-quote.cpp`](examples/example-kis-quote.cpp) is the
complete runnable version. It reads `KIS_APP_KEY`, `KIS_APP_SECRET`, and the
optional `KIS_DEMO` flag from the environment. The default symbol is Samsung
Electronics (`005930`); pass another Korean equity symbol as the first argument.

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON
cmake --build build --target example-kis-quote
./build/bin/example-kis-quote 005930
```

[`examples/example-tossinvest-quote.cpp`](examples/example-tossinvest-quote.cpp) provides the same workflow for Toss Securities. It reads `TOSS_CLIENT_ID` and `TOSS_CLIENT_SECRET` from the environment.

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON
cmake --build build --target example-tossinvest-quote
./build/bin/example-tossinvest-quote 005930
```

`KisBlockingClient` and `TossBlockingClient` provide blocking convenience APIs.
They drive the same async implementation on the caller's thread and retain the
HTTP connection between sequential calls.

## Account and holdings clients

[`examples/example-kis-account.cpp`](examples/example-kis-account.cpp) loads
KIS credentials from `KIS_APP_KEY` and `KIS_APP_SECRET`, selects the live or
demo environment with optional `KIS_DEMO=1`, and selects an account using
`KIS_CANO` plus optional `KIS_ACNT_PRDT_CD` (default `01`). It requests and
prints normalized positions and KRW totals without displaying the account
number.

```sh
cmake --build build --target example-kis-account
./build/bin/example-kis-account
```

[`examples/example-tossinvest-account.cpp`](examples/example-tossinvest-account.cpp)
loads `TOSS_CLIENT_ID` and `TOSS_CLIENT_SECRET`, discovers brokerage accounts,
prints masked account numbers, and requests holdings for the first account. To
select another discovered account, pass its displayed sequence as the first
argument.

```sh
cmake --build build --target example-tossinvest-account
./build/bin/example-tossinvest-account
./build/bin/example-tossinvest-account 123456
```

## Order clients

Both brokers expose order placement, modification, cancellation, and order
inquiry through matching async and blocking entry points.

### Ordering safety

Read these before using the order clients.

- **An order POST is never replayed.** A `TRANSPORT` error therefore leaves the
  outcome *indeterminate*: the order may or may not have reached the broker.
  Reconcile against an order inquiry before resubmitting; do not retry blindly.
- **Toss supports idempotency; KIS does not, and KIS says so.** Setting
  `OrderRequest::client_order_id_` makes a Toss resubmission safe for ten
  minutes. KIS offers no such key for individual customers — `gt_uid` is
  documented `[법인 전용]` — so **KIS refuses an order carrying one** with
  `ErrorCategory::INVALID_ARGUMENT` and `idempotency_key_unsupported`, before
  authentication and before any socket write. Accepting it and dropping it would
  leave a caller believing a resubmission is safe when it would place a second
  live order. The library never generates a key on its own either, so its absence
  stays a visible caller decision.
- **Acceptance is not execution.** An `OrderAck` means the broker recorded the
  instruction. Read the resulting state back through an order inquiry.
- **Unsupported instructions are rejected, never downgraded.** An at-the-close
  or at-the-open condition the broker cannot honour for that instrument fails
  with `ErrorCategory::INVALID_ARGUMENT` rather than silently becoming a
  day order, because a downgrade changes when the order executes.
- KIS's modifiable-order inquiry is unavailable on demo accounts and fails
  immediately there.
- KIS's daily inquiry reports no status column, so an order's status is derived
  from its quantity, fill, remainder, rejection, and cancellation columns.

### Order inquiries

[`examples/example-kis-orders.cpp`](examples/example-kis-orders.cpp) is
read-only. It lists every modifiable or cancellable order with its amendable
quantity and branch code, then prints today's orders and executions. It uses
the same environment variables as the KIS account example.

```sh
cmake --build build --target example-kis-orders
./build/bin/example-kis-orders
```

[`examples/example-tossinvest-orders.cpp`](examples/example-tossinvest-orders.cpp)
is also read-only. It prints each open order on one line, then requests only
the first page of closed orders and reports whether another cursor is
available. It does not automatically exhaust closed-order history because both
order-list requests use Toss's `ORDER_HISTORY` rate-limit group. It uses the
same environment variables as the Toss account example. Pass an account
sequence as the first argument to avoid an additional account-discovery
request; without one, the example selects the first returned account.

```sh
cmake --build build --target example-tossinvest-orders
./build/bin/example-tossinvest-orders
./build/bin/example-tossinvest-orders 123456
```

### Order request dry-run

[`examples/example-kis-place-order.cpp`](examples/example-kis-place-order.cpp)
only constructs and prints an `OrderRequest`. It never creates a client, loads
credentials, reads environment variables, or performs network I/O, so it
cannot submit an order. Every argument is optional and positional:

```text
example-kis-place-order [symbol] [quantity] [BUY|SELL] [LIMIT|MARKET] [limit-price]
```

With no arguments it shows a one-share limit buy of `005930` at KRW 70,000.
The limit price applies only to `LIMIT`; a `MARKET` dry-run must omit it.

```sh
cmake --build build --target example-kis-place-order

./build/bin/example-kis-place-order

# Shows a different dry-run request; still sends nothing.
./build/bin/example-kis-place-order 000660 2 SELL MARKET

# Shows a two-share limit buy at KRW 180,000.
./build/bin/example-kis-place-order 005380 2 BUY LIMIT 180000
```

This executable deliberately demonstrates request construction rather than
broker submission. Applications place orders through
`KisBlockingClient::place_order` or `TossBlockingClient::place_order`; for
Toss, `OrderRequest::client_order_id_` carries the idempotency key, and KIS
refuses an order that sets it.

### Not supported yet

Toss amount-based and fractional orders, Toss conditional orders, KIS
conditional/IOC/FOK/stop/after-market order divisions, KIS credit, reserved,
and overseas orders, KIS history older than three months, and real-time
execution notifications.

## Broker port

Strategy-side code should not name a broker. `BrokerSession<Client>` binds one
client and one account and exposes one verb set that mentions no vendor type, so
selecting a broker is choosing a type parameter.

```cpp
#include <algotradelib/kis>

using namespace algotradelib;

template <typename Client>
void report(BlockingSession<Client>& session) {
  auto holdings = session.holdings();          // no account argument
  auto working = session.working_orders();     // normalized WorkingOrders
  auto placed = session.place(order);          // Result<PlacedOrder<Broker>>
  session.cancel(placed.value().ref_, cancel_all(working.value().orders_.front()));
}

auto client = std::make_shared<kis::KisBlockingClient>(tls, std::move(config));
BlockingSession<kis::KisBlockingClient> session(std::move(client), account);
report(session);
```

`BrokerSession<Client>` is the asynchronous form and takes a completion token on
every verb; `BlockingSession<Client>` is the same verb set over a blocking facade
and returns `Result<T>` directly. Both apply the same normalizers, so they cannot
disagree about what a broker response means.

Binding a broker is a `BrokerTraits<Client>` specialization, which lives in that
broker's own directory. Core headers include no broker header, and adding a
broker edits no shared file — `tests/mock_broker.hpp` adds a third broker
entirely within itself. `ATL_ASSERT_BROKER(Client)` reports a missing trait
member by name.

### Order identity

`OrderRef<Broker>` is an opaque, comparable, hashable reference issued by the
adapter. KIS needs a branch code together with an order number and Toss needs one
string, so a reference carries both parts without a caller parsing either.

The broker is a **template parameter, not a stored field**: it costs nothing at
run time, and handing a KIS reference to a Toss session fails to compile rather
than failing a check. A broker's asynchronous and blocking clients share one tag,
so a reference read through `TossBlockingClient` is usable with `TossClient`. Each
session publishes the types it accepts as `Session::Ref`, `Session::Placed`,
`Session::Working`, and `Session::WorkingSet`.

Build a modification or cancellation with `cancel_all()`, `cancel_quantity()`, or
`replace_with()` on a `WorkingOrder`. They fill in whatever the bound broker
requires: KIS validates a positive whole quantity even for a full cancellation,
while Toss needs only the identifier.

A `WorkingOrder::ref_` is the reference an amendment addresses. Where a broker
also reports which earlier order a correction replaced — KIS's `orgn_odno` — that
is lineage rather than an address and is not normalized; amending it would
address an order that is no longer live. It stays reachable through
`session.client()`.

### Capabilities

`BrokerCaps<Client>` answers, at compile time, whether a broker implements an
operation. Gate broker-specific work with `if constexpr`; an ungated call to an
unsupported verb fails with a `static_assert` naming the verb and the client.
`session.client()` returns the concrete client as the deliberate escape hatch.

| Capability | KIS | Toss |
| --- | --- | --- |
| `has_account_discovery` | no | yes |
| `has_client_order_id` | no | yes |
| `has_working_orders` | yes | yes |
| `has_amendable_quantity` | yes | no |
| `has_order_by_id` | no | yes |
| `has_partial_cancel` | yes | no |
| `history_requires_range` | yes | no |
| `has_history_cursor` | no | yes |
| `has_history_symbol_filter` | no | yes |

A capability is not an availability: KIS implements the amendable-order inquiry
but does not serve it to demo accounts, which stays a run-time
`ErrorCategory::INVALID_ARGUMENT`. Anything the bound broker cannot express —
an idempotency key at KIS, a partial cancellation at Toss, an order-history
filter neither adapter sends — is refused before authentication and before any
socket write rather than dropped silently.

### Example

`example-portable-holdings` is one source file compiled for both brokers:

```bash
example-portable-holdings kis
example-portable-holdings tossinvest [account-sequence]
```

It prints normalized holdings and working orders through the port only, reads
credentials from the same environment variables as the other examples, and places
no order. Without a broker argument it prints usage and exits.


## Market data

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

### Two conventions that prevent a silent lookahead

**`BarSeries::stamp_` says which edge of its periods the timestamps mark.** Toss
stamps a minute bar at its close and a daily bar at local midnight; KIS reports a
period bar by trading date and a minute bar by execution time. The convention travels
in the value because a timestamp alone cannot reveal it, and reading one as the other
is a lookahead. `BarSeries::ascending()` makes the ordering invariant checkable — both
brokers send newest first and the adapter reverses once.

The interval, the stamp convention, and the currency live on the series rather than on
each bar, because all three are constant across one. A `Bar` is therefore 120 bytes of
prices and a timestamp and nothing else — still an exact-decimal value type for the
wire and for accounting, not a layout for modelling. Converting a series into columnar
floating-point storage is `market-analytics`'s job, at one documented boundary.

**`Bar::open_` is optional.** A trading day can produce no execution at all, which
KIS reports with `mod_yn`. An absent open is neither zero nor the previous close.

### The price grid is the library's own

**Neither broker publishes tick size anywhere in its API.** `PriceRule` is therefore
a table this library maintains, carried on `Instrument::price_rule_`. It records its
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

KRX prices funds and notes on a different ladder from cash equities and neither broker
publishes either, so an ETF or ETN arrives with `UNKNOWN` rather than the equity table.
Attaching the equity grid would be worse than attaching none: it would round onto a
ladder the exchange does not use and the order would be rejected, with the library
having confidently produced the price. A default-constructed rule is `UNKNOWN` too, so
a rule that was never set cannot silently approve anything.

### What differs between the brokers

Per the intersection principle, each of these is reachable rather than hidden, and
none is emulated for the broker that lacks it.

| Capability | KIS | Toss |
| --- | --- | --- |
| `supports_interval(ONE_WEEK/MONTH/YEAR)` | yes | no |
| `has_batch_prices` | no | yes, 200 symbols |
| `has_batch_reference_data` | no | yes, 200 symbols |
| `has_calendar_range` | yes, from a base date | no, three days |
| `has_calendar_sessions` | no | yes |

An interval a broker does not publish is **refused**, never produced by aggregating
a shorter one. A batch verb a broker lacks does not compile rather than looping
single requests behind the caller's back, which would wear one signature over
several times the request cost.

KIS's per-bar corporate-action columns, its after-hours depth totals, and Toss's
intraday session windows are not normalized at all — only one broker publishes each,
so they stay on the concrete client, reachable through `session.client()`.

### Two broker-specific behaviours worth knowing

**A KIS minute request does not cross a trading day.** KIS pages minutes by time
within a day; stepping to the previous *trading* day would need the calendar, and
its calendar endpoint is documented "가급적 1일 1회 호출" — once a day, because it touches
the broker's ledger service. Coupling a bar request to that would make it fail for a
reason unrelated to bars. The series stops at the boundary and reports
`next_cursor_` instead.

**KIS's calendar is cached per client** for the same reason, and served from the
cache for any date it already covers. Pass `refresh` to fetch again deliberately.

### The calendar answers three states, not two

```cpp
auto open = calendar.value().is_trading_day(date);   // std::optional<bool>
```

Empty means the date is **not covered**, which is different from closed. Reporting
an uncovered date as closed would stop a strategy on a day that is in fact open;
reporting it as open would let it place orders into a holiday.

### Example

`example-portable-market-data` is one source file compiled for both brokers:

```bash
example-portable-market-data kis 005930
example-portable-market-data tossinvest 005930 [account-sequence]
```

Its `report()` names no vendor type; every difference above appears as an
`if constexpr` on a capability. It reads market data only and can place no order.

## Request pacing

Requests are paced per connection, and the pacing is opt-in: with no budget
configured the transport behaves exactly as an unpaced one.

```cpp
network::HttpClientOptions options;
options.rate_limit_ = network::RateLimitBudget{8, 8, 2};  // burst, per second, reserve
kis::KisBlockingClient client(tls, std::move(config), options);
```

`burst_` is how many requests may be issued back to back from idle, `per_second_`
is the sustained rate, and `trading_reserve_` is the part of the burst that only
order traffic may spend. The defaults are deliberately conservative: KIS's per-key
allowance is an account-level portal setting its documentation does not state, so
the library cannot assert a number. KIS does note that demo accounts are metered
more tightly than live ones, again without stating either figure.

**Leaving `rate_limit_` unset issues requests unpaced**, which is the default. Not
every endpoint is metered, and one that is not should not be slowed down; an
unpaced client also keeps strict arrival order, exactly as it did before pacing
existed.

### Traffic classes

Every request carries a class, assigned by the adapter that builds it rather than
by the caller, so a strategy cannot label its own polling as trading traffic.

| Class | Priority | Spends the reserve | Examples |
| --- | --- | --- | --- |
| `AUTH` | highest | yes | token issue and refresh |
| `TRADING` | | yes | place, modify, cancel |
| `ACCOUNT` | | no | holdings, working orders, order history |
| `MARKET_DATA` | | no | quotes |
| `REFERENCE` | lowest | no | anything unlabelled |

Two consequences are worth knowing:

- The pending queue is priority-ordered, so a cancellation is sent ahead of a
  queued chart backlog rather than behind it. Ordering is stable within a class, so
  a queue carrying one class behaves exactly as a FIFO queue does, and a request
  already on the wire is never reordered around.
- `AUTH` outranks `TRADING` because every request waits behind a token refresh, a
  cancellation included. It cannot flood, because refreshes are coalesced.

KIS's amendable-order inquiry is `ACCOUNT`, not `TRADING`. KIS recommends it before
an amendment but does not require it: a caller tracking its own working orders
already holds the amendable quantity. It outranks polling, and the reserve stays
held for the amendment itself.

### How the two brokers report a refusal

They differ enough that the same handling would be wrong for one of them.

**Toss** answers HTTP 429 and documents a token bucket, publishing
`X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, and
`Retry-After` on the refusal. A stated allowance is adopted directly, because it is
the number the broker is enforcing; `BrokerError::rate_limit_` carries it. The
headers are documented *on the refusal* rather than promised, so an absent one
means waiting rather than guessing.

**KIS** answers HTTP 200 with a message code in the body — no 429, no
`Retry-After`, no allowance — and meters in two independent layers its
documentation insists not be confused:

| Layer | Code | Scope |
| --- | --- | --- |
| 개인 유량 | `EGW00201` | this application key's own per-second allowance |
| 원장 (ledger) | `EGW00215` | one endpoint's cap, shared with every other customer |

Only `EGW00215` appears in the API workbook; `EGW00201` is documented in KIS's
example repository, which is worth consulting wherever the workbook is silent.
`KisRateLimitLayer` exposes the distinction, because it tells a caller whether the
limit reached was its own or one it shares and cannot influence.

Since KIS states no allowance with either, both hold admission and count the
refusal, and **the configured budget is left exactly as set**. Reducing a stated
budget from a guess would be the same silent downgrade this library refuses
everywhere else; rising `broker_rejections_` is the signal to lower it.

`EGW00317` is deliberately not treated as a rate limit: it reports a scheduled
trading-control window, and waiting it out as congestion would misreport a closed
window.

### Reading the counters

`BrokerSession::rate_limit_stats()` returns per-class `admitted_`, `delayed_`,
`shed_`, and accumulated `waited_`, plus the allowance currently in force and the
number of broker refusals seen. Rising `delayed_` on a polling class while
`TRADING` stays undelayed is the reserve working; rising `shed_` means the queue is
full rather than the pacer slow. Read them from a completion handler, or between
calls on a blocking session.

`example-rate-limit-stats [burst] [per-second] [reserve]` prints all of this for a
budget offline, with no credentials and no network access.

## Analytics

The numeric layer a strategy computes on. It is the one part of the library that needs a
linear algebra backend, and it is optional: configure with `-DATL_USE_EIGEN=OFF` and the
quote, order, holdings, and market-data headers all still build. `<algotradelib/analytics>`
then refuses to compile with a message naming the option to set, rather than producing twenty
errors about missing types.

```cpp
#include <algotradelib/analytics>

namespace analytics = algotradelib::analytics;

// The one place a broker's exact decimals become scalars.
const analytics::SeriesFrame frame = analytics::from_bars(bars);

const auto close = frame.close();               // a view, not a copy
const auto average = analytics::sma(close, 20);
const auto bands = analytics::bollinger(close, 20, 2.0);
const auto strength = analytics::rsi(close, 14);

// And the one route back to something a broker will accept.
const auto rule = algotradelib::price_rule_for(instrument.country_, SecurityType::STOCK);
const auto limit = analytics::round_scalar_to_tick(
  bands.upper_[close.size() - 1], rule, OrderSide::SELL
);
```

`example-analytics-offline` runs the whole path with bars built in code — no credentials, no
network, identical output every time.

### Two numeric types, one boundary

`Decimal64` holds what a broker said, exactly, and stays exact through a round trip and a
reconciliation. `math::Scalar` (`double` by default) holds what a computation needs. They meet
only in `analytics/conversion.hpp`, and the outbound direction is narrower than the inbound
one: `to_scalar` is total, while `to_decimal` and `round_scalar_to_tick` return
`std::optional` and refuse a non-finite or unrepresentable value.

`round_scalar_to_tick` is the only sanctioned route from a computed number to an order price,
because a value correct to four decimal places and off the exchange's tick ladder is rejected
just as firmly as a wrong one. An instrument whose ladder the library does not know
(`PriceRule::Kind::UNKNOWN`) gets no answer rather than a guess.

### Conventions that prevent a silent lookahead

- **A result is as long as its input.** Row `i` of the output describes row `i` of the input, so it
  binds to the same index with no shifting. The warm-up region is present and unobserved, not
  absent — a shorter result would hand the caller an off-by-one that shows up only as a backtest
  that performs impossibly well.
- **Two kinds of absence, both quiet `NaN`s.** `math::missing()` means no observation was
  available; `math::undefined()` means the data was there and the statistic does not exist for it
  (a z-score over a flat window, a Sortino ratio with no downside, a beta against a market that did
  not move). `math::observed()` is false for both and every reduction masks both;
  `math::is_undefined()` tells them apart where a caller would act differently. Never zero: a
  return of zero is a claim the price did not move.
- **Every reduction masks them**, so one absent print does not blank a column. A window's divisor is
  therefore the count of values it actually saw.
- **A window is two numbers.** `window` is how many rows are looked at; `min_observed` is how many
  of them must carry a value before anything is reported. `rolling_mean(closes, 20)` demands all
  twenty; `rolling_mean(closes, 20, 15)` reports from fifteen. No sentinel means "all" — the
  default is the overload.

### Conventions that are stated because packages disagree

| Function | What this library implements |
| --- | --- |
| `ema` | `alpha = 2 / (span + 1)`, seeded on the first observed value |
| `rsi`, `atr` | Wilder's `1 / n` smoothing with Wilder's seed |
| `rolling_stdev`, `rolling_cov` | sample divisor `count - 1` |
| `sortino` | squared shortfalls divided by **all** observations, not the losing ones |
| `one_way_turnover` | `0.5 * sum(abs(delta w))`; `two_way_turnover` is exactly twice it |
| `sharpe` | per period; `annualize_ratio` is a separate call scaling by `sqrt(periods)` |

Turnover ships as two named functions rather than one with a flag, because it is usually multiplied
by a cost per unit traded and a silent factor of two there separates a profitable strategy from a
losing one. `TurnoverStart` chooses whether the first period is measured against an all-cash
portfolio (the default — building a position is real trading) or left unobserved (for comparing
strategies whose histories start on different dates).

### Speed, and where it comes from

Trailing statistics mask their input **once per series** rather than once per window, through
`math::observed_parts`: absent entries become zero and a parallel 0/1 mask carries the count. Each
window is then two plain contiguous reductions with no branch and no per-element predicate — about
**7x faster** than the equivalent scalar loop, and identical to the last bit apart from summation
order. Deviations go through the same pair, so a variance is one `squaredNorm()`.

That vocabulary lives in `math/eigen.hpp`, not in the indicators, so no analytics header names a
backend operation and a second backend implements three small functions instead of having its
expression API assumed throughout. `ema` and `cumulative_returns` stay sequential because each
value depends on the previous one.

### Storage, and why it is column-major

| Type | Holds |
| --- | --- |
| `TimeIndex` | the instants rows correspond to, with `asof` that never reaches forward |
| `SeriesFrame` | one instrument, every bar field |
| `Panel` | one field across many instruments, aligned by an explicit policy |
| `RollingWindow` | the trailing `n` observations a live loop feeds |

Time is on the rows and a series is a column, so one series is contiguous and `column()`
returns a `math::SeriesView` referencing the frame's own memory. That is also why the same
`sma` call serves a completed frame and a live rolling window: both are one contiguous
series, which is what keeps a backtest and a live loop from computing different things.

`RollingWindow` expands until it is full and rolls afterwards, in storage taken once — `2n`
rows, compacted at an amortized one row moved per push, with `compactions()` exposing the
bound. There is no unbounded expanding window, because the statistics that would want one —
a cumulative return, an all-time high, the worst drawdown so far — are answered by the
running accumulators in constant space: `RunningSum`, `RunningMoments`, `RunningExtremes`,
`RunningDrawdown`, and `RunningEma`.

`RunningEma` exists for a specific reason. An exponential average depends on all its history,
so recomputing one over a trailing window does not reproduce what a live loop would have
produced. A strategy fitted over frames and traded from a window would be trading a slightly
different indicator, and the difference does not announce itself.

### Threading

Every stateful type here is owned by one caller and is not synchronized, the model the rest of
the library follows. Parallelism belongs across units of work — one instrument, one parameter
set — each owning its own containers. Eigen's own threading is left alone: this library never
defines `EIGEN_DONT_PARALLELIZE`. When the build's flags define `_OPENMP`, the configure step
links an OpenMP runtime so Eigen's threaded matrix products resolve rather than failing at
link time.

### Linear algebra, and what this layer does not compute

Eigen's decompositions are exposed as aliases rather than wrapped: `math::Llt`, `math::Ldlt`,
`math::HouseholderQr`, `math::ColPivQr`, `math::FullPivQr`, `math::FullPivLu`, `math::Svd`,
`math::SelfAdjointEigen`, plus the triangular and self-adjoint view modes. Use them directly, so
one factorization serves many right-hand sides and a right-hand side may be a matrix:

```cpp
const math::ColPivQr qr(design);                       // pivoting, so it knows the rank too
const math::Matrix coefficients = qr.solve(targets);   // targets may be a matrix
```

There is deliberately **no regression and no covariance estimator**. Fitting is that one call, and
a sample covariance over a complete matrix is two lines of Eigen. What such functions would add is
not arithmetic but *choices* — which predictors, over what estimation window, how a target aligns
to lagged features, whether an incomplete row is dropped or imputed, pairwise or listwise deletion
— and those belong to a strategy. See [factor-models.md](../plan/todo/factor-models.md) and
[covariance-estimation.md](../plan/todo/covariance-estimation.md).

The line, which is why `macd` ships and `covariance` does not: **this layer implements what has one
right answer everyone agrees on, and defers what requires choosing.** Getting Wilder's seed or
Sortino's divisor right is work worth doing once so nobody re-derives a convention and gets it
subtly wrong. "Covariance of series with gaps" has no such canonical answer. `rolling_cov`,
`rolling_corr` and `beta` over two aligned series do, so they stay.

See the [analytics conventions](doxygen/vignettes/analytics-conventions.md) vignette for the
numerical reasoning behind the two-pass variance, Welford, Neumaier, and the choice of
decomposition.

## Strategies

A strategy is a plain struct with template member functions. It receives events and emits intents; it
performs no I/O, holds no session, and names no broker, so one source runs unchanged in a dry run, in
a live session against either broker, and later in a backtest.

```cpp
#include <algotradelib/strategy>

struct MyStrategy {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);
  }
};
```

`ATL_ASSERT_STRATEGY(MyStrategy, Ctx)` checks that at least one handler is detected and that every
handler is also detected for two unrelated probe contexts — which is what proves it is a template,
and therefore that it will run under every runner rather than only the one it was written against.

### Three umbrellas, and why they are separate

| Header | What it gives you | Links TLS? |
| --- | --- | --- |
| `<algotradelib/strategy>` | the surface a strategy is written against | no |
| `<algotradelib/vanilla>` | the vanilla strategies | no |
| `<algotradelib/runner>` | the context, the dispatcher, `DryRunner` | **no** |
| `<algotradelib/live>` | `SessionExecutor` over a bound `BrokerSession` | yes |

`<algotradelib/runner>` reaches no broker adapter, no Boost.Asio and no OpenSSL, asserted at compile
time from a translation unit that includes nothing else. A dry-run binary therefore instantiates no
adapter or transport code — verified by symbol inspection of an installed consumer — so there is no
instantiated code path from a dry run to a market. libssl is still on the link line, because the
package exports one interface target for every consumer; making the promise literal would need a
second, transport-free target.

### Emit a target, not an order

Every shipped strategy emits `TargetPosition`, and the reason is concrete. A strategy emitting orders
must remember whether it already entered — and after a restart it does not know, so it either misses
an exit or re-enters a position it holds. It also double-sizes after a partial fill: the signal still
says "hold 100", forty are filled and sixty are working, and sizing against the position alone asks
for sixty more on top.

Stating the position leaves that to reconciliation, which sees the portfolio *and* the working
orders. `BuyAndHold` never trades twice while remembering nothing at all.

### A limit price cannot be off the grid

`strategy::Price` has no public constructor. The only routes in validate against the instrument's
`PriceRule`, and an unknown ladder refuses every price rather than guessing a tick — a guessed tick is
an order the exchange rejects at best. `Price::rounded` moves a price the way that cannot cross your
intent: a buy down, a sell up.

### What the runner does so a strategy need not

Rounding to tick and lot, clamping into today's price band, diffing a target against the portfolio and
working orders, every risk limit and the kill switch, and the order-safety rules. The most important
of those: a request that fails **in transit** leaves the outcome unknown, so the runner marks that
instrument and withholds everything further for it until the broker's own order list has been read.
It is never retried, because a retry is what doubles a position.

`example-strategy-dryrun` runs four strategies over bars built in code — no credentials, no network.
`example-strategy-live` compiles one `trade()` function template for both brokers.

See the [writing a strategy](doxygen/vignettes/writing-a-strategy.md) and
[binding a broker](doxygen/vignettes/binding-a-broker.md) vignettes.

## Transport behavior

- HTTPS connections are persistent per client and requests are serialized on a
  bounded queue.
- DNS, connect, TLS, write, and read operations have deadlines.
- A failed GET may be replayed once after reconnect. POST requests are never
  replayed automatically.
- Rate-limit refusals surface as `ErrorCategory::RATE_LIMIT` with whatever hints
  the broker published, and requests are paced against a budget. See
  [Request pacing](#request-pacing).
- TLS performs SNI and certificate hostname verification.
- WebSocket sessions accept a target path and handshake headers, serialize
  writes and close, send idle keep-alive pings, and report lifecycle events
  through virtual callbacks. `WssSessionOptions` can enable bounded exponential
  reconnect backoff with jitter; adapters resubscribe from `on_open()`.
