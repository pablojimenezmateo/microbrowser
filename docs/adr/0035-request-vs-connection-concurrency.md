# ADR 0035 — Request concurrency is not connection concurrency

**Status:** accepted · **Date:** 2026-08-06

## Context

ADR 0010 and ADR 0032 gave this browser HTTP/2 sessions keyed by the ADR 0005 partition key, and
connection coalescing so six images do not open six sockets. The per-partition *concurrency*
bound stayed at six — `kMaxConnectionsPerPartition` — which was the HTTP/1.1 courtesy limit on
how many sockets a polite client opened to one host.

Under HTTP/2 that name and that number answered the wrong question. The queue counted **active
requests**, not sockets. One multiplexed session can carry dozens of streams; this browser opened
one connection and then used it six requests at a time.

**Measured** (before the split):

| Page | Symptom |
|---|---|
| old.reddit.com (Release) | 53 fetches, **91** `net.requests_deferred`, 6 h2 sessions |
| youtube.com (Debug) | **260** deferred turns on a load that needed the subresources now |

That is not a privacy win. The privacy content of the bound is "one partition must not starve
another by holding a *global* slot". It is not "one partition may only have six GET bodies in
flight on a connection that would take a hundred".

TD-0010 named the debt. This ADR is the decision.

## Decision

### 1. Two bounds, two names

| Constant | Default | Counts | Why |
|---|---|---|---|
| `kMaxConnectionsPerPartition` | 6 | sockets a partition may open | ADR 0005: no global socket pool a site can time; HTTP/1.1 still wants ~6 |
| `kMaxRequestsPerPartition` | 64 | requests in flight per partition | H2 streams (and H1 requests) on those sockets |

`RequestQueue::PromoteQueued` consults the **request** bound. The connection pool and coalescing
rules in ADR 0032 continue to decide how many sockets exist.

### 2. Sixty-four is measured enough to ship, not the end state

64 sits below a typical `SETTINGS_MAX_CONCURRENT_STREAMS` (100) and cleared youtube's deferred
spike (**260 → 39** on a Debug snapshot after the change) without a new OOM class in the same
run. A hundred concurrent response bodies, each allowed up to `HttpLimits::max_body`, is still an
aggregate memory question this ADR does **not** close — a per-queue byte budget wants its own
measurement before the request bound tracks the peer's SETTINGS fully.

### 3. Privacy stays on the partition key

Both bounds remain per ADR 0005 partition. A global request ceiling is still rejected: it is an
observable cross-site interaction. Raising the per-partition request bound does not create one.

## Consequences

- **Positive:** HTTP/2's multiplexing is usable for page loads, not only for reducing handshake
  counts. Target pages stop waiting on an artificial queue while the session is idle.
- **Positive:** The socket bound keeps its privacy documentation; the request bound can move with
  evidence without re-arguing Total Cookie Protection.
- **Negative:** More concurrent bodies mean more peak memory. Pages that fan out hundreds of
  large downloads can stress the process; the byte-budget follow-up is required before chasing
  the server's SETTINGS ceiling.
- **Negative:** Tests that assumed "six held images" must use `kMaxRequestsPerPartition` (see
  `Engine/ConcurrencyIsBoundedPerPartition`).

## Alternatives considered

**Keep one constant named "connections" that really means requests.** Rejected. The youtube and
reddit numbers exist because that lie survived ADR 0032.

**Set the request bound to the peer's `SETTINGS_MAX_CONCURRENT_STREAMS` immediately.** Rejected
until aggregate body memory is bounded. Correct multiplexing with unbounded peak RAM fails
ADR 0033's memory ranking and turns a fast load into a tab killer.

**Raise only for H2, leave H1 at six.** Attractive, and compatible with this ADR's table: the
request bound may later become "min(64, sum of peer stream limits)" for H2 and 6 for H1. Not
required to unblock the targets; deferred to the byte-budget session.
