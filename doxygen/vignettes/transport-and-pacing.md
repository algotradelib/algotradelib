# Transport and request pacing {#transport-and-pacing}

The bottom of the library: one persistent HTTPS connection per client, a bounded queue in front of
it, and an opt-in pacer that decides what goes out and in what order.

## Transport behaviour

- HTTPS connections are persistent per client and requests are serialized on a
  bounded queue.
- DNS, connect, TLS, write, and read operations have deadlines.
- A failed GET may be replayed once after reconnect. POST requests are never
  replayed automatically, and never written on a connection idle longer than
  `HttpClientOptions::unreplayed_idle_limit_` (one second): both KIS hosts close
  an idle connection after five seconds, and an order written into one failed
  as an unknown outcome KIS never received. Such a request goes out
  on a fresh connection, at the cost of a TLS handshake; Kiwoom sends its reads
  as POSTs, so most paced Kiwoom reads pay it.
- Rate-limit refusals surface as `ErrorCategory::RATE_LIMIT` with whatever hints
  the broker published, and requests are paced against the budget described below.
- TLS performs SNI and certificate hostname verification.
- WebSocket sessions accept a target path and handshake headers, serialize
  writes and close, send idle keep-alive pings, and report lifecycle events
  through virtual callbacks. `WssSessionOptions` can enable bounded exponential
  reconnect backoff with jitter; adapters resubscribe from `on_open()`.

## Request pacing

Requests are paced per connection, and the pacing is opt-in: with no budget
configured the transport behaves exactly as an unpaced one.

```cpp
network::HttpClientOptions options;
options.rate_limit_ = network::RateLimitBudget{8, 8, 2};  // burst, per second, reserve (KIS: below)
kis::KisBlockingClient client(tls, std::move(config), options);
```

`burst_` is how many requests may be issued back to back from idle, `per_second_`
is the sustained rate, and `trading_reserve_` is the part of the burst that only
order traffic may spend. The defaults are a starting point, not a safe figure: KIS's
per-key allowance is an account-level portal setting its documentation does not
state, so the library cannot assert a number. KIS does note that demo accounts are
metered more tightly than live ones, again without stating either figure.

**Measured on one account (2026-10-02):** a KIS demo account
refused reads closer than about a second apart (`EGW00201`), and with real keys the
20-page holiday calendar was refused at eight a second and served at one. The KIS
examples therefore pace at `RateLimitBudget{1, 1, 0}` (`kis_credentials::paced_options`
in `examples/kis_credentials.hpp`). At a burst of 1 the reserve changes nothing, and
a blocking client sends one operation at a time, so an order waits behind whatever
call is running on the same client: at startup, up to the calendar's 20 seconds.

**Leaving `rate_limit_` unset issues requests unpaced**, which is the default. Not
every endpoint is metered, and one that is not should not be slowed down; an
unpaced client also keeps strict arrival order, exactly as it did before pacing
existed.

## Traffic classes

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

## How the two brokers report a refusal

They differ enough that the same handling would be wrong for one of them.

**Toss** answers HTTP 429 and documents a token bucket, publishing
`X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, and
`Retry-After` on the refusal. A stated allowance is adopted directly, because it is
the number the broker is enforcing; `BrokerError::rate_limit_` carries it. The
headers are documented *on the refusal* rather than promised, so an absent one
means waiting rather than guessing.

**KIS** answers with a message code in the body — no 429, no `Retry-After`, no
allowance. On reads it was observed with HTTP 500 (2026-10-02,
`{"rt_cd":"1","msg_cd":"EGW00201",…}`), and the adapter reads the code from a
non-2xx body as well as a 2xx one. **On an order answered with a non-2xx status
the code is kept but not classified:** an HTTP 5xx on an order stays indeterminate, held until the order
list resolves it, because a rate limit is retried and a retry of an order KIS had
in fact processed would double it. KIS meters in two independent layers its
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
refusal (except the order refused with a non-2xx status above, which is not
classified and so does neither), and **the configured budget is left exactly as set**. Reducing a stated
budget from a guess would be the same silent downgrade this library refuses
everywhere else; rising `broker_rejections_` is the signal to lower it.

`EGW00317` is deliberately not treated as a rate limit: it reports a scheduled
trading-control window, and waiting it out as congestion would misreport a closed
window.

## Reading the counters

`BrokerSession::rate_limit_stats()` returns per-class `admitted_`, `delayed_`,
`shed_`, and accumulated `waited_`, plus the allowance currently in force and the
number of broker refusals seen. Rising `delayed_` on a polling class while
`TRADING` stays undelayed is the reserve working; rising `shed_` means the queue is
full rather than the pacer slow. Read them from a completion handler, or between
calls on a blocking session.

`example-rate-limit-stats [burst] [per-second] [reserve]` prints all of this for a
budget offline, with no credentials and no network access.

## Where to go next

[Binding a broker](binding-a-broker.md) sets the budget for a live session, and
[running a strategy live](running-a-strategy-live.md) explains why a polling cadence and a
cancellation compete for the same allowance.
