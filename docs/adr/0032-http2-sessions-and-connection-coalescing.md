# ADR 0032 — HTTP/2: sessions, coalescing, and what a shared connection changes

**Status:** accepted · **Date:** 2026-08-06

## Context

ADR 0010 §3 decided that this browser would speak HTTP/2, and said the interesting part
correctly: it is mostly a *parser* problem — framing, multiplexing, flow control, and HPACK, whose
CVE history is state confusion between the two ends rather than buffer overruns. That decision is
not revisited here.

What ADR 0010 did not have, and what this one is for, is the shape the rest of `src/net` has to take
to hold it. Three things turned out to be decisions rather than implementation details, and all
three were discovered by building it:

**1. A connection stops being something a request owns.** Under HTTP/1.1 a `FetchRequest` holds a
socket for the length of one exchange and hands it back. An HTTP/2 connection is *shared while it is
in use*, by as many requests as the server will take. `ConnectionPool` therefore hands out two
different things, and one of them is a `shared_ptr`.

**2. The protocol is not known until after a socket is open.** ALPN settles it during the TLS
handshake. So the naive port of HTTP/2 into a pool that opens connections on demand produces this:
six concurrent images on one host open six sockets, each discovers independently that the server
speaks HTTP/2, and the page finishes with six sessions carrying one stream each. That is the
HTTP/1.1 burst wearing a new protocol — and it is precisely what **TD-0008** measured, where
`upload.wikimedia.org` answers HTTP 429 to a burst of six parallel connections and
`en.wikipedia.org/wiki/CSS` rendered between 4 and 17 of its 19 images at random.

**3. The cost of getting the demultiplexer wrong is silent.** A DATA frame delivered to the wrong
stream serves one response's bytes into another's. Nothing crashes, nothing fails a status check,
and the page renders — wrongly.

## Decision

### 1. The session is a pool-owned object, and a request holds a reference to it

`Http2Session` owns one `Transport` and carries many requests. `ConnectionPool` owns the sessions,
keyed by the **ADR 0005 partition key** exactly as idle HTTP/1.1 connections already are — two
top-level sites sharing a CDN host get two sessions, and that remains the whole privacy content of
the pool.

`ConnectionPool::Lease` gains a `shared_ptr<Http2Session>` beside its `unique_ptr<Transport>`, and
exactly one of the two is set. A session outlives the request that opened it and dies when the pool
drops it — on failure, or after the same idle timeout an HTTP/1.1 connection gets, and for the same
reason: a socket the user did not ask to keep open is part of the privacy surface rather than a
resource decision.

### 2. One connect at a time per origin while the protocol is unknown

The pool serialises the *first* connect to an origin it has not seen: one request connects, the rest
are told `wait_for_protocol` and ask again on a later turn. When the handshake answers, either a
session exists for them to join, or the origin is recorded as HTTP/1.1 and the bound comes off —
because a bound that is right while the protocol is unknown is wrong the moment it is known, and
HTTP/1.1 wants its six parallel connections.

Two consequences of this are load-bearing and are implemented rather than hoped about:

- **The claim is given back on every path out of `FetchRequest`.** There are five, and a claim left
  behind parks every other request for that origin until the 30-second stall deadline.
  `ReleaseEverything` is one function so that adding a sixth path is hard to get wrong.
- **A parked request has no socket of its own**, so it is woken by the connector's — and
  `RequestQueue::HasRunnableWork` asks `pool.PendingConnects()`, so that a connector which was
  cancelled does not leave the parked ones sitting until that deadline either.

The per-origin protocol memo is bounded at 32 entries and keyed by the partition key, for the same
reason the resolver cache is (ADR 0011): "has this browser learned that example.com speaks HTTP/2?"
is a question about where the user has been, and a memo keyed by host would answer it across sites.

### 3. One place where the two protocols diverge

`FetchRequest::ChooseProtocol` is that place. Everything above it — the `privacy::Verdict`, cookies,
the HTTP cache, CORS, the `Origin` header — and everything below it — redirects, content coding, the
retry-once rule — is written once and runs identically.

This is why `FetchRequest` keeps the header *list* rather than a serialized request: both protocols
send the same fields and only the framing differs, and two copies of "what this request contains" is
how they would come to disagree. It is also why `Http2Session` knows nothing about URLs, cookies or
CORS — it takes a method, an authority, a target and a header list, which is the seam
`ResponseParser` already sits on for HTTP/1.1.

### 4. What the session refuses

Each of these is a decision and not a simplification:

- **Server push.** `SETTINGS_ENABLE_PUSH = 0`, and a `PUSH_PROMISE` that arrives anyway is a
  connection error. A push is a response to a request the user never made, which
  `guidelines/privacy.md` forbids before performance is discussed. Every major browser has since
  removed it; this one never had it.
- **`PRIORITY`.** Read, length-checked, and dropped. There is one loop and no scheduler to
  prioritise against, so acting on it would be pretending.
- **Trailers.** Decoded, because HPACK is stateful and a skipped block desynchronises the
  connection permanently — and then discarded. Nothing here reads a trailer, and delivering them as
  response headers would let a server change `content-type` after the body it applied to.
- **A truncation dressed as an ending.** A response is over when `END_STREAM` says so and never
  because the socket closed. Same distinction the HTTP/1.1 parser draws between a self-delimited
  body and a close-delimited one, and the reason a half-received script is not an accepted script.
- **Connection-specific fields and uppercase field names.** Their presence in an HTTP/2 message is
  the signature of a proxy translating between two protocols it does not understand, which is where
  request smuggling lives.

### 5. The bounds, and the two that are not obvious

Every bound is enforced against what arrived rather than what the peer declared. Two deserve naming
because a reader will otherwise assume one of them covers the other:

- **The header block is bounded twice** — by encoded bytes and by CONTINUATION frame count. They
  catch different attacks. The byte bound catches one enormous block; the frame count catches the
  *flood*, which is an unbounded run of **empty** CONTINUATION frames that adds no bytes at all and
  therefore slips past a byte bound forever (CVE-2024-27316 and its family). HPACK's own header-list
  bound cannot help with either, because nothing has been decoded yet.
- **HPACK's dynamic table size is bounded by what we advertised, not by what the peer asks for.** A
  "dynamic table size update" is the one message whose entire content is "make the receiver hold
  more state".

Receive windows are raised well past the protocol's 65535 default — 8MB per stream, 32MB per
connection — because that default paces a 2MB script on round trips. They are accounting numbers,
not buffers; the real bound on one response is still `HttpLimits::max_body`.

Send-side flow control includes RFC 9113 §6.9.2's retroactive rule: a `SETTINGS` that changes
`INITIAL_WINDOW_SIZE` moves **every open stream's** send window by the delta. An implementation that
applied it to new streams only deadlocks a transfer already in flight — against servers that change
the setting mid-connection, which is to say only in production.

## Consequences

**TD-0008 closes, measured.** `en.wikipedia.org/wiki/CSS`, Release build, five consecutive runs
each:

| | images drawn (of 19) | `engine.images_failed` | connections | TLS handshakes |
|---|---|---|---|---|
| HTTP/1.1 | 4, 4, 7, 4, 10 | 15 | 13 | 13 |
| HTTP/2 | 19, 19, 19, 19, 19 | 0 | 3 | 3 |

Connections and handshakes on the other target pages, same build, same day:

| page | fetches | connections | TLS handshakes | h2 sessions |
|---|---|---|---|---|
| news.ycombinator.com | 6 | 1 | 1 | 1 |
| old.reddit.com | 53 | 6 | 6 | 6 |
| www.youtube.com | 32 | 9 | 9 | 3 |

old.reddit.com was **20 connections and 20 TLS handshakes for 40 fetches** after ADR 0010 §2 landed.

**Header compression is real but is not the win.** `net.hpack_block_bytes` against
`net.hpack_decoded_bytes`: 3.68x on old.reddit.com (16,772 against 61,638), 3.31x on youtube. Tens
of kilobytes on a page that transfers megabytes. The handshakes are the win, and on wikipedia the
win is that the page renders at all.

**A page can get slower by rendering correctly, and wikipedia did.** Paint, Release build:
1.62–1.84s on HTTP/1.1 against 2.13–2.79s on HTTP/2. The HTTP/1.1 run was faster because fifteen of
its image requests were refused with a 429 in a few milliseconds each; the HTTP/2 run downloads
74KB more and decodes fifteen more images. `wait::Network` is 252ms against 240ms — unchanged. This
is the priority order in `AGENTS.md` doing what it says, and it is worth stating plainly rather
than letting a table imply a regression.

**Coalescing costs latency on the first burst to an HTTP/1.1 origin.** Requests two through six wait
one connect-and-handshake before they learn they could have connected in parallel. It is bounded to
the first burst per origin — the memo takes it off afterwards — and it is the price of not opening
six sockets to an origin that wanted one. There is no measurement of it here because the target
pages' origins all speak HTTP/2; if one is ever taken, it belongs in `docs/tech-debt.md`.

**The per-partition concurrency bound is now the wrong number**, and that *is* debt with a
measurement: see TD-0010.

**Interleaving is now real, which constrains ADR 0030.** A response body no longer arrives
contiguously on its socket. Anything downstream that assumed it does has to stop; ADR 0010 said this
would happen and it now has.

## Alternatives considered

**Let every request open its own connection and negotiate independently.** Rejected on the
measurement that motivated the whole thing: it produces six sessions where one was wanted, which is
the burst TD-0008 is about. HTTP/2 without coalescing fixes the handshake count and not the 429.

**Guess the protocol from a hostname list, or assume `h2` for `https`.** Rejected. A guess that is
wrong costs a failed connection to a server that speaks only HTTP/1.1, and there are still many. The
serialized first connect is one round trip, once per origin, and it is the truth rather than a
prediction.

**Give `Http2Session::Request` `string_view` fields.** Rejected after shipping it for one commit.
Three of the four fields are *built* by the caller — the authority is host plus port, the target is
path plus query — so a view field invites `request.authority = AuthorityFor(url)`, which binds to a
temporary that dies at the semicolon. Every server on the web reset the stream or answered 400, and
the suite stayed green because the scripted server asserted on `:path` and nothing else. Four small
allocations per request, against a network round trip, buys the class of bug being impossible rather
than merely absent.

**Implement HPACK's encoder-side dynamic table.** Not rejected so much as deliberately not done. An
encoder that indexes must track what the peer evicted, and the two ends disagreeing about that table
is the whole of HPACK's CVE history. An always-empty table cannot fall out of step, and what it
costs is measured above: header traffic that compresses 3.7x instead of more, on pages that transfer
megabytes of body. `Cookie` and `Authorization` go out **never-indexed**, which forbids every
intermediary from indexing them too — that is what keeps a session cookie out of the CRIME family of
compression side channels, and it is not an optimisation to be traded away later.

**HTTP/3 instead, or as well.** Out of scope and not close. It needs QUIC, which is a UDP transport
with its own congestion control and its own TLS integration — a far larger dependency question than
this, and one ADR 0001 would have to answer first.
