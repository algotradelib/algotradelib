# Recovering after a crash {#recovering-after-a-crash}

This is the operational half of the order journal: what to turn on before you leave a runner
unattended, what the file looks like at four in the afternoon when something has gone wrong, and how
to read the five verdicts without talking yourself into the wrong one.

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

Four steps, and two of them are easy to get subtly and silently wrong:

```cpp
// 1. Name the account. It goes in a header written before any order, so a journal whose
//    file belongs to another account refuses to open even if its first record tore.
runner::FileJournal journal("/var/lib/mybot/orders.jsonl", "kis:live:12345678-01");
if (!journal.open()) {
  std::cerr << journal.rejection() << '\n';
  return 1;   // a runner that cannot journal should not trade
}

// 2. Bind through the journal. `bind_journaled`, never `bind`.
auto live_runner = live::bind_journaled(session, journal, MyStrategy{}, limits);

// 3. Recover before the loop starts.
const auto recovery = runner::recover_runner(live_runner, journal);

// 4. Let the source sweep the ledger from the listings it reads.
live::SessionSource<Session, runner::FileJournal> source(session, {samsung()});
source.set_journal(journal);
```

**Step 2 is the one that fails silently.** `live::bind` constructs its own `SessionExecutor`
internally, so wrapping one yourself and leaving it in a variable beside the runner does nothing at
all — the runner keeps sending through the plain executor, the journal stays empty, and nothing
anywhere reports a problem. `bind_journaled` exists so that mistake is unexpressible.

**Step 4 is the one people forget.** Miss it and the journal records faithfully and resolves nothing:
every recovered instrument stays withheld forever, and you will conclude the feature is broken when it
is merely deaf.

**Step 3 does more than mark the ledger.** Three kinds of damage cannot be expressed as a mark on
one instrument, so `recover_runner` halts the runner outright for each — `local_failure()` is set and
the loop gates on it:

- the torn line named no instrument, so there is nothing to withhold;
- a *complete* line does not parse, which means something else edited the file or a repair was
  itself interrupted;
- the file **exists and could not be read**. That is not the same as no journal at all: a missing
  file means a first start and nothing owed, while an unreadable one may hold every order a
  predecessor left in flight. Only `ENOENT` counts as a clean start.

Clear any of them with `resume_after_local_failure()` once you have read the broker's list yourself.
That call also acknowledges the executor's failure count, which is what lets the runner tell a new
storage failure from the one you just fixed — without it, the next send would re-read the first
error and halt again immediately.

A **backtest journals nothing**, because it wraps nothing. There is no flag to remember to turn off.

## What the file looks like

One JSON object per line, appended, flushed and `fsync`ed before the call returns. `tail` is the
intended interface:

```
$ tail -2 /var/lib/mybot/orders.jsonl
{"id":"run-a-7","action":"place","outcome":"pending","country":"KR","venue":"COMBINED","symbol":"005930","side":"BUY","type":"LIMIT","quantity":"10","price":"70000","at":1774832400000000,"at_text":"2026-03-30T01:00:00Z","crc":"f9ee1cbf"}
{"id":"run-a-7","action":"place","outcome":"accepted","country":"KR",...,"order_id":"KRX-4412","crc":"…"}
```

Two lines per instruction, never an edit of one. The first is written **before** the send is
attempted; the second says what happened. A `pending` line with no companion is the signal.

`at` is the record and `at_text` is the caption: the integer is exact on every platform and is what
matching reads, the text is for you.

`crc` is always last. It is the CRC-32 of the line **as it would read without it** — the whole JSON
object, closing `}` included — after which that brace is replaced by `,"crc":"…"}`; to check a line by
hand, cut the member and restore the brace. A line whose bytes changed after
they were written — a bad sector, a partial overwrite, an editor — fails it and is **not read at all**:
it counts as an unreadable line, and recovery halts the runner outright. That matters for the damage no
reader of values could see, such as `005930` becoming `005931`, a real instrument. **So do not edit the
file**: a hand-edited line fails its checksum, and so does a key this library never wrote.

A journal this release begins says so in its header (`"integrity":"crc32"`), so every entry in it must
carry a checksum. A journal begun by an earlier release keeps its checksum-less lines readable as they
always were; once a checksummed line appears, a checksum-less one after it is damage. The one line this
cannot protect is the *first* checksummed line of such a file, which could lose its whole `crc` member
and pass as an older release's. It is a checksum against corruption, not a signature against someone
rewriting the file on purpose.

**Rolling back across this release is not supported on a live journal.** An earlier release reads a
checksummed line (it ignores `crc`) and appends checksum-less ones; this release then halts on them, as
it should on a line it cannot vouch for. To roll back, or to roll forward again afterwards, first let
every instruction resolve, then start the other release on a new journal file.

### Reading it under pressure

| What you see | What it means |
| --- | --- |
| `pending` then `accepted` | the order exists, and `order_id` names it |
| `pending` then `rejected` | the instruction was refused — by the broker, by the binding or client before it sent, or by local machinery that failed. The order does not exist. Nothing owed |
| `pending` then `indeterminate` | the request failed in transit. **Unknown.** Still owed |
| `pending` then `resolved` | a sweep read the broker's list and closed it. `order_id` says what it saw |
| `pending` alone, at the end of the file | the process died between the write and the answer |
| a last line with no newline | the process died *during* the write |
| the runner halted on an unreadable line | a line changed after it was written — its checksum or an unknown key says so — or a checksum-less line follows a checksummed one |

The last two are the reason the format is line-oriented. A line and its newline are written in one
call, so the newline is a commit marker: a torn write loses the tail of the buffer, and the missing
newline says so. A file that was rewritten in place could instead be corrupt as a whole.

A torn line is **not** discarded. Where the symbol is still legible it is salvaged into an entry
carrying `requires_operator`, which no sweep may decide — its id is rewritten so it pairs with no
terminal line, its outcome is forced to `pending` whatever the text said, because `outcome` may
itself be the field that was cut off, and the flag stops the matcher running window arithmetic on a
timestamp that was never recorded. (Without the flag that arithmetic resolves it instantly: the
missing timestamp reads as the epoch, so the window is long closed and no real row can fall inside
it, and the first sweep would report `not_placed`.) Where the symbol is gone, `recover_runner` halts
the runner outright, because there is no instrument to withhold and the startup gate is not enough —
that gate lifts after one order-list read, and a read cannot answer a question nobody can pose.

**Opening the journal repairs a torn tail before writing anything.** Appending straight onto a line
with no newline would glue the next record to the fragment; the combined text would then carry a
newline, so the next replay would see one unreadable complete line and lose both. So opening
terminates the fragment and writes the salvaged record beside it — and if that repair itself fails,
`open()` is false.

## The five verdicts

After recovery, each owed instruction is matched against the broker's own order list on every listing
the source reads. `runner::match_verdict_name` prints these:

| Verdict | What it means | What happens |
| --- | --- | --- |
| `matched` | exactly one row corresponds | resolved; the instrument trades again |
| `not_placed` | no row, and the window has closed | resolved; the order does not exist |
| `ambiguous` | more than one row could be it | **nothing is resolved**; you decide |
| `undecided` | no row yet, window still open | wait for the next sweep |
| `needs_operator` | the entry came from a damaged line | **only you can resolve it** |

A resolution is written back to the journal **before** the ledger is cleared — the same discipline as
writing before a send, applied to the other end. Without it the instruction would stay open on disk
and every restart would withhold the instrument again for a question already answered. If that
write-back fails, the hold deliberately stands and the runner halts.

The record's outcome is `resolved`, not `accepted` or `rejected`. Those are answers the *broker*
gave to the instruction; a sweep learns something different — what the listing shows now. On a
cancellation they can be opposites: a matched row means the target order is **still resting**, so
`accepted` would claim a withdrawal that plainly has not happened. What was observed is in
`order_id` (set when a row was found, empty when none was) and spelled out in `detail`.

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

**Which clock says the window has closed.** The runner's decision instant -- the period end of the last
bar fed -- once a bar has been fed. Before that, as at the startup read after a restart, the loop's own
clock (the wall clock for a source driven without a loop), so a restart can resolve `not_placed` at its
first read instead of waiting for a bar. On daily bars the decision instant is the previous session's
end, so after the first bar a placement left in doubt stays `undecided` until the next bar
(`labs/FINDINGS.md` F20).

**`ambiguous` is a stop, not a retry.** Two rows that could both be your order stay ambiguous however
many times you sweep; no heuristic turns them into one answer. Go and look, then clear the ledger entry
yourself with `ledger().resolve_after_reading_orders(instrument)` — named for what it requires, because
calling it without having read that list defeats the entire mechanism. If the order was this run's own,
you must also tell the runner what you found: `Runner::resolve_indeterminate_placement(instrument, side,
order_id)` with the broker's number, or with no number if it was never placed. Otherwise the run keeps
counting it as a reserved row with no reference until holdings show it (a resting order is then counted
beside its own listed row, and a target may withdraw it), and if they never do, it withholds the
instrument again once it is past the confirmation timeout from its send, at the next holdings read
(counted in `orders_unconfirmed_timed_out()`).

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

**A placement is matched against order history, not against the open-orders listing.** This is the
subtlest thing in the whole design. Both brokers' working-order inquiry returns only what is still
*resting*: Toss maps it to its `OPEN` scope, KIS to the modifiable set. A market or aggressive limit
order can fill and leave that listing within seconds — well before the first sweep — so matching
against open orders alone would find nothing, and once the window closed that absence would read as
`not_placed`. The hold would clear for an order that existed and executed, and the runner would place
it again. So any sweep with a placement owed reads the history too, with the calendar range KIS
requires and the cursor paging Toss offers, and deduplicates by order id because a still-open order
appears in both listings.

History is read **only when something is owed**, so an ordinary sweep costs no request of its own.
And a history read that fails concludes nothing at all: absence from a listing that never arrived is
not evidence, so every instruction stays owed.

**A cancellation needs no history.** It names an order id, and whether that order is still resting is
exactly what the open listing answers.

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
- **More than one writer, beyond one file.** A second writer on the same file is refused on POSIX:
  opening takes an exclusive `flock` and holds it while the journal is open, so the same program
  started twice, or a supervisor starting a new copy while the old one still runs, stops at the
  journal with the reason in `rejection()` (`example-live-program` exits 2, nothing bound). The lock
  dies with its process, and belongs to the file, not the path: rotating or deleting the journal while
  a runner still has it open leaves a new file the next process takes. Two runners on one account through *different* journal paths are not caught
  -- that is the account, not the file -- and Windows takes no lock. Keep the journal on a local disk:
  on NFS, Linux turns `flock` into a lock the journal's own reads release. The journal does check the
  *account*: it is opened with a scope such as
  `"kis:live:12345678-01"`, written into a header before the first order, and a file whose header
  names a different account refuses to open — because symbol, side, quantity and price can all agree
  across two accounts trading the same instrument. A non-empty file with no header is refused too:
  its ownership cannot be established, and that is not a question to guess at while holding a file
  that may name another account's live orders.
- **Repairing a repair.** Terminating a torn fragment and recording its stand-in are two writes, so
  a process killed between them leaves the fragment as a complete unreadable line. That is detected
  — any unreadable line halts recovery outright — but it is detected rather than repaired. A sidecar
  or a two-phase protocol would give stronger guarantees.
- **Survive a power cut on macOS.** `fsync` there returns once the data reaches the drive, not once
  the drive has committed it. A killed *process* — which is the threat this exists for — is covered on
  every platform.

## Trying it without a broker

`examples/example-order-journal.cpp` runs the whole story with no credentials, no network and no
signal raised: a send that never returns, a file holding one `pending` line, a fresh ledger that would
happily trade until recovery stops it, and two sweeps that turn `undecided` into `matched`.
