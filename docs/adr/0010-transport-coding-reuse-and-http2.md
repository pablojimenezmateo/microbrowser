# ADR 0010 — Transport: content coding, connection reuse, and HTTP/2

**Status:** accepted · **Date:** 2026-08-04

## Context

`src/net` speaks HTTP/1.1 over TLS and does three things that are each defensible alone and
indefensible together:

- `Accept-Encoding: identity` — every response arrives uncompressed.
- `Connection: close` — every response ends the connection.
- No ALPN — the TLS handshake never offers a protocol, so every server falls back to HTTP/1.1.

Loading youtube.com's front page with counters on measures what that costs: **15 fetches, 15
connections, 15 TLS handshakes.** One full handshake per subresource, and none of the transfer
compressed.

The compression figure is not marginal:

| Resource | As fetched | gzip | Saved |
|---|---|---|---|
| the document | 904 KB | 246 KB | 73% |
| the application bundle | 10.7 MB | 2.5 MB | 77% |
| the stylesheet | 3.5 MB | 369 KB | 90% |
| **total** | **15.1 MB** | **3.1 MB** | **79%** |

So the browser currently transfers about **five times** what it needs to, over about **fifteen
times** as many handshakes as it needs, on the one page it was pointed at.

Two things make this cheaper to fix than it looks. `util::Inflate` already exists and is already
fuzzed — it was written for PNG, and gzip is a DEFLATE stream with a different header. And
`Accept-Encoding: identity` is not a missing feature, it is a *request header we send*: the servers
have always been willing.

The counter-argument for the current state is real and belongs on the record. Compression on a
response is a decompression bomb waiting to be written — a few hundred KB expanding to gigabytes —
and connection reuse is a cross-request linkage, which is a privacy question before it is a
performance one. Neither is a reason to stay uncompressed and unpooled; both are reasons the design
has to say what bounds it.

## Decision

Three changes, in this order, because each is independently useful and the order is by value per
unit of risk.

### 1. `Accept-Encoding: gzip, deflate`, with a bounded inflate

The decoder exists. What has to be added is the framing and the bound.

**Every decompression is bounded twice**: an absolute ceiling on output size, and a maximum
expansion ratio against the compressed input. Exceeding either fails the response rather than
truncating it — a truncated document is a document the parser will happily misread, which is the
same argument the parse depth bound (ADR 0009) makes about half-understood programs.

The ratio bound is the one that matters, because it is the one that catches a bomb whose output
would otherwise sit just under the absolute ceiling. Both numbers get chosen the way ADR 0009's was:
measured against real pages, with the margin written down.

**A gzip fuzz target lands on the same commit**, per `guidelines/security.md`. `util::Inflate` is
already fuzzed for the raw stream; the header parsing and the bounds are new code on the hostile
path and are not covered by that.

Brotli is **not** in this decision. It needs a third-party dependency and therefore its own ADR
against ADR 0001. gzip gets 79% of the transfer back with no new dependency at all, which makes
brotli an optimisation on a solved problem rather than a blocker.

### 2. Connection reuse, keyed by the partition key

`Connection: close` goes; connections are pooled and reused.

**The pool is keyed by the partition key from ADR 0005, not by host.** This is the whole privacy
content of the change and it is not optional: a reused connection is a linkage between two requests,
observable by the server, and ADR 0005 already lists the connection pool as one of the things the
key covers. Pooling by host would create exactly the cross-site correlation the key exists to
prevent, and it would do it in a data structure rather than a policy flag — which is the failure
mode that ADR 0005 was written to make impossible.

Idle connections are closed on a timer, and that timer is subject to the zero-idle-CPU invariant:
it goes through `IdleWaitState::next_deadline_ms` like everything else, and a browser with no
connections open schedules nothing.

### 3. HTTP/2

ALPN offers `h2` and `http/1.1`; the server picks. youtube.com and google.com both choose `h2` when
offered, and ADR 0007 already records that Google effectively requires it.

HTTP/2 is a substantial piece of work and it is worth being explicit that it is mostly a *parser*
problem, which is to say a security problem: framing, stream multiplexing, flow control, and HPACK.
HPACK is the part to be most careful with — it is a stateful compressor with an attacker-controlled
dynamic table, and its CVE history is about state confusion between the two ends rather than about
buffer overruns. It gets its own fuzz target, and the dynamic table gets a hard size bound that is
enforced on the decode side regardless of what the peer claims.

**Priority: 2 before 3.** Connection reuse gets most of the handshake cost back under HTTP/1.1
already, and it is a fraction of the code. HTTP/2's remaining win over pooled HTTP/1.1 on these
pages is multiplexing, which matters once loading is concurrent. That condition is now met: ADR 0011
landed and a page's subresources are fetched at once, bounded per partition key. The ordering still
holds and the reason has shifted — reuse is a fraction of the code and gets most of the handshake
cost back, and concurrency has made the handshakes parallel rather than serial rather than making
them go away.

## Consequences

- The privacy layer is unaffected in shape: `Fetch` still takes a `privacy::Verdict` and there is
  still no overload without one. Compression and pooling sit under that, not beside it.
- Connection reuse changes what "a request is user-caused" means at the margin — a pooled connection
  outlives the request that opened it. It carries no data on its own, but it is a socket the user
  did not ask to keep open, so the idle timeout is part of the privacy surface and not just a
  resource decision.
- HTTP/2 makes the response body arrive interleaved with other streams. Anything downstream that
  assumes a body arrives contiguously has to stop assuming it, which is a real constraint on the
  incremental parsing that ADR 0011 wants.
- The 79% figure is one page on one day. It is recorded as the measurement that motivated the
  decision, not as a number to defend.

## Alternatives considered

**Keep `identity` and treat compression as a performance nicety.** Rejected on the measurement. A
5x transfer difference is not a nicety on a page this size, and on a slow connection it is the
difference between a page that loads and one that does not.

**Brotli first, since it beats gzip.** Rejected on dependency cost. Brotli is roughly 15-20% better
than gzip on this content and requires a new sanctioned library; gzip is 79% better than nothing and
requires none. The right order is obvious once they are stated that way.

**Pool connections by host, and partition later.** Rejected as exactly backwards. Retrofitting a
partition key onto a live pool means finding every place that assumed one key, which ADR 0005
already argues is the mistake that makes privacy a flag instead of a structure. The key is cheap to
put in at the start and expensive to add afterwards.
