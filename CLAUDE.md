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

**The browser renders Hacker News, and runs a page against it. The JavaScript engine is
complete** — see `docs/js-conformance-roadmap.md` for what is done, what is
deliberately approximate, and the short list of what is left. External and inline scripts,
DOM reads and writes, `style`, event handlers, and timers — a click reaches the page's own
handlers, and `preventDefault` stops the navigation it would otherwise have caused. `./build/microbrowser/microbrowser <url>` fetches a document,
parses it, resolves its cascade, lays it out, and draws it — text, tables, images and all. The
front page and a comments page both render, and clicking a story navigates to it.

`./build/microbrowser/microbrowser_snapshot <url> -o out.ppm` does the same with no window, which
is how to look at a page from a machine with no display. `-v` dumps every display list command,
`-click x,y` follows a link before the snapshot, and it always prints any script that threw.
**Use it.** Every layout and paint bug listed in the git log of the last session was found by
rendering a real page and looking at it; none of them failed a test first.

`./build/microbrowser/microbrowser_jsshell <file.js>` is the same argument for `src/js`: it runs one
file and prints what it said and what it threw, and **`-p` parses only and reports each error by
*offset* with the source around it** — a minified bundle is one line of 200KB, so a line number
locates nothing. Every JavaScript bug in the youtube.com pass was found with it in minutes.
`MICROBROWSER_JS_TREEWALK=1` selects the tree-walker here too, so running a file twice and diffing
is a differential test.

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
| `src/css` | Tokenizer, parser, selectors, cascade, computed style, user-agent sheet, HTML presentational attributes, backgrounds including images, the flex properties, `position`/`inset`, `overflow`, min/max sizing, **custom properties and `var()`** — inherited, nested, with fallbacks and the invalid-at-computed-value rule |
| `src/layout` | Box tree, block box model, line boxes with a shared baseline, line breaking and `<br>`, text alignment, auto margins, min/max-content widths, per-line text fragments, replaced elements, floats and clearance, automatic table layout, **flexbox** (both axes, grow/shrink/basis, wrap, justify/align, gaps, order), **positioning** (relative/absolute/fixed with a containing-block chain), min/max sizing, overflow clipping, display-list building |
| `src/engine` | Page (one document), PageScript (its interpreter, bindings and timers), Loader (everything network), Engine (routes messages). Hit testing for links, form controls and event targets; form submission; navigation from a click. Fetches and runs a document's scripts — external and inline, in document order — and dispatches clicks to the page before acting on them. |
| `src/bindings` | The seam between script and the document, and the only module that sees both `js` and `dom`. **A real type hierarchy** — Node/CharacterData/Element/HTMLElement and the per-tag interfaces, so `instanceof` answers and a class can extend HTMLElement; methods live on prototypes rather than on every wrapper. **Custom elements** — the registry, upgrade in place, and the connected/disconnected/attributeChanged reactions. **Events** a page makes and dispatches, untrusted by construction. `DocumentFragment`. Element-scoped queries and the element-only walk. `window`/`location`/`navigator`, element lookup and the simple selectors, attributes, `classList`, `style` (via `Proxy`), `dataset`, tree walking, creation, removal and reordering, `textContent`, event listeners with click dispatch and bubbling, and the timer queue. Where every same-origin check will live — ADR 0008. |
| `src/platform` | The only module that knows what a window is. SDL and the system font database live here. |
| `src/js` | JavaScript, and as near complete as the language gets here. Lexer, parser, a bytecode compiler and machine (names resolved to slots, calls that cannot leak a scope keeping bindings in the frame, the tree-walker kept as the differential engine behind `MICROBROWSER_JS_TREEWALK=1`), mark-sweep heap with an ephemeron pass. **Modules** — every `import`/`export` form, `import.meta`, `import()` — with the host supplying the resolver. Classes with accessors, `super`, private fields and methods, static blocks, `new.target`, the brand check. `Proxy` with every trap, and subclassing a builtin. Full `ToPrimitive`. **UTF-16 string indexing over UTF-8 storage.** Property attributes and integrity levels. `ArrayBuffer`, the nine typed arrays and `DataView`. A real `Date` with a computed calendar and a parser. `JSON` with replacer, reviver, indent and `toJSON`. A backtracking regular expression engine with `/u` code points and `\p{...}`. Symbols, iteration, `Map`/`Set`/`Weak*`/`WeakRef`, Promises and the microtask queue, and **every form of suspending a call** — `async`/`await`, generators, `yield*` with real delegation, async generators, `for await`. No `eval` and no `Function(source)`, and a test says so. Knows nothing about the DOM. Deviations are listed in `docs/js-conformance-roadmap.md`, each with its reason. |
| `src/ui` | Browser chrome: toolbar, omnibox with editing, navigation history. No dom/css/layout — the chrome is not a page. |
| `src/app` | Main loop: idle-wait policy fed by the page's soonest timer, bounded event drain, dirty-region policy, composites chrome over page, present |

Not yet started: grid (rest of M5), stacking contexts (rest of M6), tabs, downloads,
the process split and the sandbox (rest of M7), integration (M9). M8 is done. **The collector runs during evaluation**, at every loop back edge and every call: the
machine's operand and frame stacks are data, so a script that recurses while allocating is collected
through rather than starved.

**Nothing the parser accepts is handed back to the tree-walker any more.** Every remaining reason
`Compile` can return null is a bound — nesting too deep, too many instructions, more block-scoped
names than a slot index holds — or a guard against a bug in the compiler itself, and a test says so.
Suspending a call is what the machine was built for and every form of it has landed: `Await` files
the running frame and every slice of the machine's stacks it owns, and `Yield` is the same two
halves with a `next` as the trigger instead of a settled promise. An async generator is both at
once, which is why its promises are one per request rather than one per call. **The tree-walker
refuses an async function or a generator rather than running one wrong** — a wrong answer three
lines later is worse than a refusal at the call — and that refusal is now the only thing it is asked
to do that the machine does not.

One thing still delegates rather than compiles: a class is *built* by the tree-walking
`EvaluateClass`, reached through an opcode. Its method bodies are compiled; the computed keys, the
static initializers and the per-instance field initializers are walked, which is right for things
that run once per class or once per instance rather than once per call.

Loading is synchronous — the loop blocks
for the length of a fetch — and a display list carrying an image serializes the bitmap inline rather
than naming it in a resource table. Roadmap in `README.md` and `AGENTS.md`.

## Where To Pick Up

Ordered by value, not by milestone number. `docs/adr/0007-compatibility-targets.md` is the
reasoning; this is the queue.

1. **`calc()`, then `@supports`.** Custom properties and `var()` are **done** — substitution,
   inheritance, fallbacks, nesting, cycle bounds, and the invalid-at-computed-value rule that
   separates a correct implementation from one that merely skips the declaration. `calc()` is
   next because it is nearly always found next to them and the same values need it to resolve
   (550 uses). Then `@supports` (425), which is cheap and fails in the direction that produces a
   wrong page rather than a missing effect — and whose one hard requirement is that it answer
   honestly about what the engine actually supports, or it is the CSS version of ADR 0012's
   stub problem.

2. **`MutationObserver`, then `getBoundingClientRect`.** The binding layer got a lot this
   session and the ordering below is what is left of ADR 0012's list. **Done:** the element type
   hierarchy (Node/CharacterData/Element/HTMLElement and the per-tag interfaces, with
   `instanceof` answering and methods on prototypes rather than on every wrapper); events a page
   makes and dispatches, untrusted by construction, with `Event`/`CustomEvent`/`MouseEvent` and
   the older `createEvent` form; element-scoped `querySelector` and the element-only walk;
   `DocumentFragment`; and **custom elements** — `define`, `get`, upgrade-on-create and
   upgrade-on-define, plus connected, disconnected and attributeChanged.

   `MutationObserver` is next because it is what a framework watches the tree with and it is
   self-contained: a queue of records delivered as a microtask, and the microtask queue already
   exists. Then `getBoundingClientRect`, which is layout asking a question of itself and is what
   `IntersectionObserver` needs too.

   **ADR 0012's rule is the important part and it is easy to break under pressure: a stub is
   worse than an absence**, because feature detection sends a page down the native path into a
   wall where a missing name would have sent it to a polyfill that works.

3. **Asynchronous loading — ADR 0011.** The structural blocker, and what `fetch`, `XHR`,
   `requestAnimationFrame` and the module loader are all waiting on. The loop stays *blocking*:
   the platform wait gains file descriptors alongside the deadline it already takes, which is the
   same trick that let `setTimeout` land without costing idle CPU. One page currently costs 15
   serialised round trips. Test the *ordering*, not the throughput — the failure mode of
   concurrency here is a page that differs by arrival order.

4. **Transport — ADR 0010.** `Accept-Encoding: identity` and `Connection: close` are both one-line
   requests we send. Measured on one page: **5x the bytes** and **15 TLS handshakes for 15
   resources.** gzip needs no new dependency at all — `util::Inflate` is already there and already
   fuzzed — but it does need a bounded inflate and a fuzz target on the same commit, because a
   compressed response is a decompression bomb by default. Connection reuse must be keyed by the
   ADR 0005 partition key, not by host; that is the whole privacy content of it.

5. **`transform`, and with it stacking contexts.** 1391 uses. `AffineTransform` and path
   transforms already exist in `src/gfx`; what is missing is the property, the computed value and
   the display-list command. `transform` creates a stacking context, which is what finally pulls
   M6's remainder in. Animations come *after* it — animating a property that does not apply gains
   nothing — and must not leave a 60Hz loop running on a settled page.

6. **Grid, and scrolling an overflow container.** Real, and sixth on the measurement. Scrolling
   needs a scroll offset per box and an input path to move it, which is engine work rather than
   layout's; `position: sticky` parses as relative because there is no offset to compare against.

7. **Wire modules to the loader.** Follows (3) rather than preceding it: the VM half is done —
   `SetModuleResolver`, depth-first loading, post-order evaluation, cycles — and the missing host
   half is a fetch whose answer arrives later, which is exactly what (3) builds. Until then
   `<script type="module">` parses and links but cannot reach the network.

   The language itself is done. What is left is in `docs/js-conformance-roadmap.md` and is small:
   Annex B block-function hoisting, the two BigInt typed arrays, `Intl`, and the Unicode tables
   `normalize` and the rest of `\p{...}` need. Unhandled rejections still get a console line and
   nothing more.

**Use `tools/jsshell`.** It runs one JavaScript file, and `-p` reports a syntax error by *offset*
with the source around it — a minified bundle is one line of 200KB, so a line number locates
nothing. Every engine bug in the youtube.com pass was found with it in minutes. Its lesson is worth
keeping: `var` was block-scoped and un-hoisted in **both** engines, so the differential could not
see it. Two engines agreeing is evidence, not proof.

Known remaining gaps on Hacker News itself: `<select>` is laid out and submitted but not clickable,
`cellspacing` is not mapped because there is no `border-spacing`, and `:visited` deliberately
matches nothing.

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
MICROBROWSER_JS_TREEWALK=1     # run script on the tree-walker instead of the bytecode machine
```

`MICROBROWSER_JS_TREEWALK=1` is the differential switch, not a debug print: the two engines
answering the same suite is the only way to know they agree. Thirty-four tests are expected to fail
under it and the list is at the top of `tests/JsVmTests.cpp`; anything else appearing there is a
difference nobody decided on. Three tree-walker bugs were found this way rather than by reading it —
the third was a `for...of` that read `done` off a step and never wrote it back to its cursor.

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
- `docs/adr/0009` — the parse depth bound, and the measurements it comes from
- `docs/adr/0010` — transport: content coding, connection reuse, HTTP/2
- `docs/adr/0011` — asynchronous loading and the event loop, against zero-idle-CPU
- `docs/adr/0012` — which web APIs get built, in what order, and why a stub is worse than an absence
- `docs/adr/0013` — media, the video surface, and the codec dependency
- `docs/adr/0014` — the CSS features a real page actually uses, counted
- `docs/performance/m0-baseline.md` — the measurements M0 established
- `docs/performance/m1-rasterizer.md` — where paint time actually goes, and what is not hot
- `docs/performance/m6-damage.md` — what incremental repaint saves, and what it does not
- `docs/performance/m8-bytecode.md` — the machine against the tree-walker, and where the time still goes
