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

## Session 3 — the selectors that are silently not matching · 2026-08-05

**Status:** done

**Check:** the ledger asks for "a snapshot of reddit before and after, side by side… the diff
should be obvious." It is not obvious, and the reason is a finding rather than a failure — see
below. Three measurements, taken against the pre-session binary built from `9ee2a2b` in a git
worktree:

1. **Rules that now parse, deterministic.** Old reddit's ten stylesheets as served
   (`reddit.Gsb42QVNY6g.css` and nine others, 368KB total), parsed directly by
   `ParseStyleSheet` linked against each build:

   ```
   BEFORE: rules=3127 skipped=122
   AFTER : rules=3153 skipped=96
   ```

   +26 rules, −26 skipped, and **nothing lost**. On a wider sample — thirteen sheets from
   github, wikipedia, MDN and Hacker News — `233/72` became `250/55`, +17.

2. **A visible reddit diff, where the feature is actually used.**
   `https://old.reddit.com/r/television/comments/1vbtdij/…` renders the spoiler markup that
   `.md .md-spoiler-text:not(.revealed){background:#4f4f4f;cursor:pointer;color:transparent}`
   is for. Before: the spoiler text reads plainly. After: it is gone, as the author intended.
   A threshold difference mask over the two snapshots picks out exactly three words — the two
   `Spoiler`s and the `Television` inside a spoiler span — plus vote counts that drifted
   between the two fetches.

3. **A more obvious diff on a page that uses them heavily.** `en.wikipedia.org/wiki/CSS`:
   125,612 pixels differ, because the "Jump to content" skip link is now hidden until focused
   and the whole page moves up with it.

`tools/run-checks.sh tests`, `asan` and `ubsan`: 100% of 24 shards each. `css_fuzzer` over the
corpus plus five new seeds for this grammar: 774,879 runs in 91 seconds, no crash.

**Landed:** *A selector list may hold a selector list, and `:where()` is worth nothing.*

**Left:**

- `:has()` is the fifth selector of ADR 0016 §1 and is not implemented; it parses as a failure,
  so a rule using it is dropped exactly as before. The ADR prices it separately and behind a
  measurement, and this session did not take that measurement.
- `:nth-child(An+B of S)` is likewise not implemented and drops its rule. It appears zero times
  in the 23 stylesheets measured.
- **`background` on an inline box is not painted.** Found while looking at the spoiler fix: the
  rule applies — `color: transparent` took effect — but no `FillRect` with `#4f4f4f` appears in
  the display list, so a spoiler is invisible text on white rather than a grey bar. That is a
  paint gap, not a selector gap, and it is on no session's list.
- **`Selector::Matches` recurses once per compound and is not bounded.** Parsing is now bounded
  at eight levels of nesting, but a selector with a hundred thousand descendant combinators
  would still recurse a hundred thousand deep at *match* time. Pre-existing, and unreachable
  from `CssFuzzer` because the fuzzer parses without matching.

**Found:**

- **The 212 misses ADR 0016 counts are `www.reddit.com`'s stylesheet, not `old.reddit.com`'s.**
  The ADR's table (`:not(` 136, `:is(`/`:where(` 52) is from `styles-css-CSasxzfw.css`, the new
  reddit bundle, which is behind the JavaScript challenge that Phase A of the roadmap exists to
  get past. Counted directly, old reddit's sheets use these features **43 times**: 31 `:not(`,
  8 `:nth-child(`, 1 `:nth-of-type(`, and **zero** `:is(` or `:where(`. So the session's check
  as written could not have produced an obvious diff on the page it names, and the next agent
  should read a check that says "reddit" as "which reddit".
- **Most of the 43 gate a state the page is not in.** Collapsed comments, revealed spoilers,
  the traffic table, `body:not(.loggedin) .give-gold-button` (whose target is injected by
  script that never runs). +26 rules parsing is the honest measure; +26 rules *visibly firing*
  is not what happened.
- **Counter-based before/after on reddit is worthless, and it took two rounds to see why.**
  `css.rules_parsed` on the same comments page with the same binary gave 7,599 then 12,981;
  `css.tokens` gave 181,494 then 306,207. `css.sheets_parsed` was 90 both times. The cause is
  that `Tokenize` is also reached from `ParseSelectorList` and `ParseDeclarationList`, so every
  `querySelector` and every `style=""` assignment a script makes lands in those counters — and
  reddit's scripts die at a different point on every load (the masked `r is not defined`). The
  same comparison on Hacker News is bit-identical across runs. **A counter that a page's
  scripts can move is not a measurement of the parser.** Parsing the sheets directly, out of
  process, is.
- **`2n-1` and `n-1` are one token each, not three.** A CSS name may contain a hyphen and a
  digit, so `2n-1` tokenizes as a dimension whose *unit* is `n-1`. Every implementation splits
  that back apart by hand, and it is why `ParseNSuffix` exists. `2n- 1` is a third spelling
  again — unit `n-`, then a signless integer.
- **`Token` had thrown away the one bit this grammar needs.** `2n 1` is not a selector and
  `2n +1` is; the difference is an explicit `+` that the tokenizer discarded because the value
  is the same either way. One bool on `Token`, and the alternative was accepting an invalid
  selector — which applies a rule to elements nobody named.
- **The line cap pointed at a real missing module.** `StyleSheet.cpp` reached 1,224 lines and
  splitting it three ways was not busywork: `SelectorMatch.cpp` is now a pure function of
  (element, selector) that cannot see a token, which is exactly the shape ADR 0016 §2 needs
  when it puts `:hover` on the element rather than in the matcher.
- **`:not()` of an unimplemented pseudo-class is now true, and that is a decision.** It is right
  for `:hover`, `:active`, `:focus` and `:visited`, whose honest value in a static snapshot is
  false — reddit's video controls say `:not(:hover):not(:active)` and mean the resting state.
  It is wrong for `:checked` and `:disabled`, whose value is derivable from the DOM and is not
  being derived. Session 11 is where that stops being wrong. A test records it either way, so
  the day it changes, something says so.
