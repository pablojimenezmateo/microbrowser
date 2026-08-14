# ADR 0040 — web-platform-tests as the primary correctness signal

**Status:** accepted · **Date:** 2026-08-10

## Context

Every rendering, layout and binding bug found in the last twelve sessions of
this project was found the same way: load a real page, look at it, notice
something wrong, and work backwards. `docs/session-log.md` says so repeatedly,
and `CLAUDE.md` recommends the method in as many words — "Every layout and paint
bug listed in the git log of the last session was found by rendering a real
page and looking at it; none of them failed a test first."

That method has taken this browser a long way and it has four costs that are now
the binding constraint:

1. **A real page is not a stable instrument.** old.reddit.com and youtube.com
   change under us. Four of the fixes in the 2026-08-06 pass were located by an
   offset into a minified bundle that no longer exists.
2. **A real page fails for one reason at a time.** `www.reddit.com` stopped at
   `PerformanceObserver`; everything behind that binding was invisible until it
   landed. A page is a sequential lock on the features it happens to use first.
3. **A real page cannot say what a fix broke.** There is no per-feature signal,
   so a regression in a corner nothing on the front page exercises is found by
   the next session, or never.
4. **It cannot be delegated.** "Load reddit and look at it" is one agent's whole
   session. It does not shard, and two agents doing it collide.

`tools/snapshot/main.cpp` is the evidence for all four: it has grown 400 lines
of *site-specific* settling heuristics — `RedditFeedLooksReady`,
`YoutubeWatchLooksReady`, `YoutubeResultsLooksReady` — whose only job is to
decide when a particular third-party page has finished changing. That is
infrastructure spent on the instrument rather than on the browser.

web-platform-tests is the same coverage as a stable, per-feature, per-subtest
signal: ~42,000 tests in the areas this browser has code for, each one a
statement about the specification with a name, each one runnable in isolation,
all of them shardable across processes and therefore across agents.

## Decision

**WPT becomes the primary correctness signal. Real pages remain the primary
*discovery* signal.** They answer different questions and neither replaces the
other: WPT says which of the platform is implemented, and a real page says which
of the platform pages actually need. Both stay.

Six decisions make it work here — the sixth added by the first baseline run, and it
is the one that says what the instrument cannot see.

### 1. The checkout is pinned, sparse, and not vendored

`tools/wpt/fetch.sh` makes a blobless, sparse clone into `third_party/wpt`,
pinned by `tools/wpt/REVISION` and scoped by `tools/wpt/directories.txt`.
Vendoring 1.5 million files that change daily would be a fork nobody maintains
and would drown every real diff. Pinning the revision is what makes two machines
agree; listing the directories is what keeps the clone to 600MB instead of
several gigabytes, and is the one place "is this area in scope?" is written down.

### 2. The server is ours, and it is not Python

`wpt serve` is Python — wptserve plus per-test `.py` handlers plus a certificate
story. Depending on it makes "the tests do not run" a question about pip, on
every machine and in every CI container, forever.

`tools/wpt/Server.cpp` is a single-threaded, non-blocking static file server
implementing the three things a checkout cannot be read without: `.sub.`
template substitution, generated tests (`foo.any.js` is not a test, it is the
source of `foo.any.html` and `foo.any.worker.html`), and `.headers` sidecars.

It deliberately does **not** implement the `.py` handlers. A handler is arbitrary
Python; approximating one makes a test pass for the wrong reason, which is worse
than a failure. A request for a `.py` path answers 501 and the test that made it
fails visibly. This costs us a large part of `fetch/` and it is the right trade
until somebody measures that those specific tests are what is blocking a page.

**Origins come from `*.localhost`, not from `/etc/hosts`.** WPT's own hostnames
(`web-platform.test`, `not-web-platform.test`) require a privileged edit to the
machine. glibc resolves every label under `.localhost` to loopback, and
`url::Host::IsLoopbackOrLocalhost` already treats the whole suffix as local — so
`www1.localhost:8001` is a resolvable, genuinely cross-origin origin that costs
no privilege and changes nothing on the machine. Two ports are bound, so a test
has a second origin on the same host as well as a second site.

### 3. Results come back through `EvaluateScript`, not WebDriver

Upstream's `testharnessreport.js` reports to `wptrunner` over WebDriver. This
browser has no WebDriver, and building one to run tests would be a second
remote-control surface built before the first page renders — a security surface
with no user behind it, which ADR 0004's model has nothing good to say about.

Ours leaves a line-oriented report on a global and the runner reads it through
`engine::Engine::EvaluateScript`, the seam `tools/snapshot` already uses. No new
IPC message, no new binding, and nothing in `src/` knows the tests exist.

The report format is tab-separated lines rather than JSON, because the parser is
in C++ and a hand-written JSON parser inside a test tool is a place for a bug to
live that nobody would ever look for.

### 4. One process per test

A from-scratch browser fails by hanging and by crashing at least as often as it
fails by answering wrong. When tests share a process, either failure takes the
whole run with it and the result is "the run died" instead of 4,000 results.

The runner forks a child per test, reads its report from a pipe, and kills it on
a wall-clock deadline. A crash becomes one `CRASH` line, a hang becomes one
`TIMEOUT` line, and the parallelism is free. The font catalogue is scanned once
in the parent and inherited, because scanning it per test would be most of the
run.

**Nothing in the harness runs a thread.** The server is forked before anything
else exists and serves from its own single-threaded process. The browser's
zero-idle-CPU, one-place-to-block invariant is not something a test harness gets
to opt out of: a harness that polls measures its own polling.

**The wall-clock deadline is testharness.js's deadline plus a grace, and the
grace is load-bearing** (added 2026-08-11, task C4). testharness.js gives a page
`--timeout` milliseconds and *then* reports: harness `TIMEOUT`, and every subtest
with the status it actually reached. The runner used the same number, so it
killed the page at the instant it began reporting and recorded "the page never
reported" instead — a different claim, and a false one. **86 of `dom/nodes`' 327
tests were in that state**, and one of them (`Comment-constructor.html`) had
eleven passing subtests behind a single `async_test` waiting on an iframe.
Five seconds of grace made 242 subtests visible in that one directory. The cost
is bounded and paid only by a test that really does hang.

The same session found the other half of it: **`--verbose` printed nothing when
a non-OK harness status was the expected result**, so the message naming the
first script error was computed, escaped, sent down the pipe and discarded. It
prints now. Both are the founding bug of this ADR in a new place — the browser
was fine and the reporting path was not.

**Run the harness under AddressSanitizer, not only the browser.** The same
session found a heap-buffer-overflow in `Server::Serve`: the `pollfd` array is
built from the connection list, then `Accept` appends to that list, and the loop
below indexed the array by the *new* list's positions — so every freshly
accepted connection was serviced against two bytes past the end of a heap
allocation. Whatever those bytes held decided whether the connection was read
from, written to, or closed. A test server that drops a request at random is
indistinguishable, from the outside, from a browser that failed to make one, and
this is the tool the whole project's correctness signal is measured with. It had
been there since the server was written and no amount of running the *browser*
under a sanitizer would have found it.

### 5. Expectations record failures, never passes

`tests/wpt/expectations/<area>.txt` lists only what does *not* pass. The default
for every test and every subtest is PASS.

That direction is the whole point. A file that only lists failures **shrinks** as
the browser improves, so the diff of a session that fixed something is a
deletion, and a file that grows is a regression somebody had to choose. The
alternative — listing every subtest with its status — produces a 40,000-line
file whose diffs nobody reads, which is the same as having no expectations.

A newly passing subtest is an unexpected result and fails the run, exactly like a
newly failing one. Silently accepting improvement is how an expectation file
stops describing anything.

**A harness status that is not `OK` subsumes the subtests.** A test that times
out half way through reports every remaining subtest as `NOTRUN`; recording
those records the consequence of one failure rather than a thousand independent
ones. The first baseline run wrote **188,172** `NOTRUN` lines into
`encoding.txt` before this rule existed — a 217,000-line file, which is exactly
the unreadable artefact the format exists to avoid.

### 6. What the harness cannot run, and what is decided about each

*Amended 2026-08-10 by plan task B3, from the first baseline run. Counts are
against the 42,185 tests in `tools/wpt/directories.txt`.*

Six things stop a test from producing a meaningful answer. Only two of them are
the browser's fault, which is the point of writing them down: a session that
does not know the difference spends itself implementing something the harness
was never going to ask for.

| gap | tests | decision |
|---|--:|---|
| `.py` handlers | 466 handler files | **Refused, unchanged (§2).** Plan task H1 owns revisiting it. |
| `testdriver.js` | 1,158 | **Undecided** — see below. |
| No https origin | 1,219 `.https.` | **Deferred to plan task H9.** They run, over http. |
| No XML parser | 97 testharness (7,257 reftests) | **Deferred**; it is a `src/html` feature, not a harness one. |
| Workers without `importScripts` | 1,554 | **Browser gap** (ADR 0022, plan task G5). |
| Reftests without a tolerance | 20,923 | **Excluded from the baseline** until plan task F2. |

**`testdriver.js` is the one that needs a decision and does not have one.** It is
how a test synthesises input — `test_driver.click`, `send_keys`,
`action_sequence`, `bless` — and upstream implements it over WebDriver, which
§3 refuses. The observed failure is `action_sequence() is not implemented by
testdriver-vendor.js`, and it is *not* a browser bug: this browser has an input
path and `microbrowser_snapshot -click` already drives it. The seam exists; what
does not exist is a way for a page to reach it. That is the same
remote-control-surface question §3 answered for results, and it wants the same
answer — a harness-only global, reachable through `EvaluateScript`, that is not
a binding and does not exist in a normal browser process. Nobody has written
that down, so it is a task rather than a decision.

**Reftests are excluded from the baseline rather than recorded as failing.**
Recording them costs the run about six hours — the reftest half projected a full
run at nine hours against ninety minutes for the testharness half, because a
reftest renders two pages — and it buys an expectation file that plan task F2
will rewrite in full. An exact-pixel comparison against a reference rendered by
the same rasterizer calls antialiasing noise a difference; a suite whose
failures are noise is one nobody reads. `ctest` already excludes them for the
same reason.

**The build the expectations came from is part of the expectations.** A page
that has not reported inside testharness.js's own ten seconds is a `TIMEOUT`
whatever the reason, and the Debug build is four to seven times slower than the
perf build on every page. The baseline is recorded from the perf preset, so
`tools/run-checks.sh wpt` runs the perf preset — and `ctest` in a Debug build
tree passes `--timeout-multiplier 6`, which scales the page's own deadline and
the runner's wall clock together and changes nothing else. Without it the
expectation files would record which compiler produced them.

The multiplier is a mitigation and not a proof. It reconciles the dominant
failure mode — a slower engine losing a race against a fixed deadline — and it
cannot reconcile a test that asserts on a wall-clock *duration*: giving
`user-timing/measure-l3.any.html` six times as long changed its answer, in the
direction of failing. Those tests are flaky on any build and belong in the
expectations as such. If the two builds ever have to agree exactly, the answer
is to record from both, not to tune a constant.

## Consequences

- **`ctest` gains a WPT run** over the committed expectations, sharded like the
  unit suite. It is skipped with a message, not a failure, when `third_party/wpt`
  is absent — a contributor without the checkout still gets a green build.
- **The snapshot tool's site-specific heuristics become removable.** Not
  immediately: they are how the reddit and youtube checks in
  `docs/roadmap-sessions.json` are run. But every one of them is a bet on a page
  that WPT makes unnecessary, and they should be deleted as the areas they
  cover gain coverage here.
- **Work becomes parallel.** A WPT area is a self-contained assignment with a
  measurable before and after, which is what lets several agents work at once
  without colliding. `docs/wpt-plan.md` is that decomposition.
- **The first bug it found was in the harness path itself.** testharness.js runs
  its completion callbacks in one loop with no `try`/`catch`, so a throw inside
  `show_results` eats every callback registered after it. `show_results` calls
  `insertAdjacentText`, which this browser does not implement, so the first run
  reported nothing at all from a page whose tests had already run. This is the
  argument for WPT in miniature: a missing four-line binding, invisible on every
  real page, that silently destroys a whole reporting path.
- **A number now exists.** "This browser passes N of M web-platform-tests" is
  the first statement about its correctness that is not a screenshot.

## Alternatives considered

- **Keep hand-writing tests in `tests/`.** They stay — they are how a deviation
  gets pinned once it is understood, and they run in milliseconds. What they
  cannot do is tell us about a feature nobody has thought to test, which is
  precisely the failure mode of a from-scratch implementation.
- **Use `wpt run` with wptrunner and a WebDriver server.** Rejected in §3: it
  builds an unauthenticated remote-control surface into the browser, and it
  makes the Python toolchain a hard dependency of running any test.
- **Vendor a curated subset of WPT into `tests/`.** Rejected: a curated subset is
  a fork, it stops tracking upstream immediately, and the curation would be done
  by the same people who decide what to implement — so it would omit exactly the
  things nobody thought of.

---

## Amendment, 2026-08-14 — §2 gains a closed list of transcribed handlers

**§2's refusal was right and its reason still stands.** It also named the
condition under which the trade changes: *"until somebody measures that those
specific tests are what is blocking."* The measurement now exists, and it is
larger than the ADR expected.

**The measurement.** `501 Python handlers are not implemented` is the top cause
in `xhr/` by a wide margin — one run of that area asked for
`xhr/resources/content.py` 117 times, `status.py` 110 times and `delay.py` 74
times — and `fetch/api/basic/` could not report a status code, a redirect or a
posted body without one. It is not a long tail: **ten files account for most of
it**, and the whole of `xhr/` and `fetch/` sits behind them.

**What changed, and what did not.** The server now implements a **named, closed
list** of handlers in `tools/wpt/Handlers.{h,cpp}`. Everything not on the list
still answers 501, and a test is asserted to that effect
(`WptHandlers/TheSetIsClosedAndEverythingElseIsStill501`). Two rules keep this on
the right side of §2's objection, and both are structural rather than intentions:

1. **Each handler is a transcription of one specific file, with that file's
   source quoted above it.** Not "something that behaves like a redirect": the
   same parameters, the same defaults, the same order of operations. A reviewer
   can put the two side by side. Where upstream's behaviour depends on something
   this server does not have, the handler is *absent* rather than guessed.
2. **Dispatch is on the repo-relative path.** Three different `redirect.py` files
   exist in the checkout with three different behaviours; a handler keyed by its
   basename would apply one of them to all three, which is precisely the "passes
   for the wrong reason" this section refuses.

The defaults are what a test asserts against and are therefore what a
transcription silently gets wrong: `status.py` answers the reason phrase `OMG`,
an absent request header is reported as the literal string `NO`, and
`slow.py` with no parameter waits two seconds. Each is pinned by a test.

**`?pipe=` lands with it, and it is the larger half.** Hundreds of tests ask for
a status or a header on an *ordinary static file* with `?pipe=status(404)` or
`?pipe=header(Content-Type,text/html)`. A server that ignored the query served
the file as itself — a **wrong** answer rather than a missing one, and therefore
worse than the 501 this section was protecting. `status`, `header`, `slice` and
`trickle` are implemented; an unrecognised stage changes nothing.

**One thing the transcription could not keep.** Upstream implements a slow
handler with `time.sleep`, under a server that runs a thread per connection.
This server is single-threaded by the same rule as the browser (§4: "nothing in
the harness runs a thread"), and six test processes talk to it at once — so a
sleep would stall the whole run and produce a cascade of timeouts that reads
exactly like a browser bug. A delayed response is **held** in its connection and
written on a later turn of the poll loop, and the loop's timeout accounts for it.
`trickle` collapses to its total delay with the body delivered whole, which is
named in the code as a deviation: a test asserting on progressive delivery fails,
which is the correct outcome for a browser with no incremental parse (ADR 0030).

**And the server now reads a request body.** It answered before reading one, so
the bytes stayed in the buffer to be parsed as the next request line. That was
invisible while every request was a GET.
