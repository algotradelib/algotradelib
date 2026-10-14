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
entirely within itself. One check tells an adapter author their traits are
complete, `ATL_ASSERT_BROKER(Client)`; assert it once, in any source file of the
adapter's own (a test is the natural place):

- It calls every verb the capabilities claim, through the real session, with a
  plain, a move-only and a `use_future` token (or directly, for a blocking facade),
  and reads every capability constant. A missing member, a thunk with the wrong
  signature, a thunk that copies its token or an unanswered capability fails the
  build there, with the compiler's own diagnostic naming the traits and the member.
- No capability has a default. A `false` is not always the safe answer: one for
  `has_history_cursor` says the adapter returns the whole history in one answer (a
  page that still offers more is then refused as incomplete), and one for
  `has_working_orders` stops a runner keeping the orders it sent. So an unanswered
  constant does not compile rather than being chosen for the adapter.
  `working_orders_state_terminal` and `sellable_inquiry_basis` are asked only of a
  broker that lists working orders, or makes the sellable inquiry.
- Nothing checks the traits when a session is built. Without the assertion, a verb
  nobody calls yet is compiled by its first caller instead.
- It does not prove that a thunk completes exactly once, on the right executor, or
  that a capability is true of the broker. Those take run-time tests.

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

A capability is not an availability: KIS does not serve its amendable-order
inquiry to demo accounts, so on demo the adapter answers it from the daily order
inquiry instead, a run-time matter. Anything the bound broker cannot express —
an idempotency key at KIS, a partial cancellation at Toss, an order-history
filter neither adapter sends — is refused before authentication and before any
socket write rather than dropped silently.

**`history_requires_range = false` carries a contract of its own**, which an adapter
author writing one must meet. Recovery (`live::SessionSource`) asks the order history
one stated date at a time — `from_ == to_`, one inquiry per UTC day of its window,
paged by cursor as your adapter pages — because Kiwoom's single-date inquiry requires a
date and refuses a range; so accept a single-date query. An adapter that also declares
`has_working_orders = false` is additionally asked at startup with no date at all
(`SessionSource::read_order_list`), following its cursor page by page if it declares one, so
accept that too or the order-list gate stays closed. Recovery merges the two listings and drops a history row only as an **exact
copy** of a working row (`runner::exact_copy`: the same order id, instrument and
`ordered_at_`), because an order number alone is not an order's identity across days.
So **where your history can list an order your working listing also lists, stamp
`ordered_at_` on both rows alike** wherever the broker states the instant: the order is then one row.
Stamped differently, or unstamped on either side, both rows are kept, and if both answer the instruction a
journal is waiting on, it recovers as `AMBIGUOUS` and holds its instrument until an
operator settles it.

**A date the broker did not state is not evidence.** If your working listing gives only a time of day
and you supply the date (the day you read it), set `OrderSnapshot::ordered_date_assumed_` on the row,
also when the time itself could not be read. Such a row neither matches an instruction nor lets one be
found absent; where the history has a row of the same order (`runner::dates_listed_order`: the same order
id and instrument at the same time of day, and nothing either row states disagrees), the listing row is
dropped and the dated history row stands for the order on its own evidence. Left unset, an invented date
is taken as the broker's word, and a previous day's order still listed can name an order of unknown
outcome. Kiwoom's working-order listing (`ka10075`) is such a listing, and its history (`kt00007`) is
dated by the day asked for; Toss reads both of its listings (its `OPEN` and `CLOSED` scopes, which can
both list a partly filled order) with one parser from the order's `orderedAt`.

## Example

`example-portable-holdings` is one source file compiled for both brokers:

```bash
example-portable-holdings kis
example-portable-holdings tossinvest [account-sequence]
```

It prints normalized holdings and working orders through the port only, reads
credentials from the same environment variables as the other examples, and places
no order. Without a broker argument it prints usage and exits.

## A market-data source that cannot trade

A data vendor — EODHD, Twelve Data, Yahoo — can sit behind the port too, so that a
backtest reads its bars through `BlockingSession::bars`, and a `live::SessionSource`
reads bars and quotes from it while orders go to a broker
(`SessionSource<Trading, Journal, MarketData>`). The port has no read-only form,
though: every session calls `holdings`, `place`, `replace`, `cancel` and
`order_history`. Inherit `DataOnlyTraits<Tag>` and declare only what the vendor
serves:

```cpp
template <>
struct algotradelib::BrokerTraits<my::Client> : algotradelib::DataOnlyTraits<my::Tag> {
  using Client = my::Client;
  using Config = my::Config;
  static constexpr std::string_view name = my::Tag::name;
  static constexpr bool blocking = true;
  static constexpr bool supports_interval(BarInterval interval);
  static std::optional<BrokerError> validate_bars(const BarQuery& query);
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument);
  static Result<BarSeries> bars(Client& client, BarQuery query);
};
```

The base declares the account-side types, every capability `false`, and every
account-side member as a refusal **before any I/O**: `INVALID_ARGUMENT` with
`reached_broker_ = false`, which the runner reads as "refused locally, nothing
exists". Writing those refusals by hand is where it goes wrong: a default
`BrokerError{}` has category `PROTOCOL` and `reached_broker_ = true`, which the
runner reads as an order of **unknown** outcome and holds the instrument for a
reconciliation a vendor can never give. No member of the base answers success —
no empty holdings, no empty history — because both would be claims about an
account the vendor does not have.

A vendor that serves more (a calendar, reference data) declares that capability
`true` in its own specialization and adds the thunk; the conformance assertions
check it as they check any adapter.

A member the specialization declares hides every base member of that name,
overloads included; re-expose one with `using DataOnlyTraits<Tag>::holdings;`.

What a data-only session is not: a trading session. The base marks the client
(`BrokerCaps<Client>::data_only`), and `live::SessionExecutor`, which sends orders,
and the first (trading) session of a `live::SessionSource` refuse it with a
`static_assert`, so a program wiring a data vendor where orders go does not
build. It is accepted as the market-data session,
`SessionSource<Trading, Journal, MarketData>`. Called directly, the session still
refuses every order at run time. A data client written by hand without the base
declares `static constexpr bool data_only = true` itself; omitting the flag fails
compilation at either binding, as it does for every traits specialization.

`live_program::trade` and `observe` in `examples/live_program.hpp` read reference
data from the market-data session, so they need a vendor that declares
`has_reference_data`.



## Where to go next

[Binding a broker](binding-a-broker.md) puts a session behind a running strategy and sets the limits
it trades under. [Talking to a broker directly](broker-clients.md) is the layer underneath, for the
verb the port does not carry. The [architecture](architecture.md) page has the dependency argument
for why the port points the way it does.
