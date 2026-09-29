# Changelog

## algotradelib 0.0.0.9000 (development version)

Nothing has been released. `0.0.0.9000` is the development version before the first release, and it
carries no git tag. Until a release exists, this file records what changes for a project that already
builds against an earlier commit.

### Changed: `KIS_DEMO` and `KIWOOM_DEMO` are required

**A program that loaded credentials before may now refuse to start.** `KisConfig::from_env` and
`KiwoomConfig::from_env` used to read an unset `KIS_DEMO` / `KIWOOM_DEMO` — or any value but `1` — as
**live**. They now accept exactly `1` (the 모의투자 / paper-trading endpoints) or `0` (live) and throw
`std::runtime_error` naming the variable otherwise. `from_json` likewise requires `"demo": true` or
`"demo": false`; a missing key, a string or a number is refused. A refused load, from either source
and for any reason, assigns nothing: a config already loaded keeps its endpoint.

What to do: add `export KIS_DEMO=0` (or `1`) beside `KIS_APP_KEY`, the same for Kiwoom, and a `"demo"`
key to each credential file. A config constructed in code keeps `is_demo_ = false` and is unaffected.
Toss has no paper-trading environment and no such variable.

### Changed: the live loop reads the account before anything that can send

`PollingLoop` now polls, each iteration, calendar, price limits, **working orders, holdings**, buying
power, **then bars and quotes** (it was bars and quotes first, holdings before working orders). A fill
landing between the holdings read and the listing read used to vanish from both, and a target strategy
bought again on the next bar; the new order makes that fill over-count instead, which is harmless.

- **A plan sized against holdings older than the listing is withheld**, as the new
  `WithheldPlanReason::PORTFOLIO_SUPERSEDED` — **a source break for an exhaustive `switch`** over that
  enum. `Portfolio` gains `working_as_of_` and `listing_newer_than_holdings()`.
- **A withheld `TargetPosition` is retained** — withheld as superseded, absent or stale — and planned
  again after an iteration's polls once the account can be trusted and nothing holds
  (`Runner::retry_retained_targets`, `targets_retained`, `clear_retained_targets`), last in an
  iteration, after `on_phase`. A later target or order for the instrument replaces it; a stop discards
  it; nothing stated under a stop, an
  operator's halt or a local failure is retained; a stop or an abandoned run discards them. An
  `OrderIntent` is dropped. **Before, a target withheld as absent or stale was simply dropped**; now a
  one-shot target (from `on_order_update` or `on_phase`) stated during a holdings outage executes when
  the outage ends. A program driving a `Runner` without `PollingLoop` must call the retry itself.
- **`on_order_update` targets are planned after holdings**, live: the handler still runs during the
  listing poll, but its target is withheld and replanned against the holdings read that follows.
- A closing pass is unverified when the holdings are older than the listing.
- The sellable-convention probe skips a listing newer than the holdings beside it.
- A working-order cadence shorter than the holdings cadence gets a once-per-run note: plans between a
  listing and the next holdings read are withheld.
- The loop re-evaluates its gate before each poll, so a cold start's first bars, read after this
  iteration's holdings, are no longer withheld for an account not yet read.
- A bar placed after the listing read leaves a stop in the same iteration unverified: the listing
  genuinely lacks that order.

### Added

- `<algotradelib/version>`, included by `<algotradelib/core>`: `ATL_VERSION_MAJOR`, `_MINOR`, `_PATCH`,
  `_DEV`, `ATL_VERSION`, `ATL_VERSION_ENCODE(major, minor, patch)` and `ATL_VERSION_STRING`. The header
  is the single source; CMake reads the package version from it.

### Changed

- The OpenSSL floor is **3.0**, down from 3.6.2, which no decision had chosen. It is the API minimum:
  upstream support for 3.0 ended in September 2026, so run live on a supported line (3.5 LTS or newer)
  or a distribution package that still receives fixes. Only 3.6.x is tested.
- The documentation states what was already true: order routing across both Korean books (SOR) is
  opt-in at KIS; a Kiwoom order must name its venue; the KIS client supports own-account (`custtype P`)
  customers and not the partner program.

## Changes merged before this file existed

Recorded as the merge reports state them, newest first. Earlier history is not reconstructed.

### `73e857c` → `ab4706f` (stop-limit orders, journal checksums, replay limit bounds)

Source breaks:

- `OrderRequest::trigger_price_` and `OrderSnapshot::trigger_price_` are inserted right after `price_`;
  so are `strategy::PlannedOrder::trigger_price_` and `runner::JournalEntry::trigger_price_`, and
  `kis::KisAmendment::trigger_price_` before `all_remaining_`. Positional aggregate initializers that
  continue past `price_` shift or stop compiling; add the slot (usually `std::nullopt`) or name fields.
- Appended members change exact layouts (structured bindings): `KisOpenOrder::stop_released_`,
  `FillDecision::{released_, intrabar_release_fill_, bounded_}`, `QueuedOrder::released_`,
  `ReplayResult::{stop_releases_, stop_fills_intrabar_, limit_fills_bounded_}` and the matching three
  `ReportCaveats` counters.
- `NextBarFillModel::decide` gains a sixth, defaulted `bool released`; a stored pointer-to-member type
  must change.
- `strategy::RejectReason::{TRIGGER_OFF_GRID, STOP_ORDER_RESTING}` are new; exhaustive switches need
  cases.

Added: `OrderShape::trigger_price_` (a defaulted fifth constructor argument) and
`BrokerCaps::has_trigger_price`, which defaults to `false` for an out-of-tree adapter, so such an adapter
refuses a trigger rather than dropping it.

Behaviour:

- KIS (`22`) and Kiwoom (`28`) place, amend and read back stop-limit orders; Toss, and any adapter
  without the capability, refuses a trigger.
- `OrderIntent::stop_limit` exists. A target that needs a trade withdraws a resting stop first
  (`STOP_ORDER_RESTING`) and the runner retains the target until the stop's own history shows it ended
  unfilled; a restart loses that retained target.
- A replay models a day/regular stop-limit instead of rejecting it.
- **Every replay limit fill** is credited a price its bar traded and never through its limit: a
  marketable `AT_LIMIT` limit now gets the bar's worst price. Backtest results can change with no
  configuration change; `ReportCaveats::limit_fills_bounded_` counts the moved fills.
- Journal lines carry a CRC-32, and a damaged, hand-edited or unknown-key line halts recovery.
  **Rolling back or forward across this change on a live journal is unsupported**: let every
  instruction resolve, then start the other version on a new journal file.

### `f664e28` → `73e857c` (capability conformance, damaged dates, halting replays)

- `Instrument::listed_on_` / `delisted_on_` are `StatedDate`, not `std::optional<Date>`; read `state()`
  first, or ask `dates_exclude(day)`.
- `ATL_ASSERT_BROKER` requires every capability constant `BrokerCaps` reads plus `supports_interval`,
  and a thunk behind every claimed capability; an out-of-tree `BrokerTraits` may fail to compile until
  it names them. `ATL_ASSERT_BROKER_VERBS` is new and opt-in.
- Trailing members on `TradingCalendar` (`unreadable_days_`), `ReplayResult` and `ReportCaveats`
  (`unvalued_bars_`, `first_unvalued_`).
- Behaviour: Toss returns an instrument with an unreadable date instead of failing; KIS and Toss drop
  and count an unreadable calendar row; a backtest **halts** on a mark that will not fit
  (`mark_not_representable`); a fill at an `OPEN`-stamped bar's open is costed on the bar's own day
  under a dated commission schedule.

### Earlier, by branch

- `venue-aware-costs`: `ReportCaveats::commission_rate_`, `commission_minimum_` and
  `commission_from_broker_` are removed for `applied_rates_`; `Report::commission_paid_` and
  `slippage_paid_` are `std::optional<Decimal64>`; `NextBarFillModel::decide` takes a `Date`.
- `damaged-rate-bounds`: `applies_on` and `rate_for` return types that do not convert to `bool`; use
  `== RateApplicability::YES` and `.rate_`.
- `order-type-breadth`: `TimeInForce::AT_THE_CLOSE` / `AT_THE_OPEN` are removed; use
  `OrderSession::CLOSING_AUCTION` / `OPENING_AUCTION` in `OrderRequest::session_`. New `OrderType`
  enumerators and `OrderSession` break exhaustive switches under `-Werror=switch`.
