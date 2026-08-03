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

**The browser renders real pages.** `./build/microbrowser/microbrowser <url>` fetches a document,
parses it, resolves its cascade, lays it out, and draws it — text included. What exists:

| Module | State |
|---|---|
| `src/util` | Parse, StringUtil, Env, tracing, counters, DEFLATE |
| `src/gfx` | Geometry, transforms, Color, Canvas, DirtyRegion, DisplayList + its two-frame diff, Path, analytic-AA rasterizer, stroker, Painter, FreeType/HarfBuzz text, font catalog, glyph and shaped-run caches, PNG, bilinear image scaling. SDL-free. |
| `src/ipc` | Typed, versioned, serializable UI↔Engine messages, including display lists with text on them |
| `src/url` | WHATWG URL parser, Origin, Site, PartitionKey, public-suffix list |
| `src/privacy` | Blocking engine, HTTPS-only, referrer trimming, tracking-parameter removal, Verdict |
| `src/net` | HTTP/1.1, cookies, cache, sockets, TLS. `Fetch` takes a `privacy::Verdict` and has no overload without one. |
| `src/dom` | Node, Element, Text, Document |
| `src/html` | Spec-literal tokenizer and tree construction, including the table insertion modes. Form-control predicates and form ownership. |
| `src/css` | Tokenizer, parser, selectors, cascade, computed style, user-agent sheet |
| `src/layout` | Box tree, block box model, line boxes with a shared baseline, line breaking, per-line text fragments, replaced elements, floats and clearance, tables, display-list building |
| `src/engine` | Page (one document), Loader (everything network), Engine (routes messages). Hit testing for links and form controls, form submission, navigation from a click. |
| `src/platform` | The only module that knows what a window is. SDL and the system font database live here. |
| `src/js` | JavaScript: lexer, parser, tree-walking interpreter, mark-sweep heap, classes with accessors and `super`, `String.prototype`, `call`/`apply`/`bind`, part of `Array.prototype`. No bytecode VM, Promises, async, generators or regex engine. No `eval` and no `Function(source)` — there is no path from a string to running code, and a test says so. Knows nothing about the DOM — bindings are M9's seam. |
| `src/ui` | Browser chrome: toolbar, omnibox with editing, navigation history. No dom/css/layout — the chrome is not a page. |
| `src/app` | Main loop: idle-wait policy, bounded event drain, dirty-region policy, composites chrome over page, present |

Not yet started: flexbox and grid (rest of M5), stacking contexts (rest of M6), tabs, downloads,
the process split and the sandbox (rest of M7), the JS bytecode VM and the rest of the builtins
(rest of M8), integration (M9). The collector is written but can only run between top-level
statements — a tree-walker cannot scan the C++ frames holding live values mid-evaluation, so the
heap has a ceiling that surfaces as a `RangeError`. Loading is synchronous — the loop blocks
for the length of a fetch — and a display list carrying an image serializes the bitmap inline rather
than naming it in a resource table. Roadmap in `README.md` and `AGENTS.md`.

## Where To Pick Up

Ordered by value, not by milestone number. `docs/adr/0007-compatibility-targets.md` is the
reasoning; this is the queue.

1. **Hacker News.** The first named compatibility target and the closest to working. The three
   things this entry used to ask for — table insertion modes, navigation from a clicked link,
   form controls — all exist now. What is left is smaller and more specific: `<select>` is laid
   out and submitted but not clickable (no hit test opens it), and the logo is a GIF while
   `src/gfx` decodes only PNG. Load the real page and write down what is wrong before building
   anything from this list; it will find gaps that speculative work will not.
2. **The JavaScript bytecode VM.** The largest single item and the project's dominant cost. It is
   not only speed: the collector cannot run during evaluation today, because a tree-walker keeps
   live values in C++ frames it cannot scan — so the heap has a ceiling that becomes a
   `RangeError` instead of a collection. A VM's value stack is explicit and scannable, so precise
   collection and the speed arrive together. See the note at the top of `src/js/Heap.h`.
3. **Promises and a microtask queue, then async/await.** Not a language feature that bolts on: it
   changes the host event loop, which is currently a blocking wait on window events. Check it
   against the zero-idle-CPU invariant before writing any of it.
4. **A regular expression engine.** A regex literal currently evaluates to its own source text,
   which is a placeholder rather than a feature. `split`, `replace` and `replaceAll` take string
   patterns only because of it, so a regex argument reaches them as text and matches literally.
   It is the last language feature a real page uses that is not merely missing but silently
   wrong, which is what puts it above the layout work below.
5. **Flexbox, then grid.** Not optional for reddit, google, Plex or YouTube. `position:
   absolute/fixed/sticky` and a real overflow/scrolling model are in the same bucket.
6. **DOM bindings (M9).** The seam where every same-origin check will live. Nothing interactive
   works without it, and `src/js/MODULE.deps` deliberately forbids `js` from reaching `dom`
   directly so that this layer cannot be bypassed.

Known-crude spots, each with the reasoning written where the code is: loading is synchronous (the
loop blocks for a fetch); a display list carrying an image serializes the bitmap inline rather than
naming it in a resource table; scrolling an overflowing document repaints in full because there is
no scroll blit in the presenter.

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
