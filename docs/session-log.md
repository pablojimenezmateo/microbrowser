# Session log

Append-only. One entry per roadmap session, newest last. The commit history says what changed;
this says **what the next agent needs and could not derive from a diff** — what was tried and
rejected, what the check actually printed, what turned out to be wrong.

Written by `/next-session` (see `.claude/commands/next-session.md`). The state it reads is
`docs/roadmap-sessions.json`; the argument behind that state is `docs/roadmap-to-any-page.md`.

Template:

```
## Session <n> — <title> · <date>

**Status:** done | in_progress
**Check:** what you ran, and what it actually printed — not "passed".
**Landed:** the commits, by subject.
**Left:** what the next agent inherits.
**Found:** anything that contradicts the roadmap, an ADR, or CLAUDE.md.
```

---

## Session 0 — the harness itself · 2026-08-04

**Status:** done
**Check:** n/a — this session built the loop rather than a browser feature.
**Landed:** `docs/roadmap-sessions.json` (the 49 roadmap sessions as state),
`.claude/commands/next-session.md` (the per-session prompt), `tools/agent-loop.sh` (the
fresh-context driver), this file.
**Left:** Session 1 is `in_progress`. As of `b5f5c11`, `DOMContentLoaded`, `readyState`,
`URLSearchParams` and `location.search` have landed; `document.forms`, `form.elements`,
`namedItem()` and `requestSubmit()` have not.
**Found:** The ledger omits roadmap rows 43–45 (folded into 42, the process split) and 49+
(tabs, downloads, upload, printing — product surface, not engine). Phase E rows carry no `check`,
because the roadmap states none; writing one is the first deliverable of each of those sessions.

## Session 1 — the door · 2026-08-04

**Status:** done

**Check:** `./build/microbrowser/microbrowser_snapshot https://www.reddit.com/ -o out.ppm` printed

```
https://www.reddit.com/?solution=c5db4a57…&js_challenge=1&token=7afd7253…&jsc_orig_r=:
  333 commands, 101 runs, 4 fonts, 8 images, title "Reddit - The heart of the internet"
```

The URL in that line is the point: the interstitial's script filled in its own form and the
browser navigated to the answer. Before this session the same command produced
"Please wait for verification".

**Landed:**

- *Three percent-decoders become one, and the form serializer becomes correct*
- *location tells the truth about a URL, and URLSearchParams exists*
- *An assignment to a DOM property reaches the element it describes*
- *The door: a script fills in a form and the browser navigates*

**Left:** `www.reddit.com` renders but its own scripts stop on
`ReferenceError: PerformanceObserver is not defined`, thrown from the inline telemetry bundle.
Session 7 (geometry) is the next scheduled one; this is a smaller thing beside it.

**Found**, and none of it was in the roadmap or the survey:

- **`Object.assign` was writing slots, not invoking setters** — a bug in `src/js`, not in the
  bindings. `Object.assign(document.createElement('input'), {name, type, value})` is precisely the
  shape the challenge is written in, and it set three properties on the wrapper and produced an
  element with no attributes. It also meant a proxy's `set` trap was skipped by `assign`.
- **Every reflected DOM attribute was missing or getter-only.** `el.id = 'x'` was a silent no-op:
  the write succeeded, read back, and described nothing. A class set that way never reached the
  cascade.
- **`location.pathname` was everything after the host**, so `location.search` was `undefined` and
  `new URLSearchParams(undefined)` is an empty, useful-looking parameter set.
- **A bare `addEventListener('load', f)` registered on nothing.** No receiver, sloppy-mode `this`,
  and `call.self` arrives undefined — so half the pages that listen for `load` were never told.
- **`readyState` answering "complete" always was the wrong half of the trade.** It was chosen so a
  page would not wait for a `DOMContentLoaded` that had already happened; the cost was that the
  pages which *only* listen waited forever.
- **`document.forms` installed on the document's wrapper landed on an object nothing could reach.**
  `EnsureInterfaces` runs from the *first* `WrapperFor`, so asking for the document's wrapper inside
  it builds a second one and the outer call caches the first. Anything installed from
  `EnsureInterfaces` belongs on an interface prototype.
- **The click and Enter submission paths fired no `submit` event at all.** Not a session-1 item on
  paper; it is the same one line of the specification seen from the other side, and all three routes
  now go through one `Page::SubmitForm`.
- Three percent-decoders existed — `url`'s, `engine`'s, and the one `bindings` was one commit away
  from adding. They are one now, in `util`.

## Session 2 — transport · 2026-08-04

**Status:** done

**Check:** `MICROBROWSER_PERF_COUNTERS=1 ./build/microbrowser/microbrowser_snapshot
https://old.reddit.com/ -o /tmp/reddit.ppm` printed

```
[counters]             505859  net.bytes_coded
[counters]            2161404  net.bytes_decoded
[counters]             697254  net.bytes_received
[counters]                 20  net.connections
[counters]                 40  net.connections_pooled
[counters]                 20  net.connections_reused
[counters]                 40  net.fetches
[counters]                 20  net.tls_handshakes
```

Before this session the same page was **40 connections and 40 TLS handshakes for 40 fetches**,
with every byte uncompressed. The identity-equivalent transfer is
`bytes_received - bytes_coded + bytes_decoded` = 2,352,799 against 697,254 on the wire: **3.37x,
70% saved**, against the 79% in ADR 0010's table. The gap is that reddit's front page is partly
images, which were already compressed and gain nothing.

Sanitizers: `tools/run-checks.sh asan` and `ubsan` both 100% of 24 shards, after a real fix — see
below.

**Landed:**

- *gzip is a DEFLATE stream with a header, and the header is the hostile part*
- *Accept-Encoding stops lying, and a body may not grow without a bound*
- *A connection outlives the request that opened it, and belongs to one partition*
- *A test connection may now outlive the factory that made it, and says so*

**Left:** ADR 0010 §3 — HTTP/2 and ALPN — is untouched and still the ADR's own third priority.
Session 3 (the selectors that are silently not matching) is next on the ledger.

Two things a next agent should know before touching this code:

- **An idle pooled connection is not in the loop's wait.** It has a deadline and no descriptor, so
  a server that closes one while it sits idle is discovered by writing into it, not by being woken.
  That is what the one retry in `FetchRequest::MayRetryOnFreshConnection` is for. Putting idle
  sockets in the wait would let the browser notice sooner, at the cost of a wakeup per close.
- **`Engine::NextDeadlineMs` is no longer gated on `load_.active`.** It had to stop being: the case
  that matters for the idle timeout is precisely the one with nothing loading, and the old gate
  would have held a socket open until the next navigation happened along.

**Found:**

- **The check's own wording is not reachable, and the reason is the previous session.** "One
  connection per host per partition" cannot hold while six requests per partition run concurrently
  — six concurrent requests to one host need six connections under HTTP/1.1. 40 → 20 is what
  reuse is worth here; on Hacker News, a single host with five fetches, it is 5 → 4. Reuse pays
  where requests are *serialized* — a document then its subresources, a redirect chain, the click
  after a page — and concurrency is what limits it. Read the check as "far fewer than one per
  resource".
- **A gzip bomb can be refused before a single byte is produced.** The member declares its own
  uncompressed size in ISIZE, and a claim above the bound cannot be within it. This was not in the
  ADR, which describes the bound as something the decoder enforces while running. It is the one
  direction an attacker's number cannot be made useful in — lying the other way is caught by the
  checksum — and it is why `DecodeStatus::TooLarge` can mean something exact.
- **The decoder cannot say whether it stopped because of the bound.** It fails on the back
  reference that *would* exceed the ceiling, so its output length is short by an arbitrary amount
  and says nothing. The first version of this distinguished "too large" from "malformed" by
  comparing output size against the bound, and it was wrong for exactly that reason — the test
  that caught it was the one asserting a 132-byte bomb is refused. A status that depends on which
  byte a decoder happened to stop at is not a status.
- **ASan and UBSan both failed 18 of 24 shards on the pooling commit, at the same line, and the
  test suite was green.** `~ScriptedTransport` reaching into a `Factory` already destroyed: a
  pooled connection outlives the request that opened it, so in a test that declares
  `ScriptedFactory factory;` *after* the `Session` holding the engine, the factory dies first and
  the pool's idle connections die second. Production ordering was already right — `Loader` declares
  `sockets_` before `queue_` — so this was the test support, but a support class that works only
  when two locals are declared in the right order is a trap. It now detaches its connections when
  it dies. **The lesson is the sanitizers**: an ownership change that reaches past a lifetime is
  exactly what a green suite does not see.
- **`Connection: close` was hiding a second decision.** Removing it means a response's framing has
  to be exactly right, because the connection no longer ends the message — which is why
  `ResponseParser` grew `BodyWasSelfDelimiting`, `Leftover` and `NothingReceived`. A body delimited
  by the close cannot travel on a kept connection at all, and a byte past the end of a message is
  request smuggling with one browser playing both parsers.
- **`net.bytes_coded` alongside `net.bytes_decoded` is what makes the check reproducible from a
  single run.** Without the input side, "how much did compression save" needs a build of the
  previous commit to answer. It cost one line.
