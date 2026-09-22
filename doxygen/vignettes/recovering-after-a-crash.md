# Recovering after a crash

This is the operational half of the order journal: what to turn on before you leave a runner
unattended, what the file looks like at four in the afternoon when something has gone wrong, and how
to read the four verdicts without talking yourself into the wrong one.

Read it before the first unattended session, not after the first incident.

## The sentence the whole thing rests on

**An empty indeterminacy ledger does not mean nothing is owed. It means nothing is remembered.**

`runner::IndeterminacyLedger` is what stops a runner sending a second order for an instrument whose
first order might exist. It lives in memory, so a restarted process starts with an empty one — and an
empty one is indistinguishable from a clean slate. Worse, an order that was *in flight* at the moment
of a `SIGKILL` never reaches `runner::outcome_of` at all: no error path runs, so nothing was ever
marked, not even in the ledger that then died.

The journal exists to make that difference visible on disk.

## Turning it on

Three bindings, and the third is the one people forget:

```cpp
runner::FileJournal journal("/var/lib/mybot/orders.jsonl");
if (!journal.open()) {
  return 1;   // a runner that cannot journal should not trade
}

// 1. Recover first, before any runner exists.
auto live_runner = live::bind(session, MyStrategy{}, limits);
const auto recovery = runner::recover_into(journal, live_runner.ledger());

// 2. Journal every send, by wrapping the executor.
auto executor = runner::JournalingExecutor<live::SessionExecutor<Session>, runner::FileJournal>(
  live::SessionExecutor<Session>(session), journal
);

// 3. Let the source sweep the ledger from the listings it already reads.
live::SessionSource<Session, runner::FileJournal> source(session, {samsung()});
source.set_journal(journal);
```

Miss the third and the journal records faithfully and resolves nothing: every recovered instrument
stays withheld forever, and you will conclude the feature is broken when it is merely deaf.

A **backtest journals nothing**, because it wraps nothing. There is no flag to remember to turn off.

## What the file looks like

One JSON object per line, appended, flushed and `fsync`ed before the call returns. `tail` is the
intended interface:

```
$ tail -2 /var/lib/mybot/orders.jsonl
{"id":"run-a-7","action":"place","outcome":"pending","country":"KR","venue":"COMBINED","symbol":"005930","side":"BUY","type":"LIMIT","quantity":"10","price":"70000","at":1774832400000000,"at_text":"2026-03-30T01:00:00Z"}
{"id":"run-a-7","action":"place","outcome":"accepted","country":"KR",...,"order_id":"KRX-4412"}
```

Two lines per instruction, never an edit of one. The first is written **before** the send is
attempted; the second says what happened. A `pending` line with no companion is the signal.

`at` is the record and `at_text` is the caption: the integer is exact on every platform and is what
matching reads, the text is for you.

### Reading it under pressure

| What you see | What it means |
| --- | --- |
| `pending` then `accepted` | the order exists, and `order_id` names it |
| `pending` then `rejected` | the broker refused. The order does not exist. Nothing owed |
| `pending` then `indeterminate` | the request failed in transit. **Unknown.** Still owed |
| `pending` alone, at the end of the file | the process died between the write and the answer |
| a last line with no newline | the process died *during* the write |

The last two are the reason the format is line-oriented. A line and its newline are written in one
call, so the newline is a commit marker: a torn write loses the tail of the buffer, and the missing
newline says so. A file that was rewritten in place could instead be corrupt as a whole.

A torn line is **not** discarded. Where the symbol is still legible it is salvaged into an entry that
nothing can ever resolve — its id is rewritten so it pairs with no terminal line, and its outcome is
forced to `pending` whatever the text said, because `outcome` may itself be the field that was cut
off. Where the symbol is gone, recovery reports `unattributable_` and nothing specific is withheld;
the loop's own startup gate covers that case, since a runner sends nothing at all until it has read
the broker's order list once.

## The four verdicts

After recovery, each owed instruction is matched against the broker's own order list on every listing
the source reads. `runner::match_verdict_name` prints these:

| Verdict | What it means | What happens |
| --- | --- | --- |
| `matched` | exactly one row corresponds | resolved; the instrument trades again |
| `not_placed` | no row, and the window has closed | resolved; the order does not exist |
| `ambiguous` | more than one row could be it | **nothing is resolved**; you decide |
| `undecided` | no row yet, window still open | wait for the next sweep |

**`not_placed` and `undecided` are the pair to get right.** The absence of a row is evidence of
absence only *after* enough time has passed for the broker's listing to reflect a write it accepted.
Read too early, absence means nothing at all. That is what the window is for, and it is three
durations rather than one tolerance because it absorbs three unrelated things:

- `clock_skew_` — the journal's stamp is your clock, `ordered_at_` is the broker's. Default 30s, and
  generous on purpose: too narrow does not fail loudly, it silently reports `not_placed` for an order
  that exists, which is the worst outcome in this design.
- `maximum_request_` — how long the bytes could have been in the air, for an instruction nobody
  answered. Default 60s. Where both lines exist the span is exact and this is unused.
- `listing_delay_` — how long the broker's listing may lag a write it accepted. Default 30s.

**`ambiguous` is a stop, not a retry.** Two rows that could both be your order stay ambiguous however
many times you sweep; no heuristic turns them into one answer. Go and look, then clear the ledger
entry yourself with `ledger().resolve_after_reading_orders(instrument)` — named for what it requires,
because calling it without having read that list defeats the entire mechanism.

## What matching can and cannot do

**It cannot manufacture idempotency.** A journal narrows the window and makes ambiguity
machine-visible. Only a broker can promise that a duplicate is discarded, and neither supported broker
offers that on a recovery path:

- Toss accepts a `clientOrderId` on the order POST and echoes it on the *acknowledgement* — but its
  order **listing** rows carry no such field. The key is an idempotency guarantee on the write path,
  not a handle you can find an order by afterwards.
- KIS has no equivalent outside `gt_uid`, documented corporate-only (`[법인 전용]`).

So placements are matched on attributes — instrument, side, type, quantity, price, and a timestamp
inside the window — on both brokers. That is a heuristic, and `ambiguous` is how it tells you so.

**Cancellations are matched by identity**, because a cancellation names an order that already exists,
so its journal line carries the broker's own `order_id` from the moment it is written. `ambiguous` is
unreachable there.

**Two identical orders in the same second are already prevented**, which is why second-granularity
timestamps (KIS reports `ord_tmd` as `HHMMSS`) are not the problem they look like. Once an instrument
is marked, the ledger withholds *everything* for it, so a second identical order cannot be sent while
the first is unresolved.

## What this does not do yet

- **Compaction or rotation.** The file is append-only and grows. One trading day is small; a year is
  not. Rotate it yourself between sessions, and note that recovery reads what it is given — a journal
  rotated away is a journal that remembers nothing.
- **More than one writer.** One journal, one process. Two runners on one account is a larger problem
  than a journal.
- **Survive a power cut on macOS.** `fsync` there returns once the data reaches the drive, not once
  the drive has committed it. A killed *process* — which is the threat this exists for — is covered on
  every platform.

## Trying it without a broker

`examples/example-order-journal.cpp` runs the whole story with no credentials, no network and no
signal raised: a send that never returns, a file holding one `pending` line, a fresh ledger that would
happily trade until recovery stops it, and two sweeps that turn `undecided` into `matched`.
