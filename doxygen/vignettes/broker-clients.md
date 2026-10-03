# Talking to a broker directly {#broker-clients}

`KisClient`, `KiwoomClient` and `TossClient` are the layer underneath the
[broker port](the-broker-port.md). Strategy
code should not name them — that is what the port is for — but a program that needs one broker's
particular verb, or that is not running a strategy at all, talks to them.

Every verb exists twice: an asynchronous form taking a completion handler, and a blocking facade
(`KisBlockingClient`, `KiwoomBlockingClient`, `TossBlockingClient`) that drives the same implementation on the caller's thread
and keeps the HTTPS connection between sequential calls. Both apply the same normalizers, so they
cannot disagree about what a broker response means.

The brokers expose the same broker-neutral results while retaining concrete client types; this page
covers KIS and Toss in detail, and Kiwoom's own examples are `example-kiwoom-*`. The first
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
complete runnable version. It reads `KIS_APP_KEY`, `KIS_APP_SECRET`, and
`KIS_DEMO` from the environment: unset or `1` is the 모의투자 endpoints, `0` is live,
and anything else refuses to load. The default symbol is Samsung
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
demo environment with `KIS_DEMO` (demo unless it is `0`), and selects an account using
`KIS_CANO` plus optional `KIS_ACNT_PRDT_CD` (default `01`). It requests and
prints normalized positions and KRW totals without displaying the account
number.

```sh
cmake --build build --target example-kis-account
./build/bin/example-kis-account
```

### Credentials from files

An account's keys can also live in one JSON file per account, in a directory you choose and pass
at run time: `--credentials DIR` to `example-live-program kis` and the `example-kis-*` programs that
call KIS (not the offline `example-kis-place-order`), or
`ATL_CREDENTIALS_DIR`, which `example-portable-*` and `example-strategy-live` also read. They then read
`DIR/kis/demo.json`, or `DIR/kis/real.json` when `KIS_DEMO=0`, with `KisConfig::from_json` and
`KisAccount::from_json` (`app_key`, `app_secret`, `account_number`, `account_product_code`,
`demo`). A file whose `"demo"` contradicts its name is refused, and a directory without the file
stops the program rather than falling back to the environment. Keep the files `chmod 600`.

A KIS demo host refuses reference data (`CTPF1002R`) and the holiday calendar (`CTCA0903R`) with
`EGW02006`, "모의투자 TR 이 아닙니다". Both are market data, the same from any account. So
`live::SessionSource` takes a second session for its four market reads (bars, quotes, price limits,
the calendar), and `live_program::trade` and `observe` take one for those and for reference data.
`example-live-program kis` on demo builds one when `DIR/kis/real.json` exists, with an empty
`KisAccount`: the KIS client refuses every account request and order through it before any I/O
(`invalid_account`). `--market-data toss` reads them from Toss instead (`DIR/tossinvest/real.json`,
no account selected), and only on demo. Either way the run knows the market phase, so with
`--live` it trades on demo. The example paces every KIS client at one request a second
(`kis_credentials::paced_options`): faster, KIS refuses reads with `EGW00201`, and the real keys'
holiday calendar takes about 20 s of pages at startup. Toss's daily price band can differ from
KIS's for the same stock (`labs/FINDINGS.md` F27), and the band clamps limit prices, so KIS may
refuse an order priced from it.
**Kiwoom issues one key per account**: a Korean-stock and a US-stock account each have a key, and no
request carries an account number. So the Kiwoom examples (`example-kiwoom-*` and
`example-live-program kiwoom`) read one file per key: `DIR/kiwoom/demo-<label>.json`, or
`real-<label>.json` when `KIWOOM_DEMO=0`, with `KiwoomConfig::from_json` (`appkey`, `secretkey`,
`demo`). `--kiwoom-account LABEL` picks the file (default `kr`; lowercase letters, digits and `-`). As
for KIS, a directory without the file stops the program, a file whose `"demo"` contradicts its name is
refused, and **with a directory the `KIWOOM_*` variables are not read**, so setting
`ATL_CREDENTIALS_DIR` for KIS also moves Kiwoom to its files. Without a directory `--kiwoom-account` is
refused. The adapter calls domestic-stock endpoints only, so a US account's key authenticates but
serves nothing the adapter reads. Toss trading still reads its environment variables, and
`example-live-program` refuses `--credentials` for it.

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
- **Routing across both books is opt-in, and that is deliberate.** At KIS an
  unspecified venue routes to **KRX**, and SOR is a choice the caller makes with
  `Venue::COMBINED`. It is not the default because a KIS demo account refuses
  SOR: a default that fails on demo would make a demo run stop rehearsing the
  live one. Kiwoom documents its `dmst_stex_tp` as required with no default, so
  a Kiwoom order must name a venue and is refused before any request otherwise
  (a strategy runner names it with the binding's `default_route`). Every Kiwoom
  order endpoint documents `SOR` as a venue, and its working-order listing marks
  a SOR order with `sor_yn`, which the adapter reads; whether a Kiwoom demo
  account accepts SOR has not been observed. Toss offers
  no venue choice at all, and an order naming one is refused.
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
- KIS's modifiable-order inquiry is unavailable on demo accounts. There the
  adapter answers it from today's daily order inquiry, unfilled orders only
  (`VTTC0081R`, `CCLD_DVSN=02`): each row with a remaining quantity is listed,
  with that quantity as the amendable one. The listing then lags as the history
  lags; on the demo (2026-10-02) both showed a new order within one paced poll.
  A runner keeps what it sent counted until holdings or the history account for
  it, so the lag no longer places an order twice in one process; across a restart
  it still can (`running-a-strategy-live.md`, "What this does not do yet"). On demo
  the listing also keeps a DAY order that expired at the close, until the date
  changes. A released stop cannot be told apart there, and the rows
  carry an order time the live listing lacks, so recovery on demo takes a
  different matching branch than on live.
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

### Stop-limit orders

A **stop-limit** order is placeable at KIS (`ORD_DVSN` `22`, trigger in
`CNDT_PRIC`) and at Kiwoom (`trde_tp` `28`, trigger in `cond_uv`) through
`OrderRequest::trigger_price_`, which a stop-limit order requires beside its
limit and every other basis refuses. Both adapters read the trigger back onto
`OrderSnapshot::trigger_price_`.

On an amendment, `OrderShape::trigger_price_` follows the same rule as the
condition and the session: **set means the new trigger, unset means keep the
resting one** — but only when both the resting order and the new shape are
stop-limit orders (`amended_trigger_price`). A change of basis into a stop must
state a trigger, a change out of one carries none, and an instruction with no
resting shape inherits nothing. `reprice` and `cancel_all` restate the resting
trigger explicitly.

A broker whose `BrokerCaps::has_trigger_price` is false, Toss included, never
receives a trigger on a placement or a modification: the session refuses it as
`unsupported_trigger_price`. The session never refuses a cancellation for carrying
one, and `cancel_all` restates the resting trigger; Kiwoom alone refuses a
cancellation that states a *different* trigger, because `kt10003` cannot change
one on the way out.

### Not supported yet

Toss amount-based order *placement*, which is also how Toss buys a fraction — an
amount-based order is read back with its amount, and placing one is outstanding.
A fractional quantity is placed only where Toss accepts one: a US market sell, to
six decimal places, during the regular session until an hour before the close. Toss conditional
orders. KIS credit, reserved and overseas orders. KIS's 시간외 단일가 (`07`):
the inquiry publishes it, the order sheet does not. KIS history older than
three months, and real-time execution notifications.


## Where to go next

The [broker port](the-broker-port.md) is the layer above these clients, and is what strategy-side
code should be written against. [Binding a broker](binding-a-broker.md) is the operator's view of
putting one behind a running strategy.
