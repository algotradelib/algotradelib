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
- **Toss supports idempotency; KIS does not.** Setting
  `OrderRequest::client_order_id_` makes a Toss resubmission safe for ten
  minutes. KIS offers no such key for individual customers, so a KIS
  resubmission always risks a duplicate order. The library never generates a
  key on its own, so its absence stays a visible caller decision.
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
requests use Toss's `ORDER_HISTORY` rate-limit group. It uses the same
environment variables as the Toss account example and accepts an account
sequence as its first argument.

```sh
cmake --build build --target example-tossinvest-orders
./build/bin/example-tossinvest-orders
./build/bin/example-tossinvest-orders 123456
```

### Order request dry-run

[`examples/example-kis-place-order.cpp`](examples/example-kis-place-order.cpp)
only constructs and prints an `OrderRequest`. It never creates a client, loads
credentials, or performs network I/O, so no environment setting can make it
submit an order. With no arguments it shows a one-share limit buy of `005930`
at KRW 70,000. Optional positional arguments are `symbol`, `quantity`, side,
type, and limit price, in that order. Side accepts `BUY` or `SELL`; type accepts
`LIMIT` or `MARKET`. A market dry-run takes no price.

```sh
cmake --build build --target example-kis-place-order

./build/bin/example-kis-place-order

# Shows a different dry-run request; still sends nothing.
./build/bin/example-kis-place-order 000660 2 SELL MARKET

# Shows a two-share limit buy at KRW 180,000.
./build/bin/example-kis-place-order 005380 2 BUY LIMIT 180000
```

Toss order placement follows the same shape through
`TossBlockingClient::place_order`, with `OrderRequest::client_order_id_`
carrying the idempotency key.

### Not supported yet

Toss amount-based and fractional orders, Toss conditional orders, KIS
conditional/IOC/FOK/stop/after-market order divisions, KIS credit, reserved,
and overseas orders, KIS history older than three months, and real-time
execution notifications.

## Transport behavior

- HTTPS connections are persistent per client and requests are serialized on a
  bounded queue.
- DNS, connect, TLS, write, and read operations have deadlines.
- A failed GET may be replayed once after reconnect. POST requests are never
  replayed automatically.
- HTTP 429 responses surface as `ErrorCategory::RATE_LIMIT`; a numeric
  `Retry-After` header is preserved in `BrokerError::retry_after_`. The library
  does not yet expose per-group remaining/reset counters or schedule requests
  against a shared rate budget.
- TLS performs SNI and certificate hostname verification.
- WebSocket sessions accept a target path and handshake headers, serialize
  writes and close, send idle keep-alive pings, and report lifecycle events
  through virtual callbacks. `WssSessionOptions` can enable bounded exponential
  reconnect backoff with jitter; adapters resubscribe from `on_open()`.
