# Running a strategy live

This is the operational half of `runner::PollingLoop`: how to start one, how to stop one on purpose,
and which of the ways to stop it will cost you money. The programming half is in the header. Read this
before the first time you leave a strategy running unattended, because two of the things below cannot
be discovered from the API and one of them is a supervisor setting on a machine you may not have
configured yourself.

## What the loop adds over `feed`

`Runner::feed(event)` runs the pipeline once. Everything in this library is built so that call is the
same in a backtest and in production, and it is genuinely all you need for one decision.

What you must not do is write the obvious wrapper:

```cpp
while (true) { runner.feed(next_bar()); }   // don't
```

At that moment you own four things you probably did not mean to: a cadence, a rate-limit interaction, a
reconciliation obligation, and a stop control. The loop exists to own them for you.

```cpp
auto live_runner = live::bind(session, MyStrategy{}, limits);
live::SessionSource<Session> source(session, {samsung()});

runner::PollSchedule schedule;
schedule.bars_ = std::chrono::minutes(1);
schedule.holdings_ = std::chrono::seconds(30);
schedule.working_orders_ = std::chrono::seconds(30);
schedule.tick_ = std::chrono::seconds(1);

runner::PollingLoop loop(live_runner, source, schedule);
loop.set_maximum_portfolio_age(std::chrono::minutes(2));

runner::StopSource stop;
runner::SignalStop::install(stop);

const auto report = loop.run(stop);
```

`tick_` is not a poll. It is how often the loop wakes to check whether anything is due and whether a
stop has been requested, so it is also the floor on how quickly an operator gets a response. A loop
trading daily bars with a one-second tick wakes 86,400 times a day and polls once — which is the point.

## The three stops, and what each leaves you holding

| Intent | Stops sending | Cancels resting orders | Closes positions |
| --- | --- | --- | --- |
| `QUIESCE` | yes | no | no |
| `WITHDRAW` | yes | yes | no |
| `FLATTEN` | yes | yes | yes |

`QUIESCE` is the default and is what both signals mean. That is deliberate: `SIGTERM` arrives for
entirely routine reasons — a deploy, a host reboot, `docker stop` — and flattening on a reboot would
turn routine maintenance into a liquidation.

`SIGHUP` is ignored, so a dropped SSH connection cannot stop a process holding positions.

## How to stop one, from outside

**Signals**, which already reach a detached process every way you would want:

```sh
kill -TERM <pid>                              # anywhere
tmux send-keys -t trading:0.0 C-c             # into a tmux pane, from outside it
```

Both quiesce. Note the tmux form: a `Ctrl+C` typed in some *other* terminal does not reach the pane,
which is the property you wanted when you ran it in tmux in the first place.

**A control file**, which is the only channel that can name an intent other than quiesce:

```sh
echo quiesce  > /var/run/atl/stop
echo withdraw > /var/run/atl/stop
echo flatten  > /var/run/atl/stop
```

The loop reads it once per tick, so the response time is one tick. A word it does not recognize is
**reported, not guessed at** — `report.notes_` will say so — because picking the nearest intent would
mean choosing how much of your position to close on your behalf. Check the notes if a stop seems not to
have taken.

The file is never deleted by the loop, so "did my stop get read?" stays answerable afterwards.

## The hierarchy that actually matters

The fastest stop is the worst one. In increasing order of damage:

1. **Quiesce, finish the iteration, exit.** The request in flight completes and is classified, so
   nothing is left unknown. Costs one iteration plus one request timeout.
2. **Quiesce and withdraw.** Adds a cancellation per resting order, each of which can itself fail
   indeterminately.
3. **Quiesce and flatten.** Adds market orders — a trading decision taken under duress.
4. **Killing the process mid-flight.** *Manufactures* an unknown outcome: an order may have reached the
   broker with the reply lost, and nothing recorded that it might exist.
5. **`SIGKILL`.** Everything in 4, and the indeterminacy ledger dies with the process.

Levels 4 and 5 are the emergency doors. They are not faster in any way that helps you, because what
they save in seconds they cost in having to read the broker's order list by hand before trading can
resume.

## The trap: your supervisor's grace period

**This is the one thing here you cannot find out from the code, and it silently converts level 1 into
level 5.**

`systemd` sends `SIGTERM`, waits `TimeoutStopSec`, then sends `SIGKILL`. `docker stop` does the same
with a ten-second default. If that grace period is shorter than *one loop iteration plus one request
timeout*, then every clean shutdown you ever perform ends with a process killed mid-request — and you
will believe you shut down cleanly, because you asked nicely and it exited.

So make the grace period generous:

```ini
[Service]
TimeoutStopSec=120
```

```sh
docker stop --time 120 trading
```

And size it against your actual HTTP timeout, not against how long a shutdown usually takes.

## The two gates that stop you trading, on purpose

**The order list must be read before anything is sent.** A process that has just started cannot know
whether a *previous* process died mid-POST. An empty indeterminacy ledger does not mean nothing is
owed — it means nothing is remembered. So the loop reads the broker's own order list once before it
will send anything, and retries until it succeeds. While that is pending, `report.final_gate_` is
`order_list_unread`.

This is the cheapest half of reconciliation and it needs no persistence at all, which is why it is
built while the durable ledger is not.

**A stale holdings snapshot blocks sizing.** Set `set_maximum_portfolio_age` and the loop refuses to
send while the snapshot is older than that, reporting `portfolio_stale`. Reconciling a target against
stale positions is how a position gets double-sized by a route that `working_quantity` does not close.
Staleness is measured against the broker's own `as_of_` — when the positions were *true* — not against
when the reply arrived.

Both gates work by holding the guard, which is the library's one primitive for "stop sending without
withdrawing or flattening". **A hold names who took it**, and is released only by them: the loop takes
`portfolio_stale` and `order_list_unread`, you take `operator` when you call `halt()` by hand, and the
runner takes `local_failure` when its journal cannot be written. Trading resumes when the last hold is
released, so the loop opening its gate cannot lift your halt, and your `resume()` cannot lift the
loop's.

**A hold refuses cancellations too, including a stop's own.** `halt()` means "this runner sends
nothing", and a cancellation is a send — an order-writing request that can itself fail
indeterminately. So a `withdraw` or `flatten` arriving while you are halted cancels nothing, reports
`closing_refused_`, and says "the position is still held" in the notes. That is deliberate: a stop
reaches the process from a signal and from a control file, so a cancellation exempt from the halt
would let `echo withdraw > /var/run/atl/stop` send an instruction your own halt forbade. To get out
after halting, `resume()` first — one deliberate act, and `closing_complete()` then tells you whether
every instruction was accepted.

`report.final_gate_` is an optional, and names the most fundamental hold in force rather than only the
loop's own — so a run that sent nothing because *you* halted it says `operator`, instead of showing you
an open gate while nothing goes out. `guard().holds()` lists every hold when more than one stands, and
`guard().hold_detail(HaltReason::LOCAL_FAILURE)` carries the reason the journal gave up.

## Did everything I asked for go out?

Ask, because it is not always yes — and note that this is a narrower question than "am I flat", for
the reasons below:

```cpp
if (!report.closing_complete()) {
  for (const auto& note : report.notes_) { std::cerr << note << '\n'; }
}
```

`withdrawn_` and `flattened_` count what the loop **asked for**. `closing_placed_` counts what actually
**went out**. They differ, and the most common reason is the one below.

`closing_complete()` is false if *any* closing instruction failed to go, and there are six ways that
happens — `closing_rejected_` (reconciliation could not build it, or the broker refused it),
`closing_refused_`, `closing_withheld_`, `closing_indeterminate_`, `closing_unavailable_` (the
journal could not be written, so no broker was asked) and `closing_unresolved_` (a cancellation named
an order the snapshot does not hold). Cancellations count in these as well as placements: a
withdrawal that failed in transit may have withdrawn the order or may not, and that is not the same
as one that went.

**And one flag covers the case before any of those**, `closing_source_unverified_`. A stop builds its
instructions from the holdings snapshot — working orders for a `withdraw`, positions for a `flatten` —
so a stop requested before the loop's first iteration never reaches the startup order-list read and
works from whatever picture it happens to have. That picture will not contain an order a *previous
process* left resting. The pass still sends what it can see, because withdrawing some beats
withdrawing none, but it will not call the result complete. If you can, let the loop run one iteration
before stopping it.

**It is not a confirmation of anything at the exchange.** `true` means every instruction the loop
asked for was *accepted by the broker*, and that is where the library's knowledge stops. An accepted
closing order may not have filled, which only a later holdings snapshot shows. An accepted
cancellation may not have completed: cancellation is asynchronous at both brokers — Toss publishes
`PENDING_CANCEL` while it is in flight and `CANCEL_REJECTED` when it fails, returning the order to its
previous state — so a withdraw that reports complete can still have orders working.

So after any stop, read the broker's own order list and holdings before believing anything about the
state. `closing_complete()` answers one question only: did anything fail to go out.

**`RiskLimits::permitted_phases_` defaults to `{MarketPhase::OPEN}`, so a flatten outside market hours
is refused.** That is correct — there is no session to trade in — but it means an operator who asks to
be flat at 22:00 and hears nothing would believe they are flat while still holding the position. The
report says `closing_refused_` and carries a note ending "the position is still held". This is why the
counts are separate rather than one number.

## Poll failures are not send failures

A poll that fails sent nothing, so nothing needs reconciling and no ledger entry is owed. A quote that
did not arrive is an absence of information, and the loop carries on without it.

What it does bound is repetition: `FailurePolicy::consecutive_limit_` consecutive failures of one kind
and the loop quiesces rather than spinning against a broker that is refusing it, reporting
`final_gate_ == poll_failing` with `stopped_by_` empty — so you can tell the loop's own decision from an
operator's stop. Counted per kind, so a
broken working-order inquiry does not mask a healthy bar feed, and **consecutive** rather than
cumulative, so one failure in three is degraded rather than down.

A rate-limit refusal is specifically **not** a failure. It is the limiter working, and the controller
already holds capacity back from polling so that an order is never the request that waits.

## What this does not do yet

- **Stream.** It polls. `plan/todo/realtime-streaming.md` drops into the same source seam.
- **Run more than one loop.** One loop, one thread. See `plan/todo/parallel-execution.md`.

Surviving a restart is no longer on this list. Bind a `runner::FileJournal` and the ledger becomes
durable: every instruction is recorded before it is sent, a restarted runner recovers what its
predecessor left owed, and the listings this source already reads resolve it. See the
[recovering after a crash](recovering-after-a-crash.md) vignette — including why `not_placed` and
`undecided` are the pair to get right, and why an `ambiguous` verdict is a stop rather than a retry.

## Trying it without a broker

`examples/example-live-loop.cpp` runs the real loop against a source that invents its data — no
credentials, no network, and it finishes instantly because it uses `ManualClock`. It is also the
shortest worked example of writing a source for a broker this library does not support: implement
`poll` and `read_order_list`, and the cadence, the stop intents, the gates and the failure policy all
come along unchanged.
