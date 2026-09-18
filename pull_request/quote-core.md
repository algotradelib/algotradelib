# Quote Core: Persistent Async Transports and Cross-Broker Quote Clients

## Summary

This branch establishes the first broker-neutral vertical slice around current
quotes. It adds exact core market-data values, one persistent asynchronous
HTTPS transport, concrete KIS and Toss quote clients, and a shared WebSocket
state machine for plain and TLS sessions.

The implementation deliberately does not introduce a virtual broker interface
yet. Implementing the same quote workflow in two brokers first exposed the
stable shared concepts while leaving authentication, headers, endpoints, and
wire envelopes inside each broker adapter.

Compared with the focused KIS HTTP-session alternative, this branch is the
better foundation because it proves shared types and transport behavior across
two brokers, supports completion tokens and associated executors, bounds queues
and response bodies, and has an explicit safe retry policy. KIS balance and
account operations remain suitable follow-up vertical slices.

## Changes

### Core values

- Add `InstrumentId`, `Country`, `Venue`, `Currency`, `Decimal64`,
  `QuoteSnapshot`, `BrokerError`, `ErrorCategory`, and `Result<T>`.
- Parse decimal broker prices without converting through binary floating point.
- Normalize ISO 8601 timestamps with UTC offsets.
- Apply repository naming rules, including `ALL_CAPS` enum values.

### HTTP and authentication

- Keep one HTTP/1.1 TLS connection per `AsyncHttpClient` and serialize queued
  requests because pipelining is not enabled.
- Bound the request queue, response body, DNS resolution, connect, TLS, write,
  and read operations.
- Verify SNI and the peer hostname.
- Retry an idempotent GET at most once after a connection failure; never replay
  POST automatically.
- Preserve completion-token support and dispatch completion through the
  handler's associated executor.
- Coalesce concurrent token refreshes in both broker clients.
- Keep async teardown nonblocking by cancelling and closing the socket directly
  on failure, reconnect, or explicit close.
- Store pending operations by value, avoiding a separate shared allocation for
  every queued request.
- Retain the blocking `HttpClient` as a compatibility facade with bounded,
  best-effort TLS close notification.

### Broker quote clients

- Add concrete asynchronous and blocking KIS and Toss clients with matching
  quote operations.
- Validate currently supported Korean-equity instruments before network I/O.
- Normalize broker-specific success and error envelopes into shared values.
- Parse retry hints and preserve broker codes and request identifiers.
- Return cached authentication strings by value so callers cannot observe a
  reference after its lock is released.

### WebSocket sessions

- Share resolve, upgrade, read, write, close, and reconnect behavior between
  `WsSession` and `WssSession` through one CRTP state machine.
- Support configurable target paths and headers, serialized writes, graceful
  close, keep-alive pings, lifecycle callbacks, and bounded reconnect backoff.
- Use independent per-session jitter with a small `std::minstd_rand` state.

### Build and tests

- Integrate Catch2 v3 with `catch_discover_tests`; use an installed package when
  available and FetchContent as a fallback.
- Add deterministic unit coverage for exact decimals, timestamps, credential
  encoding, validation, completion-token queue errors, and broker payload
  normalization.
- Add a loopback TLS fixture with a test-only localhost certificate to verify
  real persistence and failure behavior without credentials or public network
  access.
- Export Boost through config mode in the installed package so CMake 3.30+
  consumers do not receive the removed-FindBoost policy warning.

## Verification

All checks were run inside `algotradelib-ai-quote-core`.

- Normal configure and build with examples and tests: passed.
- Catch2/CTest normal suite: 10 of 10 passed.
- `-Wall -Wextra -Wpedantic -Werror` build: passed.
- Catch2/CTest warnings-as-errors suite: 10 of 10 passed.
- AddressSanitizer and UndefinedBehaviorSanitizer build: passed.
- Catch2/CTest sanitizer suite: 10 of 10 passed.
- `git diff --check`: passed.
- Install to a temporary prefix: passed.
- Standalone CMake consumer using the installed aggregate, KIS, and Toss
  headers: configured without warnings and built successfully.

The TLS tests bind only an ephemeral IPv4 loopback port. Sandboxed runners must
permit local socket binding.

## Deferred Work

- KIS balance, holdings, pagination, and order workflows.
- A runtime-polymorphic broker interface; reconsider after quote, holdings, and
  order behavior exists in both adapters.
- HTTP connection pooling or HTTP/2; one serialized connection per origin is
  intentional until measurement demonstrates a bottleneck.
- Credentialed live-broker tests in the default suite.

## Review Notes

- The new enum values intentionally use uppercase names before the public types
  are released.
- POST retry behavior is security-sensitive for future order endpoints; the
  loopback test explicitly drops a POST response and verifies one server-side
  request only.
- The local certificate and private key are fixture-only material committed
  under `tests/`; they are not used by production code.
