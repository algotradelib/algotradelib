# Transport and request pacing {#transport-and-pacing}

The bottom of the library: one persistent HTTPS connection per client, a bounded queue in front of
it, and an opt-in pacer that decides what goes out and in what order.

## Transport behaviour

- HTTPS connections are persistent per client and requests are serialized on a
  bounded queue.
- In the asynchronous client the broker facades use (`AsyncHttpClient`), DNS,
  connect, TLS, write, and read operations have deadlines. The blocking
  `network::HttpClient` enforces none.
- A failed GET may be replayed once after reconnect. POST requests are never
  replayed automatically. Before a POST is admitted, a connection idle longer than
  `HttpClientOptions::unreplayed_idle_limit_` (one second) is replaced by a fresh
  one, once per request. If the request then waits for rate-limit admission longer
  than that limit, it is still written on the fresh connection. The refresh costs a
  TLS handshake; Kiwoom sends its reads as POSTs, so most paced Kiwoom reads pay it.
- An order (a request whose `HttpRequest::group_` is `RateLimitGroup::TRADING`, which every broker client sets on placements, amendments and cancellations) is written only on a connection no request has used: when it is admitted, a used connection is replaced, and so, once, is an unused one older than `unreplayed_idle_limit_`.
  Each order costs a TLS handshake (about 20 to 30 ms to the KIS and Kiwoom hosts).
  A server can close a connection that has carried requests well inside its advertised idle timeout: in October 2026 KIS closed one 0.66 s and another 3 s after an order's reply, and an order written into such a connection is an unknown outcome the broker never received.
  An unused connection is not guaranteed to be open either; it is only far less likely to be closed.
  An adapter of your own must set `TRADING` on its order requests to get this, with or without a rate-limit budget.
- Rate-limit refusals surface as `ErrorCategory::RATE_LIMIT` with whatever hints
  the broker published, and requests are paced against the budget described below.
- TLS sends SNI and installs a host-name check. For the peer to be verified, the
  `ssl::context` you pass must enable `ssl::verify_peer` and hold trusted roots
  (the broker-client examples do).
- WebSocket sessions accept a target path and handshake headers, serialize
  writes and close, and report lifecycle events
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

**Kiwoom, measured on a demo account (2026-10-07):** a second request to the same API within a few
milliseconds of the first is refused (`1700`, with the API's id); 300 ms apart it was served, and
different APIs back to back were too. A quote and a price-limit read are the same API (`ka10007`).
Kiwoom's specification gives no numeric allowance. The Kiwoom clients of `example-live-program` and
`example-kiwoom-account` pace at the same `RateLimitBudget{1, 1, 0}` (`kiwoom_credentials::paced_options` in
`examples/kiwoom_credentials.hpp`).

**Leaving `rate_limit_` unset issues requests unpaced**, which is the default. Not
every endpoint is metered, and one that is not should not be slowed down; an
unpaced client sends without waiting, but still in priority order (`AUTH`, then
`TRADING`, `ACCOUNT`, `MARKET_DATA`, `REFERENCE`); arrival order holds within a class.

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
`TRADING` stays undelayed is the reserve working; rising `shed_` means the queue was
full, or the budget is too slow for the request timeout. The counters are not synchronized: read them on
the client's strand (a broker's reply to a completion handler with no executor of its own runs
there; a refusal before any request is sent does not) or while no request is in progress, such as
between calls on a blocking session.

`example-rate-limit-stats [burst] [per-second] [reserve]` shows offline how a budget is spent: each
class's burst allowance, what the reserve keeps free, the sustained rate, and how a broker's refusal
changes the budget. It needs no credentials and no network access, and reads no live counters.

## Where to go next

[Binding a broker](binding-a-broker.md) sets the budget for a live session, and
[running a strategy live](running-a-strategy-live.md) explains why a polling cadence and a
cancellation compete for the same allowance.
