# Quote Core

Branch: `ai/quote-core`
Worktree: `algotradelib-ai-quote-core`
Status: implementation and verification complete; awaiting review

## Objective

Establish a low-overhead C++17 networking and broker foundation using one
read-only quote workflow implemented by both KIS and Toss Securities. The
shared surface must expose broker-neutral values to future strategy code while
keeping authentication, request headers, wire envelopes, and retry policy in
the transport and broker adapters.

## Implementation Sequence

1. Establish broker-neutral value semantics in `src/core/types.hpp`.
2. Build the bounded persistent HTTPS transport and authentication helpers.
3. Implement one quote operation in both KIS and Toss adapters and normalize
   their responses into the shared values.
4. Consolidate plain and TLS WebSocket lifecycle behavior into one state
   machine while retaining thin transport-specific wrappers.
5. Cover pure value parsing and broker normalization with Catch2.
6. Exercise connection reuse and retry safety against a deterministic local TLS
   server.
7. Harden async teardown and allocation behavior, apply the repository style
   guide, and verify installation from an external consumer.

Each step should remain a logical commit so transport, broker, test, and
documentation changes can be reviewed independently.

## Architecture

1. Core value types
   - Represent instrument identity, venue, currency, exact decimal prices,
     quote timestamps, structured errors, and explicit `Result<T>` values.
   - Support Korean equities first without embedding Korea-only assumptions in
     price, currency, or instrument representation.
   - Avoid floating-point conversion for broker decimal strings.

2. HTTPS transport
   - Maintain one persistent HTTP/1.1 TLS connection per origin.
   - Serialize requests because HTTP pipelining is not used.
   - Bound queued requests and response bodies, and enforce operation deadlines.
   - Verify SNI and certificate hostnames.
   - Reconnect and retry an idempotent GET at most once. Never automatically
     retry POST requests, including authentication and future order submission.
   - Preserve the completion handler's associated executor and support C++17
     Asio completion tokens.
   - Keep synchronous compatibility isolated from the async event-loop path.

3. Broker adapters
   - Provide concrete `KisClient` and `TossClient` classes with matching quote
     operations rather than introducing a speculative virtual broker interface.
   - Coalesce concurrent token refresh requests and normalize broker-specific
     response envelopes into `QuoteSnapshot` and `BrokerError`.
   - Provide optional blocking facades that drive the same async workflow on a
     private `io_context` without a mandatory worker thread.

4. WebSocket transport
   - Share connection state through one implementation for plain and TLS
     WebSockets.
   - Support configurable targets and headers, serialized writes and close,
     keep-alive pings, lifecycle callbacks, and optional reconnect backoff with
     per-session jitter.

## Hardening Before Merge

1. [x] Remove blocking TLS shutdown from `AsyncHttpClient`; failure and reconnect
   paths must never synchronously wait on the event-loop strand.
2. [x] Store queued HTTP operations by value where possible instead of allocating a
   separate `shared_ptr` for every request.
3. [x] Add a local TLS Catch2 fixture that verifies:
   - sequential requests reuse one TCP/TLS connection;
   - a server-requested close reconnects successfully;
   - queued responses preserve request order;
   - GET may be retried once after a dropped connection;
   - POST is never automatically replayed;
   - configured response-body limits surface an error.
4. [x] Apply the repository style guide to new public types and touched networking,
   broker, test, and CMake code.
5. [x] Re-run normal, warnings-as-errors, ASan/UBSan, CTest discovery, installation,
   and an external consumer build.

## Explicitly Deferred

- KIS account balance, holdings, orders, and pagination. These should be added
  as later vertical slices using this transport and normalized domain types.
- Abstract runtime broker interfaces. Reassess after quote, holdings, and order
  behavior exists in both adapters.
- HTTP connection pooling and HTTP/2. Add only after measurement shows one
  serialized connection per origin is insufficient.
- Credentialed live-broker tests in the default test suite.

## Acceptance Criteria

- [x] Both concrete broker clients compile through installed package metadata.
- [x] Catch2 tests are individually discoverable by CTest and require no broker
  credentials or public network access.
- [x] The local TLS fixture demonstrates persistence and retry safety rather than
  relying only on implementation inspection.
- [x] No async networking handler performs a blocking network operation.
- [x] The worktree is clean after logically separated commits and all verification
  commands pass.
