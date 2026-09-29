# The broker port {#broker-port}

Strategy-side code should not name a broker. `BrokerSession<Client>` binds one client and one account
and exposes one verb set that mentions no vendor type, so selecting a broker is choosing a type
parameter.

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
entirely within itself. Two checks tell an adapter author their traits are
complete, and they answer different questions:

- `ATL_ASSERT_BROKER(Client)` runs whenever a session is built. It names a missing
  member in its own words: a mandatory type or thunk, any capability constant left
  unanswered, and any capability claimed with nothing behind it. A blocking facade's
  thunks are checked in the shape its session calls them; an asynchronous client's
  **by name only**, because the handler a session passes is built around the
  caller's, and no stand-in can model every caller's executor or a move-only one.
- `ATL_ASSERT_BROKER_VERBS(Client)` calls every verb the capabilities claim,
  through the real session, with a plain, a move-only and a `use_future` token, and
  fails the build if one does not compile. Assert it once in any source file of the
  adapter's own — a test is the natural place — or a wrong-signature thunk for a
  capability nobody calls yet is found by the first caller instead.

## Order identity

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

## Capabilities

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

## Example

`example-portable-holdings` is one source file compiled for both brokers:

```bash
example-portable-holdings kis
example-portable-holdings tossinvest [account-sequence]
```

It prints normalized holdings and working orders through the port only, reads
credentials from the same environment variables as the other examples, and places
no order. Without a broker argument it prints usage and exits.



## Where to go next

[Binding a broker](binding-a-broker.md) puts a session behind a running strategy and sets the limits
it trades under. [Talking to a broker directly](broker-clients.md) is the layer underneath, for the
verb the port does not carry. The [architecture](architecture.md) page has the dependency argument
for why the port points the way it does.
