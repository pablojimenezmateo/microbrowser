# ADR 0020 — The network a page asks for itself, and the policies over it

**Status:** accepted · **Date:** 2026-08-04

## Context

ADR 0011 made loading asynchronous and said what that unblocks:

> **This is what unblocks the module loader.** With a loader that can answer later, `fetch()` is then
> a thin binding over the same machinery rather than a second one.

ADR 0012 ordered `XMLHttpRequest` then `fetch`, and said one sentence about the policy: **CORS is
enforced in the network process, not in bindings.** Neither ADR decided the rest, and the rest is
most of it. The survey:

| | occurrences |
|---|---|
| `fetch(` | 130 |
| `FormData` | 81 |
| `XMLHttpRequest` | 37 |
| `WebSocket` | 24 |
| `EventSource` | 20 |
| `AbortController` | 17 |
| `innerHTML` | 34 |
| `DOMParser` | 4 |

Two site-specific facts sharpen this into a decision rather than a list.

**reddit's page is fetched, not served.** Its front page HTML server-renders three posts and then
names **31 distinct `/svc/shreddit/…` endpoints** that return HTML fragments, loaded by
`<faceplate-partial>` and inserted into the tree. So `fetch` plus fragment parsing is not an
enhancement to reddit; it is how reddit has content. Plex's 97 `fetch(` sites are the same story
without the HTML.

**Both sites ship policy headers we currently ignore.** reddit sends
`content-security-policy: default-src 'none'; script-src 'nonce-…'` and marks 48 script tags with
that nonce. Plex ships `integrity="sha384-…" crossorigin="anonymous"` on all four of its
subresources. A browser that ignores both loads the pages fine — and gives up the two mechanisms
those sites are using to protect their users, on a browser whose stated priority order puts security
second overall and first among the things that are not correctness.

## Decision

### 1. One request path, and `fetch` is the shape of it

`fetch` first, `XMLHttpRequest` implemented **over the same request machinery** afterwards. ADR 0012
had this the other way round on the reasoning that the polyfill wanted XHR; that ADR's own amendment
retired the polyfill-first strategy, and 130 `fetch(` sites against 37 XHR sites is the measurement.

Both go through `net::RequestQueue`, both take a `privacy::Verdict`, and there is no overload without
one — the lint in `tests/ArchitectureInvariantsTests.cpp` already enforces that and this ADR adds
call sites to it rather than exceptions.

The request/response object model — `Request`, `Response`, `Headers`, `Body` with `.text()`,
`.json()`, `.arrayBuffer()`, `.formData()` — is built once and XHR's older shape is expressed in
terms of it. `AbortController` / `AbortSignal` is part of it and not an extra: 17 sites, and a
request that cannot be cancelled is a request that outlives the navigation that made it, which
ADR 0011 already says must be impossible.

**Streaming bodies are deferred for progressive delivery; a one-chunk stream is
not.** `response.body` returns a `ReadableStream` that yields the already-buffered
body as a single `Uint8Array` chunk. That is enough for SABR / `getReader()`
consumers (youtube.com) without claiming bytes arrive before the fetch settles.
The stream is a **branded instance** of `ReadableStream` (`instanceof` and
`ReadableStream.prototype` hold); a plain object with an own `getReader` is not
a stream. `new ReadableStream({start})` remains an illegal constructor until the
controller model exists — feature detection sees the global; construction does
not invent a stub.

### 2. CORS is enforced where the attacker is not, and that is the network process

Restating ADR 0004's rule because this is the first feature that makes it concrete rather than
theoretical. **The check happens in the network process, on the response, before any byte reaches
the renderer.** A cross-origin response without the headers that permit it is never delivered — not
delivered and then hidden.

That distinction is the whole thing. An implementation that fetches, then checks, then throws in the
binding has already put the bytes in the address space of the process running attacker-supplied
code, and every side channel from there is a cross-origin read. The process split is not finished
(ADR 0004 is M7), so **the check is written at the `net`/`engine` seam now, in the place the network
process will be**, with the response discarded rather than marked. When the split lands the code
moves without changing.

What that means concretely:

- `mode: "cors"` — the default — requires `Access-Control-Allow-Origin` to match, and a preflight
  `OPTIONS` for non-simple requests, with the preflight result cached per **partition key** rather
  than per origin (ADR 0005: a preflight cache is a cross-site linkage like any other cache).
- `mode: "no-cors"` yields an **opaque** response: status 0, no headers, no body readable. Opaque
  has to be a real thing in the response type, not a flag a binding checks, or someone will read
  through it.
- `credentials: "omit" | "same-origin" | "include"`, and `include` requires
  `Access-Control-Allow-Credentials` with a non-wildcard origin.
- Redirects are followed with the origin re-evaluated at **each hop**, because a redirect to a
  third party is how a same-origin request becomes a cross-origin one.

### 3. Content-Security-Policy is enforced, not logged

reddit's `default-src 'none'; script-src 'nonce-…'` is enforceable and cheap, and the shape of the
enforcement is the interesting part: **CSP is a check on every fetch and every script execution,
placed at the same seam the privacy verdict already occupies.** `privacy::Verdict` decides whether a
request is allowed by *our* policy; CSP decides whether it is allowed by the *page's* policy. Two
policies, one chokepoint, and a request needs both.

What lands: `default-src`, `script-src`, `style-src`, `img-src`, `connect-src`, `frame-src`,
`form-action`, `base-uri`, nonces, `'self'`, `'none'`, `'unsafe-inline'`, and host-source matching.

What does not, and why it is a decision rather than an omission:

- **`report-uri` / `report-to` are not implemented.** A violation report is a network request the
  user did not cause, sent to a third party, describing what the user's browser did. `AGENTS.md`
  forbids exactly that shape. Violations are enforced and logged locally; nothing is sent. reddit's
  three `report-to` groups and its `NEL` header get the same treatment for the same reason.
- **`unsafe-eval` is enforced** — `eval` / `Function` exist (ADR 0039); a
  governing `script-src` / `default-src` without `'unsafe-eval'` throws
  `EvalError`.

An unparseable or unknown directive **fails closed for that directive and open for the policy**,
which is what the specification requires and is the opposite of the instinct. Failing the whole
policy closed would break pages on a directive we have not implemented yet.

### 4. Subresource Integrity, because Plex is asking for it

`integrity="sha384-…"` on a `<script>` or `<link>`: hash the fetched bytes, compare, and **refuse to
execute or apply on mismatch**. It requires `crossorigin` for cross-origin resources, which is why
Plex sets both.

SRI is perhaps a hundred lines given a SHA-2 implementation, and it is the cheapest security feature
on this entire roadmap per unit of code. It also has a property worth stating: it is the only
mechanism here that protects the user against *the site's own CDN*, which is a threat model the
site chose to defend against and we would otherwise silently discard.

A SHA-256/384/512 implementation is needed and is not a third-party dependency question — the TLS
stack already has one, and reaching it is a `MODULE.deps` question rather than an ADR 0001 one.

### 5. `WebSocket` and `EventSource`: yes, and they are the first long-lived connections

Plex's 15 `WebSocket` sites are how its web client learns that something started playing somewhere
else. Without it the UI is stale until reloaded, which is a Plex that does not work rather than a
Plex that is missing a feature.

Both are implemented, and both bring the same new problem: **a connection that stays open with no
request outstanding.** Against the zero-idle-CPU invariant that is fine and it must be fine *for the
stated reason*: the socket is a descriptor handed to `IdleWaitState`, the loop blocks on it, and a
server that says nothing costs nothing. What is not fine is a keepalive ping on a timer, and
WebSocket's ping/pong is therefore **responsive only** — we answer a ping, we do not originate one.

Both are subject to `connect-src`, both are keyed by the partition key, and both die with the page
that opened them, by construction, exactly as `RequestQueue` requests do.

`EventSource` additionally reconnects on drop, which is a request the user did not directly cause.
It is bounded — exponential backoff, a cap, and it stops when the document goes away — and the bound
is written where the reconnect is.

### 6. Parsing HTML that arrived after the parse

reddit's 31 fragment endpoints return HTML, and something has to turn it into nodes. That is
`innerHTML`, `insertAdjacentHTML`, `DOMParser` and `<template>`'s content, all of which are the
**fragment parsing algorithm** — the tokenizer and tree builder run with a context element, so that
`<td>` in a fragment parsed into a `<tr>` does the right thing.

`src/html` is spec-literal and already has both halves; what is missing is the entry point and the
context-element rules. It is a genuine parser change on the most hostile input path in the browser,
so it lands with a fuzz target on the same commit, per `guidelines/security.md` — the fuzzer feeding
fragments with a randomly chosen context element, because that is the parameter no existing fuzz
target varies.

## Consequences

- **The privacy chokepoint gets busier and more valuable.** Every script-initiated request passes
  `privacy::Verdict` *and* CSP *and* CORS. Three policies at one seam is a lot of logic in one place,
  and that is correct: the alternative is three places.
- **`src/net` grows a policy layer it did not have.** Whether CORS and CSP live in `net` or in a new
  module is a `MODULE.deps` question to settle when the code is written; what this ADR fixes is that
  they do not live in `src/bindings`.
- **A page can now open connections that outlive its requests.** The privacy surface changes shape:
  ADR 0010 already noted that a pooled connection is a socket the user did not ask to keep open, and
  a WebSocket is that with the page's knowledge. Both are killed by navigation.
- **Fragment parsing is a new hostile entry point into the tree builder**, reachable from script with
  attacker-chosen context. It is the highest-risk item in this ADR and the reason the fuzz target is
  non-negotiable.
- **This finishes the third item on `CLAUDE.md`'s pick-up list** and takes the module-resolver
  question with it: with a loader that answers later, `SetModuleResolver`'s host half can fetch, and
  reddit's `<script type="module" src="data:text/javascript,…">` needs the `data:` scheme in the URL
  loader as well.

## Alternatives considered

**Implement `fetch` without CORS and rely on the eventual process split.** Rejected. It means every
cross-origin response is readable by page script for as long as the split takes, which is not a
missing feature but an open cross-origin read — the single worst thing a browser can have.

**Enforce CORS in the binding, since there is no network process yet.** Rejected on ADR 0004's rule
and on where the code ends up. A check in `src/bindings` is a check inside the process that will be
the sandboxed one; writing it there means writing it twice, and the second writing is the one
somebody skips.

**Log CSP violations without enforcing.** Rejected as the definition of security theatre. A policy
that is parsed and not applied costs the code of enforcement and delivers none of it.

**Send CSP and NEL reports, since the site asked for them.** Rejected on the privacy contract. It is
an outbound request the user did not cause, to a third party, describing the user's browsing. The
site asking is not the user consenting, and `AGENTS.md`'s rule has no exception for "the page would
like it".

**`XMLHttpRequest` first, per ADR 0012.** Superseded rather than rejected. That ordering came from
the polyfill-first strategy which ADR 0012's own amendment abandoned, and the counts point the other
way.
