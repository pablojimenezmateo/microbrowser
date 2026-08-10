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

Five decisions make it work here.

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
