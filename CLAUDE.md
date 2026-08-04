# Agent Guide

First-stop operating guide for agents working in this repository.

## Quick Scan

- `microbrowser` is a from-scratch native web browser in C++20, CMake, SDL3 for windowing only.
- Priority order: correctness → security → privacy → speed → CPU → memory → simplicity.
- **Every byte from the network and every message from a renderer is attacker-controlled.**
  Isolation is per *site*, not per tab. `guidelines/security.md` is the short version; ADR 0004 is
  the model.
- **Idle CPU is zero and must stay zero.** The process blocks in one place: the platform event wait.
- Every `src/` directory has a `MODULE.deps` contract enforced by the architecture lint. Read the
  manifest before adding a file, an include, or a class member.
- `AGENTS.md` owns repo policy. `guidelines/` owns the durable how-to. `docs/adr/` owns decisions.
- Build with `cmake`, test with `ctest`, and prefer `tools/run-checks.sh` so results are readable
  afterward without rerunning.

## Project Status

**The browser renders Hacker News, and runs a page against it.** External and inline scripts,
DOM reads and writes, `style`, event handlers, and timers — a click reaches the page's own
handlers, and `preventDefault` stops the navigation it would otherwise have caused. `./build/microbrowser/microbrowser <url>` fetches a document,
parses it, resolves its cascade, lays it out, and draws it — text, tables, images and all. The
front page and a comments page both render, and clicking a story navigates to it.

`./build/microbrowser/microbrowser_snapshot <url> -o out.ppm` does the same with no window, which
is how to look at a page from a machine with no display. `-v` dumps every display list command and
`-click x,y` follows a link before the snapshot. **Use it.** Every layout and paint bug listed in
the git log of the last session was found by rendering a real page and looking at it; none of them
failed a test first.

What exists:

| Module | State |
|---|---|
| `src/util` | Parse, StringUtil, Env, tracing, counters, DEFLATE |
| `src/gfx` | Geometry, transforms, Color and its text form, Canvas, DirtyRegion, DisplayList + its two-frame diff, Path, analytic-AA rasterizer, stroker, Painter, FreeType/HarfBuzz text, font catalog + font-stack matching, glyph and shaped-run caches, PNG decoding, SVG rendering (paths, shapes, groups, transforms), bilinear image scaling. SDL-free. |
| `src/ipc` | Typed, versioned, serializable UI↔Engine messages, including display lists with text on them |
| `src/url` | WHATWG URL parser, Origin, Site, PartitionKey, public-suffix list |
| `src/privacy` | Blocking engine, HTTPS-only, referrer trimming, tracking-parameter removal, Verdict |
| `src/net` | HTTP/1.1, cookies, cache, sockets, TLS. `Fetch` takes a `privacy::Verdict` and has no overload without one. |
| `src/dom` | Node, Element, Text, Document |
| `src/html` | Spec-literal tokenizer and tree construction, including the table insertion modes. Form-control predicates and form ownership. |
| `src/css` | Tokenizer, parser, selectors, cascade, computed style, user-agent sheet, HTML presentational attributes, backgrounds including images, the flex properties, `position`/`inset`, `overflow`, min/max sizing |
| `src/layout` | Box tree, block box model, line boxes with a shared baseline, line breaking and `<br>`, text alignment, auto margins, min/max-content widths, per-line text fragments, replaced elements, floats and clearance, automatic table layout, **flexbox** (both axes, grow/shrink/basis, wrap, justify/align, gaps, order), **positioning** (relative/absolute/fixed with a containing-block chain), min/max sizing, overflow clipping, display-list building |
| `src/engine` | Page (one document), PageScript (its interpreter, bindings and timers), Loader (everything network), Engine (routes messages). Hit testing for links, form controls and event targets; form submission; navigation from a click. Fetches and runs a document's scripts — external and inline, in document order — and dispatches clicks to the page before acting on them. |
| `src/bindings` | The seam between script and the document, and the only module that sees both `js` and `dom`. `window`/`location`/`navigator`, element lookup and the simple selectors, attributes, `classList`, `style` (via `Proxy`), `dataset`, tree walking, creation, removal and reordering, `textContent`, event listeners with click dispatch and bubbling, and the timer queue. Where every same-origin check will live — ADR 0008. |
| `src/platform` | The only module that knows what a window is. SDL and the system font database live here. |
| `src/js` | JavaScript: lexer, parser, tree-walking interpreter, mark-sweep heap with an ephemeron pass, classes with accessors and `super`, object-literal accessors, tagged templates. `String`/`Array`/`Object`/`Number`/`Math`/`Date`/`JSON` (parse and stringify), the error constructors, the URI functions, `Reflect`. A backtracking regular expression engine wired to `RegExp` and to the String methods that take a pattern. Symbols as a real value type and the iteration protocol behind `for...of`, spread, rest and destructuring. `Map`, `Set`, `WeakMap`, `WeakSet`. Promises, `queueMicrotask` and the microtask queue. No bytecode VM, async/await, generators, `Proxy` or modules. No `eval` and no `Function(source)` — there is no path from a string to running code, and a test says so. Knows nothing about the DOM — bindings are M9's seam. |
| `src/ui` | Browser chrome: toolbar, omnibox with editing, navigation history. No dom/css/layout — the chrome is not a page. |
| `src/app` | Main loop: idle-wait policy fed by the page's soonest timer, bounded event drain, dirty-region policy, composites chrome over page, present |

Not yet started: flexbox and grid (rest of M5), stacking contexts (rest of M6), tabs, downloads,
the process split and the sandbox (rest of M7), the JS bytecode VM and the rest of the builtins
(rest of M8), integration (M9). The collector runs between top-level statements and between
microtasks — both are points where nothing is in progress and every live value is reachable from
the roots — but not during evaluation, because a tree-walker cannot scan the C++ frames holding
live values. The heap still has a ceiling that surfaces as a `RangeError`, and `async`/`await`
still has nowhere to suspend to. Loading is synchronous — the loop blocks
for the length of a fetch — and a display list carrying an image serializes the bitmap inline rather
than naming it in a resource table. Roadmap in `README.md` and `AGENTS.md`.

## Where To Pick Up

Ordered by value, not by milestone number. `docs/adr/0007-compatibility-targets.md` is the
reasoning; this is the queue.

1. **Reddit or google, the next compatibility targets.** Hacker News renders and works; the
   things this entry used to ask for are done. Take the next target the same way: load it,
   snapshot it, write down what is wrong, and fix what the page actually needs. Every fix in the
   Hacker News run was found that way, and none of them was the thing that looked most likely
   beforehand — the font stack, a self-closing `<tr>`, and `text-align` never being read were all
   invisible until a real page was on screen. Known remaining gaps on Hacker News itself:
   `<select>` is laid out and submitted but not clickable, `cellspacing` is not mapped because
   there is no `border-spacing`, and `:visited` deliberately matches nothing.
2. **The JavaScript bytecode VM, and async/await with it.** The largest single item and the
   project's dominant cost. It is not only speed, and it is no longer only about the collector
   either. Two things now wait on it. The collector cannot run during evaluation, because a
   tree-walker keeps live values in C++ frames it cannot scan — so the heap has a ceiling that
   becomes a `RangeError` instead of a collection. And **an async function has to suspend**,
   which a tree-walker cannot do: its state is C++ stack frames. Promises landed without it
   because a promise only ever *schedules* a call; `await` is the one that needs the stack to be
   data. A VM's value stack is explicit, so precise collection, generators, `async`/`await` and
   the speed all arrive together. See the note at the top of `src/js/Heap.h`.
3. **`Proxy`, and modules.** `Reflect`, `WeakMap` and `WeakSet` are done. `Proxy` is the one
   left that is not a pure addition: it means a check at every property access in the
   interpreter, which is a change to the hot path. Modules bring the loading they imply.
4. **`fetch`, and `requestAnimationFrame`.** `setTimeout` is done and did arrive as an
   `IdleWaitState::next_deadline_ms` — a page with nothing pending still lets the loop block.
   `fetch` needs Promises (done) joined to the loader and its privacy verdict; `rAF` needs the
   same deadline machinery pointed at a frame rather than a timer.
5. **Grid, and the rest of overflow.** Flexbox, `position` and overflow *clipping* are in.
   What is not: grid, and scrolling an overflow container — which needs a scroll offset per box
   and an input path to move it, and is engine work rather than layout's. `position: sticky`
   parses as relative because there is no scroll offset to compare against.
6. **The rest of the DOM bindings.** Events, external scripts, removal, `style` and timers are
   all in. What is left: writing `innerHTML`, which needs *fragment* parsing rather than document
   parsing — `<td>` inside a table is a cell and anywhere else is nothing, so a setter using the
   document parser would build wrong trees quietly. Then `cloneNode`, `getBoundingClientRect`
   (which is layout asking a question of itself), and the events that are not clicks.

Known-crude spots, each with the reasoning written where the code is: loading is synchronous (the
loop blocks for a fetch); a display list carrying an image serializes the bitmap inline rather than
naming it in a resource table, which now costs more because a background image is one more bitmap
per frame; scrolling an overflowing document repaints in full because there is no scroll blit in
the presenter; a background image is re-rasterized per element rather than shared; and collecting
background images resolves the cascade a second time, before layout resolves it again.

## Development Workflow

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)

# Inner loop: test binary only.
cmake --build build --target microbrowser_tests -j$(nproc)

# Full suite, sharded across cores.
ctest --test-dir build --output-on-failure -j$(nproc)

# Benchmarks. Only built under the perf preset, and the binary refuses to print
# timings from a build without NDEBUG.
cmake --preset microbrowser-perf && cmake --build --preset microbrowser-perf --target microbrowser_bench
./build/microbrowser-perf/microbrowser/microbrowser_bench blit

# Focused, by substring.
./build/microbrowser/microbrowser_tests Canvas
./build/microbrowser/microbrowser_tests ArchitectureInvariants
./build/microbrowser/microbrowser_tests --list
```

The build auto-uses **ccache** and **ld.lld** when present; both are no-ops if absent.

Prefer the logging wrapper — it tees to a deterministic file so a result can be read back instead of
rerunning:

```bash
tools/run-checks.sh tests   # -> /tmp/microbrowser-tests.log
tools/run-checks.sh asan    # -> /tmp/microbrowser-asan.log
tools/run-checks.sh ubsan   # -> /tmp/microbrowser-ubsan.log
tools/run-checks.sh tsan    # -> /tmp/microbrowser-tsan.log
tools/run-checks.sh all
```

**After a run, READ `/tmp/microbrowser-<target>.log` instead of rebuilding and rerunning.**

TSan needs ASLR cleared (`setarch -R`); `run-checks.sh` does this automatically. Running `ctest` on
the tsan preset by hand without it fails with "unexpected memory mapping" — that is the environment,
not a bug.

## Observability

All off by default, all read once on first use:

```bash
MICROBROWSER_PERF_COUNTERS=1   # non-zero event counters, dumped at exit
MICROBROWSER_PERF_SUMMARY=1    # per-label scope table ranked by self time
MICROBROWSER_PERF_TRACE=1      # one stderr line per scope (a firehose; distorts what it measures)
MICROBROWSER_STARTUP_SUMMARY=1 # same, for startup scopes
MICROBROWSER_TRACE_REDRAW=1    # one line per presented frame: full/partial, rects, coverage
```

**Read the main-thread column of a summary first.** Self time ranks CPU cost; main time ranks what
the user waits on, and the two routinely disagree. See `guidelines/observability.md`.

## Before You Add Anything

- **A new file** — check the module's `max_tu_lines` and whether the file belongs in this module at
  all. A file that does not fit the module's `purpose:` line wants a different module.
- **A new include** — check `allow:` and the target module's `public:`. If the header you want is
  private, decide whether it should be published or whether you are reaching across a boundary.
- **A class member or public method** — check the class's `budget:`. If you must raise it, say why
  in the commit message. That is the mechanism working, not a hoop.
- **A third-party dependency** — write an ADR. `docs/adr/0001-third-party-dependencies.md` lists the
  sanctioned set and why each earns its place.
- **A timer, poll, or background wakeup** — justify it against the zero-idle-CPU invariant, and
  route it through `IdleWaitState::next_deadline_ms`.
- **A network request** — it must be user-caused and must pass the privacy layer. See
  `guidelines/privacy.md`.
- **A parser, a decoder, or an IPC message** — the input is hostile. Bounds-check every read,
  saturate every size computed from input, and land the fuzz target on the same commit. See
  `guidelines/security.md`.
- **A thread** — write down what it owns, what it borrows, and who joins it before `main` returns.

`tools/budget-report.sh` prints headroom against every budget, sorted by how close each is to its
limit. Run it before a refactor to see what is about to blow.

## Agent Best Practices

- Narrow the problem with fast repo inspection: `rg`, `rg --files`, targeted reads. Avoid broad dumps.
- Match the repo's design direction instead of preserving stale boundaries. Broad refactors are fine
  when they improve correctness or ownership.
- Prefer RAII, explicit ownership, value semantics. Inheritance only for a durable polymorphic
  boundary (`ipc::Transport` is one; most things are not).
- Keep deterministic logic out of event glue and paint code — that is what makes it testable.
- Treat performance as measurable engineering. Do not guess; the counters and scopes are there.
- When a lint fires, fix the design it is pointing at. Raising a budget to silence it is sometimes
  right, but it should be a decision, not a reflex.

## Related Docs

- `AGENTS.md` — repo policy, priority order, invariants
- `SECURITY.md` — vulnerability reporting, scope, disclosure
- `guidelines/architecture.md` — the module contract, layering, separation of concerns
- `guidelines/security.md` — trust boundaries, the process model, hostile input, memory safety
- `guidelines/privacy.md` — the privacy contract every feature is held to
- `guidelines/cpp.md` — ownership and implementation guidance
- `guidelines/performance.md` — measurement workflow and the zero-idle-CPU rule
- `guidelines/observability.md` — counters, scopes, and how to read a summary
- `guidelines/testing.md` — test strategy, reference tests, control fixtures
- `docs/adr/` — durable decisions and their reasoning
- `docs/adr/0007-compatibility-targets.md` — the five sites that must eventually work, and what they cost
- `docs/performance/m0-baseline.md` — the measurements M0 established
- `docs/performance/m1-rasterizer.md` — where paint time actually goes, and what is not hot
- `docs/performance/m6-damage.md` — what incremental repaint saves, and what it does not
