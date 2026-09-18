# Holdings Core: Cross-Broker Accounts and Positions

## Summary

This branch adds the next broker-neutral vertical slice on top of
`ai/quote-core`: normalized equity holdings for KIS and Toss Securities, plus
Toss account discovery. It continues the concrete-client-first approach so the
shared domain is informed by two real broker APIs before a runtime broker
interface is introduced.

KIS account selection remains explicit per request because KIS does not offer
the same account-discovery operation as Toss. Cash, buying power, settlement,
and net-asset values are intentionally excluded because the two holdings
endpoints do not expose equivalent concepts.

## Changes

### Shared values

- Add `Money`, `HoldingPosition`, `HoldingsTotals`, and `HoldingsSnapshot`.
- Preserve prices, fractional quantities, monetary amounts, and return ratios
  with `Decimal64`; no broker decimal passes through binary floating point.
- Keep aggregate values separated by currency instead of performing implicit
  exchange-rate conversion.
- Add `Country::UNKNOWN` so new broker market-country values can be retained
  without failing the entire response.
- Model sellable quantity as optional because KIS supplies it while Toss does
  not.

### KIS holdings

- Add `KisAccount` with an eight-digit account number and two-digit product
  code, validated before authentication or network I/O.
- Add async and blocking holdings operations using the documented domestic
  balance endpoint.
- Select `TTTC8434R` for live accounts and `VTTC8434R` for demo accounts.
- Request combined KRX/NXT valuation and normalize positions plus KRW purchase,
  market-value, and unrealized-profit totals.
- Convert KIS percentage-form return values into the shared decimal-ratio
  convention used by Toss.
- Follow `tr_cont` responses with both continuation cursors, percent-encode
  cursor values, and guard against empty, repeated, or excessive pagination.

### Toss accounts and holdings

- Add `TossAccount` and async/blocking account discovery through
  `/api/v1/accounts`.
- Preserve raw account-type strings so future enum values remain usable.
- Add async and blocking holdings operations using the numeric account
  sequence in `X-Tossinvest-Account`.
- Normalize Korean and US positions and gross KRW/USD totals independently.
- Accept unknown future market countries as `Country::UNKNOWN` while retaining
  their three-letter currency.
- Reject inconsistent known country/currency pairs, invalid account sequences,
  invalid integer fields, and inconsistent multi-currency summary nullability.
- Preserve Toss request IDs, broker codes, rate-limit classification, and
  retry hints on error responses.

### Tests

- Add Catch2 coverage for shared holdings values and exact fractional
  quantities.
- Verify KIS live/demo request selection, account validation, cursor encoding
  and extraction, response normalization, percentage conversion, and broker
  errors.
- Verify Toss account discovery, unknown account types, mixed-currency and
  empty portfolios, unknown market countries, malformed summaries, structured
  errors, and validation before network I/O.
- Retain the quote-core loopback TLS tests for persistent connection reuse,
  reconnects, retry safety, queue ordering, and response limits.

## Verification

All checks were run inside `algotradelib-ai-holdings-core`.

- Normal Debug build: passed.
- Catch2/CTest normal suite: 18 of 18 passed.
- Examples plus `-Wall -Wextra -Wpedantic -Werror` build: passed.
- Catch2/CTest warnings-as-errors suite: 18 of 18 passed.
- Examples plus AddressSanitizer/UndefinedBehaviorSanitizer build: passed.
- Catch2/CTest sanitizer suite: 18 of 18 passed.
- `git diff --check`: passed.
- Install to a temporary prefix: passed.
- Standalone CMake consumer including aggregate, KIS, and Toss headers:
  configured, built, and ran successfully against the installed package.

The TLS tests bind only an ephemeral IPv4 loopback port and make no public
network or credentialed broker calls.

## Commit Structure

1. `feat(core): add normalized holdings value types`
2. `feat(kis): add paginated domestic holdings retrieval`
3. `feat(toss): add account discovery and normalized holdings`
4. `test(kis): verify holdings continuation cursors`

## Deferred Work

- Cash, buying power, settlement balances, net assets, loan, and margin detail.
- Order placement, cancellation, fills, and idempotency behavior.
- Cross-currency portfolio valuation and exchange-rate handling.
- Runtime-polymorphic broker and account-discovery interfaces; reassess after
  the order vertical slice exposes its constraints.
- Credentialed integration tests against live or demo broker services.

## Review Notes

- The shared return field is a decimal ratio. Toss already returns ratios;
  KIS's percentage value is scaled by two decimal places during normalization.
- Toss after-cost values are deliberately not used because the corresponding
  KIS endpoint exposes gross values. The shared snapshot therefore compares
  like with like.
- KIS credentials remain in `KisConfig`, while the selected trading account is
  an argument to each holdings call. This supports multiple accounts without
  treating account identity as application authentication.
