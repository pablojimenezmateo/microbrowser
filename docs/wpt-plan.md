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

`docs/wpt-tasks.json` is the state. Set `status: "in_progress"` and `agent` to
something identifying, commit *that alone*, and push before starting. A conflict
on that one-line commit is the cheapest possible collision.

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

---

### M-A — The instrument · **done 2026-08-10**

`tools/wpt/`, `tests/wpt/expectations/`, ADR 0040. The runner, the server, the
manifest scanner, the expectation format, per-test process isolation, `ctest`
integration.

**Exit:** `./build/microbrowser/microbrowser_wpt dom/nodes/Node-appendChild.html`
reports per-subtest results. Reached.

---

### M-B — The baseline · **next, blocks nothing but informs everything**

One full run of every checked-out area, recorded, with the failures grouped by
*cause* rather than by test. This is the only milestone that must happen before
the others, and it is one long machine run plus one session of reading.

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

**Reftests are out of the baseline**, and that was decided while running it.
Recording them costs about six hours of the run — a reftest renders two pages,
and the full suite projected at nine hours against ninety minutes for the
testharness half — and buys an expectation file that F2 rewrites in full,
because an exact-pixel comparison against a reference rendered by the same
rasterizer calls antialiasing noise a difference. `ctest` already excludes them.
F2 is what brings the other 20,923 tests into the number.

**Why B6 exists, and it is not tidiness.** `--summary` writes
`docs/wpt-baseline.md` from `--summary-state` alone, and that state file has
lived in `/tmp`. Three sessions running have now regenerated the document from
a state describing only the areas they ran, producing a correctly formatted,
complete-looking table with a fifth of the rows — twice caught by a reader, once
not. `SummaryAccumulator::Write` refuses to write fewer rows than the document
already has, which turns the silent truncation into a message; B6 is the one
long machine run that makes the refusal unnecessary.

**Exit:** every area has a committed expectation file for its testharness tests
and a line in the table; `ctest` is green against them.

---

### M-C — The DOM and its bindings

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
`coll.length` as correct and `0 in coll` as **false** — the `has` trap and the
`length` getter disagreeing while reading the same cache slot through the same
target. A probe reproducing the same query in isolation passes, so the
disagreement needs the shared `Document-Element-getElementsByTagName.js` driver
to appear. Find that before rebuilding the rest.

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

### M-D — CSS: the cascade, values, and the object model

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

### M-E — Layout

The largest area by test count and the one where reftests dominate, which means
it is also the milestone that will find bugs in the rasterizer by accident.

Depends on nothing in M-C or M-D, so it can run fully in parallel from the
start — with one caveat: reftest results are only trustworthy once the harness's
fuzzy-matching story exists (task A-follow-up F2 below).

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

### M-F — Paint, colour and graphics

`css/css-backgrounds/`, `css/css-images/`, `css/css-color/`,
`css/css-transforms/`, `css/css-ui/`, `svg/`, `html/canvas/`.

| id | task | depends on | target |
|---|---|---|---|
| F1 | `background` on inline boxes (a known old.reddit gap), `background-clip`/`origin`/`repeat` in full | — | `css-backgrounds` 70% |
| F2 | **Harness:** fuzzy reftests (`<meta name=fuzzy>`), and a reftest failure that writes both PPMs and a diff next to each other | — | reftests trustworthy |
| F3 | `css/css-color/` — `color()`, `lab()`/`lch()`/`oklab()`, `color-mix()`, and the serialization rules | — | 60% |
| F4 | Gradients — `linear-gradient` interpolation, `repeating-*`, `conic-gradient` | F2 | `css-images` 60% |
| F5 | `css/css-transforms/` — 3D transforms, `perspective`, `transform-style`, and what a stacking context does to them | F2 | 60% |
| F6 | `html/canvas/` — the 2D context against the suite rather than against hand-written tests | — | 35% (§0.5) |
| F7 | `svg/` — triage first: this browser renders SVG as an image, and the suite tests it as a document. Decide the scope in an ADR before writing code | — | ADR + a number |
| F8 | Image formats — WebP and AVIF decoders (ADR 0023 counted them); `png/` and the image parts of `css-images` | — | fuzzers + 80% of `png/` |

**Exit:** `css-backgrounds` ≥ 70%, `css-transforms` ≥ 60%, fuzzy reftests
supported.

---

### M-G — Script, the event loop, and timing

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

### M-H — The network, and the security around it

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
| H9 | TLS in the harness: an https origin, so the `.https.html` half of the suite runs at all. Self-signed, trusted only by the runner | H1 | `.https.` tests runnable |
| H10 | `websockets/` + `eventsource/` — both need server support beyond static files | H1, H9 | scoped |

**Exit:** `url/` ≥ 95%, `mimesniff/` ≥ 80%, and a written decision on `.py`.

---

### M-I — Storage

`webstorage/`, `IndexedDB/`, `storage/`, `FileAPI/`.

| id | task | depends on | target |
|---|---|---|---|
| I1 | `webstorage/` — quota, events, partitioning | — | 80% |
| I2 | `IndexedDB/` — the largest single-API suite in WPT. Split by subdirectory: keys/ordering, transactions, cursors, indexes | M-C C1 | 25% (§0.5) |
| I3 | `FileAPI/` — `Blob`, `File`, `FileReader`, `URL.createObjectURL` | M-C C1 | 70% |
| I4 | `storage/` — `navigator.storage`, quota estimation, and what this browser refuses to persist (ADR 0021) | I1 | recorded |

**Exit:** `webstorage` ≥ 80%, `IndexedDB` ≥ 55%, `FileAPI` ≥ 70%.

---

### M-J — Navigation, browsing contexts, and the process split

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

### M-K — Text and internationalisation

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

### M-L — Media

`media-source/`, `html/semantics/embedded-content/media-elements/`.

| id | task | depends on | target |
|---|---|---|---|
| L1 | `HTMLMediaElement` state machine — `readyState`, `networkState`, the event order, `seek`, `play()`'s promise | — | 55% |
| L2 | `media-source/` — `SourceBuffer` append/remove, buffered ranges, `MediaSource` lifecycle | L1 | 50% |
| L3 | Track handling — `<track>`, `TextTrack`, `<source>` selection | L1 | 50% |
| L4 | The codec question end-to-end: roadmap session 27 is still `in_progress` | — | session 27 done |

**Exit:** `media-elements` ≥ 55%.

---

### M-M — Speed, memory, and idle CPU, as a first-class milestone

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

### M-N — The acceptance sites

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

## 4. Parallelism: what can actually run at once

Four to six agents, sustainably, once M-B is done:

```
        M-B baseline  (one agent, one long run)
              │
   ┌──────────┼───────────┬──────────────┬───────────────┐
   ▼          ▼           ▼              ▼               ▼
 M-C C1,C2  M-E E2,E4   M-D D1,D8     M-H H2,H3       M-K K1
 (bindings) (layout)    (css values)  (url, sniff)    (encoding)
   │          │           │              │
   ▼          ▼           ▼              ▼
 C4..C10    E1,E3,E5..  D3,D4,D5..     H4..H8  (after H1)
 (parallel) (parallel)  (parallel)     (parallel)
```

The dependency edges that actually matter, and are the only ones worth
serialising on:

- **C1 and C2 before everything in M-C, M-I and much of M-H.** Exception
  identity is load-bearing for `assert_throws_*`, which is most of the suite's
  negative tests.
- **F2 (fuzzy reftests) before any reftest-heavy layout task.** Otherwise M-E's
  numbers are noise.
- **H1 (the `.py` decision) before any `fetch/`-adjacent task.**
- **J1 (same-origin iframes) before J2, J3 and G6.**

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
