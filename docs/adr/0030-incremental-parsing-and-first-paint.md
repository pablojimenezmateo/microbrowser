# ADR 0030 — Incremental parsing, and showing a page before it is finished

**Status:** accepted · **Date:** 2026-08-04

## Context

`CLAUDE.md` lists this among the known-crude spots:

> a page appears when its load finishes rather than as it arrives, because there is no incremental
> parse or paint

and ADR 0011 was explicit that it enabled the fix without performing it:

> **It does not by itself make anything faster to paint.** Fifteen serialised round trips become a
> handful of concurrent ones, which is a latency win; incremental parsing and painting is a separate
> piece of work that this enables and does not perform.

The survey supplies the numbers that turn "crude" into "blocking":

| | bytes |
|---|---|
| reddit.com document | 405,179 |
| youtube.com document | 909,022 |
| youtube application bundle | 10,732,237 |
| plex bundles | 5,472,596 |

A 909KB document is parsed in full, its subresources fetched, its 3.5MB of CSS cascaded and its
10.7MB of script compiled and run, and only then does anything appear. On a fast connection that is
seconds of blank window; on a slow one it is the difference between a browser that works and one
that appears not to.

There is a second, less obvious cost, and it is the one that makes this an architecture decision
rather than an optimisation. **The main loop is blocked for the whole of that.** ADR 0011 made the
*network* asynchronous and left the *work* synchronous: a 10.7MB parse is one uninterruptible call.
During it the window does not redraw, the stop button does not work, and a resize is queued. A
browser whose event loop is unresponsive for seconds has lost the property that ADR 0011 was written
to protect, in a different way than by polling.

## Decision

**Parsing, style, layout and paint become incremental, and the invariant that makes it safe is
stated first because everything else is subordinate to it.**

### The invariant: chunking is not observable in the result

**The same bytes must produce the same final page regardless of how they were chunked, how the
chunks were timed, or in what order concurrent resources arrived.**

That is ADR 0011's ordering-test insight, extended from resource arrival to byte arrival. ADR 0011
already said the failure mode of asynchronous loading is nondeterminism rather than slowness, and
built its tests around delivering the same responses in several orders. Incremental parsing widens
the space of orders enormously — every split point in every response is a new order — and the test
strategy widens with it: **the canned transport delivers a document in one chunk, in bytes, and at
random boundaries, and all three must produce an identical display list.**

That test is cheap, it is the only thing standing between this feature and a class of
irreproducible rendering bugs, and it lands on the first commit rather than after.

### 1. The tokenizer becomes resumable, which it nearly is

`src/html`'s tokenizer is a spec-literal state machine, and a state machine is resumable almost by
construction: the state is already explicit, and what is missing is the ability to stop at the end of
the available input and continue where it left off.

Two rules make it correct rather than approximately correct:

- **A chunk boundary is not a token boundary.** A tag name split across two network reads is one tag.
  The tokenizer keeps its partial buffer and its state across calls, and never emits a token it has
  not finished seeing.
- **The end of input is only the end when the load says so.** The specification's end-of-file
  handling — implicit tag closing, error recovery — runs at the *real* end, never at the end of a
  chunk. Getting this wrong is how a document gets its `</body>` inserted three times.

The tree builder needs no change in kind: it already consumes tokens one at a time.

### 2. Work is chunked against a deadline, and the loop stays responsive

The parse runs in slices. Each slice processes tokens until a **time budget** is exhausted or input
runs out, then yields to the event loop. The loop drains events, redraws if the dirty region says to,
and reschedules the parse through the mechanism it already has — `IdleWaitState::next_deadline_ms`,
with a deadline of *now* while there is pending input.

This is the part that needs care against the invariant, and the resolution is the same one that made
timers safe: **the loop is told there is work rather than asked whether there is.** A parse with no
pending bytes and no pending tokens schedules nothing and the loop blocks. Zero idle CPU is
untouched, because an unfinished parse is not an idle browser.

The budget is a measured number, not a round one, chosen the way ADR 0009's parse depth bound was:
against real pages, with the margin recorded. Too small and the parse never finishes; too large and
the window stops redrawing.

**A script still blocks the parse**, because it is defined to. ADR 0011 already implemented `defer`,
`async` and `type=module` as the three points they are, and this changes none of it — a blocking
script suspends the parse, which is now a suspension of something that was already yielding rather
than a block inside one long call.

### 3. Style, layout and paint run on what exists

The document is laid out and painted in its incomplete state, repeatedly, as it grows. Three things
have to be true for that to be worth doing:

- **Layout is incremental at the block level**, or the arithmetic does not work: relaying out the
  whole document per slice makes a large page quadratic. Blocks already completed above the insertion
  point do not move when content is appended below them, and that is the property to exploit —
  layout resumes from the deepest box affected rather than from the root.
- **Style resolution is per element as it is inserted**, which it already nearly is, and the
  invalidation index from ADR 0016 is what keeps a late stylesheet from restyling everything more
  than once.
- **A stylesheet still blocks rendering**, because painting before it arrives shows the user a flash
  of unstyled content and then a different page. It does not block *parsing*, which is the split
  every engine makes and the reason `<link rel=stylesheet>` in `<head>` is not a disaster.

### 4. First contentful paint is a defined moment, and it is measured

The point of the whole exercise is a number, so the number gets a name and a scope:
**time from navigation start to the first frame containing page content.** It joins the existing
scope-based measurements, and a `docs/performance/` note records it before and after, on the same
pages, the way `m6-damage.md` did for incremental repaint.

Without that, "it feels faster" is the only available evidence, and `guidelines/performance.md`
exists to say that is not evidence.

### 5. What this does not do

- **It does not make the page finish sooner.** Total work is unchanged or slightly higher — layout
  runs more times. What changes is when the user first sees something, and whether the window
  responds meanwhile. Saying this plainly matters because the temptation to report the wrong number
  is real.
- **It does not help a page whose content is built by script**, which is youtube.com and Plex
  exactly. Their documents are a skeleton; incremental parsing paints the skeleton sooner and the
  content still waits for 10.7MB of JavaScript. **The win is largest on server-rendered documents** —
  reddit's 405KB, Hacker News, and most of the web outside the target set.
- **It does not do incremental compilation of script.** A 10.7MB bundle is still one parse and one
  compile. Whether that should be sliced against the same deadline is a real question, it interacts
  with `js::Compile`'s bounds from ADR 0009, and it is left open here rather than answered badly.

### 6. Where it sits

**Last of the sixteen ADRs from this survey, and deliberately so.** It is an optimisation of a
pipeline, and optimising a pipeline that is still missing images, fonts, selectors and geometry
optimises the wrong thing. It is written now because the invariant in §0 has to be designed in, and
because ADR 0011 left it as a named piece of unfinished business rather than an idea.

The one argument for pulling it earlier: it is the change most visible to a person using the browser,
and `CLAUDE.md`'s own strongest lesson is that looking at a real page finds what tests do not. If a
session ever ends with the schedule ahead of expectation, this is the item worth pulling forward.

## Consequences

- **Every stage of the pipeline gains a "partial" state**, and every consumer has to tolerate it. A
  layout over a tree that is still growing, a display list built from an incomplete document, a hit
  test against a page that is not finished. Each is a source of bugs that only appear under a
  specific arrival timing, which is why the determinism test is the first commit.
- **The parse can be interleaved with script execution and with paint**, which means the DOM can be
  observed mid-parse — by a `MutationObserver`, by a `DOMContentLoaded` that must not fire early, by
  a script reading `document.body` before it exists. The lifecycle events become precisely-defined
  moments rather than approximately-right ones.
- **`src/app`'s idle-wait policy gains a third input** after the deadline and the sockets: pending
  parse work. It is the same mechanism a third time, which is evidence the mechanism was the right
  one.
- **A slow page becomes visibly slow rather than invisibly slow**, which is a user-experience
  improvement and a debugging improvement at once: watching a page assemble shows where it stalls.
- **Reddit is where this pays off**, and reddit is also where it is testable — 405KB of
  server-rendered HTML with three posts in it and the rest arriving by fetch.

## Alternatives considered

**Parse on a background thread and hand the tree to the main loop.** Rejected for ADR 0011's reason,
unchanged: the engine's data structures are single-threaded by assumption throughout, and the DOM is
the most-touched of them. Slicing against a deadline gets the responsiveness with no shared mutable
state.

**Paint only at the end, but yield during the parse so the window stays responsive.** A real
half-measure and genuinely tempting: it fixes the frozen-window problem with none of the partial-state
bugs. Rejected because the frozen window is the smaller of the two complaints — a blank window that
redraws is still a blank window — and because the partial-state work is where the user-visible win is.

**Wait for the process split, so parsing blocks a renderer process rather than the browser.**
Rejected as solving a different problem. It fixes the *chrome* being frozen, not the page taking
seconds to appear, and it is years away.

**Chunk on token count rather than on time.** Rejected as unstable: a token can be a character or a
64KB text node, so a token budget produces slice times that vary by two orders of magnitude over the
same document. Time is what the loop actually cares about, so time is what is budgeted.
