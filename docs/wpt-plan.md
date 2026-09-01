# The plan: from "renders Hacker News" to "runs the web"

**Owner:** nobody in particular. This document is the decomposition several
agents work from at once. · **Started:** 2026-08-10 · **State:**
`docs/wpt-tasks.json`

`docs/adr/0040-web-platform-tests.md` is the argument for the instrument. This
is the work.

Read this first if you are about to start a session. It supersedes the ordering
in `CLAUDE.md`'s "Where To Pick Up" and in `docs/roadmap-to-any-page.md`
wherever the three disagree — those two sequence work by *which page it
unblocks*, which was the right question when there was no other signal and is
now the second-best one.

---

## The target: the test files Firefox passes and we do not

100% WPT compliance is not a meaningful target — no shipping browser achieves
it. Firefox (the closest) runs ~96% aggregate; Chromium ~95%; Safari ~93%.
Each has areas where it fails deliberately, areas where the tests are written
against another engine's behaviour, and areas where neither engine nor test is
wrong but the specification is ambiguous.

**The target is the set of test files Firefox passes and this browser does
not.** A gap is a bug; a test Firefox also fails is not our problem (yet); a
test we refuse is a decision with a name. That much has been the plan since it
was written. What changed on **2026-08-17** is the *unit*, and the reason is
worth reading before quoting any number in this document.

### Why the unit is a test file and not a percentage

This document used to say "the target for each area is Firefox's pass rate on
the same tests", and `tools/wpt/firefox-ref.py` produced a table with a `gap`
column that subtracted our subtest percentage from Firefox's. **That subtraction
is not a measurement.** Three separate reasons, each verified against the
committed tables on 2026-08-17:

1. **The two rates have different denominators.** Ours counts the subtests we
   *reported*; Firefox's counts the subtests that *exist*. A test that dies
   before `done()` contributes zero subtests to our denominator and its full
   count to Firefox's — so the rate goes **up** as we fail worse, and it is
   least trustworthy exactly where we are least correct. Measured: Firefox
   reports **1,128,812 subtests that never enter our denominator at all**,
   against the 481,764 that do. The worked example is `url/`, which reads
   "us 97.9%, firefox 89.9%, ceiling **done**" — on 9,909 subtests where Firefox
   has 15,420. We do not beat Firefox at URLs; we decline to report a third of
   the question and then take the average of what is left.

2. **The rate cannot be ranked.** `gap %` puts `FileAPI/FileReader` (2 subtests)
   and `encoding/legacy-mb-japanese` (447,722) in joint first place, both at
   `100.0`. A plan sorted by that column is sorted by nothing.

3. **Subtest counts are not a measure of work either.** Ranking by *absolute*
   subtests-behind-Firefox is no better: it puts the three
   `encoding/legacy-mb-*` generated tables — 1.1M subtests across a handful of
   files — above every layout, DOM and script area combined.

A **test file** has none of those problems. It counts as passed only when every
subtest in it passed, which is the same rule on both sides and cannot be
inflated by a harness that stopped early. It is stricter than a subtest rate by
construction, and that is the point.

```bash
python3 tools/wpt/firefox-gap.py --cache /tmp/firefox-wpt-summary.json
# -> docs/wpt-firefox-gap.md          the ranked gap, per area
python3 tools/wpt/firefox-gap.py --list-gap css/selectors
# -> the actual file names, each tagged `blocked` or `feature`
```

23,069 of our 23,146 in-scope testharness files are present in Firefox's run, so
the join is all but complete. `docs/wpt-firefox-ceiling.md` stays, because a
subtest rate is still the right way to watch *one* area move between two runs of
*this* browser. It is not the right way to compare two browsers, and its `gap`
column should not be read as one.

### Where that leaves us — 2026-08-17, against Firefox 156.0a1

| | testharness | reftest | total |
|---|--:|--:|--:|
| in our scope | 23,146 | 20,998 | 44,144 |
| Firefox passes | 19,210 | 17,961 | 37,171 |
| **we pass** | **4,295** | **6,605** | **10,900** |
| **Firefox passes, we fail** | **14,915** | **11,356** | **26,271** |
| both fail | 3,494 | 2,236 | 5,730 |
| we pass, Firefox fails | 365 | 784 | 1,149 |

**29.3% of what Firefox passes**, where the baseline's aggregate subtest rate
reads 22.3% and individual areas read 76.1%, 90.2% and 99.8%. Both numbers are
arithmetically correct. Only one of them is a comparison.

**The reftest column is a measurement as of task F9, 2026-08-17, and the day
before it read zero.** The whole suite was recorded in one run of **105
seconds** — §M-B below projected six hours, which was true before the
`WaitOnDescriptors` spin fix of the same week and is quoted here so nobody
re-projects from it. Nothing was built for it: F2 had made the comparison
honest, `RunReftest` and `--update-expectations` were already there, and what
remained was to run them and read the result. The 11.6% this section used to
quote was the same browser with 48% of the suite counted as failing, which was
the only honest reading available while nothing had ever written a reftest
result down.

Three things the rate was hiding, all of them actionable:

- **6,152 of the 14,915 are files where our harness never reported at all** —
  TIMEOUT, ERROR or CRASH before `done()`. Not one of their subtests is in any
  denominator anywhere, in either direction. The `blocked` column of
  `docs/wpt-firefox-gap.md` is this, per area, and it is the column to read
  first: a blocked file is plumbing, and plumbing is cheaper than a
  specification.
- **7,394 of the 20,998 reftests pass, and 757 of those are two blank pages
  agreeing.** A reference that fails to load renders white, so it matches any
  test that also rendered nothing, and the comparison proves nothing. The runner
  counts them beside the passes rather than deducting them — wptrunner compares
  screenshots without asking what is on them, and a rule of our own would make
  the two sides incomparable, which is the entire point of this document. Some
  are real: a reftest whose point is that an element is not visible passes blank
  in every engine. `microbrowser_wpt --reftests-only` closes with the number.
- **1,149 files are recorded as passing that Firefox fails.** An expectation file
  cannot tell "passes" from "never run", so each of these is either a real
  divergence worth a comment or a test nobody has run. `html/editing` has 90 —
  and 784 of the 1,149 are reftests, where the blank-pair pass above is the
  first thing to suspect.

### The ranking that follows from it

The top of `docs/wpt-firefox-gap.md`, by files Firefox passes and we do not.
`blocked` and `feature` are the testharness halves; `reftest` has neither
distinction, because a reftest agrees with its reference or it does not:

| area | gap | blocked | feature | reftest | note |
|---|--:|--:|--:|--:|---|
| `css/CSS2` | 3,907 | 7 | 33 | 3,867 | task E1, and it was **40 files** before F9 |
| `html/canvas` | 2,715 | 976 | 1,678 | 61 | tasks F6 and F6b |
| `html/semantics` | 1,638 | 510 | 1,006 | 122 | milestone M-O |
| `css/css-grid` | 1,126 | 415 | 96 | 615 | task E3 |
| `css/css-text` | 1,095 | 102 | 179 | 814 | task E8 |
| `css/css-writing-modes` | 1,039 | 10 | 50 | 979 | task E9 |
| `css/css-flexbox` | 952 | 172 | 78 | 702 | task E2 |
| `referrer-policy/gen` | 951 | 865 | 86 | 0 | task H8 is "record the deviations" |
| `css/css-transforms` | 591 | 1 | 92 | 498 | task F5 |
| `css/css-backgrounds` | 553 | 1 | 96 | 456 | task F1 |

**Seven of the ten are layout, and six of the ten are more than half reftest.**
That is what the gate was for. Before F9 this table's top ten were
`html/canvas`, `html/semantics`, `referrer-policy/gen`, `html/browsers`,
`css/css-grid`, `websockets`, `xhr`, `css/css-text`, `html/webappapis` and
`svg/animations` — a list with one layout area in it.

Against that, the five areas the last five sessions worked — `css/selectors`
(242), `css/cssom-view`, `css/cssom`, `css/css-syntax`, `encoding` — are still
a few hundred files between them, which is under a tenth of `css/CSS2` alone.
Those sessions were not wasted; every one of them fixed real bugs, and the
commit messages are honest about what moved. But they were **chosen by a column
that cannot rank**, and the ranking they were chosen by is the one this section
replaces.

Each task in `docs/wpt-tasks.json` now carries `firefox_gap` — `files`,
`blocked` and `feature` from this measurement — alongside the older
`firefox_ceiling`. **Rank by `firefox_gap.files`.** The `target` percentage
stays as a within-area planning number and is no longer a goal in itself: a task
is finished when its files move, and a session that raised a rate without moving
files has to say which files it moved and why the rate is the better story.

### Refusal policy

Some WPT areas test capabilities this browser has explicitly decided not to
implement. These decisions are documented in ADRs and are not bugs to chase.

**Partial refusals** (some tests in an area are blocked by ADR decisions):

| area | ADR | what is refused |
|---|---|---|
| `html/` | 0011, 0012, 0026 | `document.write` (tokenizer re-entry) |
| `html/browsers/` | 0026 §6 | `window.open` returns null, `opener` absent |
| `workers/` | 0022 §1 | `SharedWorker` refused (DedicatedWorker implemented) |
| `content-security-policy/` | 0020 §3 | `report-uri`/`report-to`/`Report-Only` not implemented |
| `css/selectors/` | 0012, 0016, 0033 | `:visited` matches nothing (privacy) |
| `streams/` | 0020 §1 | `new ReadableStream({start})` illegal constructor |
| `IndexedDB/` | 0021, 0038 | memory tier only, no disk persistence by default |
| `websockets/` | — | server does not speak WebSocket protocol (infrastructure) |
| `domparsing/` | — | `DOMParser` absent (second Document problem) |

**Full refusals** (entire areas out of scope, not in `directories.txt`):
`webrtc/` (ADR 0029), `geolocation/` (ADR 0029), `webaudio/` (ADR 0028),
`encrypted-media/` (ADR 0028), `service-workers/` (ADR 0022),
`push-api/` (ADR 0022), `webgl/` (ADR 0029).

The complete mapping is `docs/wpt-refusals.tsv`. When a test fails because of
a refusal, the expectation line carries a comment naming the ADR — this is
rule 1 in §0, not a new rule.

### Direct runners for data-table areas

Three areas have their test data as pinned tables rather than browser tests:

| runner | area | what it exercises | vectors |
|---|---|---|---|
| `microbrowser_urlconf` | `url/` | `src/url` parser/setters/IDNA | 3,900+ |
| `microbrowser_mimeconf` | `mimesniff/` | `src/util` MIME parse/serialize | ~300 |
| `microbrowser_encconf` | `encoding/` | `src/html` label resolution, decode, encode | 3,800+ |
| `microbrowser_cssconf` | `css/` | `src/css` tokenizer, supports, selectors | 216 |

Each runs in under a second against the module directly, names the exact field
or byte that differed, and is a developer tool (not a `ctest` target). Build
a runner before working an area that qualifies — the URL area went 37% → 98%
in one session because of it.

### Handler demand report

The WPT server now tracks every unhandled `.py` handler request by path. At
server shutdown it prints a ranked table of which handlers were actually
requested and how often. This replaces the `MICROBROWSER_WPT_HANDLER_REPORT=1`
that was lost in the 2026-08-15 merge and is what stops handler prioritization
from being guesswork.

---

## 0. How this is meant to be worked

### The unit of work is a WPT area, not a feature

A task is "raise `css/css-flexbox/` from 31% to 80%". That phrasing is doing
several things at once:

- It has a **number before and after**, so a session that did nothing is
  visible, and so is a session that fixed the wrong thing.
- It **fits one agent's context**. An area is a few hundred tests with a few
  dozen distinct causes behind them.
- It is **disjoint from every other task**, so agents do not collide. Two agents
  in `src/layout` at once is a merge conflict; two agents, one in flexbox and
  one in `dom/nodes/`, is not.
- It **does not decide the fix in advance**. Almost every area's failures turn
  out to be three or four causes with a long tail, and which three is not
  knowable until the tests are run. A task that named the fix would be a guess.

### The loop, for one task

`/next-wpt-task` is this section as a command: it picks by rank, claims, checks
the gate and the dependencies, and stops after one task. `tools/agent-loop.sh -c
/next-wpt-task -n 5` runs five of them, each in a fresh process. **`/next-session`
is a different command reading a different ledger** (`docs/roadmap-sessions.json`,
sequenced by which page it unblocks); the two disagree on purpose and this
document supersedes it, as §0 says.

```bash
tools/wpt/fetch.sh                                  # once per machine
./build/microbrowser/microbrowser_wpt --list css/css-flexbox/ | wc -l
./build/microbrowser/microbrowser_wpt css/css-flexbox/       # see where you are
./build/microbrowser/microbrowser_wpt --verbose css/css-flexbox/align-content-001.html
# ... fix ...
./build/microbrowser/microbrowser_wpt --update-expectations css/css-flexbox/
git diff tests/wpt/expectations/                    # this diff is the session
```

The expectation diff **is** the deliverable. A session whose expectation diff is
all deletions did the work; one that adds lines had better say why in the commit
message.

### Six rules that are not negotiable

1. **A failing test is a question, not a task.** Read the test, decide what the
   specification says, then decide whether this browser is wrong. WPT contains
   tests for things this project has deliberately refused (`docs/adr/0012`,
   `0029`, `0033`). A refusal gets an expectation line **and** a comment saying
   which ADR it comes from. Making a refusal pass by implementing what it
   refuses is a change to an ADR, which is a separate commit and a separate
   argument.
2. **Fix the cause, not the test.** Ten tests failing on one missing method is
   one fix. If a change makes exactly one test pass, suspect it.
3. **No new copy of anything.** Every area below has an existing owner module;
   the fix belongs there, behind that module's `MODULE.deps` contract. A helper
   that two modules want goes in the lower one. `tools/budget-report.sh` before
   a refactor.
4. **A parser or decoder lands with its fuzz target on the same commit.**
   Unchanged from `guidelines/security.md`. WPT does not test hostile input and
   never will.
5. **Measure before optimising.** `docs/tech-debt.md` first, then
   `MICROBROWSER_PERF_SUMMARY=1`, then a benchmark in `bench/`. A page-load
   timing on a shared machine is not a measurement.
6. **The check must have been run.** `status: done` in `docs/wpt-tasks.json`
   means the number in `target` was reached and the run that reached it is in
   the commit message. Nothing else may set it.

### Claiming a task

`docs/wpt-tasks.json` is the state. Set `status: "in_progress"`, `agent` to
something identifying, and `claimed` to today's date; commit *that alone*, and
push before starting. A conflict on that one-line commit is the cheapest
possible collision.

**And releasing one, which had no rule until 2026-08-17.** A session that ends
without finishing leaves its task `in_progress` forever, and the next agent —
correctly following the paragraph above — skips it. Seven tasks were in that
state on 2026-08-17, including J1b (549 files, #6 in the order) and G5 (which
gates F6b's 1,921). Four of them had no claim commit at all, so there was not
even a date to judge them by.

**A claim is stale when no commit has touched its area since the claim, or when
the claim is more than three days old and the task is not the one you are
currently reading a commit from.** A stale claim may be re-taken. Say so in the
claiming commit — "re-taking D4, claimed 2026-08-16, last commit in
`css/cssom-view` 2026-08-16, target not reached" — so the original agent, if it
does come back, can see what happened rather than hitting a conflict.

`claimed` is now a field. An `in_progress` task without one is stale by
definition: nobody recorded when it started, so nobody can tell whether it is.

---

## 0.5 What the baseline found · **2026-08-10, M-B done**

`docs/wpt-baseline.md` is the measurement and is generated; this is what it
changed about *the plan*. All 21,265 testharness tests in the checkout now have
a committed expectation.

**Five causes account for more of the suite than every layout bug combined.**
Ranked by distinct tests, out of 21,265:

| cause | tests | where it belongs |
|---|--:|---|
| `importScripts` is not defined | 1,380 | G5, and it is 18% of all 7,718 timeouts |
| `NOTRUN` behind one earlier failure | 1,603 | mostly `IndexedDB` — I2 |
| `OffscreenCanvas is not defined` | 889 | F6 |
| `Python handlers are not implemented` | 512 | H1, exactly as ADR 0040 §2 predicted |
| `assert_throws_js: … is not an Error subtype` | 117 | **C2** |
| `document.elementsFromPoint unsupported` | 131 | D4 |
| `action_sequence() is not implemented` | 147 | **B5** |

C1 and C2 are confirmed as the right first tasks, but not for the reason the
plan gave: the exception-identity signatures are everywhere, and they are
*behind* the five above in raw test count. Do them first anyway — they are
cheap, and every area's negative tests are gated on them.

### What C1 and C2 turned out to be · **done 2026-08-10**

Both landed, and neither was what this section predicted.

- **C2 was one pointer.** `assert_throws_js` does not use `instanceof`: it walks
  `Object.getPrototypeOf` up from the *constructor* looking for a function named
  `Error`. The NativeError constructors inherited from `Function.prototype`
  rather than from `Error`, so that walk never terminated in a match. 117 tests
  and 2,014 subtests, one line.
- **C1 was not the missing type.** `DOMException` existed. What did not was any
  binding actually throwing one: fourteen sites raised a plain `Error` with the
  DOM name inside the *message text* (`"InvalidStateError: …"`), which reads
  right to a human and answers `e.name === "Error"` to a page. The audit is the
  task; the type was the easy half. The DOM's ensure-pre-insertion-validity
  landed with it, because a type nothing throws cannot be measured.

Re-measured, per-area subtest pass rate, before → after: `dom/nodes` 22.4 → 23.4,
`dom/abort` 16.2 → 27.0, `dom/ranges` 7.4 → 8.7, `dom/traversal` 52.7 → 54.5,
`dom/events` 28.3 → 29.1, `FileAPI/url` 30.0 → 53.8, `domparsing` 24.8 → 31.4,
`domparsing/tentative` 0.7 → 3.1, `custom-elements` 17.4 → 19.1,
`custom-elements/upgrading` 6.2 → 14.3, `IndexedDB` 4.1 → 4.7, `xhr` 6.4 → 6.6.

**And a harness bug that made all of this unmeasurable, which is W1's whole
argument.** A subtest name ending in a space — what `test(function(){…})` with
no name gets from a `<title>` written with spaces inside its tags — could not
survive being written to an expectation file and read back, because the loader
trimmed the line. The runner then reported `FAIL (expected PASS)` beside
`MISSING (expected FAIL)` for the same subtest, deterministically, against the
binary that recorded it. Twelve of `dom/`'s were in that state. That is part of
the "~3% flake" the M-B session left as its first task, and it is *not* the
timeout half: a rate was two causes.

**Targets that the measurement says are wrong.** The plan set these before
anything had been run; each needs revising with a reason, which is what §2 says
to do. The gap is not uniform — three of these are a missing binding rather
than a partial implementation:

| task | target | measured | why the gap is what it is |
|---|--:|--:|---|
| I2 `IndexedDB` | 55% | 4.2% | 200,592 `NOTRUN` subtests behind a first failure |
| G5 `workers` | 60% | 1.8% | `importScripts`; 200 of 284 tests time out |
| H4 `fetch/api` | 60% | 7.4% | but `fetch/corb` is 86.8% — the `.py` gap is not uniform |
| F6 `html/canvas` | 60% | 12.2% | `OffscreenCanvas`, 889 tests |
| D5 `css/selectors` | 80% | 24.4% | |
| E2 `css-flexbox` | 80% | 15.1% | **183 of 369 tests time out** — investigate that before layout |
| D6 `css-conditional` | 70% | **88.2%** | already past it; 184 harness ERRORs are container queries |

**Three areas are much better than anyone claimed.** `html/rendering` 85.8%
over 7,169 subtests, `webstorage` 92.5%, `navigation-timing` 64.9%. The UA
stylesheet and presentational-attribute work is genuinely done.

**Three assumptions in this document are now known to be false.**

- **H2 assumed `url/` would "reach very high" because it is a JSON data file
  needing no server.** It is 21.7%, and **32 of its 71 tests time out**. A
  timeout in a data-driven test is not a URL-parser failure; look at the harness
  path before touching `src/url`.
- **F7 assumed `svg/` could not be measured because this browser renders SVG as
  an image.** `svg/animations` runs 251 of its 289 tests to completion and
  passes 1.4%. The suite runs fine; the API is absent. F7 is an implementation
  task, not a scoping ADR.
- **H8 assumed `upgrade-insecure-requests` would need a deviation argument.**
  All 196 tests run to completion and pass 0 of 992 subtests, with no harness
  failure at all — it is a clean "not implemented", which is the easiest
  possible thing to record.

### 0.6 What 2026-08-14 changed about §0.5's table

Three of the five causes above are **gone**, and all three had the same shape: a
capability this browser already had with nothing exposing it to the test. None
was a specification gap and none would have been found by reading the failures.

| cause | tests | what it actually was |
|---|--:|---|
| `importScripts` is not defined | 1,380 | a worker had **no global scope at all** — G5 |
| `not implemented by testdriver-vendor.js` | 1,158 | the input path existed; nothing exposed it — B5 |
| `Python handlers are not implemented` | 512 | ADR 0040 §2's condition was met — H1 |

**The rule worth taking from it:** when an area is at single-digit percent, check
whether the harness can *reach* the feature at all before reading the failures as
a specification gap. `workers/` was at 1.8% with a complete, tested,
thread-and-heap worker implementation behind it.

Two of §0.5's "targets the measurement says are wrong" are answered by that:

- **G5 `workers` 60% / measured 1.8%** — the diagnosis in that row ("`importScripts`")
  named a symptom. On a 400-file sample of the suite's worker variants, 20 of
  13,742 subtests passing became 4,215 of 18,000.
- **H4 `fetch/api` 60% / measured 7.4%** — the `.py` gap was the whole of it in
  `fetch/api/basic/`, which is now 237 of 462 (51.3%).

**And one thing measured here that changes what to do next:** `websockets/` (532
tests) is blocked on the **server**, not the browser. Its `.any.html` window
variants time out as well, because this server speaks no WebSocket. It is an
H1-shaped problem, and implementing `WebSocket` in a worker to chase it would
gain nothing.

## 1. Where this browser actually is

Complete or near-complete, per `CLAUDE.md` and the session log: HTML parsing and
fragment parsing, the CSS cascade with custom properties and `calc()`, block and
inline layout, floats, tables, flexbox, positioning, the JavaScript language
(bytecode VM, modules, async, generators, `Proxy`, regex), the DOM bindings
including shadow DOM and custom elements, `fetch`/XHR/CORS/CSP/SRI, HTTP/1.1 and
HTTP/2 with a connection pool, cookies, storage including IndexedDB, workers,
transforms and stacking contexts, `@font-face` with WOFF2, encodings, UAX #9
bidi and UAX #14 line breaking, canvas 2D, animations, media containers and MSE.

Not started or in flight: grid (in progress), same-origin and cross-origin
iframes, the process split and sandbox, incremental parsing and first paint,
`vertical-align`, `min()`/`max()`/`clamp()`, viewport units, `:has()`.

**Read that list against the file-level measurement before trusting it.** Half
of those entries are "complete or near-complete" on the strength of a subtest
rate, and a subtest rate is not a claim about correctness — `css/css-flexbox` is
listed as complete and passes 95 of the 343 test files Firefox passes; canvas 2D
is listed as complete and passes 334 of 2,988. The list is true about what has
been *built*; it is not evidence about what works, and it has been read as both.

That is a large browser with no idea how correct it is. The point of what
follows is to find out and then to move the number.

---

## 2. Milestones

Each milestone has **entry criteria** (what must be true to start), **tasks**
(each one an agent-session, parallel unless a dependency is named), and **exit
criteria** (a number, run, and recorded).

Percentages are *subtests passing* over *subtests run* in that WPT path, as
`microbrowser_wpt` reports them. They are targets for planning, not contracts:
an area that turns out to be 40% specification-refusals gets its target revised
in this document, with the reason.

**And they are not comparable to another browser, or to each other** — the
denominator is subtests this browser *reported*, so it shrinks when a test dies
early. Read a percentage as "did this area move between two runs of this
binary", never as "how far behind Firefox is this area". For the second question
the unit is a test file and the document is `docs/wpt-firefox-gap.md`; see §The
target above for why.

### The order · **re-ranked 2026-08-17, after gate 0**

**The milestones below are lettered in the order they were written, not the
order to do them in.** They used to be read as a sequence, which is how the last
five sessions came to spend four on M-D and one on M-K — **the 5th and 9th**
largest gaps in the tree — while the 1st and 2nd have had no session at all. The
letters stay, because they are referenced from the ledger, the session log and
half the ADRs. The order is this table, and it is `milestones[].order` in
`docs/wpt-tasks.json`, **written by `firefox-gap.py --annotate-tasks` rather
than by hand** as of F9 — it was hand-maintained before, which is one of the two
places the previous ordering came from.

| # | milestone | gap | blocked | feature | reftest | why here |
|--:|---|--:|--:|--:|--:|---|
| 1 | M-E layout | **9,285** | 773 | 710 | **7,802** | E1 (`css/CSS2/`) alone is 3,907 |
| 2 | M-F paint, colour and graphics | 5,281 | 1,226 | 2,301 | 1,754 | F6b alone is 1,939 |
| 3 | M-H the network and the security around it | 3,023 | **1,981** | 1,038 | 4 | two thirds of it is plumbing |
| 4 | M-O HTML's own elements | 1,433 | 564 | 811 | 58 | added 2026-08-17 |
| 5 | M-D CSS: cascade, values, object model | 974 | 212 | 481 | 281 | four of the last five sessions |
| 6 | M-G script, the event loop, and timing | 812 | 297 | 507 | 8 | |
| 7 | M-C the DOM and its bindings | 711 | 114 | 496 | 101 | C6 in progress |
| 8 | M-J navigation, browsing contexts, process split | 552 | 298 | 252 | 2 | gates M-O's largest task |
| 9 | M-K text and internationalisation | 367 | 41 | 129 | 197 | `encoding/` is done; this is the tail |
| 10 | M-I storage | 341 | 83 | 257 | 1 | |
| 11 | M-L media | 316 | 142 | 173 | 1 | |
| 12 | M-M speed, memory, idle CPU | — | — | — | — | a milestone of measurements, not gaps |
| 13 | M-N the acceptance sites | — | — | — | — | gates, not tasks |

**Gate 0 is done except B6, and it changed the answer.** F2 (fuzzy reftests)
landed on 2026-08-17 and F9 (reftests recorded at all) the same day. This table
said, before them, that M-E's rank of 3 was "a floor, not a measurement, and it
is entirely possible that layout is first". **Layout is first, by nearly a
factor of two over M-F**, and it was ranked 3rd of 11 by a column that could not
see 7,802 of its files. Six milestones moved. B6 (a committed summary state) is
what is left of the gate and it is a machine run, not a ranking input.

**Read the `blocked` and `reftest` columns against the gap column before picking
inside a milestone.** They are three different kinds of work. A blocked file is
one whose harness never reported: plumbing, and usually cheaper per file than a
specification. A reftest file is a picture that disagrees, and
`--reftest-artifacts DIR` is how to read one — the first three images that flag
ever produced named a paint bug in one second where the pixel count said only
"different". The three extremes in the tree: `html/canvas/element/` is 774 files
with **19** blocked, pure feature work with nothing gating it; `referrer-policy/`
is 1,330 files with **1,083** blocked, where writing feature code today would
move nothing at all; and `css/CSS2/` is 3,907 files of which **3,867 are
reftests**, an area that until 2026-08-17 read as forty files of ordinary bugs.

### Three prerequisites that multiply, and were not written down as such

Ranking by files made these visible; each gates far more than its own task.

- **H9, an https origin in the harness. ~~612 gap files.~~ Done 2026-08-18**,
  and the file count was low: **1,268 files in scope** carry `.https.` in the
  name, not 612 — that figure counted only the Firefox gap, which is the right
  number for ranking a task and the wrong one for describing what it unblocks.
  It was filed as a harness chore with no target and, having no `area`, carried
  no `firefox_gap` and sorted as *zero* in a ranking by files. That is what
  `firefox_gap_unblocks` is for, and this task is the case that justifies it.

  **Two findings worth carrying into the next prerequisite.** A missing origin
  does not only cost the tests named after it: two of the three tests
  `fetch/metadata/` gained are plain-http files, because `helper.sub.js` builds
  *every* origin it uses from `{{ports[https][0]}}`. And a reachable origin is
  not a passing test — `upgrade-insecure-requests/` is 197 files and 1,000
  subtests and is unchanged at **zero** passing, because those tests were
  reachable-but-wrong before and are reachable-and-honest now.

  **The expectations are not re-recorded.** Twenty areas can have moved; they
  are listed in `docs/session-log.md`'s H9 entry, headed by `html/` (514
  `.https.` files), `content-security-policy/` (226),
  `upgrade-insecure-requests/` (197) and `fetch/` (177).
- **J1b, same-origin iframes** (in progress). Gates O3 (520 files, the largest
  task in M-O), H8's generated tests, J2 and G6.
- **A2, the remaining `.py` handlers** (in progress). Gates the
  `common/security-features/subresource/*.py` half of H8.

**H8 now depends on all three** and did not depend on anything before. It is the
second-largest task in the tree and it is *not* startable: 1,083 blocked files
do not become 1,083 expectation comments.

---

### M-A — The instrument · **done 2026-08-10**

`tools/wpt/`, `tests/wpt/expectations/`, ADR 0040. The runner, the server, the
manifest scanner, the expectation format, per-test process isolation, `ctest`
integration.

**Exit:** `./build/microbrowser/microbrowser_wpt dom/nodes/Node-appendChild.html`
reports per-subtest results. Reached.

---

### M-B — The baseline · **done, except B6 — which is in gate 0**

One full run of every checked-out area, recorded, with the failures grouped by
*cause* rather than by test. This is the only milestone that must happen before
the others, and it is one long machine run plus one session of reading.

**B6 is still open and it is part of gate 0** (§2, The order): without a
committed summary state, a per-area re-measurement cannot regenerate
`docs/wpt-baseline.md` and every re-record is hand-merged into it. Three
sessions in a row have tripped over that. And the milestone's own premise needs
an amendment the 2026-08-17 measurement forced: **this baseline covers the
testharness half of the suite and says nothing about the other 20,998 files.**
Completing it is F9, in M-F, and it gates the ranking of everything below.

| id | task | parallel with | output |
|---|---|---|---|
| B1 | Full run, all areas, expectations committed | — | `tests/wpt/expectations/*.txt` |
| B2 | Triage: group failures by cause, rank by tests-unblocked | B1 | `docs/wpt-baseline.md` |
| B3 | Harness gaps: which areas are unrunnable and why (`.py` handlers, `testdriver.js`, https) | B1 | ADR 0040 amendment |
| B4 | Per-area pass-rate table, committed, regenerable | B1 | `docs/wpt-baseline.md` |
| B5 | `testdriver.js`: decide how a test synthesises input | B3 | ADR 0040 amendment |
| B6 | One full run writing `--summary-state` into the repository | B1 | `tests/wpt/summary-state.tsv` |

**Why B2 is a whole session.** The first run of `dom/` produced 132 unexpected
results in the first 150 tests, and the interesting quantity is not that number
— it is that four messages account for most of them (`cannot read property
'document' of undefined`, `is not an Error subtype`, `Illegal constructor`,
`did not throw`). Each is one fix worth dozens of tests. A ranked cause list is
what turns 20,000 failures into 40 sessions.

**Reftests are out of `docs/wpt-baseline.md` and are in the measurement**, and
the two are no longer the same statement. F9 recorded all 20,998 into
`tests/wpt/expectations/` on 2026-08-17 and `docs/wpt-firefox-gap.md` counts
them; this document — the subtest table — still does not, and that is a
decision rather than an omission. **A reftest has no subtests.** Putting one in
a table whose columns are "subtests reported" and "subtests passed" adds a row
of 0/0 and moves nothing, and the question a reftest answers is a file-level one,
which is the other document's whole unit. **`ctest` runs them as of 2026-08-31**
(task F10): the eight that were intermittent were nine, and seven of them were
one bug in the browser rather than anything about the suite --
`SystemFontProvider` remembered what a font stack resolved to and never forgot
it when an `@font-face` arrived, so whether a web font applied at all was a race
between the fetch and the first layout. `--reftests-only` is 0 unexpected on two
consecutive runs and `--testharness-only` is gone from the registration.

**And the cost estimate this paragraph used to carry was wrong by two orders of
magnitude**, which is worth more than the decision it justified. It said "about
six hours… the full suite projected at nine hours". The measured figure is
**105 seconds** for all 20,998, in the perf build at the default `--jobs`. The
projection was made before `platform::WaitOnDescriptors` was found to ignore its
own timeout when it had nothing to watch — a page that has finished loading and
is waiting on a timer span a core flat out, which is most of what a reftest
does. **A projection is not a measurement**, and this one deferred 48% of the
suite for a week.

**Why B6 exists, and it is not tidiness.** `--summary` writes
`docs/wpt-baseline.md` from `--summary-state` alone, and that state file has
lived in `/tmp`. Three sessions running have now regenerated the document from
a state describing only the areas they ran, producing a correctly formatted,
complete-looking table with a fifth of the rows — twice caught by a reader, once
not. `SummaryAccumulator::Write` refuses to write fewer rows than the document
already has, which turns the silent truncation into a message; B6 is the one
long machine run that makes the refusal unnecessary.

**B6 is half done as of 2026-08-17: the state file is committed at
`tests/wpt/summary-state.tsv` and covers 134 of the 297 areas.** Finish it with
`tools/wpt/baseline.sh`, which shards the suite, skips what the state already
covers — derived from the state file itself, so another session or another
machine can pick it up — and stops rather than writing a state with a hole in it.
**Two bugs in the mechanism had to be fixed first, and both were invisible from
outside:** `--summary-state` without `--summary` fed the accumulator nothing, so
a run using the flag exactly as this task intends measured everything and saved
none of it; and the row-count guard counted the hand-merged rows in the
document's preamble, so it read 301 against a 297-row table and would have
refused a complete run. `tests/WptSummaryTests.cpp` covers both.

**And the cost is measured now: five to six hours, all of it timeouts.** 6,934 of
the 23,146 testharness files are expected to TIMEOUT and 2,807 carry a
`timeout=long` meta, which is a 65-second budget each — ~2.75s of wall clock per
expected timeout at the default `--jobs`. `content-security-policy/` is 1,637s by
itself; `dom/` is 692 tests in 214s. Unlike §M-B's reftest projection this one is
not a bug in disguise, so **do not close it with `--jobs`**: `tools/wpt/main.cpp`
carries the measurement showing oversubscription deletes subtests from CPU-bound
areas and makes the pass rate rise as the run gets worse.

**Exit:** every area has a committed expectation file for its testharness tests
and a line in the table; `ctest` is green against them.

---

### M-C — The DOM and its bindings · **#7 in the order**

The largest single lever: `dom/`, `domparsing/`, `shadow-dom/`,
`custom-elements/`, `html/dom/`. Almost everything else in the suite depends on
these being right, and the first run says they are not.

Known causes already visible from 150 tests:

- **Exceptions are not `DOMException`s and not subtypes of `Error`.**
  `assert_throws_dom` needs `e instanceof DOMException`, `e.code`, `e.name`.
  `assert_throws_js` needs the thrown value to be an instance of the *page's*
  `TypeError`. This single cause is worth a four-figure number of subtests
  across every area of the suite.
- **`Document` is not fully receiver-based.** "cannot read property 'document'
  of undefined" is the `ownerDocument`/`defaultView` chain.
- **Interface constructors are not callable/newable per WebIDL** ("Illegal
  constructor: HTMLScriptElement" is correct for `new HTMLScriptElement()`, and
  the test is checking something else near it).
- **`insertAdjacentText` is missing** (ADR 0040 §Consequences).

| id | task | depends on | target |
|---|---|---|---|
| C1 | `DOMException`: real type, `name`/`code`/`message`, thrown by every binding that should throw one; audit every `Throw` in `src/bindings` | — | `assert_throws_dom` works |
| C2 | Native error types are the page's own: `TypeError`/`RangeError` from bindings are instances of the page's constructors | — | `assert_throws_js` works |
| C3 | WebIDL argument conversion: `undefined`/`null`/missing/extra-argument behaviour, per-type coercion, `[EnforceRange]` where the spec says so | C1, C2 | `dom/` +10% |
| C4 | `dom/nodes/` — mutation algorithms, `Node` comparison, adoption, `ownerDocument` | C1 | 85% |
| C5 | `dom/traversal/`, `dom/ranges/` — the content-mutation half of `Range` (ADR 0012 lists it absent) | C1 | 80% |
| C6 | `dom/events/` — the full dispatch algorithm, `composedPath`, retargeting, passive/once/capture, `Event` constructors | C1 | 85% |
| C7 | `dom/abort/`, `dom/observable/` triage — implement or refuse with an ADR line | C1 | recorded |
| C8 | `shadow-dom/` + `custom-elements/` — reactions, `adoptedCallback`, `:host`/`::slotted`, slot assignment | C4, C6 | 75% |
| C9 | `domparsing/` — `DOMParser`, `XMLSerializer`, `insertAdjacent*` | C1 | 80% |
| C10 | `html/dom/` — reflection: every reflected IDL attribute, `Element` interface mixins | C3 | 70% |
| C11 | event handler **content** attributes: `<div onclick="…">` and `setAttribute("onclick", …)` compile a handler | C6 | 200 subtests |

**C11 landed on 2026-08-12, and its security half is the part to read before
touching it.** Nothing in this browser used to compile an `on*`
*attribute*: `RunListenersOn` read the wrapper's `on<type>` **property**, so
`el.onclick = fn` worked and markup did not. That was 47 subtests in
`dom/events/Body-FrameSet-Event-Handlers.html`, the tail of
`dom/nodes/remove-unscopable.html`, and a share of the 121 in
`Event-dispatch-single-activation-behavior.html`, which drives every activation
through `oninput`/`onsubmit`/`onreset`/`ontoggle` written in markup.

It compiles **lazily** — HTML calls an unset one an *internal raw uncompiled
handler*, so the moment to compile is the first time anything looks: `RunListenersOn` already asks for
`on<type>` and finds nothing, and that is the point at which to compile the
attribute's text — no parser hook, and an element without the attribute pays a
lookup it was already paying. Cache the compiled function against the
attribute's *source text* so that changing the attribute recompiles and
`removeAttribute` clears it.

**The security half: an inline event handler is exactly what CSP
`'unsafe-inline'` governs, and `src/bindings` cannot see `src/csp`** — its
`allow:` line is a security boundary (ADR 0008), and `DocumentPolicy::
AllowsInline` lives in `src/engine`. So the gate is a flag the engine sets on
this layer, the same inversion `GeometrySource` and `NetworkSource` use. It is
one boolean per document rather than a per-handler question: nonces and hashes
do not apply to handlers, only `'unsafe-inline'` does (`script-src-attr`
falling back to `script-src`). Landing the compilation without that flag would
open a script-execution path CSP does not see.

Two things a later session must not undo. `csp::Policy::AllowsInlineHandler` is
its own function rather than `AllowsInline` with two empty strings: the rule
that a nonce **cancels** `'unsafe-inline'` is about `<script>` *elements*, which
can carry a nonce, and an attribute cannot — so
`script-src 'unsafe-inline' 'nonce-abc'` permits a handler while refusing an
un-nonced inline script, and the general form refuses both. And the compile is
**capped at 10,000 per document**, because `Interpreter::Run` begins a host turn
(resetting the step budget) and retains the AST: a loop that rewrites the
attribute and dispatches would refresh the budget metering it, which is a hang a
page can drive.

**`dom/collections` was written, measured at 7 -> 36 of 53, and then set aside
rather than landed — read this before writing it again.** A live
`HTMLCollection` is the right answer and the shape is settled: a Proxy over a
target holding (root, kind, two strings), with the match list cached against
`dom::Document::MutationVersion` so that `for (i = 0; i < coll.length; i++)` is
not quadratic. Two things that pass measured tests are worth keeping from that
attempt: the cache must key on **`ConnectedDocument`**, not `NodeDocument` — a
collection rooted at a detached element has a node document whose version never
moves, so caching against it freezes the collection at its first answer — and
the supported property names are **id then name per element in tree order**,
not all ids followed by all names.

It was set aside because it cost `Document-getElementsByTagName.html` eight
subtests with a symptom nobody should ship without understanding: for a
non-HTML-namespace or non-ASCII element, `assert_array_equals` reported
`coll.length` as correct and `0 in coll` as **false**.

**Two probes ruled out the obvious causes, and their results are the reason
this note exists rather than a fix.** A probe that appends
`createElementNS("test", "st")` to a div and asks both
`document.getElementsByTagName("st")` and `element.getElementsByTagName("st")`
reports `length=1 0in=true idx0=true item0=true keys=["0"]` for both — so the
matcher, the `has` trap, the indexed getter and the cache all agree in
isolation. A second probe that replays the earlier operations of
`Document-Element-getElementsByTagName.js` first — the `l[5] = "foopy"` expando
on an index, and the uppercase-HTML-tagName append — and *then* asks, reports
`len=1 0in=true idx0eq=true`. So neither the isolated query nor the two
obvious prior interactions reproduce it.

The measurement, so the trade is on the record: with the collection in place,
`Document-getElementsByTagName.html` + `Element-getElementsByTagName.html` go
**23 → 11 of 37**, against `dom/collections` **7 → 36 of 53**. Net +17 and a
mechanism nobody understands, which is the wrong side of this project's rules.

What is left to check, in order: whether `instanceof` on a Proxy consults a
`getPrototypeOf` trap this handler does not define (the file's *first* test is
`coll instanceof HTMLCollection`), and whether `Object.getOwnPropertyNames` on
a Proxy re-validates each reported key through `getOwnPropertyDescriptor`. Both
are engine questions in `src/js`, not binding questions — which is the useful
part of this: the next attempt should start in the Proxy implementation, not in
the collection.

**`Event-dispatch-single-activation-behavior.html` is 121 subtests and is
*not* the HTML feature it looks like — the activation behaviour already
exists.** `engine::Page::ResolveClickActivation` walks the composed ancestors
of a click target and already handles submit controls, reset controls,
checkable inputs, anchors with an `href`, and media playback. What it is
missing is a caller: it runs from `EngineInput.cpp`, which only sees *real*
pointer input, so `element.click()` from script dispatches the event and no
activation follows.

The seam to reach it already exists too, and this is the point of writing it
down: **`PendingSubmit`**. `src/bindings` cannot navigate or lay out, so a form
submission a script asks for is *recorded* on `DomBindings` and the engine
takes it after the turn ends (`TakePendingSubmit`, drained in `Page.cpp`).
A pending *activation* is the same shape -- one element recorded by
`DispatchClick` when nothing called `preventDefault`, taken by the engine, fed
to `ResolveClickActivation`. That is one member, one accessor and one drain,
not a form-control subsystem.

Two things the test needs that the walk above does not yet cover: `<details>`/
`<summary>` toggling, and a `<label>` forwarding activation to its labelled
control (with the specification's exception -- a label whose event target is
already interactive content does nothing). Both belong in the same walk.

**The next block after C11 is `Body-FrameSet-Event-Handlers.html`, 48 subtests,
and it is worth writing down because it is not what its name suggests.** The six
names (`onblur`, `onerror`, `onfocus`, `onload`, `onresize`, `onscroll`) on
`<body>` and `<frameset>` are *window-reflected*: the element's handler slot **is
the Window's**, so `body.setAttribute('onload', 'return')` has to make
`window.onload` a function and `window.onload === body.onload`. Four properties
have to hold at once, and the test asserts each separately:

- `body.onload` is **null** before anything sets it, not undefined;
- the accessor pair is **enumerable** on the prototype, because the test walks
  `for (var attribute in element)`;
- setting the *content* attribute compiles and lands on the **window**, which
  means the compile happens at the attribute write rather than lazily at
  dispatch — reading `window.onload` afterwards must already see it, and the
  window's slot is plain data;
- setting a non-function (a string, null) stores **null**.

So it wants the attribute-write path in ReflectedAttributes.cpp — which the
module deliberately has exactly one of — plus an accessor pair on
`HTMLBodyElement.prototype` and `HTMLFrameSetElement.prototype` that reads and
writes the window's slot, and the same CSP gate C11 introduced, since compiling
is compiling. A *detached* `document.createElement('body')` forwards too; the
test uses one, so there is no connectedness check to add.

What is left of C11 is the *IDL* half — `el.onclick` reading back the compiled
attribute, since HTML makes the two one slot — and that belongs to C10's
reflected-attribute table.

C1 and C2 are the two that unblock the rest of the suite and should be done
first, by one agent, in that order. C4–C10 are parallel after them.

**C1, C2 and C3 are done, and C4 landed in two halves on 2026-08-11. `dom/` is at 54.2%
(3850 of 7098 subtests), from 11.6% at the baseline.** The namespace half gave `dom::Element`
and `dom::Attribute` a namespace and a prefix length, so `tagName`, `localName`, `prefix` and
`namespaceURI` are four answers rather than two guesses at one field. The second half gave every node a **node document**
— stored, because the DOM assigns one at *creation* and it survives detachment, so no walk over
the tree derives it — and with it the node types this tree did not have (`DocumentType` with its
identifiers, `ProcessingInstruction`), the ChildNode insertions (`before`, `after`), the document
mutation constraints, and the complete per-tag interface table. `dom/nodes` 49.4% → 58.6% →
**62.2%** (3389 of 5451).

**Two of that session's findings are about the instrument and generalise past M-C.** The runner's
per-test timeout was the *same number* as testharness.js's own, so a page that timed out was
killed at the instant it began reporting: 86 of `dom/nodes`' 327 tests recorded "the page never
reported" and lost every subtest they had already run. A five-second grace made 242 of them
visible in that one directory. And **native bindings carry no `length`**, which web-platform-tests
branches on — `pre-insertion-validation-hierarchy.js` decides whether to pass a second argument by
reading `parent[method].length > 1`. Both are the same shape as ADR 0040's own founding bug: the
browser was fine and the reporting path was not.

**C4 is still open.** What is left is `Attr` as a real node, and XML documents — the latter is J1
and accounts for 488 subtests of the two `createElement*` files on its own.
`docs/wpt-tasks.json` has the detail, including the 136-subtest file in `dom/nodes` that is a WICG
proposal rather than a gap.

**C1, C2 and C3's lesson, unchanged:**
C3's lesson is worth reading before starting C4–C10, because it applies to
every one of them: the *ability* the task names was not where the subtests
were. Writing the conversion layer moved almost nothing on its own; what moved
1,354 subtests was the four DOM types it made cheap to notice were absent or
approximate — `DOMTokenList`, `CharacterData`'s five mutation operations,
`document.createEvent`'s alias table, and name validation. **Rank the area's
test files by failing-subtest count before writing any code.** One `python3`
over `tests/wpt/expectations/<area>.txt` does it, and it is the difference
between a session that fixes the thing it guessed and one that fixes the thing
that costs.

**Exit:** `dom/` ≥ 85%, `shadow-dom/` ≥ 75%, `custom-elements/` ≥ 75%,
`domparsing/` ≥ 80%.

---

### M-D — CSS: the cascade, values, and the object model · **#5 in the order**

`css/css-syntax/`, `css/css-values/`, `css/css-cascade/`, `css/css-variables/`,
`css/css-conditional/`, `css/selectors/`, `css/cssom/`, `css/cssom-view/`.

These are mostly script-visible and therefore mostly `testharness` rather than
reftests, which makes them cheap to run and unambiguous to read.

| id | task | depends on | target |
|---|---|---|---|
| D1 | `min()`, `max()`, `clamp()`, and nested math; `calc()` with more than one relative term | — | `css-values` +15% |
| D2 | Viewport units (`vw`/`vh`/`vmin`/`vmax`/`dv*`/`sv*`) — needs a viewport size in the cascade, which is a `css`↔`layout` seam decision | D1 | `css-values` +10% |
| D3 | CSSOM: `CSSStyleSheet`, `CSSRuleList`, `cssText` round-tripping, `insertRule`/`deleteRule`, `document.styleSheets` | M-C C1 | `cssom` 70% |
| D4 | CSSOM-View: `scroll*`, `client*`, `getClientRects`, `elementFromPoint`, `matchMedia` completeness | D3 | `cssom-view` 70% |
| D5 | `:has()`, `An+B of S`, `:dir()`, `:lang()`, and the specificity consequences (ADR 0016 priced `:has()` behind a measurement — take the measurement) | — | `selectors` 55% (§0.5) |
| D6 | `@media` evaluated at cascade time rather than parse time (ledger session 49's leftover), `@supports` nesting, `@layer`, and **container queries** — 184 of this area's 223 tests are a harness ERROR on them | — | `css-conditional` 90% (§0.5) |
| D7 | Serialization: every computed and specified value's exact string form. Dull, mechanical, enormous, and a prerequisite for most of `cssom` | D3 | `cssom` +10% |
| D8 | `css-syntax/` — error recovery to the letter, nested rules, custom property grammar | — | 85% |

**Exit:** `css/css-values` ≥ 75%, `css/cssom` ≥ 70%, `css/selectors` ≥ 80%,
`css/css-cascade` ≥ 80%.

---

### M-E — Layout · **#1 in the order**

The largest area by test count and the one where reftests dominate, which means
it is also the milestone that will find bugs in the rasterizer by accident.

**And it is now first in the order by nearly a factor of two — 9,285 files, of
which 7,802 are reftests.** It was ranked 3rd of 11 until 2026-08-17, on a column
that could not see the reftest half of the suite at all; gate 0 (F2, then F9) is
what moved it, and this section's "reftests dominate" turned out to be the
literal truth rather than a figure of speech. `css/CSS2/` alone (E1) went from
40 files to 3,907. The caveat this paragraph used to carry — that reftest
results are untrustworthy until fuzzy matching exists — is discharged: F2 landed,
and F9 recorded all 20,998 in 105 seconds.

Depends on nothing in M-C or M-D, so it can run fully in parallel from the
start. **Start with `--reftest-artifacts DIR`**: a pixel count cannot tell a
missing feature from an antialiasing difference and a picture can, in one
second.

| id | task | depends on | target |
|---|---|---|---|
| E1 | `css/CSS2/` — the normative block/inline model. Split by subdirectory across several agents; `normal-flow`, `floats`, `positioning`, `tables` are natural pieces | F2 | 60% |
| E2 | `css/css-flexbox/` — finish it: `align-content`, baseline alignment, `min-content` sizing of flex items, nested flex | F2 | 50% (§0.5) |
| E3 | `css/css-grid/` — the in-flight session 39, finished against the suite rather than against a page | F2 | 60% |
| E4 | `css/css-sizing/` — `min-content`/`max-content`/`fit-content`, `aspect-ratio` interactions | — | 75% |
| E5 | `css/css-position/` — `sticky` against a real scrollport, `inset` shorthand, abspos containing-block edge cases (the old.reddit `#header-bottom-right` bug is here) | F2 | 75% |
| E6 | `css/css-overflow/` — scrollports, `overflow: clip`, `text-overflow`, scrollbar gutters | E5 | 70% |
| E7 | `vertical-align` — absent entirely today; `css/CSS2/vertical-align` plus inline layout's baseline table | E1 | `CSS2/vertical-align` 70% |
| E8 | `css/css-text/` — white-space processing, `word-break`, `overflow-wrap`, `text-indent`, `tab-size` | — | 65% |
| E9 | `css/css-writing-modes/` — vertical writing modes are a whole coordinate system this browser does not have. Scope it, ADR it, then do it | E1 | ADR + 40% |
| E10 | `css/css-tables/` + `css/CSS2/tables/` — `border-spacing`, `border-collapse`, column sizing | — | 60% |
| E11 | `css/css-display/`, `css/css-box/`, `css/css-multicol/` | — | 60% / 70% / 30% |

**Exit:** `css/CSS2` ≥ 60%, `css-flexbox` ≥ 80%, `css-grid` ≥ 60%, and no reftest
in these areas failing for a *harness* reason.

---

### M-F — Paint, colour and graphics · **#2 in the order**

`css/css-backgrounds/`, `css/css-images/`, `css/css-color/`,
`css/css-transforms/`, `css/css-ui/`, `svg/`, `html/canvas/`.

| id | task | depends on | target |
|---|---|---|---|
| F1 | `background` on inline boxes (a known old.reddit gap), `background-clip`/`origin`/`repeat` in full | — | `css-backgrounds` 70% |
| F2 | **Harness:** fuzzy reftests (`<meta name=fuzzy>`), and a reftest failure that writes both PPMs and a diff next to each other | — | **done 2026-08-17** |
| F3 | `css/css-color/` — `color()`, `lab()`/`lch()`/`oklab()`, `color-mix()`, and the serialization rules | — | 60% |
| F4 | Gradients — `linear-gradient` interpolation, `repeating-*`, `conic-gradient` | F2 | `css-images` 60% |
| F5 | `css/css-transforms/` — 3D transforms, `perspective`, `transform-style`, and what a stacking context does to them | F2 | 60% |
| F6 | `html/canvas/element/` — the 2D context against the suite rather than against hand-written tests | — | 55% · **731 files, 19 blocked** |
| F6b | `html/canvas/offscreen/` — `OffscreenCanvas`, `transferControlToOffscreen`, and a 2D context on a worker thread | G5 | 40% · **1,921 files, 957 blocked** |
| F7 | `svg/` — the scope decision | — | **done 2026-09-01** · ADR 0043; SMIL refused |
| F7a | `svg/` — the SVG presentation properties become CSS properties | F7 | `svg/styling` 60% |
| F7b | `svg/` — the SVG DOM: element interfaces and base values | F7 | `svg/types` 60% |
| F7c | `svg/` — SVG boxes in the layout tree, replacing serialize-and-rasterize | F7a, F7b | `svg/geometry` 60% |
| F8 | Image formats — WebP and AVIF decoders (ADR 0023 counted them); `png/` and the image parts of `css-images` | — | fuzzers + 80% of `png/` |
| F9 | **Harness:** reftests enter the measurement at all — record them in `tests/wpt/expectations/`, count them in the baseline | F2 | **done 2026-08-17** |
| F11 | **Harness:** `reftest-wait` — a reftest that says when it is ready. The runner photographs the *initial* state of 1,193 files that say "not yet" | F9 | 1,193 files |
| F10 | **Harness:** the reftest *gate* — name the eight intermittents, then drop `--testharness-only` from the `ctest` registration | F9 | **done 2026-08-31** · seven of the eight were one font-cache bug; 7,963 → 8,005 files |

**`reftest-wait` is not implemented and it is 1,193 files (F11, found 2026-09-01).**
`class="reftest-wait"` on the root element means "do not photograph me yet", and
the test removes it — usually from a `requestAnimationFrame` — once the state it
wants recorded exists. `grep -rn reftest-wait tools/wpt/` is empty, so every one
of those files is photographed *before* the thing it tests has happened. It
passes exactly when the initial state and the final one look the same, which is
an accidental pass in the same family as F9's blank pages — and it **flips to a
failure the moment the browser starts implementing the property under test**.
Four did precisely that in F5's session, which is how it was found. The
measurement is wrong before the browser is, and that is what makes it a gate
task rather than a nicety.

**F7 turned out to be two things and the smaller one paid for the larger.** The
scope decision it names was unanswerable on 2026-09-01, because 228 of `svg/`'s
810 gap files were *blocked* — the harness never reported, so nothing about them
was evidence in either direction. 214 of them were one comparison:
`PageScript::Collect` matched `TagName() == "script"`, and an SVG document
writes its external scripts as `<h:script src="…"/>` in the XHTML namespace.
Fixing that (and the `SetHtmlDocument(false)` the engine's own XML parse had
never called) took the area's timeouts from 263 to 51 and made ADR 0043 a
document written from measurements. **The rule is the same one E3 found and it
is now three for three: when an area's `blocked` count is most of its harness
files, find what the blocked files have in common before reading any failure as
a specification gap.**

**F6 was one task over two features until 2026-08-17, and the split is worth
more than either half.** `html/canvas/` is 2,654 gap files; **1,921 of them are
`html/canvas/offscreen/`**, which is a worker feature, not a canvas feature —
G5's own notes had already named `OffscreenCanvas` as one of the three things
left in a worker after the global scope landed, at "889 tests", and it is more
than twice that. It is now F6b and depends on G5. What is left in F6 is
`html/canvas/element/`: **731 files with 19 blocked**, the purest ratio of
ordinary specification work in the top ten of the whole tree. Nothing gates it.
The 2D context exists, the tests reach it, and they disagree with it 731 times.

**F2 and F9 both landed on 2026-08-17, and between them they moved 48% of the
suite out of silence and into the ranking.** F2 is the tolerance and the picture;
F9 is the recording. The four things worth carrying forward:

- **The tolerance was never what was failing these tests.** Running all 686
  fuzzy-annotated reftests: 199 OK, 485 unexpected, and only **six** of the 199
  needed the tolerance at all. Fuzzy matching was a precondition for the
  measurement being honest, not a source of passes — and F9's full run confirms
  it, since 7,394 of 20,998 pass and almost none of them needed a tolerance.
  `tools/wpt/Reftest.cpp` is transcribed from wptrunner rather than invented,
  because a tolerance of our own would make the two sides incomparable.
- **`--reftest-artifacts DIR` is how to read a reftest failure**, and it is
  worth using before writing any layout code. The first three images this ever
  produced (`css/CSS2/backgrounds/background-003.xht`) showed the reference's
  green stripe missing from the test in one second — a paint bug, not a
  tolerance. It is opt-in and bounded (`--reftest-artifacts-limit`, default 64)
  because a full reftest run at three 1.4MB PPMs each is 90GB. With 11,356
  reftest files now on the board, this is the first command of an M-E session.
- **A passing reftest says how much room it had left**, in its harness message
  under `--verbose`: `35 pixels differ, worst channel 2; within 0-5;0-150`. A
  reftest passing by one pixel of its tolerance and one passing exactly are
  different facts about the renderer, and only one of them survives the next
  change to it. It also says `both pages blank` when there was nothing to
  compare — 757 of the 7,394 passes are two white canvases agreeing, which is
  the one way this number can rise while the browser gets worse.
- **The run is 105 seconds, not six hours.** §M-B's projection was made against
  a runner that span a core per finished page, and it kept the reftest half out
  of the measurement for a week. Re-measure before deferring anything on cost.

**Exit:** `css-backgrounds` ≥ 70%, `css-transforms` ≥ 60%, fuzzy reftests
supported.

---

### M-G — Script, the event loop, and timing · **#6 in the order**

`html/webappapis/`, `hr-time/`, `user-timing/`, `performance-timeline/`,
`web-animations/`, `workers/`, `streams/`, `webmessaging/`, `console/`.

| id | task | depends on | target |
|---|---|---|---|
| G1 | The event loop to the letter: task sources, microtask checkpoints, `queueMicrotask`, rendering opportunities | — | `html/webappapis` +15% |
| G2 | **Script time-slicing** — ADR 0036 exists; TD-0007 is the "app is not responding" the user reported. A 9.7-second uninterruptible call is a correctness bug about the event loop, not a performance nicety | G1 | TD-0007 closed |
| G3 | `streams/` — `ReadableStream` and friends. Large, self-contained, and the thing `response.body` is absent for | — | 60% |
| G4 | `web-animations/` — the API surface over the animations that exist | — | 55% |
| G5 | `workers/` — `importScripts` **first**: it is 1,380 timeouts suite-wide. Then module workers, `SharedWorker` (decide: ADR or implement), worker `fetch` | — | 40% (§0.5) |
| G6 | `webmessaging/` — `postMessage` across every context, ports, structured clone completeness | M-J J1 | 70% |
| G7 | Timing APIs — `PerformanceResourceTiming`, `PerformanceNavigationTiming`, `PerformanceObserver` buffering | — | 65% |
| G8 | `console/` — the whole surface, which is one file and cheap | — | 90% |

**Exit:** `hr-time` ≥ 90%, `html/webappapis` ≥ 60%, `streams` ≥ 60%, TD-0007
closed.

---

### M-H — The network, and the security around it · **#3 in the order**

`fetch/`, `xhr/`, `cors/`, `url/`, `mimesniff/`,
`content-security-policy/`, `subresource-integrity/`, `referrer-policy/`,
`upgrade-insecure-requests/`, `cookies/`, `websockets/`, `eventsource/`.

**This milestone is gated on a harness decision.** Most of `fetch/` and nearly
all of `cors/` point at `.py` handlers, which ADR 0040 §2 refuses to
approximate. Task H1 decides what replaces them.

| id | task | depends on | target |
|---|---|---|---|
| H1 | **Harness:** the handler question. Options: implement the ~20 most-used handlers natively in `tools/wpt/`; or a `--python-handlers` mode that shells out to a real `wpt serve` when Python is present; or declare the area out of scope and say so. Decide and amend ADR 0040 | — | ADR amendment |
| H2 | `url/` — the WHATWG URL test suite is a JSON data file and needs no server at all. Run it, fix it, and it should reach very high | — | 95% |
| H3 | `mimesniff/` — sniffing rules, which this browser does by magic number ad hoc | — | 80% |
| H4 | `fetch/api/` — request/response objects, headers guards, `body` mixin methods (`formData()` needs `FormData`, deliberately absent today) | H1 | 35% (§0.5) |
| H5 | `xhr/` — the shim over `fetch`; `timeout`, `upload`, `overrideMimeType`, response types are all deliberately absent (ADR: revisit) | H1 | 60% |
| H6 | `content-security-policy/` + `subresource-integrity/` — the enforcement points exist; the suite will find the ones that are enforced in the wrong place | H1 | 60% |
| H7 | `cookies/` — the attribute parsing, `SameSite`, and the partitioning this browser does by default | — | 70% |
| H8 | `referrer-policy/` + `upgrade-insecure-requests/` — where this browser deliberately deviates (privacy defaults, ADR 0033), record the deviation as an expectation with the ADR named | — | recorded |
| H9 | ~~TLS in the harness: an https origin, so the `.https.html` half of the suite runs at all. Self-signed, trusted only by the runner~~ **done 2026-08-18** — 1,268 files, and ADR 0040's amendment of that date is how the trust is scoped | H1 | `.https.` tests runnable |
| H10 | `websockets/` + `eventsource/` — both need server support beyond static files | H1, H9 | scoped |

**Exit:** `url/` ≥ 95%, `mimesniff/` ≥ 80%, and a written decision on `.py`.

---

### M-I — Storage · **#10 in the order**

`webstorage/`, `IndexedDB/`, `storage/`, `FileAPI/`.

| id | task | depends on | target |
|---|---|---|---|
| I1 | `webstorage/` — quota, events, partitioning | — | 80% |
| I2 | `IndexedDB/` — the largest single-API suite in WPT. Split by subdirectory: keys/ordering, transactions, cursors, indexes | M-C C1 | 25% (§0.5) |
| I3 | `FileAPI/` — `Blob`, `File`, `FileReader`, `URL.createObjectURL` | M-C C1 | 70% |
| I4 | `storage/` — `navigator.storage`, quota estimation, and what this browser refuses to persist (ADR 0021) | I1 | recorded |

**Exit:** `webstorage` ≥ 80%, `IndexedDB` ≥ 55%, `FileAPI` ≥ 70%.

---

### M-J — Navigation, browsing contexts, and the process split · **#8 in the order**

`html/browsers/`, plus the parts of `html/semantics/` about `<iframe>`.

This is the milestone with the most *architecture* in it and the least test
count, and it is on the critical path for the acceptance sites: gmail, maps and
plex are all iframe-heavy.

| id | task | depends on | target |
|---|---|---|---|
| J1 | Same-origin iframes — roadmap session 40. The binding layer is already receiver-based for `document`, which was the hard part | — | `html/browsers/the-window-object` 60% |
| J2 | Cross-origin iframes and the `Window` proxy — roadmap session 41 | J1 | 50% |
| J3 | The process split and the sandbox — roadmap session 42, ADR 0004. The engine has no `platform` on its `allow:` line precisely so this is a scheduling decision | J2 | one renderer per site |
| J4 | Session history to the letter — `history.state`, traversal, `popstate`/`hashchange` ordering, `beforeunload` | — | 65% |
| J5 | Navigation: form submission methods, `target`, redirects, fragment navigation, `<base>` | J4 | 65% |
| J6 | `window.open`, named contexts, `opener`, and what this browser refuses to open | J1 | recorded |

**Exit:** J1–J2 landed; `html/browsers/` ≥ 55%; ADR 0027 no longer describes
something that does not exist.

---

### M-K — Text and internationalisation · **#9 in the order**

`encoding/`, and the text halves of `css/css-text/`, `css/css-fonts/`,
`css/css-writing-modes/`.

| id | task | depends on | target |
|---|---|---|---|
| K1 | `encoding/` — the legacy decoders exist (roadmap session 32); the suite tests every index table exhaustively and will find the gaps | — | 85% |
| K2 | `TextEncoder`/`TextDecoder` streaming, `encodeInto`, every label alias | K1 | 90% |
| K3 | `css/css-fonts/` — `font-feature-settings`, `font-variant`, `@font-face` descriptors, `unicode-range` | — | 60% |
| K4 | Unicode data currency: `normalize()`, the rest of `\p{...}`, and the generator that produces the tables (`tools/unicode/generate.py`) | — | `Intl`-free but complete |

**Exit:** `encoding/` ≥ 85%.

---

### M-L — Media · **#11 in the order**

`media-source/`, `html/semantics/embedded-content/media-elements/`.

| id | task | depends on | target |
|---|---|---|---|
| L1 | `HTMLMediaElement` state machine — `readyState`, `networkState`, the event order, `seek`, `play()`'s promise | — | 55% |
| L2 | `media-source/` — `SourceBuffer` append/remove, buffered ranges, `MediaSource` lifecycle | L1 | 50% |
| L3 | Track handling — `<track>`, `TextTrack`, `<source>` selection | L1 | 50% |
| L4 | The codec question end-to-end: roadmap session 27 is still `in_progress` | — | session 27 done |

**Exit:** `media-elements` ≥ 55%.

---

### M-M — Speed, memory, and idle CPU, as a first-class milestone · **#12 in the order**

Not a WPT milestone: WPT has almost nothing to say about any of the four core
principles except correctness. This runs continuously alongside everything else
and has its own exit criteria.

| id | task | depends on | target |
|---|---|---|---|
| M1 | Incremental parsing and first paint — roadmap session 46, ADR 0030. A page appears when it is finished, which is the single largest perceived-latency defect left | — | first paint before load |
| M2 | TD-0005: `CollectImages` resolves the whole cascade a second time. 1.58s on wikipedia, larger than laying the page out | — | TD-0005 closed |
| M3 | TD-0003: 1.33M individually allocated AST nodes, three quarters of the parse | — | TD-0003 closed |
| M4 | TD-0010: `kMaxConnectionsPerPartition` bounds requests rather than connections, and six was the HTTP/1.1 number | — | TD-0010 closed |
| M5 | A memory budget with a number in it: peak RSS on each acceptance page, tracked like the pass rates | — | `docs/performance/` entry |
| M6 | Idle CPU regression test: a settled page must cost zero wakeups. Assert it, in `ctest`, per page | — | test exists |
| M7 | Benchmarks for layout and the DOM, in `bench/`, because timing a page load on a shared machine is worthless | — | two new bench files |

**Exit:** every open entry in `docs/tech-debt.md` is either closed or has a
measurement and a reason it is still open; idle CPU is asserted rather than
claimed.

---

### M-N — The acceptance sites · **#13 in the order**

The five sites in `docs/adr/0007-compatibility-targets.md` plus the four the
user named. These are **gates, not tasks**: each one is checked at the end of a
milestone, and what it finds becomes tasks in the *next* milestone.

| site | what it exercises that nothing else does | gate after |
|---|---|---|
| news.ycombinator.com | already renders — keep it green | every milestone |
| old.reddit.com | already renders — `vertical-align`, inline backgrounds | M-E, M-F |
| en.wikipedia.org | already renders — the CSS breadth | M-D, M-E |
| itch.io game page | canvas, WebGL(!), gamepad, fullscreen, audio | M-F, M-G |
| www.linkedin.com | SPA routing, huge bundles, service workers | M-G, M-J |
| www.youtube.com | the DI container bug at `EhE (@1323410)`, MSE, SPA | M-G, M-L |
| mail.google.com | iframes, IndexedDB, workers, long-lived script | M-I, M-J |
| maps.google.com | WebGL, canvas, pointer events, workers | M-F, M-J |
| plex (local) | MSE, HLS, codecs, media session | M-L |

**Two of these need capabilities nobody has scoped yet**: WebGL (ADR 0029
discusses the fingerprinting surface and stops short) and service workers (ADR
0022 refuses background work — maps and linkedin will both want one). Each needs
an ADR before it needs code, and the ADR may well be a refusal with a fallback.

---

### M-O — HTML's own elements · **#4 in the order** · **added 2026-08-17**

`html/semantics/`, `html/syntax/`. **This milestone exists because the ranking
by test file found 1,689 files with no task anywhere in this plan**, which is
more than the whole of `css/` below `html/canvas`. Every other area in the tree
had an owner; HTML's elements had `html/dom/` (C10, done), `html/canvas/` (F6),
`html/browsers/` (M-J) and the media-element subtree (M-L), and nothing for the
elements themselves.

| id | task | depends on | gap · blocked/feature | target |
|---|---|---|---|---|
| O3 | `html/semantics/embedded-content/` — `<img>`, `<iframe>`, `<object>`, `<embed>`, `<picture>`, `<canvas>` as elements rather than as painters: loading, decoding, the state machine, what happens when the resource fails | J1b | 520 · 237/283 | 50% |
| O2 | `html/semantics/scripting-1/` — `<script>` to the letter: the module loader, `async`/`defer`/`module` ordering, classic-vs-module fetch paths | — | 370 · 140/230 | 55% |
| O1 | `html/semantics/forms/` — constraint validation, the value/checkedness/dirty-value machinery, submission encodings, `<input>` types, `<select>`/`<option>`, `<label>` activation | — | 314 · 65/249 | 60% |
| O4 | `html/syntax/` — the tokenizer and tree builder against the suite: html5lib tests, character references, foreign content, the serializer | — | 172 · **124**/48 | 80% |

Two of these carry a finding worth more than the task. **O4's ratio is 124
blocked to 48 feature**, on the oldest module in the tree, and we already fully
pass 170 files there — so the work is almost certainly plumbing rather than
parsing, and one `--verbose` run should confirm that before anyone opens the
tokenizer. **O2's largest single cause is the module loader**, which is 226
tests suite-wide and the same open question `www.reddit.com`'s bundle stops at:
`Interpreter::SetModuleResolver` is synchronous, so wiring it means either a
pre-pass that fetches the graph or a resolver that can answer later. Decide
that before writing code.

**The four tasks name 1,376 of the milestone's 1,689 files.** The other 313 are
a real tail and are deliberately not a task yet, because none of them is an
agent-session on its own: `interactive-elements` 84, `document-metadata` 71,
`popovers` 71, `tabular-data` 27, `selectors` 24, `the-button-element` 14,
`links` 9, and five directories in single figures. Re-rank them once O1–O4 have
landed; several will turn out to be the same cause as one of the four.

`document.write` is refused (ADR 0011/0012/0026) and appears throughout these
areas. Its failures get an expectation comment naming the ADR, not an
implementation — rule 1 of §0.

**Exit:** `firefox_gap.files` under 700 across the four, and no cause in the
ranked table that is a harness reason.

---

## 3. Cross-cutting work that never finishes

These are not milestones and have no exit. They are what a session does *as
well as* its task.

- **W1 — the harness itself.** Fuzzy reftests (F2), `testdriver.js` for tests
  that need synthesised input, the `.py` question (H1), https (H9), `wpt`
  revision bumps, and keeping the run under ten minutes. When the harness costs
  a session more than it saves, fix the harness.
- **W2 — fuzzing.** Every parser gets a target on the commit it lands, and the
  corpora get run. `guidelines/security.md`.
- **W3 — the architecture lint.** `MODULE.deps` and the budgets. When one fires,
  fix the design it points at; raising a budget is sometimes right and is always
  a decision.
- **W4 — tech debt.** `docs/tech-debt.md` is read before optimising anything and
  written to when a shape is left wrong on purpose.
- **W5 — the session log.** `docs/session-log.md` records what a session *found*
  that its diff does not say. A session that cannot hand off through the git
  log, the session log and the ledger has not finished.

---

## 4. Parallelism: what can actually run at once · **re-ranked 2026-08-17**

Four to six agents, sustainably. The graph is now the ranked order of §2, not
the milestone letters:

```
  gate 0   F2 ─▶ F9 ─▶ F10          B6      (F2, F9, F10 done)
          (fuzzy) (recorded) (gated)  (summary state)
            done     done      open        open
              │  half the suite entered the measurement here, and layout is #1
   ┌──────────┼──────────┬─────────────┬──────────────┬────────────┐
   ▼          ▼          ▼             ▼              ▼            ▼
  E1        F6         H9            J1b*           A2*          G5*
 css/CSS2  canvas     https        iframes        handlers      workers
  3,907     element   origin
  files      774       ~2,000        gates O3,      gates H8's    gates F6b
  (3,867     files     files         H8, J2, G6     generated half
  reftests)   │          │              │             │
              │          └───────┬──────┘             ▼
              ▼                  ▼                   F6b  OffscreenCanvas
        O1,O2,O4            H8  referrer-policy       1,939 files
    forms, script, syntax       1,330 files
```

`*` = already in progress.

The dependency edges that actually matter, and are the only ones worth
serialising on:

- **F2 before F9, and F9 before ranking anything in M-E.** ~~Pending.~~ **Both
  landed 2026-08-17**, and this was the one edge that gated the *plan* rather
  than a task: M-E went from 3rd of 11 to 1st by a factor of two, and `css/CSS2/`
  from 40 files to 3,907. The edge that replaced it, **F9 before F10**, is
  discharged too: the gate compares reftests as of 2026-08-31, and the eight
  intermittents turned out to be nine with a single cause under seven of them.
  **The lesson is the one this section keeps re-learning:** the ledger's note on
  those eight said the browser "does not load" the fonts in question, and every
  clause of that was wrong — it loaded them and then never used them. An
  intermittent is a measurement, not an explanation, and the explanation here was
  thirty renders of one page and a hash.
- ~~**H9 (an https origin) before H8, and before anything with `.https.` in its
  name.**~~ **Landed 2026-08-18.** 1,268 files in scope rather than the ~2,000
  gap files estimated, and the edge is discharged: H8 and every `.https.` test
  can be worked now. What remains behind it is a re-record of twenty areas.
- **J1b (same-origin iframes) before O3, H8, J2 and G6.**
- **A2 (the `.py` handlers) before H8 and any `fetch/`-adjacent task.**
- **G5 (workers) before F6b.** OffscreenCanvas is a worker feature.
- **C1 and C2 before everything in M-C, M-I and much of M-H.** Exception
  identity is load-bearing for `assert_throws_*`, which is most of the suite's
  negative tests. (Both done.)

**E1, F6, O1, O2 and O4 depend on nothing that is not already done** and are
5,574 files between them — E1's dependency was F2, which landed. If an agent is
free, those are the five to take, and **E1 is the largest single task in the
tree** now that its reftests are counted.

Everything else is independent. Two agents in the same `src/` module at the same
time is the real collision risk, so the table above is also a rough map of which
tasks touch which module:

| module | tasks that touch it |
|---|---|
| `src/bindings` | C1–C10, D3, D4, G1, G6, I2, I3, J1, J6 |
| `src/layout` | E1–E11, F1 |
| `src/css` | D1–D8, E4, F1, F3, K3 |
| `src/gfx` | F2–F8 |
| `src/net` | H1–H10, M4 |
| `src/engine` | G1, G2, J1–J6, M1, M2 |
| `src/js` | C2, G3, K4 |
| `src/html` | C9, E1, J5, M1 |

---

## 5. What "done" looks like

Not a percentage on a suite. Three things, in order:

1. **The nine acceptance pages in M-N load, render and are usable**, with no
   site-specific code anywhere in `src/` or `tools/` — the settling heuristics
   in `tools/snapshot/main.cpp` are gone because nothing needs them.
2. **The four principles have numbers.** Correctness is the WPT table. Privacy
   is the deviation list in the expectation files, each one deliberate and
   ADR-backed. Speed is `docs/performance/`, on the perf preset, per page.
   CPU and memory are M-M's M5 and M6, asserted in `ctest` rather than claimed
   in a README.
3. **A regression cannot land quietly.** Every one of the above runs in `ctest`
   or in one command that a session is required to run, and the expectation
   files make an improvement and a regression equally loud.
