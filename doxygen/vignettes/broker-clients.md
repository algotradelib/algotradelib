# Talking to a broker directly {#broker-clients}

`KisClient` and `TossClient` are the layer underneath the [broker port](the-broker-port.md). Strategy
code should not name them — that is what the port is for — but a program that needs one broker's
particular verb, or that is not running a strategy at all, talks to them.

Every verb exists twice: an asynchronous form taking a completion handler, and a blocking facade
(`KisBlockingClient`, `TossBlockingClient`) that drives the same implementation on the caller's thread
and keeps the HTTPS connection between sequential calls. Both apply the same normalizers, so they
cannot disagree about what a broker response means.

Both brokers expose the same broker-neutral results while retaining concrete client types. The first
supported instrument scope is Korean equities.

## Quote clients

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

[`examples/example-kis-quote.cpp`](../../examples/example-kis-quote.cpp) is the
complete runnable version. It reads `KIS_APP_KEY`, `KIS_APP_SECRET`, and the
optional `KIS_DEMO` flag from the environment. The default symbol is Samsung
Electronics (`005930`); pass another Korean equity symbol as the first argument.

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON
cmake --build build --target example-kis-quote
./build/bin/example-kis-quote 005930
```

[`examples/example-tossinvest-quote.cpp`](../../examples/example-tossinvest-quote.cpp) provides the same workflow for Toss Securities. It reads `TOSS_CLIENT_ID` and `TOSS_CLIENT_SECRET` from the environment.

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON
cmake --build build --target example-tossinvest-quote
./build/bin/example-tossinvest-quote 005930
```

`KisBlockingClient` and `TossBlockingClient` provide blocking convenience APIs.
They drive the same async implementation on the caller's thread and retain the
HTTP connection between sequential calls.

## Account and holdings clients

[`examples/example-kis-account.cpp`](../../examples/example-kis-account.cpp) loads
KIS credentials from `KIS_APP_KEY` and `KIS_APP_SECRET`, selects the live or
demo environment with optional `KIS_DEMO=1`, and selects an account using
`KIS_CANO` plus optional `KIS_ACNT_PRDT_CD` (default `01`). It requests and
prints normalized positions and KRW totals without displaying the account
number.

```sh
cmake --build build --target example-kis-account
./build/bin/example-kis-account
```

[`examples/example-tossinvest-account.cpp`](../../examples/example-tossinvest-account.cpp)
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
- **Unsupported instructions are rejected, never downgraded.** An order shape a
  broker cannot honour fails with `ErrorCategory::INVALID_ARGUMENT` before
  authentication and before any socket write, rather than silently becoming a
  nearby shape, because a downgrade changes when and at what price the order
  executes.
- **An order's shape is three facts, not one.** `OrderType` is the price basis
  (limit, market, best-limit, primary-limit, mid-price, conditional-limit,
  stop-limit, closing-price), `TimeInForce` is the execution condition (day,
  immediate-or-cancel, fill-or-kill), and `OrderSession` is the session
  (regular, opening auction, closing auction, pre-market, after-hours,
  after-hours auction, after-market, GTP). A broker names an order with a single
  code and that code is a product of the three: KIS's `ORD_DVSN` `16` is a
  best-limit order, filled-or-killed, in the regular session. Splitting them is
  what lets a caller ask "is this a best-limit order" without enumerating six
  spellings, and what lets the two brokers report a session in one field when
  KIS spells it inside `ORD_DVSN` and Toss spells it on `timeInForce`.
- **A shape is offered per venue, not per broker.** KRX, NXT and SOR each
  publish their own set of `ORD_DVSN` codes, and they differ: a market order is
  available at KRX and through SOR and is **not** available at NXT. The library
  holds the workbook's table and refuses a combination the requested venue does
  not list, so the refusal arrives before the POST rather than after it — which
  matters because an order POST is never replayed.
- **Routing is `OrderRequest::venue_`, and it is not the instrument's venue.**
  An `InstrumentId` names an instrument and compares its venue, and every limit
  that asks "what have I already got resting in this" compares `InstrumentId`s —
  while a broker reports a holding with no venue at all. An order whose
  *identity* carried the venue it was routed to would stop matching its own
  holding, and the resting quantity would read zero. So routing has its own
  field, an adapter refuses an instrument naming a venue it does not report, and
  `OrderSnapshot::venue_` reports the routing back. Leaving `venue_` unspecified
  sends no venue and takes the broker's documented default; `Venue::COMBINED`
  asks KIS to route the order, which it calls SOR.
- **A demo account trades on KRX.** Both KIS order sheets say so, and a route it
  cannot reach is refused before any request rather than after the POST.
- **A code the library does not recognize is reported rather than discarded.**
  `OrderSnapshot::shape_code_` carries the broker's own code verbatim beside the
  three normalized facts, so an order placed through the broker's own
  application — or under a code added after this release — arrives with
  `UNKNOWN` in the enums and the real code beside them. The field stores up to
  eight characters from `A`-`Z`, `0`-`9` and `_`, which covers every code either
  broker publishes today; a future code outside that alphabet leaves it empty,
  so empty means either "no code" or "a code this field cannot hold".
- **An unreadable code is not read as an absent order.** Crash recovery cannot
  claim such a row *is* the order it recorded sending, because the shape and the
  price are both missing from it — but it does not conclude the order is gone
  either. It reports the instrument as ambiguous and withholds it, because
  concluding absence would send the order a second time beside the one already
  resting.
- KIS's modifiable-order inquiry is unavailable on demo accounts and fails
  immediately there.
- KIS's daily inquiry reports no status column, so an order's status is derived
  from its quantity, fill, remainder, rejection, and cancellation columns.

### Order inquiries

[`examples/example-kis-orders.cpp`](../../examples/example-kis-orders.cpp) is
read-only. It lists every modifiable or cancellable order with its amendable
quantity and branch code, then prints today's orders and executions. It uses
the same environment variables as the KIS account example.

```sh
cmake --build build --target example-kis-orders
./build/bin/example-kis-orders
```

[`examples/example-tossinvest-orders.cpp`](../../examples/example-tossinvest-orders.cpp)
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

[`examples/example-kis-place-order.cpp`](../../examples/example-kis-place-order.cpp)
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

Toss amount-based and fractional order *placement* — an amount-based order is
read back with its amount, and placing one is outstanding. Toss conditional
orders. KIS credit, reserved and overseas orders. KIS stop-limit *placement*:
`ORD_DVSN` `22` is read and normalized, and requires a `CNDT_PRIC` that
`OrderRequest` has nowhere to carry, so it is refused on a request; the trigger
price it needs is the same field Toss's conditional orders need and belongs with
them. KIS's 시간외 단일가 (`07`) likewise: the inquiry publishes it, the order
sheet does not. KIS history older than three months, and real-time execution
notifications.


## Where to go next

The [broker port](the-broker-port.md) is the layer above these clients, and is what strategy-side
code should be written against. [Binding a broker](binding-a-broker.md) is the operator's view of
putting one behind a running strategy.
