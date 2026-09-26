# User guide {#guide}

Eleven pages, in the order they are worth reading. The first four are the path from nothing to a
strategy running against a broker; the next four are the ones that stop a live account losing money
for a reason nobody saw coming; the last three are the machinery underneath, for extending the
library rather than using it.

Nothing here repeats the API reference. Each page exists because the reasoning behind a decision does
not fit in a `\brief`, and because the reasoning is the part that is expensive to rediscover.

## Start here

- \subpage architecture — the layers, the dependency rule, and why a strategy holds no session.
- \subpage writing-a-strategy — the struct you write, its handlers, and why it emits a target rather
  than an order.
- \subpage binding-a-broker — putting a session behind it, setting the limits, and running with no
  broker at all first.
- \subpage market-data — bars and paging, the two timestamp conventions that prevent a silent
  lookahead, the price grid, and the calendar's three answers.

## Before you trust a number, or an account

- \subpage analytics-conventions — two numeric worlds, four conventions, and the smoothing constants
  that packages disagree about.
- \subpage reading-a-backtest — what the fill model assumes, what it does not model at all, and which
  figures to distrust first.
- \subpage running-a-strategy-live — the three stops, the two gates that deliberately refuse to trade,
  and the supervisor grace period that turns the cleanest shutdown into the worst one.
- \subpage recovering-after-a-crash — the journal, the five verdicts, and how to read them at 3am
  without talking yourself into the wrong one.

## Underneath

- \subpage broker-port — `BrokerTraits`, `BrokerCaps`, order identity, and the capability table.
  Read this to add a broker.
- \subpage broker-clients — the concrete KIS and Toss clients, the order-safety rules, and the
  examples.
- \subpage transport-and-pacing — the HTTPS transport, the pacer, the five traffic classes, and how
  differently the two brokers report a refusal.
