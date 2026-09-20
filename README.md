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
